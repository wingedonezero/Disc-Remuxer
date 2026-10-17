/*
 * The disc layer's title reader: reads the opened title into the title
 * timeline and gives its frames out (DrDiscFrame). The format part says what
 * the title is (DiscTitleSource: its clips, how a block of one is read, its
 * tracks, chapter marks and the format's own values); everything after the
 * blocks is the same for every format: each clip through the clip demuxer
 * (disc/clip_demux.h: raw PES payloads with their own times, DVD / HD DVD
 * sectors and Blu-ray units of source packets through the container
 * splitters in src/mux), the clip's time reference, the payloads to their
 * tracks, the title timeline, its frames, chapters and track results.
 */

#define _GNU_SOURCE                     /* memmem */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "base/buf.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/str.h"
#include "base/vlog.h"
#include "disc/clip_demux.h"
#include "disc/disc.h"
#include "disc/model.h"
#include "timeline/disctl.h"

#define DISC_BLOCK      2048
#define TS_UNIT         6144              /* Blu-ray: 32 source packets of 192 bytes */
#define TS_PACKET       192
#define PTS_REF_SECTORS 2000              /* the PTS wrap reference is looked for in the first sectors */
#define PTS_REF_BACK    27000000LL        /* and lies 300 s (90 kHz) before the first video time */
#define DAMAGE_REPORTS  100               /* damaged ranges said one by one in a title; the rest as detail lines */

/* The clip demuxer's errors as FFmpeg's values, so nothing above sees a
 * change (the block readers' own values pass through as they are). */
static int clip_err(int ret)
{
    switch (ret) {
    case DR_ERR_EOF:      return DR_ERR_EOF;
    case DR_ERR_INVALID:  return DR_ERR_INVALID;
    case DR_ERR_NOMEM:    return DR_ERR(ENOMEM);
    case DR_ERR_EXTERNAL:
    case DR_ERR_BUG:      return DR_ERR_BUG;
    default:              return ret;
    }
}

/* ---- the title reader ---- */

/* Blocks left out as not usable, one after the other in a clip and of one
 * kind (every byte zero, or else no MPEG pack): a damaged range. The disc's
 * own reader names them where it can (DiscTitleSource.damage). */
typedef struct DamageRange {
    int      clip;
    uint32_t first, last;           /* blocks of the clip */
    int      zeros;                 /* every byte of its blocks is zero */
    int      place;                 /* the title timeline's place at its start, -1 none */
    int64_t  disc_before, disc_after; /* the latest video time on the disc before it, the first after it
                                         (ticks, the clip's clock), DR_NOPTS_VALUE: none */
    DiscDamage named;               /* as the disc's reader named it (kind 0: found here) */
} DamageRange;

/* A payload kept until its clip's time reference is known (its bytes
 * copied: the clip demuxer's stay valid only until its next payload). */
typedef struct HeldPayload {
    uint8_t *data;
    int      size;
    int      id;
    int64_t  pts;
    int64_t  pos;
} HeldPayload;

struct DiscTitleReader {
    const DiscTitleSource *src;

    /* the clip being read */
    int            clip;
    uint32_t       block;           /**< next block of that clip */
    uint32_t       nb_blocks;       /**< blocks of that clip */
    int            bs;              /**< bytes per block: 2048 (sectors) or 6144 (Blu-ray units) */
    int            clip_done;       /**< the clip's last block was given to the demuxer */
    uint32_t       skipped;         /**< unusable blocks of that clip left out so far */
    int64_t        pts_ref;         /**< that clip's PTS wrap reference (90 kHz), -1 while not known */
    int64_t        last_sector;     /**< sector of the last PES packet read */
    HeldPayload   *held;            /**< payloads read before the reference was known */
    int            nb_held, held_cap;
    DrClipDemux   *ps;              /**< the clip through the clip demuxer (MPEG-PS / MPEG-TS) */
    int           *pair;            /**< ts: per track with two keys, the key of its last PES with a time
                                         (-1 none yet) */
    int64_t       *pair_pts;        /**< and that PES packet's time */

    /* the title timeline */
    DRTitle       *rip;
    DRChapterPlan  chapter_plan;
    int            first_clip, last_clip; /**< the clips read: the title's, filler clips at its ends left out */
    int           *pos_clip;        /**< position chapters: per open mark, its clip and byte offset */
    int64_t       *pos_off;
    int            nb_pos, next_pos;
    int64_t        blocks_total;    /**< blocks of all the title's clips */
    int64_t        blocks_done;     /**< blocks read so far */
    int            chapters_set;

    /* what was read: blocks left out as not usable, and the packets of
     * streams that feed no track (per stream id: their count and bytes) */
    int64_t        unusable;
    DamageRange   *damage;          /**< the damaged ranges, in reading order */
    int            nb_damage, damage_cap;
    int            damage_open;     /**< the last block read was left out: the last range may grow (src->damage:
                                         a run of blocks left out is being read) */
    int            run_place;       /**< src->damage: the run's place on the title timeline (-1 none), */
    int64_t        run_vlast;       /**< the latest video time before it, */
    uint32_t       run_next;        /**< and the first block of the next range named in it */
    int            damage_wait;     /**< the ranges from this one on wait for the video time after them */
    int64_t        vlast;           /**< the latest video time read in the clip (ticks), DR_NOPTS_VALUE: none */
    struct { int id; int64_t packets, bytes; } unused[32];
    int            nb_unused;
    int64_t        unused_other;    /**< packets of further streams (the table is full) */
    MpegPsDiscStats ps_read;        /**< program streams: what the clips' demuxers read, summed */
    int64_t       *clip_at;         /**< per clip read: where the disc's structure starts it (ticks, its clock),
                                         DR_NOPTS_VALUE: not given */
    int            ts_break;        /**< ts: blocks were left out since the last block given */
    char         **gap_lines;       /**< ts: the stretches of stream files left out, as said */
    int            nb_gap_lines;
    int64_t        gap_bytes;       /**< and their bytes */
};


/* A program-stream clip's reading added to the title's (before its demuxer
 * closes). */
static void ps_read_add(DiscTitleReader *r)
{
    MpegPsDiscStats st;

    if (r->src->ts || !r->ps)
        return;
    dr_clip_ps_stats(r->ps, &st);
    r->ps_read.sectors         += st.sectors;
    r->ps_read.empty           += st.empty;
    r->ps_read.bad             += st.bad;
    r->ps_read.rest_dropped    += st.rest_dropped;
    r->ps_read.rest_bytes      += st.rest_bytes;
    r->ps_read.nav             += st.nav;
    r->ps_read.nav_bytes       += st.nav_bytes;
    r->ps_read.padding         += st.padding;
    r->ps_read.padding_bytes   += st.padding_bytes;
    r->ps_read.system          += st.system;
    r->ps_read.system_bytes    += st.system_bytes;
    r->ps_read.other           += st.other;
    r->ps_read.other_bytes     += st.other_bytes;
    r->ps_read.discarded       += st.discarded;
    r->ps_read.discarded_bytes += st.discarded_bytes;
}

/* A frame's bytes (a base buffer) wrapped for the packet: the packet owns
 * one reference. */

static DiscTitleReader *reader_of(DrDisc *d)
{
    return d->reader;
}

/* Room for one more damaged range. */
static int damage_room(DiscTitleReader *r)
{
    if (r->nb_damage == r->damage_cap) {
        int cap = r->damage_cap ? 2 * r->damage_cap : 16;
        DamageRange *d = dr_realloc_array(r->damage, cap, sizeof(*d));
        if (!d)
            return DR_ERR(ENOMEM);
        r->damage     = d;
        r->damage_cap = cap;
    }
    return 0;
}

/* The place on the title timeline where damage starts: the next picture
 * (none while the clip's packets wait for their time reference). */
static int damage_place(DiscTitleReader *r, int *place)
{
    *place = -1;
    if (r->pts_ref >= 0 && (*place = ff_disctl_title_place(r->rip)) < 0)
        return *place;
    return 0;
}

/* Block b of the clip being read, in buf, is not usable: it starts a damaged
 * range or makes the last one longer (the block after it, of the same kind). */
static int damaged(DrDisc *d, uint32_t b, const uint8_t *buf)
{
    DiscTitleReader *r = reader_of(d);
    DamageRange *g = r->nb_damage ? &r->damage[r->nb_damage - 1] : NULL;
    int zeros = 1, place, ret;

    for (int i = 0; i < r->bs && zeros; i++)
        zeros = !buf[i];
    dr_vlog(d->log, DR_VLOG_DEBUG, "%s %s: block %"PRIu32" is not usable (%s): left out\n", r->src->clip_kind,
           r->src->clip_name(d, r->clip), b, zeros ? "every byte zero" : "no MPEG pack");
    if (r->damage_open && g && g->clip == r->clip && g->last + 1 == b && g->zeros == zeros) {
        g->last = b;
        return 0;
    }
    if ((ret = damage_place(r, &place)) < 0 || (ret = damage_room(r)) < 0)
        return ret;
    r->damage[r->nb_damage++] = (DamageRange){ r->clip, b, b, zeros, place, r->vlast, DR_NOPTS_VALUE, { 0 } };
    r->damage_open = 1;
    return 0;
}

/* src->damage: block b of the clip being read is not usable; the first of
 * a run starts it (where the title is, the video time before it). */
static int run_left_out(DrDisc *d, uint32_t b)
{
    DiscTitleReader *r = reader_of(d);
    int ret;

    dr_vlog(d->log, DR_VLOG_DEBUG, "%s %s: block %"PRIu32" is not usable: left out\n", r->src->clip_kind,
           r->src->clip_name(d, r->clip), b);
    if (r->damage_open)
        return 0;
    if ((ret = damage_place(r, &r->run_place)) < 0)
        return ret;
    r->run_vlast   = r->vlast;
    r->run_next    = b;
    r->damage_open = 1;
    return 0;
}

/* src->damage: a range the disc's reader named (the next blocks of the run
 * being read); 1 one taken, 0 none. */
