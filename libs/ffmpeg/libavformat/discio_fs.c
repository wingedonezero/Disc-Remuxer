/*
 * Disc I/O: what all file systems share (paths, files, reads over extents)
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

#include <inttypes.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "discio.h"

void ff_discio_fs_close(DiscIOFS **fs)
{
    if (!*fs)
        return;
    if ((*fs)->ops->close)
        (*fs)->ops->close(*fs);
    av_freep(fs);
}

void ff_discio_file_free(DiscIOFile **file)
{
    if (!*file)
        return;
    av_freep(&(*file)->extents);
    av_freep(&(*file)->data);
    av_freep(file);
}

int ff_discio_file_read(DiscIOFS *fs, const DiscIOFile *file, int64_t pos,
                        uint8_t *buf, int len)
{
    DiscIOSource *src = fs->src;
    int64_t ext_start = 0;   /* file offset where the current extent begins */
    int done = 0;

    if (pos < 0 || len < 0 || pos + len > file->size) {
        av_log(src->logctx, AV_LOG_ERROR,
               "Read of %d bytes at %"PRId64" runs past the end of a %"PRId64"-byte file on '%s'\n",
               len, pos, file->size, src->name);
        return AVERROR(EINVAL);
    }
    if (file->data) {
        memcpy(buf, file->data + pos, len);
        return 0;
    }
    for (int i = 0; i < file->nb_extents && done < len; i++) {
        const DiscIOExtent *e = &file->extents[i];
        int64_t ext_bytes = e->count * DISCIO_BLOCK_SIZE;
        int64_t at = pos + done;

        if (at < ext_start + ext_bytes) {
            int64_t in_ext = at - ext_start;
            int n = FFMIN(len - done, ext_bytes - in_ext);

            if (e->sector == DISCIO_SECTOR_NOT_RECORDED) {
                memset(buf + done, 0, n);   /* not recorded: zeros */
            } else {
                int ret = ff_discio_read_bytes(src, e->sector * DISCIO_BLOCK_SIZE + in_ext,
                                               buf + done, n, src->attempts, 0);
                if (ret < 0)
                    return ret;
            }
            done += n;
        }
        ext_start += ext_bytes;
    }
    if (done < len) {
        av_log(src->logctx, AV_LOG_ERROR,
               "The extents of a %"PRId64"-byte file on '%s' end before byte %"PRId64"\n",
               file->size, src->name, pos + done);
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

int ff_discio_walk_path(const char *path, void **dir,
                        int (*subdir)(void *ctx, void **dir, const char *name, int name_len),
                        void *ctx, const char **last)
{
    const char *rest = path;

    for (;;) {
        const char *sep = strchr(rest, '/');
        const char *q;
        int ret;

        if (!sep)
            sep = strchr(rest, '\\');
        if (sep != rest)
            return AVERROR(ENOENT);
        rest++;
        q = strchr(rest, '/');
        if (!q)
            q = strchr(rest, '\\');
        if (!q) {
            *last = rest;
            return 0;
        }
        if (q == rest)
            continue;
        if ((ret = subdir(ctx, dir, rest, q - rest)) < 0)
            return ret;
        rest = q;
    }
}
