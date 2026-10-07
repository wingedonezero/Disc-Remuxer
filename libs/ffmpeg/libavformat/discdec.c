/*
 * The disc layer: the one demuxer for discs. It opens the source once
 * through the disc readers (an image file or a folder), tells which kind of
 * disc it is from its file system and hands it to that kind's format part
 * (disc.h). The disc's own details (kind, label, file system) go into the
 * disc info (discinfo.h) here; its titles come from the format part.
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

#include <limits.h>
#include <string.h>

#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "demux.h"
#include "disc.h"
#include "discinfo.h"
#include "discio.h"

/* The format parts, by the disc kind the readers tell. */
static const DiscFormat *const parts[] = {
    &ff_disc_hddvd,
    NULL
};

static const char *kind_name(enum DiscIODiscFormat k)
{
    switch (k) {
    case DISCIO_DISC_DVD:    return "DVD-Video";
    case DISCIO_DISC_BLURAY: return "Blu-ray";
    case DISCIO_DISC_HDDVD:  return "HD DVD";
    default:                 return "no disc";
    }
}

DiscTitleInfo *ff_disc_add_title(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    DiscTitleInfo *t = av_dynarray2_add((void **)&d->info.titles, &d->info.nb_titles, sizeof(*t), NULL);

    if (t) {
        memset(t, 0, sizeof(*t));
        t->index = d->info.nb_titles - 1;
    }
    return t;
}

DiscStreamInfo *ff_disc_title_add_stream(DiscTitleInfo *t)
{
    DiscStreamInfo *k = av_dynarray2_add((void **)&t->streams, &t->nb_streams, sizeof(*k), NULL);
    if (k) {
        memset(k, 0, sizeof(*k));
        k->inner_track = k->from_track = -1;
    }
    return k;
}

DiscChapterInfo *ff_disc_title_add_chapter(DiscTitleInfo *t)
{
    DiscChapterInfo *c = av_dynarray2_add((void **)&t->chapters, &t->nb_chapters, sizeof(*c), NULL);
    if (c)
        memset(c, 0, sizeof(*c));
    return c;
}

int ff_disc_title_add_file(DiscTitleInfo *t, const char *name)
{
    return av_dynarray_add_nofree(&t->files, &t->nb_files, (void *)name);
}

extern const FFInputFormat ff_disc_demuxer;

const DiscInfo *disc_info(const AVFormatContext *s)
{
    return s && s->iformat == &ff_disc_demuxer.p ? &((const DiscContext *)s->priv_data)->info : NULL;
}

static int disc_close(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;

    if (d->fmt && d->part)
        d->fmt->close(s);
    av_freep(&d->part);
    for (int i = 0; i < d->info.nb_titles; i++) {
        av_freep(&d->info.titles[i].streams);
        av_freep(&d->info.titles[i].chapters);
        av_freep(&d->info.titles[i].files);
    }
    av_freep(&d->info.titles);
    d->info.nb_titles = 0;
    ff_discio_fs_close(&d->fs);
    ff_discio_source_free(&d->image);
    return 0;
}

static int disc_read_header(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    DiscIOImageOptions opts = DISCIO_IMAGE_OPTIONS_DEFAULT;
    enum DiscIODiscFormat kind;
    int ret;

    opts.udf_reader = d->opt_udf_reader;
    if ((ret = ff_discio_source_open_file(s, s->url, &d->image)) < 0)
        return ret;
    d->image->attempts = d->opt_read_attempts;
    if ((ret = ff_discio_mount_image(d->image, &opts, &d->fs)) < 0)
        return ret;
    kind = ff_discio_disc_format(d->fs);
    for (int i = 0; parts[i] && !d->fmt; i++)
        if (parts[i]->kind == kind)
            d->fmt = parts[i];
    if (kind == DISCIO_DISC_NONE) {
        av_log(s, AV_LOG_ERROR, "'%s' holds no disc this program knows (no VIDEO_TS, HVDVD_TS / HDDVD_TS "
               "or BDMV structure)\n", s->url);
        return AVERROR_INVALIDDATA;
    }
    if (!d->fmt) {
        av_log(s, AV_LOG_ERROR, "'%s' is a %s disc, which is not read through the disc layer yet\n",
               s->url, kind_name(kind));
        return AVERROR_PATCHWELCOME;
    }
    av_log(s, AV_LOG_VERBOSE, "'%s': %s disc\n", s->url, d->fmt->name);
    d->info.kind         = d->fmt->name;
    d->info.label        = d->fs->label;
    d->info.filesystem   = d->fs->ops->name;
    d->info.udf_revision = d->fs->udf_revision;
    if (!(d->part = av_mallocz(d->fmt->priv_size)))
        return AVERROR(ENOMEM);
    return d->fmt->open(s);
}

static int disc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    DiscContext *d = s->priv_data;
    return d->fmt->read_packet(s, pkt);
}

#define OFFSET(x) offsetof(DiscContext, x)
static const AVOption disc_options[] = {
    {"title",           "the title to open (0 = the first of the disc's title list; -1 = none: only the disc info)", OFFSET(opt_title), AV_OPT_TYPE_INT, { .i64=0 }, -1, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    {"min_length",      "titles shorter than this (seconds) are listed, not selected", OFFSET(opt_min_length), AV_OPT_TYPE_INT, { .i64=0 }, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    {"keydb",           "AACS key files (KEYDB.cfg), read in this order",          OFFSET(opt_keydb),          AV_OPT_TYPE_STRING | AV_OPT_TYPE_FLAG_ARRAY, { .arr = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    {"read_attempts",   "read attempts per request on the disc",                    OFFSET(opt_read_attempts),  AV_OPT_TYPE_INT,    { .i64=DISCIO_DEFAULT_ATTEMPTS }, 1, 100, AV_OPT_FLAG_DECODING_PARAM },
    {"udf_reader",      "UDF reader for disc images",                               OFFSET(opt_udf_reader),     AV_OPT_TYPE_INT,    { .i64=DISCIO_UDF_NETBSD }, DISCIO_UDF_NETBSD, DISCIO_UDF_LINUX, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
        {"netbsd",      "based on NetBSD (default)",                                0,                          AV_OPT_TYPE_CONST,  { .i64=DISCIO_UDF_NETBSD }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
        {"linux",       "based on Linux",                                           0,                          AV_OPT_TYPE_CONST,  { .i64=DISCIO_UDF_LINUX },  0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
    {"progress",        "how far the title is read (1/10000), exported", OFFSET(progress), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, 10000, AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY },
    {NULL}
};

static const AVClass disc_class = {
    .class_name = "disc demuxer",
    .item_name  = av_default_item_name,
    .option     = disc_options,
    .version    = LIBAVUTIL_VERSION_INT
};

const FFInputFormat ff_disc_demuxer = {
    .p.name         = "disc",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Disc (HD DVD; DVD-Video and Blu-ray to come)"),
    .p.priv_class   = &disc_class,
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_NOFILE | AVFMT_NO_BYTE_SEEK |
                      AVFMT_NOGENSEARCH | AVFMT_NOBINSEARCH,
    .priv_data_size = sizeof(DiscContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_close     = disc_close,
    .read_header    = disc_read_header,
    .read_packet    = disc_read_packet,
};