static int named_damage(DrDisc *d, int flush)
{
    DiscTitleReader *r = reader_of(d);
    DiscDamage g;
    int ret;

    if ((ret = r->src->damage(d, r->clip, flush, &g)) <= 0)
        return ret;
    if ((ret = damage_room(r)) < 0)
        return ret;
    r->damage[r->nb_damage++] = (DamageRange){ r->clip, r->run_next, r->run_next + g.blocks - 1,
                                               g.kind == DISC_DAMAGE_ZERO, r->run_place, r->run_vlast,
                                               DR_NOPTS_VALUE, g };
    r->run_next += g.blocks;
    return 1;
}

/* The next block of the clip being read; EOF at the clip's end. */
static int ps_read(void *opaque, uint8_t *buf, int buf_size)
{
    DrDisc *d = opaque;
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    int ret;

    if (buf_size < r->bs)
        return DR_ERR(EINVAL);
    while (r->block < r->nb_blocks) {
        uint32_t b = r->block++;

        ret = src->read_block(d, r->clip, b, buf);
        if (ret < 0) {
            dr_vlog(d->log, DR_VLOG_ERROR, "%s %s: block %"PRIu32" of %"PRIu32" cannot be read (%s): "
                   "the rest of the %s is not read\n", src->clip_kind, src->clip_name(d, r->clip), b,
                   r->nb_blocks, dr_err_text(ret), src->clip_kind);
            r->block = r->nb_blocks;
            break;
        }
        r->blocks_done++;
        if (r->blocks_total)
            d->progress = r->blocks_done * 10000 / r->blocks_total;
        if (src->damage) {
            /* the range this read ended, if any (blocks before it) */
            int err = named_damage(d, 0);
            if (err < 0)
                return err;
        }
        if (!ret) {
            if ((ret = src->damage ? run_left_out(d, b) : damaged(d, b, buf)) < 0)
                return ret;
            r->skipped++;
            r->unusable++;
            r->ts_break = 1;
            continue;
        }
        /* blocks left out before this one (said as damage): the transport
         * stream's continuity counters start again */
        if (r->ts_break && src->ts)
            dr_clip_ts_break(r->ps);
        r->ts_break    = 0;
        r->damage_open = 0;
        return r->bs;
    }
    /* the clip's reading ends: the range being read too */
    while (src->damage && (ret = named_damage(d, 1)) > 0)
        ;
    if (src->damage && ret < 0)
        return ret;
    r->damage_open = 0;
    r->clip_done = 1;
    return DR_ERR_EOF;
}

static void held_free(DiscTitleReader *r)
{
    for (int i = 0; i < r->nb_held; i++)
        dr_free(r->held[i].data);
    r->nb_held = 0;
}

/* A Blu-ray clip's PTS wrap reference, before the clip is read (the
 * video's port finds it; every other track is linked to the video's):
 * the first TS packet of the video's PID that starts a PES packet of the
 * video's key with a PTS, in the clip's first 2000 units; the reference
 * is that PTS less 300 s. A PES header there that cannot be read ends the
 * search. */
static int ts_reference(DrDisc *d)
{
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    const DiscTsFeed *v = &src->feeds[0];
    uint32_t limit = DR_MIN(PTS_REF_SECTORS, r->nb_blocks);
    uint8_t *buf;
    int ret = 0;

    if (!(buf = dr_malloc(TS_UNIT)))
        return DR_ERR(ENOMEM);
    for (uint32_t u = 0; u < limit; u++) {
        if ((ret = src->read_block(d, r->clip, u, buf)) < 0) {
            dr_vlog(d->log, DR_VLOG_ERROR, "%s %s: unit %"PRIu32" cannot be read (%s): no time reference\n",
                   src->clip_kind, src->clip_name(d, r->clip), u, dr_err_text(ret));
            goto end;
        }
        for (int i = 0; ret && i < TS_UNIT / TS_PACKET; i++) {
            const uint8_t *p = buf + i * TS_PACKET + 4;
            int pid, off = 4, len;
            uint16_t key;
            int64_t pts;

            if (p[0] != 0x47 || (p[1] & 0x80))
                continue;
            if ((p[3] & 0x20) && p[4] >= 0xb8)
                continue;
            if (p[3] & 0x20)
                off = p[4] + 5;
            pid = (p[1] & 0x1f) << 8 | p[2];
            len = (p[3] & 0x10) ? 188 - off : 0;
            if (pid != v->pid || !(p[1] & 0x40))
                continue;
            if (!ff_mpegts_disc_pes_header(p + off, len, &key, &pts)) {
                dr_vlog(d->log, DR_VLOG_ERROR, "%s %s: a PES header of the video (PID 0x%04x) in unit %"PRIu32" cannot be "
                       "read: no time reference\n", src->clip_kind, src->clip_name(d, r->clip), pid, u);
                ret = DR_ERR_INVALID;
                goto end;
            }
            if ((v->key[0] && key != v->key[0]) || pts == DR_NOPTS_VALUE)
                continue;
            r->pts_ref = pts >= PTS_REF_BACK ? pts - PTS_REF_BACK : pts + (1LL << 33) - PTS_REF_BACK;
            dr_vlog(d->log, DR_VLOG_VERBOSE, "%s %s: first video time %"PRId64" in unit %"PRIu32": wrap reference %"
                   PRId64"\n", src->clip_kind, src->clip_name(d, r->clip), pts, u, r->pts_ref);
            ret = 0;
            goto end;
        }
    }
    dr_vlog(d->log, DR_VLOG_ERROR, "%s %s: no video time in its first %"PRIu32" units: no time reference\n",
           src->clip_kind, src->clip_name(d, r->clip), limit);
    ret = DR_ERR_INVALID;
end:
    dr_free(buf);
    return ret;
}

/* ---- stretches of a Blu-ray stream file left out ----
 * The clip demuxer keeps each stretch it
 * leaves out: a run of TS packets that cannot be read (one reason), a run
 * of all-zero source packets, a PID's data from a PES packet that cannot be
 * read to its next PES start. Each is said (a warning) with where it lies:
 * the stream file and its bytes (the format part's block_place), and
 * counted as damaged data for the title's summary. */

/* Byte pos of clip (the title's clips; -1 a side clip) as file + offset:
 * 1 known, 0 not. */
static int gap_place(DrDisc *d, int clip, int64_t pos, char *file, int size, int64_t *off)
{
    const DiscTitleSource *src = reader_of(d)->src;
    uint32_t block = pos / TS_UNIT;

    if (clip < 0 || !src->block_place || src->block_place(d, clip, block, file, size, off) < 0)
        return 0;
    *off += pos % TS_UNIT;
    return 1;
}

/* Bytes [a, b] of the clip in words, into buf. */
static void gap_bytes(DrDisc *d, int clip, const char *name, int64_t a, int64_t b, char *buf, int size)
{
    char f0[64], f1[64];
    int64_t o0, o1;

    if (!gap_place(d, clip, a, f0, sizeof(f0), &o0) || !gap_place(d, clip, b, f1, sizeof(f1), &o1))
        snprintf(buf, size, "bytes %"PRId64"-%"PRId64, a, b);
    else if (strcmp(f0, f1))
        snprintf(buf, size, "bytes %"PRId64" of %s to %"PRId64" of %s", o0, f0, o1, f1);
    else if (strlen(name) >= strlen(f0) && !strcmp(name + strlen(name) - strlen(f0), f0))
        snprintf(buf, size, "bytes %"PRId64"-%"PRId64, o0, o1);
    else
        snprintf(buf, size, "bytes %"PRId64"-%"PRId64" of %s", o0, o1, f0);
}

/* Byte pos of the clip in words, into buf. */
static void gap_byte(DrDisc *d, int clip, const char *name, int64_t pos, char *buf, int size)
{
    char f[64];
    int64_t o;

    if (!gap_place(d, clip, pos, f, sizeof(f), &o))
        snprintf(buf, size, "byte %"PRId64, pos);
    else if (strlen(name) >= strlen(f) && !strcmp(name + strlen(name) - strlen(f), f))
        snprintf(buf, size, "byte %"PRId64, o);
    else
        snprintf(buf, size, "byte %"PRId64" of %s", o, f);
}

/* The tracks a PID feeds, in words ("track 2", "tracks 2, 3"). */
static void gap_tracks(const DiscTitleSource *src, int pid, int side, char *buf, int size)
{
    int n = 0, len = 0;

    buf[0] = 0;
    for (int i = 0; i < src->nb_tracks && len < size; i++)
        if (src->feeds[i].pid == pid && src->feeds[i].side == side)
            len += snprintf(buf + len, size - len, "%s%d", n++ ? ", " : "", i);
    if (!n)
        snprintf(buf, size, "no track");
    else {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s %s", n > 1 ? "tracks" : "track", buf);
        snprintf(buf, size, "%s", tmp);
    }
}

/* The stretches the demuxer dm has left out of clip (side: side clip k + 1,
 * name its name), said and kept; flush: the clip's reading has ended. */
