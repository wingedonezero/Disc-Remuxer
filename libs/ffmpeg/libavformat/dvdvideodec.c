/*
 * DVD-Video demuxer, powered by libdvdnav and libdvdread
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * See doc/demuxers.texi for a high-level overview.
 *
 * The tactical approach is as follows:
 * 1) Open the volume with dvdread
 * 2) Analyze the user-requested title and PGC coordinates in the IFO structures
 * 3) Request playback at the coordinates and chosen angle with dvdnav
 * 5) Begin the playback (reading and demuxing) of MPEG-PS blocks
 * 6) End playback if navigation goes backwards, to a menu, or a different PGC or angle
 * 7) Close the dvdnav VM, and free dvdread's IFO structures
 */

#include "dvdvideo_internal.h"

static int dvdvideo_subdemux_read_data(void *opaque, uint8_t *buf, int buf_size)
{
    AVFormatContext *s = opaque;
    DVDVideoDemuxContext *c = s->priv_data;

    int ret;
    int is_nav_packet;

    if (c->opt_menu)
        ret = ff_dvdvideo_menu_next_ps_block(s, &c->play_state, buf, buf_size, &is_nav_packet);
    else
        ret = ff_dvdvideo_play_next_ps_block(s, &c->play_state, buf, buf_size, &is_nav_packet);

    if (ret < 0)
        goto subdemux_eof;

    if (is_nav_packet) {
        if (c->play_state.ptm_discont) {
            c->subdemux_reset = 1;

            ret = AVERROR_EOF;
            goto subdemux_eof;
        }

        return FFERROR_REDO;
    }

    return ret;

subdemux_eof:
    c->mpeg_pb.pub.eof_reached = 1;
    c->mpeg_pb.pub.error       = ret;
    c->mpeg_pb.pub.read_packet = NULL;
    c->mpeg_pb.pub.buf_end     = c->mpeg_pb.pub.buf_ptr = c->mpeg_pb.pub.buffer;

    return ret;
}

static void dvdvideo_subdemux_close(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    av_freep(&c->mpeg_pb.pub.buffer);
    avformat_close_input(&c->mpeg_ctx);
}

