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
 * DVD-Video demuxer: chapter markers
 */

#include "dvdvideo_internal.h"

int ff_dvdvideo_chapters_setup_simple(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    uint64_t time_prev = 0;
    int64_t total_duration = 0;

    int chapter_start = c->opt_chapter_start;
    int chapter_end = c->opt_chapter_end > 0 ? c->opt_chapter_end : c->play_state.pgc_nb_pg_est;

    /* dvdnav_describe_title_chapters() describes PGs rather than PTTs, so validate our range */
    if (c->play_state.pgc_nb_pg_est == 1            ||
        chapter_start > c->play_state.pgc_nb_pg_est ||
        chapter_end > c->play_state.pgc_nb_pg_est) {

        s->duration = av_rescale_q(c->play_state.pgc_duration_est,
                                   DVDVIDEO_TIME_BASE_Q, AV_TIME_BASE_Q);
        return 0;
    }

    for (int i = chapter_start - 1; i < chapter_end; i++) {
        uint64_t time_effective = c->play_state.pgc_pg_times_est[i] - c->play_state.nav_pts;

        if (time_effective - time_prev == 0)
            continue;

        if (chapter_start != chapter_end &&
            !avpriv_new_chapter(s, i, DVDVIDEO_TIME_BASE_Q, time_prev, time_effective, NULL)) {

            return AVERROR(ENOMEM);
        }

        time_prev = time_effective;
        total_duration = time_effective;
    }

    if (c->opt_chapter_start == 1 && c->opt_chapter_end == 0)
        s->duration = av_rescale_q(c->play_state.pgc_duration_est,
                                   DVDVIDEO_TIME_BASE_Q, AV_TIME_BASE_Q);
    else
        s->duration = av_rescale_q(total_duration,
                                   DVDVIDEO_TIME_BASE_Q, AV_TIME_BASE_Q);

    return 0;
}

int ff_dvdvideo_chapters_setup_preindex(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    int ret, partn, last_partn;
    int interrupt = 0, nb_chapters = 0;
    uint64_t cur_chapter_offset = 0, cur_chapter_duration = 0;
    DVDVideoPlaybackState state = {0};

    uint8_t nav_buf[DVDVIDEO_BLOCK_SIZE];
    int is_nav_packet;

    if (c->opt_chapter_start == c->opt_chapter_end)
        return 0;

    if (c->opt_menu) {
        if ((ret = ff_dvdvideo_menu_open(s, &state)) < 0)
            return ret;
        last_partn = state.celln;
    } else {
        if ((ret = ff_dvdvideo_play_open(s, &state)) < 0)
            return ret;
        last_partn = c->opt_chapter_start;
    }

    if (state.pgc->nr_of_programs == 1)
        goto end_close;

    av_log(s, AV_LOG_INFO,
           "Indexing chapter markers, this will take a long time. Please wait...\n");

    while (!(interrupt = ff_check_interrupt(&s->interrupt_callback))) {
        if (c->opt_menu)
            ret = ff_dvdvideo_menu_next_ps_block(s, &state, nav_buf, DVDVIDEO_BLOCK_SIZE, &is_nav_packet);
        else
            ret = ff_dvdvideo_play_next_ps_block(s, &state, nav_buf, DVDVIDEO_BLOCK_SIZE, &is_nav_packet);

        if (ret < 0 && ret != AVERROR_EOF)
            goto end_close;

        if (!is_nav_packet && ret != AVERROR_EOF)
            continue;

        partn = c->opt_menu ? state.celln : state.ptt;

        if (partn == last_partn) {
            cur_chapter_duration += state.vobu_duration;
            /* ensure we add the last chapter */
            if (ret != AVERROR_EOF)
                continue;
        }

        if (cur_chapter_duration > 0) {
            if (!avpriv_new_chapter(s, nb_chapters, DVDVIDEO_TIME_BASE_Q, cur_chapter_offset,
                                    cur_chapter_offset + cur_chapter_duration, NULL)) {
                ret = AVERROR(ENOMEM);
                goto end_close;
            }

            nb_chapters++;
        }

        cur_chapter_offset += cur_chapter_duration;
        cur_chapter_duration = state.vobu_duration;
        last_partn = partn;

        if (ret == AVERROR_EOF)
            break;
    }

    if (interrupt) {
        ret = AVERROR_EXIT;
        goto end_close;
    }

    if (ret < 0 && ret != AVERROR_EOF)
        goto end_close;

    s->duration = av_rescale_q(state.pgc_elapsed, DVDVIDEO_TIME_BASE_Q, AV_TIME_BASE_Q);

    av_log(s, AV_LOG_INFO, "Chapter marker indexing complete\n");
    ret = 0;

end_close:
    if (c->opt_menu)
        ff_dvdvideo_menu_close(s, &state);
    else
        ff_dvdvideo_play_close(s, &state);

    return ret;
}
