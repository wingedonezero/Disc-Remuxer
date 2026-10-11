/*
 * DVD-Video: the title list, built by libdvdread (dvdread/title_list.h:
 * DVDTitleList_rec(): from the disc's structure and the navigation scan's
 * recordings, the titles of every title table entry and program chain, the
 * ones that should not be there left out, their cells (CellWalk / CellTrim),
 * lengths, segments, chapters, streams and angles, duplicates removed). Here
 * the scan's recordings are handed in, what the list says is logged, and
 * its titles are copied for the rest of the DVD format part.
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

/* What the title list says: libdvdread's levels info, detail, warning, debug. */
static void list_note(void *opaque, dvd_note_level_t level, const char *text)
{
    static const int levels[] = { AV_LOG_INFO, AV_LOG_VERBOSE, AV_LOG_WARNING, AV_LOG_DEBUG };
    DVDDisc *d = opaque;
    av_log(d->log, levels[FFMIN((unsigned)level, 3)], "DVD: %s\n", text);
}

void ff_dvd_title_free(DVDTitle **pt)
{
    DVDTitle *t = *pt;
    if (!t)
        return;
    for (int k = 0; k < t->nb_segments; k++) {
        av_free(t->segments[k].extents);
        av_free(t->segments[k].runs);
        av_free(t->segments[k].cell_map);
    }
    av_free(t->segments);
    av_free(t->chapters);
    av_free(t->cells);
    av_freep(pt);
}

void ff_dvd_titles_free(DVDTitleList *l)
{
    for (int i = 0; i < l->nb_titles; i++)
        ff_dvd_title_free(&l->titles[i]);
    av_freep(&l->titles);
    l->nb_titles = 0;
}

/* libdvdread's title as ours (scan: the recordings it points into, the
 * scan's results at the same places). */
static int copy_title(DVDDisc *d, const DVDScan *scan, const dvd_nav_cells_t *nav, const dvd_title_t *a,
                      DVDTitle **out)
{
    DVDTitle *t;

    *out = NULL;
    if (!(t = av_mallocz(sizeof(*t))))
        return AVERROR(ENOMEM);
    *out = t;
    memcpy(t->name, a->name, sizeof(t->name));
    t->title        = a->title;
    t->pb_ty        = a->pb_ty;
    t->vtsn         = a->vtsn;
    t->vts_ttn      = a->vts_ttn;
    t->nr_of_angles = a->nr_of_angles;
    t->angle        = a->angle;
    t->vts          = d->ifos[a->vtsn];
    t->pgc          = t->vts->pgcs[a->pgcn];
    t->scan         = a->scan ? &scan->results[a->scan - nav] : NULL;
    t->declared     = a->declared;
    t->measured     = a->measured;
    t->built        = a->built;
    t->nav_invalid  = a->nav_invalid;
    t->short_title  = a->short_title;
    t->nb_audio     = a->nb_audio;
    t->audio_listed = a->audio_listed;
    t->nb_subp      = a->nb_subp;
    t->subp_width   = a->subp_width;
    t->subp_height  = a->subp_height;
    for (int i = 0; i < a->nb_audio; i++)
        t->audio[i] = (DVDStreamRef){ a->audio[i].stream, a->audio[i].attr };
    for (int i = 0; i < a->nb_subp; i++)
        t->subp[i] = (DVDStreamRef){ a->subp[i].stream, a->subp[i].attr };
    if (!(t->chapters = av_malloc_array(FFMAX(a->nb_chapters, 1), sizeof(*t->chapters))) ||
        !(t->cells = av_malloc_array(FFMAX(a->nb_cells, 1), sizeof(*t->cells))))
        return AVERROR(ENOMEM);
    for (int k = 0; k < a->nb_chapters; k++) {
        const dvd_title_chapter_t *c = &a->chapters[k];
        t->chapters[k] = (DVDChapter){ c->pgcn, c->pgn, c->cell,
                                       c->segment == DVD_TITLE_CHAPTER_NONE ? DVD_CHAPTER_NONE : c->segment,
                                       c->offset, c->time };
    }
    t->nb_chapters = a->nb_chapters;
    memcpy(t->cells, a->cells, a->nb_cells * sizeof(*t->cells));
    t->nb_cells = t->alloc_cells = a->nb_cells;
    if (a->segs.nb_segments && !(t->segments = av_calloc(a->segs.nb_segments, sizeof(*t->segments))))
        return AVERROR(ENOMEM);
    t->nb_segments = t->alloc_segments = a->segs.nb_segments;
    for (int k = 0; k < a->segs.nb_segments; k++) {
        const dvd_segment_t *s = &a->segs.segments[k];
        DVDSegment *g = &t->segments[k];
        if ((s->nb_extents && !(g->extents = av_malloc_array(s->nb_extents, sizeof(*g->extents)))) ||
            (s->nb_runs && !(g->runs = av_malloc_array(s->nb_runs, sizeof(*g->runs)))) ||
            !(g->cell_map = av_strdup(a->cell_maps[k])))
            return AVERROR(ENOMEM);
        for (int e = 0; e < s->nb_extents; e++)
            g->extents[e] = (DVDExtent){ s->extents[e].logical, s->extents[e].sector, s->extents[e].count };
        if (s->nb_runs)
            memcpy(g->runs, s->runs, s->nb_runs * sizeof(*g->runs));
        g->alloc_extents = g->nb_extents = s->nb_extents;
        g->alloc_runs    = g->nb_runs    = s->nb_runs;
        g->last_cell     = s->last_cell;
        g->size          = s->size;
    }
    return 0;
}

