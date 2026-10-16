/*
 * The clip demuxer (see clip_demux.h). The byte rules live in src/mux
 * (ps_disc.c, ts_disc.c); here: the block reading, the payloads' streams,
 * what is counted, and the stretches left out (logged, or kept for the
 * caller).
 */

#include <inttypes.h>
#include <string.h>

#include "base/err.h"
#include "base/mem.h"
#include "base/vlog.h"
#include "disc/clip_demux.h"

#define PS_SECTOR  MPEGPS_DISC_SECTOR
#define TS_UNIT    6144                 /* Blu-ray: 32 source packets of 192 bytes */
#define PS_QUEUE   8                    /* payloads of one sector (two, or more with MPEG audio) */

/* A payload of the sector being read (PS), or a copied TS payload piece. */
typedef struct ClipQueued {
    struct ClipQueued *next;            /* TS */
    int       start, end;               /* PS: bytes of the sector */
    uint8_t  *copy;                     /* TS: its bytes */
    int       size;
    int       id;
    int64_t   pts;
    int64_t   pos;
    int       kind;
    DRCodecID codec;
} ClipQueued;

struct DrClipDemux {
    void       *logctx;
    DrClipRead  read;
    void       *opaque;
    int         ts;
    int         sticky;                 /* a failed reading stays failed */

    /* program streams */
    int             lpcm_header;
    uint8_t         sec[PS_SECTOR];
    int64_t         sector;             /* sectors read */
    MpegPsDiscStats stats;
    ClipQueued      q[PS_QUEUE];
    int             q_head, q_n;

    /* transport streams */
    struct TsDisc  *tsdisc;
    int             gaps_kept;          /* kept for dr_clip_gap() instead of logged */
    uint8_t         unit[TS_UNIT];
    int             avail, off;
    int64_t         pos;                /* bytes fed to the reading */
    int64_t         got;                /* bytes taken from the block reader */
    int             ended;              /* the stream has ended (flushed unless gaps are kept) */
    ClipQueued     *head, *tail, *out;  /* the queue; out: the piece given out last */
    MpegTSDiscGap  *gaps;
    int             nb_gaps, gaps_head, gaps_cap;
};

/* ---- program streams ---- */

/* The stream of a packet: the MPEG-PS ids and codecs the payloads leave
 * with; 0 when the packet carries no stream (padding, private stream 2,
 * system header, sub-streams and stream ids no disc format uses). */
static int ps_stream_of(const MpegPsDiscPacket *p, int *id, int *kind, DRCodecID *codec)
{
    int sid = p->key & 0xff;

    if (sid == 0xbd) {
        int sub = p->key >> 8;
        *id   = sub;
        *kind = DR_KIND_AUDIO;
        if (sub >= 0x20 && sub <= 0x3f) {
            *kind  = DR_KIND_SUBTITLE;
            *codec = DR_CODEC_DVD_SUBTITLE;
        } else if (sub >= 0x80 && sub <= 0x87) {
            *codec = DR_CODEC_AC3;
        } else if ((sub >= 0x88 && sub <= 0x8f) || (sub >= 0x98 && sub <= 0x9f)) {
            *codec = DR_CODEC_DTS;
        } else if (sub >= 0xa0 && sub <= 0xaf) {
            *codec = DR_CODEC_PCM_DVD;
        } else if (sub >= 0xb0 && sub <= 0xbf) {
            *codec = DR_CODEC_TRUEHD;
        } else if (sub >= 0xc0 && sub <= 0xcf) {
            *codec = DR_CODEC_AC3;          /* AC-3 and E-AC-3 */
        } else {
            return 0;
        }
        return 1;
    }
    if (sid == 0xfd) {
        int ext = p->key >> 8;
        if (ext < 0x55 || ext > 0x5f)
            return 0;
        *id    = 0xfd00 | ext;
        *kind  = DR_KIND_VIDEO;
        *codec = DR_CODEC_VC1;
        return 1;
    }
    if (p->key > 0xff)
        return 0;
    if (sid >= 0xe0 && sid <= 0xef) {
        *id    = 0x100 | sid;
        *kind  = DR_KIND_VIDEO;
        *codec = sid == 0xe2 ? DR_CODEC_H264 : DR_CODEC_MPEG2VIDEO;
        return 1;
    }
    if (sid >= 0xc0 && sid <= 0xdf) {
        *id    = 0x100 | sid;
        *kind  = DR_KIND_AUDIO;
        *codec = DR_CODEC_MP2;
        return 1;
    }
    return 0;
}

