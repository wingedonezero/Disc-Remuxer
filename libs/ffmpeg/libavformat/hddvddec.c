/*
 * HD DVD (Advanced Content) demuxer: the orchestrator. It opens a disc image
 * through the disc readers (discio) and reads the title set's structure
 * (VTI), the playlists, the AACS keys and the title plan; the EVOB
 * reading follows in a later step.
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
#include "libavutil/opt.h"

#include "avformat.h"
#include "demux.h"
#include "discio.h"
#include "hddvd_internal.h"

typedef struct HDDVDDemuxContext {
    const AVClass *class;
    int            opt_read_attempts;
    int            opt_udf_reader;
    int            opt_min_length;
    char         **opt_keydb;
    unsigned       nb_opt_keydb;

    DiscIOSource  *image;
    DiscIOFS      *fs;
    HDDVDVTI      *vti;
    HDDVDXpl     **xpls;
    int            nb_xpls;
    HDDVDAACS     *aacs;
    HDDVDTitlePlan *plan;
} HDDVDDemuxContext;

static int hddvd_close(AVFormatContext *s)
{
    HDDVDDemuxContext *c = s->priv_data;

    ff_hddvd_titles_free(&c->plan);
    ff_hddvd_aacs_close(&c->aacs);
    ff_hddvd_xpl_free_all(&c->xpls, c->nb_xpls);
    ff_hddvd_vti_free(&c->vti);
    ff_discio_fs_close(&c->fs);
    ff_discio_source_free(&c->image);
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

    av_log(s, AV_LOG_ERROR, "HD DVD: reading a title is not implemented yet\n");
    return AVERROR_PATCHWELCOME;
}

static int hddvd_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return AVERROR_EOF;
}

#define OFFSET(x) offsetof(HDDVDDemuxContext, x)
static const AVOption hddvd_options[] = {
    {"read_attempts",   "read attempts per request on the disc",                    OFFSET(opt_read_attempts),  AV_OPT_TYPE_INT,    { .i64=DISCIO_DEFAULT_ATTEMPTS }, 1, 100, AV_OPT_FLAG_DECODING_PARAM },
    {"keydb",           "AACS key files (KEYDB.cfg), read in this order",          OFFSET(opt_keydb),          AV_OPT_TYPE_STRING | AV_OPT_TYPE_FLAG_ARRAY, { .arr = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
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
