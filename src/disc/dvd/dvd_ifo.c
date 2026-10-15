/*
 * DVD-Video: the disc's own files (each looked up once through the disc
 * layer's file system: an IFO, a BUP, a menu VOB, a title VOB file) and the
 * bytes of an information file as libdvdread chose and read them (its
 * dvd_ifo_rec_t, dvdread/disc_read.h).
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>

#include "base/err.h"
#include "base/bytes.h"
#include "base/vlog.h"
#include "base/macros.h"
#include "base/mem.h"

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
                     ext[DR_MIN(slot, 2)]);
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
            if (at < blocks && (g = dr_mallocz(sizeof(*g)))) {
                if ((g->extents = dr_mallocz(sizeof(*g->extents)))) {
                    g->nb_extents        = 1;
                    g->extents[0].sector = at;
                    g->extents[0].count  = blocks - at;
                    g->size              = g->extents[0].count * DVD_SECTOR;
                    r->region = g;
                } else {
                    dr_free(g);
                }
            }
        }
    }
    return r->file ? r : NULL;
}

/* ---- reading ---- */

int ff_dvd_ifo_read(DVDDisc *d, int set, uint32_t sector, uint8_t *buf, int len)
{
    const dvd_ifo_rec_t *f = ff_dvd_set(d, set);
    return f && ifoRead_rec_bytes(f->ifo, (int64_t)sector * DVD_SECTOR, buf, len) ? 0 : DR_ERR_INVALID;
}