typedef struct PsSectorRead {
    DrClipDemux *c;
    int64_t      pos;                   /* the sector's byte position */
} PsSectorRead;

/* Bytes start..end of the sector as a payload of the stream. */
static int ps_queue(DrClipDemux *c, int start, int end, int64_t pts, int64_t pos, int id, int kind,
                    DRCodecID codec)
{
    if (c->q_n == PS_QUEUE)
        return DR_ERR_BUG;
    c->q[(c->q_head + c->q_n++) % PS_QUEUE] =
        (ClipQueued){ .start = start, .end = end, .pts = pts, .pos = pos, .id = id, .kind = kind,
                      .codec = codec };
    return 0;
}

static int ps_on_packet(void *opaque, const uint8_t *sec, const MpegPsDiscPacket *p)
{
    PsSectorRead *r = opaque;
    DrClipDemux *c = r->c;
    DRCodecID codec = DR_CODEC_NONE;
    int id, kind = 0, start, end, ret;

    end = p->len && p->at + p->len < PS_SECTOR ? p->at + p->len : PS_SECTOR;
    if (!ps_stream_of(p, &id, &kind, &codec)) {
        /* private stream 2 and padding end the sector: their bytes are the rest of it */
        int bytes = (p->key == 0xbf || p->key == 0xbe) ? PS_SECTOR - p->at : end - p->at;
        if (p->key == 0xbf) {
            c->stats.nav++;
            c->stats.nav_bytes += bytes;
        } else if (p->key == 0xbe) {
            c->stats.padding++;
            c->stats.padding_bytes += bytes;
        } else if (p->key == 0xbb) {
            c->stats.system++;
            c->stats.system_bytes += bytes;
        } else {
            c->stats.other++;
            c->stats.other_bytes += bytes;
        }
        return 0;
    }
    start = p->at + p->payload - p->extra;          /* LPCM: its audio frame header stays */
    if (end <= start)
        return 0;
    if (p->pts_present && !p->pts_valid)
        dr_vlog(c->logctx, DR_VLOG_WARNING, "Disc pack reader: sector %"PRId64", stream 0x%x: the PTS's marker "
                "bits are wrong: the packet has no time\n", r->pos / PS_SECTOR, id);
    /* a private stream 1 audio payload whose first access unit starts inside
     * it: the bytes before it (the end of a frame that began earlier) and the
     * bytes from it go on as two payloads, the time with the second (the
     * reader hands them to the frame rules apart); not LPCM, whose payload
     * begins with its audio frame header */
    if ((p->key & 0xff) == 0xbd && kind == DR_KIND_AUDIO && !p->extra &&
        p->first_au > 0 && start + p->first_au < end) {
        if ((ret = ps_queue(c, start, start + p->first_au, DR_CLIP_NOPTS, r->pos + p->at, id, kind, codec)) < 0)
            return ret;
        start += p->first_au;
    }
    return ps_queue(c, start, end, p->pts_valid ? (int64_t)p->pts : DR_CLIP_NOPTS, r->pos + p->at, id, kind,
                    codec);
}

/* The block reader's bytes into buf, exactly size of them: size, 0 at the
 * stream's end (*partial: bytes that still came), < 0 an error. */
static int fill(DrClipDemux *c, uint8_t *buf, int size, int *partial)
{
    int n = 0;

    *partial = 0;
    while (n < size) {
        int r = c->read(c->opaque, buf + n, size - n);
        if (r == DR_ERR_EOF || r == 0) {
            *partial = n;
            return 0;
        }
        if (r < 0)
            return n ? (*partial = n, 0) : r;
        n += r;
        c->got += r;
    }
    return size;
}

