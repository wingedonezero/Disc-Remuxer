/*
 * DVD-Video: reading a title's segments sector by sector. A segment's
 * extents name sectors of its title set's title VOBs (the same title VOB
 * stream the navigation reads: an image's sectors from the IFO header's
 * title VOB start, a folder's VTS_nn_1..9.VOB joined). libdvdread reads
 * them (vob_read.h): it checks each sector (one that is no MPEG-2 pack
 * followed by a packet start code is left out, no packet of it reaches the
 * demuxer), says the damaged ranges, once each, to whoever reads, and
 * descrambles CSS with each title set's title key (looked for, checked and
 * kept by libdvdread).
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

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"

dvd_vob_reader_t *ff_dvd_title_vobs_open(DVDDisc *d, DVDIfo *f)
{
    dvd_vob_reader_t *vr;

    if (ff_dvd_reader_open(d) < 0)
        return NULL;
    if (!(vr = DVDOpenTitleVOBs_rec(f->rec, f->index)))
        av_log(d->log, AV_LOG_ERROR, "DVD: VTS_%02d: libdvdread cannot open the title VOBs\n", f->index);
    return vr;
}

int ff_dvd_title_vob_read(DVDDisc *d, DVDIfo *f, dvd_vob_reader_t *vr, uint32_t sector, uint8_t *buf,
                          dvd_vob_damage_t *ended)
{
    int ret;

    if (!vr && !(vr = f->vobs)) {
        if (f->vobs_failed || !(vr = f->vobs = ff_dvd_title_vobs_open(d, f))) {
            f->vobs_failed = 1;
            return AVERROR(EINVAL);
        }
    }
    ret = DVDReadTitleVOBSector_rec(vr, sector, buf, ended);
    return ret == -2 ? AVERROR_INVALIDDATA : ret < 0 ? AVERROR(EIO) : ret;
}

int ff_dvd_css_state(DVDDisc *d, const DVDIfo *f, uint8_t *key)
{
    return d->reader ? DVDTitleVOBKey_rec(d->reader, f->index, key) : 0;
}

int ff_dvd_title_vob_sector(DVDDisc *d, DVDIfo *f, uint32_t sector, uint8_t *buf)
{
    return ff_dvd_title_vob_read(d, f, NULL, sector, buf, NULL);
}

uint32_t ff_dvd_segment_blocks(const DVDSegment *g)
{
    const DVDExtent *e = g->nb_extents ? &g->extents[g->nb_extents - 1] : NULL;
    return e ? e->logical + e->count : 0;
}

/* The title VOB sector of a segment's block; < 0 when none holds it. */
static int segment_sector(const DVDSegment *g, uint32_t block, uint32_t *sector)
{
    int lo = 0, hi = g->nb_extents - 1;

    while (lo <= hi) {                  /* the extent holding block (the extents follow each other) */
        int mid = (lo + hi) / 2;
        const DVDExtent *e = &g->extents[mid];
        if (block < e->logical)
            hi = mid - 1;
        else if (block >= e->logical + e->count)
            lo = mid + 1;
        else {
            *sector = e->sector + (block - e->logical);
            return 0;
        }
    }
    return AVERROR(EINVAL);
}

int ff_dvd_segment_read_vobs(DVDDisc *d, const DVDTitle *t, dvd_vob_reader_t *vr, int seg, uint32_t block,
                             uint8_t *buf, dvd_vob_damage_t *ended)
{
    uint32_t sector;
    int ret = segment_sector(&t->segments[seg], block, &sector);

    if (ended)
        memset(ended, 0, sizeof(*ended));
    return ret < 0 ? ret : ff_dvd_title_vob_read(d, t->vts, vr, sector, buf, ended);
}

int ff_dvd_segment_view(const DVDTitle *t, dvd_vob_reader_t *vr, int seg)
{
    const DVDSegment *g = &t->segments[seg];
    uint32_t *sectors = av_malloc_array(FFMAX(g->nb_extents, 1), sizeof(*sectors));
    uint32_t *counts  = av_malloc_array(FFMAX(g->nb_extents, 1), sizeof(*counts));
    int ret = AVERROR(ENOMEM);

    if (sectors && counts) {
        for (int i = 0; i < g->nb_extents; i++) {
            sectors[i] = g->extents[i].sector;
            counts[i]  = g->extents[i].count;
        }
        ret = DVDUseTitleVOBView_rec(vr, sectors, counts, g->nb_extents);
    }
    av_free(sectors);
    av_free(counts);
    return ret;
}

int ff_dvd_segment_read(DVDDisc *d, const DVDTitle *t, int seg, uint32_t block, uint8_t *buf)
{
    return ff_dvd_segment_read_vobs(d, t, NULL, seg, block, buf, NULL);
}
