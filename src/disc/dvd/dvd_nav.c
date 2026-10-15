/*
 * DVD-Video: a libdvdnav navigator on the disc as the disc layer reads it.
 *
 * libdvdread opens the disc through file callbacks (libdvdnav's
 * dvdnav_open_files, libdvdread's DVDOpenFiles) that serve a virtual
 * "/dvd/VIDEO_TS" folder made from the disc's structure (dvd_disc.c,
 * dvd_ifo.c, libdvdread's disc structure), so the navigator reads exactly the bytes the rest of the
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

#include "base/macros.h"
#include "base/mem.h"
#include "base/err.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "dvd_internal.h"

#define NAV_ROOT          "dvd"
#define NAV_IFO_REPORTED  (0x20000LL * DVD_SECTOR)  /* the size an IFO / BUP file is reported with */
#define VMGM_VOBS         0xc0                      /* VMG: vmgm_vobs; VTS: vtsm_vobs */

enum { FILE_IFO, FILE_BUP, FILE_MENU_VOB, FILE_TITLE_VOB, FILE_IMAGE };

/* What the virtual folder holds for one information file. */
typedef struct NavSet {
    int         set;                    /* its information file (0: the VMG) */
    int         used;                   /* the disc has it (libdvdread read it) */
    int         ifo_bytes;              /* the IFO's bytes (vmgi/vtsi_last_sector + 1 sectors) */
    int         zeros_logged;
    int64_t     menu_bytes, title_bytes;
    DiscIOFile *menu, *title;           /* image: the VOBs' sectors */
    /* folder: the disc's menu VOB file and title VOB files (up to the first
     * missing one; the VMG's title VOB stream is its menu VOB) */
    DiscIOFile *menu_file, *title_files[9];
    int         nb_title_files;
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
    if (!(s = dr_strdup(name)) || dr_array_add_ptr(&n->listing, &n->nb_listing, s) < 0) {
        dr_free(s);
        return DR_ERR(ENOMEM);
    }
    return 0;
}