static int gaps_said(DrDisc *d, DrClipDemux *dm, int clip, int side, const char *name, int flush)
{
    DiscTitleReader *r = reader_of(d);
    MpegTSDiscGap g;
    int ret;

    while ((ret = clip_err(dr_clip_gap(dm, flush, &g))) > 0) {
        char where[192], at[160], tracks[64], line[640];
        int64_t bytes;

        flush = 0;                      /* the stretches still open ended with the first call */
        if (g.kind == MPEGTS_DISC_GAP_PTS) {
            /* nothing is left out: said, not counted */
            gap_tracks(r->src, g.pid, side, tracks, sizeof(tracks));
            gap_byte(d, clip, name, g.pos, where, sizeof(where));
            dr_vlog(d->log, DR_VLOG_WARNING, "%s: PID 0x%04x (%s): the PES header at %s has a PTS whose marker bits are "
                   "not set: read as having no time (the payload takes no time of its own)\n", name, g.pid, tracks,
                   where);
            continue;
        }
        if (g.kind == MPEGTS_DISC_GAP_DUP) {
            /* an exact repeat: left out, said, not damage */
            gap_tracks(r->src, g.pid, side, tracks, sizeof(tracks));
            gap_byte(d, clip, name, g.pos, where, sizeof(where));
            dr_vlog(d->log, DR_VLOG_WARNING, "%s: PID 0x%04x (%s): a duplicate transport packet at %s (the packet "
                   "before it again, byte for byte): left out\n", name, g.pid, tracks, where);
            continue;
        }
        if (g.kind == MPEGTS_DISC_GAP_CUT) {
            /* its rest is missing (the stretch next to it says why): said, not counted again */
            gap_tracks(r->src, g.pid, side, tracks, sizeof(tracks));
            gap_byte(d, clip, name, g.pos, where, sizeof(where));
            dr_vlog(d->log, DR_VLOG_WARNING, "%s: PID 0x%04x (%s): a PES packet cut short by the next PES start at %s: "
                   "its last %"PRId64" bytes are missing\n", name, g.pid, tracks, where, g.bytes);
            continue;
        }
        if (g.kind == MPEGTS_DISC_GAP_CC) {
            /* packets missing from the stream file: the data around them is kept */
            gap_tracks(r->src, g.pid, side, tracks, sizeof(tracks));
            gap_byte(d, clip, name, g.pos, where, sizeof(where));
            bytes = 0;
            if (g.packets)
                snprintf(line, sizeof(line), "%s: PID 0x%04x (%s): the continuity counter jumps from %d to %d at %s: "
                         "%"PRId64" transport packet(s) missing before it (counted modulo 16); the data around them "
                         "is kept", name, g.pid, tracks, g.cc_from, g.cc_to, where, g.packets);
            else
                snprintf(line, sizeof(line), "%s: PID 0x%04x (%s): the continuity counter stays at %d at %s with "
                         "other bytes (a packet out of order or missing); the data is kept", name, g.pid, tracks,
                         g.cc_to, where);
        } else if (g.kind == MPEGTS_DISC_GAP_PES) {
            gap_tracks(r->src, g.pid, side, tracks, sizeof(tracks));
            gap_byte(d, clip, name, g.pos, where, sizeof(where));
            if (g.to_end)
                snprintf(at, sizeof(at), "the end of the stream file");
            else {
                char b[96];
                gap_byte(d, clip, name, g.end, b, sizeof(b));
                snprintf(at, sizeof(at), "the next PES start at %s", b);
            }
            bytes = g.bytes;
            snprintf(line, sizeof(line), "%s: PID 0x%04x (%s): %s at %s: %"PRId64" bytes left out until %s", name,
                     g.pid, tracks, g.why, where, g.bytes, at);
        } else {
            gap_bytes(d, clip, name, g.pos, g.end - 1, where, sizeof(where));
            bytes = g.end - g.pos;
            if (g.kind == MPEGTS_DISC_GAP_BAD)
                snprintf(line, sizeof(line), "%s: %"PRId64" transport packets at %s cannot be read (%s): their data "
                         "is left out", name, g.packets, where, g.why);
            else if (bytes % TS_UNIT)
                snprintf(line, sizeof(line), "%s: %"PRId64" bytes of zeros at %s (%.2f units): skipped (blank or "
                         "unreadable sectors)", name, bytes, where, bytes / (double)TS_UNIT);
            else
                snprintf(line, sizeof(line), "%s: %"PRId64" bytes of zeros at %s (%"PRId64" units): skipped (blank "
                         "or unreadable sectors)", name, bytes, where, bytes / TS_UNIT);
        }
        dr_vlog(d->log, DR_VLOG_WARNING, "%s\n", line);
        r->gap_bytes += bytes;
        {
            char *l = dr_strdup(line);
            if (!l || dr_array_add_ptr(&r->gap_lines, &r->nb_gap_lines, l) < 0) {
                dr_free(l);
                return DR_ERR(ENOMEM);
            }
        }
    }
    return ret;
}

/* The clip being read in words: "Segment /BDMV/STREAM/00001.m2ts". */
static const char *clip_words(DrDisc *d, char *buf, int size)
{
    DiscTitleReader *r = reader_of(d);

    snprintf(buf, size, "%s %s", r->src->clip_kind, r->src->clip_name(d, r->clip));
    if (buf[0] >= 'a' && buf[0] <= 'z')
        buf[0] += 'A' - 'a';
    return buf;
}

/* Start reading clip k of the title (a segment of the title timeline): its
 * blocks, its PTS wrap reference (Blu-ray: found before the clip is read;
 * else still to be found). */
static int clip_start(DrDisc *d, int k)
{
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    int ret;

    r->clip        = k;
    r->block       = 0;
    r->nb_blocks   = src->clip_blocks(d, k);
    r->clip_done   = 0;
    r->skipped     = 0;
    r->damage_open = 0;
    r->damage_wait = r->nb_damage;
    r->vlast       = DR_NOPTS_VALUE;
    r->pts_ref     = -1;
    r->last_sector = -1;
    held_free(r);
    for (int i = 0; src->ts && i < src->nb_tracks; i++)
        r->pair[i] = -1;
    dr_vlog(d->log, DR_VLOG_VERBOSE, "Reading %s %s (%d of %d): %"PRIu32" %s\n",
           src->clip_kind, src->clip_name(d, k), k + 1, src->nb_clips, r->nb_blocks, src->ts ? "units" : "blocks");
    ps_read_add(r);
    dr_clip_close(&r->ps);
    if (src->ts) {
        /* the stretches left out are kept for gaps_said() */
        if ((ret = ts_reference(d)) < 0 ||
            (ret = clip_err(dr_clip_ts_open(&r->ps, (void *)d->log, src->pids, src->nb_pids, 0, 1, ps_read, d))) < 0)
            return ret;
    } else if ((ret = clip_err(dr_clip_ps_open(&r->ps, (void *)d->log, src->lpcm_header, ps_read, d))) < 0)
        return ret;
    return ff_disctl_title_segment(r->rip);
}

void ff_disc_title_reader_close(DiscTitleReader **reader)
{
    DiscTitleReader *r = *reader;

    if (!r)
        return;
    dr_clip_close(&r->ps);
    held_free(r);
    dr_freep(&r->held);
    dr_freep(&r->pair);
    dr_freep(&r->pair_pts);
    dr_freep(&r->pos_clip);
    dr_freep(&r->pos_off);
    dr_freep(&r->clip_at);
    dr_freep(&r->damage);
    for (int i = 0; i < r->nb_gap_lines; i++)
        dr_free(r->gap_lines[i]);
    dr_freep(&r->gap_lines);
    ff_disctl_title_close(&r->rip);
    ff_disctl_chapter_plan_free(&r->chapter_plan);
    dr_freep(reader);
}

/* Position chapters (DVD-Video): the records' marks. A broken tail (a last
 * record at 0 after one that is not, or earlier than the one before it) is
 * cut off; the records' clips must not go back (a record without a clip
 * counts as after every clip), else the title fails; the records on the
 * clips read are kept, each an open mark that the first video frame
 * from its position gives its time; when the first record is not kept and
 * the first kept one is not at the title's very start, record 0 is a mark
 * at 0. */
static int position_plan(DrDisc *d, DiscTitleReader *r)
{
    const DiscTitleSource *src = r->src;
    const int64_t *t = src->chapter_times;
    DRChapterPlan *p = &r->chapter_plan;
    int n = src->nb_chapters, first = -1, at_start = 0, m = 0;
    int64_t prev = -1;

    memset(p, 0, sizeof(*p));
    while (n > 1 && ((!t[n - 1] && t[n - 2]) || (uint64_t)t[n - 2] > (uint64_t)t[n - 1])) {
        dr_vlog(d->log, DR_VLOG_WARNING, "Chapters: record %d does not follow record %d: cut off with the records after "
               "it\n", n - 1, n - 2);
        n--;
    }
    for (int i = 0; i < n; i++) {
        int64_t kind = src->chapter_clips[i] < 0 ? INT64_MAX : src->chapter_clips[i];
        if (kind < prev) {
            dr_vlog(d->log, DR_VLOG_ERROR, "Chapters: record %d lies on an earlier segment than the one before it: the "
                   "title's chapter list is broken\n", i);
            return DR_ERR_INVALID;
        }
        prev = kind;
    }
    for (int i = 0; i < n; i++)
        if (src->chapter_clips[i] >= r->first_clip && src->chapter_clips[i] <= r->last_clip) {
            first    = i;
            at_start = !src->chapter_offsets[i] && src->chapter_clips[i] == r->first_clip;
            break;
        }
    if (first < 0)
        return 0;
    if (!(p->marks = dr_calloc(n + 1, sizeof(*p->marks))) || !(p->atoms = dr_calloc(n + 1, sizeof(*p->atoms))) ||
        !(r->pos_clip = dr_calloc(n + 1, sizeof(*r->pos_clip))) ||
        !(r->pos_off = dr_calloc(n + 1, sizeof(*r->pos_off))))
        return DR_ERR(ENOMEM);
    if (first && !at_start) {
        p->marks[m]   = 0;
        p->atoms[m++] = 0;
    }
    for (int i = first; i < n; i++) {
        if (src->chapter_clips[i] < r->first_clip || src->chapter_clips[i] > r->last_clip) {
            dr_vlog(d->log, DR_VLOG_INFO, "Chapters: record %d lies on no segment that is read: left out\n", i);
            continue;
        }
        r->pos_clip[r->nb_pos]  = src->chapter_clips[i];
        r->pos_off[r->nb_pos++] = src->chapter_offsets[i];
        p->marks[m]   = INT64_MAX;
        p->atoms[m++] = i;
    }
    p->nb_marks = p->nb_atoms = m;
    return 0;
}

