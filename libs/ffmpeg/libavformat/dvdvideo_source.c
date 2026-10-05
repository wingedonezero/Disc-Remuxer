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

struct DVDVideoSource {
    CSSGroup       css[100][2];  /* [title set][1 = menu VOBs, 0 = title VOBs] */
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
} FileHandle;

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
    if (src->folder) {
        char *p = host_path(src, path);

        if (!p || ff_discio_source_open_file(src->log, p, &h->host) < 0) {
            av_free(p);
            av_free(h);
            return NULL;
        }
        av_free(p);
        h->host->attempts = src->attempts;
        h->size = h->host->size;
        return h;
    }
    tidy_path(path, tidy, sizeof(tidy));
    if (src->fs->ops->open_file(src->fs, tidy, &h->file) < 0) {
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
    ret = h->host ? ff_discio_read_bytes(h->host, h->pos, (uint8_t *)buf, n, h->host->attempts, 0)
                  : ff_discio_file_read(h->src->fs, h->file, h->pos, (uint8_t *)buf, n);
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
    av_free(h);
    return 0;
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
    if (!(css = dvdcss_open_stream_uncached(&st, &cb))) {
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
    *out = src;
    return 0;

fail:
    source_unref(&src);
    return ret;
}
