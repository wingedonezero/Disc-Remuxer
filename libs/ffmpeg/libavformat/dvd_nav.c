/*
 * DVD-Video: a libdvdnav navigator on the disc as the disc layer reads it.
 *
 * libdvdread opens the disc through file callbacks (libdvdnav's
 * dvdnav_open_files, libdvdread's DVDOpenFiles) that serve a virtual
 * "/dvd/VIDEO_TS" folder made from the disc's structure (dvd_disc.c,
 * dvd_ifo.c), so the navigator reads exactly the bytes the rest of the
 * DVD format part reads:
 * - NAME.IFO and NAME.BUP: both the IFO's bytes as dvd_ifo.c reads them
 *   (the IFO, piece by piece from the BUP where the IFO cannot be read),
 *   vmgi/vtsi_last_sector + 1 sectors. A read past those bytes gives zeros
 *   (logged once per file): libdvdread then reads such a table as zeros
 *   instead of failing and trying the BUP. The files are reported as
 *   0x20000 sectors, the most an IFO header can give, so that such reads
 *   reach the callbacks.
 * - the menu VOB (VIDEO_TS.VOB, VTS_nn_0.VOB) and the title VOBs, as one
 *   file VTS_nn_1.VOB: in a disc folder the files (the title VOB files
 *   joined as bytes); in an image the sectors from where the IFO header
 *   puts them (vmgm_vobs / vtsm_vobs, vtstt_vobs, from the IFO's sector):
 *   a title set's menu VOB up to its title VOBs, the title VOBs and the
 *   video manager's menu VOB up to the end of the image.
 * A title set that could not be loaded has no files. Every VOB sector read
 * is handed to the caller's read hook (DVDNavHooks), if any.
 *
 * The disc reader (libdvdread: the disc's structure, the title list and
 * ripping, ff_dvd_reader_open()) gets the disc's own files instead, as the
 * file system has them, each looked up once: the VIDEO_TS folder's IFO and
 * BUP files, menu VOBs (listed, not read) and title VOB files
 * VTS_nn_1..9.VOB, each with its place on an image when it is one piece
 * there (an IFO or BUP that is one piece is read as the image from its
 * place on, its tables being addressed by sector from there), and on an
 * image the whole image (image_open; a folder has none). Which copy of an
 * information file is read and what a missing one means is libdvdread's
 * to decide.
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
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <dvdnav/dvdnav.h>
#include <dvdread/dvd_filesystem.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"

#define NAV_ROOT          "dvd"
#define REPORTS_SAID      20                        /* rule breaks of one file said as warnings */
#define NAV_IFO_REPORTED  (0x20000LL * DVD_SECTOR)  /* the size an IFO / BUP file is reported with */
#define VMGM_VOBS         0xc0                      /* VMG: vmgm_vobs; VTS: vtsm_vobs */

enum { FILE_IFO, FILE_BUP, FILE_MENU_VOB, FILE_TITLE_VOB, FILE_IMAGE };

/* What the virtual folder holds for one information file. */
typedef struct NavSet {
    DVDIfo     *f;
    int         ifo_bytes;              /* the IFO's bytes (vmgi/vtsi_last_sector + 1 sectors) */
    int         zeros_logged;
    int64_t     menu_bytes, title_bytes;
    DiscIOFile *menu, *title;           /* image: the VOBs' sectors (folder: dvd_ifo.c's files) */
} NavSet;

typedef struct NavFS {
    dvd_reader_filesystem_h fs;         /* first: libdvdread hands this back */
    DVDDisc *d;
    /* the disc reader's view (ff_dvd_reader_open()): the disc's own files
     * and the image; else the navigator's: the IFO bytes libdvdread chose,
     * the BUP the same, the VOBs */
    int      real;
    DVDNavHooks hooks;
    NavSet   sets[DVD_MAX_TITLE_SETS + 1];
    char   **listing;                   /* the disc reader's view: the VIDEO_TS file names */
    int      nb_listing, listed;
} NavFS;

typedef struct NavDir {
    NavFS *n;
    int    video_ts;                    /* listing VIDEO_TS (else the root) */
    int    next;                        /* the next entry to list */
} NavDir;

typedef struct NavFile {
    NavFS   *n;
    NavSet  *s;
    int      kind;
    int      part;                      /* the disc reader's view: the title VOB file's number (1..9) */
    DiscIOFile *image;                  /* FILE_IMAGE: the whole image */
    DiscIOFile *real;                   /* the disc reader's view: what is read */
    int64_t  pos;
} NavFile;

