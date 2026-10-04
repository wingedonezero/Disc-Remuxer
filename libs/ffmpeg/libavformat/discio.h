/*
 * Disc I/O: reading disc images and disc folders, shared by the disc demuxers
 * (DVD-Video now; HD DVD and Blu-ray later).
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

#ifndef AVFORMAT_DISCIO_H
#define AVFORMAT_DISCIO_H

#include <stdint.h>

/** Block size of disc sources (DVD, HD DVD, Blu-ray). */
#define DISCIO_BLOCK_SIZE       2048

/** Read attempts per request when no setting says otherwise. */
#define DISCIO_DEFAULT_ATTEMPTS 5

/**
 * What a source does: the raw reads behind a DiscIOSource.
 */
typedef struct DiscIOSourceOps {
    /**
     * Read up to len bytes at pos.
     * @return the number of bytes read (0 at or past the end), or a negative
     *         AVERROR code
     */
    int  (*read_at)(void *opaque, int64_t pos, uint8_t *buf, int len);
    /** Release opaque (may be NULL). */
    void (*close)(void *opaque);
} DiscIOSourceOps;

/**
 * A source of bytes: a disc image file, one file of a disc folder, or a test
 * source.
 */
typedef struct DiscIOSource {
    const DiscIOSourceOps *ops;
    void                  *opaque;
    int64_t                size;    /**< bytes */
    char                  *name;    /**< for log messages (the path) */
    void                  *logctx;  /**< av_log() context, may be NULL */
} DiscIOSource;

/**
 * Make a source from raw read operations.
 * @return the source, or NULL when out of memory (opaque is then released
 *         through ops->close)
 */
DiscIOSource *ff_discio_source_new(void *logctx, const char *name, int64_t size,
                                   const DiscIOSourceOps *ops, void *opaque);

/**
 * Open a host file (a disc image, or a file of a disc folder) as a source.
 * @return 0 or a negative AVERROR code (logged)
 */
int ff_discio_source_open_file(void *logctx, const char *path, DiscIOSource **out);

/** Close a source and set *src to NULL. */
void ff_discio_source_free(DiscIOSource **src);

/**
 * Read len bytes at pos; pos and len must be multiples of DISCIO_BLOCK_SIZE.
 *
 * A failed read is tried again, up to attempts reads in all (0 counts as 1).
 * After the first retry, the rest of the request is read one block at a time.
 * A read that returns no data, or a part of a block, is a failed attempt like
 * a read error; a read that returns more than asked fails at once. When the
 * source's size is not a whole number of blocks, its last block is completed
 * with 0xFF bytes. Every failed attempt is logged (as a warning, or at debug
 * level when quiet is set), and so are the padding and the final failure.
 *
 * @return 0 or a negative AVERROR code
 */
int ff_discio_read_blocks(DiscIOSource *src, int64_t pos, uint8_t *buf, int len,
                          int attempts, int quiet);

/**
 * Read len bytes at any pos, through ff_discio_read_blocks(): a partial first
 * and last block go through a one-block buffer, whole blocks in between are
 * read directly.
 * @return 0 or a negative AVERROR code
 */
int ff_discio_read_bytes(DiscIOSource *src, int64_t pos, uint8_t *buf, int len,
                         int attempts, int quiet);

#endif /* AVFORMAT_DISCIO_H */
