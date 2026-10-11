/*
 * DVD-Video: one information file (VIDEO_TS.IFO or VTS_nn_0.IFO) with its
 * backup copy (.BUP) and the VOB files of its set.
 *
 * Files. In a disc folder: the IFO (the BUP when the IFO is missing), the
 * menu VOB when it is a file, the title VOBs VTS_nn_1..9.VOB up to the
 * first missing one (VIDEO_TS: VIDEO_TS.VOB), the BUP (the IFO when the BUP
 * is missing). In an image: the IFO must be one piece on the image; the
 * BUP's position comes from the file system (the IFO's own when the BUP is
 * not one piece); the IFO and BUP are read as the image from their
 * position on (an IFO's tables are addressed by sector from its start).
 *
 * Reading. The header (first 1024 bytes) is read in 2048-byte pieces, each
 * from the IFO and from the BUP when the IFO's piece cannot be read; when
 * the IFO's signature (DVDVIDEO-VMG / DVDVIDEO-VTS) is wrong, only the BUP
 * is read, and a wrong BUP signature too makes the file unusable. Then the
 * whole IFO (vmgi/vtsi_last_sector + 1 sectors, up to 2 MiB) is read once,
 * with up to 16 tries per piece, each failed try moving to the other copy
 * (after the first failed try, one sector at a time); when that fails the
 * tables are read from the copies piece by piece instead.
 *
 * Checks (warnings; the file stays usable):
 * - the BUP lies where the header puts it ((vts_last_sector -
 *   vtsi_last_sector) sectors after the IFO): in an image the file
 *   system's positions, in a folder the IFO, menu VOB and title VOB file
 *   sizes;
 * - VMG: a provider identifier holding "(Fab" (a disc processed by DVDFab /
 *   MacTheRipper, which are known to rewrite IFOs);
 * - VTS: every cell's first sector and last VOBU start are VOBU starts of
 *   the VOBU address map; else (or when a VOBU address map cannot be read)
 *   the map is not trusted.
 * A title set's program chains (VTS_PGCIT), its chapter table (VTS_PTT_SRPT)
 * and VOBU address map are loaded; a program chain table or chapter table
 * that cannot be read makes the title set unusable.
 *
 * Program chain table rules (each field read only inside the table, a
 * field outside it read as 0 and logged): a program chain starting outside
 * the table is left out; one starting where an earlier one starts is left
 * out (its search pointer recorded as the same as the earlier one); one
 * whose control arrays or command table run past the table, or with more
 * than 128 commands, is left out.
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


static void ifo_name(const DVDIfo *f, char *buf, size_t size)
{
    if (f->index)
        snprintf(buf, size, "VTS_%02d", f->index);
    else
        snprintf(buf, size, "VIDEO_TS");
}

/* ---- files ---- */

static int open_file(DVDDisc *d, const char *name, DiscIOFile **out)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", d->dir, name);
    return d->fs->ops->open_file(d->fs, path, out);
}

/* A file of the set by its extension ("IFO", "BUP", "VOB"; part: the
 * title VOB number, 0 for the IFO / BUP / menu VOB). */
static int open_set_file(DVDDisc *d, int index, int part, const char *ext, DiscIOFile **out)
{
    char name[32];
    if (index)
        snprintf(name, sizeof(name), "VTS_%02d_%d.%s", index, part, ext);
    else
        snprintf(name, sizeof(name), "VIDEO_TS.%s", ext);
    return open_file(d, name, out);
}

/* The image from sector on, as one file. */
static int image_region(DVDDisc *d, int64_t sector, DiscIOFile **out)
{
    int64_t blocks = (d->fs->src->size + DVD_SECTOR - 1) / DVD_SECTOR;
    DiscIOFile *f;

    if (sector >= blocks)
        return AVERROR_INVALIDDATA;
    if (!(f = av_mallocz(sizeof(*f))) || !(f->extents = av_mallocz(sizeof(*f->extents)))) {
        av_free(f);
        return AVERROR(ENOMEM);
    }
    f->nb_extents        = 1;
    f->extents[0].sector = sector;
    f->extents[0].count  = blocks - sector;
    f->size              = f->extents[0].count * DVD_SECTOR;
    *out = f;
    return 0;
}

