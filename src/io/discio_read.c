/*
 * Disc I/O: sources and block reads with retries
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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"

#include "io/discio.h"

DiscIOSource *ff_discio_source_new(void *logctx, const char *name, int64_t size,
                                   const DiscIOSourceOps *ops, void *opaque)
{
    DiscIOSource *src = dr_mallocz(sizeof(*src));

    if (!src || !(src->name = dr_strdup(name ? name : "<unnamed>"))) {
        dr_free(src);
        if (ops->close)
            ops->close(opaque);
        return NULL;
    }
    src->ops    = ops;
    src->opaque = opaque;
    src->size     = size;
    src->logctx   = logctx;
    src->attempts = DISCIO_DEFAULT_ATTEMPTS;
    return src;
}

void ff_discio_source_free(DiscIOSource **src)
{
    if (!*src)
        return;
    if ((*src)->ops->close)
        (*src)->ops->close((*src)->opaque);
    dr_freep(&(*src)->name);
    dr_freep(src);
}

/* host files */

typedef struct HostFile {
    int fd;
} HostFile;

static int host_read_at(void *opaque, int64_t pos, uint8_t *buf, int len)
{
    HostFile *f = opaque;
    ssize_t got;

    do {
        got = pread(f->fd, buf, len, pos);
    } while (got < 0 && errno == EINTR);
    return got < 0 ? DR_ERR(errno) : (int)got;
}

static void host_close(void *opaque)
{
    HostFile *f = opaque;

    if (f) {
        close(f->fd);
        dr_free(f);
    }
}

static const DiscIOSourceOps host_ops = {
    .read_at = host_read_at,
    .close   = host_close,
};

int ff_discio_source_open_file(void *logctx, const char *path, DiscIOSource **out)
{
    struct stat st;
    HostFile *f;
    int fd, ret;

    *out = NULL;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ret = DR_ERR(errno);
        dr_io_log(logctx, DR_IO_LOG_ERROR, "Cannot open '%s': %s\n", path, dr_err_text(ret));
        return ret;
    }
    if (fstat(fd, &st) < 0) {
        ret = DR_ERR(errno);
        dr_io_log(logctx, DR_IO_LOG_ERROR, "Cannot get the size of '%s': %s\n", path, dr_err_text(ret));
        close(fd);
        return ret;
    }
    if (!S_ISREG(st.st_mode)) {
        dr_io_log(logctx, DR_IO_LOG_ERROR, "Cannot open '%s': not a regular file\n", path);
        close(fd);
        return DR_ERR(EINVAL);
    }
    if (!(f = dr_mallocz(sizeof(*f)))) {
        close(fd);
        return DR_ERR(ENOMEM);
    }
    f->fd = fd;
    if (!(*out = ff_discio_source_new(logctx, path, st.st_size, &host_ops, f)))
        return DR_ERR(ENOMEM);
    dr_io_log(logctx, DR_IO_LOG_DEBUG, "Opened '%s' (%"PRId64" bytes)\n", path, (int64_t)st.st_size);
    return 0;
}

/* block reads */

