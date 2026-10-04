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
 * DVD-Video demuxer: reading MPEG-PS blocks by playback through libdvdnav (titles) and from the menu VOBs
 */

#include "dvdvideo_internal.h"

static const uint8_t dvdvideo_nav_header[4] =           { 0x00, 0x00, 0x01, 0xBF };

static void dvdvideo_libdvdnav_log(void *opaque, dvdnav_logger_level_t level,
                                   const char *msg, va_list msg_va)
{
    AVFormatContext *s = opaque;
    char msg_buf[DVDVIDEO_LIBDVDX_LOG_BUFFER_SIZE];
    int lavu_level = AV_LOG_DEBUG;

    vsnprintf(msg_buf, sizeof(msg_buf), msg, msg_va);

    if (level == DVDNAV_LOGGER_LEVEL_ERROR)
        lavu_level = AV_LOG_ERROR;
    /* some discs have invalid language codes set for menus, which throws noisy warnings */
    else if (level == DVDNAV_LOGGER_LEVEL_WARN && !av_strstart(msg, "Language", NULL))
        lavu_level = AV_LOG_WARNING;

    av_log(s, lavu_level, "libdvdnav: %s\n", msg_buf);
}

void ff_dvdvideo_menu_close(AVFormatContext *s, DVDVideoPlaybackState *state)
{
    if (state->vob_file)
        DVDCloseFile(state->vob_file);
}

int ff_dvdvideo_menu_open(AVFormatContext *s, DVDVideoPlaybackState *state)
{
    DVDVideoDemuxContext *c = s->priv_data;
    pgci_ut_t *pgci_ut;

    pgci_ut = c->opt_menu_vts ? c->vts_ifo->pgci_ut : c->vmg_ifo->pgci_ut;
    if (!pgci_ut) {
        av_log(s, AV_LOG_ERROR, "Invalid PGC table for menu [LU %d, PGC %d]\n",
                                c->opt_menu_lu, c->opt_pgc);

        return AVERROR_INVALIDDATA;
    }

    if (c->opt_pgc < 1                      ||
        c->opt_menu_lu < 1                  ||
        c->opt_menu_lu > pgci_ut->nr_of_lus ||
        c->opt_pgc > pgci_ut->lu[c->opt_menu_lu - 1].pgcit->nr_of_pgci_srp) {

        av_log(s, AV_LOG_ERROR, "Menu [LU %d, PGC %d] not found\n", c->opt_menu_lu, c->opt_pgc);

        return AVERROR(EINVAL);
    }

    /* make sure the PGC is valid */
    state->pgcn          = c->opt_pgc;
    state->pgc           = pgci_ut->lu[c->opt_menu_lu - 1].pgcit->pgci_srp[c->opt_pgc - 1].pgc;
    if (!state->pgc || !state->pgc->program_map || !state->pgc->cell_playback) {
        av_log(s, AV_LOG_ERROR, "Invalid PGC structure for menu [LU %d, PGC %d]\n",
                                c->opt_menu_lu, c->opt_pgc);

        return AVERROR_INVALIDDATA;
    }

    /* make sure the PG is valid */
    state->entry_pgn     = c->opt_pg;
    if (state->entry_pgn < 1 || state->entry_pgn > state->pgc->nr_of_programs) {
        av_log(s, AV_LOG_ERROR, "Entry PG %d not found\n", state->entry_pgn);

        return AVERROR(EINVAL);
    }

    /* make sure the program map isn't leading us to nowhere */
    state->celln_start   = state->pgc->program_map[state->entry_pgn - 1];
    state->celln_end     = state->pgc->nr_of_cells;
    state->celln         = state->celln_start;
    if (state->celln_start > state->pgc->nr_of_cells) {
        av_log(s, AV_LOG_ERROR, "Invalid PGC structure: program map points to unknown cell\n");

        return AVERROR_INVALIDDATA;
    }

    state->sector_end    = state->pgc->cell_playback[state->celln - 1].last_sector;
    state->vobu_next     = state->pgc->cell_playback[state->celln - 1].first_sector;
    state->sector_offset = state->vobu_next;

    if (c->opt_menu_vts > 0)
        state->in_vts    = 1;

    if (!(state->vob_file = DVDOpenFile(c->dvdread, c->opt_menu_vts, DVD_READ_MENU_VOBS))) {
        av_log(s, AV_LOG_ERROR, !c->opt_menu_vts ?
                                "Unable to open main menu VOB (VIDEO_TS.VOB)\n" :
                                "Unable to open menu VOBs for VTS %d\n", c->opt_menu_vts);

        return AVERROR_EXTERNAL;
    }

    return 0;
}