/* ---- filler clips at a title's ends ----
 * Before a title is read, clips at its start and at its end that are mere
 * filler are left out (the reference's rip does the same): from the first
 * clip on while a clip is filler (never the last), then from the last clip
 * back while one is filler (never down to the first kept one). A clip is
 * never filler when the title has no track but its video or the clip is
 * larger than 120 MiB. Else it is read: without a video frame it is filler;
 * at the end, its first (frame rate + 4) video frames spanning less than
 * 1.001 s with every I picture below 5120 bytes, P below 1024 and B below
 * 512 make it filler; otherwise (both ends) it is filler when none of its
 * other tracks has a frame and its video ends less than 15 s after its
 * first time. A leading clip left out shortens the title's chapter times by
 * its video's length. LOOK INTO AGAIN LATER: whether these clips are filler
 * in every case. */

#define FILLER_MAX_BYTES  0x7800000LL       /* 120 MiB */
#define FILLER_SPAN       16200000001LL     /* 15 s (ticks) */
#define FILLER_TAIL_SPAN  1081080001LL      /* 1.001 s */

typedef struct FillerTrack {
    struct FillerProbe *p;
    int                 index;
    DRFrames           *fr;
} FillerTrack;

typedef struct FillerProbe {
    DrDisc *d;
    int              clip;
    uint32_t         block, nb_blocks;
    int              other_frame;           /* a track but the video has a frame */
    int              captions;              /* the title has a caption track (its data is in the video) */
    int              video_frames, tail_n;  /* video units seen; units the end test takes (0: no test) */
    int64_t          first, end;            /* video: first time, last time + one frame (ticks) */
    int64_t          tail_min, tail_max;    /* the end test's units' times */
    uint32_t         tail_size[3];          /* their largest I, P, B unit */
    int64_t          frame;                 /* one video frame (ticks), 0 not known */
} FillerProbe;

static int feed_key(const DiscTsFeed *f, int pid, uint16_t key);
static int block_words(void *opaque, int seg, int64_t block, char *buf, int size);

static int filler_read(void *opaque, uint8_t *buf, int size)
{
    FillerProbe *p = opaque;
    const DiscTitleSource *src = &p->d->src;
    int bs = src->ts ? TS_UNIT : DISC_BLOCK, ret;

    if (size < bs)
        return DR_ERR(EINVAL);
    while (p->block < p->nb_blocks) {
        if ((ret = src->read_block(p->d, p->clip, p->block++, buf)) < 0)
            return ret;
        if (ret)
            return bs;
    }
    return DR_ERR_EOF;
}

/* MPEG-1/2: the picture_coding_type of the unit's picture (1 I, 2 P, 3 B), 0 none. */
static int mpv_coding_type(const uint8_t *d, int n)
{
    for (int i = 0; i + 5 < n; i++)
        if (!d[i] && !d[i + 1] && d[i + 2] == 1 && !d[i + 3])
            return d[i + 5] >> 3 & 7;
    return 0;
}

/* MPEG-1/2: the frame rate of a sequence header in the unit (ticks per frame), 0 none. */
static int64_t mpv_frame(const uint8_t *d, int n, int *num, int *den)
{
    static const int rates[9][2] = { { 0, 1 }, { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 30, 1 },
                                     { 50, 1 }, { 60000, 1001 }, { 60, 1 } };
    for (int i = 0; i + 8 < n; i++)
        if (!d[i] && !d[i + 1] && d[i + 2] == 1 && d[i + 3] == 0xb3) {
            int c = d[i + 7] & 15;
            if (c < 1 || c > 8)
                return 0;
            *num = rates[c][0];
            *den = rates[c][1];
            return (int64_t)DR_TICKS_PER_SECOND * rates[c][1] / rates[c][0];
        }
    return 0;
}

static int filler_unit(void *opaque, const DRUnit *u)
{
    FillerTrack *t = opaque;
    FillerProbe *p = t->p;
    const DRTitleTrack *k = &p->d->src.tracks[t->index];

    static const uint8_t cc[8] = { 0x00, 0x00, 0x01, 0xb2, 'C', 'C', 0x01, 0xf8 };

    if (t->index) {
        p->other_frame = 1;
        return 0;
    }
    /* the caption track has a frame when the video carries caption data */
    if (p->captions && memmem(u->data, u->size, cc, sizeof(cc)))
        p->other_frame = 1;
    if (!p->frame) {
        int num = 0, den = 0;
        if (k->codec == DR_CODEC_MPEG1VIDEO || k->codec == DR_CODEC_MPEG2VIDEO)
            p->frame = mpv_frame(u->data, u->size, &num, &den);
        else if (u->pic && u->pic->rate_num > 0 && u->pic->rate_den > 0) {
            num = u->pic->rate_num;
            den = u->pic->rate_den;
            p->frame = (int64_t)DR_TICKS_PER_SECOND * den / num;
        }
        if (p->frame && !p->video_frames)
            p->tail_n = (int)((int64_t)num * 1000 / ((int64_t)den * 1000)) + 4;
    }
    if (u->time != DR_NOPTS_VALUE) {
        if (p->first == DR_NOPTS_VALUE || u->time < p->first)
            p->first = u->time;
        p->end = DR_MAX(p->end, u->time + p->frame);
    }
    if (p->video_frames < p->tail_n) {
        int c = k->codec == DR_CODEC_MPEG1VIDEO || k->codec == DR_CODEC_MPEG2VIDEO ?
                mpv_coding_type(u->data, u->size) :
                u->pic ? (u->pic->type == DR_PIC_I ? 1 : u->pic->type == DR_PIC_P ? 2 :
                          u->pic->type == DR_PIC_B ? 3 : 0) : 0;
        if (c >= 1 && c <= 3)
            p->tail_size[c - 1] = DR_MAX(p->tail_size[c - 1], (uint32_t)u->size);
        if (u->time != DR_NOPTS_VALUE) {
            p->tail_min = p->tail_min == DR_NOPTS_VALUE ? u->time : DR_MIN(p->tail_min, u->time);
            p->tail_max = p->tail_max == DR_NOPTS_VALUE ? u->time : DR_MAX(p->tail_max, u->time);
        }
    }
    p->video_frames++;
    return 0;
}

/* Whether clip k is filler (*len: its video's length, ticks). */
static int filler_clip(DrDisc *d, int k, int at_end, int *filler, int64_t *len)
{
    const DiscTitleSource *src = &d->src;
    int bs = src->ts ? TS_UNIT : DISC_BLOCK, others = 0, ret = 0, whole;
    FillerProbe p = { .d = d, .clip = k, .nb_blocks = src->clip_blocks(d, k),
                      .first = DR_NOPTS_VALUE, .tail_min = DR_NOPTS_VALUE, .tail_max = DR_NOPTS_VALUE };
    FillerTrack *t;
    DrClipDemux *dm = NULL;

    *filler = 0;
    *len    = 0;
    for (int i = 1; i < src->nb_tracks; i++)
        others += src->tracks[i].kind != DR_KIND_VIDEO && src->tracks[i].kind != DR_KIND_LAYER;
    if (!others || (int64_t)p.nb_blocks * bs > FILLER_MAX_BYTES)
        return 0;
    if (!(t = dr_calloc(src->nb_tracks, sizeof(*t))))
        return DR_ERR(ENOMEM);
    for (int i = 0; i < src->nb_tracks; i++) {
        t[i] = (FillerTrack){ &p, i, NULL };
        p.captions |= src->tracks[i].kind == DR_KIND_CAPTION;
        if (src->tracks[i].kind == DR_KIND_CAPTION || src->tracks[i].kind == DR_KIND_LAYER ||
            src->tracks[i].codec == DR_CODEC_PCM_DVD || src->tracks[i].codec == DR_CODEC_PCM_BLURAY)
            continue;                    /* captions: in the video; LPCM: any payload is a frame */
        if (ff_disctl_frames_open(&t[i].fr, (void *)d->log, src->tracks[i].codec, filler_unit, &t[i]) < 0)
            t[i].fr = NULL;              /* no parser: any payload is a frame */
    }
    if (!t[0].fr) {
        dr_vlog(d->log, DR_VLOG_WARNING, "%s %s: its video cannot be cut into frames: not checked for filler\n",
               src->clip_kind, src->clip_name(d, k));
        goto end;
    }
    if ((ret = clip_err(src->ts ? dr_clip_ts_open(&dm, (void *)d->log, src->pids, src->nb_pids, 0, 0, filler_read, &p) :
                                  dr_clip_ps_open(&dm, (void *)d->log, src->lpcm_header, filler_read, &p))) < 0)
        goto end;
    /* read until the video has a frame, (at the end) the end test has its
     * frames, and a track but the video has a frame or the video has run
     * 15 s; or until the clip ends */
    while (!(p.video_frames && (!at_end || p.video_frames >= p.tail_n) &&
             (p.other_frame || (p.first != DR_NOPTS_VALUE && p.end - p.first >= FILLER_SPAN)))) {
        DrClipPacket pkt;
        if ((ret = clip_err(dr_clip_read(dm, &pkt))) < 0)
            break;
        for (int i = 0; i < src->nb_tracks; i++) {
            int64_t time = pkt.pts == DR_NOPTS_VALUE ? DR_NOPTS_VALUE : pkt.pts * DR_TICKS_PER_PTS;
            if (src->ts ? feed_key(&src->feeds[i], pkt.id & 0xffff, (uint32_t)pkt.id >> 16) < 0
                        : d->track_ids[i] != pkt.id)
                continue;
            if (t[i].fr) {
                if ((ret = ff_disctl_frames_write(t[i].fr, pkt.data, pkt.size, time)) < 0)
                    break;
            } else if (i && pkt.size > (src->tracks[i].codec == DR_CODEC_PCM_DVD ? src->lpcm_header : 0)) {
                p.other_frame = 1;
            }
        }
        if (ret < 0)
            break;
    }
    /* the units still in the parsers: the video's always; the other
     * tracks' only when the clip was read to its end (a read stopped early
     * cuts their last unit, and their frames decide nothing more then: the
     * loop stopped with one found, or with the video's span long enough) */
    whole = ret == DR_ERR_EOF;
    if (ret == DR_ERR_EOF)
        ret = 0;
    for (int i = 0; ret >= 0 && i < src->nb_tracks; i++)
        if (t[i].fr && (!i || whole))
            ret = ff_disctl_frames_flush(t[i].fr);
    if (ret < 0)
        goto end;
    dr_vlog(d->log, DR_VLOG_DEBUG, "%s %s: filler check: %d video frame(s) from %"PRId64" to %"PRId64" ticks, another "
           "track's frame %s; end test over %d frame(s): span %"PRId64", largest I %u P %u B %u bytes\n",
           src->clip_kind, src->clip_name(d, k), p.video_frames, p.first, p.end, p.other_frame ? "yes" : "no",
           at_end ? p.tail_n : 0, p.tail_max - p.tail_min, p.tail_size[0], p.tail_size[1], p.tail_size[2]);
    if (p.first == DR_NOPTS_VALUE) {
        *filler = 1;                     /* no video frame */
    } else if (at_end && p.tail_n && p.tail_min != DR_NOPTS_VALUE && p.tail_max - p.tail_min < FILLER_TAIL_SPAN &&
               p.tail_size[0] < 0x1400 && p.tail_size[1] < 0x400 && p.tail_size[2] < 0x200) {
        *filler = 1;
        *len    = p.tail_max - p.tail_min;
    } else if (!p.other_frame && p.end - p.first < FILLER_SPAN) {     /* (the video has times: first is set) */
        *filler = 1;
        *len    = p.end - p.first;
    }
end:
    for (int i = 0; i < src->nb_tracks; i++)
        ff_disctl_frames_close(&t[i].fr);
    dr_clip_close(&dm);
    dr_free(t);
    return ret;
}

