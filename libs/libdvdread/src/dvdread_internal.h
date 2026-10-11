/*
 * This file is part of libdvdread.
 *
 * libdvdread is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * libdvdread is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with libdvdread; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef LIBDVDREAD_DVDREAD_INTERNAL_H
#define LIBDVDREAD_DVDREAD_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef _WIN32
# include <unistd.h>
#endif /* _WIN32 */

#include "dvdread/dvd_reader.h"
#include "dvdread/ifo_types.h"
#include "logger.h"

#define container_of(ptr, type, member) \
    ((type *)(((char *)(ptr)) - offsetof(type, member)))

/* to determine set paths */
#define DVD_TYPE_STRING(type) ((type==DVD_V)? "VIDEO":"AUDIO")
#define STREAM_TYPE_STRING(type) ((type==DVD_V)? 'V':'A')

typedef enum{
    DVD_V = 0,
    DVD_A = 1,
    DVD_VR = 2,
} dvd_type_t;

struct dvd_reader_s
{
    dvd_reader_device_t *rd;
    dvd_type_t dvd_type;
    void *priv; /* User provided context */
    dvd_logger_cb logcb;
    dvd_reader_filesystem_h* fs;
    /* Set 100 flags for BUP fallback, most signifiant left
       [0] for upper remaining VTS, [1] for the first Main + 63 VTS */
    uint64_t ifoBUPflags[2];
    /* the CSS title keys of the title sets' title VOBs (vob_read.c), per
     * title set: state 0 not looked for, 1 found, -1 none; where its title
     * VOBs lie on an image (base 0: not known, a folder) */
    struct { int state; uint8_t key[5]; int64_t base; uint32_t blocks; } vob_css[100];
};

/* The read position of a file (bytes from its start). */
int64_t DVDFileTell_( dvd_file_t *dvd_file );
/* A file of the disc ("/VIDEO_TS/VTS_01_0.IFO") through the application's
 * file system (DVDOpenFiles()): its size and place; < 0 when there is none. */
int DVDStatFile_( dvd_reader_t *ctx, const char *filename, dvdstat_t *st );
/* The application file system's path of a file of the disc
 * ("/VIDEO_TS/VTS_01_1.VOB"), into full_path (PATH_MAX + 1 bytes); < 0 when
 * there is no such file. */
int DVDFindFile_( dvd_reader_t *ctx, const char *filename, char *full_path );

struct ifo_handle_private_s
{
    ifo_handle_t handle;
    dvd_reader_t *ctx;
    dvd_file_t *file;
    /* References taken with ifoAddRef() on top of the opener's own one */
    unsigned int extra_refs;
    /* For reports: the file read ("VTS_01_0.IFO") and where the bytes of
     * its latest reads lie in it, so a field's byte offset can be told */
    char name[16];
    struct { const uint8_t *data; size_t size; int64_t pos; } span[32];
    unsigned int nb_span;
    /* opened by ifoOpen_rec(): its BUP beside it (NULL: the IFO stands in),
     * reads from the chosen bytes (the IFO read whole, else piece by piece
     * with the BUP for a piece the IFO cannot give) at rec_pos */
    int rec;
    dvd_file_t *bup;
    int bup_only;                       /* the IFO's signature is wrong: only the BUP is read */
    uint8_t *rec_cache;
    int rec_cache_size;
    int64_t rec_pos;
};

enum TagIdentifier {
  /* ECMA 167 3/7.2.1 */
  PrimaryVolumeDescriptor           = 1,
  AnchorVolumeDescriptorPointer     = 2,
  VolumeDescriptorPointer           = 3,
  ImplementationUseVolumeDescriptor = 4,
  PartitionDescriptor               = 5,
  LogicalVolumeDescriptor           = 6,
  UnallocatedSpaceDescriptor        = 7,
  TerminatingDescriptor             = 8,
  LogicalVolumeIntegrityDescriptor  = 9,
  /* ECMA 167 4/7.2.1 */
  FileSetDescriptor                 = 256,
  FileIdentifierDescriptor          = 257,
  AllocationExtentDescriptor        = 258,
  IndirectEntry                     = 259,
  TerminalEntry                     = 260,
  FileEntry                         = 261,
  ExtendedAttributeHeaderDescriptor = 262,
  UnallocatedSpaceEntry             = 263,
  SpaceBitmapDescriptor             = 264,
  PartitionIntegrityEntry           = 265,
  ExtendedFileEntry                 = 266,
};

int InternalUDFReadBlocksRaw(const dvd_reader_t *, uint32_t lb_number,
                     size_t block_count, unsigned char *data, int encrypted);

void *GetUDFCacheHandle(dvd_reader_t *);
void SetUDFCacheHandle(dvd_reader_t *, void *cache);
void FreeUDFCache(void *cache);

#endif /* LIBDVDREAD_DVDREAD_INTERNAL_H */
