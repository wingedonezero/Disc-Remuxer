/*
 * sys/fs/udf/udf_vfsops.c, udf_subr.c, udf_allocation.c, udf_strat_direct.c,
 * udf_vnops.c:
 * Copyright (c) 2006, 2008 Reinoud Zandijk
 * sys/fs/udf/udf_readwrite.c:
 * Copyright (c) 2007, 2008 Reinoud Zandijk
 * sys/kern/vfs_dirhash.c:
 * Copyright (c) 2008, 2025 Reinoud Zandijk
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * sys/fs/udf/ecma167-udf.h (the descriptor layouts, used here as offsets):
 * Copyright (c) 2003, 2004, 2005, 2006, 2008, 2009, 2017, 2018
 * 	Reinoud Zandijk <reinoud@NetBSD.org>
 * Copyright (c) 2001, 2002 Scott Long <scottl@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * sys/fs/udf/udf_osta.c (OSTA UDF reference routines: Unicode compression
 * and uncompression, CRC, file name translation):
 * OSTA compliant Unicode compression, uncompression routines.
 * Copyright 1995 Micro Design International, Inc.
 * Written by Jason M. Rinn.
 * Micro Design International gives permission for the free use of the
 * following source code.
 */

/*
 * Disc I/O: UDF reader based on NetBSD (the default UDF reader).
 *
 * Derived from the read side of NetBSD's UDF file system (sys/fs/udf:
 * udf_vfsops.c udf_mountfs, udf_subr.c, udf_allocation.c, udf_readwrite.c,
 * udf_strat_direct.c, udf_vnops.c udf_read, udf_osta.c; sys/kern/
 * vfs_dirhash.c) and adapted to user space: the device is a disc image read
 * in 2048-byte blocks through ff_discio_read_blocks() (one closed track of
 * the image's size), kernel plumbing (vnodes, buffer cache, locks, pools,
 * write paths, mount options) is left out, kernel printf / DPRINTF become
 * av_log() lines. Upstream function names and comments are kept.
 *
 * Changes from upstream, each kept where the upstream code it changes is:
 * - Kept for compatibility with other readers (named switches in UDFCompat,
 *   all on): a logical volume that is not closed is read anyway; up to 250
 *   allocation extent descriptors per file (upstream 50); a directory entry
 *   whose CRC fails is used when its length equals its CRC length + 16;
 *   names are translated with the WIN_95 / WIN_NT rules of the OSTA code;
 *   the system stream directory is not loaded; listings include hidden
 *   entries; a file whose data is recorded inside its file entry cannot be
 *   opened; a file's extents are one per allocation descriptor, cut where
 *   the translation stops being contiguous, never joined.
 * - The operations the common interface needs, which the kernel does
 *   through the VFS: volume facts (label, UDF revision, recording time),
 *   path walking, listing (the kind of every entry that is not a directory
 *   comes from looking its name up again), opening a file. A name lookup
 *   normalises the searched name with the UNIX rules (only '/' and NUL
 *   change), so names as listed and ".." are found.
 * - Certain endless loops end with an error and a log line, detected by
 *   structure only (never by time): the same file entry location read again
 *   in a chain (also indirect-entry cycles), an FSD chain back to a block
 *   already read, a metadata translation that comes back to a (map, block)
 *   pair, extended allocation descriptors (no reader for them upstream), an
 *   extent that starts at the end of its partition, a volume descriptor of
 *   size 0, a VAT search window that wraps below sector 0, a file extent of
 *   zero blocks.
 * - C undefined behaviour made explicit (an error instead): an empty
 *   partition slot behind a map, a partition reference past the map table,
 *   descriptors / allocation descriptors past their buffer, sparing packet
 *   length 0, a metadata partition without its metadata file.
 * - The direct strategy is used for every volume (upstream picks the rmw
 *   strategy for sparable media: same bytes); file reads are split at
 *   64 KiB boundaries like the buffer cache; the dirhash is a plain list.
 * - The UTF-8 steps of name translation are written from RFC 3629 with the
 *   same fallback rule (a byte that does not start a valid sequence is taken
 *   as ISO-8859-1), not taken from sys/fs/unicode.h.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/attributes.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "discio.h"

/* ---- constants (udf.h, ecma167-udf.h) ---- */

#define UDF_PARTITIONS          4       /* partition descriptors kept */
#define UDF_PMAPS               5       /* partition maps supported */
#define UDF_VTOP_RAWPART        UDF_PMAPS

#define UDF_VTOP_TYPE_RAW       0
#define UDF_VTOP_TYPE_UNKNOWN   0
#define UDF_VTOP_TYPE_PHYS      1
#define UDF_VTOP_TYPE_VIRT      2
#define UDF_VTOP_TYPE_SPARABLE  3
#define UDF_VTOP_TYPE_META      4

#define UDF_MAX_INDIRS_FOLLOW   1024
#define UDF_MAX_ALLOC_EXTENTS   50
#define UDF_MAX_ALLOC_EXTENTS_COMPAT 250

#define UDF_TRANS_ZERO          ((uint64_t)-1)
#define UDF_TRANS_INTERN        ((uint64_t)-3)

#define MAXPHYS                 (64 * 1024)
#define UDF_VAT_CHUNKSIZE       (64 * 1024)
#define UDF_INTEGRITY_CLOSED    1
#define UDF_PART_FLAG_ALLOCATED 1

#define TAGID_SPARING_TABLE     0
#define TAGID_PRI_VOL           1
#define TAGID_ANCHOR            2
#define TAGID_VOL               3
#define TAGID_IMP_VOL           4
#define TAGID_PARTITION         5
#define TAGID_LOGVOL            6
#define TAGID_UNALLOC_SPACE     7
#define TAGID_TERM              8
#define TAGID_LOGVOL_INTEGRITY  9
#define TAGID_FSD               256
#define TAGID_FID               257
#define TAGID_ALLOCEXTENT       258
#define TAGID_INDIRECTENTRY     259
#define TAGID_FENTRY            261
#define TAGID_EXTATTR_HDR       262
#define TAGID_SPACE_BITMAP      264
#define TAGID_EXTFENTRY         266

#define UDF_DESC_TAG_LENGTH     16
#define UDF_FID_SIZE            38
#define UDF_REGID_ID_SIZE       23

#define UDF_EXT_ALLOCATED              0
#define UDF_EXT_ALLOCATED_BUT_NOT_USED 1
#define UDF_EXT_FREE                   2
#define UDF_EXT_REDIRECT               3
#define UDF_EXT_FLAGS(len)      ((uint32_t)(len) >> 30)
#define UDF_EXT_LEN(len)        ((uint32_t)(len) & 0x3fffffff)

#define UDF_ICB_TAG_FLAGS_ALLOC_MASK 0x03
#define UDF_ICB_SHORT_ALLOC     0
#define UDF_ICB_LONG_ALLOC      1
#define UDF_ICB_INTERN_ALLOC    3

#define UDF_ICB_FILETYPE_DIRECTORY     4
#define UDF_ICB_FILETYPE_RANDOMACCESS  5
#define UDF_ICB_FILETYPE_BLOCKDEVICE   6
#define UDF_ICB_FILETYPE_CHARDEVICE    7
#define UDF_ICB_FILETYPE_FIFO          9
#define UDF_ICB_FILETYPE_SOCKET        10
#define UDF_ICB_FILETYPE_SYMLINK       12
#define UDF_ICB_FILETYPE_STREAMDIR     13
#define UDF_ICB_FILETYPE_VAT           248
#define UDF_ICB_FILETYPE_REALTIME      249

#define UDF_FILE_CHAR_VIS       (1 << 0)
#define UDF_FILE_CHAR_DIR       (1 << 1)
#define UDF_FILE_CHAR_DEL       (1 << 2)
#define UDF_FILE_CHAR_PAR       (1 << 3)

/* vnode types the file types map to */
enum { VNON, VREG, VDIR, VBLK, VCHR, VLNK, VSOCK, VFIFO };

#define NAME_MAX_UDF            511     /* NAME_MAX of dirent names */

/* Offsets of the fields used, in the packed structures of ecma167-udf.h. */
/* struct desc_tag */
#define TAG_ID                  0
#define TAG_CKSUM               4
#define TAG_DESC_CRC            8
#define TAG_DESC_CRC_LEN        10
/* struct anchor_vdp */
#define AVDP_MAIN_VDS_EX        16      /* extent_ad: len, loc */
#define AVDP_RESERVE_VDS_EX     24
/* struct pri_vol_desc */
#define PVD_REG_TIME            376
/* struct impvol_desc */
#define IVD_IMPL_ID             20      /* regid: flags, id[23], id_suffix[8] */
#define IVD_LV_INFO_LOGVOL_ID   116     /* _impl_use.lv_info.logvol_id */
/* struct part_desc */
#define PD_FLAGS                20
#define PD_PART_NUM             22
#define PD_START_LOC            188
#define PD_PART_LEN             192
/* struct logvol_desc */
#define LVD_DESC_CHARSET        20
#define LVD_LOGVOL_ID           84
#define LVD_LB_SIZE             212
#define LVD_DOMAIN_ID           216     /* regid; id at +1 */
#define LVD_FSD_LOC             248
#define LVD_MT_L                264
#define LVD_N_PM                268
#define LVD_INTEGRITY_SEQ_LOC   432     /* extent_ad: len, loc */
#define LVD_MAPS                440
/* struct unalloc_sp_desc */
#define USD_ALLOC_DESC_NUM      20
/* struct logvol_int_desc */
#define LVID_TIME               16
#define LVID_INTEGRITY_TYPE     28
#define LVID_NEXT_UNIQUE_ID     40
#define LVID_NUM_PART           72
#define LVID_L_IU               76
#define LVID_TABLES             80
/* struct udf_logvol_info (inside the integrity descriptor) */
#define LVINFO_NUM_FILES        32
#define LVINFO_NUM_DIRECTORIES  36
/* struct space_bitmap_desc */
#define SBD_NUM_BYTES           20
/* struct udf_sparing_table */
#define SPT_RT_L                48
#define SPT_ENTRIES             56      /* spare_map_entry: org, map */
/* struct fileset_desc */
#define FSD_LOGVOL_ID           112
#define FSD_ROOTDIR_ICB         400
#define FSD_NEXT_EX             448
#define FSD_STREAMDIR_ICB       464
/* struct fileid_desc */
#define FID_FILE_CHAR           18
#define FID_L_FI                19
#define FID_ICB                 20
#define FID_L_IU                36
#define FID_DATA                38
/* struct icb_tag inside file_entry / extfile_entry / indirect_entry */
#define ICB_STRAT_TYPE          20
#define ICB_FILE_TYPE           27
#define ICB_FLAGS               34
/* struct indirect_entry */
#define INDE_INDIRECT_ICB       36
/* struct file_entry and struct extfile_entry */
#define FE_INF_LEN              56
#define FE_MTIME                84
#define FE_UNIQUE_ID            160
#define FE_L_EA                 168
#define FE_L_AD                 172
#define FE_DATA                 176
#define EFE_MTIME               92
#define EFE_UNIQUE_ID           200
#define EFE_L_EA                208
#define EFE_L_AD                212
#define EFE_DATA                216
/* struct alloc_ext_entry */
#define AEE_L_AD                20
#define AEE_DATA                24
/* struct extattrhdr_desc, struct extattr_entry, struct impl_extattr_entry */
#define EAHDR_IMPL_ATTR_LOC     16
#define EAHDR_APPL_ATTR_LOC     20
#define EAHDR_SIZE              24
#define EXTATTR_ENTRY_SIZE      12
#define IMPLEXT_IU_L            12
#define IMPLEXT_IMP_ID          16      /* regid; id at +1 */
#define IMPLEXT_DATA            48
#define VATLVEXT_SIZE           144     /* struct vatlvext_extattr_entry */
/* struct udf_vat, struct udf_oldvat_tail */
#define UDF_VAT_SIZE            153
#define UDF_OLDVAT_TAIL_SIZE    36
/* part_map_spare / part_map_meta (type 2 partition maps) */
#define PM2_PART_ID             5       /* part_id.id */
#define PM2_PART_NUM            38
#define PMS_PACKET_LEN          40
#define PMS_N_ST                42
#define PMS_ST_LOC              48
#define PMM_META_FILE_LBN       40
#define PMM_META_MIRROR_FILE_LBN 44
#define PMM_META_BITMAP_FILE_LBN 48
#define PMM_ALLOC_UNIT_SIZE     52
#define PMM_ALIGNMENT_UNIT_SIZE 56
#define PMM_FLAGS               58

/* ---- data structures ---- */

struct lb_addr {
    uint32_t lb_num;
    uint16_t part_num;
};

struct long_ad {
    uint32_t       len;
    struct lb_addr loc;
};

static struct long_ad udf_long_ad(const uint8_t *b)
{
    struct long_ad ad = { AV_RL32(b), { AV_RL32(b + 4), AV_RL16(b + 8) } };
    return ad;
}

/** Which OS variant of UDFTransName / IsIllegal translates names. */
enum { UDF_NAMES_UNIX, UDF_NAMES_WINDOWS };

/** Rules that differ from the NetBSD code (all on). */
typedef struct UDFCompat {
    int accept_unclean_volume;   /* a logical volume not marked closed is read */
    int alloc_extent_limit_250;  /* up to 250 allocation extent descriptors */
    int lenient_fid_crc;         /* bad FID CRC accepted with a consistent length */
    int name_rules;              /* UDF_NAMES_WINDOWS: WIN_95 / WIN_NT rules */
    int skip_stream_directory;   /* the system stream directory is not loaded */
    int list_hidden;             /* listings include hidden entries */
    int embedded_data_not_a_file;/* data inside the file entry: not opened */
} UDFCompat;

static const UDFCompat udf_compat_default = {
    .accept_unclean_volume    = 1,
    .alloc_extent_limit_250   = 1,
    .lenient_fid_crc          = 1,
    .name_rules               = UDF_NAMES_WINDOWS,
    .skip_stream_directory    = 1,
    .list_hidden              = 1,
    .embedded_data_not_a_file = 1,
};

typedef struct UDFDirhashEntry {
    char    *name;
    uint64_t offset;
} UDFDirhashEntry;

/** A loaded file entry (struct udf_node, read side). */
typedef struct UDFNode {
    struct UDFNode *hnext;          /* node cache chain */
    struct long_ad  loc;
    uint8_t        *fe, *efe;       /* lb_size bytes each */
    uint8_t        *ext[UDF_MAX_ALLOC_EXTENTS_COMPAT];
    int             num_extensions;
    int             file_type;
    uint64_t        file_size;
    int             v_type;
    /* the directory hash (struct dirhash): live entries in directory order */
    int              dirh_complete, dirh_broken;
    UDFDirhashEntry *dirh;
    int              dirh_nb, dirh_alloc;
} UDFNode;

#define UDF_NODE_HASH 1024

/** The read side of struct udf_mount. */
typedef struct UDFMount {
    DiscIOSource *src;
    void         *logctx;
    const UDFCompat *compat;
    char          why[256];         /* the reason of the last failure */

    uint32_t sector_size;           /* discinfo.sector_size */
    uint32_t psize;                 /* sectors of the image */
    uint32_t packet_size;

    uint8_t *anchors[4];
    int      num_anchors;
    uint8_t *primary_vol, *logical_vol, *unallocated, *implementation;
    uint32_t logical_vol_len;
    uint8_t *partitions[UDF_PARTITIONS];
    uint8_t *logvol_integrity;
    uint32_t logvol_integrity_len;
    uint32_t logvol_info;           /* offset of udf_logvol_info in it */

    int vtop[UDF_PMAPS + 1];
    int vtop_tp[UDF_PMAPS + 1];
    int data_part, node_part, fids_part;

    uint32_t first_possible_vat_location, last_possible_vat_location;
    uint32_t sparable_packet_size;
    uint8_t *sparing_table;
    uint32_t sparing_table_len;
    UDFNode *metadata_node, *metadatamirror_node, *metadatabitmap_node;
    uint32_t metadata_alloc_unit_size;
    uint16_t metadata_alignment_unit_size;
    uint8_t  metadata_flags;
    UDFNode *vat_node;
    uint8_t *vat_table;
    uint32_t vat_table_len;
    uint32_t vat_offset, vat_entries;
    uint8_t *fileset_desc;

    UDFNode *nodes[UDF_NODE_HASH];  /* the node cache (vcache) */
} UDFMount;

/* ---- logging ---- */

#define UDF_LOG(ump, level, ...) av_log((ump)->logctx, level, __VA_ARGS__)

/** Records why something failed (for the message of the caller), logs it at
 *  debug level and returns err. */
static av_printf_format(3, 4) int udf_err(UDFMount *ump, int err, const char *fmt, ...)
{
    va_list vl;

    va_start(vl, fmt);
    vsnprintf(ump->why, sizeof(ump->why), fmt, vl);
    va_end(vl);
    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': %s\n", ump->src->name, ump->why);
    return err;
}

/* ---- udf_osta.c ---- */

/*
 * CRC 010041
 */
static const unsigned short crc_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};

static unsigned short udf_cksum(const unsigned char *s, int n)
{
    unsigned short crc = 0;

    while (n-- > 0)
        crc = crc_table[(crc >> 8 ^ *s++) & 0xff] ^ (crc << 8);
    return crc;
}

/* UNICODE Checksum */
static unsigned short udf_unicode_cksum(const uint16_t *s, int n)
{
    unsigned short crc = 0;

    while (n-- > 0) {
        /* Take high order byte first--corresponds to a big endian
         * byte stream.
         */
        crc = crc_table[(crc >> 8 ^ (*s >> 8)) & 0xff] ^ (crc << 8);
        crc = crc_table[(crc >> 8 ^ (*s++ & 0xff)) & 0xff] ^ (crc << 8);
    }
    return crc;
}