static int dvdvideo_subdemux_open(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;
    extern const FFInputFormat ff_mpegps_demuxer;
    int ret;

    if (!(c->mpeg_buf = av_mallocz(DVDVIDEO_BLOCK_SIZE)))
        return AVERROR(ENOMEM);

    ffio_init_context(&c->mpeg_pb, c->mpeg_buf, DVDVIDEO_BLOCK_SIZE, 0, s,
                      dvdvideo_subdemux_read_data, NULL, NULL);
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

static int dvdvideo_subdemux_reset(AVFormatContext *s)
{
    int ret;

    av_log(s, AV_LOG_VERBOSE, "Resetting sub-demuxer\n");

    dvdvideo_subdemux_close(s);
    if ((ret = dvdvideo_subdemux_open(s)) < 0)
        return ret;

    return 0;
}

static int dvdvideo_read_header(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    int ret;

    if (c->opt_menu) {
        if (c->opt_region               ||
            c->opt_title > 1            ||
            c->opt_chapter_start > 1    ||
            c->opt_chapter_end > 0) {
            av_log(s, AV_LOG_ERROR, "-menu is not compatible with the -region, -title, "
                                    "or -chapter_start/-chapter_end options\n");
            return AVERROR(EINVAL);
        }

        if (!c->opt_pgc) {
            av_log(s, AV_LOG_ERROR, "If -menu is enabled, -pgc must be set to a non-zero value\n");

            return AVERROR(EINVAL);
        }

        if ((ret = ff_dvdvideo_ifo_open(s)) < 0                                         ||
            (c->opt_preindex && (ret = ff_dvdvideo_chapters_setup_preindex(s)) < 0)     ||
            (ret = ff_dvdvideo_menu_open(s, &c->play_state)) < 0                        ||
            (ret = ff_dvdvideo_video_stream_setup(s)) < 0                               ||
            (ret = ff_dvdvideo_audio_stream_add_all(s)) < 0                             ||
            (ret = dvdvideo_subdemux_open(s)) < 0)
        return ret;

        goto end_ready;
    }

    if (c->opt_pgc && (c->opt_chapter_start > 1 || c->opt_chapter_end > 0 || c->opt_preindex)) {
        av_log(s, AV_LOG_ERROR, "PGC extraction not compatible with chapter or preindex options\n");

        return AVERROR(EINVAL);
    }

    if (!c->opt_pgc && (c->opt_chapter_end != 0 && c->opt_chapter_start > c->opt_chapter_end)) {
        av_log(s, AV_LOG_ERROR, "Chapter (PTT) range [%d, %d] is invalid\n",
                                c->opt_chapter_start, c->opt_chapter_end);

        return AVERROR(EINVAL);
    }

    if (c->opt_title == 0) {
        av_log(s, AV_LOG_INFO, "Defaulting to title #1. "
                               "This is not always the main feature, validation suggested.\n");

        c->opt_title = 1;
    }

    if ((ret = ff_dvdvideo_ifo_open(s)) < 0)
        return ret;

    if (!c->opt_pgc && c->opt_preindex && (ret = ff_dvdvideo_chapters_setup_preindex(s)) < 0)
        return ret;

    if ((ret = ff_dvdvideo_play_open(s, &c->play_state)) < 0                                  ||
        (!c->opt_pgc && !c->opt_preindex && (ret = ff_dvdvideo_chapters_setup_simple(s)) < 0) ||
        (ret = ff_dvdvideo_video_stream_setup(s)) < 0                                         ||
        (ret = ff_dvdvideo_audio_stream_add_all(s)) < 0                                       ||
        (ret = ff_dvdvideo_subp_stream_add_all(s)) < 0                                        ||
        (ret = dvdvideo_subdemux_open(s)) < 0)
        return ret;

end_ready:
    c->prev_pts = av_malloc(s->nb_streams * sizeof(int64_t));
    if (!c->prev_pts)
        return AVERROR(ENOMEM);

    for (int i = 0; i < s->nb_streams; i++)
        c->prev_pts[i] = AV_NOPTS_VALUE;

    return 0;
}

static int dvdvideo_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    DVDVideoDemuxContext *c = s->priv_data;

    int ret;
    int is_key     = 0;
    int st_mapped  = 0;
    AVStream *st_subdemux;
    uint8_t  ac3_bitstream_id;
    uint16_t ac3_frame_size;

    ret = av_read_frame(c->mpeg_ctx, pkt);
    if (ret < 0) {
        if (c->subdemux_reset && ret == AVERROR_EOF) {
            c->subdemux_reset = 0;
            c->pts_offset     = c->play_state.ptm_offset;

            if ((ret = dvdvideo_subdemux_reset(s)) < 0)
                return ret;

            return FFERROR_REDO;
        }

        return ret;
    }

    st_subdemux = c->mpeg_ctx->streams[pkt->stream_index];
    is_key      = pkt->flags & AV_PKT_FLAG_KEY;

    /* map the subdemuxer stream to the parent demuxer's stream (by startcode) */
    for (int i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->id == st_subdemux->id) {
            pkt->stream_index = s->streams[i]->index;
            st_mapped         = 1;

            break;
        }
    }

    if (!st_mapped || pkt->pts == AV_NOPTS_VALUE || pkt->dts == AV_NOPTS_VALUE)
        goto discard;

    if (!c->play_started) {
        /* try to start at the beginning of a GOP */
        if (st_subdemux->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || !is_key)
            goto discard;

        c->first_pts = pkt->pts;
        c->play_started = 1;
    }

    pkt->pts += c->pts_offset - c->first_pts;
    pkt->dts += c->pts_offset - c->first_pts;

    if (pkt->pts < 0)
        goto discard;

    /* clean up after DVD muxers which end seamless PGs on duplicate or partial AC3 samples */
    if (st_subdemux->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        st_subdemux->codecpar->codec_id == AV_CODEC_ID_AC3) {

        if (pkt->pts <= c->prev_pts[pkt->stream_index])
            goto discard;

        ret = av_ac3_parse_header(pkt->buf->data, pkt->size,
                                  &ac3_bitstream_id, &ac3_frame_size);

        if (ret < 0 || pkt->size != ac3_frame_size)
            goto discard;
    }

    av_log(s, AV_LOG_TRACE, "st=%d pts=%" PRId64 " dts=%" PRId64 " "
                            "pts_offset=%" PRId64 " first_pts=%" PRId64 "\n",
                            pkt->stream_index, pkt->pts, pkt->dts,
                            c->pts_offset, c->first_pts);

    c->prev_pts[pkt->stream_index] = pkt->pts;

    return 0;

discard:
    av_log(s, st_mapped ? AV_LOG_VERBOSE : AV_LOG_DEBUG,
           "Discarding frame @ st=%d pts=%" PRId64 " dts=%" PRId64 " is_key=%d st_mapped=%d\n",
           st_mapped ? pkt->stream_index : -1, pkt->pts, pkt->dts, is_key, st_mapped);

    if (st_mapped)
        c->prev_pts[pkt->stream_index] = pkt->pts;

    return FFERROR_REDO;
}