/* The file's single extent's first sector; 0 when the file is missing or
 * not one piece on the image. */
static int64_t one_piece(DVDDisc *d, int index, int part, const char *ext, const char *what)
{
    DiscIOFile *f = NULL;
    int64_t sector = 0;
    int ret = open_set_file(d, index, part, ext, &f);

    if (ret >= 0 && f->nb_extents == 1 && !f->data && f->extents[0].sector != DISCIO_SECTOR_NOT_RECORDED)
        sector = f->extents[0].sector;
    else
        av_log(d->log, AV_LOG_VERBOSE, "DVD: %s of %s %s on the image\n", what,
               index ? "a title set" : "VIDEO_TS", ret < 0 ? "is missing" : "is not one piece");
    ff_discio_file_free(&f);
    return sector;
}

static int open_image_files(DVDDisc *d, DVDIfo *f)
{
    DiscIOFile *ifo = NULL;
    char name[16];
    int ret;

    ifo_name(f, name, sizeof(name));
    if ((ret = open_set_file(d, f->index, 0, "IFO", &ifo)) < 0) {
        av_log(d->log, AV_LOG_WARNING, "DVD: %s.IFO cannot be opened: %s\n", name, av_err2str(ret));
        return ret;
    }
    if (ifo->nb_extents != 1 || ifo->data || ifo->extents[0].sector == DISCIO_SECTOR_NOT_RECORDED) {
        av_log(d->log, AV_LOG_WARNING, "DVD: %s.IFO is not one piece on the image: it is not read\n", name);
        ff_discio_file_free(&ifo);
        return AVERROR_INVALIDDATA;
    }
    f->ifo_sector = ifo->extents[0].sector;
    ff_discio_file_free(&ifo);
    if (!(f->bup_sector = one_piece(d, f->index, 0, "BUP", "the BUP"))) {
        av_log(d->log, AV_LOG_VERBOSE, "DVD: %s: the IFO stands in for its BUP\n", name);
        f->bup_sector = f->ifo_sector;
    }
    f->menu_vob_sector  = one_piece(d, f->index, 0, "VOB", "the menu VOB");
    f->title_vob_sector = f->index ? one_piece(d, f->index, 1, "VOB", "the first title VOB") : 0;
    if ((ret = image_region(d, f->ifo_sector, &f->copy[0])) < 0 ||
        (ret = image_region(d, f->bup_sector, &f->copy[1])) < 0)
        return ret;
    return 0;
}

static int open_folder_files(DVDDisc *d, DVDIfo *f)
{
    char name[16];
    int ret;

    ifo_name(f, name, sizeof(name));
    if ((ret = open_set_file(d, f->index, 0, "IFO", &f->copy[0])) < 0) {
        if ((ret = open_set_file(d, f->index, 0, "BUP", &f->copy[0])) < 0) {
            av_log(d->log, AV_LOG_WARNING, "DVD: neither %s.IFO nor %s.BUP can be opened\n", name, name);
            return ret;
        }
        av_log(d->log, AV_LOG_WARNING, "DVD: %s.IFO is missing: its BUP is read as the IFO\n", name);
    }
    f->ifo_bytes = f->copy[0]->size;
    if (open_set_file(d, f->index, 0, "VOB", &f->menu_vob) >= 0)
        f->menu_vob_bytes = f->menu_vob->size;
    for (int part = f->index ? 1 : 0; part <= (f->index ? 9 : 0); part++) {
        DiscIOFile *v = NULL;
        if (open_set_file(d, f->index, part, "VOB", &v) < 0)
            break;
        if ((ret = av_dynarray_add_nofree(&f->title_vobs, &f->nb_title_vobs, v)) < 0) {
            ff_discio_file_free(&v);
            return ret;
        }
        f->title_vob_bytes += v->size;
    }
    if ((ret = open_set_file(d, f->index, 0, "BUP", &f->copy[1])) < 0) {
        if ((ret = open_set_file(d, f->index, 0, "IFO", &f->copy[1])) < 0)
            return ret;
        av_log(d->log, AV_LOG_WARNING, "DVD: %s.BUP is missing: the IFO stands in for it\n", name);
    }
    f->bup_bytes = f->copy[1]->size;
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

/* ---- VOBU address maps ---- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* The VOBU starts of the map at sector, sorted without repeats; 0 for no
 * map, 1 loaded, < 0 an error. A map that cannot be read makes the IFO's
 * VOBU starts untrusted. */
/* A VOBU address map at sector through libdvdread (h): 1 read, 0 none
 * (sector 0) or not readable (then the title set's VOBU starts are not
 * trusted), or AVERROR(ENOMEM). */
static int load_vobu_map(DVDDisc *d, DVDIfo *f, ifo_handle_t *h, uint32_t sector, const char *table,
                         uint32_t **map, int *nb)
{
    uint32_t *starts;
    char name[16];
    int n;

    if (!sector)
        return 0;
    ifo_name(f, name, sizeof(name));
    if (!ifoRead_VOBU_ADMAP_rec(h, sector, table, &starts, &n)) {
        av_log(d->log, AV_LOG_WARNING, "DVD: %s: the VOBU address map at sector %"PRIu32" cannot be read: its "
               "VOBU starts are not trusted\n", name, sector);
        f->vobu_map_distrusted = 1;
        return 0;
    }
    av_freep(map);
    *map = av_memdup(starts, FFMAX(n, 1) * sizeof(*starts));
    free(starts);
    if (!*map)
        return AVERROR(ENOMEM);
    *nb = n;
    return 1;
}

static int on_map(const DVDIfo *f, uint32_t sector)
{
    return bsearch(&sector, f->vobu_map, f->nb_vobu_map, sizeof(*f->vobu_map), cmp_u32) != NULL;
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
    }
    return p;
}