int ff_dvdvideo_menu_next_ps_block(AVFormatContext *s, DVDVideoPlaybackState *state,
                                       uint8_t *buf, int buf_size, int *p_is_nav_packet)
{
    int64_t blocks_read                   = 0;
    uint8_t read_buf[DVDVIDEO_BLOCK_SIZE] = {0};
    pci_t pci                             = (pci_t) {0};
    dsi_t dsi                             = (dsi_t) {0};

    (*p_is_nav_packet)  = 0;
    state->ptm_discont  = 0;

    if (buf_size != DVDVIDEO_BLOCK_SIZE) {
        av_log(s, AV_LOG_ERROR, "Invalid buffer size (expected=%d actual=%d)\n",
                                DVDVIDEO_BLOCK_SIZE, buf_size);

        return AVERROR(EINVAL);
    }

    /* we were at the end of a vobu, so now go to the next one or EOF */
    if (!state->vobu_remaining && state->in_pgc) {
        if (state->vobu_next == SRI_END_OF_CELL) {
            if (state->celln == state->celln_end && state->sector_offset > state->sector_end)
                return AVERROR_EOF;

            state->celln++;
            state->sector_offset = state->pgc->cell_playback[state->celln - 1].first_sector;
            state->sector_end    = state->pgc->cell_playback[state->celln - 1].last_sector;
        } else {
            state->sector_offset = state->vobu_next;
        }
    }

    /* continue reading the VOBU */
    av_log(s, AV_LOG_TRACE, "reading block at offset %d\n", state->sector_offset);

    blocks_read = DVDReadBlocks(state->vob_file, state->sector_offset, 1, read_buf);
    if (blocks_read != 1) {
        av_log(s, AV_LOG_ERROR, "Unable to read VOB block: offset=%d blocks_read=%" PRId64 "\n",
                                state->sector_offset, blocks_read);

        return AVERROR_INVALIDDATA;
    }

    /* we are at the start of a VOBU, so we are expecting a NAV packet */
    if (!state->vobu_remaining) {
        if (!memcmp(&read_buf[PCI_START_BYTE - 4], dvdvideo_nav_header, 4) ||
            !memcmp(&read_buf[DSI_START_BYTE - 4], dvdvideo_nav_header, 4) ||
            read_buf[PCI_START_BYTE - 1] != 0x00                           ||
            read_buf[DSI_START_BYTE - 1] != 0x01) {

            av_log(s, AV_LOG_ERROR, "Invalid NAV packet at offset %d: PCI or DSI header mismatch\n",
                                    state->sector_offset);

            return AVERROR_INVALIDDATA;
        }

        navRead_PCI(&pci, &read_buf[PCI_START_BYTE]);
        navRead_DSI(&dsi, &read_buf[DSI_START_BYTE]);

        if (!pci.pci_gi.vobu_s_ptm                          ||
            !pci.pci_gi.vobu_e_ptm                          ||
            pci.pci_gi.vobu_s_ptm > pci.pci_gi.vobu_e_ptm) {

            av_log(s, AV_LOG_ERROR, "Invalid NAV packet at offset %d: PCI header is invalid\n",
                                    state->sector_offset);

            return AVERROR_INVALIDDATA;
        }

        state->vobu_remaining    = dsi.dsi_gi.vobu_ea;
        state->vobu_next         = dsi.vobu_sri.next_vobu == SRI_END_OF_CELL ? SRI_END_OF_CELL :
                                   dsi.dsi_gi.nv_pck_lbn + (dsi.vobu_sri.next_vobu & 0x7FFFFFFF);
        state->sector_offset++;

        if (state->in_pgc) {
            if (state->vobu_e_ptm != pci.pci_gi.vobu_s_ptm) {
                state->ptm_discont  = 1;
                state->ptm_offset  += state->vobu_e_ptm - pci.pci_gi.vobu_s_ptm;
            }
        } else {
            state->in_pgc        = 1;
            state->in_ps         = 1;
        }

        state->vobu_e_ptm        = pci.pci_gi.vobu_e_ptm;
        state->vobu_duration     = pci.pci_gi.vobu_e_ptm - pci.pci_gi.vobu_s_ptm;

        av_log(s, AV_LOG_DEBUG, "NAV packet: sector=%d "
                                "vobu_s_ptm=%d vobu_e_ptm=%d ptm_offset=%" PRId64 "\n",
                                dsi.dsi_gi.nv_pck_lbn,
                                pci.pci_gi.vobu_s_ptm, pci.pci_gi.vobu_e_ptm, state->ptm_offset);


        (*p_is_nav_packet) = 1;

        return 0;
    }

    /* we are in the middle of a VOBU, so pass on the PS packet */
    memcpy(buf, &read_buf, DVDVIDEO_BLOCK_SIZE);
    state->sector_offset++;
    state->vobu_remaining--;

    return DVDVIDEO_BLOCK_SIZE;
}

