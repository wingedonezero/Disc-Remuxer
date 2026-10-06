/*
 * HD DVD (Advanced Content) demuxer: the orchestrator. It opens a disc image
 * through the disc readers (discio) and reads the title set's structure
 * (VTI), the playlists, the AACS keys, the title plan and the titles'
 * tracks; one title's tracks become the streams. Its EVOB clips are read in
 * order through FFmpeg's MPEG-PS demuxer (raw PES payloads) into the shared
 * rip core, whose output frames are the packets.
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

#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "discio.h"
#include "discrip.h"
#include "hddvd_internal.h"
#include "internal.h"

#define HDDVD_BLOCK 2048
#define PTS_REF_SECTORS 2000              /* the PTS wrap reference is looked for in the first sectors */
#define PTS_REF_BACK    27000000LL        /* and lies 300 s (90 kHz) before the first video time */
#define TAIL_BYTES      (64LL << 20)      /* frames from a clip's last 64 MiB may be joined seamlessly */
#define LEAD_IN_TOLERANCE 110160000LL     /* HD DVD audio lead-in tolerance: 102 ms (ticks) */

typedef struct HDDVDDemuxContext {
    const AVClass *class;
    int            opt_read_attempts;
    int            opt_udf_reader;
    int            opt_min_length;
    int            opt_title;
    char         **opt_keydb;
    unsigned       nb_opt_keydb;

    DiscIOSource  *image;
    DiscIOFS      *fs;
    HDDVDVTI      *vti;
    HDDVDXpl     **xpls;
    int            nb_xpls;
    HDDVDAACS     *aacs;
    HDDVDTitlePlan *plan;

    /* reading the chosen title: its clips in order through an MPEG-PS sub-demuxer */
    const HDDVDTitle *title;
    int            clip;            /**< clip being read */
    uint32_t       block;           /**< next block of that clip */
    uint32_t       nb_blocks;       /**< blocks of that clip */
    int            clip_done;       /**< the clip's last block was given to the sub-demuxer */
    uint32_t       skipped;         /**< unusable blocks of that clip left out so far */
    uint64_t       tail_start;      /**< byte of that clip where its last 64 MiB begin */
    int64_t        pts_ref;         /**< that clip's PTS wrap reference (90 kHz), -1 while not known */
    int64_t        last_sector;     /**< sector of the last PES packet read */
    AVPacket     **held;            /**< packets read before the reference was known */
    int            nb_held, held_cap;
    AVFormatContext *mpeg_ctx;
    FFIOContext    mpeg_pb;
    uint8_t       *mpeg_buf;

    /* the shared rip core */
    DRTitle       *rip;
    DRChapterPlan  chapter_plan;
    int            rip_done;        /**< every clip was read, the core finished */
    int64_t        blocks_total;    /**< blocks of all the title's clips */
    int64_t        blocks_done;     /**< blocks read so far */
    int64_t        progress;        /**< exported option: blocks_done / blocks_total in 1/10000 */
    int            chapters_set;
    AVPacket      *in;
} HDDVDDemuxContext;

static void subdemux_close(HDDVDDemuxContext *c)
{
    av_freep(&c->mpeg_pb.pub.buffer);
    avformat_close_input(&c->mpeg_ctx);
}

/* The next block of the clip being read; EOF at the clip's end. */
static int subdemux_read(void *opaque, uint8_t *buf, int buf_size)
{
    AVFormatContext *s = opaque;
    HDDVDDemuxContext *c = s->priv_data;
    HDDVDClip *clip = c->title->clips[c->clip];
    int ret;

    if (buf_size < HDDVD_BLOCK)
        return AVERROR(EINVAL);
    while (c->block < c->nb_blocks) {
        uint32_t b = c->block++;

        ret = ff_hddvd_clip_block(s, c->aacs, c->fs, clip, b, buf);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "EVOB %s: block %"PRIu32" of %"PRIu32" cannot be read (%s): "
                   "the rest of the EVOB is not read\n", clip->evob->name, b, c->nb_blocks, av_err2str(ret));
            c->block = c->nb_blocks;
            break;
        }
        c->blocks_done++;
        if (c->blocks_total)
            c->progress = c->blocks_done * 10000 / c->blocks_total;
        if (!ret) {
            av_log(s, AV_LOG_WARNING, "EVOB %s: block %"PRIu32" is not usable: left out\n", clip->evob->name, b);
            c->skipped++;
            continue;
        }
        return HDDVD_BLOCK;
    }
    c->clip_done = 1;
    return AVERROR_EOF;
}