/*
 * Calculates a 16-bit checksum of the Implementation Use
 * Extended Attribute header or Application Use Extended Attribute
 * header. The fields AttributeType through ImplementationIdentifier
 * (or ApplicationIdentifier) inclusively represent the
 * data covered by the checksum (48 bytes).
 */
static uint16_t udf_ea_cksum(const uint8_t *data)
{
    uint16_t checksum = 0;

    for (int count = 0; count < 48; count++)
        checksum += *data++;
    return checksum;
}

/*
 * Takes an OSTA CS0 compressed unicode name, and converts it to Unicode.
 * Returns the number of unicode characters which were uncompressed, -1 if
 * the compression ID is invalid. Bytes past avail read as 0 (the recorded
 * name may claim more bytes than its buffer holds); unicode needs room for
 * numberOfBytes characters.
 */
static int udf_UncompressUnicode(int numberOfBytes, const uint8_t *UDFCompressed, int avail,
                                 uint16_t *unicode)
{
#define UDF_BYTE(i) ((i) < avail ? UDFCompressed[i] : 0)
    unsigned int compID;
    int unicodeIndex = 0, byteIndex = 1;

    compID = UDF_BYTE(0);
    /* Translate 254/255 compID values used for deleted entries */
    if (compID == 254)
        compID = 8;
    if (compID == 255)
        compID = 16;
    /* First check for valid compID. */
    if (compID != 8 && compID != 16)
        return -1;
    /* Loop through all the bytes. */
    while (byteIndex < numberOfBytes) {
        if (compID == 16) {
            /* Move the first byte to the high bits of the unicode char. */
            unicode[unicodeIndex] = UDF_BYTE(byteIndex) << 8;
            byteIndex++;
        } else {
            unicode[unicodeIndex] = 0;
        }
        if (byteIndex < numberOfBytes) {
            /* Then the next byte to the low bits. */
            unicode[unicodeIndex] |= UDF_BYTE(byteIndex);
            byteIndex++;
        }
        unicodeIndex++;
    }
    return unicodeIndex;
#undef UDF_BYTE
}

/*
 * Takes a string of unicode wide characters and returns an OSTA CS0
 * compressed unicode string. Returns the total number of bytes in the
 * compressed OSTA CS0 string, including the compression ID.
 */
static int udf_CompressUnicode(int numberOfChars, int compID, const uint16_t *unicode,
                               uint8_t *UDFCompressed)
{
    int byteIndex = 1, unicodeIndex = 0;

    /* Place compression code in first byte. */
    UDFCompressed[0] = compID;
    while (unicodeIndex < numberOfChars) {
        if (compID == 16) {
            /* First, place the high bits of the char into the byte stream. */
            UDFCompressed[byteIndex++] = (unicode[unicodeIndex] & 0xFF00) >> 8;
        }
        /* Then place the low bits into the stream. */
        UDFCompressed[byteIndex++] = unicode[unicodeIndex] & 0x00FF;
        unicodeIndex++;
    }
    return byteIndex;
}

#define MAXLEN              255
#define ILLEGAL_CHAR_MARK   0x005F
#define CRC_MARK            0x0023
#define EXT_SIZE            5
#define PERIOD              0x002E
#define SPACE               0x0020

static int UnicodeIsPrint(uint16_t ch)
{
    return (ch >= ' ') && (ch != 127);
}

/* Decides whether the given character is illegal for the given OS rules. */
static int IsIllegal(uint16_t ch, int rules)
{
    if (rules == UDF_NAMES_UNIX) {
        /* Illegal UNIX characters are NULL and slash. */
        return ch == 0x0000 || ch == 0x002F;
    }
    /* Illegal char's for OS/2 according to WARP toolkit (also WIN_95 /
     * WIN_NT). */
    return ch < 0x0020 || (ch < 0x80 && ch && strchr("\\/:*?\"<>|", ch));
}

/*
 * Translates a long file name to one using a MAXLEN and an illegal char set
 * in accord with the OSTA requirements. Assumes the name has already been
 * translated to Unicode. udfName holds nraw characters, udfLen of them are
 * the name; characters past nraw read as 0 (only the extension look-ahead
 * goes past udfLen). newName needs room for MAXLEN + 10 characters.
 *
 * Returns the number of unicode characters in the translated name.
 */
static int UDFTransName(uint16_t *newName, const uint16_t *udfName, int nraw, int udfLen, int rules)
{
#define UDF_CHAR(i) ((i) < nraw ? udfName[i] : 0)
#define UDF_BAD(c)  (IsIllegal(c, rules) || !UnicodeIsPrint(c))
    int Index, newIndex = 0, needsCRC = 0;
    int extIndex = 0, newExtIndex = 0, hasExt = 0;
    int trailIndex = 0;
    unsigned short valueCRC;
    uint16_t current;
    static const char hexChar[] = "0123456789ABCDEF";

    for (Index = 0; Index < udfLen; Index++) {
        current = UDF_CHAR(Index);
        if (UDF_BAD(current)) {
            needsCRC = 1;
            /* Replace Illegal and non-displayable chars with underscore. */
            current = ILLEGAL_CHAR_MARK;
            /* Skip any other illegal or non-displayable characters. */
            while (Index + 1 < udfLen && UDF_BAD(UDF_CHAR(Index + 1)))
                Index++;
        }
        /* Record position of extension, if one is found. */
        if (current == PERIOD && (udfLen - Index - 1) <= EXT_SIZE) {
            if (udfLen == Index + 1) {
                /* A trailing period is NOT an extension. */
                hasExt = 0;
            } else {
                hasExt = 1;
                extIndex = Index;
                newExtIndex = newIndex;
            }
        } else if (rules == UDF_NAMES_WINDOWS && current != PERIOD && current != SPACE) {
            /* Record position of last char which is NOT period or space. */
            trailIndex = newIndex;
        }
        if (newIndex < MAXLEN)
            newName[newIndex++] = current;
        else
            needsCRC = 1;
    }
    /* For OS2, 95 & NT, truncate any trailing periods and\or spaces. */
    if (rules == UDF_NAMES_WINDOWS && trailIndex != newIndex - 1) {
        /* For an empty name this is 1: the output starts with a NUL there,
         * which ends the converted name. */
        while (newIndex < trailIndex + 1)
            newName[newIndex++] = 0;
        newIndex = trailIndex + 1;
        needsCRC = 1;
        hasExt = 0; /* Trailing period does not make an extension. */
    }
    if (needsCRC) {
        uint16_t ext[EXT_SIZE];
        int localExtIndex = 0;

        if (hasExt) {
            int maxFilenameLen;

            /* Translate extension, and store it in ext. */
            for (Index = 0; Index < EXT_SIZE && extIndex + Index + 1 < udfLen; Index++) {
                current = UDF_CHAR(extIndex + Index + 1);
                if (UDF_BAD(current)) {
                    /* Replace Illegal and non-displayable chars with
                     * underscore. */
                    current = ILLEGAL_CHAR_MARK;
                    /* Skip any other illegal or non-displayable chars. */
                    while (Index + 1 < EXT_SIZE && UDF_BAD(UDF_CHAR(extIndex + Index + 2)))
                        Index++;
                }
                ext[localExtIndex++] = current;
            }
            /* Truncate filename to leave room for extension and CRC. */
            maxFilenameLen = ((MAXLEN - 5) - localExtIndex - 1);
            if (newIndex > maxFilenameLen)
                newIndex = maxFilenameLen;
            else
                newIndex = newExtIndex;
        } else if (newIndex > MAXLEN - 5) {
            /* If no extension, make sure to leave room for CRC. */
            newIndex = MAXLEN - 5;
        }
        newName[newIndex++] = CRC_MARK; /* Add mark for CRC. */
        /* Calculate CRC from original filename from FileIdentifier. */
        valueCRC = udf_unicode_cksum(udfName, FFMIN(udfLen, nraw));
        /* Convert 16-bits of CRC to hex characters. */
        newName[newIndex++] = hexChar[(valueCRC & 0xf000) >> 12];
        newName[newIndex++] = hexChar[(valueCRC & 0x0f00) >> 8];
        newName[newIndex++] = hexChar[(valueCRC & 0x00f0) >> 4];
        newName[newIndex++] = hexChar[(valueCRC & 0x000f)];
        /* Place a translated extension at end, if found. */
        if (hasExt) {
            newName[newIndex++] = PERIOD;
            for (Index = 0; Index < localExtIndex; Index++)
                newName[newIndex++] = ext[Index];
        }
    }
    return newIndex;
#undef UDF_BAD
#undef UDF_CHAR
}

/* ---- UTF-8 (RFC 3629) for udf_to_unix_name / unix_to_udf_name ---- */

/* One 16-bit unit as UTF-8 if n bytes are left; returns the bytes written
 * (0 when there is no room). Lone surrogates are encoded as three bytes like
 * any other unit from 0x800. */
static int put_utf8(uint8_t *s, int n, uint16_t wc)
{
    if (wc & 0xf800) {
        if (n < 3)
            return 0;
        s[0] = 0xE0 | (wc >> 12);
        s[1] = 0x80 | ((wc >> 6) & 0x3F);
        s[2] = 0x80 | (wc & 0x3F);
        return 3;
    }
    if (wc & 0x0780) {
        if (n < 2)
            return 0;
        s[0] = 0xC0 | (wc >> 6);
        s[1] = 0x80 | (wc & 0x3F);
        return 2;
    }
    if (n < 1)
        return 0;
    s[0] = wc;
    return 1;
}

/* One character from UTF-8 bytes (one to three bytes, lead and continuation
 * patterns, no overlong check); a byte that does not start a valid sequence
 * is taken as ISO-8859-1. *used gets the bytes taken. */
