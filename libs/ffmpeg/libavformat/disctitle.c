/*
 * The disc layer's title reader: reads the opened title through FFmpeg's
 * line into the title timeline and gives its frames out as packets. The
 * format part says what the title is (DiscTitleSource: its clips, how a
 * block of one is read, its tracks, chapter marks and the format's own
 * values); everything after the blocks is the same for every format: each
 * clip through FFmpeg's demuxer as a disc reader reads it (raw PES
 * payloads: DVD / HD DVD sectors through mpeg.c, Blu-ray units of source
 * packets through mpegts.c, both with option disc), the clip's time
 * reference, the payloads to their tracks, the title timeline, its frames,
 * chapters and track results.
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

#include <string.h>

#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "disc.h"
#include "disctl.h"
#include "internal.h"
#include "mpegts.h"

#define DISC_BLOCK      2048
#define TS_UNIT         6144              /* Blu-ray: 32 source packets of 192 bytes */
#define TS_PACKET       192
#define PTS_REF_SECTORS 2000              /* the PTS wrap reference is looked for in the first sectors */
#define PTS_REF_BACK    27000000LL        /* and lies 300 s (90 kHz) before the first video time */
#define TAIL_BYTES      (64LL << 20)      /* frames from a clip's last 64 MiB may be joined seamlessly */

/* ---- a clip through FFmpeg's demuxer ---- */

static int demux_open(DiscDemux *dm, AVFormatContext *parent, const AVInputFormat *fmt, int block,
                      AVDictionary **opts, int (*read)(void *opaque, uint8_t *buf, int size), void *opaque)
{
    uint8_t *buf;
    int ret;

    memset(dm, 0, sizeof(*dm));
    if (!(buf = av_malloc(block)))
        return AVERROR(ENOMEM);
    if (!(dm->pb = avio_alloc_context(buf, block, 0, opaque, read, NULL, NULL))) {
        av_free(buf);
        return AVERROR(ENOMEM);
    }
    dm->pb->seekable = 0;
    if (!(dm->ctx = avformat_alloc_context()))
        return AVERROR(ENOMEM);
    if (parent && (ret = ff_copy_whiteblacklists(dm->ctx, parent)) < 0) {
        avformat_free_context(dm->ctx);
        dm->ctx = NULL;
        return ret;
    }
    /* raw PES payloads with their own times: no parsing, no filled-in or
     * corrected timestamps (the title timeline times every unit) */
    dm->ctx->flags = AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
    dm->ctx->correct_ts_overflow = 0;
    dm->ctx->ctx_flags |= AVFMTCTX_UNSEEKABLE;
    dm->ctx->probesize = 0;
    dm->ctx->max_analyze_duration = 0;
    if (parent)
        dm->ctx->interrupt_callback = parent->interrupt_callback;
    dm->ctx->pb = dm->pb;
    dm->ctx->io_open = NULL;
    return avformat_open_input(&dm->ctx, "", fmt, opts);
}

int ff_disc_ps_open(DiscDemux *dm, AVFormatContext *parent, int lpcm_header,
                    int (*read)(void *opaque, uint8_t *buf, int size), void *opaque)
{
    extern const FFInputFormat ff_mpegps_demuxer;
    AVDictionary *opts = NULL;
    int ret;

    /* disc sectors; the format's LPCM audio frame header size */
    if ((ret = av_dict_set(&opts, "disc", "1", 0)) < 0 ||
        (ret = av_dict_set_int(&opts, "lpcm_header", lpcm_header, 0)) < 0) {
        av_dict_free(&opts);
        return ret;
    }
    ret = demux_open(dm, parent, &ff_mpegps_demuxer.p, DISC_BLOCK, &opts, read, opaque);
    av_dict_free(&opts);
    return ret;
}

int ff_disc_ts_open(DiscDemux *dm, AVFormatContext *parent, const int *pids, int nb_pids,
                    int (*read)(void *opaque, uint8_t *buf, int size), void *opaque)
{
    extern const FFInputFormat ff_mpegts_demuxer;
    AVDictionary *opts = NULL;
    AVBPrint bp;
    int ret;

    /* source packets as the disc reader reads them; the PIDs read */
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (int i = 0; i < nb_pids; i++)
        av_bprintf(&bp, "%s%d", i ? "," : "", pids[i]);
    if (!av_bprint_is_complete(&bp))
        ret = AVERROR(ENOMEM);
    else if ((ret = av_dict_set(&opts, "disc", "1", 0)) >= 0 && nb_pids)
        ret = av_dict_set(&opts, "disc_pids", bp.str, 0);
    av_bprint_finalize(&bp, NULL);
    if (ret >= 0)
        ret = demux_open(dm, parent, &ff_mpegts_demuxer.p, TS_UNIT, &opts, read, opaque);
    av_dict_free(&opts);
    return ret;
}