static int ps_read_packet(DrClipDemux *c, DrClipPacket *pkt)
{
    for (;;) {
        PsSectorRead r = { c, c->sector * PS_SECTOR };
        const char *why;
        int n, at, partial, ret;

        if (c->q_n) {
            const ClipQueued *q = &c->q[c->q_head];
            c->q_head = (c->q_head + 1) % PS_QUEUE;
            c->q_n--;
            *pkt = (DrClipPacket){ c->sec + q->start, q->end - q->start, q->id, q->pts, q->pos, q->kind,
                                   q->codec };
            return 0;
        }
        if (c->sticky)
            return c->sticky;
        if ((n = fill(c, c->sec, PS_SECTOR, &partial)) < 0)
            return n;
        if (!n) {
            if (!partial)
                return DR_ERR_EOF;
            dr_vlog(c->logctx, DR_VLOG_ERROR, "Disc pack reader: the stream ends %d bytes into sector %"PRId64"\n",
                    partial, c->sector);
            return c->sticky = DR_ERR_INVALID;
        }
        ret = ff_mpegps_disc_sector(c->sec, c->lpcm_header == 5, ps_on_packet, &r, &why, &at);
        c->sector++;
        c->stats.sectors++;
        switch (ret) {
        case MPEGPS_DISC_OK:
            break;
        case MPEGPS_DISC_EMPTY:
            c->stats.empty++;
            break;
        case MPEGPS_DISC_BAD:
            c->stats.bad++;
            dr_vlog(c->logctx, DR_VLOG_WARNING, "Disc pack reader: sector %"PRId64": %s: left out\n",
                    c->sector - 1, why);
            break;
        case MPEGPS_DISC_REST_DROPPED:
            c->stats.rest_dropped++;
            c->stats.rest_bytes += PS_SECTOR - at;
            dr_vlog(c->logctx, DR_VLOG_WARNING, "Disc pack reader: sector %"PRId64": %s (byte %d): the rest of "
                    "the sector is left out\n", c->sector - 1, why, at);
            break;
        case MPEGPS_DISC_FAIL:
            dr_vlog(c->logctx, DR_VLOG_ERROR, "Disc pack reader: sector %"PRId64" (byte %d): %s\n", c->sector - 1,
                    at, why);
            return c->sticky = DR_ERR_INVALID;
        default:
            if (ret < 0)
                return c->sticky = ret;
        }
    }
}

/* ---- transport streams ---- */

static int ts_queue_put(DrClipDemux *c, int id, const uint8_t *data, unsigned len, int64_t pts, int64_t pos)
{
    ClipQueued *q = dr_mallocz(sizeof(*q));

    if (!q || !(q->copy = dr_memdup(data, len ? len : 1))) {
        dr_free(q);
        return DR_ERR_NOMEM;
    }
    q->size = len;
    q->id   = id;
    q->pts  = pts;
    q->pos  = pos;
    if (c->tail)
        c->tail->next = q;
    else
        c->head = q;
    c->tail = q;
    return 0;
}

static void ts_queued_free(ClipQueued **q)
{
    if (*q) {
        dr_free((*q)->copy);
        dr_freep(q);
    }
}

static int ts_emit_cb(void *opaque, int pid, uint16_t key, const uint8_t *data, unsigned len, int64_t pts,
                      int64_t pos)
{
    return ts_queue_put(opaque, pid | key << 16, data, len, pts, pos);
}

/* A stretch left out (or a PTS / cut-short record): kept for the caller, or
 * logged as before the reading moved out. */