static uint16_t get_utf8(const uint8_t *s, size_t sz, int *used)
{
    static const int utf_count[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 2, 2, 3, 0 };
    int c = utf_count[s[0] >> 4];

    if (c == 0 || (size_t)c > sz)
        c = 1;
    if (c == 2 && (s[1] & 0xc0) == 0x80) {
        *used = 2;
        return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if (c == 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *used = 3;
        return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    *used = 1;
    return s[0];
}

/* ---- names (udf_subr.c) ---- */

static const char osta_id[] = "OSTA Compressed Unicode";

/* udf_osta_charset */
static void udf_osta_charset(uint8_t charspec[64])
{
    memset(charspec, 0, 64);
    memcpy(charspec + 1, osta_id, sizeof(osta_id) - 1);
}

/* charspec is OSTA CS0: type 0 and "OSTA Compressed Unicode" */
static int udf_is_osta_typ0(const uint8_t *chsp)
{
    return chsp[0] == 0 && !memcmp(chsp + 1, osta_id, sizeof(osta_id));
}

/*
 * A recorded name (len bytes at id, of which avail are in the buffer) as
 * UTF-8 in result: at most result_len bytes, then a NUL (result needs
 * result_len + 1 bytes). The name ends at its first NUL.
 * Returns the length.
 */
static int udf_to_unix_name(UDFMount *ump, char *result, int result_len, const uint8_t *id, int avail,
                            int len, const uint8_t *chsp, int rules)
{
    uint8_t *outchp = (uint8_t *)result;
    int out = 0;

    if (udf_is_osta_typ0(chsp)) {
        uint16_t raw_name[256], unix_name[MAXLEN + 10];
        int nraw, ucode_chars, nice_uchars, n = FFMIN(len, 256);

        nraw = ucode_chars = udf_UncompressUnicode(n, id, avail, raw_name);
        if (ucode_chars >= 0) {
            /* MIN(ucode_chars, UnicodeLength(raw_name)): the name ends at its first NUL */
            int l = 0;
            while (l < ucode_chars && raw_name[l])
                l++;
            ucode_chars = l;
        }
        nice_uchars = UDFTransName(unix_name, raw_name, FFMAX(nraw, 0), ucode_chars, rules);
        /* output UTF8 */
        for (int i = 0; i < nice_uchars; i++) {
            uint16_t ch = unix_name[i];
            int nout = put_utf8(outchp + out, result_len - out, ch);
            out += nout;
            if (!ch)
                break;
        }
    } else {
        /* assume 8bit char length byte latin-1 */
        if (avail < 1 || id[0] != 8)
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': a name in a non-OSTA character set does not "
                    "start with the 8-bit marker\n", ump->src->name);
        for (int i = 1; i < avail && id[i] && out < result_len; i++)
            outchp[out++] = id[i];
    }
    /* the result is a C string */
    for (int i = 0; i < out; i++)
        if (!outchp[i]) {
            out = i;
            break;
        }
    outchp[out] = 0;
    return out;
}

/*
 * A UTF-8 name to OSTA CS0 (8-bit when every character fits, else 16-bit) in
 * result (room for 1 + 2 * name_len bytes); *result_len gets its length in
 * an 8-bit field, as on disc.
 */
static void unix_to_udf_name(uint8_t *result, uint8_t *result_len, const char *name, int name_len,
                             uint16_t *raw_name /* room for name_len units */)
{
    const uint8_t *inchp = (const uint8_t *)name;
    size_t cnt = name_len;
    int udf_chars = 0, bits = 8;

    /* convert utf8 to unicode-16 */
    while (cnt) {
        int used;
        raw_name[udf_chars] = get_utf8(inchp, cnt, &used);
        if (raw_name[udf_chars] > 0xff)
            bits = 16;
        inchp += used;
        cnt   -= used;
        udf_chars++;
    }
    udf_chars = udf_CompressUnicode(udf_chars, bits, raw_name, result);
    *result_len = udf_chars;
}

/* ---- descriptor checks (udf_subr.c) ---- */

/*
 * Check if the blob starts with a good UDF tag. Tags are protected by a
 * checksum over the header except one byte at position 4 that is the checksum
 * itself.
 */
static int udf_check_tag(const uint8_t *tag)
{
    uint8_t sum = 0;

    for (int cnt = 0; cnt < 16; cnt++)
        if (cnt != TAG_CKSUM)
            sum += tag[cnt];
    /* bad tag header checksum; this is not a valid tag */
    return sum == tag[TAG_CKSUM] ? 0 : AVERROR_INVALIDDATA;
}

/*
 * check tag payload will check descriptor CRC as specified.
 * If the descriptor is too long, it will return 1 otherwise -1 (bad CRC).
 * buf_len is the size of the buffer holding the descriptor: a CRC area past
 * it is a broken descriptor (bad CRC).
 */
static int udf_check_tag_payload(const uint8_t *tag, uint32_t buf_len, uint32_t max_length)
{
    uint16_t crc_len = AV_RL16(tag + TAG_DESC_CRC_LEN);

    /* check payload CRC if applicable */
    if (crc_len == 0)
        return 0;
    if (crc_len > max_length)
        return 1;
    if (UDF_DESC_TAG_LENGTH + (uint32_t)crc_len > buf_len ||
        udf_cksum(tag + UDF_DESC_TAG_LENGTH, crc_len) != AV_RL16(tag + TAG_DESC_CRC)) {
        /* bad payload CRC; this is a broken tag */
        return -1;
    }
    return 0;
}

/*
 * XXX note the different semantics from udfclient: for FIDs it still rounds
 * up to sectors. Use udf_fidsize() for a correct length.
 * The fixed sizes are those of the packed on-disc structures; dscr holds at
 * least 2048 bytes.
 */
static uint32_t udf_tagsize(const uint8_t *dscr, uint32_t lb_size)
{
    uint32_t size, num_lb;

    switch (AV_RL16(dscr + TAG_ID)) {
    case TAGID_LOGVOL:          /* sizeof(struct logvol_desc) - 1 + mt_l */
        size = 440 + AV_RL32(dscr + LVD_MT_L);
        break;
    case TAGID_UNALLOC_SPACE:   /* sizeof(unalloc_sp_desc) - elmsz + n * elmsz */
        size = 24 + AV_RL32(dscr + USD_ALLOC_DESC_NUM) * 8;
        break;
    case TAGID_FID:
        size = UDF_FID_SIZE + dscr[FID_L_FI] + AV_RL16(dscr + FID_L_IU);
        size = (size + 3) & ~3u;
        break;
    case TAGID_LOGVOL_INTEGRITY: /* sizeof(logvol_int_desc) - 4 + l_iu + 2 * num_part * 4 */
        size = 80 + AV_RL32(dscr + LVID_L_IU) + 2 * AV_RL32(dscr + LVID_NUM_PART) * 4;
        break;
    case TAGID_SPACE_BITMAP:    /* sizeof(space_bitmap_desc) - 1 + num_bytes */
        size = 24 + AV_RL32(dscr + SBD_NUM_BYTES);
        break;
    case TAGID_SPARING_TABLE:   /* sizeof(udf_sparing_table) - elmsz + rt_l * elmsz */
        size = 56 + AV_RL16(dscr + SPT_RT_L) * 8u;
        break;
    case TAGID_FENTRY:          /* sizeof(file_entry) + l_ea + l_ad - 1 */
        size = 177 + AV_RL32(dscr + FE_L_EA) + AV_RL32(dscr + FE_L_AD) - 1;
        break;
    case TAGID_EXTFENTRY:
        size = 217 + AV_RL32(dscr + EFE_L_EA) + AV_RL32(dscr + EFE_L_AD) - 1;
        break;
    case TAGID_FSD:             /* sizeof(struct fileset_desc) */
    default:                    /* sizeof(union dscrptr) */
        size = 512;
        break;
    }

    if ((size == 0) || (lb_size == 0))
        return 0;
    if (lb_size == 1)
        return size;
    /* round up in sectors */
    num_lb = (size + lb_size - 1) / lb_size;
    return num_lb * lb_size;
}

static uint32_t udf_fidsize(const uint8_t *fid)
{
    uint32_t size = UDF_FID_SIZE + fid[FID_L_FI] + AV_RL16(fid + FID_L_IU);
    return (size + 3) & ~3u;
}

/* ---- reads (udf_readwrite.c, udf_strat_direct.c) ---- */

/* SYNC reading of n blocks from specified sector */
static int udf_read_phys_sectors(UDFMount *ump, uint8_t *blob, uint32_t start, uint32_t sectors)
{
    uint32_t sector_size = ump->sector_size;
    int64_t rblkno = start;

    while (sectors > 0) {
        uint32_t piece = FFMIN(MAXPHYS / sector_size, sectors);
        int ret = ff_discio_read_blocks(ump->src, rblkno * sector_size, blob, piece * sector_size,
                                        ump->src->attempts, 0);
        if (ret < 0)
            return udf_err(ump, ret, "reading sector %"PRId64" failed (%s)", rblkno, av_err2str(ret));
        rblkno += piece;
        blob   += piece * sector_size;
        sectors -= piece;
    }
    return 0;
}

/*
 * synchronous generic descriptor read: *dstp gets the descriptor (*lenp its
 * buffer size, whole sectors), or NULL for a blank block (an all-zero sector
 * passes the tag checksum, so in practice a blank block comes back as a
 * zero-filled descriptor with tag id 0).
 */
static int udf_read_phys_dscr(UDFMount *ump, uint32_t sector, uint8_t **dstp, uint32_t *lenp)
{
    uint32_t sector_size = ump->sector_size, dscrlen, buf_len = sector_size;
    uint8_t *dst;
    int error, i;

    *dstp = NULL;
    *lenp = 0;
    /* read initial piece */
    if (!(dst = av_malloc(sector_size)))
        return AVERROR(ENOMEM);
    if ((error = udf_read_phys_sectors(ump, dst, sector, 1)) < 0) {
        av_free(dst);
        return error;
    }
    /* check if its a valid tag */
    if (udf_check_tag(dst) < 0) {
        /* check if its an empty block */
        for (i = 0; i < (int)sector_size; i++)
            if (dst[i])
                break;
        av_free(dst);
        if (i == (int)sector_size) {
            /* return no error but with no dscrptr */
            UDF_LOG(ump, AV_LOG_TRACE, "UDF on '%s': sector %"PRIu32" is blank\n", ump->src->name, sector);
            return 0;
        }
        return udf_err(ump, AVERROR_INVALIDDATA, "descriptor at sector %"PRIu32" has a bad tag checksum", sector);
    }
    /* calculate descriptor size */
    dscrlen = udf_tagsize(dst, sector_size);

    /* compared as signed values: a size of 2^31 or more reads no further sectors */
    if ((int32_t)dscrlen > (int32_t)sector_size) {
        uint32_t sectors = (dscrlen + sector_size - 1) / sector_size;
        uint8_t *new_dst;

        UDF_LOG(ump, AV_LOG_TRACE, "UDF on '%s': sector %"PRIu32": multi block descriptor of %"PRIu32" bytes\n",
                ump->src->name, sector, dscrlen);
        if (!(new_dst = av_realloc(dst, (size_t)sectors * sector_size))) {
            av_free(dst);
            return AVERROR(ENOMEM);
        }
        dst = new_dst;
        buf_len = sectors * sector_size;
        if ((error = udf_read_phys_sectors(ump, dst + sector_size, sector + 1, sectors - 1)) < 0) {
            av_free(dst);
            return error;
        }
    }
    error = udf_check_tag_payload(dst, buf_len, dscrlen);
    if (error) {
        av_free(dst);
        if (error > 0)
            return udf_err(ump, AVERROR_INVALIDDATA,
                           "descriptor at sector %"PRIu32" has a CRC length larger than the descriptor", sector);
        return udf_err(ump, AVERROR_INVALIDDATA, "descriptor at sector %"PRIu32" has a bad CRC", sector);
    }
    *dstp = dst;
    *lenp = buf_len;
    return 0;
}

static int udf_translate_vtop(UDFMount *ump, const struct long_ad *icb_loc,
                              uint32_t *lb_numres, uint32_t *extres);

/* udf_read_logvol_dscr through udf_read_nodedscr_direct: translates the ICB
 * address, reads the descriptor and keeps one logical block of it. */
static int udf_read_logvol_dscr(UDFMount *ump, const struct long_ad *icb, uint8_t **dscrptr)
{
    uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    uint32_t sector, dummy, len;
    uint8_t *tmpdscr;
    int error;

    *dscrptr = NULL;
    if ((error = udf_translate_vtop(ump, icb, &sector, &dummy)) < 0)
        return error;
    /* try to read in fe/efe */
    if ((error = udf_read_phys_dscr(ump, sector, &tmpdscr, &len)) < 0)
        return error;
    if (!tmpdscr) /* not reachable (see udf_read_phys_dscr); upstream would copy from NULL */
        return udf_err(ump, AVERROR_INVALIDDATA, "the descriptor at block %"PRIu32" of partition "
                       "reference %u is a blank block", icb->loc.lb_num, icb->loc.part_num);
    if (!(*dscrptr = av_malloc(lb_size))) {
        av_free(tmpdscr);
        return AVERROR(ENOMEM);
    }
    memcpy(*dscrptr, tmpdscr, FFMIN(lb_size, len));
    if (lb_size > len)
        memset(*dscrptr + len, 0, lb_size - len);
    av_free(tmpdscr);
    return 0;
}

/* ---- allocation (udf_allocation.c) ---- */

static const uint8_t *udf_node_dscr(const UDFNode *node)
{
    return node->fe ? node->fe : node->efe;
}

static int udf_node_icbflags(const UDFNode *node)
{
    return AV_RL16(udf_node_dscr(node) + ICB_FLAGS);
}

/* offset of data, l_ea, l_ad of the node's file entry */
static void udf_node_ad_area(const UDFNode *node, uint32_t *base, uint32_t *l_ea, uint32_t *l_ad)
{
    if (node->fe) {
        *base = FE_DATA;
        *l_ea = AV_RL32(node->fe + FE_L_EA);
        *l_ad = AV_RL32(node->fe + FE_L_AD);
    } else {
        *base = EFE_DATA;
        *l_ea = AV_RL32(node->efe + EFE_L_EA);
        *l_ad = AV_RL32(node->efe + EFE_L_AD);
    }
}

static uint64_t udf_node_inf_len(const UDFNode *node)
{
    return AV_RL64(udf_node_dscr(node) + FE_INF_LEN);
}

static uint64_t udf_node_unique_id(const UDFNode *node)
{
    return node->fe ? AV_RL64(node->fe + FE_UNIQUE_ID) : AV_RL64(node->efe + EFE_UNIQUE_ID);
}

static const uint8_t *udf_node_mtime(const UDFNode *node)
{
    return node->fe ? node->fe + FE_MTIME : node->efe + EFE_MTIME;
}

/*
 * udf_get_adslot: allocation descriptor number slot of node, through its
 * allocation extent descriptors. Short descriptors take the partition of the
 * node itself. Returns 0 (*eof set at the end) or AVERROR_INVALIDDATA where
 * upstream cannot produce an element and its callers would loop forever on
 * the same slot: extended allocation descriptors (not read upstream), or
 * descriptors that run past the descriptor buffer (*why says which).
 */
static int udf_get_adslot(UDFMount *ump, const UDFNode *node, uint32_t slot, struct long_ad *icb,
                          int *eof, const char **why)
{
    const uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    const uint8_t *buf = udf_node_dscr(node);
    uint32_t base, l_ea, l_ad, offset, adlen;
    uint64_t data_pos;
    int addr_type, extnr = -1;

    udf_node_ad_area(node, &base, &l_ea, &l_ad);
    addr_type = udf_node_icbflags(node) & UDF_ICB_TAG_FLAGS_ALLOC_MASK;
    memset(icb, 0, sizeof(*icb));
    *eof = 0;

    /* just in case we're called on an intern, its EOF */
    if (addr_type == UDF_ICB_INTERN_ALLOC) {
        *eof = 1;
        return 0;
    }
    if (addr_type == UDF_ICB_SHORT_ALLOC) {
        adlen = 8;          /* sizeof(struct short_ad) */
    } else if (addr_type == UDF_ICB_LONG_ALLOC) {
        adlen = 16;         /* sizeof(struct long_ad) */
    } else {
        /* Extended descriptors are not read. With an empty descriptor area
         * the lookup still ends: the word at the area start is no allocation
         * extent pointer, or there is no extent to go to. */
        if (l_ad == 0) {
            uint64_t at = (uint64_t)base + l_ea;
            uint32_t word = at + 4 <= lb_size ? AV_RL32(buf + at) : 0;

            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': file entry at block %"PRIu32" uses extended "
                    "allocation descriptors\n", ump->src->name, node->loc.loc.lb_num);
            if (word < 0xc0000000u || !node->num_extensions) {
                *eof = 1;
                return 0;
            }
        }
        *why = "extended allocation descriptors";
        return AVERROR_INVALIDDATA;
    }

#define UDF_ELEMENT(at, out) do {                                               \
        if ((at) + adlen > lb_size) {                                           \
            *why = "allocation descriptors run past the descriptor";            \
            return AVERROR_INVALIDDATA;                                         \
        }                                                                       \
        (out).len = AV_RL32(buf + (at));                                        \
        (out).loc.lb_num = AV_RL32(buf + (at) + 4);                             \
        (out).loc.part_num = addr_type == UDF_ICB_SHORT_ALLOC ?                 \
            node->loc.loc.part_num : AV_RL16(buf + (at) + 8);                   \
    } while (0)

    /* if offset too big, we go to the allocation extensions */
    data_pos = (uint64_t)base + l_ea;
    offset   = slot * adlen;
    while (offset >= l_ad) {
        struct long_ad l_icb;

        /* check if our last entry is a redirect (with an empty area this
         * reads the bytes just before it, as upstream does) */
        if (data_pos + l_ad < adlen) {
            *why = "allocation descriptors run past the descriptor";
            return AVERROR_INVALIDDATA;
        }
        UDF_ELEMENT(data_pos + l_ad - adlen, l_icb);
        if (UDF_EXT_FLAGS(l_icb.len) != UDF_EXT_REDIRECT) {
            l_ad = 0;   /* force EOF */
            break;
        }
        /* advance to next extent */
        extnr++;
        if (extnr >= node->num_extensions) {
            l_ad = 0;   /* force EOF */
            break;
        }
        offset   = offset - l_ad;
        buf      = node->ext[extnr];
        l_ad     = AV_RL32(buf + AEE_L_AD);
        data_pos = AEE_DATA;    /* sizeof(struct alloc_ext_entry) - 1 */
    }

    /* XXX l_ad == 0 should be enough to check */
    if ((offset >= l_ad) || (l_ad == 0)) {
        *eof = 1;
        return 0;
    }
    /* get the element */
    UDF_ELEMENT(data_pos + offset, *icb);
    return 0;
#undef UDF_ELEMENT
}

/* udf_vat_read: one 32-bit VAT entry at byte offset of the table */
static int udf_vat_read(UDFMount *ump, uint32_t *blob, uint32_t offset)
{
    if ((uint64_t)offset + 4 > (uint64_t)ump->vat_offset + (uint64_t)ump->vat_entries * 4 ||
        (uint64_t)offset + 4 > ump->vat_table_len)
        return AVERROR_INVALIDDATA;
    *blob = AV_RL32(ump->vat_table + offset);
    return 0;
}

/*
 * udf_translate_vtop: logical block icb_loc to an absolute sector, and how
 * many blocks from there are contiguous.
 */
static int udf_translate_vtop(UDFMount *ump, const struct long_ad *icb_loc,
                              uint32_t *lb_numres, uint32_t *extres)
{
    const uint8_t *pdesc;
    uint32_t lb_size, len, lb_num, lb_rel, lb_packet, udf_rw32_lbmap, ext_offset;
    uint64_t foffset, end_foffset;
    uint32_t *seen = NULL;      /* metadata blocks seen in this translation */
    int nb_seen = 0, vpart, part, slot, flags, eof, error;
    struct long_ad s_icb_loc;
    const char *why;

    vpart  = icb_loc->loc.part_num;
    lb_num = icb_loc->loc.lb_num;
    if (vpart > UDF_VTOP_RAWPART)
        goto fail;

translate_again:
    if (vpart > UDF_VTOP_RAWPART) {
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': a metadata extent names partition reference %d, "
                "past the map table\n", ump->src->name, vpart);
        goto fail;
    }
    part  = ump->vtop[vpart];
    pdesc = part < UDF_PARTITIONS ? ump->partitions[part] : NULL;

    switch (ump->vtop_tp[vpart]) {
    case UDF_VTOP_TYPE_RAW:
        /* 1:1 to the end of the device */
        *lb_numres = lb_num;
        *extres = INT32_MAX;
        av_free(seen);
        return 0;
    case UDF_VTOP_TYPE_PHYS:
        /* an empty partition slot behind the map (upstream: NULL access) */
        if (!pdesc)
            goto fail;
        /* transform into its disc logical block */
        if (lb_num > AV_RL32(pdesc + PD_PART_LEN))
            goto fail;
        *lb_numres = lb_num + AV_RL32(pdesc + PD_START_LOC);
        /* extent from here to the end of the partition */
        *extres = AV_RL32(pdesc + PD_PART_LEN) - lb_num;
        av_free(seen);
        return 0;
    case UDF_VTOP_TYPE_VIRT:
        /* only maps one logical block, lookup in VAT */
        if (lb_num >= ump->vat_entries)     /* XXX > or >= ? */
            goto fail;
        /* lookup in virtual allocation table file */
        if (udf_vat_read(ump, &udf_rw32_lbmap, ump->vat_offset + lb_num * 4) < 0)
            goto fail;
        lb_num = udf_rw32_lbmap;
        if (!pdesc)
            goto fail;
        /* transform into its disc logical block */
        if (lb_num > AV_RL32(pdesc + PD_PART_LEN))
            goto fail;
        *lb_numres = lb_num + AV_RL32(pdesc + PD_START_LOC);
        /* just one logical block */
        *extres = 1;
        av_free(seen);
        return 0;
    case UDF_VTOP_TYPE_SPARABLE:
        /* sparing packet length 0 (upstream: division by zero) */
        if (!ump->sparable_packet_size || !ump->sparing_table)
            goto fail;
        /* check if the packet containing the lb_num is remapped */
        lb_packet = lb_num / ump->sparable_packet_size;
        lb_rel    = lb_num % ump->sparable_packet_size;
        for (int rel = 0; rel < AV_RL16(ump->sparing_table + SPT_RT_L); rel++) {
            uint32_t at = SPT_ENTRIES + 8 * rel;
            if (at + 8 > ump->sparing_table_len)
                break;
            if (lb_packet == AV_RL32(ump->sparing_table + at)) {
                /* NOTE maps to absolute disc logical block! */
                *lb_numres = AV_RL32(ump->sparing_table + at + 4) + lb_rel;
                *extres    = ump->sparable_packet_size - lb_rel;
                av_free(seen);
                return 0;
            }
        }
        if (!pdesc)
            goto fail;
        /* transform into its disc logical block */
        if (lb_num > AV_RL32(pdesc + PD_PART_LEN))
            goto fail;
        *lb_numres = lb_num + AV_RL32(pdesc + PD_START_LOC);
        /* rest of block */
        *extres = ump->sparable_packet_size - lb_rel;
        av_free(seen);
        return 0;
    case UDF_VTOP_TYPE_META:
        /* a (map, block) pair seen again is a certain endless loop (only one
         * metadata map exists) */
        for (int i = 0; i < nb_seen; i++)
            if (seen[i] == lb_num) {
                UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': metadata partition translation loops back "
                        "to block %"PRIu32"; stopped\n", ump->src->name, lb_num);
                goto fail;
            }
        if (!(nb_seen & (nb_seen - 1))) {
            uint32_t *n = av_realloc_array(seen, nb_seen ? 2 * nb_seen : 4, sizeof(*seen));
            if (!n) {
                av_free(seen);
                return AVERROR(ENOMEM);
            }
            seen = n;
        }
        seen[nb_seen++] = lb_num;
        /* a metadata partition without its metadata file (upstream: NULL access) */
        if (!ump->metadata_node)
            goto fail;

        /* we have to look into the file's allocation descriptors */
        lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);

        /* get first overlapping extent */
        foffset = 0;
        slot    = 0;
        for (;;) {
            if ((error = udf_get_adslot(ump, ump->metadata_node, slot, &s_icb_loc, &eof, &why)) < 0) {
                UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': metadata file: %s\n", ump->src->name, why);
                goto fail;
            }
            if (eof) {
                UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': meta partition translation failed: "
                        "can't seek location\n", ump->src->name);
                goto fail;
            }
            len   = s_icb_loc.len;
            flags = UDF_EXT_FLAGS(len);
            len   = UDF_EXT_LEN(len);

            if (flags == UDF_EXT_REDIRECT) {
                slot++;
                continue;
            }
            end_foffset = foffset + len;
            if (end_foffset > (uint64_t)lb_num * lb_size)
                break;  /* found */
            foffset = end_foffset;
            slot++;
        }
        /* found overlapping slot (32-bit, as upstream) */
        ext_offset = lb_num * lb_size - (uint32_t)foffset;

        /* process extent offset */
        lb_num  = s_icb_loc.loc.lb_num;
        vpart   = s_icb_loc.loc.part_num;
        lb_num += (ext_offset + lb_size - 1) / lb_size;

        if (flags != UDF_EXT_ALLOCATED) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': metadata partition translation failed: "
                    "not allocated\n", ump->src->name);
            goto fail;
        }
        /*
         * vpart and lb_num are updated, translate again since we
         * might be mapped on sparable media
         */
        goto translate_again;
    default:
        break;
    }

fail:
    av_free(seen);
    return udf_err(ump, AVERROR_INVALIDDATA, "logical block %"PRIu32" of partition reference %u could "
                   "not be translated to a sector", icb_loc->loc.lb_num, icb_loc->loc.part_num);
}