static int dvdvideo_close(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    dvdvideo_subdemux_close(s);

    if (c->opt_menu)
        ff_dvdvideo_menu_close(s, &c->play_state);
    else
        ff_dvdvideo_play_close(s, &c->play_state);

    ff_dvdvideo_ifo_close(s);
    ff_dvdvideo_source_close(&c->source);

    if (c->prev_pts)
        av_freep(&c->prev_pts);

    return 0;
}

static int dvdvideo_read_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    DVDVideoDemuxContext *c = s->priv_data;
    int     ret;
    int64_t new_nav_pts;
    pci_t*  new_nav_pci;
    dsi_t*  new_nav_dsi;
    int     seek_failed = 0;

    if (c->opt_menu || c->opt_chapter_start > 1) {
        av_log(s, AV_LOG_ERROR, "Seeking is not compatible with menus or chapter extraction\n");

        return AVERROR_PATCHWELCOME;
    }

    if ((flags & AVSEEK_FLAG_BYTE))
        return AVERROR(ENOSYS);

    if (timestamp < 0 || timestamp > s->duration)
        return AVERROR(EINVAL);

    if (!c->seek_warned) {
        av_log(s, AV_LOG_WARNING, "Seeking is inherently unreliable and will result "
                                  "in imprecise timecodes from this point\n");
        c->seek_warned = 1;
    }

    /* dvdnav loses NAV packets when seeking on multi-angle discs, so enforce angle 1 then revert */
    if (c->nb_angles > 1) {
        if (dvdnav_angle_change(c->play_state.dvdnav, 1) != DVDNAV_STATUS_OK) {
            av_log(s, AV_LOG_ERROR, "Unable to open angle 1 for seeking\n");

            return AVERROR_EXTERNAL;
        }
    }

    /* XXX(PATCHWELCOME): use dvdnav_jump_to_sector_by_time(c->play_state.dvdnav, timestamp, 0)
     * when it is available in a released version of libdvdnav; it is more accurate */
    if (dvdnav_time_search(c->play_state.dvdnav, timestamp) != DVDNAV_STATUS_OK) {
        seek_failed = 1;
    }

    if (c->nb_angles > 1) {
        if (dvdnav_angle_change(c->play_state.dvdnav, c->opt_angle) != DVDNAV_STATUS_OK) {
            av_log(s, AV_LOG_ERROR, "Unable to revert to angle %d after seeking\n", c->opt_angle);

            return AVERROR_EXTERNAL;
        }
    }

    if (seek_failed) {
        av_log(s, AV_LOG_ERROR, "libdvdnav: seeking to %" PRId64 " failed\n", timestamp);

        return AVERROR_EXTERNAL;
    }

    new_nav_pts = dvdnav_get_current_time   (c->play_state.dvdnav);
    new_nav_pci = dvdnav_get_current_nav_pci(c->play_state.dvdnav);
    new_nav_dsi = dvdnav_get_current_nav_dsi(c->play_state.dvdnav);

    if (new_nav_pci == NULL || new_nav_dsi == NULL) {
        av_log(s, AV_LOG_ERROR, "Invalid NAV packet after seeking\n");

        return AVERROR_INVALIDDATA;
    }

    c->play_state.in_pgc      = 1;
    c->play_state.in_ps       = 0;
    c->play_state.is_seeking  = 1;
    c->play_state.nav_pts     = timestamp;
    c->play_state.ptm_offset  = timestamp;
    c->play_state.ptm_discont = 0;
    c->play_state.vobu_e_ptm  = new_nav_pci->pci_gi.vobu_s_ptm;

    /* if there are multiple angles, skip the next 3 VOBUs as dvdnav will be at the wrong angle */
    c->play_state.nb_vobu_skip = c->nb_angles > 1 ? 3 : 0;

    c->first_pts              = 0;
    c->play_started           = 0;
    c->pts_offset             = timestamp;
    c->subdemux_reset         = 0;

    if ((ret = dvdvideo_subdemux_reset(s)) < 0)
        return ret;

    av_log(s, AV_LOG_DEBUG, "seeking: requested_nav_pts=%" PRId64 " new_nav_pts=%" PRId64 "\n",
                            timestamp, new_nav_pts);

    return 0;
}


