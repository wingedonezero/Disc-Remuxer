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

#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"

#define MAX_COMMANDS 128            /* pre + post + cell commands of one program chain */

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
    int64_t pos = (int64_t)sector * DVD_SECTOR;
    char name[16];
    int ret = 0;

    ifo_name(f, name, sizeof(name));
    if (f->cache) {
        if (pos + len > f->cache_size) {
            av_log(d->log, AV_LOG_WARNING, "DVD: %s: %d bytes at sector %"PRIu32" lie past the IFO's %d "
                   "bytes\n", name, len, sector, f->cache_size);
            return AVERROR_INVALIDDATA;
        }
        memcpy(buf, f->cache + pos, len);
        return 0;
    }
    for (int done = 0; done < len; ) {
        int n = FFMIN(len - done, DVD_SECTOR);
        if (f->bup_only || (ret = ff_discio_file_read(d->fs, f->copy[0], pos + done, buf + done, n)) < 0) {
            if (!f->bup_only)
                av_log(d->log, AV_LOG_WARNING, "DVD: %s: IFO bytes %"PRId64"-%"PRId64" read from the BUP\n",
                       name, pos + done, pos + done + n - 1);
            if ((ret = ff_discio_file_read(d->fs, f->copy[1], pos + done, buf + done, n)) < 0) {
                av_log(d->log, AV_LOG_ERROR, "DVD: %s: IFO bytes %"PRId64"-%"PRId64" cannot be read from the "
                       "IFO or the BUP\n", name, pos + done, pos + done + n - 1);
                return ret;
            }
        }
        done += n;
    }
    return 0;
}

/* The whole IFO: tries per piece, each failed try moving to the other copy;
 * after the first failed try one sector at a time. */
static int read_whole(DVDDisc *d, DVDIfo *f, uint8_t *buf, int len)
{
    DiscIOFile *copies[2] = { f->copy[0], f->copy[1] };
    int nb = 2, cur = 0, single = 0;
    char name[16];

    ifo_name(f, name, sizeof(name));
    if (f->bup_only) {
        copies[0] = f->copy[1];
        nb = 1;
    }
    for (int off = 0; off < len; ) {
        int tries = 0, want, ret;
        for (;;) {
            if (tries && !single)
                single = 1;
            want = single ? FFMIN(DVD_SECTOR, len - off) : len - off;
            if (off + want > copies[cur]->size)
                ret = AVERROR(EIO);         /* past the end of a folder's file */
            else
                ret = ff_discio_file_read_attempts(d->fs, copies[cur], off, buf + off, want, 1, 1);
            if (ret >= 0)
                break;
            tries++;
            av_log(d->log, AV_LOG_WARNING, "DVD: %s: reading the whole IFO, %d bytes at %d from the %s failed "
                   "(%s); try %d of %d\n", name, want, off, copies[cur] == f->copy[0] ? "IFO" : "BUP",
                   av_err2str(ret), tries, DVD_IFO_CACHE_TRIES);
            if (tries >= DVD_IFO_CACHE_TRIES)
                return ret;
            cur = (cur + 1) % nb;
        }
        off += want;
    }
    return 0;
}

uint32_t ff_dvd_table_len_last_byte(const uint8_t *first, uint32_t field)
{
    uint32_t v = AV_RB32(first + field) + 1;
    return v >= field + 4 ? v : 0;
}

/* VTS_PGCIT: whole sectors up to its last byte. */
static uint32_t len_pgcit(const uint8_t *first, uint32_t field)
{
    return (AV_RB32(first + 4) & 0xfffff800) + DVD_SECTOR;
}