/*
 * Translate an extent (in logical_blocks) into logical block numbers; used
 * for read operations. DOESN'T check extents. map gets num_lb entries: a
 * sector or UDF_TRANS_ZERO, or a single UDF_TRANS_INTERN for data recorded
 * in the ICB.
 */
static int udf_translate_file_extent(UDFMount *ump, const UDFNode *udf_node, uint32_t from,
                                     uint32_t num_lb, uint64_t *map)
{
    struct long_ad t_ad, s_ad;
    uint64_t transsec, foffset, end_foffset;
    uint32_t transsec32, lb_size, ext_offset, lb_num, len, overlap, translen;
    uint16_t vpart_num;
    int eof, error, flags, slot;
    const char *why;

    lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);

    /* do the work */
    if ((udf_node_icbflags(udf_node) & UDF_ICB_TAG_FLAGS_ALLOC_MASK) == UDF_ICB_INTERN_ALLOC) {
        *map = UDF_TRANS_INTERN;
        return 0;
    }

#define UDF_GET_ADSLOT() do {                                                                   \
        if ((error = udf_get_adslot(ump, udf_node, slot, &s_ad, &eof, &why)) < 0) {             \
            UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': file entry at block %"PRIu32": %s\n",    \
                    ump->src->name, udf_node->loc.loc.lb_num, why);                             \
            return udf_err(ump, error, "the file entry at block %"PRIu32" of partition "        \
                           "reference %u could not be read: %s", udf_node->loc.loc.lb_num,      \
                           udf_node->loc.loc.part_num, why);                                    \
        }                                                                                       \
    } while (0)

    /* find first overlapping extent */
    foffset = 0;
    slot    = 0;
    for (;;) {
        UDF_GET_ADSLOT();
        if (eof) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': translate file extent failed: can't seek location\n",
                    ump->src->name);
            return udf_err(ump, AVERROR_INVALIDDATA, "a file block lies beyond the file's allocation descriptors");
        }
        len   = s_ad.len;
        flags = UDF_EXT_FLAGS(len);
        len   = UDF_EXT_LEN(len);

        if (flags == UDF_EXT_REDIRECT) {
            slot++;
            continue;
        }
        end_foffset = foffset + len;
        if (end_foffset > (uint64_t)from * lb_size)
            break;  /* found */
        foffset = end_foffset;
        slot++;
    }
    /* found overlapping slot */
    ext_offset = (uint64_t)from * lb_size - foffset;

    for (;;) {
        UDF_GET_ADSLOT();
        if (eof) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': translate file extent failed: past eof\n",
                    ump->src->name);
            return udf_err(ump, AVERROR_INVALIDDATA, "a file block lies beyond the file's allocation descriptors");
        }
        len   = s_ad.len;
        flags = UDF_EXT_FLAGS(len);
        len   = UDF_EXT_LEN(len);

        lb_num    = s_ad.loc.lb_num;
        vpart_num = s_ad.loc.part_num;

        end_foffset = foffset + len;

        /* process extent, don't forget to advance on ext_offset! */
        lb_num    += (ext_offset + lb_size - 1) / lb_size;
        overlap    = (len - ext_offset + lb_size - 1) / lb_size;
        ext_offset = 0;

        /*
         * note that the while(){} is necessary for the extent that
         * the udf_translate_vtop() returns doesn't have to span the
         * whole extent.
         */
        overlap = FFMIN(overlap, num_lb);
        while (overlap && (flags != UDF_EXT_REDIRECT)) {
            switch (flags) {
            case UDF_EXT_FREE:
            case UDF_EXT_ALLOCATED_BUT_NOT_USED:
                transsec = UDF_TRANS_ZERO;
                translen = overlap;
                while (overlap && num_lb && translen) {
                    *map++ = transsec;
                    lb_num++;
                    overlap--; num_lb--; translen--;
                }
                break;
            case UDF_EXT_ALLOCATED:
                t_ad.len = 0;
                t_ad.loc.lb_num   = lb_num;
                t_ad.loc.part_num = vpart_num;
                if ((error = udf_translate_vtop(ump, &t_ad, &transsec32, &translen)) < 0)
                    return error;
                transsec = transsec32;
                if (translen == 0) {
                    /* no progress is possible: upstream would repeat this
                     * translation forever */
                    UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': file entry at block %"PRIu32": an extent "
                            "starts at the end of its partition\n", ump->src->name, udf_node->loc.loc.lb_num);
                    return udf_err(ump, AVERROR_INVALIDDATA, "the file entry at block %"PRIu32" of partition "
                                   "reference %u could not be read: an extent starts at the end of its partition",
                                   udf_node->loc.loc.lb_num, udf_node->loc.loc.part_num);
                }
                while (overlap && num_lb && translen) {
                    *map++ = transsec;
                    lb_num++; transsec++;
                    overlap--; num_lb--; translen--;
                }
                break;
            }
        }
        if (num_lb == 0)
            break;

        if (flags != UDF_EXT_REDIRECT)
            foffset = end_foffset;
        slot++;
    }
    return 0;
#undef UDF_GET_ADSLOT
}

/* ---- nodes (udf_subr.c) ---- */

static void udf_dispose_node(UDFNode *node)
{
    if (!node)
        return;
    av_free(node->fe);
    av_free(node->efe);
    for (int i = 0; i < node->num_extensions; i++)
        av_free(node->ext[i]);
    for (int i = 0; i < node->dirh_nb; i++)
        av_free(node->dirh[i].name);
    av_free(node->dirh);
    av_free(node);
}

typedef struct UDFChainLoc {
    uint32_t lb_num;
    uint16_t part_num;
    int      strat4096;
} UDFChainLoc;

/*
 * udf_loadvnode: reads the file entry chain at key (indirect entries,
 * strategy 4096) and the allocation extent descriptors.
 */
static int udf_loadvnode(UDFMount *ump, const struct long_ad *key, UDFNode **out)
{
    uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    const int limit = ump->compat->alloc_extent_limit_250 ? UDF_MAX_ALLOC_EXTENTS_COMPAT
                                                          : UDF_MAX_ALLOC_EXTENTS;
    struct long_ad node_icb_loc, icb_loc;
    uint32_t sector, dummy;
    UDFChainLoc *visited = NULL;
    int nb_visited = 0, strat4096 = 0, num_indir_followed = 0, slot, eof, error;
    const char *fail_why = NULL;
    UDFNode *udf_node;
    uint8_t *dscr;

#define NODE_FAIL(reason) do { fail_why = (reason); goto fail; } while (0)

    *out = NULL;
    memset(&node_icb_loc, 0, sizeof(node_icb_loc));
    node_icb_loc.len = lb_size;
    node_icb_loc.loc = key->loc;

    /* garbage check: translate udf_node_icb_loc to sectornr */
    if (udf_translate_vtop(ump, &node_icb_loc, &sector, &dummy) < 0) {
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': can't translate icb address\n", ump->src->name);
        return udf_err(ump, AVERROR_INVALIDDATA, "the file entry at block %"PRIu32" of partition reference %u "
                       "could not be read: can't translate icb address", key->loc.lb_num, key->loc.part_num);
    }

    /* build udf_node (do initialise!) */
    if (!(udf_node = av_mallocz(sizeof(*udf_node))))
        return AVERROR(ENOMEM);
    udf_node->loc = node_icb_loc;

    icb_loc = node_icb_loc;
    for (;;) {
        int dscr_type, strat, known = 0;

        /* locations already read in this chain, with the 4096 state: one seen
         * again means the chain can never end */
        for (int i = 0; i < nb_visited; i++)
            if (visited[i].lb_num == icb_loc.loc.lb_num && visited[i].part_num == icb_loc.loc.part_num &&
                visited[i].strat4096 == strat4096)
                known = 1;
        if (known) {
            UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': the file entry chain of block %"PRIu32" returns to "
                    "block %"PRIu32"; it would never end\n", ump->src->name, key->loc.lb_num,
                    icb_loc.loc.lb_num);
            NODE_FAIL("file entry chain loops");
        }
        if (!(nb_visited & (nb_visited - 1))) {
            UDFChainLoc *n = av_realloc_array(visited, nb_visited ? 2 * nb_visited : 4, sizeof(*visited));
            if (!n) {
                error = AVERROR(ENOMEM);
                goto fail_error;
            }
            visited = n;
        }
        visited[nb_visited].lb_num    = icb_loc.loc.lb_num;
        visited[nb_visited].part_num  = icb_loc.loc.part_num;
        visited[nb_visited].strat4096 = strat4096;
        nb_visited++;

        /* try to read in fe/efe */
        if ((error = udf_read_logvol_dscr(ump, &icb_loc, &dscr)) < 0) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': node fe/efe failed: %s\n", ump->src->name, ump->why);
            NODE_FAIL("file entry unreadable");
        }

        /* process descriptor based on the descriptor type */
        dscr_type = AV_RL16(dscr + TAG_ID);

        /* if dealing with an indirect entry, follow the link */
        if (dscr_type == TAGID_INDIRECTENTRY) {
            icb_loc = udf_long_ad(dscr + INDE_INDIRECT_ICB);
            av_free(dscr);
            if (++num_indir_followed > UDF_MAX_INDIRS_FOLLOW)
                NODE_FAIL("too many indirect entries");
            continue;
        }

        /* only file entries and extended file entries allowed here */
        if ((dscr_type != TAGID_FENTRY) && (dscr_type != TAGID_EXTFENTRY)) {
            av_free(dscr);
            NODE_FAIL("not a file entry");
        }

        if (udf_tagsize(dscr, lb_size) != lb_size)
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': file entry at block %"PRIu32" is longer than one block\n",
                    ump->src->name, icb_loc.loc.lb_num);

        /* record and process/update (ext)fentry */
        strat = AV_RL16(dscr + ICB_STRAT_TYPE);
        udf_node->file_type = dscr[ICB_FILE_TYPE];
        udf_node->file_size = AV_RL64(dscr + FE_INF_LEN);
        if (dscr_type == TAGID_FENTRY) {
            av_free(udf_node->fe);
            udf_node->fe = dscr;
        } else {
            av_free(udf_node->efe);
            udf_node->efe = dscr;
        }

        /*
         * Strategy 4096 is a daisy linked chain terminating with an
         * unrecorded sector or a TERM descriptor. The next
         * descriptor is to be found in the sector that follows the
         * current sector.
         */
        if (strat == 4096) {
            strat4096 = 1;
            icb_loc.loc.lb_num = icb_loc.loc.lb_num + 1;
        }

        /*
         * Strategy 4 is the normal strategy and terminates, but if
         * we're in strategy 4096, we can't have strategy 4 mixed in
         */
        if (strat == 4) {
            if (strat4096)
                NODE_FAIL("strategy 4 entry inside a strategy 4096 chain");
            break;      /* done */
        }
        /* any other strategy: upstream reads the same location again,
         * forever; the visited check above stops it */
    }
    av_freep(&visited);

    /*
     * Go through all allocations extents of this descriptor and when
     * encountering a redirect read in the allocation extension. These are
     * daisy-chained.
     */
    udf_node->num_extensions = 0;
    slot = 0;
    for (;;) {
        const char *why;

        if (udf_get_adslot(ump, udf_node, slot, &icb_loc, &eof, &why) < 0)
            NODE_FAIL(why);
        if (eof)
            break;
        slot++;

        if (UDF_EXT_FLAGS(icb_loc.len) != UDF_EXT_REDIRECT)
            continue;

        if (udf_node->num_extensions >= limit) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': udf_get_node: implementation limit, too many "
                    "allocation extensions on udf_node\n", ump->src->name);
            NODE_FAIL("too many allocation extent descriptors");
        }
        /* length can only be *one* lb : UDF 2.50/2.3.7.1 */
        if (UDF_EXT_LEN(icb_loc.len) != lb_size) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': udf_get_node: bad allocation extension size in "
                    "udf_node\n", ump->src->name);
            NODE_FAIL("bad allocation extent descriptor size");
        }
        /* load in allocation extent */
        if (udf_read_logvol_dscr(ump, &icb_loc, &dscr) < 0)
            NODE_FAIL("allocation extent descriptor unreadable");
        /* process read-in descriptor */
        if (AV_RL16(dscr + TAG_ID) != TAGID_ALLOCEXTENT) {
            av_free(dscr);
            NODE_FAIL("not an allocation extent descriptor");
        }
        udf_node->ext[udf_node->num_extensions++] = dscr;
    }

    /*
     * Translate UDF filetypes into vnode types.
     *
     * Systemfiles like the meta main and mirror files are not treated as
     * normal files, so we type them as having no type. UDF dictates that
     * they are not allowed to be visible.
     */
    switch (udf_node->file_type) {
    case UDF_ICB_FILETYPE_DIRECTORY:
    case UDF_ICB_FILETYPE_STREAMDIR:    udf_node->v_type = VDIR;  break;
    case UDF_ICB_FILETYPE_BLOCKDEVICE:  udf_node->v_type = VBLK;  break;
    case UDF_ICB_FILETYPE_CHARDEVICE:   udf_node->v_type = VCHR;  break;
    case UDF_ICB_FILETYPE_SOCKET:       udf_node->v_type = VSOCK; break;
    case UDF_ICB_FILETYPE_FIFO:         udf_node->v_type = VFIFO; break;
    case UDF_ICB_FILETYPE_SYMLINK:      udf_node->v_type = VLNK;  break;
    case UDF_ICB_FILETYPE_RANDOMACCESS:
    case UDF_ICB_FILETYPE_REALTIME:     udf_node->v_type = VREG;  break;
    default:
        /* VAT, metadata files and anything else */
        udf_node->v_type = VNON;
    }
    UDF_LOG(ump, AV_LOG_TRACE, "UDF on '%s': node at block %"PRIu32" partition %u: file type %d, "
            "%"PRIu64" bytes, %d allocation extent descriptor(s)\n", ump->src->name, key->loc.lb_num,
            key->loc.part_num, udf_node->file_type, udf_node->file_size, udf_node->num_extensions);
    *out = udf_node;
    return 0;

fail:
    error = udf_err(ump, AVERROR_INVALIDDATA, "the file entry at block %"PRIu32" of partition reference %u "
                    "could not be read: %s", key->loc.lb_num, key->loc.part_num, fail_why);
fail_error:
    av_free(visited);
    udf_dispose_node(udf_node);
    return error;
#undef NODE_FAIL
}

/* udf_get_node: the node at node_icb_loc, from the cache or loaded */
static int udf_get_node(UDFMount *ump, const struct long_ad *node_icb_loc, UDFNode **udf_noderes)
{
    unsigned h = (node_icb_loc->loc.lb_num * 31u + node_icb_loc->loc.part_num) % UDF_NODE_HASH;
    UDFNode *node;
    int error;

    *udf_noderes = NULL;
    for (node = ump->nodes[h]; node; node = node->hnext)
        if (node->loc.loc.lb_num == node_icb_loc->loc.lb_num &&
            node->loc.loc.part_num == node_icb_loc->loc.part_num) {
            *udf_noderes = node;
            return 0;
        }
    if ((error = udf_loadvnode(ump, node_icb_loc, &node)) < 0)
        return error;
    node->hnext = ump->nodes[h];
    ump->nodes[h] = node;
    *udf_noderes = node;
    return 0;
}

/* ---- file reads (udf_subr.c, udf_vnops.c) ---- */

/* udf_read_internal: data recorded in the file entry, then zeros to the end
 * of the block (blob holds lb_size bytes) */
static void udf_read_internal(UDFMount *ump, const UDFNode *node, uint8_t *blob)
{
    uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    const uint8_t *d = udf_node_dscr(node);
    uint32_t base, l_ea, l_ad;
    uint64_t start, inflen = udf_node_inf_len(node), n;

    udf_node_ad_area(node, &base, &l_ea, &l_ad);
    /*
     * XXX there should be real bounds-checking logic here,
     * in case ->l_ea or ->inf_len contains nonsense.
     * (bounded here by the descriptor's block)
     */
    start = FFMIN((uint64_t)base + l_ea, lb_size);
    n = FFMIN(FFMIN(inflen, lb_size - start), lb_size);
    /* copy out info */
    memcpy(blob, d + start, n);
    memset(blob + n, 0, lb_size - n);
}

/* udf_read_filebuf: sectors whole blocks of the file from block from */
static int udf_read_filebuf(UDFMount *ump, const UDFNode *udf_node, uint32_t from, uint32_t sectors,
                            uint8_t *buf)
{
    uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    uint64_t mapping[MAXPHYS / DISCIO_BLOCK_SIZE];
    int error;

    if (sectors > FF_ARRAY_ELEMS(mapping))
        return AVERROR_BUG;
    if ((error = udf_translate_file_extent(ump, udf_node, from, sectors, mapping)) < 0)
        return error;

    /* pre-check if its an internal */
    if (mapping[0] == UDF_TRANS_INTERN) {
        udf_read_internal(ump, udf_node, buf);
        memset(buf + lb_size, 0, (size_t)(sectors - 1) * lb_size);
        return 0;
    }

    /* request read-in of data from disc scheduler */
    for (uint32_t sector = 0; sector < sectors; sector++) {
        uint8_t *buf_pos = buf + (size_t)sector * lb_size;
        uint64_t run_start;
        uint32_t run_length;

        /* check if its zero or unmapped to stop reading */
        if (mapping[sector] == UDF_TRANS_ZERO) {
            /* copy zero sector */
            memset(buf_pos, 0, lb_size);
            continue;
        }
        run_start  = mapping[sector];
        run_length = 1;
        while (sector < sectors - 1) {
            if (mapping[sector + 1] != mapping[sector] + 1)
                break;
            run_length++;
            sector++;
        }
        if (run_start > UINT32_MAX)
            return udf_err(ump, AVERROR_INVALIDDATA, "logical block %"PRIu32" of partition reference %u "
                           "could not be translated to a sector", from, udf_node->loc.loc.part_num);
        if ((error = udf_read_phys_sectors(ump, buf_pos, run_start, run_length)) < 0)
            return error;
    }
    return 0;
}