void ff_disc_demux_close(DiscDemux *dm)
{
    avformat_close_input(&dm->ctx);
    if (dm->pb)
        av_freep(&dm->pb->buffer);
    avio_context_free(&dm->pb);
}

/* ---- the title reader ---- */

struct DiscTitleReader {
    const DiscTitleSource *src;

    /* the clip being read */
    int            clip;
    uint32_t       block;           /**< next block of that clip */
    uint32_t       nb_blocks;       /**< blocks of that clip */
    int            bs;              /**< bytes per block: 2048 (sectors) or 6144 (Blu-ray units) */
    int            clip_done;       /**< the clip's last block was given to the demuxer */
    uint32_t       skipped;         /**< unusable blocks of that clip left out so far */
    uint64_t       tail_start;      /**< byte of that clip where its last 64 MiB begin */
    int64_t        pts_ref;         /**< that clip's PTS wrap reference (90 kHz), -1 while not known */
    int64_t        last_sector;     /**< sector of the last PES packet read */
    AVPacket     **held;            /**< packets read before the reference was known */
    int            nb_held, held_cap;
    DiscDemux      ps;              /**< the clip through FFmpeg's demuxer (MPEG-PS / MPEG-TS) */
    int           *pair;            /**< ts: per track with two keys, the key of its last PES with a time
                                         (-1 none yet) */
    int64_t       *pair_pts;        /**< and that PES packet's time */

    /* the title timeline */
    DRTitle       *rip;
    DRChapterPlan  chapter_plan;
    int64_t        blocks_total;    /**< blocks of all the title's clips */
    int64_t        blocks_done;     /**< blocks read so far */
    int            chapters_set;
    AVPacket      *in;
};

static DiscTitleReader *reader_of(AVFormatContext *s)
{
    return ((DiscContext *)s->priv_data)->reader;
}

/* The next block of the clip being read; EOF at the clip's end. */
static int ps_read(void *opaque, uint8_t *buf, int buf_size)
{
    AVFormatContext *s = opaque;
    DiscTitleReader *r = reader_of(s);
    const DiscTitleSource *src = r->src;
    int ret;

    if (buf_size < r->bs)
        return AVERROR(EINVAL);
    while (r->block < r->nb_blocks) {
        uint32_t b = r->block++;

        ret = src->read_block(s, r->clip, b, buf);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "%s %s: block %"PRIu32" of %"PRIu32" cannot be read (%s): "
                   "the rest of the %s is not read\n", src->clip_kind, src->clip_name(s, r->clip), b,
                   r->nb_blocks, av_err2str(ret), src->clip_kind);
            r->block = r->nb_blocks;
            break;
        }
        r->blocks_done++;
        if (r->blocks_total)
            ((DiscContext *)s->priv_data)->progress = r->blocks_done * 10000 / r->blocks_total;
        if (!ret) {
            av_log(s, AV_LOG_WARNING, "%s %s: block %"PRIu32" is not usable: left out\n", src->clip_kind,
                   src->clip_name(s, r->clip), b);
            r->skipped++;
            continue;
        }
        return r->bs;
    }
    r->clip_done = 1;
    return AVERROR_EOF;
}

static void held_free(DiscTitleReader *r)
{
    for (int i = 0; i < r->nb_held; i++)
        av_packet_free(&r->held[i]);
    r->nb_held = 0;
}

/* A Blu-ray clip's PTS wrap reference, before the clip is read (the
 * video's port finds it; every other track is linked to the video's):
 * the first TS packet of the video's PID that starts a PES packet of the
 * video's key with a PTS, in the clip's first 2000 units; the reference
 * is that PTS less 300 s. A PES header there that cannot be read ends the
 * search. */
