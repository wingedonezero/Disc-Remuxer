/*
 * Blu-ray transport streams as a disc reader reads them (see ts_disc.h).
 *
 * A TS packet with a bad sync byte, the transport error bit or an
 * adaptation field length >= 184 is left out; all-zero source packets are
 * skipped; runs of either are reported as stretches. For each PID read, a
 * PES header (at most 752 bytes) is collected until complete; its payload
 * is given out at once, the rest of the PES packet as each TS packet brings
 * it; a PES packet of length 0 runs to the next PES start; a TS packet that
 * starts a PES without a start code continues the current one when one is
 * running; what cannot be read (no start code, a header that does not
 * parse, a header longer than its packet, a header that does not fit) drops
 * the PID's data until its next PES start.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "base/bytes.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"

#include "mux/ts_disc.h"

#define DISC_HDR_MAX     0x2f0
#define DISC_UNBOUNDED   0x7ffffff0u
#define TS_PACKET_SIZE   188
#define NB_PID           8192

enum { DISC_HEADER = 1, DISC_BODY = 2, DISC_DROP = 3 };

typedef struct DiscTsPid {
    uint8_t  hdr[DISC_HDR_MAX];
    unsigned buffered;
    int      state;
    uint32_t remaining;           /* bytes of the running PES body still to come, DISC_UNBOUNDED none */
    uint16_t key;                 /* the running PES packet's stream key */
    int64_t  pts;                 /* the running PES packet's PTS, until a piece of it is given out */
    int      dialog;              /* a text subtitle PID */
    const char *drop_why;         /* DISC_DROP: why, and where it began and the payload bytes left out */
    int64_t  drop_pos, drop_bytes;
} DiscTsPid;

struct TsDisc {
    TsDiscEvents  ev;
    DiscTsPid    *pid[NB_PID];
    int          *pids;
    unsigned      nb_pids;
    MpegTSDiscGap run;            /* the run of bad / zero source packets being read (kind 0: none) */
};

typedef struct DiscPes {
    uint16_t key;
    unsigned off;                 /* payload start (header length) */
    unsigned total;               /* PES_packet_length + 6, 0 unbounded */
    int64_t  pts;                 /* TS_DISC_NOPTS none */
    int      bad_pts;             /* its flags say it has a PTS, its marker bits are not set */
} DiscPes;

/* A PES header (the disc reader's rules): 1 parsed, 0 not usable. t may be
 * NULL (the standalone header read): its notes are then dropped. */
static int disc_pes(TsDisc *t, const uint8_t *p, unsigned avail, DiscPes *o)
{
    uint8_t sid, flags;

    if (avail < 6 || p[0] || p[1] || p[2] != 1)
        return 0;
    sid      = p[3];
    o->key   = sid;
    o->pts   = TS_DISC_NOPTS;
    o->bad_pts = 0;
    o->total = dr_rb16(p + 4) ? dr_rb16(p + 4) + 6 : 0;
    if ((sid & 0xfe) == 0xbe) {
        o->off = 6;
        return 1;
    }
    if (avail < 9)
        return 0;
    o->off = p[8] + 9;
    if (!((sid >= 0xe0 && sid <= 0xe2) || sid == 0xfd || sid == 0xbd || (sid & 0xe0) == 0xc0))
        return 1;
    flags = p[7];
    if (flags >= 0xc0 || (flags & 0xc0) == 0x80) {
        if (avail < 0xe)
            return 0;
        if ((p[9] & 1) && (p[11] & 1) && (p[13] & 1))
            o->pts = (int64_t)(p[9] & 0x0e) << 29 | (dr_rb16(p + 10) >> 1) << 15 | dr_rb16(p + 12) >> 1;
        else
            o->bad_pts = 1;
    }
    if ((flags & 1) && !(flags & 0x3e)) {
        unsigned q = 9 + 5 * ((flags >> 7) & 1) + 5 * ((flags >> 6) & 1);
        if (avail < q + 1)
            return 0;
        if (p[q] < 0x40) {
            if (p[q] & 1) {
                unsigned r = q + ((p[q] & 0x10) ? 3 : 1);
                if (avail < r + 2)
                    return 0;
                if (p[r] == 0x81 && p[r + 1] < 0x80)
                    o->key = p[r + 1] << 8 | sid;
            }
        } else if (t && t->ev.note) {
            t->ev.note(t->ev.opaque, -1, "a PES extension with private data or a pack header");
        }
    } else if ((flags & 1) && t && t->ev.note) {
        t->ev.note(t->ev.opaque, -1, "a PES extension behind other optional fields");
    }
    return avail >= o->off;
}