/* Blocks per buffer: MAXPHYS (64 KiB) of 2048-byte sectors. Reads are split
 * at these boundaries, as the buffer cache does. */
#define BUF_BLOCKS 32

/*
 * udf_read: up to len bytes of node at pos; *got gets how many were read
 * (fewer only at the end of the file). Each block is translated through the
 * allocation descriptors, not recorded blocks read as zeros, data recorded in
 * the file entry is copied from it.
 */
static int udf_read(UDFMount *ump, const UDFNode *node, uint64_t pos, uint8_t *dst, size_t len,
                    size_t *got)
{
    uint64_t file_size = udf_node_inf_len(node);
    uint64_t lb = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    uint8_t *data;
    size_t done = 0;
    int error = 0;

    *got = 0;
    if (pos >= file_size || !len)
        return 0;
    len = FFMIN(file_size - pos, len);
    if (!(data = av_malloc(BUF_BLOCKS * lb)))
        return AVERROR(ENOMEM);
    while (done < len) {
        uint64_t at = pos + done;
        uint64_t from = at / lb;
        /* one buffer: up to the next 64 KiB boundary or the end of the read */
        uint64_t buf_end_block = (from / BUF_BLOCKS + 1) * BUF_BLOCKS;
        uint64_t last_block = (pos + len - 1) / lb;
        uint64_t to = FFMIN(buf_end_block, last_block + 1);
        size_t skip = at - from * lb, n;

        if ((error = udf_read_filebuf(ump, node, from > UINT32_MAX ? UINT32_MAX : (uint32_t)from,
                                      to - from, data)) < 0)
            break;
        n = FFMIN((to - from) * lb - skip, len - done);
        memcpy(dst + done, data + skip, n);
        done += n;
    }
    av_free(data);
    if (error < 0)
        return error;
    *got = len;
    return 0;
}

/* ---- directories (udf_subr.c, vfs_dirhash.c) ---- */

/* One file identifier descriptor read by udf_read_fid_stream. */
typedef struct UDFFid {
    int            file_char;
    struct long_ad icb;
    char           name[NAME_MAX_UDF + 1];  /* as the dirent gets it ("..", for the parent) */
} UDFFid;

/* why udf_read_fid_stream returned no entry */
enum {
    FID_OK    = 0,
    FID_END   = 1,      /* *offset >= file_size (EINVAL): end of the directory */
    FID_SHORT = 2,      /* fewer than 38 bytes left (EIO, no message) */
    FID_BROKEN = 3,     /* "UDF: BROKEN DIRECTORY ENTRY" (EIO) */
    FID_READ  = 4,      /* the read itself failed (*err) */
};

/* udf_read_fid_stream: the file identifier descriptor at *offset of
 * directory dir_node; moves *offset past it. */
static int udf_read_fid_stream(UDFMount *ump, const UDFNode *dir_node, uint64_t *offset, UDFFid *out,
                               int *err)
{
    uint64_t file_size = udf_node_inf_len(dir_node);
    uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE), fid_size, l_iu, start;
    uint8_t *fid;
    size_t want, got;
    int error;

    *err = 0;
    /* check if we're past the end of the directory */
    if (*offset >= file_size)
        return FID_END;
    if (file_size - *offset < UDF_FID_SIZE) {
        /* short dir ... */
        return FID_SHORT;
    }
    if (!(fid = av_mallocz(lb_size))) {
        *err = AVERROR(ENOMEM);
        return FID_READ;
    }
    want = FFMIN(file_size - *offset, lb_size);
    if ((error = udf_read(ump, dir_node, *offset, fid, want, &got)) < 0 || got != want) {
        *err = error < 0 ? error
                         : udf_err(ump, AVERROR_INVALIDDATA, "a read of %zu bytes at %"PRIu64" runs past the "
                                   "end of a %"PRIu64"-byte file", want, *offset, file_size);
        av_free(fid);
        return FID_READ;
    }

    /* check if our FID header is OK */
    if (udf_check_tag(fid) < 0 || AV_RL16(fid + TAG_ID) != TAGID_FID)
        goto brokendir;
    /* check for length */
    fid_size = udf_fidsize(fid);
    if (file_size - *offset < fid_size)
        goto brokendir;
    /* check FID contents */
    if (udf_check_tag_payload(fid, lb_size, lb_size)) {
        uint32_t crc_len = AV_RL16(fid + TAG_DESC_CRC_LEN);

        if (ump->compat->lenient_fid_crc && fid_size == crc_len + 16) {
            UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': directory entry at offset %"PRIu64" of the directory "
                    "at block %"PRIu32" has a bad CRC; its length is consistent, so it is used\n",
                    ump->src->name, *offset, dir_node->loc.loc.lb_num);
        } else {
            goto brokendir;
        }
    }

    /* create resulting dirent structure */
    l_iu  = AV_RL16(fid + FID_L_IU);
    start = FFMIN(FID_DATA + l_iu, lb_size);
    udf_to_unix_name(ump, out->name, NAME_MAX_UDF, fid + start, lb_size - start, fid[FID_L_FI],
                     ump->logical_vol + LVD_DESC_CHARSET, ump->compat->name_rules);
    out->file_char = fid[FID_FILE_CHAR];
    /* '..' has no name, so provide one */
    if (out->file_char & UDF_FILE_CHAR_PAR)
        strcpy(out->name, "..");
    out->icb = udf_long_ad(fid + FID_ICB);
    av_free(fid);

    /* advance */
    *offset += fid_size;
    return FID_OK;

brokendir:
    /* note that is sometimes a bit quick to report */
    UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': BROKEN DIRECTORY ENTRY (directory at block %"PRIu32", "
            "offset %"PRIu64")\n", ump->src->name, dir_node->loc.loc.lb_num, *offset);
    av_free(fid);
    return FID_BROKEN;
}

static void udf_dirhash_purge(UDFNode *dir_node)
{
    for (int i = 0; i < dir_node->dirh_nb; i++)
        av_freep(&dir_node->dirh[i].name);
    dir_node->dirh_nb = 0;
}

/* udf_dirhash_fill: builds the directory hash of dir_node once. A broken
 * entry or a read error marks it broken; every later lookup fails. */
static int udf_dirhash_fill(UDFMount *ump, UDFNode *dir_node)
{
    uint64_t file_size, pre_diroffset, diroffset;
    UDFFid fid;
    int error = 0;

    if (dir_node->dirh_broken)
        return udf_err(ump, AVERROR_INVALIDDATA, "a directory holds a broken entry");
    if (dir_node->dirh_complete)
        return 0;

    /* make sure we have a clean dirhash to add to */
    udf_dirhash_purge(dir_node);
    file_size = udf_node_inf_len(dir_node);
    diroffset = 0;
    while (diroffset < file_size) {
        int stop, err;

        /* transfer a new fid/dirent */
        pre_diroffset = diroffset;
        stop = udf_read_fid_stream(ump, dir_node, &diroffset, &fid, &err);
        if (stop != FID_OK) {
            dir_node->dirh_broken = 1;
            udf_dirhash_purge(dir_node);
            error = stop == FID_READ ? err
                                     : udf_err(ump, AVERROR_INVALIDDATA, "a directory holds a broken entry");
            break;
        }
        if (fid.file_char & UDF_FILE_CHAR_DEL)
            continue;   /* deleted extent: not entered */
        /* append to the dirhash */
        if (dir_node->dirh_nb == dir_node->dirh_alloc) {
            int n = dir_node->dirh_alloc ? 2 * dir_node->dirh_alloc : 16;
            UDFDirhashEntry *e = av_realloc_array(dir_node->dirh, n, sizeof(*e));
            if (!e) {
                error = AVERROR(ENOMEM);
                break;
            }
            dir_node->dirh = e;
            dir_node->dirh_alloc = n;
        }
        if (!(dir_node->dirh[dir_node->dirh_nb].name = av_strdup(fid.name))) {
            error = AVERROR(ENOMEM);
            break;
        }
        dir_node->dirh[dir_node->dirh_nb++].offset = pre_diroffset;
    }
    if (error == AVERROR(ENOMEM)) {
        udf_dirhash_purge(dir_node);
        return error;
    }
    dir_node->dirh_complete = 1;
    return error;
}

/*
 * udf_lookup_name_in_dir: the ICB of name in dir_node (*found = 0 when there
 * is none). The name is first brought to canonical form (OSTA CS0 and back)
 * with the UNIX name rules, which change only '/' and NUL: a name as listed
 * (also one changed by the Windows rules) and ".." are found as they are.
 * With duplicate names, the last one in directory order wins (entries are
 * inserted at the head of their hash line).
 */
static int udf_lookup_name_in_dir(UDFMount *ump, UDFNode *dir_node, const char *name, int namelen,
                                  struct long_ad *icb_loc, int *found)
{
    uint8_t osta_charspec[64], *compressed;
    uint16_t *raw;
    uint8_t l_fi;
    char s_name[NAME_MAX_UDF + 1];
    int error;

    /* set default return */
    *found = 0;
    memset(icb_loc, 0, sizeof(*icb_loc));

    /* get our dirhash and make sure its read in */
    if ((error = udf_dirhash_fill(ump, dir_node)) < 0)
        return error;

    /* convert given unix name to canonical unix name */
    compressed = av_malloc(1 + 2 * (size_t)namelen);
    raw = av_malloc_array(FFMAX(namelen, 1), sizeof(*raw));
    if (!compressed || !raw) {
        av_free(compressed);
        av_free(raw);
        return AVERROR(ENOMEM);
    }
    udf_osta_charset(osta_charspec);
    unix_to_udf_name(compressed, &l_fi, name, namelen, raw);
    udf_to_unix_name(ump, s_name, NAME_MAX_UDF, compressed, FFMIN(1 + 2 * (size_t)namelen, 256), l_fi,
                     osta_charspec, UDF_NAMES_UNIX);
    av_free(compressed);
    av_free(raw);

    /* search our dirhash hits */
    for (int i = dir_node->dirh_nb - 1; i >= 0; i--) {
        uint64_t diroffset;
        UDFFid fid;
        int stop, err;

        if (strcmp(dir_node->dirh[i].name, s_name))
            continue;
        /* check this hit */
        diroffset = dir_node->dirh[i].offset;
        /* transfer a new fid/dirent */
        stop = udf_read_fid_stream(ump, dir_node, &diroffset, &fid, &err);
        if (stop == FID_READ)
            return err;
        if (stop != FID_OK)
            return udf_err(ump, AVERROR_INVALIDDATA, "a directory holds a broken entry");
        /* see if its our entry */
        if (!strcmp(fid.name, s_name)) {
            *found = 1;
            *icb_loc = fid.icb;
            return 0;
        }
    }
    return 0;
}

/* ---- mounting (udf_subr.c) ---- */

static int udf_read_anchor(UDFMount *ump, uint32_t sector, uint8_t **dst)
{
    uint32_t len;
    int error = udf_read_phys_dscr(ump, sector, dst, &len);

    if (error < 0)
        return error;
    /* blank terminator blocks are not allowed here */
    if (!*dst)
        return AVERROR(ENOENT);
    if (AV_RL16(*dst + TAG_ID) != TAGID_ANCHOR) {
        av_freep(dst);
        UDF_LOG(ump, AV_LOG_TRACE, "UDF on '%s': sector %"PRIu32" is not an anchor\n", ump->src->name, sector);
        return AVERROR(ENOENT);
    }
    return 0;
}

/*
 * udf_read_anchors for an image (one closed track of psize sectors): anchors
 * at 256, end - 256, end and 512, in that order.
 */
static int udf_read_anchors(UDFMount *ump)
{
    uint32_t track_start = 0;
    uint32_t track_end = track_start + ump->psize - 1;
    uint32_t positions[4];
    int ok = 0;

    /* get our packet size: take max, but not bigger than 64 */
    ump->packet_size = FFMIN(MAXPHYS / ump->sector_size, 64);

    /* read anchors start+256, start+512, end-256, end */
    positions[0] = track_start + 256;
    positions[1] = track_end - 256;
    positions[2] = track_end;
    positions[3] = track_start + 512;   /* [UDF 2.60/6.11.2] */
    /* XXX shouldn't +512 be preferred over +256 for compat with Roxio CD */

    for (int anch = 0; anch < 4; anch++) {
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': reading anchor %d at sector %"PRIu32"\n",
                ump->src->name, anch, positions[anch]);
        if (udf_read_anchor(ump, positions[anch], &ump->anchors[ok]) == 0)
            ok++;
    }

    /* VATs are only recorded on sequential media, but initialise */
    ump->first_possible_vat_location = track_start + 2;
    ump->last_possible_vat_location  = track_end;
    ump->num_anchors = ok;
    return ok;
}

/*
 * BUGALERT: some rogue implementations use random physical partition
 * numbers to break other implementations so lookup the number.
 * Returns the slot holding it, else the first empty slot, else UDF_PARTITIONS.
 */
static int udf_find_raw_phys(UDFMount *ump, uint16_t raw_phys_part)
{
    int phys_part;

    for (phys_part = 0; phys_part < UDF_PARTITIONS; phys_part++) {
        const uint8_t *part = ump->partitions[phys_part];
        if (!part)
            break;
        if (AV_RL16(part + PD_PART_NUM) == raw_phys_part)
            break;
    }
    return phys_part;
}

/* we dont try to be smart; we just record the parts (the last of each kind
 * wins; no volume descriptor sequence numbers are compared) */
#define UDF_UPDATE_DSCR(name, dscr) do { av_free(name); name = dscr; } while (0)

static int udf_process_vds_descriptor(UDFMount *ump, uint8_t *dscr, uint32_t len)
{
    int phys_part;

    switch (AV_RL16(dscr + TAG_ID)) {
    case TAGID_PRI_VOL:         /* primary partition */
        UDF_UPDATE_DSCR(ump->primary_vol, dscr);
        break;
    case TAGID_LOGVOL:          /* logical volume */
        UDF_UPDATE_DSCR(ump->logical_vol, dscr);
        ump->logical_vol_len = len;
        break;
    case TAGID_UNALLOC_SPACE:   /* unallocated space */
        UDF_UPDATE_DSCR(ump->unallocated, dscr);
        break;
    case TAGID_IMP_VOL:         /* implementation */
        /* XXX do we care about multiple impl. descr ? */
        UDF_UPDATE_DSCR(ump->implementation, dscr);
        break;
    case TAGID_PARTITION:       /* physical partition */
        /* not much use if its not allocated */
        if ((AV_RL16(dscr + PD_FLAGS) & UDF_PART_FLAG_ALLOCATED) == 0) {
            av_free(dscr);
            break;
        }
        phys_part = udf_find_raw_phys(ump, AV_RL16(dscr + PD_PART_NUM));
        if (phys_part == UDF_PARTITIONS) {
            /* upstream frees the descriptor twice here; it is rejected once */
            av_free(dscr);
            return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: more than %d "
                           "partition descriptors", UDF_PARTITIONS);
        }
        UDF_UPDATE_DSCR(ump->partitions[phys_part], dscr);
        break;
    case TAGID_VOL:             /* volume space extender; rare */
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': VDS extender ignored\n", ump->src->name);
        av_free(dscr);
        break;
    default:
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': unhandled VDS type %d\n", ump->src->name,
                AV_RL16(dscr + TAG_ID));
        av_free(dscr);
    }
    return 0;
}
#undef UDF_UPDATE_DSCR

/* the descriptors of one sequence extent, up to a blank block or a
 * terminating descriptor; an empty extent is an error */
static int udf_read_vds_extent(UDFMount *ump, uint32_t loc, uint32_t len)
{
    uint32_t sector_size = ump->sector_size, dscr_size, dlen;
    uint8_t *dscr;
    int error;

    /* loc is sectornr, len is in bytes */
    if (!len)
        return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: empty volume descriptor sequence");
    while (len) {
        if ((error = udf_read_phys_dscr(ump, loc, &dscr, &dlen)) < 0)
            return error;
        /* blank block is a terminator */
        if (!dscr)
            return 0;
        /* TERM descriptor is a terminator */
        if (AV_RL16(dscr + TAG_ID) == TAGID_TERM) {
            av_free(dscr);
            return 0;
        }
        /* process all others */
        dscr_size = udf_tagsize(dscr, sector_size);
        /* a descriptor of size 0 would be read again at the same place with
         * the same length left, without end */
        if (dscr_size == 0) {
            av_free(dscr);
            UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': volume descriptor at block %"PRIu32" has size 0; "
                    "the sequence cannot be read past it\n", ump->src->name, loc);
            return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: volume descriptor of size 0");
        }
        if ((error = udf_process_vds_descriptor(ump, dscr, dlen)) < 0)
            return error;
        len -= dscr_size;
        loc += dscr_size / sector_size;
    }
    return 0;
}

/*
 * read in VDS space provided by the anchors; if one descriptor read fails,
 * try the mirror sector.
 *
 * check if 2nd anchor is different from 1st; if so, go for 2nd. This avoids
 * the `compatibility features' of DirectCD that may confuse stuff completely.
 */
static int udf_read_vds_space(UDFMount *ump)
{
    const uint8_t *anchor = ump->anchors[0], *anchor2 = ump->anchors[1];
    uint32_t main_loc, main_len, reserve_loc, reserve_len;
    int error;

    if (anchor2) {
        if (memcmp(anchor + AVDP_MAIN_VDS_EX, anchor2 + AVDP_MAIN_VDS_EX, 8))
            anchor = anchor2;
        /* reserve is specified to be a literal copy of main */
    }
    main_len    = AV_RL32(anchor + AVDP_MAIN_VDS_EX);
    main_loc    = AV_RL32(anchor + AVDP_MAIN_VDS_EX + 4);
    reserve_len = AV_RL32(anchor + AVDP_RESERVE_VDS_EX);
    reserve_loc = AV_RL32(anchor + AVDP_RESERVE_VDS_EX + 4);

    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': main volume descriptor sequence at sector %"PRIu32", "
            "%"PRIu32" bytes\n", ump->src->name, main_loc, main_len);
    error = udf_read_vds_extent(ump, main_loc, main_len);
    if (error < 0) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': reading in reserve VDS extent (main sequence failed: %s)\n",
                ump->src->name, ump->why);
        error = udf_read_vds_extent(ump, reserve_loc, reserve_len);
    }
    return error;
}