#define OFFSET(x) offsetof(DVDVideoDemuxContext, x)
static const AVOption dvdvideo_options[] = {
    {"angle",           "playback angle number",                                    OFFSET(opt_angle),          AV_OPT_TYPE_INT,    { .i64=1 },     1,          9,         AV_OPT_FLAG_DECODING_PARAM },
    {"chapter_end",     "exit chapter (PTT) number (0=end)",                        OFFSET(opt_chapter_end),    AV_OPT_TYPE_INT,    { .i64=0 },     0,          99,        AV_OPT_FLAG_DECODING_PARAM },
    {"chapter_start",   "entry chapter (PTT) number",                               OFFSET(opt_chapter_start),  AV_OPT_TYPE_INT,    { .i64=1 },     1,          99,        AV_OPT_FLAG_DECODING_PARAM },
    {"menu",            "demux menu domain",                                        OFFSET(opt_menu),           AV_OPT_TYPE_BOOL,   { .i64=0 },     0,          1,         AV_OPT_FLAG_DECODING_PARAM },
    {"menu_lu",         "menu language unit",                                       OFFSET(opt_menu_lu),        AV_OPT_TYPE_INT,    { .i64=1 },     1,          99,        AV_OPT_FLAG_DECODING_PARAM },
    {"menu_vts",        "menu VTS (0=VMG root menu)",                               OFFSET(opt_menu_vts),       AV_OPT_TYPE_INT,    { .i64=1 },     0,          99,        AV_OPT_FLAG_DECODING_PARAM },
    {"pg",              "entry PG number (when paired with PGC number)",            OFFSET(opt_pg),             AV_OPT_TYPE_INT,    { .i64=1 },     1,          255,       AV_OPT_FLAG_DECODING_PARAM },
    {"pgc",             "entry PGC number (0=auto)",                                OFFSET(opt_pgc),            AV_OPT_TYPE_INT,    { .i64=0 },     0,          999,       AV_OPT_FLAG_DECODING_PARAM },
    {"preindex",        "enable for accurate chapter markers, slow (2-pass read)",  OFFSET(opt_preindex),       AV_OPT_TYPE_BOOL,   { .i64=0 },     0,          1,         AV_OPT_FLAG_DECODING_PARAM },
    {"region",          "playback region number (0=free)",                          OFFSET(opt_region),         AV_OPT_TYPE_INT,    { .i64=0 },     0,          8,         AV_OPT_FLAG_DECODING_PARAM },
    {"title",           "title number (0=auto)",                                    OFFSET(opt_title),          AV_OPT_TYPE_INT,    { .i64=0 },     0,          99,        AV_OPT_FLAG_DECODING_PARAM },
    {"trim",            "trim padding cells from start",                            OFFSET(opt_trim),           AV_OPT_TYPE_BOOL,   { .i64=1 },     0,          1,         AV_OPT_FLAG_DECODING_PARAM },
    {"read_attempts",   "read attempts per request on the disc",                    OFFSET(opt_read_attempts),  AV_OPT_TYPE_INT,    { .i64=DISCIO_DEFAULT_ATTEMPTS }, 1, 100, AV_OPT_FLAG_DECODING_PARAM },
    {"udf_reader",      "UDF reader for disc images",                               OFFSET(opt_udf_reader),     AV_OPT_TYPE_INT,    { .i64=DISCIO_UDF_NETBSD }, DISCIO_UDF_NETBSD, DISCIO_UDF_LINUX, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
        {"netbsd",      "based on NetBSD (default)",                                0,                          AV_OPT_TYPE_CONST,  { .i64=DISCIO_UDF_NETBSD }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
        {"linux",       "based on Linux",                                           0,                          AV_OPT_TYPE_CONST,  { .i64=DISCIO_UDF_LINUX },  0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
    {"prefer_iso_old_udf102", "read UDF 1.02 images recorded before 2006 through ISO 9660 when it holds a valid DVD-Video structure",
                                                                                    OFFSET(opt_prefer_iso),     AV_OPT_TYPE_BOOL,   { .i64=1 },     0,          1,         AV_OPT_FLAG_DECODING_PARAM },
    {NULL}
};

static const AVClass dvdvideo_class = {
    .class_name = "DVD-Video demuxer",
    .item_name  = av_default_item_name,
    .option     = dvdvideo_options,
    .version    = LIBAVUTIL_VERSION_INT
};

const FFInputFormat ff_dvdvideo_demuxer = {
    .p.name         = "dvdvideo",
    .p.long_name    = NULL_IF_CONFIG_SMALL("DVD-Video"),
    .p.priv_class   = &dvdvideo_class,
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_TS_DISCONT   | AVFMT_SEEK_TO_PTS |
                      AVFMT_NOFILE   | AVFMT_NO_BYTE_SEEK | AVFMT_NOGENSEARCH | AVFMT_NOBINSEARCH,
    .priv_data_size = sizeof(DVDVideoDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_close     = dvdvideo_close,
    .read_header    = dvdvideo_read_header,
    .read_packet    = dvdvideo_read_packet,
    .read_seek      = dvdvideo_read_seek
};