int ff_discio_read_blocks(DiscIOSource *src, int64_t pos, uint8_t *buf, int len,
                          int attempts, int quiet)
{
    const int64_t bs = DISCIO_BLOCK_SIZE;
    const int64_t padded_end = (src->size + bs - 1) / bs * bs;
    const int fail_level = quiet ? DR_IO_LOG_DEBUG : DR_IO_LOG_WARNING;
    int single_block = 0;
    int off = 0;

    if (attempts < 1)
        attempts = 1;
    if (pos < 0 || pos % bs || len < 0 || len % bs) {
        dr_io_log(src->logctx, DR_IO_LOG_ERROR,
               "Read of '%s' refused: offset %"PRId64" and length %d must be whole %d-byte blocks\n",
               src->name, pos, len, DISCIO_BLOCK_SIZE);
        return DR_ERR(EINVAL);
    }

    while (off < len) {
        int tries = 0;
        int got;

        for (;;) {
            char reason[96];
            int want, err = 0;

            if (tries > 0 && !single_block) {
                single_block = 1;
                dr_io_log(src->logctx, DR_IO_LOG_DEBUG,
                       "Reading the rest of the request on '%s' one block at a time\n", src->name);
            }
            want = single_block ? DISCIO_BLOCK_SIZE : len - off;
            got  = src->ops->read_at(src->opaque, pos, buf + off, want);

            if (got >= 0 && got % bs && pos + got == src->size && pos + want == padded_end) {
                /* the last block of a source that is not a whole number of blocks */
                dr_io_log(src->logctx, DR_IO_LOG_VERBOSE,
                       "Padded %d bytes after the end of '%s' (%"PRId64" bytes, not a whole number "
                       "of blocks) with 0xFF\n", want - got, src->name, src->size);
                memset(buf + off + got, 0xFF, want - got);
                got = want;
            }
            if (got > want) {
                dr_io_log(src->logctx, DR_IO_LOG_ERROR,
                       "Read of '%s' at offset %"PRId64" returned %d bytes, more than the %d asked\n",
                       src->name, pos, got, want);
                return DR_ERR_EXTERNAL;   /* the source broke its contract */
            }
            if (got < 0) {
                err = got;
                dr_strlcpy(reason, dr_err_text(got), sizeof(reason));
            } else if (got == 0) {
                err = DR_ERR(EIO);
                snprintf(reason, sizeof(reason), "the read returned 0 of %d bytes", want);
            } else if (got % bs) {
                err = DR_ERR(EIO);
                snprintf(reason, sizeof(reason), "the read returned %d of %d bytes", got, want);
            }
            if (!err)
                break;

            tries++;
            if (tries >= attempts) {
                dr_io_log(src->logctx, quiet ? DR_IO_LOG_DEBUG : DR_IO_LOG_ERROR,
                       "Read of '%s' at offset %"PRId64" failed after %d attempt(s): %s\n",
                       src->name, pos, tries, reason);
                return err;
            }
            dr_io_log(src->logctx, fail_level,
                   "Error reading '%s' at offset %"PRId64" (%d bytes): %s; retry %d of %d\n",
                   src->name, pos, want, reason, tries, attempts - 1);
        }
        off += got;
        pos += got;
    }
    return 0;
}

int ff_discio_read_bytes(DiscIOSource *src, int64_t pos, uint8_t *buf, int len,
                         int attempts, int quiet)
{
    const int64_t bs = DISCIO_BLOCK_SIZE;
    uint8_t block[DISCIO_BLOCK_SIZE];
    int out = 0, whole, ret;

    if (pos < 0 || len < 0)
        return DR_ERR(EINVAL);
    /* partial first block */
    if (pos % bs && len > 0) {
        int64_t start = pos - pos % bs;
        int skip = pos - start;
        int n = DR_MIN(DISCIO_BLOCK_SIZE - skip, len);

        if ((ret = ff_discio_read_blocks(src, start, block, DISCIO_BLOCK_SIZE, attempts, quiet)) < 0)
            return ret;
        memcpy(buf, block + skip, n);
        out += n;
        pos += n;
    }
    /* whole blocks */
    whole = (len - out) / DISCIO_BLOCK_SIZE * DISCIO_BLOCK_SIZE;
    if (whole > 0) {
        if ((ret = ff_discio_read_blocks(src, pos, buf + out, whole, attempts, quiet)) < 0)
            return ret;
        out += whole;
        pos += whole;
    }
    /* partial last block */
    if (out < len) {
        if ((ret = ff_discio_read_blocks(src, pos, block, DISCIO_BLOCK_SIZE, attempts, quiet)) < 0)
            return ret;
        memcpy(buf + out, block, len - out);
    }
    return 0;
}
