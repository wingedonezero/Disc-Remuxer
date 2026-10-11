/*
 * DVD-Video: one information file (VIDEO_TS.IFO or VTS_nn_0.IFO) as
 * libdvdread read and checked it (dvdread/disc_read.h: which copy of it,
 * its tables, where its VOBs start, whether its VOBU address map is
 * trusted), copied for the rest of the DVD format part, and the VOB files
 * of a disc folder's set (the menu VOB, the title VOBs VTS_nn_1..9.VOB up
 * to the first missing one; VIDEO_TS: VIDEO_TS.VOB) for the navigator's
 * reads.
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

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>

#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"


/* ---- files ---- */

const DVDDiscFile *ff_dvd_disc_file(DVDDisc *d, int set, int slot)
{
    static const char *const ext[3] = { "IFO", "BUP", "VOB" };
    DVDDiscFile *r;
    char path[64];
    int64_t at;

    if (set < 0 || set > DVD_MAX_TITLE_SETS || slot < 0 || slot >= DVD_FILE_SLOTS || (!set && slot > 2))
        return NULL;
    r = &d->files[set][slot];
    if (!r->looked) {
        r->looked = 1;
        if (set)
            snprintf(path, sizeof(path), "%s/VTS_%02d_%d.%s", d->dir, set, slot < 3 ? 0 : slot - 2,
                     ext[FFMIN(slot, 2)]);
        else
            snprintf(path, sizeof(path), "%s/VIDEO_TS.%s", d->dir, ext[slot]);
        if (d->fs->ops->open_file(d->fs, path, &r->file) < 0)
            r->file = NULL;
        /* an image's IFO / BUP in one piece: read as the image from its place
         * (its tables are addressed by sector from there) */
        if (r->file && !d->folder && slot < 2 && r->file->nb_extents == 1 && !r->file->data &&
            (at = r->file->extents[0].sector) != DISCIO_SECTOR_NOT_RECORDED) {
            int64_t blocks = (d->fs->src->size + DVD_SECTOR - 1) / DVD_SECTOR;
            DiscIOFile *g;
            if (at < blocks && (g = av_mallocz(sizeof(*g)))) {
                if ((g->extents = av_mallocz(sizeof(*g->extents)))) {
                    g->nb_extents        = 1;
                    g->extents[0].sector = at;
                    g->extents[0].count  = blocks - at;
                    g->size              = g->extents[0].count * DVD_SECTOR;
                    r->region = g;
                } else {
                    av_free(g);
                }
            }
        }
    }
    return r->file ? r : NULL;
}

int ff_dvd_ifo_files(DVDDisc *d, DVDIfo *f)
{
    const DVDDiscFile *r;

    if (!d->folder)
        return 0;
    if ((r = ff_dvd_disc_file(d, f->index, 2))) {
        f->menu_vob       = r->file;
        f->menu_vob_bytes = r->file->size;
    }
    for (int part = 1; f->index && part <= 9 && (r = ff_dvd_disc_file(d, f->index, 2 + part)); part++) {
        f->title_vobs[f->nb_title_vobs++] = r->file;
        f->title_vob_bytes += r->file->size;
    }
    if (!f->index && f->menu_vob) {     /* VIDEO_TS: its title VOB stream is its menu VOB */
        f->title_vobs[f->nb_title_vobs++] = f->menu_vob;
        f->title_vob_bytes = f->menu_vob_bytes;
    }
    return 0;
}

/* ---- reading ---- */

int ff_dvd_ifo_read(DVDDisc *d, DVDIfo *f, uint32_t sector, uint8_t *buf, int len)
{
    return f->rec && ifoRead_rec_bytes(f->rec, (int64_t)sector * DVD_SECTOR, buf, len) ? 0 : AVERROR_INVALIDDATA;
}

void ff_dvd_table_free(DVDTable *t)
{
    av_freep(&t->data);
    t->size = 0;
}

/* ---- VTS_PGCIT ---- */

static void free_pgc(DVDPgc **pgc)
{
    if (!*pgc)
        return;
    av_freep(&(*pgc)->commands);
    av_freep(&(*pgc)->program_map);
    av_freep(&(*pgc)->cells);
    av_freep(pgc);
}

