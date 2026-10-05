/*
 * DVD-Video demuxer: the disc source. Every byte the demuxer and the DVD
 * libraries read comes through the disc readers (discio): a disc image is
 * read through the file system ff_discio_mount_image() chooses, a disc folder
 * as host files; libdvdread and libdvdnav get their files from here
 * (DVDOpenFiles / dvdnav_open_files). CSS is handled here too: libdvdcss
 * finds a title set's key from the disc data (read through discio as well)
 * and the demuxer descrambles the blocks it receives.
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

#include <dirent.h>
#include <stdarg.h>
#include <dvdcss/dvdcss.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discio.h"
#include "dvdvideo_internal.h"

/* CSS state of one VOB group (a title set's menu or title VOBs) */
enum {
    CSS_UNKNOWN = 0,    /* not looked for yet */
    CSS_KEY,            /* key found */
    CSS_CLEAR,          /* libdvdcss found no scrambled sector: no key */
    CSS_FAILED,         /* the key could not be found: not tried again */
};

typedef struct CSSGroup {
    int     state;
    int     warned;     /* a scrambled sector without a key was reported */
    uint8_t key[DVDCSS_KEY_SIZE];
} CSSGroup;

/* One file of a VOB group of a disc folder */
typedef struct VOBPiece {
    DiscIOSource *host;
    int64_t       start;         /* position of its first byte in the group */
} VOBPiece;

/* A VOB group: the menu VOB of a title set (VIDEO_TS.VOB for the VMG) or its
 * title VOBs. Disc folder: the bytes of the files VTS_nn_1.VOB ..
 * VTS_nn_9.VOB joined (a file that is not a whole number of blocks does not
 * start a new block for the next one); libdvdread reads the group through
 * vob_read too. Disc image: a range of image blocks placed by the IFO (see
 * vob_group_open_image()). */
typedef struct VOBGroup {
    int      nb_pieces;          /* folder: 0 = the group has no file */
    VOBPiece pieces[9];
    int64_t  bytes;              /* folder: the bytes of all its files */
    int      error;              /* folder: a file could not be opened */
    int      in_image;           /* image: the range below is set */
    int64_t  image_start;        /* image: first block of the group */
    int64_t  image_end;          /* image: block after its last one */
} VOBGroup;

struct DVDVideoSource {
    CSSGroup       css[100][2];  /* [title set][1 = menu VOBs, 0 = title VOBs] */
    VOBGroup      *vobs[100][2]; /* as css; made at the first use */
    void          *log;
    char          *path;      /* the path the source was opened from */
    char          *folder;    /* disc folder: the host folder the files are in */
    DiscIOSource  *image;     /* disc image: the image file */
    DiscIOFS      *fs;        /* disc image: its file system */
    int            attempts;  /* read attempts per request */
    int            refs;      /* the demuxer + every file-callback set handed out */
};

static void source_unref(DVDVideoSource **psrc)
{
    DVDVideoSource *src = *psrc;

    *psrc = NULL;
    if (!src || --src->refs > 0)
        return;
    for (int n = 0; n < 100; n++)
        for (int m = 0; m < 2; m++) {
            VOBGroup *g = src->vobs[n][m];

            for (int i = 0; g && i < g->nb_pieces; i++)
                ff_discio_source_free(&g->pieces[i].host);
            av_freep(&src->vobs[n][m]);
        }
    ff_discio_fs_close(&src->fs);
    ff_discio_source_free(&src->image);
    av_freep(&src->folder);
    av_freep(&src->path);
    av_free(src);
}

void ff_dvdvideo_source_close(DVDVideoSource **psrc)
{
    source_unref(psrc);
}

/* ---- paths ---- */

/* A path of libdvdread ("/", "//VIDEO_TS/", "/VIDEO_TS/VTS_01_1.VOB") with
 * repeated and trailing separators dropped; "" becomes "/". */
static void tidy_path(const char *in, char *out, size_t size)
{
    size_t n = 0;

    out[n++] = '/';
    for (const char *p = in; *p && n + 1 < size; p++) {
        if (*p == '/' && out[n - 1] == '/')
            continue;
        out[n++] = *p;
    }
    if (n > 1 && out[n - 1] == '/')
        n--;
    out[n] = 0;
}

static char *host_path(const DVDVideoSource *src, const char *path)
{
    char tidy[4096];

    tidy_path(path, tidy, sizeof(tidy));
    return av_asprintf("%s%s", src->folder, strcmp(tidy, "/") ? tidy : "");
}

/* ---- the file callbacks libdvdread reads through ---- */

typedef struct DirHandle {
    char **names;
    int    nb_names;
    int    next;
} DirHandle;

typedef struct FileHandle {
    DVDVideoSource *src;
    DiscIOFile     *file;     /* disc image: the file in the image */
    DiscIOSource   *host;     /* disc folder: the host file */
    int64_t         size;
    int64_t         pos;
    char           *path;     /* as libdvdread named it (tidied) */
    /* an IFO file: its backup copy, opened at the first failed read (a BUP
     * holds the same bytes as its IFO); reads go to the copy that read last */
    int             is_ifo;
    int             bup_tried;
    int             copy;      /* 0 = the IFO, 1 = the BUP */
    DiscIOFile     *bup_file;
    DiscIOSource   *bup_host;
    int64_t         bup_size;
} FileHandle;

/* Open the backup copy of an IFO (same name, .BUP for .IFO). */
static void open_bup(FileHandle *h)
{
    DVDVideoSource *src = h->src;
    size_t n = strlen(h->path);
    char *bup;

    h->bup_tried = 1;
    if (!(bup = av_strdup(h->path)))
        return;
    memcpy(bup + n - 3, h->path[n - 3] == 'I' ? "BUP" : "bup", 3);
    if (src->folder) {
        char *p = host_path(src, bup);

        if (p && ff_discio_source_open_file(src->log, p, &h->bup_host) >= 0) {
            h->bup_host->attempts = src->attempts;
            h->bup_size = h->bup_host->size;
        }
        av_free(p);
    } else if (src->fs->ops->open_file(src->fs, bup, &h->bup_file) >= 0) {
        h->bup_size = h->bup_file->size;
    }
    if (!h->bup_file && !h->bup_host)
        av_log(src->log, AV_LOG_ERROR, "%s cannot be opened as the backup copy of %s\n", bup, h->path);
    av_free(bup);
}