/* ---- the IFO ---- */

/* Every cell's first sector and last VOBU start on the VOBU address map;
 * the first that is not makes the map untrusted. */
static void check_cells(DVDDisc *d, DVDIfo *f)
{
    char name[16];

    ifo_name(f, name, sizeof(name));
    for (int i = 0; i < f->nb_pgcs; i++) {
        const DVDPgc *p = f->pgcs[i];
        for (int c = 0; p && c < p->nr_of_cells; c++) {
            const DVDCell *cl = &p->cells[c];
            uint32_t miss = cl->first_sector;
            if (on_map(f, miss))
                miss = cl->last_vobu_start_sector;
            if (!on_map(f, miss)) {
                av_log(d->log, AV_LOG_WARNING, "DVD: %s: program chain %d cell %d: sector %"PRIu32" is no VOBU "
                       "start of the VOBU address map: its VOBU starts are not trusted (the VOBs must be "
                       "scanned)\n", name, i + 1, c + 1, miss);
                f->vobu_map_distrusted = 1;
                return;
            }
        }
    }
}

static int load(DVDDisc *d, DVDIfo *f)
{
    char name[16];
    uint32_t pgcit;
    int ret;

    ifo_name(f, name, sizeof(name));
    /* libdvdread opens the IFO and its BUP and chooses: the header from the
     * IFO, from the BUP when the IFO's signature is wrong; the whole IFO
     * read once, else piece by piece (what it finds: reported) */
    if ((ret = ff_dvd_reader_open(d)) < 0)
        return ret;
    if (!(f->rec = ifoOpen_rec(d->reader, f->index)) ||
        !ifoRead_rec_bytes(f->rec, 0, f->header, DVD_IFO_HEADER))
        return AVERROR_INVALIDDATA;
    ifoGet_rec_state(f->rec, &f->bup_only, &f->cache, &f->cache_size);

    if (d->folder)
        f->title_vob_blocks = (f->index ? f->title_vob_bytes : f->menu_vob_bytes) / DVD_SECTOR;
    else
        f->title_vob_blocks = (uint32_t)(f->bup_sector - f->ifo_sector) - ff_dvd_header32(f, DVD_TT_SRPT);
    /* the VOBU address maps: through libdvdread (ff_dvd_ifo_tables()) */

    if (!f->index)
        return 0;

    pgcit = ff_dvd_header32(f, DVD_VTS_PGCIT);
    if (pgcit == 0 || pgcit == 0xffffffff) {
        av_log(d->log, AV_LOG_VERBOSE, "DVD: %s has no program chain table\n", name);
        f->no_pgcit = 1;
        return 0;
    }
    /* the program chain and chapter tables: through libdvdread once every
     * IFO is loaded (ff_dvd_ifo_tables()) */
    if (!d->folder) {
        /* the title VOBs from the IFO header, when the file system agrees
         * (or has no single piece for them) */
        uint32_t start = ff_dvd_header32(f, DVD_TT_SRPT) + (uint32_t)f->ifo_sector;
        if (ff_dvd_header32(f, DVD_TT_SRPT))
            f->title_vob_base = start;  /* the title VOB stream the navigation reads (dvd_nav.c) */
        if (!f->title_vob_sector || f->title_vob_sector == start)
            f->title_vob_start = start;
        else
            av_log(d->log, AV_LOG_WARNING, "DVD: %s: the IFO header puts the title VOBs at sector %"PRIu32", the "
                   "file system at %"PRId64": decided when they are read\n", name, start, f->title_vob_sector);
    }
    return 0;
}

