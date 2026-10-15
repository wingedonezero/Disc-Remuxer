/*
 * DVD-Video: reading a title's segments sector by sector. A segment's
 * extents name sectors of its title set's title VOBs (the same title VOB
 * stream the navigation reads: an image's sectors from the IFO header's
 * title VOB start, a folder's VTS_nn_1..9.VOB joined). dvd_vob.c reads
 * them: it checks each sector (one that is no MPEG-2 pack followed by a
 * packet start code is left out, no packet of it reaches the demuxer), says
 * the damaged ranges, once each, to whoever reads, and descrambles CSS with
 * each title set's title key (looked for, checked and kept per title set).
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

#include <string.h>

#include "base/err.h"
#include "base/vlog.h"
#include <stdio.h>

#include "base/macros.h"
#include "base/mem.h"

#include "dvd_internal.h"

dvd_vob_reader_t *ff_dvd_title_vobs_open(DVDDisc *d, int vts)
{
    const dvd_ifo_rec_t *f = ff_dvd_set(d, vts);

    if (ff_dvd_reader_open(d) < 0 || !f)
        return NULL;
    return DVDOpenTitleVOBs_rec(d, f->ifo, vts);   /* when they cannot be opened, it is said why */
}

int ff_dvd_title_vob_read(DVDDisc *d, int vts, dvd_vob_reader_t *vr, uint32_t sector, uint8_t *buf,
                          dvd_vob_damage_t *ended)
{
    int ret;

    if (!vr && !(vr = ff_dvd_own_reading(d, vts)))
        return DR_ERR(EINVAL);
    ret = DVDReadTitleVOBSector_rec(vr, sector, buf, ended);
    return ret == -2 ? DR_ERR_INVALID : ret < 0 ? DR_ERR(EIO) : ret;
}

int ff_dvd_css_state(DVDDisc *d, int vts, uint8_t *key)
{
    return d->reader ? DVDTitleVOBKey_rec(d, vts, key) : 0;
}

int ff_dvd_title_vob_sector(DVDDisc *d, int vts, uint32_t sector, uint8_t *buf)
{
    return ff_dvd_title_vob_read(d, vts, NULL, sector, buf, NULL);
}

uint32_t ff_dvd_segment_blocks(const dvd_segment_t *g)
{
    const dvd_extent_t *e = g->nb_extents ? &g->extents[g->nb_extents - 1] : NULL;
    return e ? e->logical + e->count : 0;
}

/* The title VOB sector of a segment's block; < 0 when none holds it. */
static int segment_sector(const dvd_segment_t *g, uint32_t block, uint32_t *sector)
{
    int lo = 0, hi = g->nb_extents - 1;

    while (lo <= hi) {                  /* the extent holding block (the extents follow each other) */
        int mid = (lo + hi) / 2;
        const dvd_extent_t *e = &g->extents[mid];
        if (block < e->logical)
            hi = mid - 1;
        else if (block >= e->logical + e->count)
            lo = mid + 1;
        else {
            *sector = e->sector + (block - e->logical);
            return 0;
        }
    }
    return DR_ERR(EINVAL);
}

int ff_dvd_segment_read_vobs(DVDDisc *d, const dvd_title_t *t, dvd_vob_reader_t *vr, int seg, uint32_t block,
                             uint8_t *buf, dvd_vob_damage_t *ended)
{
    uint32_t sector;
    int ret = segment_sector(&t->segs.segments[seg], block, &sector);

    if (ended)
        memset(ended, 0, sizeof(*ended));
    return ret < 0 ? ret : ff_dvd_title_vob_read(d, t->vtsn, vr, sector, buf, ended);
}

int ff_dvd_segment_place(const dvd_title_t *t, const dvd_vob_reader_t *vr, int seg, uint32_t block, char *file,
                         int size, int64_t *offset)
{
    char name[16];
    uint32_t sector;

    if (segment_sector(&t->segs.segments[seg], block, &sector) < 0 ||
        !DVDTitleVOBSectorPlace_rec(vr, sector, name, offset))
        return DR_ERR(EINVAL);
    snprintf(file, size, "%s", name);
    return 0;
}

int ff_dvd_segment_view(const dvd_title_t *t, dvd_vob_reader_t *vr, int seg)
{
    const dvd_segment_t *g = &t->segs.segments[seg];
    uint32_t *sectors = dr_malloc_array(DR_MAX(g->nb_extents, 1), sizeof(*sectors));
    uint32_t *counts  = dr_malloc_array(DR_MAX(g->nb_extents, 1), sizeof(*counts));
    int ret = DR_ERR(ENOMEM);

    if (sectors && counts) {
        for (int i = 0; i < g->nb_extents; i++) {
            sectors[i] = g->extents[i].sector;
            counts[i]  = g->extents[i].count;
        }
        ret = DVDUseTitleVOBView_rec(vr, sectors, counts, g->nb_extents);
    }
    dr_free(sectors);
    dr_free(counts);
    return ret;
}

int ff_dvd_segment_read(DVDDisc *d, const dvd_title_t *t, int seg, uint32_t block, uint8_t *buf)
{
    return ff_dvd_segment_read_vobs(d, t, NULL, seg, block, buf, NULL);
}

/* ---- VOBUs: the title set's (dvd_vobu.c) on its own reading of the title
 * VOBs (libdvdread's) ---- */

dvd_vob_reader_t *ff_dvd_own_reading(DVDDisc *d, int vts)
{
    const dvd_ifo_rec_t *f;
    DVDCells *c;

    if (!d->rec || vts < 1 || vts > d->rec->nr_of_title_sets)
        return NULL;
    f = &d->rec->ifos[vts];
    c = &d->cells[vts];
    if (!c->own_opened && f->ifo) {
        c->own_opened = 1;
        c->own = DVDOpenTitleVOBs_rec(d, f->ifo, vts);   /* when they cannot be opened, it is said why */
    }
    return c->own;
}

int ff_dvd_nav_read(DVDDisc *d, int vts, uint32_t sector, dvd_vobu_nav_t *n)
{
    DVDCells *vr = ff_dvd_cells(d, vts);
    return vr ? DVDReadVobuNav_rec(vr, sector, n) : 0;
}

int ff_dvd_map_trusted(const DVDDisc *d, int vts)
{
    return d->reader && DVDVobuMapTrusted_rec(d, vts);
}

int ff_dvd_vobu_start_check(DVDDisc *d, int vts, uint32_t sector)
{
    DVDCells *vr = ff_dvd_cells(d, vts);
    return vr ? DVDVobuStart_rec(vr, sector) : 0;
}