static int subdemux_open(AVFormatContext *s)
{
    HDDVDDemuxContext *c = s->priv_data;
    extern const FFInputFormat ff_mpegps_demuxer;
    int ret;

    if (!(c->mpeg_buf = av_mallocz(HDDVD_BLOCK)))
        return AVERROR(ENOMEM);
    ffio_init_context(&c->mpeg_pb, c->mpeg_buf, HDDVD_BLOCK, 0, s, subdemux_read, NULL, NULL);
    c->mpeg_pb.pub.seekable = 0;
    if (!(c->mpeg_ctx = avformat_alloc_context()))
        return AVERROR(ENOMEM);
    if ((ret = ff_copy_whiteblacklists(c->mpeg_ctx, s)) < 0) {
        avformat_free_context(c->mpeg_ctx);
        c->mpeg_ctx = NULL;
        return ret;
    }
    /* raw PES payloads with their own times: no parsing, no filled-in or
     * corrected timestamps (the rip core times every unit) */
    c->mpeg_ctx->flags = AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
    c->mpeg_ctx->correct_ts_overflow = 0;
    c->mpeg_ctx->ctx_flags |= AVFMTCTX_UNSEEKABLE;
    c->mpeg_ctx->probesize = 0;
    c->mpeg_ctx->max_analyze_duration = 0;
    c->mpeg_ctx->interrupt_callback = s->interrupt_callback;
    c->mpeg_ctx->pb = &c->mpeg_pb.pub;
    c->mpeg_ctx->io_open = NULL;
    return avformat_open_input(&c->mpeg_ctx, "", &ff_mpegps_demuxer.p, NULL);
}

static void held_free(HDDVDDemuxContext *c)
{
    for (int i = 0; i < c->nb_held; i++)
        av_packet_free(&c->held[i]);
    c->nb_held = 0;
}

/* Start reading clip k of the title (a segment of the rip core): its blocks,
 * where its last 64 MiB begin, its PTS wrap reference still to be found. */
static int clip_start(AVFormatContext *s, int k)
{
    HDDVDDemuxContext *c = s->priv_data;
    const HDDVDTitle *t = c->title;
    const HDDVDClip *clip = t->clips[k];
    int ret;

    c->clip        = k;
    c->block       = 0;
    c->nb_blocks   = clip->size / HDDVD_BLOCK;
    c->clip_done   = 0;
    c->skipped     = 0;
    c->tail_start  = clip->size >= TAIL_BYTES ? clip->size - TAIL_BYTES : 0;
    c->pts_ref     = -1;
    c->last_sector = -1;
    held_free(c);
    av_log(s, AV_LOG_VERBOSE, "Reading EVOB %s (%d of %d): %"PRIu32" blocks, the last 64 MiB from byte %"PRIu64"\n",
           clip->evob->name, k + 1, t->nb_clips, c->nb_blocks, c->tail_start);
    subdemux_close(c);
    if ((ret = subdemux_open(s)) < 0)
        return ret;
    return ff_discrip_title_segment(c->rip);
}