static void real_list(NavFS *n)
{
    if (n->listed)
        return;
    n->listed = 1;
    if (n->d->fs->ops->list_dir(n->d->fs, n->d->dir[0] ? n->d->dir : "/", real_list_entry, n) < 0)
        dr_vlog(n->d->log, DR_VLOG_WARNING, "DVD: the VIDEO_TS folder cannot be listed\n");
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
    for (int i = 0; i < ff_dvd_nb_sets(n->d) && i <= DVD_MAX_TITLE_SETS; i++) {
        NavSet *s = &n->sets[i];
        char base[16], want[32];

        if (!s->used)
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

    if (depth < 0 || depth > 1 || !(dir = dr_mallocz(sizeof(*dir))))
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

        if (i >= ff_dvd_nb_sets(n->d) || !s->used)
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
    dr_free(dir);
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
        !(file = dr_mallocz(sizeof(*file))))
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

    if (n->d->folder || !(file = dr_mallocz(sizeof(*file))))
        return NULL;
    if (!(f = dr_mallocz(sizeof(*f))) || !(f->extents = dr_mallocz(sizeof(*f->extents)))) {
        dr_free(f);
        dr_free(file);
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
    const dvd_ifo_rec_t *f = ff_dvd_set(n->d, s->set);
    int own = pos < s->ifo_bytes ? (int)DR_MIN(len, s->ifo_bytes - pos) : 0;

    if (own) {
        if (f->whole) {                 /* from the whole IFO libdvdread read */
            if (!ifoRead_rec_bytes(f->ifo, pos, buf, own))
                return DR_ERR_INVALID;
        } else {
            /* whole sectors through dvd_ifo.c, the part asked for copied out */
            uint8_t sector[DVD_SECTOR];
            for (int done = 0; done < own; ) {
                int64_t at = pos + done;
                int in = at % DVD_SECTOR, k = DR_MIN(own - done, DVD_SECTOR - in), ret;
                if ((ret = ff_dvd_ifo_read(n->d, s->set, at / DVD_SECTOR, sector, DVD_SECTOR)) < 0)
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
            set_name(s->set, name, sizeof(name));
            dr_vlog(n->d->log, DR_VLOG_WARNING, "DVD: navigation reads %s.IFO bytes %"PRId64"-%"PRId64" past its "
                   "%d bytes: zeros given [untested on real discs]\n", name, pos + own, pos + len - 1, s->ifo_bytes);
            s->zeros_logged = 1;
        }
    }
    return 0;
}

/* Bytes of the title VOB files of a folder, joined. */
static int read_joined(DVDDisc *d, const NavSet *s, int64_t pos, uint8_t *buf, int len)
{
    for (int i = 0; i < s->nb_title_files && len > 0; i++) {
        DiscIOFile *v = s->title_files[i];
        if (pos >= v->size) {
            pos -= v->size;
            continue;
        }
        int k = (int)DR_MIN(len, v->size - pos), ret;
        if ((ret = ff_discio_file_read(d->fs, v, pos, buf, k)) < 0)
            return ret;
        buf += k;
        len -= k;
        pos  = 0;
    }
    return len ? DR_ERR(EINVAL) : 0;
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
    len = (int)DR_MIN((int64_t)DR_MIN(size, INT_MAX / 2), total - file->pos);
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
        ret = ff_discio_file_read(file->n->d->fs, file->n->d->folder ? s->menu_file : s->menu, file->pos,
                                  (uint8_t *)buf, len);
        break;
    default:
        ret = file->n->d->folder ? read_joined(file->n->d, s, file->pos, (uint8_t *)buf, len)
                                 : ff_discio_file_read(file->n->d->fs, s->title, file->pos, (uint8_t *)buf, len);
    }
    if (ret < 0)
        return -1;
    if (file->kind > FILE_BUP && file->n->hooks.vob_read) {
        /* whole sectors only (libdvdread reads VOBs by the sector) */
        for (int64_t at = file->pos; at + DVD_SECTOR <= file->pos + len; at += DVD_SECTOR) {
            if (at % DVD_SECTOR)
                break;
            if (file->n->hooks.vob_read(file->n->hooks.opaque, s->set, file->kind == FILE_MENU_VOB,
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
    dr_free(file);
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
        dr_free(n->listing[i]);
    dr_free(n->listing);
    dr_free(n);
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
    count = DR_MIN(count, blocks - sector);
    if (!(f = dr_mallocz(sizeof(*f))) || !(f->extents = dr_mallocz(sizeof(*f->extents)))) {
        dr_free(f);
        return DR_ERR(ENOMEM);
    }
    f->nb_extents        = 1;
    f->extents[0].sector = sector;
    f->extents[0].count  = count;
    f->size              = count * DVD_SECTOR;
    *out   = f;
    *bytes = f->size;
    return 0;
}

static int setup_set(DVDDisc *d, NavSet *s, int set)
{
    const dvd_ifo_rec_t *f = ff_dvd_set(d, set);
    uint32_t menu  = ff_dvd_header32(f, VMGM_VOBS);
    uint32_t title = set ? ff_dvd_header32(f, DVD_TT_SRPT) : 0;      /* vtstt_vobs */
    int ret;

    s->set       = set;
    s->used      = 1;
    s->ifo_bytes = ((ff_dvd_header32(f, DVD_I_LAST_SECTOR) & 0x1ffff) + 1) * DVD_SECTOR;
    if (d->folder) {
        const DVDDiscFile *r;
        if ((r = ff_dvd_disc_file(d, set, 2))) {
            s->menu_file  = r->file;
            s->menu_bytes = r->file->size;
        }
        for (int part = 1; set && part <= 9 && (r = ff_dvd_disc_file(d, set, 2 + part)); part++) {
            s->title_files[s->nb_title_files++] = r->file;
            s->title_bytes += r->file->size;
        }
        if (!set && s->menu_file)       /* the VMG's title VOB stream: its menu VOB */
            s->title_files[s->nb_title_files++] = s->menu_file;
        return 0;
    }
    /* the menu VOB and the title VOBs from where libdvdread decided they
     * start: the menu VOB up to the title VOBs (a title set's, by the IFO
     * header), else to the end of the image; the title VOBs to the end of
     * the image */
    if (menu && f->menu_vob_start &&
        (ret = image_part(d, f->menu_vob_start, title > menu ? f->ifo_sector + title - f->menu_vob_start :
                          INT64_MAX, &s->menu, &s->menu_bytes)) < 0)
        return ret;
    if (f->title_vob_start && (ret = image_part(d, f->title_vob_start, INT64_MAX, &s->title, &s->title_bytes)) < 0)
        return ret;
    return 0;
}

/* ---- what libdvdread and libdvdnav say: one log ---- */

/* Their lines into the log, as the DVD engine's ("DVD: "); both libraries
 * share the levels INFO (details), ERROR, WARN, DEBUG and NOTICE. The
 * navigation scan speaks the same way (dvd_scan.c). */
void ff_dvd_lib_vlog(DVDDisc *d, int level, const char *fmt, va_list va)
{
    static const int levels[] = { DR_VLOG_VERBOSE, DR_VLOG_ERROR, DR_VLOG_WARNING, DR_VLOG_DEBUG, DR_VLOG_INFO };
    char line[1024];
    size_t n;

    vsnprintf(line, sizeof(line), fmt, va);
    n = strlen(line);
    if (n && line[n - 1] == '\n')
        line[n - 1] = 0;
    dr_vlog(d->log, levels[DR_MIN((unsigned)level, 4)], "DVD: %s\n", line);
}

void ff_dvd_lib_log(DVDDisc *d, int level, const char *fmt, ...)
{
    va_list va;

    va_start(va, fmt);
    ff_dvd_lib_vlog(d, level, fmt, va);
    va_end(va);
}

static void nav_log(void *priv, dvdnav_logger_level_t level, const char *fmt, va_list va)
{
    ff_dvd_lib_vlog(priv, level, fmt, va);
}

static void read_log(void *priv, dvd_logger_level_t level, const char *fmt, va_list va)
{
    ff_dvd_lib_vlog(priv, level, fmt, va);
}

static void findings_say(void *priv, dvd_logger_level_t level, const char *line)
{
    DVDDisc *d = priv;
    dr_vlog(d->log, level == DVD_LOGGER_LEVEL_NOTICE ? DR_VLOG_INFO : DR_VLOG_VERBOSE, "DVD: %s\n", line);
}

void ff_dvd_report(DVDDisc *d, const dvd_report_t *r)
{
    /* the reader's own logger (ff_dvd_reader_open()) */
    dvd_logger_cb logcb = { read_log, NULL, d->findings };

    DVDReadReport(d, &logcb, r);
}

void ff_dvd_reports_count(const DVDDisc *d, int *harmless, int *breaks)
{
    DVDFindingsCount(d->findings, harmless, breaks);
}

void ff_dvd_reports_log(DVDDisc *d)
{
    DVDFindingsSummary(d->findings, findings_say, d);
}

/* The disc's findings book (said once, counted), shared by every reader. */
static int findings_book(DVDDisc *d)
{
    if (!d->findings && !(d->findings = DVDFindingsNew()))
        return DR_ERR(ENOMEM);
    return 0;
}

int ff_dvd_nav_logger(DVDDisc *d, dvdnav_logger_cb *cb)
{
    int ret = findings_book(d);
    *cb = (dvdnav_logger_cb){ nav_log, NULL, d->findings };
    return ret;
}

/* The callbacks serving the disc's structure to libdvdread (the IFOs as read
 * here, the VOBs from the disc layer). */
static int nav_fs_new(DVDDisc *d, const DVDNavHooks *hooks, int real, NavFS **out)
{
    NavFS *n;
    int ret;

    *out = NULL;
    if (!(n = dr_mallocz(sizeof(*n))))
        return DR_ERR(ENOMEM);
    n->d    = d;
    n->real = real;
    if (hooks)
        n->hooks = *hooks;
    n->fs = (dvd_reader_filesystem_h){ NULL, nav_close, nav_stat, nav_dir_open, nav_dir_read, nav_dir_close,
                                       nav_file_open, nav_file_read, nav_file_seek, nav_file_close,
                                       real && !d->folder ? nav_image_open : NULL, real ? nav_file_pieces : NULL };
    for (int i = 0; !real && i < ff_dvd_nb_sets(d) && i <= DVD_MAX_TITLE_SETS; i++) {
        if (!ff_dvd_set(d, i))
            continue;
        if ((ret = setup_set(d, &n->sets[i], i)) < 0) {
            nav_close(&n->fs);
            return ret;
        }
    }
    *out = n;
    return 0;
}

int ff_dvd_reader_open(DVDDisc *d)
{
    dvd_logger_cb logcb = { read_log, NULL, NULL };
    NavFS *n;
    int ret;

    if (d->reader)
        return 0;
    if ((ret = findings_book(d)) < 0 || (ret = nav_fs_new(d, NULL, 1, &n)) < 0)
        return ret;
    logcb.findings = d->findings;
    /* the reader owns the callbacks once open */
    if (!(d->reader = DVDOpenFiles(d, &logcb, "/" NAV_ROOT, &n->fs))) {
        dr_vlog(d->log, DR_VLOG_ERROR, "DVD: libdvdread cannot open the disc's information files\n");
        nav_close(&n->fs);
        return DR_ERR_EXTERNAL;
    }
    d->read_fs = &n->fs;
    return 0;
}

int ff_dvd_nav_open(DVDDisc *d, const DVDNavHooks *hooks, dvdnav_t **out)
{
    dvdnav_logger_cb logcb;
    NavFS *n;
    int ret;

    *out = NULL;
    if ((ret = ff_dvd_nav_logger(d, &logcb)) < 0 || (ret = nav_fs_new(d, hooks, 0, &n)) < 0)
        return ret;
    /* the navigator owns the callbacks from here on, also on failure */
    if (dvdnav_open_files(out, d, &logcb, "/" NAV_ROOT, &n->fs) != DVDNAV_STATUS_OK) {
        dr_vlog(d->log, DR_VLOG_ERROR, "DVD: the navigator cannot be opened on the disc\n");
        *out = NULL;
        return DR_ERR_EXTERNAL;
    }
    return 0;
}