static int read_part(FileHandle *h, int64_t pos, uint8_t *buf, int n)
{
    return h->host ? ff_discio_read_bytes(h->host, pos, buf, n, h->host->attempts, 0)
                   : ff_discio_file_read(h->src->fs, h->file, pos, buf, n);
}

/* One try (one read attempt, failures logged at debug level) of n bytes at pos
 * from copy 0 (the IFO) or 1 (its BUP). */
static int read_ifo_copy(FileHandle *h, int copy, int64_t pos, uint8_t *buf, int n)
{
    if (!copy)
        return h->host ? ff_discio_read_bytes(h->host, pos, buf, n, 1, 1)
                       : ff_discio_file_read_attempts(h->src->fs, h->file, pos, buf, n, 1, 1);
    if (pos + n > h->bup_size)
        return AVERROR(EIO);
    return h->bup_host ? ff_discio_read_bytes(h->bup_host, pos, buf, n, 1, 1)
                       : ff_discio_file_read_attempts(h->src->fs, h->bup_file, pos, buf, n, 1, 1);
}

/* After a failed try: the other copy is used from now on (when the IFO has a
 * BUP that can be opened). */
static void switch_ifo_copy(FileHandle *h, int64_t pos, int tries)
{
    if (!h->bup_tried)
        open_bup(h);
    if (h->bup_file || h->bup_host)
        h->copy = !h->copy;
    av_log(h->src->log, AV_LOG_WARNING, "%s: read at byte %"PRId64" failed (try %d of %d); the next try reads the %s\n",
           h->path, pos, tries, DVDVIDEO_IFO_READ_TRIES, h->copy ? "backup copy (BUP)" : "IFO");
}

/* IFO reads: up to DVDVIDEO_IFO_READ_TRIES tries per block, the first one
 * covering the whole request, the others one block each; every failed try
 * moves to the other copy (IFO <-> BUP), and later reads stay on the copy
 * that read last. */
static int read_ifo(FileHandle *h, int64_t pos, uint8_t *buf, int len)
{
    int tries;

    if (read_ifo_copy(h, h->copy, pos, buf, len) >= 0)
        return 0;
    tries = 1;
    switch_ifo_copy(h, pos, tries);
    for (int done = 0; done < len;) {
        int64_t at = pos + done;
        int n = FFMIN(len - done, DISCIO_BLOCK_SIZE - (int)(at % DISCIO_BLOCK_SIZE));

        while (read_ifo_copy(h, h->copy, at, buf + done, n) < 0) {
            if (++tries >= DVDVIDEO_IFO_READ_TRIES) {
                av_log(h->src->log, AV_LOG_ERROR, "%s: block %"PRId64" could not be read from the IFO or its "
                       "backup copy in %d tries\n", h->path, at / DISCIO_BLOCK_SIZE, tries);
                return AVERROR(EIO);
            }
            switch_ifo_copy(h, at, tries);
        }
        tries = 0;
        done += n;
    }
    return 0;
}

static DVDVideoSource *source_of(dvd_reader_filesystem_h *fs)
{
    return fs->internal;
}

static void fs_close(dvd_reader_filesystem_h *fs)
{
    DVDVideoSource *src = source_of(fs);

    source_unref(&src);
    av_free(fs);
}

static int fs_stat(dvd_reader_filesystem_h *fs, const char *path, dvdstat_t *st)
{
    DVDVideoSource *src = source_of(fs);
    char tidy[4096];

    if (src->folder) {
        struct stat hs;
        char *p = host_path(src, path);
        int ret = p ? stat(p, &hs) : -1;

        av_free(p);
        if (ret < 0)
            return -1;
        st->size    = hs.st_size;
        st->st_mode = S_ISDIR(hs.st_mode) ? DVD_S_IFDIR : S_ISREG(hs.st_mode) ? DVD_S_IFREG : 0;
        return 0;
    }

    tidy_path(path, tidy, sizeof(tidy));
    if (src->fs->ops->find_dir(src->fs, tidy) >= 0) {
        st->size    = 0;
        st->st_mode = DVD_S_IFDIR;
        return 0;
    } else {
        DiscIOFile *f = NULL;

        if (src->fs->ops->open_file(src->fs, tidy, &f) < 0)
            return -1;
        st->size    = f->size;
        st->st_mode = DVD_S_IFREG;
        ff_discio_file_free(&f);
        return 0;
    }
}