static int ts_gap_cb(void *opaque, const MpegTSDiscGap *g)
{
    DrClipDemux *c = opaque;

    if (!c->gaps_kept) {
        switch (g->kind) {
        case MPEGTS_DISC_GAP_PTS:
            dr_vlog(c->logctx, DR_VLOG_WARNING, "TS: PID 0x%04x: a PES header at byte %"PRId64" has a PTS whose "
                    "marker bits are not set: read as having no time\n", g->pid, g->pos);
            break;
        case MPEGTS_DISC_GAP_CUT:
            dr_vlog(c->logctx, DR_VLOG_WARNING, "TS: PID 0x%04x: a PES packet cut short at byte %"PRId64"\n",
                    g->pid, g->pos);
            break;
        case MPEGTS_DISC_GAP_CC:
            dr_vlog(c->logctx, DR_VLOG_DEBUG, "TS: PID 0x%04x: the continuity counter goes from %d to %d at byte "
                    "%"PRId64"\n", g->pid, g->cc_from, g->cc_to, g->pos);
            break;
        case MPEGTS_DISC_GAP_DUP:
            dr_vlog(c->logctx, DR_VLOG_DEBUG, "TS: PID 0x%04x: a duplicate transport packet at byte %"PRId64": left "
                    "out\n", g->pid, g->pos);
            break;
        case MPEGTS_DISC_GAP_PES:
            dr_vlog(c->logctx, DR_VLOG_DEBUG, "TS: PID 0x%04x: %s at byte %"PRId64": %"PRId64" bytes left out "
                    "until %s\n", g->pid, g->why, g->pos, g->bytes,
                    g->to_end ? "the stream's end" : "the next PES start");
            break;
        default:
            dr_vlog(c->logctx, DR_VLOG_DEBUG, "TS: bytes %"PRId64"-%"PRId64": %"PRId64" source packets %s%s%s: "
                    "left out\n", g->pos, g->end - 1, g->packets,
                    g->kind == MPEGTS_DISC_GAP_ZERO ? "of zeros" : "that cannot be read (",
                    g->kind == MPEGTS_DISC_GAP_ZERO ? "" : g->why, g->kind == MPEGTS_DISC_GAP_ZERO ? "" : ")");
        }
        return 0;
    }
    if (c->gaps_head && c->gaps_head == c->nb_gaps)
        c->gaps_head = c->nb_gaps = 0;
    if (c->nb_gaps == c->gaps_cap) {
        int cap = c->gaps_cap ? 2 * c->gaps_cap : 16;
        MpegTSDiscGap *q = dr_realloc_array(c->gaps, cap, sizeof(*q));
        if (!q)
            return DR_ERR_NOMEM;
        c->gaps     = q;
        c->gaps_cap = cap;
    }
    c->gaps[c->nb_gaps++] = *g;
    return 0;
}

static void ts_drop_cb(void *opaque, int pid, const char *why)
{
    DrClipDemux *c = opaque;

    if (!c->gaps_kept)
        dr_vlog(c->logctx, DR_VLOG_WARNING, "TS: PID 0x%04x: %s: its data is dropped until its next PES start\n",
                pid, why);
}

static void ts_note_cb(void *opaque, int pid, const char *text)
{
    DrClipDemux *c = opaque;

    if (pid >= 0)
        dr_vlog(c->logctx, DR_VLOG_DEBUG, "TS: PID 0x%04x: %s\n", pid, text);
    else
        dr_vlog(c->logctx, DR_VLOG_DEBUG, "TS: %s\n", text);
}

static void ts_error_cb(void *opaque, const char *text)
{
    DrClipDemux *c = opaque;

    dr_vlog(c->logctx, DR_VLOG_ERROR, "TS: %s\n", text);
}

/* The stream has ended: the stretches still open end with it (logged here,
 * unless they are kept: then the caller's dr_clip_gap(flush) does this). */
static int ts_ended(DrClipDemux *c)
{
    int ret;

    if (c->ended)
        return DR_ERR_EOF;
    c->ended = 1;
    if (!c->gaps_kept && (ret = ts_disc_flush(c->tsdisc, c->got)) < 0)
        return ret;
    return DR_ERR_EOF;
}