int ff_mpegts_disc_pes_header(const uint8_t *p, unsigned avail, uint16_t *key, int64_t *pts)
{
    DiscPes pes;

    if (!disc_pes(NULL, p, avail, &pes))
        return 0;
    *key = pes.key;
    *pts = pes.pts;
    return 1;
}

/* A payload piece of the running PES packet of d; the PES packet's time
 * goes with its first piece that has bytes. */
static int disc_emit(TsDisc *t, DiscTsPid *d, int pid, const uint8_t *data, unsigned len, int64_t pos)
{
    int64_t pts = d->pts;

    if (!len)
        return 0;
    d->pts = TS_DISC_NOPTS;
    return t->ev.emit(t->ev.opaque, pid, d->key, data, len, pts, pos);
}

/* The run of bad / zero source packets ends. */
static int run_end(TsDisc *t)
{
    int ret = 0;

    if (t->run.kind)
        ret = t->ev.gap(t->ev.opaque, &t->run);
    t->run.kind = 0;
    return ret;
}

/* A bad (why) or zero (why NULL) source packet at pos: the run grows or a
 * new one starts. */
static int run_add(TsDisc *t, int kind, const char *why, int64_t pos)
{
    int ret;

    if (t->run.kind == kind && t->run.why == why && t->run.end == pos) {
        t->run.end += TS_DISC_SRC_PACKET;
        t->run.packets++;
        return 0;
    }
    if ((ret = run_end(t)) < 0)
        return ret;
    t->run = (MpegTSDiscGap){ .kind = kind, .pid = -1, .pos = pos, .end = pos + TS_DISC_SRC_PACKET, .packets = 1,
                              .why = why };
    return 0;
}

/* The PID's data is dropped from the TS packet at pos until its next PES
 * start; bytes: its payload bytes left out already. */
static void disc_drop(TsDisc *t, DiscTsPid *d, int pid, int why, int64_t pos, unsigned bytes)
{
    static const char *const whys[] = { "a PES header that does not fit", "a PES packet without its start code",
                                        "a PES header that cannot be read", "a PES packet shorter than its header",
                                        "", "a PES body longer than its packet" };
    const char *w = why >= 0 && why < (int)DR_ARRAY_ELEMS(whys) ? whys[why] : "?";

    if (t->ev.drop)
        t->ev.drop(t->ev.opaque, pid, w);
    d->drop_why   = w;
    d->drop_pos   = pos;
    d->drop_bytes = bytes;
    d->remaining  = DISC_UNBOUNDED;
    d->buffered   = 0;
    d->state      = DISC_DROP;
}

/* A PID's drop ends at the TS packet at end (to_end: the stream's end). */
static int drop_end(TsDisc *t, DiscTsPid *d, int pid, int64_t end, int to_end)
{
    MpegTSDiscGap g = { .kind = MPEGTS_DISC_GAP_PES, .pid = pid, .pos = d->drop_pos, .end = end,
                        .bytes = d->drop_bytes, .why = d->drop_why, .to_end = to_end };

    if (!d->drop_why)
        return 0;
    d->drop_why = NULL;
    return t->ev.gap(t->ev.opaque, &g);
}

int ts_disc_flush(TsDisc *t, int64_t end)
{
    int ret;

    if ((ret = run_end(t)) < 0)
        return ret;
    for (unsigned i = 0; i < t->nb_pids; i++) {
        DiscTsPid *d = t->pid[t->pids[i]];
        if (d && (ret = drop_end(t, d, t->pids[i], end, 1)) < 0)
            return ret;
    }
    return 0;
}