/* The clips to read (first_clip .. last_clip) and the length of the filler
 * left out before them (*skip, ticks). */
/* Clips a .. b (from 0), filler at the title's start or end, said in one
 * line (why: the reason, NULL none). */
static void filler_said(DrDisc *d, int a, int b, const char *where, const char *why)
{
    const DiscTitleSource *src = reader_of(d)->src;
    char reason[64] = "";

    if (why)
        snprintf(reason, sizeof(reason), " (%s)", why);
    if (a == b)
        dr_vlog(d->log, DR_VLOG_INFO, "%s %s (%d of %d): filler at the title's %s%s: left out\n", src->clip_kind,
               src->clip_name(d, a), a + 1, src->nb_clips, where, reason);
    else
        dr_vlog(d->log, DR_VLOG_INFO, "%ss %s to %s (%d-%d of %d): filler at the title's %s%s: left out\n", src->clip_kind,
               src->clip_name(d, a), src->clip_name(d, b), a + 1, b + 1, src->nb_clips, where, reason);
}

static int filler_trim(DrDisc *d, DiscTitleReader *r, int64_t *skip)
{
    const DiscTitleSource *src = r->src;
    int first = 0, last = src->nb_clips - 1, filler, ret, run = 0;
    const char *why = NULL, *w;
    int64_t len;

    *skip = 0;
    /* a run of clips left out for the same reason is one line */
    for (; first < last; first++) {
        if ((ret = filler_clip(d, first, 0, &filler, &len)) < 0)
            return ret;
        if (!filler)
            break;
        w = len ? "short, without other tracks' data" : "no video frame";
        if (first && why && w != why) {
            filler_said(d, run, first - 1, "start", why);
            run = first;
        }
        why = w;
        *skip += len;
    }
    if (why)
        filler_said(d, run, first - 1, "start", why);
    run = last;
    for (; last > first; last--) {
        if ((ret = filler_clip(d, last, 1, &filler, &len)) < 0)
            return ret;
        if (!filler)
            break;
    }
    if (last < run)
        filler_said(d, last + 1, run, "end", NULL);
    r->first_clip = first;
    r->last_clip  = last;
    return 0;
}

/* ---- clips of their own (Blu-ray text subtitles) ----
 * Each side clip a track takes is read whole before the title's clips,
 * through the clip demuxer with its tracks' PIDs (a
 * PES packet without a PTS that starts with a dialog takes the dialog's
 * start). Its PTS wrap reference is the first time read in it, less 300 s
 * (a time below it has wrapped). Its tracks get its payloads on its own
 * clock; the format part tells how far that clock lies ahead of the first
 * clip's. A unit that cannot be read ends the clip's reading (said). */

typedef struct SideRead {
    DrDisc *d;
    int              side;
    uint32_t         block, nb_blocks;
    int64_t          left_out;          /* units not usable */
} SideRead;

static int side_unit(void *opaque, uint8_t *buf, int size)
{
    SideRead *sr = opaque;
    const DiscTitleSource *src = reader_of(sr->d)->src;

    if (size < TS_UNIT)
        return DR_ERR(EINVAL);
    while (sr->block < sr->nb_blocks) {
        uint32_t b = sr->block++;
        int ret = src->side_read(sr->d, sr->side, b, buf);
        if (ret < 0) {
            dr_vlog(sr->d->log, DR_VLOG_WARNING, "%s: unit %"PRIu32" of %"PRIu32" cannot be read (%s): the rest of it is "
                   "not read\n", src->side_name(sr->d, sr->side), b, sr->nb_blocks, dr_err_text(ret));
            sr->block = sr->nb_blocks;
            break;
        }
        if (!ret) {
            sr->left_out++;
            continue;
        }
        return TS_UNIT;
    }
    return DR_ERR_EOF;
}

/* A payload of the side clip, copied and kept until the whole clip is read
 * (its wrap reference is its first time). */
static int side_hold(HeldPayload **held, int *nb, int *cap, const DrClipPacket *p)
{
    uint8_t *copy;

    if (*nb == *cap) {
        int c = *cap ? 2 * *cap : 64;
        HeldPayload *h = dr_realloc_array(*held, c, sizeof(*h));
        if (!h)
            return DR_ERR(ENOMEM);
        *held = h;
        *cap  = c;
    }
    if (!(copy = dr_memdup(p->data, p->size ? p->size : 1)))
        return DR_ERR(ENOMEM);
    (*held)[(*nb)++] = (HeldPayload){ copy, p->size, p->id, p->pts, p->pos };
    return 0;
}

static int read_side(DrDisc *d, int side)
{
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    const char *name = src->side_name(d, side);
    SideRead sr = { d, side, 0, src->side_blocks(d, side), 0 };
    HeldPayload *held = NULL;
    int nb_held = 0, held_cap = 0, pids[16], nb_pids = 0, ret;
    int64_t offset = 0, ref = -1;
    DrClipDemux *dm = NULL;

    for (int i = 0; i < src->nb_tracks; i++) {
        int p = 0;
        if (src->feeds[i].side != side + 1)
            continue;
        while (p < nb_pids && pids[p] != src->feeds[i].pid)
            p++;
        if (p == nb_pids && nb_pids < (int)DR_ARRAY_ELEMS(pids))
            pids[nb_pids++] = src->feeds[i].pid;
    }
    if (!nb_pids)
        return 0;
    src->side_offset(d, side, r->first_clip, &offset);
    for (int i = 0; i < src->nb_tracks; i++)
        if (src->feeds[i].side == side + 1 && (ret = ff_disctl_title_side_open(r->rip, i, offset, name)) < 0)
            return ret;
    dr_vlog(d->log, DR_VLOG_VERBOSE, "Reading %s: %"PRIu32" units\n", name, sr.nb_blocks);
    /* text subtitles (dialog times), the stretches left out kept for gaps_said() */
    if ((ret = clip_err(dr_clip_ts_open(&dm, (void *)d->log, pids, nb_pids, 1, 1, side_unit, &sr))) < 0)
        goto end;
    /* the whole clip first: its wrap reference is its first time */
    for (;;) {
        DrClipPacket pkt;
        if ((ret = clip_err(dr_clip_read(dm, &pkt))) < 0)
            break;
        if (ref < 0 && pkt.pts != DR_NOPTS_VALUE) {
            ref = pkt.pts >= PTS_REF_BACK ? pkt.pts - PTS_REF_BACK : pkt.pts + (1LL << 33) - PTS_REF_BACK;
            dr_vlog(d->log, DR_VLOG_VERBOSE, "%s: first time %"PRId64" at byte %"PRId64": wrap reference %"PRId64"\n",
                   name, pkt.pts, pkt.pos, ref);
        }
        if ((ret = side_hold(&held, &nb_held, &held_cap, &pkt)) < 0)
            goto end;
    }
    if (ret != DR_ERR_EOF) {
        dr_vlog(d->log, DR_VLOG_WARNING, "%s cannot be read to its end (%s): what was read is used\n", name,
               dr_err_text(ret));
    }
    if (sr.left_out)
        dr_vlog(d->log, DR_VLOG_WARNING, "%s: %"PRId64" units not usable: left out\n", name, sr.left_out);
    {
        char words[256];
        snprintf(words, sizeof(words), "%s", name);
        if (words[0] >= 'a' && words[0] <= 'z')
            words[0] += 'A' - 'a';
        if ((ret = gaps_said(d, dm, -1, side + 1, words, 1)) < 0)
            goto end;
    }
    if (ref < 0 && nb_held)
        dr_vlog(d->log, DR_VLOG_WARNING, "%s: no time in it (no PES time, no dialog): its dialogs have none\n", name);
    for (int h = 0; h < nb_held; h++) {
        const HeldPayload *p = &held[h];
        int64_t time = p->pts == DR_NOPTS_VALUE || ref < 0 ? DR_NOPTS_VALUE :
                       (p->pts < ref ? p->pts + (1LL << 33) : p->pts) * DR_TICKS_PER_PTS;
        for (int i = 0; i < src->nb_tracks; i++) {
            if (src->feeds[i].side != side + 1 ||
                feed_key(&src->feeds[i], p->id & 0xffff, (uint32_t)p->id >> 16) < 0)
                continue;
            if ((ret = ff_disctl_title_side_payload(r->rip, i, p->data, p->size, time)) < 0)
                goto end;
        }
    }
    for (int i = 0; i < src->nb_tracks; i++)
        if (src->feeds[i].side == side + 1 && (ret = ff_disctl_title_side_end(r->rip, i)) < 0)
            goto end;
    ret = 0;
end:
    for (int h = 0; h < nb_held; h++)
        dr_free(held[h].data);
    dr_free(held);
    dr_clip_close(&dm);
    return ret;
}