int ff_dvd_title_vob_place(DVDDisc *d, DVDIfo *f, uint32_t sector, char *file, int size, int64_t *offset)
{
    int64_t pos = (int64_t)sector * DVD_SECTOR, at;

    if (d->folder) {
        /* the title VOB files one after the other */
        for (int i = 0; i < f->nb_title_vobs; i++) {
            if (pos < f->title_vobs[i]->size) {
                snprintf(file, size, "VTS_%02d_%d.VOB", f->index, i + 1);
                *offset = pos;
                return 0;
            }
            pos -= f->title_vobs[i]->size;
        }
        return AVERROR(EINVAL);
    }
    /* an image: the title VOBs are read from the IFO header's place on
     * (ff_dvd_vob_read()); the VOB file whose extents hold that sector */
    if (!ff_dvd_header32(f, DVD_TT_SRPT))
        return AVERROR(EINVAL);
    at = f->ifo_sector + ff_dvd_header32(f, DVD_TT_SRPT) + sector;
    for (int part = 1; part <= 9; part++) {
        DiscIOFile *v = NULL;
        int64_t before = 0;
        if (open_set_file(d, f->index, part, "VOB", &v) < 0)
            break;
        for (int e = 0; e < v->nb_extents; e++) {
            const DiscIOExtent *x = &v->extents[e];
            if (x->sector != DISCIO_SECTOR_NOT_RECORDED && at >= x->sector && at < x->sector + x->count &&
                before + (at - x->sector) * DVD_SECTOR < v->size) {
                snprintf(file, size, "VTS_%02d_%d.VOB", f->index, part);
                *offset = before + (at - x->sector) * DVD_SECTOR;
                ff_discio_file_free(&v);
                return 0;
            }
            before += x->count * DVD_SECTOR;
        }
        ff_discio_file_free(&v);
    }
    return AVERROR(EINVAL);
}

int ff_dvd_ifo_raw_table(DVDDisc *d, DVDIfo *f, uint32_t sector, int last_byte_at, const char *table,
                         DVDTable *t)
{
    ifo_handle_t *h = f->rec;
    uint8_t *data = NULL;
    uint32_t size;
    int ok, ret = 0;

    ff_dvd_table_free(t);
    if (!h)
        return AVERROR_INVALIDDATA;
    ok = last_byte_at < 0 ? ifoRead_TT_SRPT_rec(h, -last_byte_at, &data, &size) :
                            ifoRead_table_rec(h, sector, last_byte_at, table, &data, &size);
    if (!ok)
        ret = AVERROR_INVALIDDATA;
    else if (data && !(t->data = av_memdup(data, size)))
        ret = AVERROR(ENOMEM);
    else
        t->size = data ? size : 0;
    free(data);
    return ret;
}