static int ts_read_packet(DrClipDemux *c, DrClipPacket *pkt)
{
    ts_queued_free(&c->out);
    while (!c->head) {
        int ret;

        if (c->sticky)
            return c->sticky;
        if (c->avail - c->off < TS_DISC_SRC_PACKET) {
            int partial;
            memmove(c->unit, c->unit + c->off, c->avail - c->off);
            c->avail -= c->off;
            c->off    = 0;
            ret = fill(c, c->unit + c->avail, TS_UNIT - c->avail, &partial);
            if (ret < 0)
                return c->sticky = ret;
            c->avail += ret ? ret : partial;
            if (c->avail < TS_DISC_SRC_PACKET)      /* a partial tail is not a source packet */
                return ts_ended(c);
        }
        ret = ts_disc_source_packet(c->tsdisc, c->unit + c->off, c->pos);
        c->off += TS_DISC_SRC_PACKET;
        c->pos += TS_DISC_SRC_PACKET;
        if (ret < 0)
            return c->sticky = ret;
    }
    c->out  = c->head;
    c->head = c->out->next;
    if (!c->head)
        c->tail = NULL;
    *pkt = (DrClipPacket){ c->out->copy, c->out->size, c->out->id, c->out->pts, c->out->pos, 0, DR_CODEC_NONE };
    return 0;
}

/* ---- the calls ---- */

int dr_clip_ps_open(DrClipDemux **c, void *logctx, int lpcm_header, DrClipRead read, void *opaque)
{
    DrClipDemux *d = dr_mallocz(sizeof(*d));

    if (!d)
        return DR_ERR_NOMEM;
    d->logctx      = logctx;
    d->read        = read;
    d->opaque      = opaque;
    d->lpcm_header = lpcm_header;
    *c = d;
    return 0;
}

int dr_clip_ts_open(DrClipDemux **c, void *logctx, const int *pids, int nb_pids, int dialog, int gaps,
                    DrClipRead read, void *opaque)
{
    static const TsDiscEvents ev = { NULL, ts_emit_cb, ts_gap_cb, ts_drop_cb, ts_note_cb, ts_error_cb };
    DrClipDemux *d = dr_mallocz(sizeof(*d));
    TsDiscEvents e = ev;

    if (!d)
        return DR_ERR_NOMEM;
    d->logctx    = logctx;
    d->read      = read;
    d->opaque    = opaque;
    d->ts        = 1;
    d->gaps_kept = gaps;
    e.opaque     = d;
    if (!(d->tsdisc = ts_disc_new(pids, nb_pids, dialog ? pids : NULL, dialog ? nb_pids : 0, &e))) {
        dr_free(d);
        return DR_ERR_NOMEM;
    }
    *c = d;
    return 0;
}

int dr_clip_read(DrClipDemux *c, DrClipPacket *pkt)
{
    return c->ts ? ts_read_packet(c, pkt) : ps_read_packet(c, pkt);
}

void dr_clip_ts_break(DrClipDemux *c)
{
    if (c && c->ts)
        ts_disc_break(c->tsdisc);
}

int dr_clip_gap(DrClipDemux *c, int flush, MpegTSDiscGap *gap)
{
    int ret;

    if (!c || !c->ts)
        return 0;
    if (flush && (ret = ts_disc_flush(c->tsdisc, c->got)) < 0)
        return ret;
    if (c->gaps_head == c->nb_gaps)
        return 0;
    *gap = c->gaps[c->gaps_head++];
    return 1;
}

void dr_clip_ps_stats(const DrClipDemux *c, MpegPsDiscStats *st)
{
    *st = c->stats;
}

void dr_clip_close(DrClipDemux **c)
{
    DrClipDemux *d = *c;

    if (!d)
        return;
    ts_disc_free(&d->tsdisc);
    ts_queued_free(&d->out);
    while (d->head) {
        ClipQueued *q = d->head;
        d->head = q->next;
        ts_queued_free(&q);
    }
    dr_free(d->gaps);
    dr_freep(c);
}