int ff_dvd_titles_build(DVDDisc *d, const DVDScan *scan, const DVDTitleOptions *o, DVDTitleList *out)
{
    dvd_title_options_t lo = { o->cell_mode, o->cell_check, av_get_random_seed(), o->min_length, list_note, d };
    dvd_title_list_t list = { 0 };
    dvd_nav_cells_t *nav;
    int ret = 0;

    memset(out, 0, sizeof(*out));
    if (!(nav = av_calloc(FFMAX(scan->nb_results, 1), sizeof(*nav))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < scan->nb_results; i++) {
        const DVDScanResult *r = &scan->results[i];
        memcpy(nav[i].name, r->name, sizeof(nav[i].name));
        nav[i].title    = r->title;
        nav[i].pgc      = r->pgc;
        nav[i].nb_cells = r->nb_cells;
        nav[i].cells    = r->cells;
    }
    if (DVDTitleList_rec(d->rec, nav, scan->nb_results, &lo, &list) < 0) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if (list.nb_titles && !(out->titles = av_calloc(list.nb_titles, sizeof(*out->titles)))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < list.nb_titles; i++) {
        ret = copy_title(d, scan, nav, list.titles[i], &out->titles[i]);
        out->nb_titles += !!out->titles[i];
        if (ret < 0)
            goto end;
    }
end:
    if (ret < 0)
        ff_dvd_titles_free(out);
    DVDFreeTitleList_rec(&list);
    av_free(nav);
    return ret;
}

int ff_dvd_cell_check(DVDDisc *d, int vts, int pgcn, int cell, int mode)
{
    DVDIfo *f = vts > 0 && vts < d->nb_ifos ? d->ifos[vts] : NULL;
    DVDPgc *p = f && !f->failed && pgcn > 0 && pgcn <= f->nb_pgcs ? f->pgcs[pgcn - 1] : NULL;
    dvd_vob_reader_t *vr;

    if (!p || cell < 1 || cell > p->nr_of_cells)
        return AVERROR(EINVAL);
    DVDSetCellCheck_rec(d->reader, (dvd_cell_check_t)mode, av_get_random_seed());
    vr = ff_dvd_own_reading(d, f);
    return !vr || DVDCellCheckFailed_rec(vr, p->number, cell - 1);
}

char *ff_dvd_titles_dump(const DVDTitleList *l)
{
    AVBPrint bp;
    char *s;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (int i = 0; i < l->nb_titles; i++) {
        const DVDTitle *t = l->titles[i];
        int64_t size = 0;
        av_bprintf(&bp, "title %d name %s ttn %d vts %d pgc %d angle %d/%d cells %d chapters %d length %"PRIu32
                   " real %"PRIu32" segments %d", i, t->name, t->title + 1, t->vtsn, t->pgc->number + 1,
                   t->angle + 1, t->nr_of_angles, t->nb_cells, t->nb_chapters, t->declared, t->measured,
                   t->nb_segments);
        for (int k = 0; k < t->nb_segments; k++)
            size += t->segments[k].size;
        av_bprintf(&bp, " size %"PRId64" map ", size);
        for (int k = 0; k < t->nb_segments; k++)
            av_bprintf(&bp, "%s%s", k ? "," : "", t->segments[k].cell_map);
        if (!t->nb_segments)
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
