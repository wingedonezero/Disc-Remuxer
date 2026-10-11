/*
 * DVD-Video: the title list, built by libdvdread (dvdread/title_list.h:
 * DVDTitleList_rec(): from the disc's structure and the navigation scan's
 * recordings, the titles of every title table entry and program chain, the
 * ones that should not be there left out, their cells (CellWalk / CellTrim),
 * lengths, segments, chapters, streams and angles, duplicates removed). Here
 * the scan's recordings are handed in, what the list says is logged, and
 * the rest of the DVD format part uses its titles as they are (its lines go
 * into the log through libdvdread's logger).
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

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <dvdread/title_list.h>

#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/random_seed.h"

#include "dvd_internal.h"

int ff_dvd_title_vobs_overlap(const DVDDisc *d, int vts)
{
    return DVDDiscTitleVOBsOverlap_rec(d->rec, vts);
}

int ff_dvd_titles_build(DVDDisc *d, const dvdnav_scan_results_t *scan, const DVDTitleOptions *o,
                        dvd_title_list_t *out)
{
    dvd_title_options_t lo = { o->cell_mode, o->cell_check, av_get_random_seed(), o->min_length };

    return DVDTitleList_rec(d->rec, scan->results, scan->nb_results, &lo, out) < 0 ? AVERROR(ENOMEM) : 0;
}

int ff_dvd_cell_check(DVDDisc *d, int vts, int pgcn, int cell, int mode)
{
    const dvd_ifo_rec_t *f = vts > 0 ? ff_dvd_set(d, vts) : NULL;
    const pgc_rec_t *p = f && f->pgcit && pgcn > 0 && pgcn <= f->pgcit->nr_of_pgcs ? f->pgcit->pgcs[pgcn - 1] : NULL;
    dvd_vob_reader_t *vr;

    if (!p || cell < 1 || cell > p->nr_of_cells)
        return AVERROR(EINVAL);
    DVDSetCellCheck_rec(d->reader, (dvd_cell_check_t)mode, av_get_random_seed());
    vr = ff_dvd_own_reading(d, vts);
    return !vr || DVDCellCheckFailed_rec(vr, p->number, cell - 1);
}

char *ff_dvd_titles_dump(const dvd_title_list_t *l)
{
    AVBPrint bp;
    char *s;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (int i = 0; i < l->nb_titles; i++) {
        const dvd_title_t *t = l->titles[i];
        int64_t size = 0;
        av_bprintf(&bp, "title %d name %s ttn %d vts %d pgc %d angle %d/%d", i, t->name, t->title + 1, t->vtsn,
                   t->pgcn + 1, t->angle + 1, t->nr_of_angles);
        if (t->nr_of_angles > 1)
            av_bprintf(&bp, " main %d", t->main_angle);
        av_bprintf(&bp, " cells %d chapters %d length %"PRIu32" real %"PRIu32" segments %d", t->nb_cells,
                   t->nb_chapters, t->declared, t->measured, t->segs.nb_segments);
        for (int k = 0; k < t->segs.nb_segments; k++)
            size += t->segs.segments[k].size;
        av_bprintf(&bp, " size %"PRId64" map ", size);
        for (int k = 0; k < t->segs.nb_segments; k++)
            av_bprintf(&bp, "%s%s", k ? "," : "", t->cell_maps[k]);
        if (!t->segs.nb_segments)
            av_bprintf(&bp, "-");
        av_bprintf(&bp, " scan %s tracks V", t->scan ? t->scan->name : "-");
        for (int k = 0; k < t->nb_audio; k++)
            av_bprintf(&bp, ",A%u.%u", t->audio[k].stream, t->audio[k].attr);
        for (int k = 0; k < t->nb_subp; k++)
            av_bprintf(&bp, ",S%u.%u", t->subp[k].stream, t->subp[k].attr);
        av_bprintf(&bp, "%s\n", t->short_title ? " short" : "");
    }
    if (av_bprint_finalize(&bp, &s) < 0)
        return NULL;
    return s;
}