int ff_disc_title_reader_open(DrDisc *d, const DiscTitleSource *src)
{
    DiscTitleReader *r;
    int64_t skip;
    int ret;

    if (!(r = d->reader = dr_mallocz(sizeof(*r))))
        return DR_ERR(ENOMEM);
    r->src = src;
    r->bs  = src->ts ? TS_UNIT : DISC_BLOCK;
    if (src->ts && (!(r->pair = dr_calloc(src->nb_tracks, sizeof(*r->pair))) ||
                    !(r->pair_pts = dr_calloc(src->nb_tracks, sizeof(*r->pair_pts)))))
        return DR_ERR(ENOMEM);
    /* filler clips at the ends left out; no "Chapter 00" */
    if ((ret = filler_trim(d, r, &skip)) < 0 ||
        (ret = src->chapter_clips ? position_plan(d, r) :
               ff_disctl_chapter_plan((void *)d->log, src->chapter_times, src->nb_chapters, skip, 0, &r->chapter_plan)) < 0)
        return ret;
    {
        char helper[4096];
        DRTitleConfig cfg = {
            .nb_tracks = src->nb_tracks, .tracks = src->tracks,
            .lpcm_hd = src->lpcm_header == 5,
            .marks = r->chapter_plan.marks, .nb_marks = r->chapter_plan.nb_marks,
            .cc_helper = d->opt_cc_helper && *d->opt_cc_helper ? d->opt_cc_helper :
                         ff_disctl_ccx_default(helper, sizeof(helper)),
            .open_marks = src->chapter_clips ? r->chapter_plan.marks : NULL,
            .chapter_rule = d->chapter_rule,
            .nb_segments = r->last_clip - r->first_clip + 1,
            .place = block_words, .place_opaque = d,
        };
        if ((ret = ff_disctl_title_open(&r->rip, (void *)d->log, &cfg)) < 0)
            return ret;
    }
    for (int i = r->first_clip; i <= r->last_clip; i++)
        r->blocks_total += src->clip_blocks(d, i);
    for (int k = 0; src->ts && k < src->nb_sides; k++)
        if ((ret = read_side(d, k)) < 0)
            return ret;
    return clip_start(d, r->first_clip);
}

/* Blu-ray: the track's key the payload has (0 / 1), -1 when the track
 * does not take it. */
static int feed_key(const DiscTsFeed *f, int pid, uint16_t key)
{
    if (f->pid != pid)
        return -1;
    if (!f->nb_keys || !f->key[0])
        return 0;
    for (int j = 0; j < f->nb_keys; j++)
        if (f->key[j] == key)
            return j;
    return -1;
}

/* Blu-ray, a track with two keys (a core frame and its extension): the
 * PES packets with a time alternate, the first key first, and an
 * extension has its core's time. What breaks this is reported (the
 * reference drops such pairs; here the bytes go on as they come). */
static void pair_check(DrDisc *d, int i, int j, int64_t pts, int64_t pos)
{
    DiscTitleReader *r = reader_of(d);
    const DiscTsFeed *f = &r->src->feeds[i];
    const char *what = NULL;

    if (pts == DR_NOPTS_VALUE)
        return;
    if (r->pair[i] == j)
        what = j ? "an extension follows an extension" : "a core frame without its extension";
    else if (j == 1 && r->pair[i] < 0)
        what = "an extension before the first core frame";
    else if (j == 1 && r->pair_pts[i] != pts)
        what = "an extension with another time than its core frame";
    if (what)
        dr_vlog(d->log, DR_VLOG_WARNING, "%s %s: track %d (PID 0x%04x, keys 0x%04x / 0x%04x): %s at byte %"PRId64
               " [untested on real discs]\n", r->src->clip_kind, r->src->clip_name(d, r->clip), i, f->pid,
               f->key[0], f->key[1], what, pos);
    r->pair[i]     = j;
    r->pair_pts[i] = pts;
}

/* Where block of the title's segment seg lies, in words: the file and the
 * byte offset there (when the format part tells them), and how far into the
 * title (or the segment) it is. */
static int block_words(void *opaque, int seg, int64_t block, char *buf, int size)
{
    DrDisc *d = opaque;
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    const char *unit = src->ts ? "unit" : "sector";
    int clip = r->first_clip + seg;
    char file[64], part[32] = "the title";
    int64_t off;

    if (clip > r->last_clip || block < 0)
        return DR_ERR(EINVAL);
    if (r->last_clip > r->first_clip)
        snprintf(part, sizeof(part), "segment %d", seg + 1);
    if (src->block_place && block <= UINT32_MAX &&
        src->block_place(d, clip, (uint32_t)block, file, sizeof(file), &off) >= 0)
        snprintf(buf, size, "in the %s at %s byte %"PRId64", byte %"PRId64" of %s", unit, file, off,
                 block * r->bs, part);
    else
        snprintf(buf, size, "in the %s at byte %"PRId64" of %s %s", unit, block * r->bs, src->clip_kind,
                 src->clip_name(d, clip));
    return 0;
}

/* A payload of the clip being read to every track it feeds (a DTS-HD
 * stream also feeds the DTS track taken out of it): its PES time with the
 * clip's wrap reference, in ticks. */
static int feed(DrDisc *d, const DrClipPacket *pkt)
{
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    int64_t time = DR_NOPTS_VALUE;
    /* the clip's block the payload is read from (blocks left out before it counted) */
    int64_t block = pkt->pos >= 0 ? (pkt->pos + (int64_t)r->skipped * r->bs) / r->bs : -1;
    int n = 0, ret;

    if (pkt->pts != DR_NOPTS_VALUE)
        time = (pkt->pts < r->pts_ref ? pkt->pts + (1LL << 33) : pkt->pts) * DR_TICKS_PER_PTS;
    /* video times around damaged ranges (program streams) */
    if (!src->ts && pkt->id == d->track_ids[0] && time != DR_NOPTS_VALUE) {
        for (; r->damage_wait < r->nb_damage; r->damage_wait++)
            r->damage[r->damage_wait].disc_after = time;
        r->vlast = r->vlast == DR_NOPTS_VALUE ? time : DR_MAX(r->vlast, time);
    }
    /* TS: every track of the title (a merged second layer has no stream of
     * its own); program streams: the streams */
    for (int i = 0; i < src->nb_tracks; i++) {
        if (src->ts) {
            int j = src->feeds[i].side ? -1 : feed_key(&src->feeds[i], pkt->id & 0xffff, (uint32_t)pkt->id >> 16);
            if (j < 0)
                continue;
            if (src->feeds[i].nb_keys == 2)
                pair_check(d, i, j, pkt->pts, pkt->pos);
        } else if (d->track_ids[i] != pkt->id)
            continue;
        /* the video from a chapter's position on (a position not reached in
         * its clip: from the next clip's first video) */
        while (!i && r->next_pos < r->nb_pos &&
               (r->pos_clip[r->next_pos] < r->clip ||
                (r->pos_clip[r->next_pos] == r->clip &&
                 r->pos_off[r->next_pos] <= pkt->pos + (int64_t)r->skipped * r->bs))) {
            if ((ret = ff_disctl_title_chapter_position(r->rip)) < 0)
                return ret;
            r->next_pos++;
        }
        if ((ret = ff_disctl_title_payload_at(r->rip, i, pkt->data, pkt->size, time, block)) < 0)
            return ret;
        n++;
    }
    if (!n) {
        int u = 0;
        dr_vlog(d->log, DR_VLOG_TRACE, "%s %s: packet of stream 0x%x (no track): left out\n",
               src->clip_kind, src->clip_name(d, r->clip), pkt->id);
        while (u < r->nb_unused && r->unused[u].id != pkt->id)
            u++;
        if (u == r->nb_unused && u < (int)DR_ARRAY_ELEMS(r->unused))
            r->unused[r->nb_unused++].id = pkt->id;
        if (u < r->nb_unused) {
            r->unused[u].packets++;
            r->unused[u].bytes += pkt->size;
        } else
            r->unused_other++;
    }
    return 0;
}

static int hold(DiscTitleReader *r, const DrClipPacket *pkt)
{
    uint8_t *copy;

    if (r->nb_held == r->held_cap) {
        int cap = r->held_cap ? 2 * r->held_cap : 64;
        HeldPayload *h = dr_realloc_array(r->held, cap, sizeof(*h));
        if (!h)
            return DR_ERR(ENOMEM);
        r->held     = h;
        r->held_cap = cap;
    }
    if (!(copy = dr_memdup(pkt->data, pkt->size ? pkt->size : 1)))
        return DR_ERR(ENOMEM);
    r->held[r->nb_held++] = (HeldPayload){ copy, pkt->size, pkt->id, pkt->pts, pkt->pos };
    return 0;
}

/* The clip just read: where the disc's structure starts it, on the
 * title timeline's clock for it (its wrap reference), kept for the check
 * at the title's end. */
static int start_keep(DrDisc *d)
{
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    int64_t at, *p;

    if (!src->clip_start)
        return 0;
    if (!(p = dr_realloc_array(r->clip_at, r->clip - r->first_clip + 1, sizeof(*p))))
        return DR_ERR(ENOMEM);
    r->clip_at = p;
    at = r->pts_ref < 0 ? DR_NOPTS_VALUE : src->clip_start(d, r->clip);
    p[r->clip - r->first_clip] = at == DR_NOPTS_VALUE ? at :
                                 (at < r->pts_ref ? at + (1LL << 33) : at) * DR_TICKS_PER_PTS;
    return 0;
}