int ff_dvd_ifo_table(DVDDisc *d, DVDIfo *f, uint32_t sector, DVDTable *t,
                     uint32_t (*len_of)(const uint8_t *first, uint32_t field), uint32_t field)
{
    uint8_t first[DVD_SECTOR];
    uint32_t len;
    int ret;

    ff_dvd_table_free(t);
    if ((ret = ff_dvd_ifo_read(d, f, sector, first, DVD_SECTOR)) < 0)
        return ret;
    if (!(len = len_of(first, field)))
        return 0;
    if (len > 0x3fffffff)
        return AVERROR_INVALIDDATA;
    if (!(t->data = av_malloc(len)))
        return AVERROR(ENOMEM);
    t->size = len;
    memcpy(t->data, first, FFMIN(len, DVD_SECTOR));
    if (len > DVD_SECTOR && (ret = ff_dvd_ifo_read(d, f, sector + 1, t->data + DVD_SECTOR, len - DVD_SECTOR)) < 0) {
        ff_dvd_table_free(t);
        return ret;
    }
    return 0;
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
static int load_vobu_map(DVDDisc *d, DVDIfo *f, uint32_t sector, uint32_t **map, int *nb)
{
    DVDTable t = { 0 };
    char name[16];
    int n, k = 0, ret;

    if (!sector)
        return 0;
    ifo_name(f, name, sizeof(name));
    ret = ff_dvd_ifo_table(d, f, sector, &t, ff_dvd_table_len_last_byte, 0);
    if (ret < 0 || t.size < 4) {
        av_log(d->log, AV_LOG_WARNING, "DVD: %s: the VOBU address map at sector %"PRIu32" cannot be read: its "
               "VOBU starts are not trusted\n", name, sector);
        f->vobu_map_distrusted = 1;
        ff_dvd_table_free(&t);
        return ret == AVERROR(ENOMEM) ? ret : 0;
    }
    n = t.size / 4;
    av_freep(map);
    if (!(*map = av_malloc_array(FFMAX(n - 1, 1), sizeof(**map)))) {
        ff_dvd_table_free(&t);
        return AVERROR(ENOMEM);
    }
    for (int i = 1; i < n; i++)
        (*map)[k++] = AV_RB32(t.data + 4 * i);
    qsort(*map, k, sizeof(**map), cmp_u32);
    n = 0;
    for (int i = 0; i < k; i++)
        if (!n || (*map)[n - 1] != (*map)[i])
            (*map)[n++] = (*map)[i];
    *nb = n;
    ff_dvd_table_free(&t);
    return 1;
}

static int on_map(const DVDIfo *f, uint32_t sector)
{
    return bsearch(&sector, f->vobu_map, f->nb_vobu_map, sizeof(*f->vobu_map), cmp_u32) != NULL;
}

/* ---- VTS_PGCIT ---- */

typedef struct Fields {
    DVDDisc       *d;
    const DVDIfo  *f;
    const uint8_t *p;
    uint32_t       len;
} Fields;

static void outside(const Fields *t, uint32_t off, int bytes)
{
    char name[16];
    ifo_name(t->f, name, sizeof(name));
    av_log(t->d->log, AV_LOG_DEBUG, "DVD: %s: VTS_PGCIT field of %d byte(s) at %"PRIu32" lies outside the "
           "table (%"PRIu32" bytes): read as 0\n", name, bytes, off, t->len);
}

static uint8_t  B(const Fields *t, uint32_t o) { if ((uint64_t)o + 1 > t->len) { outside(t, o, 1); return 0; } return t->p[o]; }
static uint16_t W(const Fields *t, uint32_t o) { if ((uint64_t)o + 2 > t->len) { outside(t, o, 2); return 0; } return AV_RB16(t->p + o); }
static uint32_t L(const Fields *t, uint32_t o) { if ((uint64_t)o + 4 > t->len) { outside(t, o, 4); return 0; } return AV_RB32(t->p + o); }

static void free_pgc(DVDPgc **pgc)
{
    if (!*pgc)
        return;
    av_freep(&(*pgc)->commands);
    av_freep(&(*pgc)->program_map);
    av_freep(&(*pgc)->cells);
    av_freep(pgc);
}

/* One program chain at off; NULL (logged) when it is left out. */
static int parse_pgc(const Fields *t, int i, uint32_t entry, uint32_t off, DVDPgc **out)
{
    char name[16];
    DVDPgc *p;
    uint16_t pm, cply, cpos, ct;

    *out = NULL;
    ifo_name(t->f, name, sizeof(name));
    if (!(p = av_mallocz(sizeof(*p))))
        return AVERROR(ENOMEM);
    p->number         = i;
    p->entry          = entry;
    p->nr_of_programs = B(t, off + 2);
    p->nr_of_cells    = B(t, off + 3);
    p->playback_time  = L(t, off + 4);
    p->prohibited_ops = L(t, off + 8);
    if ((p->nr_of_programs && !(p->program_map = av_mallocz(p->nr_of_programs))) ||
        (p->nr_of_cells && !(p->cells = av_calloc(p->nr_of_cells, sizeof(*p->cells))))) {
        free_pgc(&p);
        return AVERROR(ENOMEM);
    }
    pm = W(t, off + 0xe6);
    for (int k = 0; k < p->nr_of_programs; k++)
        p->program_map[k] = B(t, off + pm + k);
    cpos = W(t, off + 0xea);
    cply = W(t, off + 0xe8);
    if (!p->nr_of_cells) {
        *out = p;                       /* no cells: nothing more is read */
        return 0;
    }
    for (int c = 0; c < p->nr_of_cells; c++) {
        DVDCell *cl = &p->cells[c];
        uint32_t cp = off + cply + 0x18 * c;
        cl->vob_id_nr = W(t, off + cpos + 4 * c);
        cl->cell_nr   = B(t, off + cpos + 4 * c + 3);
        if ((uint64_t)cp + 4 <= t->len)
            memcpy(cl->category, t->p + cp, 4);
        cl->playback_time          = L(t, cp + 4);
        cl->first_sector           = L(t, cp + 8);
        cl->first_ilvu_end_sector  = L(t, cp + 0x0c);
        cl->last_vobu_start_sector = L(t, cp + 0x10);
        cl->last_sector            = L(t, cp + 0x14);
    }
    if ((uint64_t)off + 0x9c > t->len)
        goto broken;
    memcpy(p->audio_control, t->p + off + 0x0c, 16);
    memcpy(p->subp_control, t->p + off + 0x1c, 128);
    ct = (uint64_t)off + 0xe6 <= t->len ? AV_RB16(t->p + off + 0xe4) : (outside(t, off + 0xe4, 2), 0);
    if (ct) {
        uint32_t b = off + ct, n;
        p->nr_of_pre  = W(t, b);
        p->nr_of_post = W(t, b + 2);
        p->nr_of_cell = W(t, b + 4);
        n = p->nr_of_pre + p->nr_of_post + p->nr_of_cell;
        if (n > MAX_COMMANDS) {
            av_log(t->d->log, AV_LOG_WARNING, "DVD: %s: program chain %d has %"PRIu32" commands (more than %d): "
                   "it is left out\n", name, i + 1, n, MAX_COMMANDS);
            free_pgc(&p);
            return 0;
        }
        /* the table's header and its commands inside the table (also
         * without commands: one byte past the header) */
        if ((uint64_t)b + 8 >= t->len || (uint64_t)b + n * 8 + 8 > t->len)
            goto broken;
        if (n) {
            if (!(p->commands = av_malloc(n * 8))) {
                free_pgc(&p);
                return AVERROR(ENOMEM);
            }
            memcpy(p->commands, t->p + b + 8, n * 8);
        }
    }
    for (int j = 0; j < 16; j++)
        p->palette[j] = L(t, off + 0xa4 + 4 * j);
    *out = p;
    return 0;
broken:
    av_log(t->d->log, AV_LOG_WARNING, "DVD: %s: program chain %d runs past the end of VTS_PGCIT: it is left out\n",
           name, i + 1);
    free_pgc(&p);
    return 0;
}

static int load_pgcit(DVDDisc *d, DVDIfo *f, uint32_t sector)
{
    DVDTable tab = { 0 };
    Fields t = { d, f };
    uint32_t *starts = NULL, last;
    int *start_of = NULL, nr, nb_starts = 0, ret;
    char name[16];

    ifo_name(f, name, sizeof(name));
    if ((ret = ff_dvd_ifo_table(d, f, sector, &tab, len_pgcit, 0)) < 0)
        return ret;
    t.p   = tab.data;
    t.len = tab.size;
    nr   = W(&t, 0);
    last = t.len >= 8 ? AV_RB32(t.p + 4) + 1 : (outside(&t, 4, 4), 1);
    if (nr && (!(f->pgcs = av_calloc(nr, sizeof(*f->pgcs))) || !(f->pgc_same_as = av_malloc_array(nr, sizeof(int))) ||
               !(starts = av_malloc_array(nr, sizeof(*starts))) || !(start_of = av_malloc_array(nr, sizeof(int))))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    f->nb_pgcs = nr;
    for (int i = 0; i < nr; i++) {
        uint32_t entry = L(&t, 8 + 8 * i), off = L(&t, 0x0c + 8 * i);
        f->pgc_same_as[i] = -1;
        if ((uint64_t)off + 4 >= last) {
            av_log(d->log, AV_LOG_WARNING, "DVD: %s: program chain %d starts outside VTS_PGCIT (byte %"PRIu32
                   "): it is left out\n", name, i + 1, off);
            continue;
        }
        if (nr != 1) {
            int dup = -1;
            for (int k = 0; k < nb_starts && dup < 0; k++)
                if (starts[k] == off)
                    dup = k;
            if (dup >= 0) {
                f->pgc_same_as[i] = start_of[dup];
                av_log(d->log, AV_LOG_VERBOSE, "DVD: %s: program chain %d starts where program chain %d does: "
                       "it is left out\n", name, i + 1, f->pgc_same_as[i] + 1);
                continue;
            }
            start_of[nb_starts] = i;
            starts[nb_starts++] = off;
        }
        if ((ret = parse_pgc(&t, i, entry, off, &f->pgcs[i])) < 0)
            goto end;
    }
    ret = 0;
end:
    av_free(starts);
    av_free(start_of);
    ff_dvd_table_free(&tab);
    return ret;
}

/* ---- the IFO ---- */

static const char *signature(const DVDIfo *f)
{
    return f->index ? "DVDVIDEO-VTS" : "DVDVIDEO-VMG";
}

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

/* Where the header puts the BUP against where it is. */
static void check_bup(DVDDisc *d, DVDIfo *f)
{
    int64_t want = (int64_t)(ff_dvd_header32(f, DVD_LAST_SECTOR) - ff_dvd_header32(f, DVD_I_LAST_SECTOR)) * DVD_SECTOR;
    int64_t have;
    char name[16];

    ifo_name(f, name, sizeof(name));
    if (d->folder)
        have = f->ifo_bytes + f->menu_vob_bytes + (f->index ? f->title_vob_bytes : 0);
    else
        have = (f->bup_sector - f->ifo_sector) * DVD_SECTOR;
    if (have != want)
        av_log(d->log, AV_LOG_WARNING, "DVD: %s: the header puts the BUP %"PRId64" sectors after the IFO "
               "(vts_last_sector %"PRIu32" - vtsi_last_sector %"PRIu32"), %s %"PRId64" sectors%s\n", name,
               want / DVD_SECTOR, ff_dvd_header32(f, DVD_LAST_SECTOR), ff_dvd_header32(f, DVD_I_LAST_SECTOR),
               d->folder ? "the IFO and VOB files make" : "the file system puts it", have / DVD_SECTOR,
               d->folder && have % DVD_SECTOR ? " (not whole sectors)" : "");
}

static int load(DVDDisc *d, DVDIfo *f)
{
    char name[16];
    uint32_t size, pgcit;
    uint32_t *menu_map = NULL;
    int nb_menu_map = 0, ret;

    ifo_name(f, name, sizeof(name));
    if ((ret = ff_dvd_ifo_read(d, f, 0, f->header, DVD_IFO_HEADER)) < 0)
        return ret;
    if (memcmp(f->header, signature(f), 12)) {
        av_log(d->log, AV_LOG_WARNING, "DVD: %s.IFO does not start with %s: its BUP is read instead\n", name,
               signature(f));
        f->bup_only = 1;
        if ((ret = ff_dvd_ifo_read(d, f, 0, f->header, DVD_IFO_HEADER)) < 0)
            return ret;
        if (memcmp(f->header, signature(f), 12)) {
            av_log(d->log, AV_LOG_ERROR, "DVD: %s: neither the IFO nor the BUP starts with %s: it cannot be "
                   "read\n", name, signature(f));
            return AVERROR_INVALIDDATA;
        }
    }

    /* the whole IFO, once */
    size = (ff_dvd_header32(f, DVD_I_LAST_SECTOR) & 0x1ffff) * DVD_SECTOR;
    if (size > DVD_IFO_CACHE_MAX - DVD_SECTOR) {
        av_log(d->log, AV_LOG_VERBOSE, "DVD: %s: the IFO is larger than %d bytes: its tables are read from the "
               "files\n", name, DVD_IFO_CACHE_MAX);
    } else {
        size += DVD_SECTOR;
        if (!(f->cache = av_malloc(size)))
            return AVERROR(ENOMEM);
        if (read_whole(d, f, f->cache, size) < 0) {
            av_log(d->log, AV_LOG_WARNING, "DVD: %s: the whole IFO (%"PRIu32" bytes) cannot be read: its tables "
                   "are read piece by piece\n", name, size);
            av_freep(&f->cache);
        } else {
            f->cache_size = size;
        }
    }

    if (d->folder)
        f->title_vob_blocks = (f->index ? f->title_vob_bytes : f->menu_vob_bytes) / DVD_SECTOR;
    else
        f->title_vob_blocks = (uint32_t)(f->bup_sector - f->ifo_sector) - ff_dvd_header32(f, DVD_TT_SRPT);
    check_bup(d, f);
    if ((ret = load_vobu_map(d, f, ff_dvd_header32(f, DVD_M_VOBU_ADMAP), &menu_map, &nb_menu_map)) < 0)
        return ret;
    if (ret > 0 && nb_menu_map)
        f->menu_vobu_last = menu_map[nb_menu_map - 1];
    av_freep(&menu_map);

    if (!f->index) {
        const uint8_t *pid = f->header + DVD_PROVIDER_ID;
        for (int i = 0; i + 4 <= 32; i++)
            if (!memcmp(pid + i, "(Fab", 4)) {
                av_log(d->log, AV_LOG_WARNING, "DVD: the provider identifier (%.32s) says the disc was "
                       "processed by DVDFab / MacTheRipper, which are known to rewrite IFOs\n", (const char *)pid);
                break;
            }
        return 0;
    }

    pgcit = ff_dvd_header32(f, DVD_VTS_PGCIT);
    if (pgcit == 0 || pgcit == 0xffffffff) {
        av_log(d->log, AV_LOG_VERBOSE, "DVD: %s has no program chain table\n", name);
        f->no_pgcit = 1;
        return 0;
    }
    if ((ret = load_pgcit(d, f, pgcit)) < 0) {
        av_log(d->log, AV_LOG_ERROR, "DVD: %s: its program chain table (VTS_PGCIT) cannot be read\n", name);
        return ret;
    }
    if ((ret = ff_dvd_ifo_table(d, f, ff_dvd_header32(f, DVD_VTS_PTT_SRPT), &f->ptt_srpt,
                                ff_dvd_table_len_last_byte, 4)) < 0) {
        av_log(d->log, AV_LOG_ERROR, "DVD: %s: its chapter table (VTS_PTT_SRPT) cannot be read\n", name);
        return ret;
    }
    if ((ret = load_vobu_map(d, f, ff_dvd_header32(f, DVD_VTS_VOBU_ADMAP), &f->vobu_map, &f->nb_vobu_map)) < 0)
        return ret;
    if (ret > 0)
        check_cells(d, f);
    if (!d->folder) {
        /* the title VOBs from the IFO header, when the file system agrees
         * (or has no single piece for them) */
        uint32_t start = ff_dvd_header32(f, DVD_TT_SRPT) + (uint32_t)f->ifo_sector;
        if (!f->title_vob_sector || f->title_vob_sector == start)
            f->title_vob_start = start;
        else
            av_log(d->log, AV_LOG_WARNING, "DVD: %s: the IFO header puts the title VOBs at sector %"PRIu32", the "
                   "file system at %"PRId64": decided when they are read\n", name, start, f->title_vob_sector);
    }
    return 0;
}

int ff_dvd_ifo_load(DVDDisc *d, int index, DVDIfo **out)
{
    DVDIfo *f = av_mallocz(sizeof(*f));
    int ret;

    if (!f)
        return AVERROR(ENOMEM);
    f->index = index;
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
    av_freep(&f->cache);
    for (int i = 0; i < f->nb_pgcs; i++)
        free_pgc(&f->pgcs[i]);
    av_freep(&f->pgcs);
    av_freep(&f->pgc_same_as);
    ff_dvd_table_free(&f->ptt_srpt);
    av_freep(&f->vobu_map);
    av_freep(pf);
}