static int hddvd_close(AVFormatContext *s)
{
    HDDVDDemuxContext *c = s->priv_data;

    subdemux_close(c);
    held_free(c);
    av_freep(&c->held);
    av_packet_free(&c->in);
    ff_discrip_title_close(&c->rip);
    ff_discrip_chapter_plan_free(&c->chapter_plan);
    ff_hddvd_titles_free(&c->plan);
    ff_hddvd_aacs_close(&c->aacs);
    ff_hddvd_xpl_free_all(&c->xpls, c->nb_xpls);
    ff_hddvd_vti_free(&c->vti);
    ff_discio_fs_close(&c->fs);
    ff_discio_source_free(&c->image);
    return 0;
}

/* The id FFmpeg's MPEG-PS demuxer gives a track's packets: video stream_id
 * 0xE0 (MPEG-2) / 0xE2 (AVC) / 0xFD with extension 0x55 (VC-1); private
 * stream 1 sub-streams AC-3 0x80+n, DTS 0x88+n, LPCM 0xA0+n, MLP 0xB0+n,
 * Dolby Digital Plus 0xC0+n, sub-pictures 0x20+n; MPEG audio stream_id
 * 0xC0+n. */
static int pes_id(const HDDVDTrack *k)
{
    static const int audio_base[8] = { 0x80, 0xb0, 0x1c0, -1, 0xa0, 0xa0, 0x88, 0xc0 };

    switch (k->type) {
    case AVMEDIA_TYPE_VIDEO:
        return k->codec == AV_CODEC_ID_VC1 ? 0xfd55 : k->codec == AV_CODEC_ID_H264 ? 0x1e2 : 0x1e0;
    case AVMEDIA_TYPE_AUDIO:
        return k->coding >= 0 && k->coding < 8 && audio_base[k->coding] >= 0 ? audio_base[k->coding] + k->number : -1;
    case AVMEDIA_TYPE_SUBTITLE:
        return 0x20 + k->number;
    default:
        return -1;
    }
}

static int add_streams(AVFormatContext *s, const HDDVDTitle *t)
{
    for (int i = 0; i < t->nb_tracks; i++) {
        const HDDVDTrack *k = &t->tracks[i];
        AVStream *st = avformat_new_stream(s, NULL);

        if (!st)
            return AVERROR(ENOMEM);
        st->id                    = pes_id(k);
        st->codecpar->codec_type  = k->type;
        st->codecpar->codec_id    = k->codec;
        avpriv_set_pts_info(st, 64, 1, DR_TICKS_PER_SECOND);
        if (k->core == 2)
            st->disposition |= AV_DISPOSITION_DEPENDENT;
        if (k->type == AVMEDIA_TYPE_SUBTITLE) {
            char idx[1024];
            int n = ff_discrip_vobsub_header(idx, sizeof(idx), k->width, k->height, k->palette);
            int ret;

            st->codecpar->width   = k->width;
            st->codecpar->height  = k->height;
            /* the VobSub index header (FFmpeg's DVD subtitle decoder reads
             * size and palette from it) */
            if (n < 0 || n >= sizeof(idx))
                return AVERROR_BUG;
            if ((ret = ff_alloc_extradata(st->codecpar, n)) < 0)
                return ret;
            memcpy(st->codecpar->extradata, idx, n);
        }
        if (k->lang[0] && av_dict_set(&st->metadata, "language", k->lang, 0) < 0)
            return AVERROR(ENOMEM);
    }
    return 0;
}

static int rip_open(AVFormatContext *s);