/* Each clip's first picture against where the disc's structure starts it. */
static void start_check(DrDisc *d)
{
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    int checked = 0, off = 0;

    if (!src->clip_start || !r->clip_at)
        return;
    for (int k = r->first_clip; k <= r->last_clip; k++) {
        int64_t first, at = r->clip_at[k - r->first_clip], diff;
        if (at == DR_NOPTS_VALUE || ff_disctl_title_segment_start(r->rip, k - r->first_clip, &first) < 0) {
            dr_vlog(d->log, DR_VLOG_VERBOSE, "Title check: %s %s: %s or its first picture not known: its start is not "
                   "checked\n", src->clip_kind, src->clip_name(d, k), src->clip_start_name);
            continue;
        }
        diff = first - at;
        checked++;
        if (DR_ABS(diff) < DR_TICKS_PER_PTS) {
            dr_vlog(d->log, DR_VLOG_VERBOSE, "Title check: %s %s: its first picture is at %s: checked\n", src->clip_kind,
                   src->clip_name(d, k), src->clip_start_name);
            continue;
        }
        off++;
        dr_vlog(d->log, DR_VLOG_WARNING, "Title check: %s %s (%d of %d): its first picture is %.3f ms %s %s (the title "
               "%s there)\n", src->clip_kind, src->clip_name(d, k), k + 1, src->nb_clips,
               DR_ABS(diff) / (DR_TICKS_PER_SECOND / 1000.0), diff > 0 ? "after" : "before", src->clip_start_name,
               diff > 0 ? "leaves out what the disc plays before it" : "has pictures the disc starts later");
    }
    dr_vlog(d->log, off ? DR_VLOG_WARNING : DR_VLOG_VERBOSE, "Title check: %d of %d %s(s) start their first picture at %s%s\n",
           checked - off, checked, src->clip_kind, src->clip_start_name, off ? " (see the warnings above)" : "");
}

/* One PES packet of the title into the title timeline. The clip's PTS wrap
 * reference is the time of the first video packet that starts a sector in
 * its first 2000 sectors, less 300 s; packets before it wait for it. A
 * time below the reference has wrapped (+2^33). */
static int read_input(DrDisc *d)
{
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    DrClipPacket pkt;
    int64_t sector;
    int first, ret;

    ret = clip_err(dr_clip_read(r->ps, &pkt));
    /* Blu-ray: the stretches of the stream file left out so far (all of
     * them at its end) */
    if (src->ts) {
        char name[256];
        int err = gaps_said(d, r->ps, r->clip, 0, clip_words(d, name, sizeof(name)),
                            ret == DR_ERR_EOF && r->clip_done);
        if (err < 0)
            return err;
    }
    if (ret == DR_ERR_EOF && r->clip_done) {
        if (r->pts_ref < 0) {
            dr_vlog(d->log, DR_VLOG_ERROR, "%s %s: no video time in its first %d sectors: no time reference\n",
                   src->clip_kind, src->clip_name(d, r->clip), PTS_REF_SECTORS);
            return DR_ERR_INVALID;
        }
        if ((ret = start_keep(d)) < 0)
            return ret;
        if (r->clip + 1 <= r->last_clip)
            return clip_start(d, r->clip + 1);
        return ff_disctl_title_finish(r->rip);
    }
    if (ret < 0)
        return ret;
    if (src->ts)                /* the reference is known: every payload goes on */
        return feed(d, &pkt);
    sector = pkt.pos / DISC_BLOCK + r->skipped;
    first  = sector != r->last_sector;
    r->last_sector = sector;
    if (r->pts_ref < 0) {
        int64_t limit = DR_MIN(PTS_REF_SECTORS, (int64_t)r->nb_blocks);

        if (pkt.id == d->track_ids[0] && pkt.pts != DR_NOPTS_VALUE && first && sector < limit) {
            r->pts_ref = pkt.pts >= PTS_REF_BACK ? pkt.pts - PTS_REF_BACK : pkt.pts + (1LL << 33) - PTS_REF_BACK;
            dr_vlog(d->log, DR_VLOG_VERBOSE, "%s %s: first video time %"PRId64" in sector %"PRId64": wrap reference %"
                   PRId64"\n", src->clip_kind, src->clip_name(d, r->clip), pkt.pts, sector, r->pts_ref);
            for (int i = 0; i < r->nb_held; i++) {
                DrClipPacket h = { r->held[i].data, r->held[i].size, r->held[i].id, r->held[i].pts,
                                   r->held[i].pos, 0, DR_CODEC_NONE };
                if ((ret = feed(d, &h)) < 0)
                    break;
            }
            held_free(r);
            if (ret < 0)
                return ret;
        } else if (sector >= limit) {
            dr_vlog(d->log, DR_VLOG_ERROR, "%s %s: no video time in its first %"PRId64" sectors: no time reference\n",
                   src->clip_kind, src->clip_name(d, r->clip), limit);
            return DR_ERR_INVALID;
        } else {
            return hold(r, &pkt);
        }
    }
    return feed(d, &pkt);
}

/* The chapters of the read title, once the timeline finished. */
static int set_chapters(DrDisc *d)
{
    DiscTitleReader *r = reader_of(d);
    DRChapter *ch;
    int n, ret;

    r->chapters_set = 1;
    if ((ret = ff_disctl_title_chapters(r->rip, &r->chapter_plan, &ch, &n)) < 0)
        return ret;
    if (n && !(d->chapters = dr_calloc(n, sizeof(*d->chapters)))) {
        dr_free(ch);
        return DR_ERR_NOMEM;
    }
    for (int i = 0; i < n; i++) {
        const char *name = ch[i].record >= 0 && r->src->chapter_names && r->src->chapter_names[ch[i].record] &&
                           *r->src->chapter_names[ch[i].record] ? r->src->chapter_names[ch[i].record] : NULL;
        d->chapters[i] = (DrDiscChapter){ ch[i].start, ch[i].end, name };
    }
    d->nb_chapters = n;
    dr_free(ch);
    d->duration = ff_disctl_title_duration(r->rip);
    return 0;
}

/* A linear PCM track leaves as little-endian PCM: its stream takes the
 * format of the first audio frame header. */
static int lpcm_params(DrDisc *d, int k)
{
    DiscTitleReader *r = reader_of(d);
    DrDiscTrack *t = &d->out_tracks[k];
    const DRLpcm *p;
    int bits;

    if ((t->codec != DR_CODEC_PCM_DVD && t->codec != DR_CODEC_PCM_BLURAY) ||
        !(p = ff_disctl_title_lpcm(r->rip, k)))
        return 0;
    bits = 0;
    for (uint64_t m = p->chmask; m; m >>= 1)
        bits += m & 1;
    t->codec       = p->bits > 16 ? DR_CODEC_PCM_S24LE : DR_CODEC_PCM_S16LE;
    t->sample_rate = p->rate;
    t->bits        = p->bits;
    t->channels    = p->channels;
    t->layout      = p->chmask && bits == p->channels ? p->chmask : 0;
    return 0;
}

/* A text subtitle stream's header (extradata): its dialog style segment
 * (the Matroska codec private of S_HDMV/TEXTST), once read. */
static int textst_header(DrDisc *d, int k)
{
    DrDiscTrack *t = &d->out_tracks[k];
    const uint8_t *style;
    int size;

    if (t->codec != DR_CODEC_HDMV_TEXT_SUBTITLE || t->extradata ||
        !(style = ff_disctl_title_textst_style(reader_of(d)->rip, k, &size)))
        return 0;
    if (!(d->track_extradata[k] = dr_memdup(style, size)))
        return DR_ERR_NOMEM;
    t->extradata      = d->track_extradata[k];
    t->extradata_size = size;
    return 0;
}

/* A time on the title timeline as h:mm:ss.mmm (truncated). */
static const char *hms(char *buf, int size, int64_t t)
{
    int64_t ms = t < 0 ? 0 : t / (DR_TICKS_PER_SECOND / 1000);

    snprintf(buf, size, "%d:%02d:%02d.%03d", (int)(ms / 3600000), (int)(ms / 60000 % 60), (int)(ms / 1000 % 60),
             (int)(ms % 1000));
    return buf;
}

/* A formatted piece of a log line into buf. */
static const char *strf_s(char *buf, int size, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return buf;
}

/* Bytes as MB of 1024^2 bytes, one decimal, cut off (as the caller's sizes). */
static double mb(int64_t bytes)
{
    return (double)(bytes * 10 / 1048576) / 10;
}

/* Where a damaged range lies: the files holding its first and last block
 * and the byte offsets there (as the disc's reader named them, else from
 * src->block_place); 0 when not known. */
static int damage_where(DrDisc *d, const DamageRange *g, char *file0, char *file1, int size,
                        int64_t *off0, int64_t *off1)
{
    const DiscTitleSource *src = reader_of(d)->src;

    if (g->named.kind) {
        if (!g->named.file[0] || !g->named.last_file[0])
            return 0;
        snprintf(file0, size, "%s", g->named.file);
        snprintf(file1, size, "%s", g->named.last_file);
        *off0 = g->named.offset;
        *off1 = g->named.last_offset;
        return 1;
    }
    return src->block_place && src->block_place(d, g->clip, g->first, file0, size, off0) >= 0 &&
           src->block_place(d, g->clip, g->last, file1, size, off1) >= 0;
}

/* A damaged range in words to the caller's list (d->damage). */
static int add_damage(DrDisc *d, char *text)
{
    if (!text || dr_array_add_ptr(&d->damage, &d->nb_damage, text) < 0) {
        dr_free(text);
        return DR_ERR_NOMEM;
    }
    return 0;
}

/* The damaged ranges of the title, one line each (where they lie on the
 * disc, what they are, where the title passes them), for the caller too:
 * damaged_ranges, damaged_bytes and damage (each range in words). */