/* A program chain as libdvdread read it, ours (NULL: out of memory). */
static DVDPgc *pgc_of(const pgc_rec_t *r)
{
    DVDPgc *p = av_mallocz(sizeof(*p));
    int n = r->nr_of_pre + r->nr_of_post + r->nr_of_cell;

    if (!p)
        return NULL;
    p->number         = r->number;
    p->entry          = r->entry;
    p->nr_of_programs = r->nr_of_programs;
    p->nr_of_cells    = r->nr_of_cells;
    p->playback_time  = r->playback_time;
    p->prohibited_ops = r->prohibited_ops;
    memcpy(p->audio_control, r->audio_control, sizeof(p->audio_control));
    memcpy(p->subp_control, r->subp_control, sizeof(p->subp_control));
    memcpy(p->palette, r->palette, sizeof(p->palette));
    p->nr_of_pre  = r->nr_of_pre;
    p->nr_of_post = r->nr_of_post;
    p->nr_of_cell = r->nr_of_cell;
    if ((r->commands && n && !(p->commands = av_memdup(r->commands, n * 8))) ||
        (r->program_map && r->nr_of_programs && !(p->program_map = av_memdup(r->program_map, r->nr_of_programs))) ||
        (r->cells && r->nr_of_cells && !(p->cells = av_calloc(r->nr_of_cells, sizeof(*p->cells))))) {
        free_pgc(&p);
        return NULL;
    }
    for (int c = 0; c < r->nr_of_cells && p->cells; c++) {
        DVDCell *cl = &p->cells[c];
        const cell_rec_t *rc = &r->cells[c];
        memcpy(cl->category, rc->category, 4);
        cl->playback_time          = rc->playback_time;
        cl->first_sector           = rc->first_sector;
        cl->first_ilvu_end_sector  = rc->first_ilvu_end_sector;
        cl->last_vobu_start_sector = rc->last_vobu_start_sector;
        cl->last_sector            = rc->last_sector;
        cl->vob_id_nr              = rc->vob_id_nr;
        cl->cell_nr                = rc->cell_nr;
        cl->pgcn                   = r->number;
        cl->index                  = c;
    }
    return p;
}

/* ---- the IFO ---- */

int ff_dvd_ifo_from_rec(DVDDisc *d, DVDIfo *f, const dvd_ifo_rec_t *r)
{
    const pgcit_rec_t *rec = r->pgcit;

    f->rec = r->ifo;
    if (!f->rec || !ifoRead_rec_bytes(f->rec, 0, f->header, DVD_IFO_HEADER)) {
        f->failed = 1;
        return 0;
    }
    ifoGet_rec_state(f->rec, &f->bup_only, &f->cache, &f->cache_size);
    f->ifo_sector            = r->ifo_sector;
    f->bup_sector            = r->bup_sector;
    f->title_vob_blocks      = r->title_vob_sectors;
    f->menu_vob_base         = (uint32_t)r->menu_vob_start;
    f->title_vob_start       = f->title_vob_base = (uint32_t)r->title_vob_start;
    f->menu_vobu_last        = r->menu_vobu_last;
    f->no_pgcit              = r->no_pgcit;
    f->map_untrusted_at_load = !r->map_trusted;
    if (r->nb_vobu_map && !(f->vobu_map = av_memdup(r->vobu_map, r->nb_vobu_map * sizeof(*f->vobu_map))))
        return AVERROR(ENOMEM);
    f->nb_vobu_map = r->nb_vobu_map;
    if (r->ptt_srpt && !(f->ptt_srpt.data = av_memdup(r->ptt_srpt, r->ptt_srpt_size)))
        return AVERROR(ENOMEM);
    f->ptt_srpt.size = r->ptt_srpt ? r->ptt_srpt_size : 0;
    if (!rec)
        return 0;
    if (rec->nr_of_pgcs &&
        (!(f->pgcs = av_calloc(rec->nr_of_pgcs, sizeof(*f->pgcs))) ||
         !(f->pgc_same_as = av_malloc_array(rec->nr_of_pgcs, sizeof(*f->pgc_same_as)))))
        return AVERROR(ENOMEM);
    f->nb_pgcs = rec->nr_of_pgcs;
    for (int i = 0; i < rec->nr_of_pgcs; i++) {
        f->pgc_same_as[i] = rec->same_as[i];
        if (rec->pgcs[i] && !(f->pgcs[i] = pgc_of(rec->pgcs[i])))
            return AVERROR(ENOMEM);
    }
    return 0;
}

int ff_dvd_map_find(const DVDIfo *f, uint32_t sector, int *exact)
{
    int lo = 0, hi = f->nb_vobu_map;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (f->vobu_map[mid] < sector)
            lo = mid + 1;
        else
            hi = mid;
    }
    *exact = lo < f->nb_vobu_map && f->vobu_map[lo] == sector;
    return lo;
}

void ff_dvd_ifo_free(DVDIfo **pf)
{
    DVDIfo *f = *pf;

    if (!f)
        return;
    f->menu_vob = NULL;                 /* the disc's (ff_dvd_disc_file()) */
    f->cache = NULL;                    /* libdvdread's, as f->rec */
    f->rec   = NULL;
    for (int i = 0; i < f->nb_pgcs; i++)
        free_pgc(&f->pgcs[i]);
    av_freep(&f->pgcs);
    av_freep(&f->pgc_same_as);
    ff_dvd_table_free(&f->ptt_srpt);
    av_freep(&f->vobu_map);
    av_freep(pf);
}
