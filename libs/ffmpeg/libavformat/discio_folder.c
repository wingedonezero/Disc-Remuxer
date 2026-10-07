/*
 * Disc I/O: a disc folder (a copy of a disc's files on the host) as a file
 * system
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

/*
 * A path on the disc is the folder's path, '/', and the path without its
 * first '/' or '\\', looked up as it is written (the host's own rules: exact
 * case on Linux); a file must be a regular file. A directory is listed in
 * the host's own order (not sorted), "." and ".." included, an entry being a
 * directory when the host says so (an entry of unknown type counts as a
 * file). Each file is read as a host file of its own (DiscIOFile.host).
 */

/* DT_DIR (the entry type the host gives in a listing) */
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discio.h"

typedef struct Folder {
    char *root;                 /* the folder's path, without a trailing separator */
} Folder;

/* The host path of a path on the disc (av_free()), or NULL. */
static char *host_path(const DiscIOFS *fs, const char *path)
{
    const Folder *f = fs->priv;

    if (*path == '/' || *path == '\\')
        path++;
    return av_asprintf("%s/%s", f->root, path);
}

static int folder_open_file(DiscIOFS *fs, const char *path, DiscIOFile **out)
{
    struct stat st;
    DiscIOFile *file;
    char *p = host_path(fs, path);
    int ret;

    *out = NULL;
    if (!p)
        return AVERROR(ENOMEM);
    if (stat(p, &st) < 0 || !S_ISREG(st.st_mode)) {
        av_free(p);
        return AVERROR(ENOENT);
    }
    if (!(file = av_mallocz(sizeof(*file)))) {
        av_free(p);
        return AVERROR(ENOMEM);
    }
    ret = ff_discio_source_open_file(fs->src->logctx, p, &file->host);
    av_free(p);
    if (ret < 0) {
        av_free(file);
        return ret;
    }
    file->host->attempts = fs->src->attempts;
    file->size = file->host->size;
    *out = file;
    return 0;
}

static int folder_list_dir(DiscIOFS *fs, const char *path, DiscIODirCallback cb, void *opaque)
{
    char *p = host_path(fs, path);
    struct dirent *e;
    DIR *d;
    int ret = 0;

    if (!p)
        return AVERROR(ENOMEM);
    d = opendir(p);
    av_free(p);
    if (!d)
        return AVERROR(ENOENT);
    while ((e = readdir(d)))
        if ((ret = cb(opaque, e->d_name, e->d_type == DT_DIR)))
            break;
    closedir(d);
    return ret;
}

static int folder_find_dir(DiscIOFS *fs, const char *path)
{
    struct stat st;
    char *p = host_path(fs, path);
    int ret;

    if (!p)
        return AVERROR(ENOMEM);
    ret = stat(p, &st) < 0 || !S_ISDIR(st.st_mode) ? AVERROR(ENOENT) : 0;
    av_free(p);
    return ret;
}

static void folder_close(DiscIOFS *fs)
{
    Folder *f = fs->priv;

    if (f)
        av_freep(&f->root);
    av_freep(&fs->priv);
    ff_discio_source_free(&fs->src);
}

static const DiscIOFSOps folder_ops = {
    .name      = "folder",
    .open_file = folder_open_file,
    .list_dir  = folder_list_dir,
    .find_dir  = folder_find_dir,
    .close     = folder_close,
    .kind      = DISCIO_FS_FOLDER,
};

/* The folder itself is no source of bytes: every read goes to a file. */
static int no_read(void *opaque, int64_t pos, uint8_t *buf, int len)
{
    return AVERROR(EINVAL);
}

static const DiscIOSourceOps folder_source_ops = {
    .read_at = no_read,
};

int ff_discio_folder_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) >= 0 && S_ISDIR(st.st_mode);
}

int ff_discio_mount_folder(void *logctx, const char *path, int attempts, DiscIOFS **out)
{
    DiscIOFS *fs;
    Folder *f;
    size_t n = strlen(path);

    *out = NULL;
    if (!ff_discio_folder_is_dir(path)) {
        av_log(logctx, AV_LOG_ERROR, "'%s' is not a folder\n", path);
        return AVERROR(ENOTDIR);
    }
    if (!(fs = av_mallocz(sizeof(*fs))) || !(f = fs->priv = av_mallocz(sizeof(*f))) ||
        !(f->root = av_strdup(path)) ||
        !(fs->src = ff_discio_source_new(logctx, path, 0, &folder_source_ops, NULL))) {
        if (fs)
            folder_close(fs);
        av_free(fs);
        return AVERROR(ENOMEM);
    }
    /* one trailing separator is not part of the folder's path */
    if (n > 1 && (f->root[n - 1] == '/' || f->root[n - 1] == '\\'))
        f->root[n - 1] = 0;
    fs->ops           = &folder_ops;
    fs->src->attempts = attempts;
    *out = fs;
    av_log(logctx, AV_LOG_VERBOSE, "'%s': a disc folder, read as host files\n", f->root);
    return 0;
}