static int hddvd_read_header(AVFormatContext *s)
{
    HDDVDDemuxContext *c = s->priv_data;
    DiscIOImageOptions opts = DISCIO_IMAGE_OPTIONS_DEFAULT;
    int ret;

    opts.udf_reader = c->opt_udf_reader;
    if ((ret = ff_discio_source_open_file(s, s->url, &c->image)) < 0)
        return ret;
    c->image->attempts = c->opt_read_attempts;
    if ((ret = ff_discio_mount_image(c->image, &opts, &c->fs)) < 0)
        return ret;
    if (ff_discio_disc_format(c->fs) != DISCIO_DISC_HDDVD) {
        av_log(s, AV_LOG_ERROR, "'%s' is not an HD DVD image (no /HVDVD_TS/HVA00001.VTI "
               "or /HDDVD_TS/HVA00001.VTI with /ADV_OBJ/DISCID.DAT)\n", s->url);
        return AVERROR_INVALIDDATA;
    }
    if ((ret = ff_hddvd_vti_open(s, c->fs, &c->vti)) < 0)
        return ret;
    if ((ret = ff_hddvd_xpl_load(s, c->fs, &c->xpls, &c->nb_xpls)) < 0)
        return ret;
    ff_hddvd_evob_marks(s, c->vti, c->xpls, c->nb_xpls);
    if ((ret = ff_hddvd_aacs_open(s, c->fs, (const char *const *)c->opt_keydb, c->nb_opt_keydb,
                                  c->nb_xpls, &c->aacs)) < 0)
        return ret;
    if ((ret = ff_hddvd_titles_plan(s, c->fs, c->vti, c->xpls, c->nb_xpls, c->opt_min_length, &c->plan)) < 0)
        return ret;
    if ((ret = ff_hddvd_tracks_build(s, c->fs, c->aacs, c->vti, c->xpls, c->nb_xpls, c->plan)) < 0)
        return ret;
    if (c->opt_title >= c->plan->nb_titles) {
        av_log(s, AV_LOG_ERROR, "HD DVD: title %d does not exist (the disc has %d)\n", c->opt_title,
               c->plan->nb_titles);
        return AVERROR(EINVAL);
    }
    av_dict_set_int(&s->metadata, "titles", c->plan->nb_titles, 0);
    /* the disc as a whole, for the caller's overview */
    av_dict_set(&s->metadata, "disc", "HD DVD", 0);
    av_dict_set(&s->metadata, "label", c->fs->label, 0);
    av_dict_set(&s->metadata, "filesystem", c->fs->ops->name, 0);
    if (c->fs->udf_revision)
        av_dict_set_int(&s->metadata, "udf_revision", c->fs->udf_revision, 0);
    av_dict_set_int(&s->metadata, "encrypted", c->aacs != NULL, 0);
    c->title = &c->plan->titles[c->opt_title];
    if (c->title->name && *c->title->name && av_dict_set(&s->metadata, "title", c->title->name, 0) < 0)
        return AVERROR(ENOMEM);
    /* the disc's chapter records (the ripped title's chapters are known at its end) */
    if (av_dict_set_int(&s->metadata, "chapters", c->title->nb_marks, 0) < 0)
        return AVERROR(ENOMEM);
    if ((ret = add_streams(s, c->title)) < 0)
        return ret;
    s->duration = av_rescale(c->title->duration, AV_TIME_BASE, 90000);
    if ((ret = rip_open(s)) < 0)
        return ret;
    return clip_start(s, 0);
}

/* The chapter plan from the title's marks and the rip core for its tracks
 * (track 0 is the title's video). */