/* The collected PES header bytes: payloads given out as far as they go. */
static int disc_headers(TsDisc *t, DiscTsPid *d, int pid, int64_t pos)
{
    for (;;) {
        DiscPes pes;
        unsigned hlen, end, n, used;
        int ret;

        if (d->buffered < 6)
            return 0;
        if (d->hdr[0] || d->hdr[1] || d->hdr[2] != 1) {
            disc_drop(t, d, pid, 1, pos, d->buffered);
            return 0;
        }
        if ((d->hdr[3] & 0xfe) == 0xbe)
            hlen = 6;
        else if (d->buffered >= 9)
            hlen = d->hdr[8] + 9;
        else
            return 0;
        if (d->buffered < hlen)
            return 0;
        if (!disc_pes(t, d->hdr, d->buffered, &pes)) {
            disc_drop(t, d, pid, 2, pos, d->buffered);
            return 0;
        }
        if (pes.total && pes.total - 1 < pes.off) {
            disc_drop(t, d, pid, 3, pos, d->buffered);
            return 0;
        }
        if (pes.off > d->buffered)
            return 0;
        end = pes.total ? DR_MIN(d->buffered, pes.total) : d->buffered;
        n   = end - pes.off;
        d->key = pes.key;
        d->pts = pes.pts;
        if (t->ev.pes && (ret = t->ev.pes(t->ev.opaque, pid, pes.key)) < 0)
            return ret;
        if (pes.bad_pts) {
            MpegTSDiscGap g = { .kind = MPEGTS_DISC_GAP_PTS, .pid = pid, .pos = pos, .end = pos };
            if ((ret = t->ev.gap(t->ev.opaque, &g)) < 0)
                return ret;
        }
        if (d->dialog && d->pts == TS_DISC_NOPTS && n >= 8 && d->hdr[pes.off] == 0x82 &&
            dr_rb16(d->hdr + pes.off + 1) > 8)
            d->pts = (int64_t)(d->hdr[pes.off + 3] & 1) << 32 | dr_rb32(d->hdr + pes.off + 4);
        if ((ret = disc_emit(t, d, pid, d->hdr + pes.off, n, pos)) < 0)
            return ret;
        if (!pes.total) {
            d->state     = DISC_BODY;
            d->remaining = DISC_UNBOUNDED;
            d->buffered  = 0;
            return 0;
        }
        used = n + pes.off;
        if (end == d->buffered) {
            d->buffered = 0;
            if (pes.total == used) {
                d->state = DISC_HEADER;
            } else {
                d->state     = DISC_BODY;
                d->remaining = pes.total - used;
            }
            return 0;
        }
        /* another PES packet follows in the collected bytes */
        memmove(d->hdr, d->hdr + pes.total, d->buffered - pes.total);
        d->buffered -= pes.total;
        d->state = DISC_HEADER;
    }
}

