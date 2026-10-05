/*
 * HD DVD (Advanced Content) demuxer: the orchestrator. It opens a disc image
 * through the disc readers (discio) and reads the title set's structure
 * (VTI), the playlists, the AACS keys, the title plan and the titles'
 * tracks; one title's tracks become the streams. The EVOB reading follows
 * in a later step.
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

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "discio.h"
#include "hddvd_internal.h"
#include "internal.h"

#define HDDVD_BLOCK 2048

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
    int64_t        pts_offset;      /**< added to the clip's timestamps (90 kHz) */
    AVFormatContext *mpeg_ctx;
    FFIOContext    mpeg_pb;
    uint8_t       *mpeg_buf;
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
        if (!ret) {
            av_log(s, AV_LOG_WARNING, "EVOB %s: block %"PRIu32" is not usable: left out\n", clip->evob->name, b);
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
    c->mpeg_ctx->flags = AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_GENPTS;
    c->mpeg_ctx->ctx_flags |= AVFMTCTX_UNSEEKABLE;
    c->mpeg_ctx->probesize = 0;
    c->mpeg_ctx->max_analyze_duration = 0;
    c->mpeg_ctx->interrupt_callback = s->interrupt_callback;
    c->mpeg_ctx->pb = &c->mpeg_pb.pub;
    c->mpeg_ctx->io_open = NULL;
    return avformat_open_input(&c->mpeg_ctx, "", &ff_mpegps_demuxer.p, NULL);
}

/* Start reading clip k of the title: its blocks, its time offset (the clips
 * before it laid end to end, each from its start time). */
static int clip_start(AVFormatContext *s, int k)
{
    HDDVDDemuxContext *c = s->priv_data;
    const HDDVDTitle *t = c->title;
    const HDDVDEvob *e = t->clips[k]->evob;
    int64_t before = 0;

    for (int i = 0; i < k; i++) {
        const HDDVDEvob *p = t->clips[i]->evob;
        before += (p->start_ptm <= p->end_ptm ? p->end_ptm : p->end_ptm + (1LL << 32)) - p->start_ptm;
    }
    c->clip       = k;
    c->block      = 0;
    c->nb_blocks  = t->clips[k]->size / HDDVD_BLOCK;
    c->clip_done  = 0;
    c->pts_offset = before - e->start_ptm;
    av_log(s, AV_LOG_VERBOSE, "Reading EVOB %s (%d of %d): %"PRIu32" blocks, start time %"PRIu32", "
           "time offset %"PRId64"\n", e->name, k + 1, t->nb_clips, c->nb_blocks, e->start_ptm, c->pts_offset);
    subdemux_close(c);
    return subdemux_open(s);
}

static int hddvd_close(AVFormatContext *s)
{
    HDDVDDemuxContext *c = s->priv_data;

    subdemux_close(c);
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
        avpriv_set_pts_info(st, 64, 1, 90000);
        if (k->core == 2)
            st->disposition |= AV_DISPOSITION_DEPENDENT;
        if (k->type == AVMEDIA_TYPE_SUBTITLE) {
            st->codecpar->width   = k->width;
            st->codecpar->height  = k->height;
        }
        if (k->lang[0] && av_dict_set(&st->metadata, "language", k->lang, 0) < 0)
            return AVERROR(ENOMEM);
    }
    return 0;
}

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
    c->title = &c->plan->titles[c->opt_title];
    if ((ret = add_streams(s, c->title)) < 0)
        return ret;
    for (int i = 0; i < c->title->nb_marks; i++)
        if (!avpriv_new_chapter(s, i, (AVRational){ 1, 1000 }, c->title->marks[i].ms,
                                i + 1 < c->title->nb_marks ? c->title->marks[i + 1].ms : c->title->duration / 90,
                                c->title->marks[i].name && *c->title->marks[i].name ? c->title->marks[i].name : NULL))
            return AVERROR(ENOMEM);
    s->duration = av_rescale(c->title->duration, AV_TIME_BASE, 90000);
    return clip_start(s, 0);
}

static int hddvd_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    HDDVDDemuxContext *c = s->priv_data;
    AVStream *sub;
    int ret;

    ret = av_read_frame(c->mpeg_ctx, pkt);
    if (ret == AVERROR_EOF && c->clip_done) {
        if (c->clip + 1 >= c->title->nb_clips)
            return AVERROR_EOF;
        if ((ret = clip_start(s, c->clip + 1)) < 0)
            return ret;
        return FFERROR_REDO;
    }
    if (ret < 0)
        return ret;
    sub = c->mpeg_ctx->streams[pkt->stream_index];
    for (int i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->id == sub->id) {
            pkt->stream_index = i;
            if (pkt->pts != AV_NOPTS_VALUE)
                pkt->pts += c->pts_offset;
            if (pkt->dts != AV_NOPTS_VALUE)
                pkt->dts += c->pts_offset;
            return 0;
        }
    }
    av_log(s, AV_LOG_DEBUG, "EVOB %s: packet of stream 0x%x (no track): left out\n",
           c->title->clips[c->clip]->evob->name, sub->id);
    av_packet_unref(pkt);
    return FFERROR_REDO;
}

#define OFFSET(x) offsetof(HDDVDDemuxContext, x)
static const AVOption hddvd_options[] = {
    {"read_attempts",   "read attempts per request on the disc",                    OFFSET(opt_read_attempts),  AV_OPT_TYPE_INT,    { .i64=DISCIO_DEFAULT_ATTEMPTS }, 1, 100, AV_OPT_FLAG_DECODING_PARAM },
    {"keydb",           "AACS key files (KEYDB.cfg), read in this order",          OFFSET(opt_keydb),          AV_OPT_TYPE_STRING | AV_OPT_TYPE_FLAG_ARRAY, { .arr = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    {"title",           "the title to open (0 = the first of the disc's title list)", OFFSET(opt_title), AV_OPT_TYPE_INT, { .i64=0 }, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
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
