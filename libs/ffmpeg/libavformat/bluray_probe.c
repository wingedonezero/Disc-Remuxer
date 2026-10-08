/*
 * Blu-ray: the stream probes of a title's tracks (the reference program's),
 * from the stream data of the segment the tracks are described from.
 *
 * Reading: the segment's items in order, 6144-byte aligned units (32 source
 * packets: 4-byte TP_extra_header + 188-byte TS packet), decrypted (AACS);
 * a 3-D (interleaved) item reads its base-view clip. A PES scan of a PID
 * reads from the start until one PES stream key (stream_id, or stream_id
 * 0xFD with its stream_id_extension: key 0xFD | ext << 8) has been seen
 * `limit` times, the end, or 2 GiB; all-zero units are skipped.
 *
 * Probes, once per clip stream:
 * - TrueHD, E-AC-3, DTS-HD: 3 PES headers of one key; neither key 0x71FD
 *   nor 0x76FD seen = no core substream (E-AC-3 / DTS-HD then give one
 *   track);
 * - AVC video (not on BDAV clips): a single PES stream id other than 0xE0
 *   (within 20 PES headers; 0xE1..0xE7) is noted on the clip's first stream;
 * - HEVC on PID 0x1015 beside the clip's first stream (HEVC on another PID):
 *   the second layer counts when its first parameter sets (at most 1000 NAL
 *   units, until 6 SPS, a PPS and a VPS) include a Dolby Vision RPU (NAL
 *   type 62); noted on the clip's first stream;
 * - BDAV (M2TS) clips: the program map (PMT, first 2 MiB) gives private
 *   streams (coding type 0x06) their kind by descriptor (0x52 ARIB caption,
 *   0x56 teletext of type 2 or 5, 0x59 DVB subtitles, 0x6a AC-3, 0x7a E-AC-3,
 *   0x7b DTS) and audio / subtitle streams their language (descriptor 0x0a,
 *   0x56, 0x59; else the first video PID's). [untested on real discs]
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <inttypes.h>
#include <string.h>

#include "libavutil/crc.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "bluray_internal.h"

#define UNIT       BLURAY_UNIT
#define PACKET     192
#define SCAN_CAP   0x80000001LL       /* a PES scan ends here */
#define PSI_SCAN   0x200000           /* a table is looked for in the first 2 MiB */
#define MAX_KEYS   64
#define HEVC_UNITS 1000

/* ---- the segment as aligned units ---- */

static int64_t item_length(const BluraySegItem *it, int both)
{
    if (it->dep && both)
        return it->length;                  /* 3-D: both views interleaved */
    return it->dep ? (int64_t)it->clip->packets * PACKET : it->length;   /* 3-D: the base view only */
}

int64_t ff_bluray_seg_size_views(const BluraySegment *s, int both)
{
    int64_t n = 0;
    for (int i = 0; i < s->nb_items; i++)
        n += item_length(&s->items[i], both && s->interleaved);
    return n;
}

int64_t ff_bluray_seg_size(const BluraySegment *s)
{
    return ff_bluray_seg_size_views(s, 0);
}

void ff_bluray_seg_reader_init(BluraySegReader *u, void *log, DiscIOFS *fs, BlurayAACS *aacs,
                               const BluraySegment *s)
{
    *u = (BluraySegReader){ .log = log, .fs = fs, .aacs = aacs, .s = s, .item = -1 };
}

void ff_bluray_seg_reader_both(BluraySegReader *u)
{
    u->both = u->s->interleaved;
}

void ff_bluray_seg_reader_close(BluraySegReader *u)
{
    ff_discio_file_free(&u->f);
    ff_discio_file_free(&u->f2);
    av_freep(&u->chunks);
    u->nb_chunks = 0;
    u->item = -1;
}

/* The runs of a 3-D item read with both views: the two stream files cut at
 * their extent start points, run i of the file read first, then run i of
 * the other (which file is first: the pair's interleave order). */