/*
 * Read in the logical volume integrity sequence pointed to by our logical
 * volume descriptor: the last logical volume integrity descriptor of it. A
 * read error does not stop the walk ("hope for the best... maybe the next is
 * ok"), but an error left by its last step discards the descriptor. The link
 * to a next integrity extent is never taken upstream (it tests a pointer that
 * is always NULL at that point), so it is not followed here either.
 */
static void udf_retrieve_lvint(UDFMount *ump)
{
    uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    uint32_t len     = AV_RL32(ump->logical_vol + LVD_INTEGRITY_SEQ_LOC);
    uint32_t lbnum   = AV_RL32(ump->logical_vol + LVD_INTEGRITY_SEQ_LOC + 4);
    uint8_t *lvint = NULL, *dscr;
    uint32_t lvint_len = 0, dlen;
    int error = 0;

    while (len) {
        /* read in our integrity descriptor */
        error = udf_read_phys_dscr(ump, lbnum, &dscr, &dlen);
        if (!error) {
            int dscr_type;

            if (!dscr)
                break;      /* empty terminates */
            dscr_type = AV_RL16(dscr + TAG_ID);
            if (dscr_type == TAGID_TERM) {
                av_free(dscr);
                break;      /* clean terminator */
            }
            if (dscr_type != TAGID_LOGVOL_INTEGRITY) {
                /* fatal... corrupt disc */
                av_free(dscr);
                error = AVERROR(ENOENT);
                break;
            }
            av_free(lvint);
            lvint = dscr;
            lvint_len = dlen;
        } else {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': integrity sequence sector %"PRIu32" unreadable\n",
                    ump->src->name, lbnum);
        }
        /* proceed sequential */
        lbnum += 1;
        len   -= lb_size;
    }
    /* clean up the mess, esp. when there is an error */
    if (error && lvint)
        av_freep(&lvint);
    ump->logvol_integrity = lvint;
    ump->logvol_integrity_len = lvint ? lvint_len : 0;
}

/* strncmp over UDF_REGID_ID_SIZE bytes against a NUL-terminated name */
static int udf_regid_is(const uint8_t *id, const char *name)
{
    return !strncmp((const char *)id, name, UDF_REGID_ID_SIZE);
}

/* udf_process_vds: checks the volume descriptors and sets up the partition
 * map table */
static int udf_process_vds(UDFMount *ump)
{
    uint32_t n_pm, map_pos;
    int n_phys = 0, n_virt = 0, n_spar = 0, n_meta = 0;

    /* we need at least one primary and one logical volume descriptor */
    if (!ump->primary_vol || !ump->logical_vol)
        return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: primary or logical volume "
                       "descriptor missing");
    /* we need at least one partition descriptor */
    if (!ump->partitions[0])
        return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: no partition descriptor");

    /* check logical volume sector size verses device sector size */
    if (AV_RL32(ump->logical_vol + LVD_LB_SIZE) != ump->sector_size) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': format violation, lb_size != sector size\n", ump->src->name);
        return udf_err(ump, AVERROR_INVALIDDATA, "logical block size %"PRIu32" differs from the %"PRIu32"-byte "
                       "sector size", AV_RL32(ump->logical_vol + LVD_LB_SIZE), ump->sector_size);
    }
    /* check domain name */
    if (memcmp(ump->logical_vol + LVD_DOMAIN_ID + 1, "*OSTA UDF Compliant", 20)) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': disc not OSTA UDF Compliant, aborting\n", ump->src->name);
        return udf_err(ump, AVERROR_INVALIDDATA, "the volume is not OSTA UDF compliant");
    }

    /* retrieve logical volume integrity sequence */
    udf_retrieve_lvint(ump);
    /*
     * We need at least one logvol integrity descriptor recorded.  Note
     * that its OK to have an open logical volume integrity here. The VAT
     * will close/update the integrity.
     */
    if (!ump->logvol_integrity)
        return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: no logical volume integrity "
                       "descriptor");

    /* process derived structures */
    n_pm = AV_RL32(ump->logical_vol + LVD_N_PM);       /* num partmaps */
    ump->logvol_info = LVID_TABLES + 8 * n_pm;         /* &lvint->tables[2 * n_pm] */

    /*
     * check logvol mappings: effective virt->log partmap translation
     * check and recording of the mapping results. Saves expensive
     * strncmp() in tight places.
     */
    if (n_pm > UDF_PMAPS) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': too many mappings (%"PRIu32")\n", ump->src->name, n_pm);
        return udf_err(ump, AVERROR_INVALIDDATA, "%"PRIu32" partition maps (at most %d are supported)",
                       n_pm, UDF_PMAPS);
    }

    /* count types and set partition numbers */
    ump->data_part = ump->node_part = ump->fids_part = 0;
    map_pos = LVD_MAPS;
    for (uint32_t log_part = 0; log_part < n_pm; log_part++) {
        const uint8_t *pmap_pos;
        int pmap_stype, pmap_size, pmap_type, phys_part, raw_phys_part;

        if (map_pos + 64 > ump->logical_vol_len)
            return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: partition maps run "
                           "past the descriptor");
        pmap_pos   = ump->logical_vol + map_pos;
        pmap_stype = pmap_pos[0];
        pmap_size  = pmap_pos[1];
        switch (pmap_stype) {
        case 1:     /* physical mapping */
            raw_phys_part = AV_RL16(pmap_pos + 4);
            pmap_type = UDF_VTOP_TYPE_PHYS;
            n_phys++;
            ump->data_part = log_part;
            ump->node_part = log_part;
            ump->fids_part = log_part;
            break;
        case 2:     /* virtual/sparable/meta mapping */
            raw_phys_part = AV_RL16(pmap_pos + PM2_PART_NUM);
            pmap_type = UDF_VTOP_TYPE_UNKNOWN;
            if (udf_regid_is(pmap_pos + PM2_PART_ID, "*UDF Virtual Partition")) {
                pmap_type = UDF_VTOP_TYPE_VIRT;
                n_virt++;
                ump->node_part = log_part;
                break;
            }
            if (udf_regid_is(pmap_pos + PM2_PART_ID, "*UDF Sparable Partition")) {
                pmap_type = UDF_VTOP_TYPE_SPARABLE;
                n_spar++;
                ump->data_part = log_part;
                ump->node_part = log_part;
                ump->fids_part = log_part;
                break;
            }
            if (udf_regid_is(pmap_pos + PM2_PART_ID, "*UDF Metadata Partition")) {
                pmap_type = UDF_VTOP_TYPE_META;
                n_meta++;
                ump->node_part = log_part;
                ump->fids_part = log_part;
                break;
            }
            break;
        default:
            return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: unknown partition map type");
        }

        /*
         * BUGALERT: some rogue implementations use random physical
         * partition numbers to break other implementations so lookup
         * the number.
         */
        phys_part = udf_find_raw_phys(ump, raw_phys_part);
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': partition map %"PRIu32" -> partition %d (slot %d), type %d\n",
                ump->src->name, log_part, raw_phys_part, phys_part, pmap_type);
        if (phys_part == UDF_PARTITIONS)
            return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: partition map names no "
                           "partition");
        if (pmap_type == UDF_VTOP_TYPE_UNKNOWN)
            return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: unknown partition map");

        ump->vtop   [log_part] = phys_part;
        ump->vtop_tp[log_part] = pmap_type;

        map_pos += pmap_size;
    }
    /* not winning the beauty contest */
    ump->vtop_tp[UDF_VTOP_RAWPART] = UDF_VTOP_TYPE_RAW;

    /* test some basic UDF assertions/requirements */
    if ((n_virt > 1) || (n_spar > 1) || (n_meta > 1))
        return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: more than one virtual, "
                       "sparable or metadata map");
    if (n_virt) {
        if ((n_phys == 0) || n_spar || n_meta)
            return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: virtual map without a "
                           "physical map, or with sparable / metadata maps");
    }
    if (n_spar + n_phys == 0)
        return udf_err(ump, AVERROR_INVALIDDATA, "bad volume descriptor sequence: no physical or sparable map");

    /* print results */
    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': data partition %d, node partition %d, fids partition %d\n",
            ump->src->name, ump->data_part, ump->node_part, ump->fids_part);
    /* signal its OK for now */
    return 0;
}

/*
 * Update logical volume name in all structures that keep a record of it. We
 * use memmove since each of them might be specified as a source.
 *
 * Note that it doesn't update the VAT structure!
 */
static void udf_update_logvolname(UDFMount *ump, const uint8_t *logvol_id)
{
    uint8_t id[128];

    memcpy(id, logvol_id, sizeof(id));
    memmove(ump->logical_vol + LVD_LOGVOL_ID, id, 128);
    if (ump->fileset_desc)
        memmove(ump->fileset_desc + FSD_LOGVOL_ID, id, 128);
    if (ump->implementation)
        memmove(ump->implementation + IVD_LV_INFO_LOGVOL_ID, id, 128);
}

static int udf_impl_extattr_check(const uint8_t *implext)
{
    if (!strncmp((const char *)implext + IMPLEXT_IMP_ID + 1, "*UDF", 4)) {
        /* checksum valid? */
        if (AV_RL16(implext + IMPLEXT_DATA) != udf_ea_cksum(implext))
            return AVERROR_INVALIDDATA;
    }
    return 0;
}

/*
 * udf_extattr_search_intern: offset and length of extended attribute sattr
 * (named sattrname for implementation / application use) in the extended
 * attribute area of node. The area lies inside the node's block.
 */
static int udf_extattr_search_intern(UDFMount *ump, const UDFNode *node, uint32_t sattr,
                                     const char *sattrname, uint32_t *offsetp, uint32_t *lengthp)
{
    uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    uint32_t sector_size = ump->sector_size, offset, a_l, base, ul_ea, l_ad, avail;
    const uint8_t *eahdr;
    int32_t l_ea;

    /* get information from fe/efe */
    udf_node_ad_area(node, &base, &ul_ea, &l_ad);
    l_ea  = ul_ea;
    eahdr = udf_node_dscr(node) + base;
    avail = lb_size - base;

    /* something recorded here? */
    if (l_ea == 0)
        return AVERROR(ENOENT);

    /* check extended attribute tag; what to do if it fails? */
    if (udf_check_tag(eahdr) < 0)
        return AVERROR_INVALIDDATA;
    if (AV_RL16(eahdr + TAG_ID) != TAGID_EXTATTR_HDR)
        return AVERROR_INVALIDDATA;
    if (udf_check_tag_payload(eahdr, avail, EAHDR_SIZE))
        return AVERROR_INVALIDDATA;

    /* looking for Ecma-167 attributes? */
    offset = EAHDR_SIZE;

    /* looking for either implementation use or application use */
    if (sattr == 2048) {                /* [4/48.10.8] */
        offset = AV_RL32(eahdr + EAHDR_IMPL_ATTR_LOC);
        if (offset == UINT32_MAX)
            return AVERROR(ENOENT);
    }
    if (sattr == 65536) {               /* [4/48.10.9] */
        offset = AV_RL32(eahdr + EAHDR_APPL_ATTR_LOC);
        if (offset == UINT32_MAX)
            return AVERROR(ENOENT);
    }

    /* paranoia check offset and l_ea */
    if ((uint32_t)l_ea + offset >= sector_size - EXTATTR_ENTRY_SIZE)
        return AVERROR_INVALIDDATA;

    /* find our extended attribute  */
    l_ea -= offset;
    /* l_ea >= sizeof(struct extattr_entry) compares as size_t */
    while ((uint64_t)(int64_t)l_ea >= EXTATTR_ENTRY_SIZE) {
        const uint8_t *pos;
        uint32_t type;

        if (offset + EXTATTR_ENTRY_SIZE > avail)
            return AVERROR_INVALIDDATA;
        pos = eahdr + offset;
        /* get complete attribute length and check for roque values */
        type = AV_RL32(pos);
        a_l  = AV_RL32(pos + 8);
        if ((a_l == 0) || (a_l > (uint32_t)l_ea))
            return AVERROR_INVALIDDATA;

        if (type == sattr) {
            /* we might have found it! */
            if (type < 2048) {          /* Ecma-167 attribute */
                *offsetp = offset;
                *lengthp = a_l;
                return 0;               /* success */
            }
            /*
             * Implementation use and application use extended attributes
             * have a name to identify. They share the same structure only
             * UDF implementation use extended attributes have a checksum
             * we need to check
             */
            if (offset + IMPLEXT_IMP_ID + 1 < avail) {
                const char *id = (const char *)pos + IMPLEXT_IMP_ID + 1;
                size_t room = avail - (offset + IMPLEXT_IMP_ID + 1), n = 0;

                while (n < room && id[n])
                    n++;
                if (n == room)          /* no NUL in the block */
                    return AVERROR_INVALIDDATA;
                if (!strcmp(id, sattrname)) {
                    /* we have found our appl/implementation attribute */
                    *offsetp = offset;
                    *lengthp = a_l;
                    return 0;           /* success */
                }
            } else {
                return AVERROR_INVALIDDATA;
            }
        }
        /* next attribute */
        l_ea   -= a_l;
        offset += a_l;
    }
    /* not found */
    return AVERROR(ENOENT);
}

static void udf_set_lvinfo(UDFMount *ump, uint32_t at, const uint8_t *src, uint32_t len)
{
    if (ump->logvol_integrity && ump->logvol_info + at + len <= ump->logvol_integrity_len)
        memcpy(ump->logvol_integrity + ump->logvol_info + at, src, len);
}

/* udf_update_lvid_from_vat_extattr: the "*UDF VAT LVExtension" attribute of a
 * UDF 1.50 VAT updates the file counts and the logical volume identifier, if
 * it belongs to this VAT */
static void udf_update_lvid_from_vat_extattr(UDFMount *ump, const UDFNode *vat_node)
{
    uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    uint32_t offset, a_l, base, l_ea, l_ad, iu_l;
    const uint8_t *implext, *lvextpos;

    if (udf_extattr_search_intern(ump, vat_node, 2048, "*UDF VAT LVExtension", &offset, &a_l) < 0)
        return;
    udf_node_ad_area(vat_node, &base, &l_ea, &l_ad);
    if ((uint64_t)base + offset + IMPLEXT_DATA + 2 > lb_size)
        return;
    implext = udf_node_dscr(vat_node) + base + offset;
    if (udf_impl_extattr_check(implext) < 0)
        return;

    /* paranoia */
    iu_l = AV_RL32(implext + IMPLEXT_IU_L);
    if ((uint64_t)a_l != IMPLEXT_DATA + (uint64_t)iu_l + VATLVEXT_SIZE) {
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': VAT LVExtension size doesn't compute\n", ump->src->name);
        return;
    }
    if ((uint64_t)base + offset + IMPLEXT_DATA + iu_l + VATLVEXT_SIZE > lb_size)
        return;
    /* we have found our "VAT LVExtension attribute */
    lvextpos = implext + IMPLEXT_DATA + iu_l;

    /* check if it was updated the last time */
    if (AV_RL64(lvextpos) == udf_node_unique_id(vat_node)) {
        udf_set_lvinfo(ump, LVINFO_NUM_FILES, lvextpos + 8, 4);
        udf_set_lvinfo(ump, LVINFO_NUM_DIRECTORIES, lvextpos + 12, 4);
        udf_update_logvolname(ump, lvextpos + 16);
    } else {
        /* upstream replaces the attribute by a free space EA in the node's
         * memory copy; nothing reads it again */
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': VAT LVExtension out of date\n", ump->src->name);
    }
}

/* udf_read of exactly len bytes */
static int udf_read_exact(UDFMount *ump, const UDFNode *node, uint64_t pos, uint8_t *dst, size_t len)
{
    size_t got;
    int error = udf_read(ump, node, pos, dst, len, &got);

    if (error < 0)
        return error;
    if (got != len)
        return udf_err(ump, AVERROR_INVALIDDATA, "a read of %zu bytes at %"PRIu64" runs past the end of a "
                       "%"PRIu64"-byte file", len, pos, udf_node_inf_len(node));
    return 0;
}

/* udf_check_for_vat: accepts vat_node as the VAT (UDF 1.50 or 2.00 format)
 * and reads the whole table; a VAT closes the logical volume */
