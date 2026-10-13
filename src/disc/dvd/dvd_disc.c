/*
 * DVD-Video: the disc's structure, read and checked by libdvdread
 * (dvdread/disc_read.h, DVDOpenDisc_rec(): the VMG, the title table and its
 * checks, the text data and the disc's name in it, every title set and its
 * tables, where each title set starts, the VOBU address maps' trust), on
 * the disc's own files (dvd_nav.c); the rest of the DVD format part uses it
 * as it is. Here the disc is opened, what libdvdread found summed up, and
 * the disc named: the volume label (an image), else (a folder) the VMG's
 * provider identifier; the text data's name besides. Without text data a
 * /DVDROM/iTunesInfo.xml is noted (its name is not read).
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
#include <string.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/vob_read.h>

#include "base/mem.h"
#include "base/err.h"
#include "base/bytes.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "dvd_internal.h"

int ff_dvd_disc_open(void *log, DiscIOFS *fs, const char *dir, int folder, const char *label, DVDDisc *d)
{
    const dvd_disc_rec_t *r;
    int ret;

    memset(d, 0, sizeof(*d));
    d->log    = log;
    d->fs     = fs;
    d->dir    = dir;
    d->folder = folder;

    /* libdvdread reads and checks the disc */
    if ((ret = ff_dvd_reader_open(d)) < 0)
        return ret;
    if (!(r = d->rec = DVDOpenDisc_rec(d->reader))) {
        ff_dvd_reports_log(d);
        dr_vlog(log, DR_VLOG_ERROR, "DVD: the disc cannot be read as DVD-Video (see the IFO check above)\n");
        return DR_ERR_INVALID;
    }

    /* its name: the volume label, else the provider identifier */
    if (label && !(d->volume_name = dr_strdup(label)))
        return DR_ERR(ENOMEM);
    if (!r->text) {
        DiscIOFile *f = NULL;
        if (fs->ops->open_file(fs, "/DVDROM/iTunesInfo.xml", &f) >= 0)
            dr_vlog(log, DR_VLOG_WARNING, "DVD: /DVDROM/iTunesInfo.xml is present: the name in it is not read "
                   "[untested on real discs]\n");
        ff_discio_file_free(&f);
    }
    if (!d->volume_name && !(d->volume_name = dr_strndup(r->provider_id, 32)))
        return DR_ERR(ENOMEM);

    /* what libdvdread found, summed up */
    ff_dvd_reports_log(d);
    return 0;
}

void ff_dvd_disc_free(DVDDisc *d)
{
    /* libdvdread's handles, then libdvdread (its callbacks read the files below) */
    DVDCloseDisc_rec(d->rec);           /* its information files and title VOB readings */
    d->rec = NULL;
    if (d->reader)
        DVDClose(d->reader);
    d->reader = NULL;
    for (int i = 0; i <= DVD_MAX_TITLE_SETS; i++)
        for (int k = 0; k < DVD_FILE_SLOTS; k++) {
            ff_discio_file_free(&d->files[i][k].file);
            ff_discio_file_free(&d->files[i][k].region);
            d->files[i][k].looked = 0;
        }
    dr_freep(&d->volume_name);
    DVDFindingsFree(d->findings);
    d->findings = NULL;
}

static void dump_text(const dvd_text_node_t *n, int depth, DrStr *bp)
{
    dr_str_catf(bp, "%*s  node kind 0x%02x at %u\n", 2 * depth, "", n->kind, n->offset);
    for (int i = 0; i < n->nb_texts; i++)
        dr_str_catf(bp, "%*s    text 0x%02x \"%s\"\n", 2 * depth, "", n->text_kinds[i], n->texts[i]);
    for (int i = 0; i < n->nb_children; i++)
        dump_text(n->children[i], depth + 1, bp);
}

void ff_dvd_disc_dump(const DVDDisc *d, DrStr *bp)
{
    const dvd_disc_rec_t *r = d->rec;

    dr_str_catf(bp, "volume name \"%s\", text name \"%s\", %d title set(s), %u title table entries\n",
               d->volume_name ? d->volume_name : "", r->text_name ? r->text_name : "", r->nr_of_title_sets,
               r->tt_srpt && r->tt_srpt_size >= 2 ? dr_rb16(r->tt_srpt) : 0);
    for (int i = 0; i < ff_dvd_nb_sets(d); i++) {
        const dvd_ifo_rec_t *f = ff_dvd_set(d, i);
        const pgcit_rec_t *t = f ? f->pgcit : NULL;
        dr_str_catf(bp, "%s%02d: %s", i ? "VTS_" : "VIDEO_TS ", i, !f ? "not read\n" : "");
        if (!f)
            continue;
        dr_str_catf(bp, "last sector %"PRIu32", IFO last sector %"PRIu32", %s%s, title VOBs %"PRIu32" sectors",
                   ff_dvd_header32(f, DVD_LAST_SECTOR), ff_dvd_header32(f, DVD_I_LAST_SECTOR),
                   f->whole ? "read whole" : "read in pieces", f->bup_only ? " (from the BUP)" : "",
                   f->title_vob_sectors);
        if (!d->folder)
            dr_str_catf(bp, ", IFO at %"PRId64", BUP at %"PRId64", title VOBs at %"PRIu32, f->ifo_sector,
                       f->bup_sector, (uint32_t)f->title_vob_start);
        dr_str_catf(bp, ", %d program chain(s), %d VOBU start(s)%s%s\n", t ? t->nr_of_pgcs : 0, f->nb_vobu_map,
                   !ff_dvd_map_trusted(d, i) ? ", VOBU map not trusted" : "",
                   f->no_pgcit ? ", no VTS_PGCIT" : "");
        for (int p = 0; t && p < t->nr_of_pgcs; p++) {
            const pgc_rec_t *g = t->pgcs[p];
            if (!g) {
                dr_str_catf(bp, "  PGC %d: left out%s\n", p + 1, t->same_as[p] >= 0 ? " (same start as an "
                           "earlier one)" : "");
                continue;
            }
            dr_str_catf(bp, "  PGC %d: %u program(s), %u cell(s), %u+%u+%u commands\n", p + 1, g->nr_of_programs,
                       g->nr_of_cells, g->nr_of_pre, g->nr_of_post, g->nr_of_cell);
            for (int c = 0; c < g->nr_of_cells; c++)
                dr_str_catf(bp, "    cell %d: vob %u cell %u, sectors %"PRIu32"-%"PRIu32" (last VOBU %"PRIu32")\n",
                           c + 1, g->cells[c].vob_id_nr, g->cells[c].cell_nr, g->cells[c].first_sector,
                           g->cells[c].last_sector, g->cells[c].last_vobu_start_sector);
        }
    }
    if (r->text) {
        dr_str_catf(bp, "text data, language \"%.3s\":\n", r->text->language);
        dump_text(r->text->root, 0, bp);
    }
}