static int item_chunks(BluraySegReader *u, const BluraySegItem *it)
{
    const BlurayClip *a = it->clip, *c = it->dep;
    const BlurayClip *first = c->interleave == 2 ? c : a, *second = c->interleave == 2 ? a : c;
    int n = first->nb_ss_extents;
    int64_t pos = 0;

    if (!(u->chunks = av_calloc(2 * n, sizeof(*u->chunks))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 2; k++) {
            const BlurayClip *x = k ? second : first;
            int64_t s0 = (int64_t)x->ss_extents[i] * PACKET;
            int64_t s1 = i + 1 < n ? (int64_t)x->ss_extents[i + 1] * PACKET : (int64_t)x->packets * PACKET;
            u->chunks[u->nb_chunks++] = (BlurayChunk){ pos, s0, s1 - s0, x == c };
            pos += s1 - s0;
        }
    return pos == it->length ? 0 : AVERROR_BUG;
}

int ff_bluray_seg_read(BluraySegReader *u, int64_t pos, uint8_t *buf)
{
    int64_t base = 0;
    int ret;

    for (int i = 0; i < u->s->nb_items; i++) {
        const BluraySegItem *it = &u->s->items[i];
        int both = it->dep && u->both, lo = 0, hi;
        int64_t len = item_length(it, u->both), off = (it->dep ? 0 : it->start) + pos - base;
        BlurayClip *clip = it->clip;
        DiscIOFile *f;
        if (pos >= base + len) {
            base += len;
            continue;
        }
        if (u->item != i) {
            ff_bluray_seg_reader_close(u);
            if ((ret = u->fs->ops->open_file(u->fs, it->clip->stream_path, &u->f)) < 0 ||
                (both && (ret = u->fs->ops->open_file(u->fs, it->dep->stream_path, &u->f2)) < 0)) {
                av_log(u->log, AV_LOG_ERROR, "Blu-ray: %s cannot be opened: %s\n",
                       u->f ? it->dep->stream_path : it->clip->stream_path, av_err2str(ret));
                return ret;
            }
            if (both && (ret = item_chunks(u, it)) < 0) {
                av_log(u->log, AV_LOG_ERROR, "Blu-ray: the views of %s cannot be read interleaved\n",
                       it->clip->stream_path);
                return ret;
            }
            u->item = i;
        }
        if (pos + UNIT > base + len)
            return AVERROR_EOF;             /* a part unit at the end */
        f = u->f;
        if (both) {
            /* the run holding the unit (runs are whole units: extents are
             * multiples of 32 source packets) */
            hi = u->nb_chunks - 1;
            while (lo < hi) {
                int m = (lo + hi + 1) / 2;
                if (u->chunks[m].pos <= pos - base)
                    lo = m;
                else
                    hi = m - 1;
            }
            off = u->chunks[lo].off + pos - base - u->chunks[lo].pos;
            if (u->chunks[lo].view) {
                f    = u->f2;
                clip = it->dep;
            }
        }
        if ((ret = ff_discio_file_read(u->fs, f, off, buf, UNIT)) < 0) {
            av_log(u->log, AV_LOG_ERROR, "Blu-ray: error '%s' occurred while reading '%s' at offset '%"PRId64"'\n",
                   av_err2str(ret), clip->stream_path, off);
            return ret;
        }
        return ff_bluray_aacs_unit(u->log, u->aacs, clip, buf);
    }
    return AVERROR_EOF;
}

/* ---- TS packets and PES headers ---- */

typedef struct TsHead {
    uint16_t pid;
    int      pusi;
    int      off, len;               /* payload */
} TsHead;

static int ts_head(const uint8_t *p, TsHead *h)
{
    int off = 4;

    if (p[0] != 0x47 || (p[1] & 0x80))
        return 0;
    h->pusi = p[1] >> 6;
    h->pid  = (p[1] & 0x1f) << 8 | p[2];
    if (p[3] & 0x20) {
        if (p[4] >= 0xb8)
            return 0;
        off = p[4] + 5;
    }
    h->off = off;
    h->len = (p[3] & 0x10) ? 188 - off : 0;
    return 1;
}

/* A PES header: 1 and its stream key (stream_id, or stream_id | extension
 * << 8 for an extended stream id), 0 not usable. */
static int pes_key(const uint8_t *p, int avail, uint16_t *key)
{
    uint8_t sid, flags;

    if (avail < 6 || p[0] || p[1] || p[2] != 1)
        return 0;
    sid  = p[3];
    *key = sid;
    if ((sid & 0xfe) == 0xbe)
        return 1;
    if (avail < 9)
        return 0;
    if (!((sid >= 0xe0 && sid <= 0xe2) || sid == 0xfd || sid == 0xbd || (sid & 0xe0) == 0xc0))
        return 1;
    flags = p[7];
    if ((flags >= 0xc0 || (flags & 0xc0) == 0x80) && avail < 0xe)
        return 0;
    if ((flags & 1) && !(flags & 0x3e)) {
        int o = 9 + 5 * ((flags >> 7) & 1) + 5 * ((flags >> 6) & 1);
        if (avail < o + 1)
            return 0;
        if (p[o] < 0x40 && (p[o] & 1)) {
            int q = o + ((p[o] & 0x10) ? 3 : 1);
            if (avail < q + 2)
                return 0;
            if (p[q] == 0x81 && p[q + 1] < 0x80)
                *key = p[q + 1] << 8 | sid;
        }
    }
    return avail >= p[8] + 9;
}

typedef struct Keys {
    uint16_t key[MAX_KEYS];
    int      count[MAX_KEYS];
    int      n;
} Keys;

/* The PES stream keys of pid until one is seen limit times: 1, 0 the scan
 * failed (logged). */
static int scan_pes(BluraySegReader *u, uint16_t pid, int limit, Keys *k)
{
    int64_t end = (ff_bluray_seg_size(u->s) + UNIT - 1) / UNIT * UNIT;
    uint8_t buf[UNIT];

    memset(k, 0, sizeof(*k));
    for (int64_t pos = 0; pos < end && pos < SCAN_CAP; pos += UNIT) {
        int ret = ff_bluray_seg_read(u, pos, buf), zero = 1;
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return 0;
        for (int i = 0; i < UNIT && zero; i++)
            zero = !buf[i];
        if (zero)
            continue;
        for (int i = 0; i < UNIT / PACKET; i++) {
            const uint8_t *p = buf + i * PACKET + 4;
            const uint8_t *pl;
            TsHead h;
            uint16_t key;
            int e;
            if (!ts_head(p, &h) || h.pid != pid || !h.pusi)
                continue;
            pl = p + h.off;
            if (h.len < 6 || pl[0] || pl[1] || pl[2] != 1)
                continue;
            if ((pl[3] & 0xfe) != 0xbe && (h.len < 9 || pl[8] + 9 > h.len))
                continue;
            if (!pes_key(pl, h.len, &key)) {
                av_log(u->log, AV_LOG_ERROR, "Blu-ray: PID 0x%04x: a PES header cannot be read\n", pid);
                return 0;
            }
            for (e = 0; e < k->n && k->key[e] != key; e++)
                ;
            if (e == k->n) {
                if (k->n == MAX_KEYS)
                    continue;
                k->key[k->n++] = key;
            }
            if (++k->count[e] >= limit)
                return 1;
        }
    }
    return 1;
}

static int has_key(const Keys *k, uint16_t key)
{
    for (int i = 0; i < k->n; i++)
        if (k->key[i] == key)
            return 1;
    return 0;
}

/* ---- program map (BDAV) ---- */

/* The first complete section of table_id on pid within the first 2 MiB
 * (CRC checked), cached per clip; NULL none. */
static const uint8_t *psi_section(BluraySegReader *u, BlurayClip *c, uint8_t table_id, uint16_t pid)
{
    const AVCRC *crc = av_crc_get_table(AV_CRC_32_IEEE);
    uint8_t acc[0x1008], buf[UNIT];
    int64_t limit = FFMIN(ff_bluray_seg_size(u->s), PSI_SCAN);
    int n = 0;

    if (c->pmt_tried)
        return c->pmt_len ? c->pmt : NULL;
    c->pmt_tried = 1;
    for (int64_t pos = 0; pos < limit; pos += UNIT) {
        if (ff_bluray_seg_read(u, pos, buf) < 0)
            break;
        for (int i = 0; i < UNIT / PACKET; i++) {
            const uint8_t *p = buf + i * PACKET + 4;
            TsHead h;
            int len;
            if (!ts_head(p, &h) || h.pid != pid || !h.len)
                continue;
            if (h.pusi) {
                int ptr = p[h.off], rest = h.len - (ptr + 1);
                if (rest <= 0)
                    continue;
                acc[0] = 0;
                memmove(acc, p + h.off + 1 + ptr, FFMIN(rest, (int)sizeof(acc)));
                n = acc[0] == table_id ? FFMIN(rest, (int)sizeof(acc)) : 0;
            } else if (n) {
                int add = FFMIN(h.len, (int)sizeof(acc) - n);
                memcpy(acc + n, p + h.off, add);
                n += add;
            }
            len = n >= 3 ? ((acc[1] & 0x0f) << 8 | acc[2]) + 3 : INT32_MAX;
            if (n >= 3 && n >= len) {
                if (!av_crc(crc, UINT32_MAX, acc, len) && (c->pmt = av_memdup(acc, len))) {
                    c->pmt_len = len;
                    return c->pmt;
                }
                n = 0;
            }
        }
    }
    return NULL;
}

/* The ES descriptors of pid in a PMT: 1 (*d, *len), 0 none. */
static int pmt_es(void *log, const uint8_t *s, uint16_t pid, const uint8_t **d, int *len)
{
    int sl = (s[1] & 0x0f) << 8 | s[2], end = sl - 1, o;

    if (sl < 0xd || s[0] != 2) {
        av_log(log, AV_LOG_DEBUG, "Blu-ray: the program map is not usable\n");
        return 0;
    }
    for (o = ((s[10] & 0x0f) << 8 | s[11]) + 0xc; o + 5 <= end; ) {
        int il = (s[o + 3] & 0x0f) << 8 | s[o + 4];
        if (o + 5 + il > end)
            return 0;
        if (((s[o + 1] & 0x1f) << 8 | s[o + 2]) == pid && il) {
            *d   = s + o + 5;
            *len = il;
            return 1;
        }
        o += 5 + il;
    }
    return 0;
}

static void pmt_kind(void *log, const uint8_t *pmt, BlurayClipStream *e)
{
    const uint8_t *d;
    int len;

    if (!pmt || !pmt_es(log, pmt, e->pid, &d, &len))
        return;
    for (int o = 0; o + 2 <= len; o += 2 + d[o + 1]) {
        uint8_t sub = 0;
        switch (d[o]) {
        case 0x52: sub = 0xc9; break;
        case 0x59: sub = 0xc6; break;
        case 0x6a: sub = 0x41; break;
        case 0x7a: sub = 0x4d; break;
        case 0x7b: sub = 0x43; break;
        case 0x56:
            if (o + 5 < len + 1 && ((d[o + 5] >> 3) == 2 || (d[o + 5] >> 3) == 5))
                sub = 0xc7;
            break;
        }
        if (sub) {
            e->subtype = sub;
            e->probe  |= BLURAY_PROBE_SUBTYPE;
            return;
        }
    }
}

static int pmt_language(void *log, const uint8_t *pmt, uint16_t pid, char lang[4])
{
    const uint8_t *d;
    int len;

    if (!pmt || !pmt_es(log, pmt, pid, &d, &len))
        return 0;
    for (int o = 0; o + 2 <= len; o += 2 + d[o + 1])
        if ((d[o] == 0x0a || d[o] == 0x56 || d[o] == 0x59) && o + 5 <= len) {
            memcpy(lang, d + o + 2, 3);
            lang[3] = 0;
            return 1;
        }
    return 0;
}

/* ---- HEVC second layer ---- */

typedef struct NalScan {
    int     units, sps, pps, vps, rpu;
    int     zeros;                  /* zero bytes before the current byte */
    int     in_nal, header_next;
} NalScan;

static void nal_type(NalScan *n, uint8_t b)
{
    int t = (b >> 1) & 0x3f;
    n->units++;
    n->sps += t == 33;
    n->pps += t == 34;
    n->vps += t == 32;
    n->rpu += t == 62;
}

static int nal_done(const NalScan *n)
{
    return n->units >= HEVC_UNITS || (n->sps >= 6 && n->pps && n->vps);
}

static void nal_bytes(NalScan *n, const uint8_t *p, int len)
{
    for (int i = 0; i < len && !nal_done(n); i++) {
        if (n->header_next) {
            nal_type(n, p[i]);
            n->header_next = 0;
        }
        if (p[i] == 1 && n->zeros >= 2)
            n->header_next = 1;
        n->zeros = p[i] ? 0 : n->zeros + 1;
    }
}

/* The HEVC stream on pid 0x1015 carries Dolby Vision RPUs among its first
 * parameter sets: 1, 0 not, < 0 the stream cannot be read. */
static int hevc_second_layer(BluraySegReader *u)
{
    int64_t end = (ff_bluray_seg_size(u->s) + UNIT - 1) / UNIT * UNIT;
    uint8_t buf[UNIT];
    NalScan n = { 0 };
    int in_pes = 0;

    for (int64_t pos = 0; pos < end && !nal_done(&n); pos += UNIT) {
        int ret = ff_bluray_seg_read(u, pos, buf);
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return ret;
        for (int i = 0; i < UNIT / PACKET && !nal_done(&n); i++) {
            const uint8_t *p = buf + i * PACKET + 4, *pl;
            TsHead h;
            int len;
            if (!ts_head(p, &h) || h.pid != 0x1015 || !h.len)
                continue;
            pl  = p + h.off;
            len = h.len;
            if (h.pusi) {
                if (len < 9 || pl[0] || pl[1] || pl[2] != 1 || pl[8] + 9 > len) {
                    in_pes = 0;
                    continue;
                }
                in_pes = (pl[3] & 0xf0) == 0xe0;
                len -= pl[8] + 9;
                pl  += pl[8] + 9;
            }
            if (in_pes)
                nal_bytes(&n, pl, len);
        }
    }
    av_log(u->log, AV_LOG_DEBUG, "Blu-ray: PID 0x1015: %d NAL units, %d SPS, %d PPS, %d VPS, %d Dolby Vision RPU\n",
           n.units, n.sps, n.pps, n.vps, n.rpu);
    return n.sps && n.pps && n.vps && n.rpu;
}

/* ---- the probe ---- */

int ff_bluray_probe_stream(void *log, DiscIOFS *fs, BlurayAACS *aacs, const BluraySegment *s,
                           BlurayClip *c, BlurayClipStream *e, int code, int nb_parts)
{
    BluraySegReader u;
    BlurayClipStream *f = &c->program.streams[0];
    int m2ts = c->hdr.type == MKBETAG('M', '2', 'T', 'S'), ret = 0;
    Keys k;

    if (e->probe & BLURAY_PROBE_DONE)
        return 0;
    ff_bluray_seg_reader_init(&u, log, fs, aacs, s);
    if (nb_parts >= 2 && code >= 0x40 && code < 0xc0) {
        if (!scan_pes(&u, e->pid, 3, &k)) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        if (!has_key(&k, 0x71fd) && !has_key(&k, 0x76fd)) {
            e->probe |= BLURAY_PROBE_NO_CORE;
            av_log(log, AV_LOG_VERBOSE, "Blu-ray: clip %05u PID 0x%04x has no core substream\n", c->number, e->pid);
        }
    }
    if (m2ts) {
        const uint8_t *pmt = NULL;
        e->probe |= BLURAY_PROBE_M2TS;
        if (e->coding_type == 0x06 || code == 0 || code >= 0x40)
            pmt = psi_section(&u, c, 2, c->program.pmt_pid);
        if (e->coding_type == 0x06)
            pmt_kind(log, pmt, e);
        if (code == 0 || code >= 0x40) {
            int found = pmt_language(log, pmt, e->pid, e->probe_lang);
            for (int i = 0; !found && i < c->program.nb_streams; i++) {
                const BlurayClipStream *v = &c->program.streams[i];
                if (v->coding_type == 0x02 || v->coding_type == 0xea || v->coding_type == 0x1b) {
                    if (v->pid)
                        found = pmt_language(log, pmt, v->pid, e->probe_lang);
                    break;
                }
            }
            if (found)
                e->probe |= BLURAY_PROBE_LANG;
        }
        av_log(log, AV_LOG_DEBUG, "Blu-ray: clip %05u PID 0x%04x: BDAV program map probed [untested on real discs]\n",
               c->number, e->pid);
    }
    if (e->coding_type == 0x24 && f->coding_type == 0x24) {
        if (e->pid == 0x1015 && f->pid != 0x1015) {
            if ((ret = hevc_second_layer(&u)) < 0)
                goto end;
            if (ret) {
                f->probe |= BLURAY_PROBE_HEVC_EL;
                av_log(log, AV_LOG_VERBOSE, "Blu-ray: clip %05u: HEVC second layer (Dolby Vision) on PID 0x1015\n",
                       c->number);
            }
            ret = 0;
        }
    } else if (!m2ts && e->coding_type == 0x1b) {
        if (!scan_pes(&u, e->pid, 1, &k)) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        if (!(k.n == 1 && k.key[0] == 0xe0)) {
            if (!scan_pes(&u, e->pid, 20, &k)) {
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
            if (!k.n) {
                av_log(log, AV_LOG_DEBUG, "Blu-ray: clip %05u: no PES packet of its AVC video\n", c->number);
            } else if (k.n == 1 && k.key[0] != 0xe0) {
                av_log(log, AV_LOG_VERBOSE, "Blu-ray: clip %05u: AVC video on PES stream id 0x%02x\n", c->number,
                       k.key[0]);
                if ((k.key[0] & 0xfff8) == 0xe0)
                    f->probe |= BLURAY_PROBE_AVC_ID | (k.key[0] - 0xe0);
            }
        }
    }
    e->probe |= BLURAY_PROBE_DONE;
end:
    ff_bluray_seg_reader_close(&u);
    return ret;
}

int ff_bluray_pid_scan(void *log, DiscIOFS *fs, BlurayAACS *aacs, const BluraySegment *s, uint16_t pid, int limit)
{
    BluraySegReader u;
    Keys k;
    int ok;

    ff_bluray_seg_reader_init(&u, log, fs, aacs, s);
    ok = scan_pes(&u, pid, limit, &k);

    ff_bluray_seg_reader_close(&u);
    return ok ? k.n : AVERROR_INVALIDDATA;
}
