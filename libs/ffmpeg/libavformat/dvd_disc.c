/*
 * DVD-Video: the disc's structure, read and checked by libdvdread
 * (dvdread/disc_read.h, DVDOpenDisc_rec(): the VMG, the title table and its
 * checks, the text data and the disc's name in it, every title set and its
 * tables, where each title set starts, the VOBU address maps' trust), on
 * the disc's own files (dvd_nav.c). Here it is copied for the rest of the
 * DVD format part (dvd_ifo.c), what libdvdread found is logged, and the
 * disc is named: the volume label (an image), else (a folder) the VMG's
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

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

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
        av_log(log, AV_LOG_ERROR, "DVD: the disc cannot be read as DVD-Video (see the IFO check above)\n");
        return AVERROR_INVALIDDATA;
    }
    d->nr_of_title_sets = r->nr_of_title_sets;
    d->text             = r->text;
    if (!(d->title_set_start = av_calloc(d->nr_of_title_sets + 1, sizeof(*d->title_set_start))))
        return AVERROR(ENOMEM);
    memcpy(d->title_set_start, r->title_set_start, d->nr_of_title_sets * sizeof(*d->title_set_start));
    if (r->tt_srpt && !(d->tt_srpt.data = av_memdup(r->tt_srpt, r->tt_srpt_size)))
        return AVERROR(ENOMEM);
    d->tt_srpt.size = r->tt_srpt ? r->tt_srpt_size : 0;

    /* every information file (a title set libdvdread could not read stays a
     * placeholder) and a folder's VOB files */
    for (int i = 0; i <= d->nr_of_title_sets; i++) {
        DVDIfo *f = av_mallocz(sizeof(*f));
        if (!f || av_dynarray_add_nofree(&d->ifos, &d->nb_ifos, f) < 0) {
            av_free(f);
            return AVERROR(ENOMEM);
        }
        f->index = i;
        if ((ret = ff_dvd_ifo_from_rec(d, f, &r->ifos[i])) < 0 ||
            (!f->failed && (ret = ff_dvd_ifo_files(d, f)) < 0))
            return ret;
    }

    /* the names */
    if (label && !(d->volume_name = av_strdup(label)))
        return AVERROR(ENOMEM);
    if (r->text_name && !(d->text_name = av_strdup(r->text_name)))
        return AVERROR(ENOMEM);
    if (!d->text) {
        DiscIOFile *f = NULL;
        if (fs->ops->open_file(fs, "/DVDROM/iTunesInfo.xml", &f) >= 0)
            av_log(log, AV_LOG_WARNING, "DVD: /DVDROM/iTunesInfo.xml is present: the name in it is not read "
                   "[untested on real discs]\n");
        ff_discio_file_free(&f);
    }
    if (!d->volume_name && !(d->volume_name = av_strndup(r->provider_id, 32)))
        return AVERROR(ENOMEM);

    /* what libdvdread found, logged */
    ff_dvd_reports_log(d);
    return 0;
}

void ff_dvd_disc_free(DVDDisc *d)
{
    /* libdvdread's handles, then libdvdread (its callbacks read the IFOs below) */
    DVDCloseDisc_rec(d->rec);           /* its information files and title VOB readings */
    d->rec = NULL;
    if (d->reader)
        DVDClose(d->reader);
    d->reader = NULL;
    for (int i = 0; i < d->nb_ifos; i++)
        ff_dvd_ifo_free(&d->ifos[i]);
    av_freep(&d->ifos);
    for (int i = 0; i <= DVD_MAX_TITLE_SETS; i++)
        for (int k = 0; k < DVD_FILE_SLOTS; k++) {
            ff_discio_file_free(&d->files[i][k].file);
            ff_discio_file_free(&d->files[i][k].region);
            d->files[i][k].looked = 0;
        }
    d->nb_ifos = 0;
    ff_dvd_table_free(&d->tt_srpt);
    av_freep(&d->title_set_start);
    d->text = NULL;
    av_freep(&d->volume_name);
    av_freep(&d->text_name);
    for (int i = 0; i < d->nb_reports; i++)
        av_freep(&d->reports[i].text);
    av_freep(&d->reports);
    d->nb_reports = d->reports_cap = d->reports_said = 0;
}

static void dump_text(const dvd_text_node_t *n, int depth, AVBPrint *bp)
{
    av_bprintf(bp, "%*s  node kind 0x%02x at %u\n", 2 * depth, "", n->kind, n->offset);
    for (int i = 0; i < n->nb_texts; i++)
        av_bprintf(bp, "%*s    text 0x%02x \"%s\"\n", 2 * depth, "", n->text_kinds[i], n->texts[i]);
    for (int i = 0; i < n->nb_children; i++)
        dump_text(n->children[i], depth + 1, bp);
}

void ff_dvd_disc_dump(const DVDDisc *d, AVBPrint *bp)
{
    av_bprintf(bp, "volume name \"%s\", text name \"%s\", %d title set(s), %u title table entries\n",
               d->volume_name ? d->volume_name : "", d->text_name ? d->text_name : "", d->nr_of_title_sets,
               d->tt_srpt.size >= 2 ? AV_RB16(d->tt_srpt.data) : 0);
    for (int i = 0; i < d->nb_ifos; i++) {
        const DVDIfo *f = d->ifos[i];
        av_bprintf(bp, "%s%02d: %s", f->index ? "VTS_" : "VIDEO_TS ", f->index, f->failed ? "not read\n" : "");
        if (f->failed)
            continue;
        av_bprintf(bp, "last sector %"PRIu32", IFO last sector %"PRIu32", %s%s, title VOBs %"PRIu32" sectors",
                   ff_dvd_header32(f, DVD_LAST_SECTOR), ff_dvd_header32(f, DVD_I_LAST_SECTOR),
                   f->cache ? "read whole" : "read in pieces", f->bup_only ? " (from the BUP)" : "",
                   f->title_vob_blocks);
        if (!d->folder)
            av_bprintf(bp, ", IFO at %"PRId64", BUP at %"PRId64", title VOBs at %"PRIu32, f->ifo_sector,
                       f->bup_sector, f->title_vob_start);
        av_bprintf(bp, ", %d program chain(s), %d VOBU start(s)%s%s\n", f->nb_pgcs, f->nb_vobu_map,
                   !ff_dvd_map_trusted(d, f) ? ", VOBU map not trusted" : "", f->no_pgcit ? ", no VTS_PGCIT" : "");
        for (int p = 0; p < f->nb_pgcs; p++) {
            const DVDPgc *g = f->pgcs[p];
            if (!g) {
                av_bprintf(bp, "  PGC %d: left out%s\n", p + 1, f->pgc_same_as[p] >= 0 ? " (same start as an "
                           "earlier one)" : "");
                continue;
            }
            av_bprintf(bp, "  PGC %d: %u program(s), %u cell(s), %u+%u+%u commands\n", p + 1, g->nr_of_programs,
                       g->nr_of_cells, g->nr_of_pre, g->nr_of_post, g->nr_of_cell);
            for (int c = 0; c < g->nr_of_cells; c++)
                av_bprintf(bp, "    cell %d: vob %u cell %u, sectors %"PRIu32"-%"PRIu32" (last VOBU %"PRIu32")\n",
                           c + 1, g->cells[c].vob_id_nr, g->cells[c].cell_nr, g->cells[c].first_sector,
                           g->cells[c].last_sector, g->cells[c].last_vobu_start_sector);
        }
    }
    if (d->text) {
        av_bprintf(bp, "text data, language \"%.3s\":\n", d->text->language);
        dump_text(d->text->root, 0, bp);
    }
}