static int add_name(void *opaque, const char *name, int is_dir)
{
    DirHandle *d = opaque;
    char *copy = av_strdup(name);

    if (!copy || av_dynarray_add_nofree(&d->names, &d->nb_names, copy) < 0) {
        av_free(copy);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static void dir_free(DirHandle *d)
{
    for (int i = 0; i < d->nb_names; i++)
        av_free(d->names[i]);
    av_free(d->names);
    av_free(d);
}

static void *fs_dir_open(dvd_reader_filesystem_h *fs, const char *path)
{
    DVDVideoSource *src = source_of(fs);
    DirHandle *d = av_mallocz(sizeof(*d));
    char tidy[4096];

    if (!d)
        return NULL;
    if (src->folder) {
        char *p = host_path(src, path);
        DIR *dir = p ? opendir(p) : NULL;
        struct dirent *e;

        av_free(p);
        if (!dir) {
            av_free(d);
            return NULL;
        }
        while ((e = readdir(dir)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && add_name(d, e->d_name, 0) < 0)
                break;
        closedir(dir);
        return d;
    }
    tidy_path(path, tidy, sizeof(tidy));
    if (src->fs->ops->list_dir(src->fs, tidy, add_name, d) < 0) {
        dir_free(d);
        return NULL;
    }
    return d;
}

static int fs_dir_read(void *dir, dvd_dirent_t *entry)
{
    DirHandle *d = dir;

    if (d->next >= d->nb_names)
        return 1;
    av_strlcpy(entry->d_name, d->names[d->next++], sizeof(entry->d_name));
    return 0;
}

static void fs_dir_close(void *dir)
{
    dir_free(dir);
}

static void *fs_file_open(dvd_reader_filesystem_h *fs, const char *path)
{
    DVDVideoSource *src = source_of(fs);
    FileHandle *h = av_mallocz(sizeof(*h));
    char tidy[4096];

    if (!h)
        return NULL;
    h->src = src;
    tidy_path(path, tidy, sizeof(tidy));
    if (!(h->path = av_strdup(tidy))) {
        av_free(h);
        return NULL;
    }
    h->is_ifo = strlen(tidy) > 4 && !av_strcasecmp(tidy + strlen(tidy) - 4, ".IFO");
    if (src->folder) {
        char *p = host_path(src, path);

        if (!p || ff_discio_source_open_file(src->log, p, &h->host) < 0) {
            av_free(p);
            av_free(h->path);
            av_free(h);
            return NULL;
        }
        av_free(p);
        h->host->attempts = src->attempts;
        h->size = h->host->size;
        return h;
    }
    if (src->fs->ops->open_file(src->fs, tidy, &h->file) < 0) {
        av_free(h->path);
        av_free(h);
        return NULL;
    }
    h->size = h->file->size;
    return h;
}

static ssize_t fs_file_read(void *file, char *buf, size_t size)
{
    FileHandle *h = file;
    int n = FFMIN(size, (size_t)FFMAX(h->size - h->pos, 0));
    int ret;

    n = FFMIN(n, INT_MAX & ~(DISCIO_BLOCK_SIZE - 1));
    if (!n)
        return 0;
    ret = h->is_ifo ? read_ifo(h, h->pos, (uint8_t *)buf, n) : read_part(h, h->pos, (uint8_t *)buf, n);
    if (ret < 0)
        return -1;
    h->pos += n;
    return n;
}

static off64_t fs_file_seek(void *file, off64_t offset, int whence)
{
    FileHandle *h = file;
    int64_t pos = whence == SEEK_SET ? offset :
                  whence == SEEK_CUR ? h->pos + offset :
                  whence == SEEK_END ? h->size + offset : -1;

    if (pos < 0)
        return -1;
    h->pos = pos;
    return pos;
}

static int fs_file_close(void *file)
{
    FileHandle *h = file;

    ff_discio_file_free(&h->file);
    ff_discio_source_free(&h->host);
    ff_discio_file_free(&h->bup_file);
    ff_discio_source_free(&h->bup_host);
    av_free(h->path);
    av_free(h);
    return 0;
}

/* Folders: libdvdread reads the VOB groups through ff_dvdvideo_source_vob_read
 * (the files' bytes joined, see VOBGroup), not file by file. A group without a
 * readable file is served as empty: libdvdread then has no such file. */
static int64_t fs_vob_blocks(dvd_reader_filesystem_h *fs, int title, int menu)
{
    int64_t bytes;

    if (ff_dvdvideo_source_vob_bytes(source_of(fs), title, menu, &bytes) < 0)
        return 0;
    return (bytes + DISCIO_BLOCK_SIZE - 1) / DISCIO_BLOCK_SIZE;
}

static int fs_vob_read(dvd_reader_filesystem_h *fs, int title, int menu, uint32_t block, size_t count,
                       unsigned char *data)
{
    DVDVideoSource *src = source_of(fs);
    size_t i;

    for (i = 0; i < count && i <= INT_MAX; i++) {
        int ret = ff_dvdvideo_source_vob_read(src, title, menu, (int64_t)block + i, data + i * DISCIO_BLOCK_SIZE,
                                              src->attempts);
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return -1;
    }
    return i;
}

dvd_reader_filesystem_h *ff_dvdvideo_source_files(DVDVideoSource *src)
{
    dvd_reader_filesystem_h *fs = av_mallocz(sizeof(*fs));

    if (!fs)
        return NULL;
    src->refs++;
    fs->internal   = src;
    fs->close      = fs_close;
    fs->stat       = fs_stat;
    fs->dir_open   = fs_dir_open;
    fs->dir_read   = fs_dir_read;
    fs->dir_close  = fs_dir_close;
    fs->file_open  = fs_file_open;
    fs->file_read  = fs_file_read;
    fs->file_seek  = fs_file_seek;
    fs->file_close = fs_file_close;
    if (src->folder) {
        fs->vob_blocks = fs_vob_blocks;
        fs->vob_read   = fs_vob_read;
    }
    return fs;
}

/* The name of the entry of directory dir that equals name ignoring case, as
 * libdvdread looks names up. */
typedef struct FindName {
    const char *want;
    char        found[256];
} FindName;

static int find_name_cb(void *opaque, const char *name, int is_dir)
{
    FindName *f = opaque;

    if (!av_strcasecmp(name, f->want) && !f->found[0])
        av_strlcpy(f->found, name, sizeof(f->found));
    return 0;
}

/* ---- CSS ---- */

/* A discio source read by libdvdcss through its stream callbacks. */
typedef struct CSSStream {
    DiscIOSource *src;
    int64_t       pos;
} CSSStream;

static int css_stream_seek(void *opaque, uint64_t pos)
{
    CSSStream *st = opaque;

    if (pos > (uint64_t)st->src->size)
        return -1;
    st->pos = pos;
    return 0;
}

static int css_stream_read(void *opaque, void *buf, int len)
{
    CSSStream *st = opaque;
    int n = FFMIN(len, FFMAX(st->src->size - st->pos, 0));

    if (n > 0 && ff_discio_read_bytes(st->src, st->pos, buf, n, st->src->attempts, 0) < 0)
        return -1;
    st->pos += n;
    return n;
}

/* libdvdcss's messages into the demuxer's log: its errors as warnings (the
 * source logs what they lead to), the rest at verbose level. */
static void css_log(void *opaque, int level, const char *fmt, va_list args)
{
    char msg[1024];

    vsnprintf(msg, sizeof(msg), fmt, args);
    av_log(opaque, level == DVDCSS_LOG_ERROR ? AV_LOG_WARNING : AV_LOG_VERBOSE, "libdvdcss: %s\n", msg);
}

/* The name of an entry of the directory at path (image: in the image; folder:
 * relative to the disc folder) that equals name ignoring case, in out. */
static int find_entry(DVDVideoSource *src, const char *path, const char *name, char *out, size_t size)
{
    FindName f = { name };

    if (src->folder) {
        char *p = host_path(src, path);
        DIR *dir = p ? opendir(p) : NULL;
        struct dirent *e;

        av_free(p);
        if (!dir)
            return AVERROR(ENOENT);
        while ((e = readdir(dir)))
            find_name_cb(&f, e->d_name, 0);
        closedir(dir);
    } else if (src->fs->ops->list_dir(src->fs, path, find_name_cb, &f) < 0) {
        return AVERROR(ENOENT);
    }
    if (!f.found[0])
        return AVERROR(ENOENT);
    av_strlcpy(out, f.found, size);
    return 0;
}

/* The path of a VOB of the disc ("/VIDEO_TS/VTS_01_1.VOB", or "/VTS_01_1.VOB"
 * when the files lie in the root), looked up as libdvdread does: the root
 * first, then the VIDEO_TS directory, names ignoring case. */
static int find_vob(DVDVideoSource *src, const char *name, char *path, size_t size)
{
    char found[256], dir[256];

    if (find_entry(src, "/", name, found, sizeof(found)) >= 0) {
        snprintf(path, size, "/%s", found);
        return 0;
    }
    if (find_entry(src, "/", "VIDEO_TS", dir, sizeof(dir)) < 0)
        return AVERROR(ENOENT);
    snprintf(path, size, "/%s", dir);
    if (find_entry(src, path, name, found, sizeof(found)) < 0)
        return AVERROR(ENOENT);
    snprintf(path, size, "/%s/%s", dir, found);
    return 0;
}

/* Confirm a title key on sectors first..first + count - 1 of from: the first
 * scrambled sector the content check can test must descramble to valid
 * content. 1 = confirmed (*at = the sector), 0 = no sector could be tested,
 * -1 = every tested sector failed (*tested of them). */
static int css_confirm_key(DVDVideoSource *src, DiscIOSource *from, int64_t first, int64_t count,
                           const uint8_t *key, int64_t *at, int *tested)
{
    uint8_t sec[DVDVIDEO_BLOCK_SIZE];

    *tested = 0;
    for (int64_t s = first; s < first + count; s++) {
        if (ff_discio_read_blocks(from, s * DVDVIDEO_BLOCK_SIZE, sec, sizeof(sec), src->attempts, 1) < 0)
            continue;
        if (ff_dvdvideo_css_scrambled_pes(sec) != 14 || !ff_dvdvideo_css_can_test(sec))
            continue;
        (*tested)++;
        if (dvdcss_unscramble_sector(key, sec) == 1 && ff_dvdvideo_css_content_valid(sec)) {
            *at = s;
            return 1;
        }
    }
    return *tested ? -1 : 0;
}

/* Find the CSS key of a VOB group: libdvdcss reads the VOB data (from its first
 * block) through discio and returns the key, which is then confirmed on the
 * VOB's sectors by the content check. */
static void css_find_key(DVDVideoSource *src, int vtsn, int menu, CSSGroup *g)
{
    dvdcss_stream_cb cb = { .pf_seek = css_stream_seek, .pf_read = css_stream_read };
    CSSStream st = { 0 };
    DiscIOSource *host = NULL;
    char name[32], path[600];
    const char *what = menu ? "menu" : "title";
    int64_t count, at = 0;
    dvdcss_t css;
    int block = 0, ret, tested;

    g->state = CSS_FAILED;
    if (menu)
        snprintf(name, sizeof(name), vtsn ? "VTS_%02d_0.VOB" : "VIDEO_TS.VOB", vtsn);
    else
        snprintf(name, sizeof(name), "VTS_%02d_1.VOB", vtsn);
    if (find_vob(src, name, path, sizeof(path)) < 0) {
        av_log(src->log, AV_LOG_ERROR, "CSS: %s not found, so the key of title set %d (%s VOBs) cannot be "
               "looked for\n", name, vtsn, what);
        return;
    }
    if (src->folder) {
        char *p = host_path(src, path);

        ret = p ? ff_discio_source_open_file(src->log, p, &host) : AVERROR(ENOMEM);
        av_free(p);
        if (ret < 0)
            return;
        host->attempts = src->attempts;
        st.src = host;
        count  = host->size / DVDVIDEO_BLOCK_SIZE;
    } else {
        DiscIOFile *f = NULL;

        if (src->fs->ops->open_file(src->fs, path, &f) < 0 || f->nb_extents < 1 ||
            f->extents[0].sector == DISCIO_SECTOR_NOT_RECORDED) {
            av_log(src->log, AV_LOG_ERROR, "CSS: %s has no recorded data to look for the key in\n", path);
            ff_discio_file_free(&f);
            return;
        }
        block = f->extents[0].sector;
        count = f->extents[0].count;
        ff_discio_file_free(&f);
        st.src = src->image;
    }

    av_log(src->log, AV_LOG_VERBOSE, "CSS: looking for the key of title set %d (%s VOBs) from %s, block %d\n",
           vtsn, what, path, block);
    if (!(css = dvdcss_open_stream_uncached(&st, &cb, css_log, src->log))) {
        av_log(src->log, AV_LOG_ERROR, "CSS: libdvdcss could not be started on %s\n", path);
        goto end;
    }
    ret = dvdcss_title_key(css, block, g->key);
    dvdcss_close(css);

    if (!ret) {
        g->state = CSS_CLEAR;
        av_log(src->log, AV_LOG_WARNING, "CSS: libdvdcss found no scrambled sector in title set %d (%s VOBs), "
               "but one was met\n", vtsn, what);
        goto end;
    }
    if (ret < 0) {
        av_log(src->log, AV_LOG_ERROR, "CSS: the title key of title set %d (%s VOBs) could not be found\n",
               vtsn, what);
        goto end;
    }
    av_log(src->log, AV_LOG_DEBUG, "CSS: title key %02x:%02x:%02x:%02x:%02x\n",
           g->key[0], g->key[1], g->key[2], g->key[3], g->key[4]);

    switch (css_confirm_key(src, st.src, block, count, g->key, &at, &tested)) {
    case 1:
        g->state = CSS_KEY;
        av_log(src->log, AV_LOG_INFO, "CSS: title set %d (%s VOBs) is scrambled; its title key was found and "
               "confirmed (block %"PRId64" of %s descrambles to valid content)\n", vtsn, what, at,
               src->folder ? path : "the image");
        break;
    case 0:
        g->state = CSS_KEY;
        av_log(src->log, AV_LOG_WARNING, "CSS: title set %d (%s VOBs): title key found, but no block of %s can "
               "confirm it; it is used unconfirmed\n", vtsn, what, path);
        break;
    default:
        av_log(src->log, AV_LOG_ERROR, "CSS: title set %d (%s VOBs): the title key libdvdcss found is wrong: "
               "none of %d scrambled blocks descrambles to valid content\n", vtsn, what, tested);
        break;
    }

end:
    ff_discio_source_free(&host);
}

int ff_dvdvideo_source_descramble(DVDVideoSource *src, int vtsn, int menu, uint8_t *block)
{
    CSSGroup *g;
    int pes = ff_dvdvideo_css_scrambled_pes(block);

    if (pes <= 0)
        return 0;
    if (vtsn < 0 || vtsn > 99)
        return AVERROR(EINVAL);
    g = &src->css[vtsn][!!menu];
    if (g->state == CSS_UNKNOWN)
        css_find_key(src, vtsn, menu, g);
    if (g->state != CSS_KEY) {
        if (!g->warned)
            av_log(src->log, AV_LOG_ERROR, "CSS: scrambled sectors in title set %d (%s VOBs) cannot be "
                   "descrambled without a title key\n", vtsn, menu ? "menu" : "title");
        g->warned = 1;
        return AVERROR_INVALIDDATA;
    }
    if (pes != 14) {
        /* the descrambler reads the scrambling flag at the usual place only */
        av_log(src->log, AV_LOG_ERROR, "CSS: a scrambled sector whose PES starts at byte %d (after pack "
               "stuffing) cannot be descrambled\n", pes);
        return AVERROR_PATCHWELCOME;
    }
    return dvdcss_unscramble_sector(g->key, block) == 1 ? 1 : AVERROR_BUG;
}

/* ---- opening ---- */

static int is_dvd_file(const char *name)
{
    size_t n = strlen(name);

    return n > 4 && (!av_strcasecmp(name + n - 4, ".IFO") || !av_strcasecmp(name + n - 4, ".BUP") ||
                     !av_strcasecmp(name + n - 4, ".VOB"));
}

/* Pressed DVD-Video discs keep every file in one piece; an image whose IFO,
 * BUP or VOB files are scattered (extents that do not follow each other, or
 * unrecorded runs) is not supported. */
static int check_files_in_one_piece(DVDVideoSource *src)
{
    FindName vts = { "VIDEO_TS" };
    DirHandle *d;
    char dir[300];
    int scattered = 0, ret;

    if ((ret = src->fs->ops->list_dir(src->fs, "/", find_name_cb, &vts)) < 0)
        return ret;
    if (!vts.found[0])
        return 0;   /* files directly in the root are found by libdvdread there */
    snprintf(dir, sizeof(dir), "/%s", vts.found);
    if (!(d = av_mallocz(sizeof(*d))))
        return AVERROR(ENOMEM);
    if ((ret = src->fs->ops->list_dir(src->fs, dir, add_name, d)) < 0)
        goto end;
    for (int i = 0; i < d->nb_names; i++) {
        char path[600];
        DiscIOFile *f = NULL;
        int pieces = 0;

        if (!is_dvd_file(d->names[i]))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, d->names[i]);
        if (src->fs->ops->open_file(src->fs, path, &f) < 0)
            continue;
        for (int k = 0; k < f->nb_extents; k++) {
            const DiscIOExtent *e = &f->extents[k];

            if (!k || e->sector == DISCIO_SECTOR_NOT_RECORDED ||
                f->extents[k - 1].sector == DISCIO_SECTOR_NOT_RECORDED ||
                e->sector != f->extents[k - 1].sector + f->extents[k - 1].count)
                pieces++;
        }
        if (pieces > 1) {
            av_log(src->log, AV_LOG_WARNING, "%s is stored in %d separate pieces on the image (a DVD-Video "
                   "disc keeps every file in one piece)\n", path, pieces);
            scattered++;
        }
        ff_discio_file_free(&f);
    }
    if (scattered) {
        av_log(src->log, AV_LOG_ERROR, "'%s': %d DVD-Video file(s) are stored in separate pieces; images like "
               "this are not supported yet\n", src->path, scattered);
        ret = AVERROR_PATCHWELCOME;
    }
end:
    dir_free(d);
    return ret;
}

/* ---- IFO layout checks ---- */

/* The first block of an IFO (through the file callbacks: a block the IFO
 * cannot give comes from its BUP). */
static int read_ifo_header(DVDVideoSource *src, const char *path, uint8_t *buf)
{
    dvd_reader_filesystem_h fs = { .internal = src };
    FileHandle *h = fs_file_open(&fs, path);
    int ret;

    if (!h)
        return AVERROR(ENOENT);
    ret = h->size >= DISCIO_BLOCK_SIZE && fs_file_read(h, (char *)buf, DISCIO_BLOCK_SIZE) == DISCIO_BLOCK_SIZE
          ? 0 : AVERROR_INVALIDDATA;
    fs_file_close(h);
    return ret;
}

/* Sectors of a file of the disc folder (0 when it does not exist). */
static int64_t folder_file_sectors(DVDVideoSource *src, const char *name)
{
    struct stat st;
    char path[600], *p;
    int64_t n = 0;

    if (find_vob(src, name, path, sizeof(path)) < 0 || !(p = host_path(src, path)))
        return 0;
    if (!stat(p, &st) && S_ISREG(st.st_mode))
        n = (st.st_size + DISCIO_BLOCK_SIZE - 1) / DISCIO_BLOCK_SIZE;
    av_free(p);
    return n;
}

/* First sector of a file of the image, -1 when it is missing or not recorded. */
static int64_t image_file_sector(DVDVideoSource *src, const char *name)
{
    DiscIOFile *f = NULL;
    char path[600];
    int64_t sector = -1;

    if (find_vob(src, name, path, sizeof(path)) < 0 || src->fs->ops->open_file(src->fs, path, &f) < 0)
        return -1;
    if (f->nb_extents > 0 && f->extents[0].sector != DISCIO_SECTOR_NOT_RECORDED)
        sector = f->extents[0].sector;
    ff_discio_file_free(&f);
    return sector;
}

/* Where the IFO header puts the BUP (vmg/vts_last_sector minus
 * vmgi/vtsi_last_sector, sectors after the IFO), against the disc: on an
 * image the BUP's position; in a folder the IFO, menu VOB and title VOB files,
 * which the BUP follows on the disc. Warnings only. */
static void check_bup_position(DVDVideoSource *src, int vtsn, const uint8_t *hdr)
{
    char base[16], name[32];
    uint32_t want = AV_RB32(hdr + 0x0c) - (AV_RB32(hdr + 0x1c) & 0x1ffff);

    if (vtsn)
        snprintf(base, sizeof(base), "VTS_%02d_0", vtsn);
    else
        snprintf(base, sizeof(base), "VIDEO_TS");
    if (src->folder) {
        int64_t have;

        snprintf(name, sizeof(name), "%s.IFO", base);
        have = folder_file_sectors(src, name);
        snprintf(name, sizeof(name), "%s.VOB", base);
        have += folder_file_sectors(src, name);
        for (int k = 1; vtsn && k <= 9; k++) {
            int64_t n;

            snprintf(name, sizeof(name), "VTS_%02d_%d.VOB", vtsn, k);
            if (!(n = folder_file_sectors(src, name)))
                break;
            have += n;
        }
        if (have != want)
            av_log(src->log, AV_LOG_WARNING, "%s.IFO: its header puts the backup copy (BUP) %"PRIu32" sectors after "
                   "the IFO; the IFO and VOB files before it hold %"PRId64" sectors (%s)\n", base, want, have,
                   have < want ? "the rest lay outside the files on the disc, or a file is short"
                               : "the files hold more than the header counts");
    } else {
        int64_t ifo, bup;

        snprintf(name, sizeof(name), "%s.IFO", base);
        ifo = image_file_sector(src, name);
        snprintf(name, sizeof(name), "%s.BUP", base);
        bup = image_file_sector(src, name);
        if (ifo < 0)
            return;
        if (bup < 0)
            av_log(src->log, AV_LOG_WARNING, "%s.BUP (the backup copy of %s.IFO) is missing on the image\n",
                   base, base);
        else if (bup - ifo != want)
            av_log(src->log, AV_LOG_WARNING, "%s.IFO: its header puts the backup copy (BUP) %"PRIu32" sectors "
                   "after the IFO; on the image it is %"PRId64" sectors after it\n", base, want, bup - ifo);
    }
}

/* The IFO layout checks of the whole disc: the VMG and every title set. */
static void check_ifo_layout(DVDVideoSource *src)
{
    uint8_t hdr[DISCIO_BLOCK_SIZE];
    char path[600], name[32];
    int nts;

    if (find_vob(src, "VIDEO_TS.IFO", path, sizeof(path)) < 0 || read_ifo_header(src, path, hdr) < 0) {
        av_log(src->log, AV_LOG_WARNING, "VIDEO_TS.IFO cannot be read for the layout checks\n");
        return;
    }
    /* a provider identifier naming a ripping program that rewrites discs */
    for (int i = 0x40; i <= 0x5c; i++)
        if (!memcmp(hdr + i, "(Fab", 4)) {
            av_log(src->log, AV_LOG_WARNING, "The disc was processed by DVDFab or MacTheRipper (provider "
                   "identifier '%.32s'), which are known to produce damaged VOB files; the original disc is the "
                   "better source\n", (const char *)hdr + 0x40);
            break;
        }
    check_bup_position(src, 0, hdr);
    nts = AV_RB16(hdr + 0x3e);   /* vmg_nr_of_title_sets */
    if (nts < 1 || nts > 99) {
        av_log(src->log, AV_LOG_WARNING, "VIDEO_TS.IFO gives %d title sets (DVD-Video allows 1 to 99); the title "
               "sets are not checked\n", nts);
        return;
    }
    for (int n = 1; n <= nts; n++) {
        snprintf(name, sizeof(name), "VTS_%02d_0.IFO", n);
        if (find_vob(src, name, path, sizeof(path)) < 0) {
            av_log(src->log, AV_LOG_WARNING, "%s (title set %d of %d) is missing\n", name, n, nts);
            continue;
        }
        if (read_ifo_header(src, path, hdr) < 0) {
            av_log(src->log, AV_LOG_WARNING, "%s cannot be read for the layout checks\n", name);
            continue;
        }
        check_bup_position(src, n, hdr);
    }
    av_log(src->log, AV_LOG_VERBOSE, "IFO layout checked: the VMG and %d title set(s)\n", nts);
}

/* ---- VOB groups ---- */

int ff_dvdvideo_source_find(DVDVideoSource *src, const char *name, char *path, size_t size)
{
    return find_vob(src, name, path, size);
}

/* Open the files of a VOB group of a disc folder: the menu VOB, or the title
 * VOBs from VTS_nn_1.VOB up to the first that is missing or not a regular
 * file. An empty file adds nothing (the next one follows the one before it);
 * a regular file that cannot be opened makes the whole group unreadable. */
static int vob_group_open_folder(DVDVideoSource *src, int vtsn, int menu, VOBGroup *g)
{
    for (int k = menu ? 0 : 1; k <= (menu ? 0 : 9); k++) {
        VOBPiece *p = &g->pieces[g->nb_pieces];
        char name[32], path[600], *hp;
        struct stat st;
        int ret;

        if (!vtsn)
            snprintf(name, sizeof(name), "VIDEO_TS.VOB");
        else
            snprintf(name, sizeof(name), "VTS_%02d_%d.VOB", vtsn, k);
        if (find_vob(src, name, path, sizeof(path)) < 0)
            break;
        if (!(hp = host_path(src, path)))
            return AVERROR(ENOMEM);
        if (stat(hp, &st) < 0 || !S_ISREG(st.st_mode)) {
            av_log(src->log, AV_LOG_WARNING, "'%s' is not a regular file: the VOB files of %s end before it\n",
                   path, vtsn ? "the title set" : "the video manager");
            av_free(hp);
            break;
        }
        ret = ff_discio_source_open_file(src->log, hp, &p->host);
        av_free(hp);
        if (ret < 0) {
            if (vtsn)
                av_log(src->log, AV_LOG_ERROR, "'%s' cannot be opened: the %s VOBs of title set %d cannot be read\n",
                       path, menu ? "menu" : "title", vtsn);
            else
                av_log(src->log, AV_LOG_ERROR, "'%s' cannot be opened: the menu VOBs of the video manager cannot "
                       "be read\n", path);
            g->error = ret;
            return 0;
        }
        if (!p->host->size) {
            av_log(src->log, AV_LOG_VERBOSE, "'%s' is empty: skipped\n", path);
            ff_discio_source_free(&p->host);
            continue;
        }
        p->start  = g->bytes;
        g->bytes += p->host->size;
        g->nb_pieces++;
    }
    return 0;
}

/* The image block of a file recorded as exactly one extent; -1 otherwise
 * (missing, or in several extents). A run that is not recorded counts as
 * block 0xfffffffe, which lies past the end of any image. */
static int64_t image_single_extent(DVDVideoSource *src, const char *name)
{
    DiscIOFile *f = NULL;
    char path[600];
    int64_t sector = -1;

    if (find_vob(src, name, path, sizeof(path)) < 0 || src->fs->ops->open_file(src->fs, path, &f) < 0)
        return -1;
    if (f->nb_extents == 1)
        sector = f->extents[0].sector == DISCIO_SECTOR_NOT_RECORDED ? 0xfffffffe : f->extents[0].sector;
    ff_discio_file_free(&f);
    return sector;
}

/* Whether the block at image block `at` is the first NAV pack of a VOB: a NAV
 * pack whose PCI and DSI both name sector 0. */
static int image_vob_starts_at(DVDVideoSource *src, int64_t at)
{
    uint8_t buf[DISCIO_BLOCK_SIZE];

    return at >= 0 && ff_discio_read_blocks(src->image, at * DISCIO_BLOCK_SIZE, buf, sizeof(buf), 3, 1) >= 0 &&
           ff_dvdvideo_is_nav_pack(buf) && !AV_RB32(buf + 0x2d) && !AV_RB32(buf + 0x40b);
}

/* A VOB group of a disc image, placed by its IFO: blocks are counted from the
 * IFO's first block. The group starts at the IFO header's vmgm_vobs /
 * vtsm_vobs (menu) or vtstt_vobs (title VOBs) when the file system puts the
 * VOB file there too, or puts it at the IFO's own block (distance 0); a VOB
 * file the file system cannot place counts as image block 0, whose distance
 * from the IFO wraps around (32 bits) and is compared like any other. When the
 * two disagree, at whichever of them holds the first NAV pack of a VOB, the IFO's
 * first, else at vtstt_vobs. A title set's menu VOBs end where its title VOBs
 * start (when the header gives them); everything else reaches to the end of
 * the image, and so does a group whose start lies past its end. */
static int vob_group_open_image(DVDVideoSource *src, int vtsn, int menu, VOBGroup *g)
{
    const char *what = menu ? "menu VOBs" : "title VOBs";
    int64_t ifo_sector, vob_sector, image_end, start, end;
    uint32_t title_start, ifo_value, fs_value, pick;
    uint8_t hdr[DISCIO_BLOCK_SIZE];
    char name[32], vob[32], path[600], set[32];

    snprintf(name, sizeof(name), vtsn ? "VTS_%02d_0.IFO" : "VIDEO_TS.IFO", vtsn);
    snprintf(set, sizeof(set), vtsn ? "Title set %d" : "The video manager", vtsn);
    if ((ifo_sector = image_single_extent(src, name)) < 0) {
        av_log(src->log, AV_LOG_DEBUG, "%s is missing or not one extent: its VOBs cannot be placed\n", name);
        return 0;
    }
    if (find_vob(src, name, path, sizeof(path)) < 0 || read_ifo_header(src, path, hdr) < 0)
        return 0;
    title_start = AV_RB32(hdr + 0xc4);
    ifo_value   = menu ? AV_RB32(hdr + 0xc0) : title_start;
    if (menu)
        snprintf(vob, sizeof(vob), vtsn ? "VTS_%02d_0.VOB" : "VIDEO_TS.VOB", vtsn);
    else
        snprintf(vob, sizeof(vob), "VTS_%02d_1.VOB", vtsn);
    vob_sector = image_single_extent(src, vob);
    fs_value   = (uint32_t)((vob_sector < 0 ? 0 : vob_sector) - ifo_sector);

    pick = ifo_value;
    if (ifo_value != fs_value && fs_value) {
        av_log(src->log, AV_LOG_WARNING, "%s: the IFO puts its %s %"PRIu32" blocks after the IFO, the file system "
               "%"PRIu32" blocks after it\n", set, what, ifo_value, fs_value);
        if (image_vob_starts_at(src, ifo_sector + ifo_value)) {
            av_log(src->log, AV_LOG_VERBOSE, "%s: the IFO's position of its %s starts a VOB: used\n", set, what);
        } else if (image_vob_starts_at(src, ifo_sector + fs_value)) {
            av_log(src->log, AV_LOG_VERBOSE, "%s: the file system's position of its %s starts a VOB: used\n",
                   set, what);
            pick = fs_value;
        } else {
            av_log(src->log, AV_LOG_WARNING, "%s: neither position starts a VOB; its %s are read from where its "
                   "title VOBs start (%"PRIu32" blocks after the IFO)\n", set, what, title_start);
            pick = title_start;
        }
    }
    image_end = (src->image->size + DISCIO_BLOCK_SIZE - 1) / DISCIO_BLOCK_SIZE;
    start     = ifo_sector + pick;
    end       = menu && vtsn && title_start ? ifo_sector + title_start : image_end;
    if (start > end) {
        av_log(src->log, AV_LOG_WARNING, "%s: its %s start after their end (block %"PRId64" > %"PRId64"); they are "
               "read up to the end of the image\n", set, what, start, end);
        end = image_end;
    }
    g->in_image    = 1;
    g->image_start = start;
    g->image_end   = end;
    av_log(src->log, AV_LOG_DEBUG, "%s: %s at image blocks %"PRId64" - %"PRId64"\n", set, what, start, end);
    return 0;
}

static void vob_group_free(VOBGroup **g)
{
    for (int i = 0; *g && i < (*g)->nb_pieces; i++)
        ff_discio_source_free(&(*g)->pieces[i].host);
    av_freep(g);
}

static int vob_group(DVDVideoSource *src, int vtsn, int menu, VOBGroup **out)
{
    VOBGroup **slot;
    int ret;

    if (vtsn < 0 || vtsn > 99 || (!vtsn && !menu))
        return AVERROR(EINVAL);
    slot = &src->vobs[vtsn][!!menu];
    if (!*slot) {
        if (!(*slot = av_mallocz(sizeof(**slot))))
            return AVERROR(ENOMEM);
        ret = src->folder ? vob_group_open_folder(src, vtsn, menu, *slot)
                          : vob_group_open_image(src, vtsn, menu, *slot);
        if (ret < 0) {
            vob_group_free(slot);
            return ret;
        }
    }
    *out = *slot;
    return 0;
}

int ff_dvdvideo_source_vob_read(DVDVideoSource *src, int vtsn, int menu, int64_t sector, uint8_t *buf,
                                int attempts)
{
    VOBGroup *g;
    int64_t pos;
    int ret, n;

    if ((ret = vob_group(src, vtsn, menu, &g)) < 0)
        return ret;
    if (sector < 0)
        return AVERROR(EINVAL);
    if (g->in_image) {
        if (sector >= g->image_end - g->image_start)
            return AVERROR_EOF;
        return ff_discio_read_blocks(src->image, (g->image_start + sector) * DISCIO_BLOCK_SIZE, buf,
                                     DISCIO_BLOCK_SIZE, attempts, 0);
    }
    if (g->error)
        return g->error;
    if (!g->nb_pieces)
        return AVERROR(ENOENT);
    pos = sector * DISCIO_BLOCK_SIZE;
    if (pos >= g->bytes)
        return AVERROR_EOF;
    /* a block may hold the end of one file and the start of the next; after
     * the end of the last file the block is filled with 0xFF */
    n = FFMIN(DISCIO_BLOCK_SIZE, g->bytes - pos);
    memset(buf + n, 0xFF, DISCIO_BLOCK_SIZE - n);
    for (int i = 0; i < g->nb_pieces; i++) {
        VOBPiece *p = &g->pieces[i];
        int64_t from = FFMAX(pos, p->start), to = FFMIN(pos + n, p->start + p->host->size);

        if (from >= to)
            continue;
        if ((ret = ff_discio_read_bytes(p->host, from - p->start, buf + (from - pos), to - from, attempts, 0)) < 0)
            return ret;
    }
    return 0;
}

int ff_dvdvideo_source_vob_bytes(DVDVideoSource *src, int vtsn, int menu, int64_t *bytes)
{
    VOBGroup *g;
    int ret;

    *bytes = 0;
    if ((ret = vob_group(src, vtsn, menu, &g)) < 0)
        return ret;
    if (g->in_image) {
        *bytes = (g->image_end - g->image_start) * DISCIO_BLOCK_SIZE;
        return 0;
    }
    if (g->error)
        return g->error;
    if (!g->nb_pieces)
        return AVERROR(ENOENT);
    *bytes = g->bytes;
    return 0;
}

int64_t ff_dvdvideo_source_file_sector(DVDVideoSource *src, const char *name)
{
    return src->folder ? -1 : image_single_extent(src, name);
}

int ff_dvdvideo_source_open(void *log, const char *path, const DiscIOImageOptions *opts, int attempts,
                            DVDVideoSource **out)
{
    DVDVideoSource *src;
    struct stat st;
    int ret;

    *out = NULL;
    if (stat(path, &st) < 0) {
        ret = AVERROR(errno);
        av_log(log, AV_LOG_ERROR, "Cannot open '%s': %s\n", path, av_err2str(ret));
        return ret;
    }
    if (!(src = av_mallocz(sizeof(*src))) || !(src->path = av_strdup(path))) {
        av_free(src);
        return AVERROR(ENOMEM);
    }
    src->log      = log;
    src->refs     = 1;
    src->attempts = attempts;

    if (S_ISDIR(st.st_mode)) {
        /* a folder holding VIDEO_TS, the VIDEO_TS folder, or a folder with the
         * DVD files directly: libdvdread finds VIDEO_TS.IFO in any of them */
        size_t n = strlen(path);

        while (n > 1 && path[n - 1] == '/')
            n--;
        if (!(src->folder = av_strndup(path, n))) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        av_log(log, AV_LOG_INFO, "Reading the disc folder '%s'\n", src->folder);
        check_ifo_layout(src);
        *out = src;
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        av_log(log, AV_LOG_ERROR, "'%s' is neither a disc folder nor a disc image file\n", path);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if ((ret = ff_discio_source_open_file(log, path, &src->image)) < 0)
        goto fail;
    src->image->attempts = attempts;
    if ((ret = ff_discio_mount_image(src->image, opts, &src->fs)) < 0)
        goto fail;
    switch (ff_discio_disc_format(src->fs)) {
    case DISCIO_DISC_DVD:
        break;
    case DISCIO_DISC_BLURAY:
        av_log(log, AV_LOG_ERROR, "'%s' is a Blu-ray image; only DVD-Video is supported yet\n", path);
        ret = AVERROR_PATCHWELCOME;
        goto fail;
    case DISCIO_DISC_HDDVD:
        av_log(log, AV_LOG_ERROR, "'%s' is an HD DVD image; only DVD-Video is supported yet\n", path);
        ret = AVERROR_PATCHWELCOME;
        goto fail;
    default:
        av_log(log, AV_LOG_ERROR, "'%s' holds no DVD-Video structure (no /VIDEO_TS/VIDEO_TS.IFO)\n", path);
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }
    if ((ret = check_files_in_one_piece(src)) < 0)
        goto fail;
    check_ifo_layout(src);
    *out = src;
    return 0;

fail:
    source_unref(&src);
    return ret;
}