static int report_damage(DrDisc *d)
{
    DiscTitleReader *r = reader_of(d);
    const DiscTitleSource *src = r->src;
    const char *unit = r->bs == DISC_BLOCK ? "sector" : "unit";
    int64_t bytes = 0;
    int ret = 0;

    if (!r->nb_damage && !r->nb_gap_lines)
        return 0;
    for (int i = 0; i < r->nb_damage; i++) {
        const DamageRange *g = &r->damage[i];
        int64_t n = (int64_t)g->last - g->first + 1, off0, off1, at;
        char file0[64] = "", file1[64] = "", where[192], when[96] = "", a[32];
        int level = i < DAMAGE_REPORTS ? DR_VLOG_WARNING : DR_VLOG_VERBOSE;

        bytes += n * r->bs;
        /* where: the files holding its first and last block */
        if (damage_where(d, g, file0, file1, sizeof(file0), &off0, &off1)) {
            if (!strcmp(file0, file1))
                snprintf(where, sizeof(where), "%s %.1f-%.1f MB", file0, mb(off0),
                         mb(off1 + r->bs));
            else
                snprintf(where, sizeof(where), "%s %.1f MB to %s %.1f MB", file0, mb(off0), file1,
                         mb(off1 + r->bs));
        } else {
            off0 = off1 = -1;
            snprintf(where, sizeof(where), "%s %s blocks %"PRIu32"-%"PRIu32, src->clip_kind,
                     src->clip_name(d, g->clip), g->first, g->last);
        }
        /* when: where the title's pictures before it end; how far the
         * disc's video times jump over it (about the video missing) */
        if (g->place >= 0 && ff_disctl_title_place_time(r->rip, g->place, &at) >= 0) {
            int64_t jump = g->disc_before != DR_NOPTS_VALUE && g->disc_after != DR_NOPTS_VALUE ?
                           g->disc_after - g->disc_before : 0;
            snprintf(when, sizeof(when), "; at %s of the title%s", hms(a, sizeof(a), at),
                     jump >= DR_TICKS_PER_SECOND / 10 ? strf_s((char[40]){ 0 }, 40, ", about %.1f s of video",
                                                             jump / (double)DR_TICKS_PER_SECOND) : "");
        }
        dr_vlog(d->log, level, "Damaged: %s (%"PRId64" %s%s, %.1f MB): %s, left out%s\n", where, n, unit,
               n == 1 ? "" : "s", mb(n * r->bs), g->zeros ? "every byte is zero" : "no MPEG data",
               when);
        if (off0 >= 0)
            dr_vlog(d->log, DR_VLOG_VERBOSE, "Damaged: %s %s blocks %"PRIu32"-%"PRIu32" = %s byte %"PRId64" to %s byte %"
                   PRId64"\n", src->clip_kind, src->clip_name(d, g->clip), g->first, g->last, file0, off0, file1,
                   off1 + r->bs - 1);
        if (ret >= 0)
            ret = add_damage(d, dr_asprintf_str("%s (%"PRId64" %s%s, %.1f MB): %s%s", where, n, unit, n == 1 ? "" : "s",
                                                mb(n * r->bs), g->zeros ? "every byte is zero" : "no MPEG data", when));
    }
    if (r->nb_damage > DAMAGE_REPORTS)
        dr_vlog(d->log, DR_VLOG_WARNING, "Damaged: %d more range(s), in the detail lines\n", r->nb_damage - DAMAGE_REPORTS);
    /* Blu-ray: the stretches of stream files left out (each said when read) */
    if (r->nb_gap_lines) {
        dr_vlog(d->log, DR_VLOG_WARNING, "Damaged: %d stretch%s of the stream files left out, %.3f MB in all (see the "
               "warnings above)\n", r->nb_gap_lines, r->nb_gap_lines == 1 ? "" : "es", mb(r->gap_bytes));
        for (int i = 0; i < r->nb_gap_lines && ret >= 0; i++)
            ret = add_damage(d, dr_strdup(r->gap_lines[i]));
        bytes += r->gap_bytes;
    }
    if (ret >= 0) {
        d->damaged_ranges = r->nb_damage + r->nb_gap_lines;
        d->damaged_bytes  = bytes;
    }
    return ret;
}

/* What the title gives the caller at its end: chapters, the untested
 * features met, the caption data taken out of the video, each track's
 * result. */
static int title_end(DrDisc *d)
{
    DiscTitleReader *r = d->reader;
    /* untested paths of the title timeline, and tracks the format part has
     * not met on a real disc yet */
    int64_t review = ff_disctl_title_review(r->rip) + d->untested;
    int ret;

    if ((ret = set_chapters(d)) < 0 || (ret = report_damage(d)) < 0)
        return ret;
    /* the disc's own check of what was read (each damaged unit was logged) */
    if (r->src->integrity)
        r->src->integrity(d);
    /* what was read and not ripped */
    {
        DrStr bp;
        dr_str_init(&bp);
        for (int u = 0; u < r->nb_unused; u++)
            dr_str_catf(&bp, "%s0x%x %"PRId64" (%"PRId64" bytes)", u ? ", " : "", r->unused[u].id,
                       r->unused[u].packets, r->unused[u].bytes);
        if (r->unused_other)
            dr_str_catf(&bp, ", %"PRId64" of further streams", r->unused_other);
        dr_vlog(d->log, r->unusable ? DR_VLOG_INFO : DR_VLOG_VERBOSE, "Title check: read %"PRId64" blocks of %s(s) %d-%d (%d of %d; %"PRId64
               " not usable, left out); packets of streams without a track, left out: %s\n", r->blocks_done,
               r->src->clip_kind, r->first_clip + 1, r->last_clip + 1, r->last_clip - r->first_clip + 1,
               r->src->nb_clips, r->unusable, r->nb_unused || r->unused_other ? bp.str : "none");
        dr_str_free(&bp);
        start_check(d);
        if (!r->src->ts) {
            const MpegPsDiscStats *p = &r->ps_read;
            ps_read_add(r);
            dr_vlog(d->log, DR_VLOG_VERBOSE, "Title check: sectors: %"PRId64" read, %"PRId64" empty; left out: %"PRId64
                   " navigation packets (%"PRId64" bytes), %"PRId64" system headers (%"PRId64" bytes), %"PRId64
                   " padding packets (%"PRId64" bytes), %"PRId64" other packets of no stream (%"PRId64" bytes)\n",
                   p->sectors, p->empty, p->nav, p->nav_bytes, p->system, p->system_bytes, p->padding,
                   p->padding_bytes, p->other, p->other_bytes);
            if (p->bad || p->rest_dropped || p->discarded)
                dr_vlog(d->log, DR_VLOG_WARNING, "Title check: sectors: %"PRId64" left out (a short header), %"PRId64
                       " with their rest left out (%"PRId64" bytes), %"PRId64" packets of discarded streams (%"
                       PRId64" bytes)\n", p->bad, p->rest_dropped, p->rest_bytes, p->discarded,
                       p->discarded_bytes);
        }
    }
    /* features met that no real disc has tested: said, and given to the
     * caller (the log's "[untested on real discs]" lines tell which) */
    if (review) {
        dr_vlog(d->log, DR_VLOG_WARNING, "%s: %"PRId64" untested feature(s) met in this title: see the log's "
               "\"[untested on real discs]\" lines\n", d->fmt->name, review);
        d->untested_total = review;
    }
    /* line-21 caption data the timeline took out of the video and did not
     * keep as a track: reported */
    {
        int64_t blocks, text;
        ff_disctl_title_cc_dropped(r->rip, &blocks, &text);
        if (blocks && d->nb_out_tracks) {
            d->out_tracks[0].cc_blocks = blocks;
            d->out_tracks[0].cc_text   = text;
        }
    }
    /* each track's result for the caller: frames, warnings, start delay */
    for (int i = 0; i < d->nb_out_tracks; i++) {
        DrDiscTrack *t = &d->out_tracks[i];
        int64_t frames, warnings, delay, lost;
        if (ff_disctl_title_track_result(r->rip, i, &frames, &warnings, &delay) < 0)
            continue;
        t->frames      = frames;
        t->warnings    = warnings;
        t->delay_ticks = delay;
        ff_disctl_title_track_holes(r->rip, i, &t->holes, &t->hole_ticks);
        if (ff_disctl_title_cc_result(r->rip, i, &frames, &warnings, &lost) >= 0) {
            t->has_cc    = 1;
            t->cc_blocks = frames;
            t->cues      = warnings;
            t->cc_lost   = lost;
        }
        if (ff_disctl_title_spu_result(r->rip, i, &frames, &warnings) >= 0) {
            t->has_spu = 1;
            t->no_stop = frames;
            t->outside = warnings;
        }
        if (t->codec == DR_CODEC_HDMV_PGS_SUBTITLE &&
            ff_disctl_title_forced_objects(r->rip, i, &frames) >= 0) {
            t->has_forced_objects = 1;
            t->forced_objects     = frames;
        }
    }
    (void)ret;
    return 0;
}

/* The title timeline's next output frame (times in ticks of
 * 1/1,080,000,000 s); empty markers carry no bytes and are not given out. */
int ff_disc_title_reader_frame(DrDisc *d, DrDiscFrame *out)
{
    DiscTitleReader *r = reader_of(d);
    int ret;

    for (;;) {
        DRFrame f;
        int k;

        ret = ff_disctl_title_frame(r->rip, &k, &f);
        if (!ret) {
            if (!f.size) {
                ff_disctl_frame_unref(&f);
                continue;
            }
            if ((ret = lpcm_params(d, k)) < 0 || (ret = textst_header(d, k)) < 0) {
                ff_disctl_frame_unref(&f);
                return ret;
            }
            /* the frame's reference moves to the caller */
            out->buf      = f.buf;
            out->data     = f.data;
            out->size     = f.size;
            out->track    = k;
            out->time     = f.time;
            out->duration = f.dur;
            out->key      = !!(f.flags & DR_F_KEY);
            out->discard  = !!(f.flags & DR_F_DISCARD);
            return 0;
        }
        if (ret == DR_ERR_EOF) {
            if (!r->chapters_set && (ret = title_end(d)) < 0)
                return ret;
            return DR_ERR_EOF;
        }
        if (ret != DR_ERR(EAGAIN))
            return ret;
        if ((ret = read_input(d)) < 0)
            return ret;
    }
}