static int ts_reference(AVFormatContext *s)
{
    DiscTitleReader *r = reader_of(s);
    const DiscTitleSource *src = r->src;
    const DiscTsFeed *v = &src->feeds[0];
    uint32_t limit = FFMIN(PTS_REF_SECTORS, r->nb_blocks);
    uint8_t *buf;
    int ret = 0;

    if (!(buf = av_malloc(TS_UNIT)))
        return AVERROR(ENOMEM);
    for (uint32_t u = 0; u < limit; u++) {
        if ((ret = src->read_block(s, r->clip, u, buf)) < 0) {
            av_log(s, AV_LOG_ERROR, "%s %s: unit %"PRIu32" cannot be read (%s): no time reference\n",
                   src->clip_kind, src->clip_name(s, r->clip), u, av_err2str(ret));
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
                av_log(s, AV_LOG_ERROR, "%s %s: a PES header of the video (PID 0x%04x) in unit %"PRIu32" cannot be "
                       "read: no time reference\n", src->clip_kind, src->clip_name(s, r->clip), pid, u);
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
            if ((v->key[0] && key != v->key[0]) || pts == AV_NOPTS_VALUE)
                continue;
            r->pts_ref = pts >= PTS_REF_BACK ? pts - PTS_REF_BACK : pts + (1LL << 33) - PTS_REF_BACK;
            av_log(s, AV_LOG_VERBOSE, "%s %s: first video time %"PRId64" in unit %"PRIu32": wrap reference %"
                   PRId64"\n", src->clip_kind, src->clip_name(s, r->clip), pts, u, r->pts_ref);
            ret = 0;
            goto end;
        }
    }
    av_log(s, AV_LOG_ERROR, "%s %s: no video time in its first %"PRIu32" units: no time reference\n",
           src->clip_kind, src->clip_name(s, r->clip), limit);
    ret = AVERROR_INVALIDDATA;
end:
    av_free(buf);
    return ret;
}

/* Start reading clip k of the title (a segment of the title timeline): its
 * blocks, where its last 64 MiB begin, its PTS wrap reference (Blu-ray:
 * found before the clip is read; else still to be found). */
static int clip_start(AVFormatContext *s, int k)
{
    DiscTitleReader *r = reader_of(s);
    const DiscTitleSource *src = r->src;
    uint64_t size;
    int ret;

    r->clip        = k;
    r->block       = 0;
    r->nb_blocks   = src->clip_blocks(s, k);
    r->clip_done   = 0;
    r->skipped     = 0;
    size           = (uint64_t)r->nb_blocks * r->bs;
    r->tail_start  = size >= TAIL_BYTES ? size - TAIL_BYTES : 0;
    r->pts_ref     = -1;
    r->last_sector = -1;
    held_free(r);
    for (int i = 0; src->ts && i < src->nb_tracks; i++)
        r->pair[i] = -1;
    av_log(s, AV_LOG_VERBOSE, "Reading %s %s (%d of %d): %"PRIu32" %s, the last 64 MiB from byte %"PRIu64"\n",
           src->clip_kind, src->clip_name(s, k), k + 1, src->nb_clips, r->nb_blocks, src->ts ? "units" : "blocks",
           r->tail_start);
    ff_disc_demux_close(&r->ps);
    if (src->ts) {
        if ((ret = ts_reference(s)) < 0 ||
            (ret = ff_disc_ts_open(&r->ps, s, src->pids, src->nb_pids, ps_read, s)) < 0)
            return ret;
    } else if ((ret = ff_disc_ps_open(&r->ps, s, src->lpcm_header, ps_read, s)) < 0)
        return ret;
    return ff_disctl_title_segment(r->rip);
}

void ff_disc_title_reader_close(DiscTitleReader **reader)
{
    DiscTitleReader *r = *reader;

    if (!r)
        return;
    ff_disc_demux_close(&r->ps);
    held_free(r);
    av_freep(&r->held);
    av_freep(&r->pair);
    av_freep(&r->pair_pts);
    av_packet_free(&r->in);
    ff_disctl_title_close(&r->rip);
    ff_disctl_chapter_plan_free(&r->chapter_plan);
    av_freep(reader);
}

