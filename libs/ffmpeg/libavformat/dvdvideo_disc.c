/*
 * DVD-Video demuxer: the disc as the navigation scan and the title plan see it
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
 * The whole disc: the VMG and every title set's IFO (libdvdread, read through
 * the disc source), and per title set what the scan and the title plan need
 * about its title VOBs: the VOBU address map (sorted, distinct) and whether it
 * can be trusted, how many blocks the title VOBs hold, and on images where
 * they start. The VOBU records (dvdvideo_vobu.c) are kept here too.
 */

#include <stdarg.h>
#include <stdio.h>

#include "libavutil/tree.h"

#include "dvdvideo_internal.h"

static void disc_libdvdread_log(void *opaque, dvd_logger_level_t level, const char *msg, va_list args)
{
    char buf[DVDVIDEO_LIBDVDX_LOG_BUFFER_SIZE];

    vsnprintf(buf, sizeof(buf), msg, args);
    av_log(opaque, level <= DVD_LOGGER_LEVEL_WARN ? AV_LOG_DEBUG : AV_LOG_TRACE, "disc: libdvdread: %s\n", buf);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

    return (x > y) - (x < y);
}

/* The VOBU address map of the title VOBs, sorted, each start once. */
static int read_vobu_map(DVDVideoTitleSet *ts)
{
    const vobu_admap_t *map = ts->ifo->vts_vobu_admap;
    size_t nb, k = 0;

    if (!map || !map->vobu_start_sectors || map->last_byte + 1 < VOBU_ADMAP_SIZE)
        return 0;
    nb = (map->last_byte + 1 - VOBU_ADMAP_SIZE) / 4;
    if (!nb)
        return 0;
    if (!(ts->vobu_starts = av_memdup(map->vobu_start_sectors, nb * sizeof(*ts->vobu_starts))))
        return AVERROR(ENOMEM);
    qsort(ts->vobu_starts, nb, sizeof(*ts->vobu_starts), cmp_u32);
    for (size_t i = 0; i < nb; i++)
        if (!k || ts->vobu_starts[k - 1] != ts->vobu_starts[i])
            ts->vobu_starts[k++] = ts->vobu_starts[i];
    ts->nb_vobu_starts = k;
    return 0;
}

/* Images: the image block the title VOBs start at by the IFO (the IFO's block
 * + vtstt_vobs), unless the file system puts VTS_nn_1.VOB elsewhere; title
 * sets with a known start share their VOBU records by image block. 0 =
 * unknown (folders): their records are kept per title set. */
static uint32_t title_vobs_base(DVDVideoDisc *d, int vtsn, int64_t ifo_sector)
{
    char name[32];
    int64_t vob_sector;
    uint32_t s;

    if (ifo_sector < 0)
        return 0;
    s = ifo_sector + d->vts[vtsn].ifo->vtsi_mat->vtstt_vobs;
    snprintf(name, sizeof(name), "VTS_%02d_1.VOB", vtsn);
    vob_sector = ff_dvdvideo_source_file_sector(d->src, name);
    if (vob_sector > 0 && (uint32_t)vob_sector != s) {
        av_log(d->log, AV_LOG_DEBUG, "disc: title set %d: title VOBs at image block %"PRIu32" by the IFO, at "
               "%"PRId64" by the file system\n", vtsn, s, vob_sector);
        return 0;
    }
    return s;
}

/* The blocks of the title VOBs: images from the IFO (where the BUP lies after
 * the IFO, minus vtstt_vobs; the IFO is its own BUP when no BUP is in one
 * extent), folders the title VOB files. */
static uint32_t title_vobs_sectors(DVDVideoDisc *d, int vtsn, int64_t ifo_sector)
{
    char name[32];
    int64_t bup, bytes;

    if (ifo_sector < 0) {
        if (ff_dvdvideo_source_vob_bytes(d->src, vtsn, 0, &bytes) < 0)
            return 0;
        return bytes / DVDVIDEO_BLOCK_SIZE;
    }
    snprintf(name, sizeof(name), "VTS_%02d_0.BUP", vtsn);
    if ((bup = ff_dvdvideo_source_file_sector(d->src, name)) < 0)
        bup = ifo_sector;
    return (uint32_t)(bup - ifo_sector) - d->vts[vtsn].ifo->vtsi_mat->vtstt_vobs;
}

