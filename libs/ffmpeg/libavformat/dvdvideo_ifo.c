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

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

    return (x > y) - (x < y);
}

int ff_dvdvideo_check_vts(void *log, dvd_reader_t *dvd, int vtsn, const ifo_handle_t *ifo)
{
    const pgcit_t *pgcit = ifo->vts_pgcit;
    const vobu_admap_t *map = ifo->vts_vobu_admap;
    uint32_t *starts = NULL;
    size_t nb = 0;
    ssize_t vob_sectors = -1;
    dvd_file_t *vobs;
    int nb_cells = 0, found = 0;

    if (!pgcit)
        return 0;
    if (map && map->vobu_start_sectors && map->last_byte + 1 >= VOBU_ADMAP_SIZE) {
        nb = (map->last_byte + 1 - VOBU_ADMAP_SIZE) / 4;
        if ((starts = av_memdup(map->vobu_start_sectors, nb * sizeof(*starts))))
            qsort(starts, nb, sizeof(*starts), cmp_u32);
    }
    if ((vobs = DVDOpenFile(dvd, vtsn, DVD_READ_TITLE_VOBS))) {
        vob_sectors = DVDFileSize(vobs);
        DVDCloseFile(vobs);
    }
    if (vob_sectors < 0)
        av_log(log, AV_LOG_WARNING, "VTS %d: the size of the title VOBs is unknown; cells are not checked "
               "against it\n", vtsn);

    for (int i = 0; i < pgcit->nr_of_pgci_srp; i++) {
        const pgc_t *pgc = pgcit->pgci_srp[i].pgc;

        if (!pgc || !pgc->cell_playback)
            continue;
        for (int j = 0; j < pgc->nr_of_cells; j++) {
            const cell_playback_t *cell = &pgc->cell_playback[j];
            uint32_t first = cell->first_sector, last = cell->last_vobu_start_sector;

            nb_cells++;
            if (starts && !(found & DVDVIDEO_VTS_MAP_DISTRUSTED)) {
                const uint32_t *missing = !bsearch(&first, starts, nb, sizeof(*starts), cmp_u32) ? &first :
                                          !bsearch(&last,  starts, nb, sizeof(*starts), cmp_u32) ? &last  : NULL;

                if (missing) {
                    av_log(log, AV_LOG_WARNING, "VTS %d: cell %d of PGC %d %s at sector %"PRIu32", which the VOBU "
                           "address map (VTS_VOBU_ADMAP) does not list as a VOBU start; the map cannot be trusted "
                           "for this title set\n", vtsn, j + 1, i + 1,
                           missing == &first ? "starts" : "has its last VOBU", *missing);
                    found |= DVDVIDEO_VTS_MAP_DISTRUSTED;
                }
            }
            if (vob_sectors >= 0 && cell->last_sector >= (uint64_t)vob_sectors &&
                !(found & DVDVIDEO_VTS_CELL_PAST_VOBS)) {
                av_log(log, AV_LOG_ERROR, "VTS %d: cell %d of PGC %d ends at sector %"PRIu32", but the title VOBs "
                       "hold only %zd sectors: video data of this cell is missing\n", vtsn, j + 1, i + 1,
                       cell->last_sector, vob_sectors);
                found |= DVDVIDEO_VTS_CELL_PAST_VOBS;
            }
        }
    }
    if (!found)
        av_log(log, AV_LOG_VERBOSE, "VTS %d: all %d cells lie inside the title VOBs (%zd sectors) and start and end "
               "on VOBUs of the VOBU address map (%zu entries)\n", vtsn, nb_cells, vob_sectors, nb);
    av_free(starts);
    return found;
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
        if (c->vts_ifo)
            ff_dvdvideo_check_vts(s, c->dvdread, c->opt_menu_vts, c->vts_ifo);

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
    ff_dvdvideo_check_vts(s, c->dvdread, title_info.title_set_nr, c->vts_ifo);

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
