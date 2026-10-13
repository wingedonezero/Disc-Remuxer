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

#include "base/str.h"
#include "base/macros.h"
#include "base/err.h"
#include "base/vlog.h"
#include "base/mem.h"
#include "base/mem.h"

#include "dvd_internal.h"

int ff_dvd_title_vobs_overlap(const DVDDisc *d, int vts)
{
    return DVDDiscTitleVOBsOverlap_rec(d->rec, vts);
}

int ff_dvd_titles_build(DVDDisc *d, const dvdnav_scan_results_t *scan, const DVDTitleOptions *o,
                        dvd_title_list_t *out)
{
    dvd_title_options_t lo = { o->cell_mode, o->cell_check, dr_random_seed(), o->min_length };

    return DVDTitleList_rec(d->rec, scan->results, scan->nb_results, &lo, out) < 0 ? DR_ERR(ENOMEM) : 0;
}

int ff_dvd_cell_check(DVDDisc *d, int vts, int pgcn, int cell, int mode)
{
    const dvd_ifo_rec_t *f = vts > 0 ? ff_dvd_set(d, vts) : NULL;
    const pgc_rec_t *p = f && f->pgcit && pgcn > 0 && pgcn <= f->pgcit->nr_of_pgcs ? f->pgcit->pgcs[pgcn - 1] : NULL;
    dvd_vob_reader_t *vr;

    if (!p || cell < 1 || cell > p->nr_of_cells)
        return DR_ERR(EINVAL);
    DVDSetCellCheck_rec(d->reader, (dvd_cell_check_t)mode, dr_random_seed());
    vr = ff_dvd_own_reading(d, vts);
    return !vr || DVDCellCheckFailed_rec(vr, p->number, cell - 1);
}

char *ff_dvd_titles_dump(const dvd_title_list_t *l)
{
    DrStr bp;

    dr_str_init(&bp);
    for (int i = 0; i < l->nb_titles; i++) {
        const dvd_title_t *t = l->titles[i];
        int64_t size = 0;
        dr_str_catf(&bp, "title %d name %s ttn %d vts %d pgc %d angle %d/%d", i, t->name, t->title + 1, t->vtsn,
                   t->pgcn + 1, t->angle + 1, t->nr_of_angles);
        if (t->nr_of_angles > 1)
            dr_str_catf(&bp, " main %d", t->main_angle);
        dr_str_catf(&bp, " cells %d chapters %d length %"PRIu32" real %"PRIu32" segments %d", t->nb_cells,
                   t->nb_chapters, t->declared, t->measured, t->segs.nb_segments);
        for (int k = 0; k < t->segs.nb_segments; k++)
            size += t->segs.segments[k].size;
        dr_str_catf(&bp, " size %"PRId64" map ", size);
        for (int k = 0; k < t->segs.nb_segments; k++)
            dr_str_catf(&bp, "%s%s", k ? "," : "", t->cell_maps[k]);
        if (!t->segs.nb_segments)
            dr_str_catf(&bp, "-");
        dr_str_catf(&bp, " scan %s tracks V", t->scan ? t->scan->name : "-");
        for (int k = 0; k < t->nb_audio; k++)
            dr_str_catf(&bp, ",A%u.%u", t->audio[k].stream, t->audio[k].attr);
        for (int k = 0; k < t->nb_subp; k++)
            dr_str_catf(&bp, ",S%u.%u", t->subp[k].stream, t->subp[k].attr);
        dr_str_catf(&bp, "\n");
    }
    return dr_str_take(&bp, 1);
}