static int disc_ts_packet(TsDisc *t, const uint8_t *p, int64_t pos)
{
    int pid, pusi, off = 4;
    unsigned len;
    const uint8_t *pl;
    DiscTsPid *d;
    char text[128];

    if (p[0] != 0x47)
        return run_add(t, MPEGTS_DISC_GAP_BAD, "no sync byte", pos);
    if (p[1] & 0x80)
        return run_add(t, MPEGTS_DISC_GAP_BAD, "transport_error_indicator set", pos);
    pid = (p[1] & 0x1f) << 8 | p[2];
    if (p[3] & 0x20) {
        if (p[4] >= 0xb8)
            return run_add(t, MPEGTS_DISC_GAP_BAD, "an adaptation field longer than the packet", pos);
        off = p[4] + 5;
    }
    {
        int ret = run_end(t);
        if (ret < 0)
            return ret;
    }
    if (!(d = t->pid[pid]))
        return 0;
    pusi = p[1] & 0x40;
    pl   = p + off;
    len  = (p[3] & 0x10) ? TS_PACKET_SIZE - off : 0;

    if (pusi) {
        if (d->state != DISC_HEADER && len >= 6 && (pl[0] || pl[1] || pl[2] != 1)) {
            if (t->ev.note)
                t->ev.note(t->ev.opaque, pid, "a PES start without a start code: taken as a continuation");
        } else {
            int ret;
            if (d->remaining != DISC_UNBOUNDED && d->state != DISC_HEADER) {
                MpegTSDiscGap g = { .kind = MPEGTS_DISC_GAP_CUT, .pid = pid, .pos = pos, .end = pos,
                                    .bytes = d->remaining };
                if ((ret = t->ev.gap(t->ev.opaque, &g)) < 0)
                    return ret;
            }
            if (d->state == DISC_DROP && (ret = drop_end(t, d, pid, pos, 0)) < 0)
                return ret;
            d->remaining = 0;
            d->buffered  = 0;
            d->state     = DISC_HEADER;
            goto collect;
        }
    }
    if (d->state == DISC_BODY || d->state == DISC_DROP) {
        if (d->state == DISC_BODY && d->remaining != DISC_UNBOUNDED && len > d->remaining) {
            disc_drop(t, d, pid, 5, pos, len);
            return 0;
        }
        if (d->remaining == DISC_UNBOUNDED) {
            if (d->state == DISC_BODY)
                return disc_emit(t, d, pid, pl, len, pos);
            d->drop_bytes += len;
            return 0;
        }
        if (len > d->remaining) {
            if (t->ev.error) {
                snprintf(text, sizeof(text), "PID 0x%04x: a TS packet at byte %" PRId64 " runs past its PES packet",
                         pid, pos);
                t->ev.error(t->ev.opaque, text);
            }
            return DR_ERR_INVALID;
        }
        if (d->state == DISC_BODY) {
            int ret = disc_emit(t, d, pid, pl, len, pos);
            if (ret < 0)
                return ret;
        }
        d->remaining -= len;
        if (!d->remaining) {
            d->buffered = 0;
            d->state    = DISC_HEADER;
        }
        return 0;
    }
    if (d->state != DISC_HEADER) {
        if (t->ev.error) {
            snprintf(text, sizeof(text), "PID 0x%04x: unknown reading state", pid);
            t->ev.error(t->ev.opaque, text);
        }
        return DR_ERR_EXTERNAL;
    }
collect:
    if (len > DISC_HDR_MAX - d->buffered) {
        disc_drop(t, d, pid, 0, pos, d->buffered + len);
        return 0;
    }
    memcpy(d->hdr + d->buffered, pl, len);
    d->buffered += len;
    return disc_headers(t, d, pid, pos);
}

int ts_disc_source_packet(TsDisc *t, const uint8_t *p, int64_t pos)
{
    int zero = 1;

    for (int i = 0; i < TS_DISC_SRC_PACKET && zero; i++)
        zero = !p[i];
    if (zero)
        return run_add(t, MPEGTS_DISC_GAP_ZERO, NULL, pos);
    return disc_ts_packet(t, p + 4, pos);
}

TsDisc *ts_disc_new(const int *pids, unsigned nb_pids, const int *dialog_pids, unsigned nb_dialog,
                    const TsDiscEvents *ev)
{
    TsDisc *t = dr_mallocz(sizeof(*t));

    if (!t)
        return NULL;
    t->ev = *ev;
    if (nb_pids && !(t->pids = dr_memdup(pids, nb_pids * sizeof(*pids)))) {
        dr_free(t);
        return NULL;
    }
    t->nb_pids = nb_pids;
    for (unsigned i = 0; i < nb_pids; i++) {
        int pid = pids[i];
        if (pid < 0 || pid >= NB_PID)
            continue;
        if (!t->pid[pid]) {
            if (!(t->pid[pid] = dr_mallocz(sizeof(DiscTsPid)))) {
                ts_disc_free(&t);
                return NULL;
            }
            t->pid[pid]->state     = DISC_HEADER;
            t->pid[pid]->remaining = DISC_UNBOUNDED;
            t->pid[pid]->pts       = TS_DISC_NOPTS;
        }
    }
    for (unsigned i = 0; i < nb_dialog; i++)
        if (dialog_pids[i] >= 0 && dialog_pids[i] < NB_PID && t->pid[dialog_pids[i]])
            t->pid[dialog_pids[i]]->dialog = 1;
    return t;
}

void ts_disc_free(TsDisc **tp)
{
    TsDisc *t = *tp;

    *tp = NULL;
    if (!t)
        return;
    for (unsigned i = 0; i < NB_PID; i++)
        dr_free(t->pid[i]);
    dr_free(t->pids);
    dr_free(t);
}