static int udf_check_for_vat(UDFMount *ump, const UDFNode *vat_node)
{
    uint8_t raw_vat[UDF_VAT_SIZE], *vat_table;
    uint32_t vat_length, vat_offset, vat_entries, vat_table_alloc_len;
    int filetype, error;

    /* vat_length is really 64 bits though impossible */
    vat_length = (uint32_t)udf_node_inf_len(vat_node);

    /* Check icb filetype! it has to be 0 or UDF_ICB_FILETYPE_VAT */
    filetype = vat_node->file_type;
    if ((filetype != 0) && (filetype != UDF_ICB_FILETYPE_VAT))
        return udf_err(ump, AVERROR(ENOENT), "the VAT could not be read");

    vat_table_alloc_len = ((vat_length + UDF_VAT_CHUNKSIZE - 1) / UDF_VAT_CHUNKSIZE) * UDF_VAT_CHUNKSIZE;
    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': possible VAT of %"PRIu32" bytes (table %"PRIu32" bytes)\n",
            ump->src->name, vat_length, vat_table_alloc_len);

    /*
     * check contents of the file if its the old 1.50 VAT table format.
     * Its notoriously broken and allthough some implementations support an
     * extension as defined in the UDF 1.50 errata document, its doubtful
     * to be useable since a lot of implementations don't maintain it.
     */
    if (filetype == 0) {
        /* definition */
        vat_offset  = 0;
        vat_entries = (vat_length - 36) / 4;

        /* read in tail of virtual allocation table file */
        if ((error = udf_read_exact(ump, vat_node, (uint64_t)(uint32_t)(vat_entries * 4), raw_vat,
                                    UDF_OLDVAT_TAIL_SIZE)) < 0)
            return error;
        /* check 1.50 VAT */
        if (strncmp((const char *)raw_vat + 1, "*UDF Virtual Alloc Tbl", 22)) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': VAT format 1.50 rejected\n", ump->src->name);
            return udf_err(ump, AVERROR(ENOENT), "the VAT could not be read");
        }
        /*
         * update LVID from "*UDF VAT LVExtension" extended attribute
         * if present.
         */
        udf_update_lvid_from_vat_extattr(ump, vat_node);
    } else {
        /* read in head of virtual allocation table file */
        if ((error = udf_read_exact(ump, vat_node, 0, raw_vat, UDF_VAT_SIZE)) < 0)
            return error;
        /* definition */
        vat_offset  = AV_RL16(raw_vat);     /* header_len */
        vat_entries = (vat_length - vat_offset) / 4;

        /* num_files, num_directories, min_udf_readver, min_udf_writever,
         * max_udf_writever */
        udf_set_lvinfo(ump, LVINFO_NUM_FILES, raw_vat + 136, 14);
        udf_update_logvolname(ump, raw_vat + 4);
    }

    /* read in complete VAT file */
    if (!(vat_table = av_malloc(FFMAX(vat_length, 1))))
        return AVERROR(ENOMEM);
    if ((error = udf_read_exact(ump, vat_node, 0, vat_table, vat_length)) < 0) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': read in of complete VAT file failed (%s)\n",
                ump->src->name, ump->why);
        av_free(vat_table);
        return error;
    }

    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': VAT format accepted, marking it closed\n", ump->src->name);
    AV_WL64(ump->logvol_integrity + LVID_NEXT_UNIQUE_ID, udf_node_unique_id(vat_node));
    AV_WL32(ump->logvol_integrity + LVID_INTEGRITY_TYPE, UDF_INTEGRITY_CLOSED);
    memcpy(ump->logvol_integrity + LVID_TIME, udf_node_mtime(vat_node), 12);

    /* if we're updating, free old allocated space */
    av_free(ump->vat_table);
    ump->vat_table     = vat_table;
    ump->vat_table_len = vat_length;
    ump->vat_offset    = vat_offset;
    ump->vat_entries   = vat_entries;
    return 0;
}

/*
 * udf_search_vat: looks for the VAT backwards from the end of the track in
 * windows of 64 sectors; within a window the last accepted candidate wins.
 *
 * The window arithmetic is unsigned: once a window runs below sector 0 it
 * wraps, and the upstream loop then goes on for 2^26 more rounds or forever
 * (depending on the track length modulo 64), checking only sectors already
 * checked. After the wrapped window every sector from 0 to the end has been
 * tried, so the search stops there with the same result.
 */
static int udf_search_vat(UDFMount *ump)
{
    uint32_t early_vat_loc, late_vat_loc, vat_loc;
    UDFNode *vat_node, *accepted_vat_node = NULL;
    struct long_ad icb_loc;
    int error;

    /*
     * Start reading forward in blocks from the first possible vat
     * location. If not found in this block, start again a bit before
     * until we get a hit.
     */
    late_vat_loc  = ump->last_possible_vat_location;
    early_vat_loc = FFMAX(late_vat_loc - 64, ump->first_possible_vat_location);

    do {
        int wrapped = early_vat_loc > late_vat_loc;

        vat_loc = early_vat_loc;
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': checking for a VAT in sectors %"PRIu32" to %"PRIu32"\n",
                ump->src->name, early_vat_loc, late_vat_loc);
        do {
            /* a location past the end of the image cannot be read; upstream
             * tries anyway and fails, so it is skipped here */
            if (vat_loc < ump->psize) {
                memset(&icb_loc, 0, sizeof(icb_loc));
                icb_loc.loc.part_num = UDF_VTOP_RAWPART;
                icb_loc.loc.lb_num   = vat_loc;
                error = udf_get_node(ump, &icb_loc, &vat_node);
                if (!error)
                    error = udf_check_for_vat(ump, vat_node);
                if (error == AVERROR(ENOMEM))
                    return error;
                if (!error) {
                    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': VAT candidate accepted at %"PRIu32"\n",
                            ump->src->name, vat_loc);
                    accepted_vat_node = vat_node;
                }
            }
            vat_loc++;  /* walk forward */
        } while (vat_loc <= late_vat_loc);
        if (accepted_vat_node)
            break;
        if (wrapped) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': VAT search window wrapped below sector 0; every sector "
                    "has been tried\n", ump->src->name);
            break;
        }
        early_vat_loc = FFMAX(early_vat_loc - 64, ump->first_possible_vat_location);
        late_vat_loc  = FFMIN(early_vat_loc + 64, ump->last_possible_vat_location);
    } while (late_vat_loc > ump->first_possible_vat_location);

    /* keep our last accepted VAT node around */
    if (accepted_vat_node) {
        ump->vat_node = accepted_vat_node;
        return 0;
    }
    return udf_err(ump, AVERROR(ENOENT), "no VAT found");
}

/* udf_read_sparables: the first readable sparing table of the map at pos in
 * the logical volume descriptor */
static int udf_read_sparables(UDFMount *ump, uint32_t pos)
{
    const uint8_t *pms = ump->logical_vol + pos;
    int n_st;

    ump->sparable_packet_size = AV_RL16(pms + PMS_PACKET_LEN);
    if (ump->sparable_packet_size < ump->packet_size)   /* upstream: KASSERT */
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': sparable packet size %"PRIu32" below the packet size %"PRIu32"\n",
                ump->src->name, ump->sparable_packet_size, ump->packet_size);

    n_st = pms[PMS_N_ST];
    for (int spar = 0; spar < n_st; spar++) {
        uint32_t lb_num, len;
        uint8_t *dscr;

        if (pos + PMS_ST_LOC + 4 * spar + 4 > ump->logical_vol_len)
            break;
        lb_num = AV_RL32(pms + PMS_ST_LOC + 4 * spar);
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': checking for sparing table %"PRIu32"\n", ump->src->name, lb_num);
        if (udf_read_phys_dscr(ump, lb_num, &dscr, &len) == 0 && dscr) {
            if (AV_RL16(dscr + TAG_ID) == TAGID_SPARING_TABLE) {
                av_free(ump->sparing_table);
                ump->sparing_table = dscr;
                ump->sparing_table_len = len;
                UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': sparing table accepted (%d entries)\n",
                        ump->src->name, AV_RL16(dscr + SPT_RT_L));
                break;  /* we're done */
            }
            av_free(dscr);
        }
    }
    if (ump->sparing_table)
        return 0;
    return udf_err(ump, AVERROR(ENOENT), "the sparing table could not be read");
}

/* udf_read_metadata_nodes: the metadata file, its mirror and its bitmap.
 * Read-only: the mirror stands in when the main file cannot be read. */
static int udf_read_metadata_nodes(UDFMount *ump, uint32_t pos)
{
    const uint8_t *pmm = ump->logical_vol + pos;
    struct long_ad icb_loc;
    uint32_t lbn;

    /* extract our allocation parameters set up on format */
    ump->metadata_alloc_unit_size     = AV_RL32(pmm + PMM_ALLOC_UNIT_SIZE);
    ump->metadata_alignment_unit_size = AV_RL16(pmm + PMM_ALIGNMENT_UNIT_SIZE);
    ump->metadata_flags               = pmm[PMM_FLAGS];

    memset(&icb_loc, 0, sizeof(icb_loc));
    icb_loc.loc.part_num = udf_find_raw_phys(ump, AV_RL16(pmm + PM2_PART_NUM));

    icb_loc.loc.lb_num = AV_RL32(pmm + PMM_META_FILE_LBN);
    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': metadata file at block %"PRIu32"\n", ump->src->name, icb_loc.loc.lb_num);
    if (udf_get_node(ump, &icb_loc, &ump->metadata_node) == AVERROR(ENOMEM))
        return AVERROR(ENOMEM);

    lbn = AV_RL32(pmm + PMM_META_MIRROR_FILE_LBN);
    if (lbn != UINT32_MAX) {
        icb_loc.loc.lb_num = lbn;
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': metadata copy file at block %"PRIu32"\n", ump->src->name, lbn);
        if (udf_get_node(ump, &icb_loc, &ump->metadatamirror_node) == AVERROR(ENOMEM))
            return AVERROR(ENOMEM);
    }
    lbn = AV_RL32(pmm + PMM_META_BITMAP_FILE_LBN);
    if (lbn != UINT32_MAX) {
        icb_loc.loc.lb_num = lbn;
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': metadata bitmap file at block %"PRIu32"\n", ump->src->name, lbn);
        if (udf_get_node(ump, &icb_loc, &ump->metadatabitmap_node) == AVERROR(ENOMEM))
            return AVERROR(ENOMEM);
    }

    /* if we're mounting read-only we relax the requirements */
    if (ump->metadata_node)
        return 0;
    if (ump->metadatamirror_node) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': Metadata file not readable, substituting Metadata copy file\n",
                ump->src->name);
        ump->metadata_node = ump->metadatamirror_node;
        ump->metadatamirror_node = NULL;
        return 0;
    }
    return udf_err(ump, AVERROR_INVALIDDATA, "the metadata file could not be read");
}

/* udf_read_vds_tables: VAT, sparing table or metadata files for each
 * partition map */
static int udf_read_vds_tables(UDFMount *ump)
{
    uint32_t n_pm = AV_RL32(ump->logical_vol + LVD_N_PM), pos = LVD_MAPS;
    int error;

    /* Iterate (again) over the part mappings for locations */
    for (uint32_t log_part = 0; log_part < n_pm; log_part++) {
        switch (ump->vtop_tp[log_part]) {
        case UDF_VTOP_TYPE_PHYS:
            /* nothing */
            break;
        case UDF_VTOP_TYPE_VIRT:
            /* search and load VAT */
            if ((error = udf_search_vat(ump)) < 0)
                return error == AVERROR(ENOMEM) ? error
                                                : udf_err(ump, AVERROR_INVALIDDATA, "the VAT could not be read");
            break;
        case UDF_VTOP_TYPE_SPARABLE:
            /* load one of the sparable tables */
            if ((error = udf_read_sparables(ump, pos)) < 0)
                return error;
            break;
        case UDF_VTOP_TYPE_META:
            /* load the associated file descriptors */
            if ((error = udf_read_metadata_nodes(ump, pos)) < 0)
                return error;
            break;
        default:
            break;
        }
        pos += ump->logical_vol[pos + 1];
    }
    return 0;
}

/* udf_read_rootdirs: the file set descriptor sequence (the last FSD wins),
 * then the root directory; the system stream directory as
 * UDFCompat.skip_stream_directory says */
static int udf_read_rootdirs(UDFMount *ump)
{
    struct long_ad fsd_loc, rootdir_icb, streamdir_icb, next;
    struct lb_addr *visited = NULL;
    uint32_t lb_num, dummy, fsd_len, len;
    int nb_visited = 0;
    UDFNode *node;
    uint8_t *dscr;

    /* get fileset descriptor sequence */
    fsd_loc = udf_long_ad(ump->logical_vol + LVD_FSD_LOC);
    fsd_len = fsd_loc.len;

    while (fsd_len) {
        int dscr_type, known = 0;

        /* an FSD extent chain that comes back to a block already read can
         * never end */
        for (int i = 0; i < nb_visited; i++)
            if (visited[i].lb_num == fsd_loc.loc.lb_num && visited[i].part_num == fsd_loc.loc.part_num)
                known = 1;
        if (known) {
            UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': the file set descriptor chain returns to block "
                    "%"PRIu32"; stopped\n", ump->src->name, fsd_loc.loc.lb_num);
            break;
        }
        if (!(nb_visited & (nb_visited - 1))) {
            struct lb_addr *n = av_realloc_array(visited, nb_visited ? 2 * nb_visited : 4, sizeof(*visited));
            if (!n) {
                av_free(visited);
                return AVERROR(ENOMEM);
            }
            visited = n;
        }
        visited[nb_visited++] = fsd_loc.loc;

        /* translate fsd_loc to lb_num */
        if (udf_translate_vtop(ump, &fsd_loc, &lb_num, &dummy) < 0)
            break;
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': reading FSD at sector %"PRIu32"\n", ump->src->name, lb_num);
        /* end markers */
        if (udf_read_phys_dscr(ump, lb_num, &dscr, &len) < 0 || !dscr)
            break;
        /* analyse */
        dscr_type = AV_RL16(dscr + TAG_ID);
        if (dscr_type == TAGID_TERM) {
            av_free(dscr);
            break;
        }
        if (dscr_type != TAGID_FSD) {
            av_free(dscr);
            av_free(visited);
            return udf_err(ump, AVERROR_INVALIDDATA, "the file set descriptor or the root directory could not "
                           "be read");
        }
        /*
         * TODO check for multiple fileset descriptors; its only
         * picking the last now. Also check for FSD
         * correctness/interpretability
         */
        /* update */
        av_free(ump->fileset_desc);
        ump->fileset_desc = dscr;

        /* continue to the next fsd */
        fsd_len -= ump->sector_size;
        fsd_loc.loc.lb_num = fsd_loc.loc.lb_num + 1;

        /* follow up to fsd->next_ex (long_ad) if its not null */
        next = udf_long_ad(ump->fileset_desc + FSD_NEXT_EX);
        if (next.len) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': follow up FSD extent\n", ump->src->name);
            fsd_loc = next;
            fsd_len = next.len;
        }
    }
    av_free(visited);

    /* there has to be one */
    if (!ump->fileset_desc)
        return udf_err(ump, AVERROR_INVALIDDATA, "the file set descriptor or the root directory could not be read");

    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': FSD read in fine\n", ump->src->name);
    udf_update_logvolname(ump, ump->logical_vol + LVD_LOGVOL_ID);

    /*
     * Now the FSD is known, read in the rootdirectory and if one exists,
     * the system stream dir. Some files in the system streamdir are not
     * wanted in this implementation since they are not maintained.
     */
    rootdir_icb   = udf_long_ad(ump->fileset_desc + FSD_ROOTDIR_ICB);
    streamdir_icb = udf_long_ad(ump->fileset_desc + FSD_STREAMDIR_ICB);

    /* try to read in the rootdir */
    if (udf_get_node(ump, &rootdir_icb, &node) < 0)
        return udf_err(ump, AVERROR_INVALIDDATA, "the file set descriptor or the root directory could not be read");

    /*
     * Try the system stream directory; not very likely in the ones we
     * test, but for completeness.
     */
    if (streamdir_icb.len) {
        if (ump->compat->skip_stream_directory) {
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': system stream directory present; not loaded\n",
                    ump->src->name);
        } else if (udf_get_node(ump, &streamdir_icb, &node) < 0) {
            UDF_LOG(ump, AV_LOG_INFO, "UDF on '%s': streamdir defined but error in streamdir reading\n",
                    ump->src->name);
        } else {
            UDF_LOG(ump, AV_LOG_INFO, "UDF on '%s': streamdir defined but ignored\n", ump->src->name);
        }
    }
    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': Rootdir(s) read in fine\n", ump->src->name);
    return 0;
}

/* ---- udf_vfsops.c ---- */

/* udf_mountfs, read-only: anchors, volume descriptor sequence, its checks,
 * the support tables (VAT, sparing table, metadata files), the integrity
 * state and the root directories */
static int udf_mountfs(UDFMount *ump)
{
    uint32_t sector_size = ump->sector_size, integrity;
    int bshift = 1, error;

    /* sector size of the device: a power of two below 8192 */
    while ((1u << bshift) < sector_size)
        bshift++;
    if ((1u << bshift) != sector_size || sector_size >= 8192) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': sector size %"PRIu32" is not supported\n",
                ump->src->name, sector_size);
        return AVERROR(EINVAL);
    }

    /* read all anchors to get volume descriptor sequence */
    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': reading anchors\n", ump->src->name);
    if (udf_read_anchors(ump) == 0) {
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': no anchor volume descriptor pointer\n", ump->src->name);
        snprintf(ump->why, sizeof(ump->why), "no UDF anchor volume descriptor pointer found");
        return AVERROR_INVALIDDATA;
    }
    UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': %d anchor(s) read\n", ump->src->name, ump->num_anchors);

    /* read in volume descriptor space */
    if ((error = udf_read_vds_space(ump)) < 0) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': error reading volume space (%s)\n", ump->src->name, ump->why);
        snprintf(ump->why, sizeof(ump->why), "the volume descriptor sequence could not be read (main and "
                 "reserve copy)");
        return error == AVERROR(ENOMEM) ? error : AVERROR_INVALIDDATA;
    }

    /* check consistency and completeness */
    if ((error = udf_process_vds(ump)) < 0) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': disc not properly formatted (bad VDS): %s\n",
                ump->src->name, ump->why);
        return error;
    }

    /* read vds support tables like VAT, sparable etc. */
    if ((error = udf_read_vds_tables(ump)) < 0) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': error in format or damaged disc (VDS tables failing): %s\n",
                ump->src->name, ump->why);
        return error;
    }

    /* check if volume integrity is closed otherwise its dirty */
    integrity = AV_RL32(ump->logvol_integrity + LVID_INTEGRITY_TYPE);
    if (integrity != UDF_INTEGRITY_CLOSED) {
        if (ump->compat->accept_unclean_volume) {
            UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': file system is not clean (logical volume integrity "
                    "%"PRIu32"); reading it anyway\n", ump->src->name, integrity);
        } else {
            UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': file system is not clean; please fsck(8)\n", ump->src->name);
            snprintf(ump->why, sizeof(ump->why), "the logical volume is not closed (file system not clean)");
            return AVERROR(EPERM);
        }
    }

    /* read root directory */
    if ((error = udf_read_rootdirs(ump)) < 0) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': disc not properly formatted or damaged disc (rootdirs "
                "failing): %s\n", ump->src->name, ump->why);
        return error == AVERROR(ENOMEM) ? error : AVERROR_INVALIDDATA;
    }
    return 0;
}

