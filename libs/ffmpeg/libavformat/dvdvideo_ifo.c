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
 * DVD-Video demuxer: opening the volume and the IFO structures (libdvdread)
 */

#include "dvdvideo_internal.h"

static void dvdvideo_libdvdread_log(void *opaque, dvd_logger_level_t level,
                                    const char *msg, va_list msg_va)
{
    AVFormatContext *s = opaque;
    char msg_buf[DVDVIDEO_LIBDVDX_LOG_BUFFER_SIZE];
    int lavu_level = AV_LOG_DEBUG;

    vsnprintf(msg_buf, sizeof(msg_buf), msg, msg_va);

    if (level == DVD_LOGGER_LEVEL_ERROR)
        lavu_level = AV_LOG_ERROR;
    else if (level == DVD_LOGGER_LEVEL_WARN)
        lavu_level = AV_LOG_WARNING;

    av_log(s, lavu_level, "libdvdread: %s\n", msg_buf);
}

void ff_dvdvideo_ifo_close(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    if (c->vts_ifo)
        ifoClose(c->vts_ifo);

    if (c->vmg_ifo)
        ifoClose(c->vmg_ifo);

    if (c->dvdread)
        DVDClose(c->dvdread);
}

int ff_dvdvideo_ifo_open(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    dvd_logger_cb dvdread_log_cb;
    dvd_reader_filesystem_h *files;
    title_info_t title_info;

    if (!c->source) {
        DiscIOImageOptions opts = { .udf_reader                = c->opt_udf_reader,
                                    .prefer_iso_for_old_udf102 = c->opt_prefer_iso };
        int ret = ff_dvdvideo_source_open(s, s->url, &opts, c->opt_read_attempts, &c->source);

        if (ret < 0)
            return ret;
    }

    dvdread_log_cb = (dvd_logger_cb) { .pf_log = dvdvideo_libdvdread_log };
    if (!(files = ff_dvdvideo_source_files(c->source)))
        return AVERROR(ENOMEM);
    /* on failure the files stay with the caller */
    if (!(c->dvdread = DVDOpenFiles(s, &dvdread_log_cb, "/", files)))
        files->close(files);

    if (!c->dvdread) {
        av_log(s, AV_LOG_ERROR, "Unable to open the DVD-Video structure\n");

        return AVERROR_EXTERNAL;
    }

    if (!(c->vmg_ifo = ifoOpen(c->dvdread, 0))) {
        av_log(s, AV_LOG_ERROR, "Unable to open the VMG (VIDEO_TS.IFO)\n");

        return AVERROR_EXTERNAL;
    }

    if (c->opt_menu) {
        if (c->opt_menu_vts > 0 && !(c->vts_ifo = ifoOpen(c->dvdread, c->opt_menu_vts))) {
            av_log(s, AV_LOG_ERROR, "Unable to open IFO structure for VTS %d\n", c->opt_menu_vts);

            return AVERROR_EXTERNAL;
        }

        return 0;
    }

    if (c->opt_title > c->vmg_ifo->tt_srpt->nr_of_srpts) {
        av_log(s, AV_LOG_ERROR, "Title %d not found\n", c->opt_title);

        return AVERROR_STREAM_NOT_FOUND;
    }

    title_info = c->vmg_ifo->tt_srpt->title[c->opt_title - 1];
    if (c->opt_angle > title_info.nr_of_angles) {
        av_log(s, AV_LOG_ERROR, "Angle %d not found\n", c->opt_angle);

        return AVERROR_STREAM_NOT_FOUND;
    }

    if (title_info.nr_of_ptts < 1) {
        av_log(s, AV_LOG_ERROR, "Title %d has invalid headers (no PTTs found)\n", c->opt_title);

        return AVERROR_INVALIDDATA;
    }

    if (c->opt_chapter_start > title_info.nr_of_ptts ||
       (c->opt_chapter_end > 0 && c->opt_chapter_end > title_info.nr_of_ptts)) {
        av_log(s, AV_LOG_ERROR, "Chapter (PTT) range [%d, %d] is invalid\n",
                                c->opt_chapter_start, c->opt_chapter_end);

        return AVERROR_INVALIDDATA;
    }

    if (!(c->vts_ifo = ifoOpen(c->dvdread, title_info.title_set_nr))) {
        av_log(s, AV_LOG_ERROR, "Unable to process IFO structure for VTS %d\n",
                                title_info.title_set_nr);

        return AVERROR_EXTERNAL;
    }

    if (title_info.vts_ttn < 1                                      ||
        title_info.vts_ttn > 99                                     ||
        title_info.vts_ttn > c->vts_ifo->vts_ptt_srpt->nr_of_srpts  ||
        c->vts_ifo->vtsi_mat->nr_of_vts_audio_streams > 8           ||
        c->vts_ifo->vtsi_mat->nr_of_vts_subp_streams > 32) {

        av_log(s, AV_LOG_ERROR, "Title %d has invalid headers in VTS\n", c->opt_title);
        return AVERROR_INVALIDDATA;
    }

    c->nb_angles = title_info.nr_of_angles;

    return 0;
}

int ff_dvdvideo_is_cell_promising(AVFormatContext *s, pgc_t *pgc, int celln)
{
    dvd_time_t cell_duration = pgc->cell_playback[celln - 1].playback_time;

    return cell_duration.second >= 1 || cell_duration.minute >= 1 || cell_duration.hour >= 1;
}

int ff_dvdvideo_is_pgc_promising(AVFormatContext *s, pgc_t *pgc)
{
    for (int i = 1; i <= pgc->nr_of_cells; i++)
        if (ff_dvdvideo_is_cell_promising(s, pgc, i))
            return 1;

    return 0;
}