int ff_dvd_ifo_tables(DVDDisc *d, DVDIfo *f)
{
    ifo_handle_t *h = f->rec;
    pgcit_rec_t *rec = NULL;
    uint32_t *menu_map = NULL, ptt_size;
    uint8_t *ptt = NULL;
    char name[16];
    int nb_menu_map = 0, ret = 0;

    ifo_name(f, name, sizeof(name));
    if (!h) {
        av_log(d->log, AV_LOG_ERROR, "DVD: %s cannot be read by libdvdread\n", name);
        return AVERROR_INVALIDDATA;
    }
    /* the menus' VOBU address map: its last VOBU start */
    if ((ret = load_vobu_map(d, f, h, ff_dvd_header32(f, DVD_M_VOBU_ADMAP), f->index ? "VTSM_VOBU_ADMAP" :
                             "VMGM_VOBU_ADMAP", &menu_map, &nb_menu_map)) < 0)
        goto end;
    if (ret > 0 && nb_menu_map)
        f->menu_vobu_last = menu_map[nb_menu_map - 1];
    ret = 0;
    if (!f->index || f->no_pgcit)
        goto end;
    if (!ifoRead_VTS_PGCIT_rec(h, &rec)) {
        av_log(d->log, AV_LOG_ERROR, "DVD: %s: its program chain table (VTS_PGCIT) cannot be read\n", name);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if (!rec) {
        f->no_pgcit = 1;
        goto end;
    }
    if (rec->nr_of_pgcs &&
        (!(f->pgcs = av_calloc(rec->nr_of_pgcs, sizeof(*f->pgcs))) ||
         !(f->pgc_same_as = av_malloc_array(rec->nr_of_pgcs, sizeof(*f->pgc_same_as))))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    f->nb_pgcs = rec->nr_of_pgcs;
    for (int i = 0; i < rec->nr_of_pgcs; i++) {
        f->pgc_same_as[i] = rec->same_as[i];
        if (rec->pgcs[i] && !(f->pgcs[i] = pgc_of(rec->pgcs[i]))) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }
    /* the chapter table as recorded (the title list reads it) */
    if (!ifoRead_table_rec(h, h->vtsi_mat->vts_ptt_srpt, 4, "VTS_PTT_SRPT", &ptt, &ptt_size)) {
        av_log(d->log, AV_LOG_ERROR, "DVD: %s: its chapter table (VTS_PTT_SRPT) cannot be read\n", name);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if (ptt && !(f->ptt_srpt.data = av_memdup(ptt, ptt_size))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    f->ptt_srpt.size = ptt ? ptt_size : 0;
    /* the title VOBs' VOBU address map, checked against the cells */
    if ((ret = load_vobu_map(d, f, h, ff_dvd_header32(f, DVD_VTS_VOBU_ADMAP), "VTS_VOBU_ADMAP", &f->vobu_map,
                             &f->nb_vobu_map)) < 0)
        goto end;
    if (ret > 0)
        check_cells(d, f);
    ret = 0;
end:
    av_free(menu_map);
    free(ptt);
    ifoFree_VTS_PGCIT_rec(&rec);
    return ret;
}

int ff_dvd_ifo_load(DVDDisc *d, int index, DVDIfo **out)
{
    DVDIfo *f = av_mallocz(sizeof(*f));
    int ret;

    if (!f)
        return AVERROR(ENOMEM);
    f->index = index;
    if (av_dynarray_add_nofree(&d->ifos, &d->nb_ifos, f) < 0) {
        av_free(f);
        return AVERROR(ENOMEM);
    }
    *out = f;
    ret = d->folder ? open_folder_files(d, f) : open_image_files(d, f);
    if (ret >= 0)
        ret = load(d, f);
    if (ret < 0)
        f->failed = 1;
    return ret;
}

void ff_dvd_ifo_free(DVDIfo **pf)
{
    DVDIfo *f = *pf;

    if (!f)
        return;
    ff_discio_file_free(&f->copy[0]);
    ff_discio_file_free(&f->copy[1]);
    ff_discio_file_free(&f->menu_vob);
    for (int i = 0; i < f->nb_title_vobs; i++)
        ff_discio_file_free(&f->title_vobs[i]);
    av_freep(&f->title_vobs);
    f->cache = NULL;                    /* libdvdread's */
    if (f->rec)
        ifoClose(f->rec);
    f->rec = NULL;
    for (int i = 0; i < f->nb_pgcs; i++)
        free_pgc(&f->pgcs[i]);
    av_freep(&f->pgcs);
    av_freep(&f->pgc_same_as);
    ff_dvd_table_free(&f->ptt_srpt);
    av_freep(&f->vobu_map);
    ff_dvd_vobu_recs_free(&f->vobus);
    av_freep(pf);
}