static void set_name(int index, char *buf, size_t size)
{
    if (index)
        snprintf(buf, size, "VTS_%02d", index);
    else
        snprintf(buf, size, "VIDEO_TS");
}

/* ---- the folder ---- */

/* The path's components after the root: -1 not ours, 0 the root,
 * 1 VIDEO_TS, 2 a file of VIDEO_TS (its name in *name). */
static int parse_path(const char *path, const char **name)
{
    const char *c[4];
    int len[4], n = 0;

    for (const char *p = path; *p; ) {
        const char *e;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        for (e = p; *e && *e != '/'; e++)
            ;
        if (n == 4)
            return -1;
        c[n] = p;
        len[n++] = e - p;
        p = e;
    }
    if (n < 1 || n > 3 || len[0] != strlen(NAV_ROOT) || strncmp(c[0], NAV_ROOT, len[0]))
        return -1;
    if (n >= 2 && (len[1] != 8 || strncasecmp(c[1], "VIDEO_TS", 8)))
        return -1;
    if (n == 3)
        *name = c[2];
    return n - 1;
}

/* ---- the disc reader's view: the disc's own files ---- */

/* The information file and slot (0 IFO, 1 BUP, 2 menu VOB, 3.. title VOB
 * file 1..9) a VIDEO_TS file name stands for; < 0 when it is none. */
static int real_slot(const char *name, int *set, int *slot)
{
    static const char *const ext[3] = { "IFO", "BUP", "VOB" };
    int vts, part, len = strlen(name);

    if (len == 12 && !strncasecmp(name, "VIDEO_TS.", 9)) {
        for (int k = 0; k < 3; k++)
            if (!strcasecmp(name + 9, ext[k])) {
                *set  = 0;
                *slot = k;
                return 0;
            }
        return -1;
    }
    if (len != 12 || strncasecmp(name, "VTS_", 4) || name[6] != '_' || name[8] != '.' ||
        name[4] < '0' || name[4] > '9' || name[5] < '0' || name[5] > '9' || name[7] < '0' || name[7] > '9')
        return -1;
    vts  = (name[4] - '0') * 10 + name[5] - '0';
    part = name[7] - '0';
    if (vts < 1)
        return -1;
    for (int k = 0; k < 3; k++)
        if (!strcasecmp(name + 9, ext[k])) {
            if (k < 2 && part)
                return -1;
            *set  = vts;
            *slot = k < 2 ? k : 2 + part;
            return 0;
        }
    return -1;
}

static int slot_kind(int slot)
{
    return slot == 0 ? FILE_IFO : slot == 1 ? FILE_BUP : slot == 2 ? FILE_MENU_VOB : FILE_TITLE_VOB;
}

/* A file's first sector on the image when it is one piece there, else -1. */
static int64_t one_piece_at(const DiscIOFile *f)
{
    return f && f->nb_extents == 1 && !f->data && f->extents[0].sector != DISCIO_SECTOR_NOT_RECORDED ?
           f->extents[0].sector : -1;
}

/* The disc's file in set / slot (NULL: there is none). */
static const DVDDiscFile *real_file(NavFS *n, int set, int slot)
{
    return ff_dvd_disc_file(n->d, set, slot);
}