void ff_dvdvideo_play_close(AVFormatContext *s, DVDVideoPlaybackState *state)
{
    if (!state->dvdnav)
        return;

    /* not allocated by av_malloc() */
    if (state->pgc_pg_times_est)
        free(state->pgc_pg_times_est);

    if (dvdnav_close(state->dvdnav) != DVDNAV_STATUS_OK)
        av_log(s, AV_LOG_ERROR, "Unable to close dvdnav successfully, dvdnav error: %s\n",
                                dvdnav_err_to_string(state->dvdnav));
}

int ff_dvdvideo_play_open(AVFormatContext *s, DVDVideoPlaybackState *state)
{
    DVDVideoDemuxContext *c = s->priv_data;

    dvdnav_logger_cb dvdnav_log_cb;
    dvdnav_status_t dvdnav_open_status;
    int32_t disc_region_mask;
    int32_t player_region_mask;
    int cur_title, cur_pgcn, cur_pgn;
    pgc_t *pgc;

    dvdnav_log_cb = (dvdnav_logger_cb) { .pf_log = dvdvideo_libdvdnav_log };
    dvdnav_open_status = dvdnav_open2(&state->dvdnav, s, &dvdnav_log_cb, s->url);

    if (!state->dvdnav                                                          ||
        dvdnav_open_status != DVDNAV_STATUS_OK                                  ||
        dvdnav_set_readahead_flag(state->dvdnav, 0) != DVDNAV_STATUS_OK         ||
        dvdnav_set_PGC_positioning_flag(state->dvdnav, 1) != DVDNAV_STATUS_OK   ||
        dvdnav_get_region_mask(state->dvdnav, &disc_region_mask) != DVDNAV_STATUS_OK) {

        av_log(s, AV_LOG_ERROR, "Unable to open the DVD for playback\n");
        goto end_dvdnav_error;
    }

    player_region_mask = c->opt_region > 0 ? (1 << (c->opt_region - 1)) : disc_region_mask;
    if (dvdnav_set_region_mask(state->dvdnav, player_region_mask) != DVDNAV_STATUS_OK) {
        av_log(s, AV_LOG_ERROR, "Unable to set the playback region code %d\n", c->opt_region);

        goto end_dvdnav_error;
    }

    if (c->opt_pgc > 0) {
        if (dvdnav_program_play(state->dvdnav, c->opt_title, c->opt_pgc, c->opt_pg) != DVDNAV_STATUS_OK) {
            av_log(s, AV_LOG_ERROR, "Unable to start playback at title %d, PGC %d, PG %d\n",
                                    c->opt_title, c->opt_pgc, c->opt_pg);

            goto end_dvdnav_error;
        }

        state->pgcn = c->opt_pgc;
        state->entry_pgn = c->opt_pg;
    } else {
        if (dvdnav_part_play(state->dvdnav, c->opt_title, c->opt_chapter_start) != DVDNAV_STATUS_OK ||
            dvdnav_current_title_program(state->dvdnav, &cur_title, &cur_pgcn, &cur_pgn) != DVDNAV_STATUS_OK) {

            av_log(s, AV_LOG_ERROR, "Unable to start playback at title %d, chapter (PTT) %d\n",
                                    c->opt_title, c->opt_chapter_start);
            goto end_dvdnav_error;
        }

        state->pgcn = cur_pgcn;
        state->entry_pgn = cur_pgn;
    }

    pgc = c->vts_ifo->vts_pgcit->pgci_srp[state->pgcn - 1].pgc;

    if (pgc->pg_playback_mode != 0) {
        av_log(s, AV_LOG_ERROR, "Non-sequential PGCs, such as shuffles, are not supported\n");

        return AVERROR_PATCHWELCOME;
    }

    if (c->opt_trim && !ff_dvdvideo_is_pgc_promising(s, pgc)) {
        av_log(s, AV_LOG_ERROR, "Title %d, PGC %d looks empty (may consist of padding cells), "
                                "if you want to try anyway, disable the -trim option\n",
                                c->opt_title, state->pgcn);

        return AVERROR_INVALIDDATA;
    }

    if (dvdnav_angle_change(state->dvdnav, c->opt_angle) != DVDNAV_STATUS_OK) {
        av_log(s, AV_LOG_ERROR, "Unable to start playback at angle %d\n", c->opt_angle);

        goto end_dvdnav_error;
    }

    /* dvdnav_describe_title_chapters() performs several validations on the title structure */
    /* take advantage of this side effect to increase chances of a safe navigation path */
    state->pgc_nb_pg_est = dvdnav_describe_title_chapters(state->dvdnav, c->opt_title,
                                                          &state->pgc_pg_times_est,
                                                          &state->pgc_duration_est);

    /* dvdnav returning 0 PGs is documented as an error condition */
    if (!state->pgc_nb_pg_est) {
        av_log(s, AV_LOG_ERROR, "Unable to read chapter information for title %d\n", c->opt_title);

        goto end_dvdnav_error;
    }

    state->nav_pts = dvdnav_get_current_time(state->dvdnav);
    state->vtsn = c->vmg_ifo->tt_srpt->title[c->opt_title - 1].title_set_nr;
    state->pgc = pgc;

    return 0;

end_dvdnav_error:
    if (state->dvdnav)
        av_log(s, AV_LOG_ERROR, "dvdnav error: %s\n", dvdnav_err_to_string(state->dvdnav));
    else
        av_log(s, AV_LOG_ERROR, "dvdnav could not be initialized\n");

    return AVERROR_EXTERNAL;
}

