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
 *   joined as bytes); in an image the sectors the IFO header gives from the
 *   IFO's sector (vmgm_vobs / vtsm_vobs, vtstt_vobs; the BUP's offset from
 *   vmg/vts_last_sector - vmgi/vtsi_last_sector).
 * A title set that could not be loaded has no files.
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

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"

#define NAV_ROOT          "dvd"
#define NAV_IFO_REPORTED  (0x20000LL * DVD_SECTOR)  /* the size an IFO / BUP file is reported with */
#define VMGM_VOBS         0xc0                      /* VMG: vmgm_vobs; VTS: vtsm_vobs */

enum { FILE_IFO, FILE_MENU_VOB, FILE_TITLE_VOB };

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
    NavSet   sets[DVD_MAX_TITLE_SETS + 1];
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

/* The set and kind a VIDEO_TS file name stands for; < 0 when there is no
 * such file. */
static int find_file(NavFS *n, const char *name, NavSet **set, int *kind)
{
    for (int i = 0; i <= n->d->nr_of_title_sets && i < n->d->nb_ifos; i++) {
        NavSet *s = &n->sets[i];
        char base[16], want[32];

        if (!s->f)
            continue;
        set_name(i, base, sizeof(base));
        for (int k = 0; k < 4; k++) {
            static const char *const ext[4] = { "IFO", "BUP", "VOB", "VOB" };
            int kd = k < 2 ? FILE_IFO : k == 2 ? FILE_MENU_VOB : FILE_TITLE_VOB;
            if (kd == FILE_MENU_VOB && !s->menu_bytes)
                continue;
            if (kd == FILE_TITLE_VOB && (!i || !s->title_bytes))
                continue;
            if (i)
                snprintf(want, sizeof(want), "%s_%d.%s", base, kd == FILE_TITLE_VOB, ext[k]);
            else
                snprintf(want, sizeof(want), "%s.%s", base, ext[k]);
            if (!strcasecmp(name, want)) {
                *set  = s;
                *kind = kd;
                return 0;
            }
        }
    }
    return -1;
}

static int64_t file_size(const NavSet *s, int kind)
{
    return kind == FILE_IFO ? NAV_IFO_REPORTED : kind == FILE_MENU_VOB ? s->menu_bytes : s->title_bytes;
}

static int nav_stat(dvd_reader_filesystem_h *fs, const char *path, dvdstat_t *st)
{
    NavFS *n = (NavFS *)fs;
    const char *name = NULL;
    NavSet *s;
    int kind, depth = parse_path(path, &name);

    if (depth < 0)
        return -1;
    if (depth < 2) {
        st->size    = 0;
        st->st_mode = DVD_S_IFDIR;
        return 0;
    }
    if (find_file(n, name, &s, &kind) < 0)
        return -1;
    st->size    = file_size(s, kind);
    st->st_mode = DVD_S_IFREG;
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

/* Every file name of the folder, in turn: per set IFO, BUP, menu VOB, title VOB. */
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
            snprintf(entry->d_name, sizeof(entry->d_name), "%s_%d.%s", base, k == 3, k == 0 ? "IFO" : k == 1 ? "BUP" : "VOB");
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
    NavFile *file;
    NavSet *s;
    int kind;

    if (parse_path(path, &name) != 2 || find_file(n, name, &s, &kind) < 0 || !(file = av_mallocz(sizeof(*file))))
        return NULL;
    file->n    = n;
    file->s    = s;
    file->kind = kind;
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
static int read_joined(NavFS *n, DVDIfo *f, int64_t pos, uint8_t *buf, int len)
{
    for (int i = 0; i < f->nb_title_vobs && len > 0; i++) {
        DiscIOFile *v = f->title_vobs[i];
        if (pos >= v->size) {
            pos -= v->size;
            continue;
        }
        int k = (int)FFMIN(len, v->size - pos), ret;
        if ((ret = ff_discio_file_read(n->d->fs, v, pos, buf, k)) < 0)
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
    int64_t total = file_size(s, file->kind);
    int len, ret;

    if (file->pos >= total)
        return 0;
    len = (int)FFMIN((int64_t)FFMIN(size, INT_MAX / 2), total - file->pos);
    switch (file->kind) {
    case FILE_IFO:
        ret = read_ifo(file->n, s, file->pos, (uint8_t *)buf, len);
        break;
    case FILE_MENU_VOB:
        ret = ff_discio_file_read(file->n->d->fs, file->n->d->folder ? s->f->menu_vob : s->menu, file->pos,
                                  (uint8_t *)buf, len);
        break;
    default:
        ret = file->n->d->folder ? read_joined(file->n, s->f, file->pos, (uint8_t *)buf, len)
                                 : ff_discio_file_read(file->n->d->fs, s->title, file->pos, (uint8_t *)buf, len);
    }
    if (ret < 0)
        return -1;
    file->pos += len;
    return len;
}

static off64_t nav_file_seek(void *p, off64_t offset, int whence)
{
    NavFile *file = p;
    int64_t to = whence == SEEK_SET ? offset :
                 whence == SEEK_CUR ? file->pos + offset :
                 whence == SEEK_END ? file_size(file->s, file->kind) + offset : -1;

    if (to < 0)
        return -1;
    file->pos = to;
    return to;
}

static int nav_file_close(void *file)
{
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
    av_free(n);
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
    uint32_t bup   = ff_dvd_header32(f, DVD_LAST_SECTOR) - ff_dvd_header32(f, DVD_I_LAST_SECTOR);
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
    if (menu && (ret = image_part(d, f->ifo_sector + menu, (title ? title : bup) - menu, &s->menu, &s->menu_bytes)) < 0)
        return ret;
    if (title && (ret = image_part(d, f->ifo_sector + title, bup - title, &s->title, &s->title_bytes)) < 0)
        return ret;
    return 0;
}

static void nav_log(void *priv, dvdnav_logger_level_t level, const char *fmt, va_list va)
{
    /* DVDNAV_LOGGER_LEVEL_INFO, _ERROR, _WARN, _DEBUG */
    static const int levels[] = { AV_LOG_VERBOSE, AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_DEBUG };
    char line[1024];

    vsnprintf(line, sizeof(line), fmt, va);
    av_log(priv, levels[FFMIN((unsigned)level, 3)], "libdvdnav: %s\n", line);
}

int ff_dvd_nav_open(DVDDisc *d, dvdnav_t **out)
{
    static const dvdnav_logger_cb logcb = { nav_log };
    NavFS *n;
    int ret;

    *out = NULL;
    if (!(n = av_mallocz(sizeof(*n))))
        return AVERROR(ENOMEM);
    n->d  = d;
    n->fs = (dvd_reader_filesystem_h){ NULL, nav_close, nav_stat, nav_dir_open, nav_dir_read, nav_dir_close,
                                       nav_file_open, nav_file_read, nav_file_seek, nav_file_close };
    for (int i = 0; i < d->nb_ifos && i <= DVD_MAX_TITLE_SETS; i++) {
        if (!d->ifos[i] || d->ifos[i]->failed)
            continue;
        if ((ret = setup_set(d, &n->sets[i], d->ifos[i])) < 0) {
            nav_close(&n->fs);
            return ret;
        }
    }
    /* the navigator owns the callbacks from here on, also on failure */
    if (dvdnav_open_files(out, d->log, &logcb, "/" NAV_ROOT, &n->fs) != DVDNAV_STATUS_OK) {
        av_log(d->log, AV_LOG_ERROR, "DVD: the navigator cannot be opened on the disc\n");
        *out = NULL;
        return AVERROR_EXTERNAL;
    }
    return 0;
}