/* The disc reader's view: the VIDEO_TS file names of the disc. */
static int real_list_entry(void *opaque, const char *name, int is_dir)
{
    NavFS *n = opaque;
    int set, slot;
    char *s;

    if (is_dir || real_slot(name, &set, &slot) < 0)
        return 0;
    if (!(s = av_strdup(name)) || av_dynarray_add_nofree(&n->listing, &n->nb_listing, s) < 0) {
        av_free(s);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static void real_list(NavFS *n)
{
    if (n->listed)
        return;
    n->listed = 1;
    if (n->d->fs->ops->list_dir(n->d->fs, n->d->dir[0] ? n->d->dir : "/", real_list_entry, n) < 0)
        av_log(n->d->log, AV_LOG_WARNING, "DVD: the VIDEO_TS folder cannot be listed\n");
}

/* ---- the navigator's view ---- */

/* The set and kind a VIDEO_TS file name stands for (the disc reader's view:
 * *part a title VOB file's number, *real the disc's file); < 0 when there
 * is no such file. */
static int find_file(NavFS *n, const char *name, NavSet **set, int *kind, int *part, const DVDDiscFile **real)
{
    *real = NULL;
    if (n->real) {
        int i, slot;
        if (real_slot(name, &i, &slot) < 0 || !(*real = real_file(n, i, slot)))
            return -1;
        *set  = &n->sets[i];
        *kind = slot_kind(slot);
        *part = slot >= 3 ? slot - 2 : 0;
        return 0;
    }
    for (int i = 0; i < n->d->nb_ifos && i <= DVD_MAX_TITLE_SETS; i++) {
        NavSet *s = &n->sets[i];
        char base[16], want[32];

        if (!s->f)
            continue;
        set_name(i, base, sizeof(base));
        /* IFO, BUP, menu VOB, title VOB */
        for (int k = 0; k < 4; k++) {
            static const char *const ext[4] = { "IFO", "BUP", "VOB", "VOB" };
            int kd = slot_kind(k);
            if ((kd == FILE_MENU_VOB && !s->menu_bytes) || (kd == FILE_TITLE_VOB && (!i || !s->title_bytes)))
                continue;
            if (i)
                snprintf(want, sizeof(want), "%s_%d.%s", base, k >= 3, ext[k]);
            else
                snprintf(want, sizeof(want), "%s.%s", base, ext[k]);
            if (!strcasecmp(name, want)) {
                *set  = s;
                *kind = kd;
                *part = k >= 3;
                return 0;
            }
        }
    }
    return -1;
}

/* The size of a file: the disc reader's view the disc's file's (an image's
 * IFO / BUP in one piece: up to the end of the image), the navigator's as
 * it serves them. */
static int64_t file_size(NavFS *n, NavSet *s, int kind, const DVDDiscFile *real)
{
    if (real)
        return real->region ? real->region->size : real->file->size;
    return kind <= FILE_BUP ? NAV_IFO_REPORTED : kind == FILE_MENU_VOB ? s->menu_bytes : s->title_bytes;
}

static int nav_stat(dvd_reader_filesystem_h *fs, const char *path, dvdstat_t *st)
{
    NavFS *n = (NavFS *)fs;
    const char *name = NULL;
    const DVDDiscFile *real;
    NavSet *s;
    int kind, part, depth = parse_path(path, &name);

    if (depth < 0)
        return -1;
    if (depth < 2) {
        st->size    = 0;
        st->st_mode = DVD_S_IFDIR;
        return 0;
    }
    if (find_file(n, name, &s, &kind, &part, &real) < 0)
        return -1;
    st->size    = file_size(n, s, kind, real);
    st->st_mode = DVD_S_IFREG;
    st->sector  = -1;
    if (real && !n->d->folder)          /* where the file starts on the image */
        st->sector = one_piece_at(real->file);
    return 0;
}

static void *nav_dir_open(dvd_reader_filesystem_h *fs, const char *path)
{
    const char *name = NULL;
    int depth = parse_path(path, &name);
    NavDir *dir;

    if (depth < 0 || depth > 1 || !(dir = av_mallocz(sizeof(*dir))))
        return NULL;
    dir->n        = (NavFS *)fs;
    dir->video_ts = depth == 1;
    return dir;
}

/* Every file name of the folder, in turn: the disc reader's view the disc's
 * own; the navigator's per set IFO, BUP, menu VOB, title VOB. */
static int nav_dir_read(void *p, dvd_dirent_t *entry)
{
    NavDir *dir = p;
    NavFS *n = dir->n;

    if (!dir->video_ts) {
        if (dir->next++)
            return 1;
        snprintf(entry->d_name, sizeof(entry->d_name), "VIDEO_TS");
        return 0;
    }
    if (n->real) {
        real_list(n);
        if (dir->next >= n->nb_listing)
            return 1;
        snprintf(entry->d_name, sizeof(entry->d_name), "%s", n->listing[dir->next++]);
        return 0;
    }
    for (; dir->next < 4 * (DVD_MAX_TITLE_SETS + 1); dir->next++) {
        int i = dir->next / 4, k = dir->next % 4;
        NavSet *s = &n->sets[i];
        char base[16];

        if (i >= n->d->nb_ifos || !s->f)
            continue;
        if ((k == 2 && !s->menu_bytes) || (k == 3 && (!i || !s->title_bytes)))
            continue;
        set_name(i, base, sizeof(base));
        if (i)
            snprintf(entry->d_name, sizeof(entry->d_name), "%s_%d.%s", base, k >= 3, k == 0 ? "IFO" : k == 1 ?
                     "BUP" : "VOB");
        else
            snprintf(entry->d_name, sizeof(entry->d_name), "%s.%s", base, k == 0 ? "IFO" : k == 1 ? "BUP" : "VOB");
        dir->next++;
        return 0;
    }
    return 1;
}

static void nav_dir_close(void *dir)
{
    av_free(dir);
}

static void *nav_file_open(dvd_reader_filesystem_h *fs, const char *path)
{
    NavFS *n = (NavFS *)fs;
    const char *name = NULL;
    const DVDDiscFile *real;
    NavFile *file;
    NavSet *s;
    int kind, part;

    if (parse_path(path, &name) != 2 || find_file(n, name, &s, &kind, &part, &real) < 0 ||
        (n->real && kind == FILE_MENU_VOB) ||   /* the disc reader reads no menu VOB (yet) */
        !(file = av_mallocz(sizeof(*file))))
        return NULL;
    file->n    = n;
    file->s    = s;
    file->kind = kind;
    file->part = part;
    if (real)
        file->real = real->region ? real->region : real->file;
    return file;
}

/* The disc reader's view of an image: where a title VOB file's pieces lie
 * on it (DiscIOFile extents; a piece not recorded: -1). */
static int nav_file_pieces(dvd_reader_filesystem_h *fs, const char *path, int64_t (*pieces)[2], int max)
{
    NavFS *n = (NavFS *)fs;
    const char *name = NULL;
    const DVDDiscFile *real;
    DiscIOFile *f;
    NavSet *s;
    int kind, part;

    if (n->d->folder || parse_path(path, &name) != 2 || find_file(n, name, &s, &kind, &part, &real) < 0 ||
        kind != FILE_TITLE_VOB || !real || (f = real->file)->data)
        return -1;
    for (int i = 0; i < f->nb_extents && i < max; i++) {
        pieces[i][0] = f->extents[i].sector == DISCIO_SECTOR_NOT_RECORDED ? -1 : f->extents[i].sector;
        pieces[i][1] = f->extents[i].count;
    }
    return f->nb_extents;
}

/* The disc reader's view of an image: the whole image as one file. */
static void *nav_image_open(dvd_reader_filesystem_h *fs)
{
    NavFS *n = (NavFS *)fs;
    int64_t blocks = (n->d->fs->src->size + DVD_SECTOR - 1) / DVD_SECTOR;
    NavFile *file;
    DiscIOFile *f;

    if (n->d->folder || !(file = av_mallocz(sizeof(*file))))
        return NULL;
    if (!(f = av_mallocz(sizeof(*f))) || !(f->extents = av_mallocz(sizeof(*f->extents)))) {
        av_free(f);
        av_free(file);
        return NULL;
    }
    f->nb_extents        = 1;
    f->extents[0].sector = 0;
    f->extents[0].count  = blocks;
    f->size              = blocks * DVD_SECTOR;
    file->n     = n;
    file->kind  = FILE_IMAGE;
    file->image = f;
    return file;
}

/* IFO bytes: the IFO's own up to its size, zeros after it. */
static int read_ifo(NavFS *n, NavSet *s, int64_t pos, uint8_t *buf, int len)
{
    DVDIfo *f = s->f;
    int own = pos < s->ifo_bytes ? (int)FFMIN(len, s->ifo_bytes - pos) : 0;

    if (own) {
        if (f->cache) {
            memcpy(buf, f->cache + pos, own);
        } else {
            /* whole sectors through dvd_ifo.c, the part asked for copied out */
            uint8_t sector[DVD_SECTOR];
            for (int done = 0; done < own; ) {
                int64_t at = pos + done;
                int in = at % DVD_SECTOR, k = FFMIN(own - done, DVD_SECTOR - in), ret;
                if ((ret = ff_dvd_ifo_read(n->d, f, at / DVD_SECTOR, sector, DVD_SECTOR)) < 0)
                    return ret;
                memcpy(buf + done, sector + in, k);
                done += k;
            }
        }
    }
    if (own < len) {
        memset(buf + own, 0, len - own);
        if (!s->zeros_logged) {
            char name[16];
            set_name(f->index, name, sizeof(name));
            av_log(n->d->log, AV_LOG_WARNING, "DVD: navigation reads %s.IFO bytes %"PRId64"-%"PRId64" past its "
                   "%d bytes: zeros given [untested on real discs]\n", name, pos + own, pos + len - 1, s->ifo_bytes);
            s->zeros_logged = 1;
        }
    }
    return 0;
}

/* Bytes of the title VOB files of a folder, joined. */
static int read_joined(DVDDisc *d, DVDIfo *f, int64_t pos, uint8_t *buf, int len)
{
    for (int i = 0; i < f->nb_title_vobs && len > 0; i++) {
        DiscIOFile *v = f->title_vobs[i];
        if (pos >= v->size) {
            pos -= v->size;
            continue;
        }
        int k = (int)FFMIN(len, v->size - pos), ret;
        if ((ret = ff_discio_file_read(d->fs, v, pos, buf, k)) < 0)
            return ret;
        buf += k;
        len -= k;
        pos  = 0;
    }
    return len ? AVERROR(EINVAL) : 0;
}

static ssize_t nav_file_read(void *p, char *buf, size_t size)
{
    NavFile *file = p;
    NavSet *s = file->s;
    int64_t total = file->kind == FILE_IMAGE ? file->image->size : file->real ? file->real->size :
                    file_size(file->n, s, file->kind, NULL);
    int len, ret;

    if (file->pos >= total)
        return 0;
    len = (int)FFMIN((int64_t)FFMIN(size, INT_MAX / 2), total - file->pos);
    if (file->n->real) {                /* the real file (or the image); a read error stays one */
        DiscIOFile *f = file->kind == FILE_IMAGE ? file->image : file->real;
        ret = ff_discio_file_read(file->n->d->fs, f, file->pos, (uint8_t *)buf, len);
        if (ret < 0)
            return -1;
        file->pos += len;
        return len;
    }
    switch (file->kind) {
    case FILE_IFO:
    case FILE_BUP:
        ret = read_ifo(file->n, s, file->pos, (uint8_t *)buf, len);
        break;
    case FILE_MENU_VOB:
        ret = ff_discio_file_read(file->n->d->fs, file->n->d->folder ? s->f->menu_vob : s->menu, file->pos,
                                  (uint8_t *)buf, len);
        break;
    default:
        ret = file->n->d->folder ? read_joined(file->n->d, s->f, file->pos, (uint8_t *)buf, len)
                                 : ff_discio_file_read(file->n->d->fs, s->title, file->pos, (uint8_t *)buf, len);
    }
    if (ret < 0)
        return -1;
    if (file->kind > FILE_BUP && file->n->hooks.vob_read) {
        /* whole sectors only (libdvdread reads VOBs by the sector) */
        for (int64_t at = file->pos; at + DVD_SECTOR <= file->pos + len; at += DVD_SECTOR) {
            if (at % DVD_SECTOR)
                break;
            if (file->n->hooks.vob_read(file->n->hooks.opaque, s->f->index, file->kind == FILE_MENU_VOB,
                                        (uint32_t)(at / DVD_SECTOR), (const uint8_t *)buf + (at - file->pos)) < 0)
                return -1;
        }
    }
    file->pos += len;
    return len;
}

static off64_t nav_file_seek(void *p, off64_t offset, int whence)
{
    NavFile *file = p;
    int64_t to = whence == SEEK_SET ? offset :
                 whence == SEEK_CUR ? file->pos + offset :
                 whence == SEEK_END ? (file->kind == FILE_IMAGE ? file->image->size : file->real ? file->real->size :
                                       file_size(file->n, file->s, file->kind, NULL)) + offset : -1;

    if (to < 0)
        return -1;
    file->pos = to;
    return to;
}

static int nav_file_close(void *p)
{
    NavFile *file = p;
    ff_discio_file_free(&file->image);
    av_free(file);
    return 0;
}

static void nav_close(dvd_reader_filesystem_h *fs)
{
    NavFS *n = (NavFS *)fs;

    for (int i = 0; i <= DVD_MAX_TITLE_SETS; i++) {
        ff_discio_file_free(&n->sets[i].menu);
        ff_discio_file_free(&n->sets[i].title);
    }
    for (int i = 0; i < n->nb_listing; i++)
        av_free(n->listing[i]);
    av_free(n->listing);
    av_free(n);
}

int ff_dvd_vob_read(DVDDisc *d, DVDIfo *f, int menu, uint32_t sector, uint8_t *buf)
{
    int64_t pos = (int64_t)sector * DVD_SECTOR, blocks, start, end;
    uint32_t mvobs, tvobs;
    DiscIOExtent e;
    DiscIOFile one = { 0 };

    if (d->folder) {
        if (menu)
            return f->menu_vob ? ff_discio_file_read(d->fs, f->menu_vob, pos, buf, DVD_SECTOR) : AVERROR(EINVAL);
        return read_joined(d, f, pos, buf, DVD_SECTOR);
    }
    /* the same sectors as the navigator's files (setup_set) */
    blocks = (d->fs->src->size + DVD_SECTOR - 1) / DVD_SECTOR;
    mvobs  = ff_dvd_header32(f, VMGM_VOBS);
    tvobs  = f->index ? ff_dvd_header32(f, DVD_TT_SRPT) : 0;
    if (menu ? !mvobs || !f->menu_vob_base : !f->title_vob_base)
        return AVERROR(EINVAL);
    /* the menu VOBs and the title VOBs from where libdvdread decided they
     * start; a title set's menu VOBs up to the IFO header's title VOBs */
    start = menu ? f->menu_vob_base : f->title_vob_base;
    end   = menu && tvobs > mvobs ? f->ifo_sector + tvobs : blocks;
    if (start <= 0 || start + sector >= FFMIN(end, blocks))
        return AVERROR(EINVAL);
    e.sector        = start + sector;
    e.count         = 1;
    one.size        = DVD_SECTOR;
    one.nb_extents  = 1;
    one.extents     = &e;
    return ff_discio_file_read(d->fs, &one, 0, buf, DVD_SECTOR);
}

/* ---- setting it up ---- */

/* An image's sectors from sector on, count of them, as one file (clamped to
 * the image). */
static int image_part(DVDDisc *d, int64_t sector, int64_t count, DiscIOFile **out, int64_t *bytes)
{
    int64_t blocks = (d->fs->src->size + DVD_SECTOR - 1) / DVD_SECTOR;
    DiscIOFile *f;

    *bytes = 0;
    if (sector <= 0 || count <= 0 || sector >= blocks)
        return 0;
    count = FFMIN(count, blocks - sector);
    if (!(f = av_mallocz(sizeof(*f))) || !(f->extents = av_mallocz(sizeof(*f->extents)))) {
        av_free(f);
        return AVERROR(ENOMEM);
    }
    f->nb_extents        = 1;
    f->extents[0].sector = sector;
    f->extents[0].count  = count;
    f->size              = count * DVD_SECTOR;
    *out   = f;
    *bytes = f->size;
    return 0;
}

static int setup_set(DVDDisc *d, NavSet *s, DVDIfo *f)
{
    uint32_t menu  = ff_dvd_header32(f, VMGM_VOBS);
    uint32_t title = f->index ? ff_dvd_header32(f, DVD_TT_SRPT) : 0;   /* vtstt_vobs */
    int ret;

    s->f         = f;
    s->ifo_bytes = ((ff_dvd_header32(f, DVD_I_LAST_SECTOR) & 0x1ffff) + 1) * DVD_SECTOR;
    if (d->folder) {
        s->menu_bytes  = f->menu_vob ? f->menu_vob->size : 0;
        s->title_bytes = f->index ? f->title_vob_bytes : 0;
        return 0;
    }
    /* the menu VOB and the title VOBs from where libdvdread decided they
     * start: the menu VOB up to the title VOBs (a title set's, by the IFO
     * header), else to the end of the image; the title VOBs to the end of
     * the image */
    if (menu && f->menu_vob_base &&
        (ret = image_part(d, f->menu_vob_base, title > menu ? (int64_t)f->ifo_sector + title - f->menu_vob_base :
                          INT64_MAX, &s->menu, &s->menu_bytes)) < 0)
        return ret;
    if (f->title_vob_base && (ret = image_part(d, f->title_vob_base, INT64_MAX, &s->title, &s->title_bytes)) < 0)
        return ret;
    return 0;
}

/* ---- what libdvdread and libdvdnav report ---- */

/* Their log lines (with a reporter set, the lines of findings come at the
 * debug level). Both libraries share the levels INFO, ERROR, WARN, DEBUG. */
static void lib_log(DVDDisc *d, const char *lib, int level, const char *fmt, va_list va)
{
    static const int levels[] = { AV_LOG_VERBOSE, AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_DEBUG };
    char line[1024];

    vsnprintf(line, sizeof(line), fmt, va);
    av_log(d->log, levels[FFMIN((unsigned)level, 3)], "%s: %s\n", lib, line);
}

static void nav_log(void *priv, dvdnav_logger_level_t level, const char *fmt, va_list va)
{
    lib_log(priv, "libdvdnav", level, fmt, va);
}

static void read_log(void *priv, dvd_logger_level_t level, const char *fmt, va_list va)
{
    lib_log(priv, "libdvdread", level, fmt, va);
}

/* A finding, kept once (the navigation reads the same IFO and VOBUs again). */
static void lib_report(void *priv, const dvd_report_t *r)
{
    DVDDisc *d = priv;
    DVDReport *k;

    /* damaged title VOB sectors: the reader gives them to whoever reads
     * (the title reader says them); the report's own line is at the debug level */
    if (r->nb_sectors)
        return;
    /* the CSS steps of the title VOB reading: said as they happen */
    if (r->table && !strcmp(r->table, "CSS")) {
        av_log(d->log, r->level == DVD_REPORT_ERROR ? AV_LOG_ERROR : r->level == DVD_REPORT_WARN ? AV_LOG_WARNING :
               AV_LOG_INFO, "DVD: %s\n", r->text);
        return;
    }
    for (int i = 0; i < d->nb_reports; i++) {
        const DVDReport *o = &d->reports[i];
        if (o->level == (int)r->level && o->offset == r->offset && o->sector == r->sector &&
            !strcmp(o->file, r->file ? r->file : "") && !strcmp(o->table, r->table ? r->table : "") &&
            !strcmp(o->text, r->text))
            return;
    }
    if (d->nb_reports == d->reports_cap) {
        int cap = d->reports_cap ? 2 * d->reports_cap : 32;
        DVDReport *n = av_realloc_array(d->reports, cap, sizeof(*n));
        if (!n)
            return;
        d->reports     = n;
        d->reports_cap = cap;
    }
    k = &d->reports[d->nb_reports];
    memset(k, 0, sizeof(*k));
    if (!(k->text = av_strdup(r->text)))
        return;
    k->level  = r->level;
    k->offset = r->offset;
    k->sector = r->sector;
    snprintf(k->file, sizeof(k->file), "%s", r->file ? r->file : "");
    snprintf(k->table, sizeof(k->table), "%s", r->table ? r->table : "");
    if (r->bytes) {
        k->nb_bytes = (int)FFMIN(r->nb_bytes, sizeof(k->bytes));
        memcpy(k->bytes, r->bytes, k->nb_bytes);
    }
    d->nb_reports++;
}

void ff_dvd_reports_count(const DVDDisc *d, int *harmless, int *breaks)
{
    *harmless = *breaks = 0;
    for (int i = 0; i < d->nb_reports; i++)
        if (d->reports[i].level == DVD_REPORT_INFO)
            (*harmless)++;
        else
            (*breaks)++;
}

static void report_line(void *log, int level, const char *check, const DVDReport *k)
{
    char at[48] = "", bytes[64] = "", where[96];
    if (k->offset >= 0)
        snprintf(at, sizeof(at), " at byte 0x%"PRIx64, k->offset);
    for (int b = 0, p = 0; b < k->nb_bytes && p + 4 < (int)sizeof(bytes); b++)
        p += snprintf(bytes + p, sizeof(bytes) - p, "%s%02x", b ? " " : " (bytes ", k->bytes[b]);
    if (k->nb_bytes)
        av_strlcat(bytes, ")", sizeof(bytes));
    snprintf(where, sizeof(where), "%s%s%s%s%s", k->file, k->file[0] && k->table[0] ? " " : "", k->table, at, bytes);
    av_log(log, level, "%s: %s%s%s\n", check, where, where[0] ? ": " : "", k->text);
}

/* The order of the files in the log: VIDEO_TS, the title sets by number,
 * then the navigation's findings (no file). */
static int file_rank(const char *file)
{
    int n;
    if (!file[0])
        return 1000;
    if (!strncmp(file, "VIDEO_TS", 8))
        return 0;
    return sscanf(file, "VTS_%d", &n) == 1 ? n : 999;
}

void ff_dvd_reports_log(DVDDisc *d)
{
    /* per file, in file order; within a file in the order found */
    for (int rank = 0; rank <= 1000; rank++) {
        for (int i = d->reports_said; i < d->nb_reports; i++) {
            const char *file = d->reports[i].file;
            const char *check = file[0] ? "IFO check" : "Navigation check";
            int harmless = 0, breaks = 0, done = 0;

            if (file_rank(file) != rank)
                continue;
            for (int j = d->reports_said; j < i && !done; j++)
                done = !strcmp(d->reports[j].file, file);
            if (done)
                continue;
            for (int j = i; j < d->nb_reports; j++) {
                const DVDReport *k = &d->reports[j];
                if (strcmp(k->file, file))
                    continue;
                /* rule breaks as warnings, the first REPORTS_SAID of a file;
                 * data that cannot be used always */
                report_line(d->log, k->level == DVD_REPORT_ERROR ||
                            (k->level != DVD_REPORT_INFO && breaks < REPORTS_SAID) ? AV_LOG_WARNING : AV_LOG_VERBOSE,
                            check, k);
                if (k->level == DVD_REPORT_INFO)
                    harmless++;
                else
                    breaks++;
            }
            if (breaks)
                av_log(d->log, AV_LOG_INFO, "%s: %s%s%d finding(s) that break the DVD-Video rules (%s)\n", check,
                       file, file[0] ? ": " : "", breaks,
                       breaks > REPORTS_SAID ? "the first ones above, all in the detail lines" : "above");
            if (harmless)
                av_log(d->log, AV_LOG_INFO, "%s: %s%s%d harmless finding(s) (see the detail lines): kept\n", check,
                       file, file[0] ? ": " : "", harmless);
        }
    }
    d->reports_said = d->nb_reports;
}

/* The callbacks serving the disc's structure to libdvdread (the IFOs as read
 * here, the VOBs from the disc layer). */
static int nav_fs_new(DVDDisc *d, const DVDNavHooks *hooks, int real, NavFS **out)
{
    NavFS *n;
    int ret;

    *out = NULL;
    if (!(n = av_mallocz(sizeof(*n))))
        return AVERROR(ENOMEM);
    n->d    = d;
    n->real = real;
    if (hooks)
        n->hooks = *hooks;
    n->fs = (dvd_reader_filesystem_h){ NULL, nav_close, nav_stat, nav_dir_open, nav_dir_read, nav_dir_close,
                                       nav_file_open, nav_file_read, nav_file_seek, nav_file_close,
                                       real && !d->folder ? nav_image_open : NULL, real ? nav_file_pieces : NULL };
    for (int i = 0; !real && i < d->nb_ifos && i <= DVD_MAX_TITLE_SETS; i++) {
        if (!d->ifos[i] || d->ifos[i]->failed)
            continue;
        if ((ret = setup_set(d, &n->sets[i], d->ifos[i])) < 0) {
            nav_close(&n->fs);
            return ret;
        }
    }
    *out = n;
    return 0;
}

int ff_dvd_reader_open(DVDDisc *d)
{
    static const dvd_logger_cb logcb = { read_log, lib_report };
    NavFS *n;
    int ret;

    if (d->reader)
        return 0;
    if ((ret = nav_fs_new(d, NULL, 1, &n)) < 0)
        return ret;
    /* the reader owns the callbacks once open */
    if (!(d->reader = DVDOpenFiles(d, &logcb, "/" NAV_ROOT, &n->fs))) {
        av_log(d->log, AV_LOG_ERROR, "DVD: libdvdread cannot open the disc's information files\n");
        nav_close(&n->fs);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

int ff_dvd_nav_open(DVDDisc *d, const DVDNavHooks *hooks, dvdnav_t **out)
{
    static const dvdnav_logger_cb logcb = { nav_log, lib_report };
    NavFS *n;
    int ret;

    *out = NULL;
    if ((ret = nav_fs_new(d, hooks, 0, &n)) < 0)
        return ret;
    /* the navigator owns the callbacks from here on, also on failure */
    if (dvdnav_open_files(out, d, &logcb, "/" NAV_ROOT, &n->fs) != DVDNAV_STATUS_OK) {
        av_log(d->log, AV_LOG_ERROR, "DVD: the navigator cannot be opened on the disc\n");
        *out = NULL;
        return AVERROR_EXTERNAL;
    }
    return 0;
}