static int rip_open(AVFormatContext *s)
{
    HDDVDDemuxContext *c = s->priv_data;
    const HDDVDTitle *t = c->title;
    DRTitleTrack *tracks;
    int64_t *records;
    int ret;

    if (!(records = av_calloc(FFMAX(t->nb_marks, 1), sizeof(*records))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < t->nb_marks; i++)
        records[i] = (int64_t)t->marks[i].ms * (DR_TICKS_PER_SECOND / 1000);
    /* no leading clip is left out yet (skip 0); no "Chapter 00" */
    ret = ff_discrip_chapter_plan(s, records, t->nb_marks, 0, 0, &c->chapter_plan);
    av_free(records);
    if (ret < 0)
        return ret;
    if (!(tracks = av_calloc(t->nb_tracks, sizeof(*tracks))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < t->nb_tracks; i++) {
        const HDDVDTrack *k = &t->tracks[i];
        tracks[i].codec       = k->codec;
        tracks[i].kind        = k->type == AVMEDIA_TYPE_VIDEO ? DR_KIND_VIDEO :
                                k->type == AVMEDIA_TYPE_AUDIO ? DR_KIND_AUDIO : DR_KIND_SUBTITLE;
        tracks[i].audio_flags = k->core == 2 ? DR_AUDIO_CORE_ONLY : 0;
    }
    {
        DRTitleConfig cfg = {
            .nb_tracks = t->nb_tracks, .tracks = tracks, .tolerance = LEAD_IN_TOLERANCE, .lpcm_hd = 1,
            .marks = c->chapter_plan.marks, .nb_marks = c->chapter_plan.nb_marks,
        };
        ret = ff_discrip_title_open(&c->rip, s, &cfg);
    }
    av_free(tracks);
    if (ret < 0)
        return ret;
    if (!(c->in = av_packet_alloc()))
        return AVERROR(ENOMEM);
    for (int i = 0; i < t->nb_clips; i++)
        c->blocks_total += t->clips[i]->size / HDDVD_BLOCK;
    return 0;
}

/* A payload of the clip being read to every track it feeds (a DTS-HD
 * stream also feeds its core track): its PES time with the clip's wrap
 * reference, in ticks; whether it lies in the clip's last 64 MiB. */
static int feed(AVFormatContext *s, const AVPacket *pkt)
{
    HDDVDDemuxContext *c = s->priv_data;
    const AVStream *sub = c->mpeg_ctx->streams[pkt->stream_index];
    int64_t time = AV_NOPTS_VALUE;
    int tail = (uint64_t)pkt->pos + (uint64_t)c->skipped * HDDVD_BLOCK >= c->tail_start, n = 0, ret;

    if (pkt->pts != AV_NOPTS_VALUE)
        time = (pkt->pts < c->pts_ref ? pkt->pts + (1LL << 33) : pkt->pts) * DR_TICKS_PER_PTS;
    for (int i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->id != sub->id)
            continue;
        if ((ret = ff_discrip_title_payload(c->rip, i, pkt->data, pkt->size, time, tail)) < 0)
            return ret;
        n++;
    }
    if (!n)
        av_log(s, AV_LOG_TRACE, "EVOB %s: packet of stream 0x%x (no track): left out\n",
               c->title->clips[c->clip]->evob->name, sub->id);
    return 0;
}

static int hold(HDDVDDemuxContext *c, AVPacket *pkt)
{
    if (c->nb_held == c->held_cap) {
        int cap = c->held_cap ? 2 * c->held_cap : 64;
        AVPacket **h = av_realloc_array(c->held, cap, sizeof(*h));
        if (!h)
            return AVERROR(ENOMEM);
        c->held     = h;
        c->held_cap = cap;
    }
    if (!(c->held[c->nb_held] = av_packet_alloc()))
        return AVERROR(ENOMEM);
    av_packet_move_ref(c->held[c->nb_held++], pkt);
    return 0;
}

/* One PES packet of the title into the rip core. The clip's PTS wrap
 * reference is the time of the first video packet that starts a sector in
 * its first 2000 sectors, less 300 s; packets before it wait for it. A
 * time below the reference has wrapped (+2^33). */
static int read_input(AVFormatContext *s)
{
    HDDVDDemuxContext *c = s->priv_data;
    AVPacket *pkt = c->in;
    const AVStream *sub;
    int64_t sector;
    int first, ret;

    ret = av_read_frame(c->mpeg_ctx, pkt);
    if (ret == AVERROR_EOF && c->clip_done) {
        if (c->pts_ref < 0) {
            av_log(s, AV_LOG_ERROR, "EVOB %s: no video time in its first %d sectors: no time reference\n",
                   c->title->clips[c->clip]->evob->name, PTS_REF_SECTORS);
            return AVERROR_INVALIDDATA;
        }
        if (c->clip + 1 < c->title->nb_clips)
            return clip_start(s, c->clip + 1);
        if ((ret = ff_discrip_title_finish(c->rip)) < 0)
            return ret;
        c->rip_done = 1;
        return 0;
    }
    if (ret < 0)
        return ret;
    sub    = c->mpeg_ctx->streams[pkt->stream_index];
    sector = pkt->pos / HDDVD_BLOCK + c->skipped;
    first  = sector != c->last_sector;
    c->last_sector = sector;
    if (c->pts_ref < 0) {
        int64_t limit = FFMIN(PTS_REF_SECTORS, (int64_t)(c->title->clips[c->clip]->size / HDDVD_BLOCK));

        if (sub->id == s->streams[0]->id && pkt->pts != AV_NOPTS_VALUE && first && sector < limit) {
            c->pts_ref = pkt->pts >= PTS_REF_BACK ? pkt->pts - PTS_REF_BACK : pkt->pts + (1LL << 33) - PTS_REF_BACK;
            av_log(s, AV_LOG_VERBOSE, "EVOB %s: first video time %"PRId64" in sector %"PRId64": wrap reference %"
                   PRId64"\n", c->title->clips[c->clip]->evob->name, pkt->pts, sector, c->pts_ref);
            for (int i = 0; i < c->nb_held; i++)
                if ((ret = feed(s, c->held[i])) < 0)
                    break;
            held_free(c);
            if (ret < 0) {
                av_packet_unref(pkt);
                return ret;
            }
        } else if (sector >= limit) {
            av_log(s, AV_LOG_ERROR, "EVOB %s: no video time in its first %"PRId64" sectors: no time reference\n",
                   c->title->clips[c->clip]->evob->name, limit);
            av_packet_unref(pkt);
            return AVERROR_INVALIDDATA;
        } else {
            return hold(c, pkt);
        }
    }
    ret = feed(s, pkt);
    av_packet_unref(pkt);
    return ret;
}

/* The chapters of the ripped title, once the core finished. */
static int set_chapters(AVFormatContext *s)
{
    HDDVDDemuxContext *c = s->priv_data;
    DRChapter *ch;
    int n, ret;

    c->chapters_set = 1;
    if ((ret = ff_discrip_title_chapters(c->rip, &c->chapter_plan, &ch, &n)) < 0)
        return ret;
    for (int i = 0; i < n; i++) {
        const char *name = ch[i].record >= 0 && c->title->marks[ch[i].record].name &&
                           *c->title->marks[ch[i].record].name ? c->title->marks[ch[i].record].name : NULL;
        if (!avpriv_new_chapter(s, i, (AVRational){ 1, DR_TICKS_PER_SECOND }, ch[i].start, ch[i].end, name)) {
            av_free(ch);
            return AVERROR(ENOMEM);
        }
    }
    av_free(ch);
    s->duration = av_rescale(ff_discrip_title_duration(c->rip), AV_TIME_BASE, DR_TICKS_PER_SECOND);
    return 0;
}

/* A linear PCM track leaves as little-endian PCM: its stream takes the
 * format of the first audio frame header. */
static int lpcm_params(AVFormatContext *s, int k)
{
    HDDVDDemuxContext *c = s->priv_data;
    AVCodecParameters *par = s->streams[k]->codecpar;
    const DRLpcm *p;

    if (par->codec_id != AV_CODEC_ID_PCM_DVD || !(p = ff_discrip_title_lpcm(c->rip, k)))
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

/* The rip core's next output frame as a packet (time base 1/1,080,000,000
 * s); empty markers carry no bytes and are not given out. */
static int hddvd_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    HDDVDDemuxContext *c = s->priv_data;
    int ret;

    for (;;) {
        DRFrame f;
        int k;

        ret = ff_discrip_title_frame(c->rip, &k, &f);
        if (!ret) {
            if (!f.size) {
                ff_discrip_frame_unref(&f);
                continue;
            }
            if ((ret = lpcm_params(s, k)) < 0) {
                ff_discrip_frame_unref(&f);
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
            int64_t review = ff_discrip_title_review(c->rip);
            if (!c->chapters_set) {
                if ((ret = set_chapters(s)) < 0)
                    return ret;
                /* features met that no real disc has tested: the caller
                 * reports the job as failed so that its log is looked at */
                if (review) {
                    av_log(s, AV_LOG_ERROR, "HD DVD: %"PRId64" untested feature(s) met in this title: "
                           "check the log before using the output\n", review);
                    if ((ret = av_dict_set_int(&s->metadata, "untested", review, 0)) < 0)
                        return ret;
                }
                /* each track's result for the caller: frames, warnings, start delay (ms) */
                for (int i = 0; i < s->nb_streams; i++) {
                    int64_t frames, warnings, delay;
                    AVDictionary **m = &s->streams[i]->metadata;
                    if (ff_discrip_title_track_result(c->rip, i, &frames, &warnings, &delay) < 0)
                        continue;
                    if ((ret = av_dict_set_int(m, "frames", frames, 0)) < 0 ||
                        (ret = av_dict_set_int(m, "warnings", warnings, 0)) < 0 ||
                        (ret = av_dict_set_int(m, "delay_us", delay / (DR_TICKS_PER_SECOND / 1000000), 0)) < 0)
                        return ret;
                }
            }
            return AVERROR_EOF;
        }
        if (ret != AVERROR(EAGAIN))
            return ret;
        if ((ret = read_input(s)) < 0)
            return ret;
    }
}

#define OFFSET(x) offsetof(HDDVDDemuxContext, x)
static const AVOption hddvd_options[] = {
    {"read_attempts",   "read attempts per request on the disc",                    OFFSET(opt_read_attempts),  AV_OPT_TYPE_INT,    { .i64=DISCIO_DEFAULT_ATTEMPTS }, 1, 100, AV_OPT_FLAG_DECODING_PARAM },
    {"keydb",           "AACS key files (KEYDB.cfg), read in this order",          OFFSET(opt_keydb),          AV_OPT_TYPE_STRING | AV_OPT_TYPE_FLAG_ARRAY, { .arr = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    {"title",           "the title to open (0 = the first of the disc's title list)", OFFSET(opt_title), AV_OPT_TYPE_INT, { .i64=0 }, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    {"progress",        "how far the title is read (1/10000), exported", OFFSET(progress), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, 10000, AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY },
    {"min_length",      "titles shorter than this (seconds) are listed, not selected", OFFSET(opt_min_length), AV_OPT_TYPE_INT, { .i64=0 }, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    {"udf_reader",      "UDF reader for disc images",                               OFFSET(opt_udf_reader),     AV_OPT_TYPE_INT,    { .i64=DISCIO_UDF_NETBSD }, DISCIO_UDF_NETBSD, DISCIO_UDF_LINUX, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
        {"netbsd",      "based on NetBSD (default)",                                0,                          AV_OPT_TYPE_CONST,  { .i64=DISCIO_UDF_NETBSD }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
        {"linux",       "based on Linux",                                           0,                          AV_OPT_TYPE_CONST,  { .i64=DISCIO_UDF_LINUX },  0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
    {NULL}
};

static const AVClass hddvd_class = {
    .class_name = "HD DVD demuxer",
    .item_name  = av_default_item_name,
    .option     = hddvd_options,
    .version    = LIBAVUTIL_VERSION_INT
};

const FFInputFormat ff_hddvd_demuxer = {
    .p.name         = "hddvd",
    .p.long_name    = NULL_IF_CONFIG_SMALL("HD DVD (Advanced Content)"),
    .p.priv_class   = &hddvd_class,
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_NOFILE | AVFMT_NO_BYTE_SEEK |
                      AVFMT_NOGENSEARCH | AVFMT_NOBINSEARCH,
    .priv_data_size = sizeof(HDDVDDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_close     = hddvd_close,
    .read_header    = hddvd_read_header,
    .read_packet    = hddvd_read_packet,
};