/* ---- the common interface (what the kernel does through the VFS) ---- */

static void udf_free_mount(UDFMount *ump)
{
    for (int i = 0; i < 4; i++)
        av_free(ump->anchors[i]);
    av_free(ump->primary_vol);
    av_free(ump->logical_vol);
    av_free(ump->unallocated);
    av_free(ump->implementation);
    for (int i = 0; i < UDF_PARTITIONS; i++)
        av_free(ump->partitions[i]);
    av_free(ump->logvol_integrity);
    av_free(ump->sparing_table);
    av_free(ump->vat_table);
    av_free(ump->fileset_desc);
    for (int h = 0; h < UDF_NODE_HASH; h++)
        while (ump->nodes[h]) {
            UDFNode *n = ump->nodes[h];
            ump->nodes[h] = n->hnext;
            udf_dispose_node(n);
        }
    av_free(ump);
}

/*
 * Volume facts: the label (logical volume identifier, its length byte read as
 * signed, at most 258 bytes of UTF-8, cut to the room of fs->label), the UDF
 * revision from an implementation use volume descriptor that holds
 * "*UDF LV Info", and the recording time of the primary volume descriptor.
 */
static void udf_volume_info(UDFMount *ump, DiscIOFS *fs)
{
    const uint8_t *lvd = ump->logical_vol;
    const uint8_t *iu = ump->implementation;
    char label[0x102 + 1];
    int len, keep;

    len = udf_to_unix_name(ump, label, 0x102, lvd + LVD_LOGVOL_ID, 128, (int8_t)lvd[LVD_LOGVOL_ID + 127],
                           lvd + LVD_DESC_CHARSET, ump->compat->name_rules);
    keep = len;
    if (keep > DISCIO_LABEL_SIZE - 1) {
        /* cut at a character boundary */
        keep = DISCIO_LABEL_SIZE - 1;
        while (keep > 0 && ((uint8_t)label[keep] & 0xc0) == 0x80)
            keep--;
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': the volume label (%d bytes) is cut to %d bytes\n",
                ump->src->name, len, keep);
    }
    memcpy(fs->label, label, keep);
    fs->label[keep] = 0;

    fs->udf_revision = 0;
    if (iu && !memcmp(iu + IVD_IMPL_ID + 1, "*UDF LV Info", 12)) {
        int zero = 1;
        for (int i = IVD_IMPL_ID + 13; i < IVD_IMPL_ID + 24; i++)
            if (iu[i])
                zero = 0;
        if (zero)
            fs->udf_revision = AV_RL16(iu + IVD_IMPL_ID + 24);
    }
    memset(fs->udf_recording_time, 0, sizeof(fs->udf_recording_time));
    if (ump->primary_vol)
        memcpy(fs->udf_recording_time, ump->primary_vol + PVD_REG_TIME, 12);
}

static int udf_root_node(UDFMount *ump, UDFNode **out)
{
    struct long_ad icb = udf_long_ad(ump->fileset_desc + FSD_ROOTDIR_ICB);
    return udf_get_node(ump, &icb, out);
}

/* The sub-directory name of dir (AVERROR(ENOENT) when it does not exist or is
 * not a directory). */
static int udf_subdir(UDFMount *ump, UDFNode *dir, const char *name, int namelen, UDFNode **out)
{
    struct long_ad icb;
    int found, error;

    if ((error = udf_lookup_name_in_dir(ump, dir, name, namelen, &icb, &found)) < 0)
        return error;
    if (!found)
        return AVERROR(ENOENT);
    if ((error = udf_get_node(ump, &icb, out)) < 0)
        return error;
    return (*out)->v_type == VDIR ? 0 : AVERROR(ENOENT);
}

static int udf_walk_subdir(void *ctx, void **dirp, const char *name, int name_len)
{
    UDFNode *sub;
    int error = udf_subdir(ctx, *dirp, name, name_len, &sub);

    if (error < 0)
        return error;
    *dirp = sub;
    return 0;
}

/* Walks all but the last component of path from the root through
 * sub-directories. */
static int udf_walk(UDFMount *ump, const char *path, UDFNode **parent, const char **last)
{
    UDFNode *root;
    void *dir;
    int error;

    if ((error = udf_root_node(ump, &root)) < 0)
        return error;
    dir = root;
    if ((error = ff_discio_walk_path(path, &dir, udf_walk_subdir, ump, last)) < 0)
        return error;
    *parent = dir;
    return 0;
}

static int udf_open_dir(UDFMount *ump, const char *path, UDFNode **out)
{
    UDFNode *parent;
    const char *last;
    int error;

    if (!strcmp(path, "/"))
        return udf_root_node(ump, out);   /* the root's file entry must load */
    if ((error = udf_walk(ump, path, &parent, &last)) < 0)
        return error;
    return udf_subdir(ump, parent, last, strlen(last), out);
}

/*
 * Where a file block lies: data recorded at *sector, not recorded (*sector =
 * -1), or recorded inside the file entry (return 1); *count blocks from there
 * are contiguous. The allocation descriptor that holds file block block, and
 * how many blocks of it are left from there (the block offset rounded up).
 * Recorded blocks are translated to a sector, and the count is cut where the
 * translation stops being contiguous (partition end, sparing packet, virtual
 * allocation table entry). Descriptors that point to the next allocation
 * extent are stepped over without counting.
 */
static int udf_block_extent(UDFMount *ump, const UDFNode *node, uint32_t block, int64_t *sector,
                            uint32_t *count)
{
    uint32_t bs = AV_RL32(ump->logical_vol + LVD_LB_SIZE), off, into, rem, n, lb, run;
    uint64_t target = (uint64_t)block * bs, start = 0, end;
    struct long_ad ad, next, t;
    uint32_t slot = 0;
    const char *why;
    int eof, error;

    if ((udf_node_icbflags(node) & UDF_ICB_TAG_FLAGS_ALLOC_MASK) == UDF_ICB_INTERN_ALLOC)
        return 1;

#define UDF_AD_AT(slot_, ad_) do {                                                              \
        if ((error = udf_get_adslot(ump, node, slot_, &(ad_), &eof, &why)) < 0)                 \
            return udf_err(ump, error, "the file entry at block %"PRIu32" of partition "        \
                           "reference %u could not be read: %s", node->loc.loc.lb_num,          \
                           node->loc.loc.part_num, why);                                        \
        if (eof)                                                                                \
            return udf_err(ump, AVERROR_INVALIDDATA,                                            \
                           "a file block lies beyond the file's allocation descriptors");       \
    } while (0)

    for (;;) {
        UDF_AD_AT(slot, ad);
        if (UDF_EXT_FLAGS(ad.len) == UDF_EXT_REDIRECT) {
            slot++;
            continue;
        }
        end = start + UDF_EXT_LEN(ad.len);
        if (end <= target) {
            start = end;
            slot++;
            continue;
        }
        break;
    }
    slot++;
    off  = (uint32_t)target - (uint32_t)start;
    into = (off + bs - 1) / bs;
    rem  = ~off + bs + UDF_EXT_LEN(ad.len);
    if (rem < bs) {
        /* nothing of this descriptor is left (cannot happen for a block
         * inside it): the next non-empty data descriptor, from its start */
        for (;;) {
            uint32_t r;

            UDF_AD_AT(slot, next);
            r = UDF_EXT_LEN(next.len) + bs - 1;
            slot++;
            if (r >= bs && UDF_EXT_FLAGS(next.len) != UDF_EXT_REDIRECT) {
                ad   = next;
                n    = r / bs;
                into = 0;
                break;
            }
        }
    } else {
        n = rem / bs;
    }
#undef UDF_AD_AT
    if (UDF_EXT_FLAGS(ad.len) != UDF_EXT_ALLOCATED) {
        *sector = -1;
        *count  = n;
        return 0;
    }
    t.len = 0;
    t.loc.lb_num   = ad.loc.lb_num + into;
    t.loc.part_num = ad.loc.part_num;
    if ((error = udf_translate_vtop(ump, &t, &lb, &run)) < 0)
        return error;
    *sector = lb;
    *count  = FFMIN(n, run);
    return 0;
}

/*
 * A file's extents in file order, one per piece of its allocation: each
 * allocation descriptor (cut where its translation stops being contiguous),
 * never joined with the next one even when they are adjacent on the disc.
 * Not-recorded pieces have sector -1. The last extent ends at the file size,
 * rounded up to whole 2048-byte sectors. A file whose data is recorded in its
 * file entry has no sector extents (an error). Logical blocks of 2048 bytes
 * are assumed, as on every image.
 */
static int udf_file_extents(UDFMount *ump, const UDFNode *node, DiscIOFile *f)
{
    uint64_t bs = AV_RL32(ump->logical_vol + LVD_LB_SIZE);
    int shift = 0;
    uint64_t size = node->file_size;
    uint64_t pad = (size & 0x7ff) ? 0x800 - (size & 0x7ff) : 0;
    uint64_t blocks_in_file, pos = 0, rem = size;
    int nb_alloc = 0, error;

    while (shift < 63 && (bs >> shift) > 1)
        shift++;
    blocks_in_file = (size + bs - 1) >> shift;
    while (rem != 0) {
        uint32_t b, len, blocks;
        uint64_t bytes, adj;
        int64_t sector;

        if (pos & (bs - 1))
            return udf_err(ump, AVERROR_INVALIDDATA, "the file entry at block %"PRIu32" of partition reference "
                           "%u could not be read: a file piece does not end on a block boundary",
                           node->loc.loc.lb_num, node->loc.loc.part_num);
        b = (uint32_t)(pos >> shift);
        error = udf_block_extent(ump, node, b, &sector, &len);
        if (error < 0)
            return error;
        if (error > 0)
            return udf_err(ump, AVERROR_INVALIDDATA, "the file entry at block %"PRIu32" of partition reference "
                           "%u could not be read: the file's data is recorded inside its file entry",
                           node->loc.loc.lb_num, node->loc.loc.part_num);
        blocks = FFMIN(len, (uint32_t)(blocks_in_file - b));
        if (blocks == 0) {
            /* the same block would be translated again forever */
            return udf_err(ump, AVERROR_INVALIDDATA, "the file entry at block %"PRIu32" of partition reference "
                           "%u could not be read: a file piece of zero blocks (no progress possible)",
                           node->loc.loc.lb_num, node->loc.loc.part_num);
        }
        bytes = (uint64_t)blocks << shift;
        adj = pos + bytes == size + pad ? pad : 0;
        if (f->nb_extents == nb_alloc) {
            int n = nb_alloc ? 2 * nb_alloc : 4;
            DiscIOExtent *e = av_realloc_array(f->extents, n, sizeof(*e));
            if (!e)
                return AVERROR(ENOMEM);
            f->extents = e;
            nb_alloc = n;
        }
        f->extents[f->nb_extents].sector = sector;
        f->extents[f->nb_extents].count  = (uint32_t)((bytes - adj + 0x7ff) >> 11);
        f->nb_extents++;
        pos += bytes - adj;
        rem -= bytes - adj;
    }
    return 0;
}

/*
 * Opens the regular file at path: its size and its extents, one per piece of
 * the file's allocation (see udf_file_extents). A file whose data is recorded
 * in its file entry has no extents: it cannot be opened when
 * UDFCompat.embedded_data_not_a_file is on, else its data is returned.
 */
static int udf_netbsd_open_file(DiscIOFS *fs, const char *path, DiscIOFile **out)
{
    UDFMount *ump = fs->priv;
    UDFNode *parent, *node;
    struct long_ad icb;
    const char *last;
    DiscIOFile *f;
    int found, error;

    *out = NULL;
    if ((error = udf_walk(ump, path, &parent, &last)) < 0)
        return error;
    if ((error = udf_lookup_name_in_dir(ump, parent, last, strlen(last), &icb, &found)) < 0)
        return error;
    if (!found)
        return AVERROR(ENOENT);
    if ((error = udf_get_node(ump, &icb, &node)) < 0)
        return error;
    if (node->v_type != VREG)
        return AVERROR(ENOENT);

    if (!(f = av_mallocz(sizeof(*f))))
        return AVERROR(ENOMEM);
    f->size = node->file_size;
    if ((udf_node_icbflags(node) & UDF_ICB_TAG_FLAGS_ALLOC_MASK) == UDF_ICB_INTERN_ALLOC &&
        !ump->compat->embedded_data_not_a_file) {
        uint32_t lb_size = AV_RL32(ump->logical_vol + LVD_LB_SIZE);

        if (!(f->data = av_malloc(lb_size))) {
            ff_discio_file_free(&f);
            return AVERROR(ENOMEM);
        }
        udf_read_internal(ump, node, f->data);
        if ((uint64_t)f->size > lb_size)
            f->size = lb_size;
    } else if ((error = udf_file_extents(ump, node, f)) < 0) {
        UDF_LOG(ump, AV_LOG_WARNING, "UDF on '%s': could not turn the file entry of '%s' into sector extents; "
                "the UDF file system seems damaged (%s)\n", ump->src->name, path, ump->why);
        ff_discio_file_free(&f);
        return error;
    }
    *out = f;
    return 0;
}

/*
 * Lists the directory at path in directory order: deleted entries are
 * skipped, hidden ones listed as UDFCompat.list_hidden says, the parent entry
 * is a directory named "..", no "." entry. Every entry that is not a
 * directory is looked up again by its name (with duplicate names: the last
 * one), as for its kind and size; a failure there fails the listing. A broken
 * entry fails the listing; a read error ends it.
 */
static int udf_netbsd_list_dir(DiscIOFS *fs, const char *path, DiscIODirCallback cb, void *opaque)
{
    UDFMount *ump = fs->priv;
    uint64_t offset = 0;
    UDFNode *dir;
    int error;

    if ((error = udf_open_dir(ump, path, &dir)) < 0)
        return error;
    for (;;) {
        UDFFid fid;
        int stop, err, is_dir;

        stop = udf_read_fid_stream(ump, dir, &offset, &fid, &err);
        if (stop == FID_END)
            break;
        if (stop == FID_SHORT || stop == FID_BROKEN)
            return udf_err(ump, AVERROR_INVALIDDATA, "a directory holds a broken entry");
        if (stop == FID_READ) {
            if (err == AVERROR(ENOMEM))
                return err;
            UDF_LOG(ump, AV_LOG_DEBUG, "UDF on '%s': directory listing of '%s' ended by a read error: %s\n",
                    ump->src->name, path, ump->why);
            break;
        }
        if (fid.file_char & UDF_FILE_CHAR_DEL)
            continue;
        if ((fid.file_char & UDF_FILE_CHAR_VIS) && !ump->compat->list_hidden)
            continue;
        is_dir = !!(fid.file_char & UDF_FILE_CHAR_DIR);
        if (!is_dir) {
            struct long_ad icb;
            UDFNode *node;
            int found;

            if ((error = udf_lookup_name_in_dir(ump, dir, fid.name, strlen(fid.name), &icb, &found)) < 0)
                return error;
            if (!found)
                return udf_err(ump, AVERROR_INVALIDDATA, "'%s' listed in '%s' not found again", fid.name, path);
            if ((error = udf_get_node(ump, &icb, &node)) < 0)
                return error;
        }
        if ((error = cb(opaque, fid.name, is_dir)))
            return error;
    }
    return 0;
}

static void udf_netbsd_close(DiscIOFS *fs)
{
    if (fs->priv)
        udf_free_mount(fs->priv);
    fs->priv = NULL;
}

static const DiscIOFSOps udf_netbsd_ops = {
    .name      = "UDF (NetBSD)",
    .open_file = udf_netbsd_open_file,
    .list_dir  = udf_netbsd_list_dir,
    .close     = udf_netbsd_close,
};

int ff_discio_udf_netbsd_mount(DiscIOSource *src, DiscIOFS **out)
{
    DiscIOFS *fs;
    UDFMount *ump;
    int64_t sectors;
    int error;

    *out = NULL;
    if (!(fs = av_mallocz(sizeof(*fs))) || !(ump = av_mallocz(sizeof(*ump)))) {
        av_free(fs);
        return AVERROR(ENOMEM);
    }
    fs->ops  = &udf_netbsd_ops;
    fs->priv = ump;
    fs->src  = src;
    ump->src    = src;
    ump->logctx = src->logctx;
    ump->compat = &udf_compat_default;
    /* discinfo of an image: one closed single-track disc of psize sectors */
    ump->sector_size = DISCIO_BLOCK_SIZE;
    sectors = src->size / DISCIO_BLOCK_SIZE;
    ump->psize = sectors > UINT32_MAX ? UINT32_MAX : (uint32_t)sectors;

    if ((error = udf_mountfs(ump)) < 0) {
        UDF_LOG(ump, AV_LOG_DEBUG, "UDF (NetBSD) on '%s': no UDF file system mounted: %s\n", src->name, ump->why);
        udf_free_mount(ump);
        av_free(fs);
        return error;
    }
    udf_volume_info(ump, fs);
    av_log(src->logctx, AV_LOG_VERBOSE, "UDF file system on '%s' (NetBSD reader): label '%s', UDF revision "
           "0x%04x, recorded %d\n", src->name, fs->label, fs->udf_revision, AV_RL16(fs->udf_recording_time + 2));
    *out = fs;
    return 0;
}