void ff_dvdvideo_disc_close(DVDVideoDisc **pdisc)
{
    DVDVideoDisc *d = *pdisc;

    if (!d)
        return;
    for (int n = 0; n < 100; n++) {
        if (d->vts[n].ifo)
            ifoClose(d->vts[n].ifo);
        av_free(d->vts[n].vobu_starts);
    }
    if (d->dvdread)
        DVDClose(d->dvdread);
    ff_dvdvideo_vobu_free(d);
    av_freep(pdisc);
}

int ff_dvdvideo_disc_open(void *log, DVDVideoSource *src, DVDVideoDisc **out)
{
    dvd_logger_cb log_cb = { .pf_log = disc_libdvdread_log };
    dvd_reader_filesystem_h *files;
    DVDVideoDisc *d;
    int ret;

    *out = NULL;
    if (!(d = av_mallocz(sizeof(*d))))
        return AVERROR(ENOMEM);
    d->log = log;
    d->src = src;
    if (!(files = ff_dvdvideo_source_files(src))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    /* on failure the files stay with the caller */
    if (!(d->dvdread = DVDOpenFiles(log, &log_cb, "/", files))) {
        files->close(files);
        av_log(log, AV_LOG_ERROR, "Unable to open the DVD-Video structure\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    if (!(d->vts[0].ifo = ifoOpen(d->dvdread, 0))) {
        av_log(log, AV_LOG_ERROR, "Unable to open the VMG (VIDEO_TS.IFO)\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    d->nb_vts = FFMIN(d->vts[0].ifo->vmgi_mat->vmg_nr_of_title_sets, 99);
    for (int n = 1; n <= d->nb_vts; n++) {
        DVDVideoTitleSet *ts = &d->vts[n];
        char name[32];
        int64_t ifo_sector;

        if (!(ts->ifo = ifoOpen(d->dvdread, n))) {
            av_log(log, AV_LOG_WARNING, "VTS_%02d_0.IFO cannot be opened; title set %d cannot be used\n", n, n);
            continue;
        }
        if ((ret = read_vobu_map(ts)) < 0)
            goto fail;
        ts->map_distrusted = !!(ff_dvdvideo_check_vts(log, d->dvdread, n, ts->ifo) & DVDVIDEO_VTS_MAP_DISTRUSTED);
        snprintf(name, sizeof(name), "VTS_%02d_0.IFO", n);
        ifo_sector             = ff_dvdvideo_source_file_sector(src, name);
        ts->title_vobs_base    = title_vobs_base(d, n, ifo_sector);
        ts->title_vobs_sectors = title_vobs_sectors(d, n, ifo_sector);
        av_log(log, AV_LOG_DEBUG, "disc: title set %d: %d VOBU map entries%s, title VOBs %"PRIu32" blocks, at image "
               "block %"PRIu32"\n", n, ts->nb_vobu_starts, ts->map_distrusted ? " (distrusted)" : "",
               ts->title_vobs_sectors, ts->title_vobs_base);
    }
    *out = d;
    return 0;

fail:
    ff_dvdvideo_disc_close(&d);
    return ret;
}

int ff_dvdvideo_disc_is_vobu_start(const DVDVideoTitleSet *ts, uint32_t sector)
{
    return ts->nb_vobu_starts &&
           bsearch(&sector, ts->vobu_starts, ts->nb_vobu_starts, sizeof(*ts->vobu_starts), cmp_u32);
}

int ff_dvdvideo_cell_flags(const pgc_t *pgc, int k)
{
    const cell_playback_t *c = &pgc->cell_playback[k];

    return c->block_mode << 6 | c->block_type << 4 | c->seamless_play << 3 | c->interleaved << 2 |
           c->stc_discontinuity << 1 | c->seamless_angle;
}

const cell_playback_t *ff_dvdvideo_disc_cell(const DVDVideoDisc *d, int vtsn, int pgcn, int celln)
{
    const ifo_handle_t *ifo = vtsn >= 1 && vtsn <= d->nb_vts ? d->vts[vtsn].ifo : NULL;
    const pgc_t *pgc;

    if (!ifo || !ifo->vts_pgcit || pgcn < 1 || pgcn > ifo->vts_pgcit->nr_of_pgci_srp)
        return NULL;
    pgc = ifo->vts_pgcit->pgci_srp[pgcn - 1].pgc;
    if (!pgc || !pgc->cell_playback || celln < 1 || celln > pgc->nr_of_cells)
        return NULL;
    return &pgc->cell_playback[celln - 1];
}