int ff_dvdvideo_play_next_ps_block(AVFormatContext *s, DVDVideoPlaybackState *state,
                                       uint8_t *buf, int buf_size, int *p_is_nav_packet)
{
    DVDVideoDemuxContext *c = s->priv_data;

    uint8_t nav_buf[DVDVIDEO_BLOCK_SIZE] = {0};
    int nav_event;
    int nav_len;

    dvdnav_vts_change_event_t *e_vts;
    dvdnav_cell_change_event_t *e_cell;
    int cur_title, cur_pgcn, cur_pgn, cur_angle, cur_title_unused, cur_ptt, cur_nb_angles;
    pci_t *e_pci;
    dsi_t *e_dsi;

    (*p_is_nav_packet)  = 0;
    state->ptm_discont  = 0;

    if (buf_size != DVDVIDEO_BLOCK_SIZE) {
        av_log(s, AV_LOG_ERROR, "Invalid buffer size (expected=%d actual=%d)\n",
                                DVDVIDEO_BLOCK_SIZE, buf_size);

        return AVERROR(EINVAL);
    }

    for (int i = 0; i < DVDVIDEO_MAX_PS_SEARCH_BLOCKS; i++) {
        if (ff_check_interrupt(&s->interrupt_callback))
            return AVERROR_EXIT;

        if (dvdnav_get_next_block(state->dvdnav, nav_buf, &nav_event, &nav_len) != DVDNAV_STATUS_OK) {
            av_log(s, AV_LOG_ERROR, "Unable to read next block of PGC\n");

            goto end_dvdnav_error;
        }

        /* STOP event can come at any time and should be honored */
        if (nav_event == DVDNAV_STOP)
            return AVERROR_EOF;

        if (nav_len > DVDVIDEO_BLOCK_SIZE) {
            av_log(s, AV_LOG_ERROR, "Invalid block size (expected<=%d actual=%d)\n",
                                    DVDVIDEO_BLOCK_SIZE, nav_len);

            return AVERROR_INVALIDDATA;
        }

        if (dvdnav_current_title_info(state->dvdnav, &cur_title, &cur_ptt) != DVDNAV_STATUS_OK) {
            av_log(s, AV_LOG_ERROR, "Unable to determine current title coordinates\n");

            goto end_dvdnav_error;
        }

        /* we somehow navigated to a menu */
        if (cur_title == 0 || !dvdnav_is_domain_vts(state->dvdnav))
            return AVERROR_EOF;

        if (dvdnav_current_title_program(state->dvdnav, &cur_title_unused, &cur_pgcn, &cur_pgn) != DVDNAV_STATUS_OK) {
            av_log(s, AV_LOG_ERROR, "Unable to determine current PGC coordinates\n");

            goto end_dvdnav_error;
        }

        /* we somehow left the PGC */
        if (state->in_pgc && cur_pgcn != state->pgcn)
            return AVERROR_EOF;

        if (dvdnav_get_angle_info(state->dvdnav, &cur_angle, &cur_nb_angles) != DVDNAV_STATUS_OK) {
            av_log(s, AV_LOG_ERROR, "Unable to determine current video angle\n");

            goto end_dvdnav_error;
        }

        av_log(s, nav_event == DVDNAV_BLOCK_OK ? AV_LOG_TRACE : AV_LOG_DEBUG,
               "new block: i=%d nav_event=%d nav_len=%d cur_title=%d "
               "cur_ptt=%d cur_angle=%d cur_celln=%d cur_pgcn=%d cur_pgn=%d "
               "play_in_vts=%d play_in_pgc=%d play_in_ps=%d\n",
               i, nav_event, nav_len, cur_title,
               cur_ptt, cur_angle, state->celln, cur_pgcn, cur_pgn,
               state->in_vts, state->in_pgc, state->in_ps);

        switch (nav_event) {
            case DVDNAV_VTS_CHANGE:
                if (state->in_vts)
                    return AVERROR_EOF;

                e_vts = (dvdnav_vts_change_event_t *) nav_buf;

                if (e_vts->new_vtsN == state->vtsn && e_vts->new_domain == DVD_DOMAIN_VTSTitle)
                    state->in_vts = 1;

                continue;
            case DVDNAV_CELL_CHANGE:
                if (!state->in_vts)
                    continue;

                e_cell = (dvdnav_cell_change_event_t *) nav_buf;

                av_log(s, AV_LOG_DEBUG, "new cell: prev=%d new=%d\n", state->celln, e_cell->cellN);

                if (!state->in_ps && !state->in_pgc) {
                    if (cur_title == c->opt_title                        &&
                        (c->opt_pgc || cur_ptt == c->opt_chapter_start)  &&
                        cur_pgcn == state->pgcn                          &&
                        cur_pgn == state->entry_pgn) {

                        state->in_pgc = 1;
                    }
                } else if (!state->is_seeking &&
                           (state->celln >= e_cell->cellN || state->pgn > cur_pgn)) {
                    return AVERROR_EOF;
                }

                state->celln = e_cell->cellN;
                state->ptt = cur_ptt;
                state->pgn = cur_pgn;

                continue;
            case DVDNAV_NAV_PACKET:
                if (!state->in_pgc)
                    continue;

                if ((!state->is_seeking && state->ptt > 0 && state->ptt > cur_ptt) ||
                    (c->opt_chapter_end > 0 && cur_ptt > c->opt_chapter_end)) {
                    return AVERROR_EOF;
                }

                if (nav_len != DVDVIDEO_BLOCK_SIZE) {
                    av_log(s, AV_LOG_ERROR, "Invalid NAV packet size (expected=%d actual=%d)\n",
                                            DVDVIDEO_BLOCK_SIZE, nav_len);

                    return AVERROR_INVALIDDATA;
                }

                e_pci = dvdnav_get_current_nav_pci(state->dvdnav);
                e_dsi = dvdnav_get_current_nav_dsi(state->dvdnav);

                if (e_pci == NULL || e_dsi == NULL ||
                    e_pci->pci_gi.vobu_s_ptm > e_pci->pci_gi.vobu_e_ptm) {

                    av_log(s, AV_LOG_ERROR, "Invalid NAV packet\n");
                    return AVERROR_INVALIDDATA;
                }

                if (state->nb_vobu_skip > 0) {
                    av_log(s, AV_LOG_VERBOSE, "Skipping VOBU at SCR %d\n",
                                              e_dsi->dsi_gi.nv_pck_scr);
                    state->nb_vobu_skip -= 1;
                    continue;
                }

                state->vobu_duration = e_pci->pci_gi.vobu_e_ptm - e_pci->pci_gi.vobu_s_ptm;
                state->pgc_elapsed += state->vobu_duration;
                state->nav_pts = dvdnav_get_current_time(state->dvdnav);
                state->ptt = cur_ptt;
                state->pgn = cur_pgn;

                av_log(s, AV_LOG_DEBUG,
                       "NAV packet: s_ptm=%d e_ptm=%d "
                       "scr=%d lbn=%d vobu_duration=%d nav_pts=%" PRId64 "\n",
                       e_pci->pci_gi.vobu_s_ptm, e_pci->pci_gi.vobu_e_ptm,
                       e_dsi->dsi_gi.nv_pck_scr,
                       e_pci->pci_gi.nv_pck_lbn, state->vobu_duration, state->nav_pts);

                if (!state->in_ps) {
                    if (c->opt_trim && !ff_dvdvideo_is_cell_promising(s, state->pgc, state->celln)) {
                        av_log(s, AV_LOG_INFO, "Skipping padding cell #%d\n", state->celln);

                        i = 0;
                        continue;
                    }

                    av_log(s, AV_LOG_DEBUG, "navigation: locked to program stream\n");

                    state->in_ps = 1;
                } else {
                    if (state->vobu_e_ptm != e_pci->pci_gi.vobu_s_ptm) {
                        state->ptm_discont  = 1;
                        state->ptm_offset  += state->vobu_e_ptm - e_pci->pci_gi.vobu_s_ptm;
                    }
                }

                state->vobu_e_ptm = e_pci->pci_gi.vobu_e_ptm;

                (*p_is_nav_packet) = 1;

                return 0;
            case DVDNAV_BLOCK_OK:
                if (!state->in_ps) {
                    if (state->in_pgc)
                        i = 0; /* necessary in case we are skipping junk cells at the beginning */
                    continue;
                }

                if (nav_len != DVDVIDEO_BLOCK_SIZE) {
                    av_log(s, AV_LOG_ERROR, "Invalid MPEG block size (expected=%d actual=%d)\n",
                                            DVDVIDEO_BLOCK_SIZE, nav_len);

                    return AVERROR_INVALIDDATA;
                }

                if (cur_angle != c->opt_angle) {
                    av_log(s, AV_LOG_ERROR, "Unexpected angle change (expected=%d new=%d)\n",
                                            c->opt_angle, cur_angle);

                    return AVERROR_INPUT_CHANGED;
                }

                if (state->pgn != cur_pgn)
                    av_log(s, AV_LOG_WARNING, "Unexpected PG change (expected=%d actual=%d); "
                                              "this could be due to a missed NAV packet\n",
                                              state->pgn, cur_pgn);

                memcpy(buf, &nav_buf, nav_len);

                state->is_seeking = 0;

                return nav_len;
            case DVDNAV_WAIT:
                if (dvdnav_wait_skip(state->dvdnav) != DVDNAV_STATUS_OK) {
                    av_log(s, AV_LOG_ERROR, "Unable to skip WAIT event\n");

                    goto end_dvdnav_error;
                }

                continue;
            case DVDNAV_STILL_FRAME:
            case DVDNAV_HOP_CHANNEL:
            case DVDNAV_HIGHLIGHT:
                if (state->in_ps)
                    return AVERROR_EOF;

                if (nav_event == DVDNAV_STILL_FRAME) {
                    if (dvdnav_still_skip(state->dvdnav) != DVDNAV_STATUS_OK) {
                        av_log(s, AV_LOG_ERROR, "Unable to skip still image\n");

                        goto end_dvdnav_error;
                    }
                }

                continue;
            default:
                continue;
        }
    }

    av_log(s, AV_LOG_ERROR, "Unable to find next program stream block\n");

    return AVERROR_INVALIDDATA;

end_dvdnav_error:
    av_log(s, AV_LOG_ERROR, "dvdnav error (title=%d pgc=%d pg=%d cell=%d): %s\n",
                            cur_title, cur_pgcn, cur_pgn, state->celln,
                            dvdnav_err_to_string(state->dvdnav));

    return AVERROR_EXTERNAL;
}