int ff_disc_title_reader_open(AVFormatContext *s, const DiscTitleSource *src)
{
    DiscContext *d = s->priv_data;
    DiscTitleReader *r;
    int ret;

    if (!(r = d->reader = av_mallocz(sizeof(*r))))
        return AVERROR(ENOMEM);
    r->src = src;
    r->bs  = src->ts ? TS_UNIT : DISC_BLOCK;
    if (src->ts && (!(r->pair = av_calloc(src->nb_tracks, sizeof(*r->pair))) ||
                    !(r->pair_pts = av_calloc(src->nb_tracks, sizeof(*r->pair_pts)))))
        return AVERROR(ENOMEM);
    /* no leading clip is left out yet (skip 0); no "Chapter 00" */
    if ((ret = ff_disctl_chapter_plan(s, src->chapter_times, src->nb_chapters, 0, 0, &r->chapter_plan)) < 0)
        return ret;
    {
        DRTitleConfig cfg = {
            .nb_tracks = src->nb_tracks, .tracks = src->tracks, .tolerance = src->tolerance,
            .lpcm_hd = src->lpcm_header == 5,
            .marks = r->chapter_plan.marks, .nb_marks = r->chapter_plan.nb_marks,
        };
        if ((ret = ff_disctl_title_open(&r->rip, s, &cfg)) < 0)
            return ret;
    }
    if (!(r->in = av_packet_alloc()))
        return AVERROR(ENOMEM);
    for (int i = 0; i < src->nb_clips; i++)
        r->blocks_total += src->clip_blocks(s, i);
    return clip_start(s, 0);
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
static void pair_check(AVFormatContext *s, int i, int j, int64_t pts, const AVPacket *pkt)
{
    DiscTitleReader *r = reader_of(s);
    const DiscTsFeed *f = &r->src->feeds[i];
    const char *what = NULL;

    if (pts == AV_NOPTS_VALUE)
        return;
    if (r->pair[i] == j)
        what = j ? "an extension follows an extension" : "a core frame without its extension";
    else if (j == 1 && r->pair[i] < 0)
        what = "an extension before the first core frame";
    else if (j == 1 && r->pair_pts[i] != pts)
        what = "an extension with another time than its core frame";
    if (what)
        av_log(s, AV_LOG_WARNING, "%s %s: track %d (PID 0x%04x, keys 0x%04x / 0x%04x): %s at byte %"PRId64
               " [untested on real discs]\n", r->src->clip_kind, r->src->clip_name(s, r->clip), i, f->pid,
               f->key[0], f->key[1], what, pkt->pos);
    r->pair[i]     = j;
    r->pair_pts[i] = pts;
}

/* A payload of the clip being read to every track it feeds (a DTS-HD
 * stream also feeds the DTS track taken out of it): its PES time with the
 * clip's wrap reference, in ticks; whether it lies in the clip's last 64 MiB. */
static int feed(AVFormatContext *s, const AVPacket *pkt)
{
    DiscTitleReader *r = reader_of(s);
    const DiscTitleSource *src = r->src;
    const AVStream *sub = r->ps.ctx->streams[pkt->stream_index];
    int64_t time = AV_NOPTS_VALUE;
    int tail = (uint64_t)pkt->pos + (uint64_t)r->skipped * r->bs >= r->tail_start, n = 0, ret;

    if (pkt->pts != AV_NOPTS_VALUE)
        time = (pkt->pts < r->pts_ref ? pkt->pts + (1LL << 33) : pkt->pts) * DR_TICKS_PER_PTS;
    for (int i = 0; i < s->nb_streams; i++) {
        if (src->ts) {
            int j = feed_key(&src->feeds[i], sub->id & 0xffff, (uint32_t)sub->id >> 16);
            if (j < 0)
                continue;
            if (src->feeds[i].nb_keys == 2)
                pair_check(s, i, j, pkt->pts, pkt);
        } else if (s->streams[i]->id != sub->id)
            continue;
        if ((ret = ff_disctl_title_payload(r->rip, i, pkt->data, pkt->size, time, tail)) < 0)
            return ret;
        n++;
    }
    if (!n)
        av_log(s, AV_LOG_TRACE, "%s %s: packet of stream 0x%x (no track): left out\n",
               src->clip_kind, src->clip_name(s, r->clip), sub->id);
    return 0;
}

static int hold(DiscTitleReader *r, AVPacket *pkt)
{
    if (r->nb_held == r->held_cap) {
        int cap = r->held_cap ? 2 * r->held_cap : 64;
        AVPacket **h = av_realloc_array(r->held, cap, sizeof(*h));
        if (!h)
            return AVERROR(ENOMEM);
        r->held     = h;
        r->held_cap = cap;
    }
    if (!(r->held[r->nb_held] = av_packet_alloc()))
        return AVERROR(ENOMEM);
    av_packet_move_ref(r->held[r->nb_held++], pkt);
    return 0;
}

/* One PES packet of the title into the title timeline. The clip's PTS wrap
 * reference is the time of the first video packet that starts a sector in
 * its first 2000 sectors, less 300 s; packets before it wait for it. A
 * time below the reference has wrapped (+2^33). */
static int read_input(AVFormatContext *s)
{
    DiscTitleReader *r = reader_of(s);
    const DiscTitleSource *src = r->src;
    AVPacket *pkt = r->in;
    const AVStream *sub;
    int64_t sector;
    int first, ret;

    ret = av_read_frame(r->ps.ctx, pkt);
    if (ret == AVERROR_EOF && r->clip_done) {
        if (r->pts_ref < 0) {
            av_log(s, AV_LOG_ERROR, "%s %s: no video time in its first %d sectors: no time reference\n",
                   src->clip_kind, src->clip_name(s, r->clip), PTS_REF_SECTORS);
            return AVERROR_INVALIDDATA;
        }
        if (r->clip + 1 < src->nb_clips)
            return clip_start(s, r->clip + 1);
        return ff_disctl_title_finish(r->rip);
    }
    if (ret < 0)
        return ret;
    if (src->ts) {              /* the reference is known: every payload goes on */
        ret = feed(s, pkt);
        av_packet_unref(pkt);
        return ret;
    }
    sub    = r->ps.ctx->streams[pkt->stream_index];
    sector = pkt->pos / DISC_BLOCK + r->skipped;
    first  = sector != r->last_sector;
    r->last_sector = sector;
    if (r->pts_ref < 0) {
        int64_t limit = FFMIN(PTS_REF_SECTORS, (int64_t)r->nb_blocks);

        if (sub->id == s->streams[0]->id && pkt->pts != AV_NOPTS_VALUE && first && sector < limit) {
            r->pts_ref = pkt->pts >= PTS_REF_BACK ? pkt->pts - PTS_REF_BACK : pkt->pts + (1LL << 33) - PTS_REF_BACK;
            av_log(s, AV_LOG_VERBOSE, "%s %s: first video time %"PRId64" in sector %"PRId64": wrap reference %"
                   PRId64"\n", src->clip_kind, src->clip_name(s, r->clip), pkt->pts, sector, r->pts_ref);
            for (int i = 0; i < r->nb_held; i++)
                if ((ret = feed(s, r->held[i])) < 0)
                    break;
            held_free(r);
            if (ret < 0) {
                av_packet_unref(pkt);
                return ret;
            }
        } else if (sector >= limit) {
            av_log(s, AV_LOG_ERROR, "%s %s: no video time in its first %"PRId64" sectors: no time reference\n",
                   src->clip_kind, src->clip_name(s, r->clip), limit);
            av_packet_unref(pkt);
            return AVERROR_INVALIDDATA;
        } else {
            return hold(r, pkt);
        }
    }
    ret = feed(s, pkt);
    av_packet_unref(pkt);
    return ret;
}

/* The chapters of the read title, once the timeline finished. */
static int set_chapters(AVFormatContext *s)
{
    DiscTitleReader *r = reader_of(s);
    DRChapter *ch;
    int n, ret;

    r->chapters_set = 1;
    if ((ret = ff_disctl_title_chapters(r->rip, &r->chapter_plan, &ch, &n)) < 0)
        return ret;
    for (int i = 0; i < n; i++) {
        const char *name = ch[i].record >= 0 && r->src->chapter_names && r->src->chapter_names[ch[i].record] &&
                           *r->src->chapter_names[ch[i].record] ? r->src->chapter_names[ch[i].record] : NULL;
        if (!avpriv_new_chapter(s, i, (AVRational){ 1, DR_TICKS_PER_SECOND }, ch[i].start, ch[i].end, name)) {
            av_free(ch);
            return AVERROR(ENOMEM);
        }
    }
    av_free(ch);
    s->duration = av_rescale(ff_disctl_title_duration(r->rip), AV_TIME_BASE, DR_TICKS_PER_SECOND);
    return 0;
}

/* A linear PCM track leaves as little-endian PCM: its stream takes the
 * format of the first audio frame header. */
static int lpcm_params(AVFormatContext *s, int k)
{
    DiscTitleReader *r = reader_of(s);
    AVCodecParameters *par = s->streams[k]->codecpar;
    const DRLpcm *p;

    if (par->codec_id != AV_CODEC_ID_PCM_DVD || !(p = ff_disctl_title_lpcm(r->rip, k)))
        return 0;
    par->codec_id              = p->bits > 16 ? AV_CODEC_ID_PCM_S24LE : AV_CODEC_ID_PCM_S16LE;
    par->sample_rate           = p->rate;
    par->bits_per_coded_sample = p->bits;
    av_channel_layout_uninit(&par->ch_layout);
    if (p->chmask && av_popcount64(p->chmask) == p->channels)
        return av_channel_layout_from_mask(&par->ch_layout, p->chmask);
    av_channel_layout_default(&par->ch_layout, p->channels);
    return 0;
}

/* What the title gives the caller at its end: chapters, the untested
 * features met, the caption data taken out of the video, each track's
 * result. */
static int title_end(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    DiscTitleReader *r = d->reader;
    int64_t review = ff_disctl_title_review(r->rip);
    int ret;

    if ((ret = set_chapters(s)) < 0)
        return ret;
    /* features met that no real disc has tested: the caller reports the job
     * as failed so that its log is looked at */
    if (review) {
        av_log(s, AV_LOG_ERROR, "%s: %"PRId64" untested feature(s) met in this title: "
               "check the log before using the output\n", d->fmt->name, review);
        if ((ret = av_dict_set_int(&s->metadata, "untested", review, 0)) < 0)
            return ret;
    }
    /* line-21 caption data the timeline took out of the video and did not
     * keep as a track: reported */
    {
        int64_t blocks, text;
        ff_disctl_title_cc_dropped(r->rip, &blocks, &text);
        if (blocks && ((ret = av_dict_set_int(&s->streams[0]->metadata, "cc_blocks", blocks, 0)) < 0 ||
                       (ret = av_dict_set_int(&s->streams[0]->metadata, "cc_text", text, 0)) < 0))
            return ret;
    }
    /* each track's result for the caller: frames, warnings, start delay (ms) */
    for (int i = 0; i < s->nb_streams; i++) {
        int64_t frames, warnings, delay;
        AVDictionary **m = &s->streams[i]->metadata;
        if (ff_disctl_title_track_result(r->rip, i, &frames, &warnings, &delay) < 0)
            continue;
        if ((ret = av_dict_set_int(m, "frames", frames, 0)) < 0 ||
            (ret = av_dict_set_int(m, "warnings", warnings, 0)) < 0 ||
            (ret = av_dict_set_int(m, "delay_us", delay / (DR_TICKS_PER_SECOND / 1000000), 0)) < 0)
            return ret;
        if (ff_disctl_title_spu_result(r->rip, i, &frames, &warnings) >= 0 &&
            ((ret = av_dict_set_int(m, "no_stop", frames, 0)) < 0 ||
             (ret = av_dict_set_int(m, "outside", warnings, 0)) < 0))
            return ret;
    }
    return 0;
}

/* The title timeline's next output frame as a packet (time base
 * 1/1,080,000,000 s); empty markers carry no bytes and are not given out. */
int ff_disc_title_reader_packet(AVFormatContext *s, AVPacket *pkt)
{
    DiscTitleReader *r = reader_of(s);
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
            if ((ret = lpcm_params(s, k)) < 0) {
                ff_disctl_frame_unref(&f);
                return ret;
            }
            pkt->buf          = f.buf;
            pkt->data         = f.data;
            pkt->size         = f.size;
            pkt->pts          = f.time;
            pkt->dts          = AV_NOPTS_VALUE;
            pkt->duration     = f.dur;
            pkt->pos          = f.pos;
            pkt->stream_index = k;
            if (f.flags & DR_F_KEY)
                pkt->flags |= AV_PKT_FLAG_KEY;
            if (f.flags & DR_F_DISCARD)
                pkt->flags |= AV_PKT_FLAG_DISPOSABLE;
            return 0;
        }
        if (ret == AVERROR_EOF) {
            if (!r->chapters_set && (ret = title_end(s)) < 0)
                return ret;
            return AVERROR_EOF;
        }
        if (ret != AVERROR(EAGAIN))
            return ret;
        if ((ret = read_input(s)) < 0)
            return ret;
    }
}
