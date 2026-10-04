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
    int                    attempts; /**< read attempts used by the file systems
                                          (DISCIO_DEFAULT_ATTEMPTS unless set) */
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

/* ---- file systems (ISO 9660 / Joliet, UDF) ---- */

/** Room for a volume label, including the terminating NUL. */
#define DISCIO_LABEL_SIZE 161

/** A run of consecutive sectors of the source. */
typedef struct DiscIOExtent {
    int64_t sector;   /**< first 2048-byte sector, from the start of the source */
    int64_t count;    /**< sectors */
} DiscIOExtent;

/** A file found in a file system: its size and where its data lies. */
typedef struct DiscIOFile {
    int64_t       size;        /**< bytes */
    int           nb_extents;
    DiscIOExtent *extents;     /**< in file order; together >= size bytes */
    /**
     * The file's data, for a file recorded inside its file entry (UDF
     * "embedded" data): size bytes, no extents. NULL otherwise.
     */
    uint8_t      *data;
} DiscIOFile;

struct DiscIOFS;

/** Called once per directory entry by DiscIOFSOps.list_dir. */
typedef int (*DiscIODirCallback)(void *opaque, const char *name, int is_dir);

/** What a file system does (one table per file-system reader). */
typedef struct DiscIOFSOps {
    /** e.g. "ISO 9660", "Joliet", "UDF (NetBSD)" */
    const char *name;
    /**
     * Find the file at path ("/VIDEO_TS/VIDEO_TS.IFO"; separators '/' or '\',
     * the path starts with one).
     * @return 0, AVERROR(ENOENT) when there is no such file, or another
     *         negative AVERROR code
     */
    int  (*open_file)(struct DiscIOFS *fs, const char *path, DiscIOFile **out);
    /**
     * Call cb for every entry of the directory at path ("/" = root), in disc
     * order; a non-zero return of cb ends the listing with that value.
     * @return 0, AVERROR(ENOENT), AVERROR_INVALIDDATA for a corrupt directory
     */
    int  (*list_dir)(struct DiscIOFS *fs, const char *path, DiscIODirCallback cb, void *opaque);
    void (*close)(struct DiscIOFS *fs);
} DiscIOFSOps;

/** A mounted file system on a source. */
typedef struct DiscIOFS {
    const DiscIOFSOps *ops;
    void              *priv;
    DiscIOSource      *src;     /**< not owned */
    char               label[DISCIO_LABEL_SIZE];  /**< as decoded, may be empty */
    /** UDF only: revision from the implementation use volume descriptor
     *  ("*UDF LV Info"), e.g. 0x0102, 0x0250; 0 when there is none. */
    uint16_t           udf_revision;
    /** UDF only: recording date and time of the primary volume descriptor
     *  (ECMA-167 1/7.3 timestamp, 12 bytes as recorded; year = bytes 2-3 LE). */
    uint8_t            udf_recording_time[12];
} DiscIOFS;

/** Close a file system and set *fs to NULL. */
void ff_discio_fs_close(DiscIOFS **fs);

/** Free a file and set *file to NULL. */
void ff_discio_file_free(DiscIOFile **file);

/**
 * Read len bytes of file at byte pos, from its extents, with the source's
 * attempt count.
 * @return 0, AVERROR(EINVAL) for a range past the end of the file, or a
 *         read error
 */
int ff_discio_file_read(DiscIOFS *fs, const DiscIOFile *file, int64_t pos,
                        uint8_t *buf, int len);

/**
 * Walk path from the root: calls subdir for every component but the last
 * (empty components skipped; '/' and '\\' separate, '/' wins while the rest
 * holds one; the path must start with a separator) and returns the last
 * component in *last. subdir returns 0 and its new directory in *dir, or a
 * negative AVERROR code (AVERROR(ENOENT) for a missing directory).
 * @return 0 or the first error
 */
int ff_discio_walk_path(const char *path, void **dir,
                        int (*subdir)(void *ctx, void **dir, const char *name, int name_len),
                        void *ctx, const char **last);

/**
 * Mount the UDF file system of src with the NetBSD-based reader (the default
 * reader): NetBSD sys/fs/udf, read side, with the changes listed in the file.
 * @return 0 or a negative AVERROR code
 */
int ff_discio_udf_netbsd_mount(DiscIOSource *src, DiscIOFS **out);

/**
 * Mount the UDF file system of src with the Linux-based reader: Linux fs/udf,
 * read side, mounted read-only with default options.
 * @return 0 or a negative AVERROR code
 */
int ff_discio_udf_linux_mount(DiscIOSource *src, DiscIOFS **out);

/**
 * Mount the ISO 9660 file system (joliet = 0) or the Joliet one (joliet = 1)
 * of src: the first primary (and supplementary) volume descriptor of sectors
 * 16-127 must carry CD001 and 2048-byte blocks.
 * @return 0 or a negative AVERROR code
 */
int ff_discio_iso9660_mount(DiscIOSource *src, int joliet, DiscIOFS **out);

/**
 * The first 14 digits (YYYYMMDDHHMMSS) of the primary volume descriptor's
 * volume creation date of an ISO 9660 / Joliet file system, NUL-terminated.
 * @return 0, or AVERROR(EINVAL) when fs is not ISO 9660 / Joliet
 */
int ff_discio_iso9660_creation_date(const DiscIOFS *fs, char date[15]);

#endif /* AVFORMAT_DISCIO_H */
