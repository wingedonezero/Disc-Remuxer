// SPDX-License-Identifier: GPL-2.0-only
/*
 * Disc I/O: UDF reader based on Linux fs/udf (read side)
 *
 * Derived from the UDF file system of the Linux kernel, fs/udf (torvalds/linux
 * 551c722f): super.c, lowlevel.c, misc.c, partition.c, inode.c, directory.c,
 * namei.c, dir.c and unicode.c, adapted to a userspace reader of disc images.
 * The UTF-8 encoding step follows utf32_to_utf8 of fs/nls/nls_base.c.
 *
 * COPYRIGHT (of the original files)
 *  (C) 1998 Dave Boynton
 *  (C) 1998-2004 Ben Fennema
 *  (C) 1999-2000 Stelias Computing Inc
 *  (C) 2000 Stelias Computing Inc
 *  (C) 1998-2001 Ben Fennema
 *  (C) 1999-2001 Ben Fennema
 * and the later contributors to fs/udf of the Linux kernel.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * The volume is mounted the way Linux mounts it read-only with default options
 * (no novrs, session, anchor, lastblock, unhide, undelete, no NLS character
 * set: names are UTF-8). Function names, order and comments follow the
 * upstream files; the kernel plumbing (buffer heads, page cache, locks, the
 * inode life cycle, extent cache, write paths, mount options) is left out.
 *
 * Adaptations to a disc image:
 * - The device is the image: 2048-byte sectors, last block = size - 1, no
 *   multisession. Block sizes 2048 and 4096 are tried (Linux starts at the
 *   device's logical block size; on 2048-byte media smaller sizes find no
 *   anchor).
 * - Blocks are read with ff_discio_read_blocks() and the source's attempt
 *   count; every failed attempt is logged there.
 * - Reads past the end of a buffer that the C code would make on malformed
 *   lengths end with an error instead.
 * - The volume descriptor sequence also keeps the implementation use volume
 *   descriptor, for the UDF revision of DiscIOFS.
 *
 * Structural guards (never time-based), where the C code could loop forever
 * or step outside its data:
 * - udf_find_vat_block stops at block 0 instead of wrapping below it.
 * - A strategy-4096 indirect entry chain ends after 1024 entries, as upstream.
 *
 * The operations of the common interface follow the Linux VFS on top of the
 * driver: a path is split on '/', empty components and "." are skipped, ".."
 * goes through the parent entry and the root's ".." is the root ('\' is a
 * plain character). A listing is readdir without "." (deleted and hidden
 * entries skipped, names that cannot be converted skipped, the parent entry
 * as ".."), each entry's kind taken from looking its name up (first match),
 * as `ls -l` does. A file's extents are the device sectors of its blocks as
 * udf_map_block maps them, adjacent pieces joined; a run of blocks that are
 * not recorded (or lie past the allocation descriptors) is an extent with
 * sector -1 (reads as zeros). Data recorded inside the file entry is returned
 * in DiscIOFile.data, with no extents. The label is the logical volume
 * identifier decoded with udf_dstrCS0toChar (at most 254 bytes, as upstream,
 * cut before a character that does not fit); the UDF revision and the
 * recording time are read from the implementation use and primary volume
 * descriptors.
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

/* ---- constants (ecma_167.h, osta_udf.h, udfdecl.h, udf_sb.h) ---- */

#define UDF_MAX_BLOCKSIZE       4096

#define TAG_IDENT_PVD           0x0001
#define TAG_IDENT_AVDP          0x0002
#define TAG_IDENT_VDP           0x0003
#define TAG_IDENT_IUVD          0x0004
#define TAG_IDENT_PD            0x0005
#define TAG_IDENT_LVD           0x0006
#define TAG_IDENT_USD           0x0007
#define TAG_IDENT_TD            0x0008
#define TAG_IDENT_LVID          0x0009
#define TAG_IDENT_FSD           0x0100
#define TAG_IDENT_FID           0x0101
#define TAG_IDENT_IE            0x0103
#define TAG_IDENT_FE            0x0105
#define TAG_IDENT_EAHD          0x0106
#define TAG_IDENT_USE           0x0107
#define TAG_IDENT_EFE           0x010A

#define VSD_FIRST_SECTOR_OFFSET 32768
#define VSD_MAX_SECTOR_OFFSET   0x800000

/*
 * Maximum number of Terminating Descriptor / Logical Volume Integrity
 * Descriptor redirections. The chosen numbers are arbitrary - just that we
 * hopefully don't limit any real use of rewritten inode on write-once media
 * but avoid looping for too long on corrupted media.
 */
#define UDF_MAX_TD_NESTING      64
#define UDF_MAX_LVID_NESTING    1000
#define UDF_MAX_READ_VERSION    0x0260
#define UDF_MAX_ICB_NESTING     1024
/*
 * Only 1 indirect extent in a row really makes sense but allow upto 16 in case
 * someone does some weird stuff.
 */
#define UDF_MAX_INDIR_EXTS      16

#define PD_ACCESS_TYPE_NONE         0
#define PD_ACCESS_TYPE_READ_ONLY    1
#define PD_ACCESS_TYPE_WRITE_ONCE   2

#define ICBTAG_FLAG_AD_MASK     0x0007
#define ICBTAG_FLAG_AD_SHORT    0x0000
#define ICBTAG_FLAG_AD_LONG     0x0001
#define ICBTAG_FLAG_AD_IN_ICB   0x0003

#define ICBTAG_FILE_TYPE_UNDEF      0x00
#define ICBTAG_FILE_TYPE_DIRECTORY  0x04
#define ICBTAG_FILE_TYPE_REGULAR    0x05
#define ICBTAG_FILE_TYPE_BLOCK      0x06
#define ICBTAG_FILE_TYPE_CHAR       0x07
#define ICBTAG_FILE_TYPE_FIFO       0x09
#define ICBTAG_FILE_TYPE_SOCKET     0x0A
#define ICBTAG_FILE_TYPE_SYMLINK    0x0C
#define ICBTAG_FILE_TYPE_VAT20      0xF8
#define ICBTAG_FILE_TYPE_REALTIME   0xF9
#define ICBTAG_FILE_TYPE_MAIN       0xFA
#define ICBTAG_FILE_TYPE_MIRROR     0xFB
#define ICBTAG_FILE_TYPE_BITMAP     0xFC

/* extent types (extLength >> 30) */
#define EXT_RECORDED_ALLOCATED      0
#define EXT_NEXT_EXTENT_ALLOCDESCS  3
#define UDF_EXTENT_LENGTH_MASK      0x3FFFFFFF

#define FID_FILE_CHAR_HIDDEN    0x01
#define FID_FILE_CHAR_DELETED   0x04
#define FID_FILE_CHAR_PARENT    0x08

/* structure sizes */
#define SIZEOF_TAG              16
#define SIZEOF_LVD              440   /* struct logicalVolDesc */
#define SIZEOF_GPM1             6     /* struct genericPartitionMap1 */
#define SIZEOF_LVID             80    /* struct logicalVolIntegrityDesc */
#define SIZEOF_FE               176   /* struct fileEntry */
#define SIZEOF_EFE              216   /* struct extendedFileEntry */
#define SIZEOF_USE              40    /* struct unallocSpaceEntry */
#define SIZEOF_AED              24    /* struct allocExtDesc */
#define SIZEOF_FID              38    /* struct fileIdentDesc */
#define SIZEOF_EAHD             24    /* struct extendedAttrHeaderDesc */
#define SIZEOF_GAF              12    /* struct genericFormat */
#define SIZEOF_SPACE_BITMAP     24    /* struct spaceBitmapDesc */
#define SIZEOF_SPARING_TABLE    56    /* struct sparingTable without mapEntry */
#define SIZEOF_SPARING_ENTRY    8

#define UDF_NAME_LEN            254   /* output size of a file name */
#define UDF_NAME_PAD            4

#define UDF_BAD_BLOCK           0xFFFFFFFFu
/* upstream's -EAGAIN of the mount steps: try the next anchor / sequence */
#define UDF_TRY_NEXT            1

/* ---- in-memory structures ---- */

/* struct kernel_lb_addr */
typedef struct LbAddr {
    uint32_t lbn;
    uint16_t part;
} LbAddr;

/* UDF_*_MAP* partition types; PART_NONE = a type-2 map with an unknown
 * identifier (no translation function: root + block) */
enum PartType {
    PART_NONE,
    PART_TYPE1,
    PART_VIRTUAL15,
    PART_VIRTUAL20,
    PART_SPARABLE,
    PART_METADATA,
};

/* read side of struct udf_inode_info with the inode fields used */
typedef struct UDFInode {
    LbAddr   location;
    uint16_t alloc_type;
    int      efe, use;
    uint8_t *data;          /* i_data: the block after the (extended) file entry header */
    uint32_t data_len;
    uint32_t len_eattr, len_alloc;
    uint64_t size;
    uint8_t  file_type;
    int      hidden;
} UDFInode;

/* struct udf_part_map, read side */
typedef struct PartMap {
    enum PartType ptype;
    uint16_t  partition_num;
    uint32_t  root, len;
    uint8_t  *spar_map[4];  /* sparing tables that passed the checks */
    uint16_t  packet_len;
    uint32_t  vat_start_offset, vat_num_entries;
    uint32_t  meta_file_loc, mirror_file_loc, bitmap_file_loc;
    uint16_t  phys_partition_ref;
    UDFInode *metadata_fe, *mirror_fe;  /* owned by the inode cache */
    int       mirror_loaded;            /* MF_MIRROR_FE_LOADED */
} PartMap;

typedef struct ICacheSlot {
    uint32_t  block;
    UDFInode *inode;
} ICacheSlot;

/* the read side of struct udf_sb_info */
typedef struct UDFLinux {
    DiscIOSource *src;
    uint32_t  blocksize;    /* sb->s_blocksize */
    int       bits;         /* sb->s_blocksize_bits */
    uint64_t  sectors;      /* size of the device in 2048-byte sectors */
    uint32_t  last_block;
    uint8_t  *lvid;         /* s_lvid_bh */
    PartMap  *partmaps;
    int       nb_partmaps;
    uint16_t  partition;    /* s_partition: partition reference of the file set */
    UDFInode *vat_inode;    /* owned by the inode cache */
    int       rw_incompat;  /* UDF_FLAG_RW_INCOMPAT */
    uint8_t  *pvd, *lvd, *iuvd;
    LbAddr    rootdir;
    /* the inode cache (iget_locked), keyed by physical block */
    ICacheSlot *icache;
    unsigned  icache_size, icache_count;
    char      why[160];     /* reason of the last failure, for the final log line */
} UDFLinux;

/* struct extent_position */
typedef struct ExtentPosition {
    uint8_t *bh;            /* allocation extent block (blocksize bytes) or NULL */
    uint32_t offset;
    LbAddr   block;
} ExtentPosition;

/* one allocation descriptor read by udf_current_aext */
typedef struct Aext {
    LbAddr   eloc;
    uint32_t elen;
    int      etype;
} Aext;

/* ---- logging ---- */

static av_printf_format(3, 4) void udf_log(UDFLinux *sb, int level, const char *fmt, ...)
{
    char msg[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    av_log(sb->src->logctx, level, "UDF (Linux) on '%s': %s\n", sb->src->name, msg);
}

/* Records why an operation fails (for its final log line), logs it at debug
 * level and returns code. */
static av_printf_format(3, 4) int udf_fail(UDFLinux *sb, int code, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(sb->why, sizeof(sb->why), fmt, ap);
    va_end(ap);
    udf_log(sb, AV_LOG_DEBUG, "%s", sb->why);
    return code;
}

/* the file entry at loc cannot be used: why */
static int udf_corrupt(UDFLinux *sb, LbAddr loc, const char *why)
{
    return udf_fail(sb, AVERROR_INVALIDDATA,
                    "the file entry at block %"PRIu32" of partition reference %u could not be read: %s",
                    loc.lbn, loc.part, why);
}

/* a directory holds a broken entry */
static int udf_dir_corrupt(UDFLinux *sb, const UDFInode *dir, const char *why)
{
    udf_log(sb, AV_LOG_WARNING, "directory at block %"PRIu32": %s", dir->location.lbn, why);
    return udf_fail(sb, AVERROR_INVALIDDATA, "the directory at block %"PRIu32" holds a broken entry (%s)",
                    dir->location.lbn, why);
}

/* ---- CRC (crc_itu_t: polynomial 0x1021, MSB first) ---- */

static uint16_t crc_itu_t(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int k = 0; k < 8; k++)
            crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---- lowlevel.c: device access ---- */

/* device blocks of blocksize bytes */
static uint64_t udf_nr_blocks(const UDFLinux *sb)
{
    return sb->sectors * DISCIO_BLOCK_SIZE / sb->blocksize;
}

/* sb_bread: one block into buf (blocksize bytes) */
static int sb_bread(UDFLinux *sb, uint32_t block, uint8_t *buf)
{
    return ff_discio_read_blocks(sb->src, (int64_t)block * sb->blocksize, buf, sb->blocksize,
                                 sb->src->attempts, 0);
}

/* udf_get_last_block for an image: its last block */
static uint32_t udf_get_last_block(const UDFLinux *sb)
{
    uint64_t n = udf_nr_blocks(sb);

    if (n > UINT32_MAX || n == 0)
        return 0;
    return (uint32_t)(n - 1);
}

/* ---- the inode cache ---- */

static void udf_free_inode(UDFInode *inode)
{
    if (inode) {
        av_free(inode->data);
        av_free(inode);
    }
}

static void icache_clear(UDFLinux *sb)
{
    for (unsigned i = 0; i < sb->icache_size; i++)
        udf_free_inode(sb->icache[i].inode);
    av_freep(&sb->icache);
    sb->icache_size = sb->icache_count = 0;
}

static UDFInode *icache_find(const UDFLinux *sb, uint32_t block)
{
    if (!sb->icache_size)
        return NULL;
    for (unsigned i = (block * 2654435761u) & (sb->icache_size - 1); sb->icache[i].inode;
         i = (i + 1) & (sb->icache_size - 1))
        if (sb->icache[i].block == block)
            return sb->icache[i].inode;
    return NULL;
}

static void icache_put(ICacheSlot *slots, unsigned size, uint32_t block, UDFInode *inode)
{
    unsigned i = (block * 2654435761u) & (size - 1);

    while (slots[i].inode)
        i = (i + 1) & (size - 1);
    slots[i].block = block;
    slots[i].inode = inode;
}

static int icache_insert(UDFLinux *sb, uint32_t block, UDFInode *inode)
{
    if ((sb->icache_count + 1) * 2 > sb->icache_size) {
        unsigned size = sb->icache_size ? sb->icache_size * 2 : 64;
        ICacheSlot *slots = av_calloc(size, sizeof(*slots));

        if (!slots)
            return AVERROR(ENOMEM);
        for (unsigned i = 0; i < sb->icache_size; i++)
            if (sb->icache[i].inode)
                icache_put(slots, size, sb->icache[i].block, sb->icache[i].inode);
        av_free(sb->icache);
        sb->icache      = slots;
        sb->icache_size = size;
    }
    icache_put(sb->icache, sb->icache_size, block, inode);
    sb->icache_count++;
    return 0;
}

/* ---- misc.c ---- */

static uint8_t udf_tag_checksum(const uint8_t *t)
{
    uint8_t checksum = 0;

    for (int i = 0; i < SIZEOF_TAG; ++i)
        if (i != 4) /* position of checksum */
            checksum += t[i];
    return checksum;
}

/*
 * Read the first block of a tagged descriptor: the block at block when its
 * tag records location, its checksum and version (2 or 3) are right and its
 * CRC matches over a CRC length that fits the block.
 * Returns 0 (bh holds the block, *ident its tag identifier) or -1.
 */
static int udf_read_tagged(UDFLinux *sb, uint32_t block, uint32_t location, uint8_t *bh,
                           uint16_t *ident)
{
    unsigned crc_len;
    uint8_t checksum;
    uint16_t ver;
    int ret;

    /* Read the block */
    if (block == 0xFFFFFFFF)
        return -1;

    if ((ret = sb_bread(sb, block, bh)) < 0) {
        udf_log(sb, AV_LOG_DEBUG, "read failed, block=%"PRIu32", location=%"PRIu32" (%s)",
                block, location, av_err2str(ret));
        return -1;
    }

    *ident = AV_RL16(bh);

    if (location != AV_RL32(bh + 12)) {
        udf_log(sb, AV_LOG_TRACE, "location mismatch block %"PRIu32", tag %"PRIu32" != %"PRIu32,
                block, AV_RL32(bh + 12), location);
        return -1;
    }

    /* Verify the tag checksum */
    checksum = udf_tag_checksum(bh);
    if (checksum != bh[4]) {
        udf_log(sb, AV_LOG_DEBUG, "tag checksum failed, block %"PRIu32": 0x%02x != 0x%02x",
                block, checksum, bh[4]);
        return -1;
    }

    /* Verify the tag version */
    ver = AV_RL16(bh + 2);
    if (ver != 0x0002 && ver != 0x0003) {
        udf_log(sb, AV_LOG_DEBUG, "tag version 0x%04x != 0x0002 || 0x0003, block %"PRIu32, ver, block);
        return -1;
    }

    /* Verify the descriptor CRC */
    crc_len = AV_RL16(bh + 10);
    if (crc_len + SIZEOF_TAG > sb->blocksize) {
        udf_log(sb, AV_LOG_DEBUG, "block %"PRIu32": CRC length %u exceeds block size", block, crc_len);
        return -1;
    }
    if (AV_RL16(bh + 8) == crc_itu_t(0, bh + SIZEOF_TAG, crc_len))
        return 0;

    udf_log(sb, AV_LOG_DEBUG, "Crc failure block %"PRIu32": crc = %u, crclen = %u",
            block, AV_RL16(bh + 8), crc_len);
    return -1;
}

static uint32_t udf_get_pblock(UDFLinux *sb, uint32_t block, uint16_t partition, uint32_t offset);

/* the tagged block at logical address loc + offset */
static int udf_read_ptagged(UDFLinux *sb, LbAddr loc, uint32_t offset, uint8_t *bh, uint16_t *ident)
{
    return udf_read_tagged(sb, udf_get_pblock(sb, loc.lbn, loc.part, offset),
                           loc.lbn + offset, bh, ident);
}

/* The extended attribute of type / subtype in the inode's extended
 * attribute area, as an offset into i_data, or -1. */
static int64_t udf_get_extendedattr(const UDFInode *inode, uint32_t type, uint8_t subtype)
{
    const uint8_t *ea = inode->data;
    uint64_t offset;

    if (!inode->len_eattr || inode->data_len < SIZEOF_EAHD)
        return -1;

    /* check checksum/crc */
    if (AV_RL16(ea) != TAG_IDENT_EAHD || AV_RL32(ea + 12) != inode->location.lbn)
        return -1;

    if (type < 2048)
        offset = SIZEOF_EAHD;
    else if (type < 65536)
        offset = AV_RL32(ea + 16);     /* impAttrLocation */
    else
        offset = AV_RL32(ea + 20);     /* appAttrLocation */

    while (offset + SIZEOF_GAF < inode->len_eattr) {
        uint32_t attr_length;

        if (offset + SIZEOF_GAF > inode->data_len)
            return -1;
        attr_length = AV_RL32(ea + offset + 8);

        /* Detect undersized elements and buffer overflows */
        if (attr_length < SIZEOF_GAF || attr_length > inode->len_eattr - offset)
            break;

        if (AV_RL32(ea + offset) == type && ea[offset + 4] == subtype)
            return offset;
        offset += attr_length;
    }
    return -1;
}

/* ---- inode.c (part 1): allocation descriptors ---- */

/* udf_file_entry_alloc_offset */
static uint32_t udf_file_entry_alloc_offset(const UDFInode *inode)
{
    if (inode->use)
        return SIZEOF_USE;
    if (inode->efe)
        return SIZEOF_EFE + inode->len_eattr;
    return SIZEOF_FE + inode->len_eattr;
}

static int inode_is_dir(const UDFInode *inode)
{
    return inode->file_type == ICBTAG_FILE_TYPE_DIRECTORY;
}

static int inode_is_reg(const UDFInode *inode)
{
    return inode->file_type == ICBTAG_FILE_TYPE_REALTIME ||
           inode->file_type == ICBTAG_FILE_TYPE_REGULAR ||
           inode->file_type == ICBTAG_FILE_TYPE_UNDEF ||
           inode->file_type == ICBTAG_FILE_TYPE_VAT20;
}

static void epos_release(ExtentPosition *epos)
{
    av_freep(&epos->bh);
}

/*
 * udf_current_aext: the allocation descriptor at epos.
 * Returns 1 (found), 0 at the end, or a negative AVERROR code.
 */
static int udf_current_aext(UDFLinux *sb, const UDFInode *inode, ExtentPosition *epos, Aext *a,
                            int inc)
{
    const uint8_t *buf;
    uint64_t start, alen, at;
    uint32_t raw_len, adsize;
    uint64_t buf_len;

    if (!epos->bh) {
        uint32_t alloc_offset = udf_file_entry_alloc_offset(inode);

        if (!epos->offset)
            epos->offset = alloc_offset;
        /* ptr = i_data + offset - alloc_offset + lenEAttr */
        buf     = inode->data;
        buf_len = inode->data_len;
        start   = (uint64_t)inode->len_eattr - alloc_offset;   /* may be "negative": wraps back below */
        alen    = (uint64_t)alloc_offset + inode->len_alloc;
    } else {
        if (!epos->offset)
            epos->offset = SIZEOF_AED;
        buf     = epos->bh;
        buf_len = sb->blocksize;
        start   = 0;
        alen    = SIZEOF_AED + (uint64_t)AV_RL32(epos->bh + 20);  /* lengthAllocDescs */
        if (alen > UINT32_MAX)
            return udf_corrupt(sb, inode->location, "bad allocation extent");
        if (alen > buf_len)
            return udf_corrupt(sb, inode->location, "allocation extent descriptor longer than its block");
    }

    switch (inode->alloc_type) {
    case ICBTAG_FLAG_AD_SHORT:
        adsize = 8;
        break;
    case ICBTAG_FLAG_AD_LONG:
        adsize = 16;
        break;
    default:
        udf_log(sb, AV_LOG_DEBUG, "alloc_type = %u unsupported", inode->alloc_type);
        return udf_corrupt(sb, inode->location, "unsupported allocation descriptor type");
    }

    if ((uint64_t)epos->offset + adsize > alen)
        return 0;
    at = (start + epos->offset) & 0xFFFFFFFF;
    if (at + adsize > buf_len)
        return udf_corrupt(sb, inode->location, "allocation descriptors run past the buffer");
    raw_len = AV_RL32(buf + at);
    if (!raw_len)
        return 0;
    if (inc)
        epos->offset += adsize;

    a->eloc.lbn = AV_RL32(buf + at + 4);
    a->eloc.part = adsize == 8 ? inode->location.part : AV_RL16(buf + at + 8);
    a->elen  = raw_len & UDF_EXTENT_LENGTH_MASK;
    a->etype = raw_len >> 30;

    if (a->eloc.part >= sb->nb_partmaps) {
        udf_log(sb, AV_LOG_DEBUG, "invalid partition reference %u (partitions %d)",
                a->eloc.part, sb->nb_partmaps);
        return udf_corrupt(sb, inode->location, "invalid partition reference in an extent");
    }
    return 1;
}

/*
 * udf_next_aext: like udf_current_aext, following allocation extent
 * descriptors (at most 16 in a row; their blocks are read without a tag
 * check). Returns 1, 0 at the end, or a negative AVERROR code.
 */
static int udf_next_aext(UDFLinux *sb, const UDFInode *inode, ExtentPosition *epos, Aext *a, int inc)
{
    unsigned indirections = 0;

    while (1) {
        uint32_t block;
        int ret = udf_current_aext(sb, inode, epos, a, inc);

        if (ret <= 0)
            return ret;
        if (a->etype != EXT_NEXT_EXTENT_ALLOCDESCS)
            return ret;

        if (++indirections > UDF_MAX_INDIR_EXTS) {
            udf_log(sb, AV_LOG_WARNING, "too many indirect extents in the inode at block %"PRIu32,
                    inode->location.lbn);
            return udf_corrupt(sb, inode->location, "too many indirect extents");
        }

        epos->block  = a->eloc;
        epos->offset = SIZEOF_AED;
        block = udf_get_pblock(sb, a->eloc.lbn, a->eloc.part, 0);
        if (!epos->bh && !(epos->bh = av_malloc(sb->blocksize)))
            return AVERROR(ENOMEM);
        if ((ret = sb_bread(sb, block, epos->bh)) < 0) {
            udf_log(sb, AV_LOG_DEBUG, "reading block %"PRIu32" failed!", block);
            av_freep(&epos->bh);
            return udf_fail(sb, ret, "reading the allocation extent at block %"PRIu32" failed", block);
        }
    }
}

/*
 * inode_bmap: the extent holding file block block. Returns 1 (*a the extent,
 * *offset the block's offset in it), 0 past the last extent, or a negative
 * AVERROR code.
 */
static int inode_bmap(UDFLinux *sb, const UDFInode *inode, uint64_t block, Aext *a, uint64_t *offset)
{
    uint64_t lbcount = 0, bcount = block << sb->bits;
    ExtentPosition pos = { .bh = NULL, .offset = 0, .block = inode->location };
    int err;

    do {
        err = udf_next_aext(sb, inode, &pos, a, 1);
        if (err <= 0) {
            epos_release(&pos);
            return err;
        }
        lbcount += a->elen;
    } while (lbcount <= bcount);
    epos_release(&pos);
    *offset = (bcount + a->elen - lbcount) >> sb->bits;
    return 1;
}

/*
 * udf_map_block without create: *pblk = the device block of file block
 * block, or UDF_BAD_BLOCK when it is not recorded (reads as zeros).
 */
static int udf_map_block(UDFLinux *sb, const UDFInode *inode, uint64_t block, uint32_t *pblk)
{
    uint64_t offset;
    Aext a;
    int ret;

    if (inode->alloc_type == ICBTAG_FLAG_AD_IN_ICB)
        return udf_corrupt(sb, inode->location, "block mapping of embedded data");

    *pblk = UDF_BAD_BLOCK;
    ret = inode_bmap(sb, inode, block, &a, &offset);
    if (ret < 0)
        return ret;
    if (ret > 0 && a.etype == EXT_RECORDED_ALLOCATED)
        *pblk = udf_get_pblock(sb, a.eloc.lbn, a.eloc.part, (uint32_t)offset);
    return 0;
}

/*
 * udf_bread without create: file block block into buf. Returns 1 (read),
 * 0 when the block is not recorded, or a negative AVERROR code.
 */
static int udf_bread(UDFLinux *sb, const UDFInode *inode, uint32_t block, uint8_t *buf)
{
    uint32_t pblk;
    int ret = udf_map_block(sb, inode, block, &pblk);

    if (ret < 0)
        return ret;
    if (pblk == UDF_BAD_BLOCK)
        return 0;
    if ((ret = sb_bread(sb, pblk, buf)) < 0)
        return udf_fail(sb, ret, "reading block %"PRIu32" failed", pblk);
    return 1;
}

/* ---- partition.c ---- */

static uint32_t udf_get_pblock_virt15(UDFLinux *sb, uint32_t block, uint16_t partition, uint32_t offset)
{
    const PartMap *map = &sb->partmaps[partition];
    const UDFInode *vat = sb->vat_inode;
    uint32_t loc;

    if (!vat)
        return UDF_BAD_BLOCK;

    if (block >= map->vat_num_entries) {
        udf_log(sb, AV_LOG_DEBUG, "Trying to access block beyond end of VAT (%"PRIu32" max %"PRIu32")",
                block, map->vat_num_entries);
        return UDF_BAD_BLOCK;
    }

    if (vat->alloc_type == ICBTAG_FLAG_AD_IN_ICB) {
        uint64_t at = map->vat_start_offset + 4ULL * block;

        if (at + 4 > vat->data_len)
            return UDF_BAD_BLOCK;
        loc = AV_RL32(vat->data + at);
    } else {
        uint8_t bh[UDF_MAX_BLOCKSIZE];
        uint32_t bs = sb->blocksize;
        uint32_t index = (bs - map->vat_start_offset) / 4;
        uint32_t newblock;

        if (block >= index) {
            uint32_t b = block - index;
            newblock = 1 + b / (bs / 4);
            index    = b % (bs / 4);
        } else {
            newblock = 0;
            index    = map->vat_start_offset / 4 + block;
        }

        if (udf_bread(sb, vat, newblock, bh) <= 0) {
            udf_log(sb, AV_LOG_DEBUG, "get_pblock(UDF_VIRTUAL_MAP, %"PRIu32", %u) failed", block, partition);
            return UDF_BAD_BLOCK;
        }
        loc = AV_RL32(bh + 4 * index);
    }

    if (vat->location.part == partition) {
        udf_log(sb, AV_LOG_DEBUG, "recursive call to udf_get_pblock!");
        return UDF_BAD_BLOCK;
    }

    return udf_get_pblock(sb, loc, vat->location.part, offset);
}

static uint32_t udf_get_pblock_spar15(UDFLinux *sb, uint32_t block, uint16_t partition, uint32_t offset)
{
    const PartMap *map = &sb->partmaps[partition];
    const uint8_t *st = NULL;
    uint32_t plen = map->packet_len;
    uint32_t packet = (block + offset) & ~(plen - 1);

    for (int i = 0; i < 4; i++) {
        if (map->spar_map[i]) {
            st = map->spar_map[i];
            break;
        }
    }

    if (st) {
        unsigned rt_l = AV_RL16(st + 48);   /* reallocationTableLen */

        for (unsigned i = 0; i < rt_l; i++) {
            unsigned at = SIZEOF_SPARING_TABLE + SIZEOF_SPARING_ENTRY * i;
            uint32_t orig_loc;

            if (at + SIZEOF_SPARING_ENTRY > sb->blocksize)
                break;
            orig_loc = AV_RL32(st + at);
            if (orig_loc >= 0xFFFFFFF0)
                break;
            else if (orig_loc == packet)
                return AV_RL32(st + at + 4) + ((block + offset) & (plen - 1));
            else if (orig_loc > packet)
                break;
        }
    }

    return map->root + block + offset;
}

static uint32_t udf_try_read_meta(UDFLinux *sb, const UDFInode *inode, uint32_t block,
                                  uint16_t partition, uint32_t offset)
{
    uint64_t ext_offset;
    Aext a;
    int err = inode_bmap(sb, inode, block, &a, &ext_offset);

    if (err <= 0 || a.etype != EXT_RECORDED_ALLOCATED)
        return UDF_BAD_BLOCK;
    /* map to sparable/physical partition desc */
    return udf_get_pblock(sb, a.eloc.lbn, sb->partmaps[partition].phys_partition_ref,
                          (uint32_t)ext_offset + offset);
}

static UDFInode *udf_find_metadata_inode_efe(UDFLinux *sb, uint32_t meta_file_loc, uint16_t partition_ref);

static uint32_t udf_get_pblock_meta25(UDFLinux *sb, uint32_t block, uint16_t partition, uint32_t offset)
{
    PartMap *map = &sb->partmaps[partition];
    const UDFInode *inode = map->metadata_fe ? map->metadata_fe : map->mirror_fe;
    uint32_t retblk;

    if (!inode)
        return UDF_BAD_BLOCK;

    retblk = udf_try_read_meta(sb, inode, block, partition, offset);
    if (retblk == UDF_BAD_BLOCK && map->metadata_fe) {
        udf_log(sb, AV_LOG_WARNING, "error reading from METADATA, trying to read from MIRROR");
        if (!map->mirror_loaded) {
            map->mirror_fe = udf_find_metadata_inode_efe(sb, map->mirror_file_loc,
                                                         map->phys_partition_ref);
            map->mirror_loaded = 1;
        }

        inode = map->mirror_fe;
        if (!inode)
            return UDF_BAD_BLOCK;
        retblk = udf_try_read_meta(sb, inode, block, partition, offset);
    }

    return retblk;
}

/* logical block block + offset of partition reference partition to a
 * device block; UDF_BAD_BLOCK when it cannot be translated */
static uint32_t udf_get_pblock(UDFLinux *sb, uint32_t block, uint16_t partition, uint32_t offset)
{
    const PartMap *map;

    if (partition >= sb->nb_partmaps) {
        udf_log(sb, AV_LOG_DEBUG, "block=%"PRIu32", partition=%u, offset=%"PRIu32": invalid partition",
                block, partition, offset);
        return UDF_BAD_BLOCK;
    }
    map = &sb->partmaps[partition];
    switch (map->ptype) {
    case PART_VIRTUAL15:
    case PART_VIRTUAL20:
        return udf_get_pblock_virt15(sb, block, partition, offset);
    case PART_SPARABLE:
        return udf_get_pblock_spar15(sb, block, partition, offset);
    case PART_METADATA:
        return udf_get_pblock_meta25(sb, block, partition, offset);
    default:
        return map->root + block + offset;
    }
}

/* ---- inode.c (part 2): reading inodes ---- */

static int udf_read_inode(UDFLinux *sb, LbAddr iloc, int hidden_inode, UDFInode **out)
{
    uint8_t bh[UDF_MAX_BLOCKSIZE], ibh[UDF_MAX_BLOCKSIZE];
    uint32_t bs = sb->blocksize;
    unsigned indirections = 0;
    unsigned link_count;
    uint16_t ident, strategy;
    UDFInode *inode;
    int ret;

reread:
    if (iloc.part >= sb->nb_partmaps) {
        udf_log(sb, AV_LOG_DEBUG, "partition reference: %u > logical volume partitions: %d",
                iloc.part, sb->nb_partmaps);
        return udf_corrupt(sb, iloc, "partition reference out of range");
    }

    if (iloc.lbn >= sb->partmaps[iloc.part].len) {
        udf_log(sb, AV_LOG_DEBUG, "block=%"PRIu32", partition=%u out of range", iloc.lbn, iloc.part);
        return udf_corrupt(sb, iloc, "block out of range");
    }

    if (udf_read_ptagged(sb, iloc, 0, bh, &ident) < 0)
        return udf_corrupt(sb, iloc, "file entry unreadable");

    if (ident != TAG_IDENT_FE && ident != TAG_IDENT_EFE && ident != TAG_IDENT_USE) {
        udf_log(sb, AV_LOG_DEBUG, "inode at block %"PRIu32" has tag %u", iloc.lbn, ident);
        return udf_corrupt(sb, iloc, "not a file entry");
    }

    strategy = AV_RL16(bh + 20);    /* icbTag.strategyType */
    if (strategy == 4096) {
        uint16_t iident;

        if (!udf_read_ptagged(sb, iloc, 1, ibh, &iident) && iident == TAG_IDENT_IE &&
            AV_RL32(ibh + 36)) {    /* indirectICB.extLength */
            iloc.lbn  = AV_RL32(ibh + 40);
            iloc.part = AV_RL16(ibh + 44);
            if (++indirections > UDF_MAX_ICB_NESTING) {
                udf_log(sb, AV_LOG_WARNING, "too many ICBs in ICB hierarchy (max %d supported)",
                        UDF_MAX_ICB_NESTING);
                return udf_corrupt(sb, iloc, "too many indirect entries");
            }
            goto reread;
        }
    } else if (strategy != 4) {
        udf_log(sb, AV_LOG_WARNING, "unsupported strategy type: %u", strategy);
        return udf_corrupt(sb, iloc, "unsupported strategy type");
    }

    if (!(inode = av_mallocz(sizeof(*inode))))
        return AVERROR(ENOMEM);
    inode->location   = iloc;
    inode->alloc_type = AV_RL16(bh + 34) & ICBTAG_FLAG_AD_MASK;   /* icbTag.flags */
    inode->file_type  = bh[27];                                    /* icbTag.fileType */
    inode->hidden     = hidden_inode;
    if (inode->alloc_type != ICBTAG_FLAG_AD_SHORT &&
        inode->alloc_type != ICBTAG_FLAG_AD_LONG &&
        inode->alloc_type != ICBTAG_FLAG_AD_IN_ICB) {
        ret = udf_corrupt(sb, iloc, "unsupported allocation descriptor type");
        goto out;
    }

    {
        uint32_t hdr = ident == TAG_IDENT_EFE ? SIZEOF_EFE : ident == TAG_IDENT_FE ? SIZEOF_FE : SIZEOF_USE;

        inode->efe = ident == TAG_IDENT_EFE;
        inode->use = ident == TAG_IDENT_USE;
        inode->data_len = bs - hdr;
        if (!(inode->data = av_malloc(inode->data_len))) {
            ret = AVERROR(ENOMEM);
            goto out;
        }
        memcpy(inode->data, bh + hdr, inode->data_len);
    }
    if (inode->use) {
        inode->len_alloc = AV_RL32(bh + 36);    /* lengthAllocDescs */
        if (inode->len_alloc > bs - SIZEOF_USE) {
            ret = udf_corrupt(sb, iloc, "allocation descriptors longer than the block");
            goto out;
        }
        *out = inode;
        return 0;
    }

    link_count = AV_RL16(bh + 48);   /* fileLinkCount */
    if (!link_count) {
        if (!hidden_inode) {
            ret = udf_corrupt(sb, iloc, "link count is zero");
            goto out;
        }
        link_count = 1;
    }

    inode->size = AV_RL64(bh + 56);  /* informationLength */
    if (!inode->efe) {
        inode->len_eattr = AV_RL32(bh + 168);
        inode->len_alloc = AV_RL32(bh + 172);
    } else {
        inode->len_eattr = AV_RL32(bh + 208);
        inode->len_alloc = AV_RL32(bh + 212);
    }

    /*
     * Sanity check length of allocation descriptors and extended attrs to
     * avoid integer overflows
     */
    if (inode->len_eattr > bs || inode->len_alloc > bs) {
        ret = udf_corrupt(sb, iloc, "extended attribute or allocation length larger than the block");
        goto out;
    }
    /* Now do exact checks */
    if (udf_file_entry_alloc_offset(inode) + inode->len_alloc > bs) {
        ret = udf_corrupt(sb, iloc, "allocation descriptors run past the block");
        goto out;
    }
    /* Sanity checks for files in ICB so that we don't get confused later */
    if (inode->alloc_type == ICBTAG_FLAG_AD_IN_ICB) {
        /*
         * For file in ICB data is stored in allocation descriptor
         * so sizes should match
         */
        if (inode->len_alloc != inode->size) {
            ret = udf_corrupt(sb, iloc, "embedded data length differs from the file size");
            goto out;
        }
        /* File in ICB has to fit in there... */
        if (inode->size > bs - udf_file_entry_alloc_offset(inode)) {
            ret = udf_corrupt(sb, iloc, "embedded data does not fit the block");
            goto out;
        }
    }

    switch (inode->file_type) {
    case ICBTAG_FILE_TYPE_DIRECTORY:
    case ICBTAG_FILE_TYPE_REALTIME:
    case ICBTAG_FILE_TYPE_REGULAR:
    case ICBTAG_FILE_TYPE_UNDEF:
    case ICBTAG_FILE_TYPE_VAT20:
    case ICBTAG_FILE_TYPE_FIFO:
    case ICBTAG_FILE_TYPE_SOCKET:
    case ICBTAG_FILE_TYPE_SYMLINK:
        break;
    case ICBTAG_FILE_TYPE_MAIN:
        udf_log(sb, AV_LOG_DEBUG, "METADATA FILE-----");
        break;
    case ICBTAG_FILE_TYPE_MIRROR:
        udf_log(sb, AV_LOG_DEBUG, "METADATA MIRROR FILE-----");
        break;
    case ICBTAG_FILE_TYPE_BITMAP:
        udf_log(sb, AV_LOG_DEBUG, "METADATA BITMAP FILE-----");
        break;
    case ICBTAG_FILE_TYPE_BLOCK:
    case ICBTAG_FILE_TYPE_CHAR:
        /* device nodes need a device specification attribute */
        if (udf_get_extendedattr(inode, 12, 1) < 0) {
            ret = udf_corrupt(sb, iloc, "device node without a device specification");
            goto out;
        }
        break;
    default:
        udf_log(sb, AV_LOG_WARNING, "inode at block %"PRIu32" has unknown file type %u",
                iloc.lbn, inode->file_type);
        ret = udf_corrupt(sb, iloc, "unknown file type");
        goto out;
    }
    (void)link_count;
    *out = inode;
    return 0;

out:
    udf_free_inode(inode);
    return ret;
}

/* udf_iget / udf_iget_special: the inode at ino, from the cache (keyed by
 * its physical block) or read */
static int udf_iget(UDFLinux *sb, LbAddr ino, int hidden, UDFInode **out)
{
    uint32_t block = udf_get_pblock(sb, ino.lbn, ino.part, 0);
    UDFInode *inode = icache_find(sb, block);
    int ret;

    if (inode) {
        if (inode->hidden != hidden)
            return udf_corrupt(sb, ino, "inode read as hidden and as visible");
        *out = inode;
        return 0;
    }
    if ((ret = udf_read_inode(sb, ino, hidden, &inode)) < 0)
        return ret;
    if ((ret = icache_insert(sb, block, inode)) < 0) {
        udf_free_inode(inode);
        return ret;
    }
    *out = inode;
    return 0;
}

/* a metadata file, which must use short allocation descriptors; NULL if not */
static UDFInode *udf_find_metadata_inode_efe(UDFLinux *sb, uint32_t meta_file_loc, uint16_t partition_ref)
{
    LbAddr addr = { meta_file_loc, partition_ref };
    UDFInode *metadata_fe;

    if (udf_iget(sb, addr, 1, &metadata_fe) < 0) {
        udf_log(sb, AV_LOG_WARNING, "metadata inode efe not found");
        return NULL;
    }
    if (metadata_fe->alloc_type != ICBTAG_FLAG_AD_SHORT) {
        udf_log(sb, AV_LOG_WARNING, "metadata inode efe does not have short allocation descriptors!");
        udf_corrupt(sb, addr, "metadata file without short allocation descriptors");
        return NULL;
    }
    return metadata_fe;
}

/* ---- super.c: mounting ---- */

static void udf_sb_free_partitions(UDFLinux *sb)
{
    for (int i = 0; i < sb->nb_partmaps; i++)
        for (int k = 0; k < 4; k++)
            av_freep(&sb->partmaps[i].spar_map[k]);
    av_freep(&sb->partmaps);
    sb->nb_partmaps = 0;
}

/*
 * Check VSD descriptor. Returns -1 in case we are at the end of volume
 * recognition area, 0 if the descriptor is valid but non-interesting, 1 if
 * we found one of NSR descriptors we are looking for.
 */
static int identify_vsd(const uint8_t *vsd)
{
    const uint8_t *id = vsd + 1;    /* stdIdent */

    if (!memcmp(id, "NSR02", 5) || !memcmp(id, "NSR03", 5))
        return 1;
    if (!memcmp(id, "CD001", 5) || !memcmp(id, "BEA01", 5) ||
        !memcmp(id, "BOOT2", 5) || !memcmp(id, "CDW02", 5))
        return 0;
    /* TEA01 or invalid id : end of volume recognition area */
    return -1;
}

/*
 * Check Volume Structure Descriptors (ECMA 167 2/9.1)
 * We also check any "CD-ROM Volume Descriptor Set" (ECMA 167 2/8.3.1)
 * @return   1 if NSR02 or NSR03 found,
 *          -1 if first sector read error, 0 otherwise
 */
static int udf_check_vsd(UDFLinux *sb)
{
    uint8_t bh[UDF_MAX_BLOCKSIZE];
    uint64_t sector = VSD_FIRST_SECTOR_OFFSET;
    uint64_t sectorsize = FFMAX(sb->blocksize, DISCIO_BLOCK_SIZE);
    int nsr = 0, read_any = 0;

    udf_log(sb, AV_LOG_DEBUG, "Starting at sector %u (%"PRIu32" byte sectors)",
            (unsigned)(sector >> sb->bits), sb->blocksize);
    for (; !nsr && sector < VSD_MAX_SECTOR_OFFSET; sector += sectorsize) {
        const uint8_t *vsd;

        /* Read a block */
        if (sb_bread(sb, (uint32_t)(sector >> sb->bits), bh) < 0)
            break;
        read_any = 1;

        vsd = bh + (sector & (sb->blocksize - 1));
        nsr = identify_vsd(vsd);
        /* Found NSR or end? */
        if (nsr)
            break;
        /*
         * Special handling for improperly formatted VRS (e.g., Win10)
         * where components are separated by 2048 bytes even though
         * sectors are 4K
         */
        if (sb->blocksize == 4096) {
            nsr = identify_vsd(vsd + 2048);
            /* Ignore unknown IDs... */
            if (nsr < 0)
                nsr = 0;
        }
    }

    if (nsr > 0)
        return 1;
    else if (!read_any && sector == VSD_FIRST_SECTOR_OFFSET)
        return -1;
    else
        return 0;
}

/* udf_verify_domain_identifier on a read-only mount: a domain that is not
 * OSTA UDF compliant, or is marked dirty or write-protected, only marks the
 * volume as not writable */
static void udf_verify_domain_identifier(UDFLinux *sb, const uint8_t *ident, const char *dname)
{
    int compliant = !memcmp(ident + 1, "*OSTA UDF Compliant", 19);
    int dirty     = ident[0] & 1;           /* ENTITYID_FLAGS_DIRTY */
    int protect   = ident[26] & 3;          /* domainFlags: hard / soft write protect */

    if (!compliant)
        udf_log(sb, AV_LOG_WARNING, "Not OSTA UDF compliant %s descriptor.", dname);
    else if (dirty)
        udf_log(sb, AV_LOG_WARNING, "Possibly not OSTA UDF compliant %s descriptor.", dname);
    if (!compliant || dirty || protect)
        sb->rw_incompat = 1;
}

/* udf_find_fileset + udf_load_fileset */
static int udf_find_fileset(UDFLinux *sb, LbAddr fileset)
{
    uint8_t bh[UDF_MAX_BLOCKSIZE];
    uint16_t ident;

    if (fileset.lbn == 0xFFFFFFFF && fileset.part == 0xFFFF)
        return udf_fail(sb, AVERROR_INVALIDDATA, "no file set location");

    if (udf_read_ptagged(sb, fileset, 0, bh, &ident) < 0)
        return udf_fail(sb, AVERROR_INVALIDDATA, "the file set descriptor could not be read");
    if (ident != TAG_IDENT_FSD)
        return udf_fail(sb, AVERROR_INVALIDDATA, "no file set descriptor at the file set location");

    udf_log(sb, AV_LOG_DEBUG, "Fileset at block=%"PRIu32", partition=%u", fileset.lbn, fileset.part);

    sb->partition = fileset.part;
    udf_verify_domain_identifier(sb, bh + 416, "file set");
    sb->rootdir.lbn  = AV_RL32(bh + 404);   /* rootDirectoryICB.extLocation */
    sb->rootdir.part = AV_RL16(bh + 408);
    udf_log(sb, AV_LOG_DEBUG, "Rootdir at block=%"PRIu32", partition=%u", sb->rootdir.lbn, sb->rootdir.part);
    return 0;
}

/*
 * Load primary Volume Descriptor
 *
 * Return <0 on error, 0 on success. UDF_TRY_NEXT means next sequence
 * should be tried.
 */
static int udf_load_pvoldesc(UDFLinux *sb, uint32_t block)
{
    uint8_t bh[UDF_MAX_BLOCKSIZE];
    uint16_t ident;

    if (udf_read_tagged(sb, block, block, bh, &ident) < 0)
        return UDF_TRY_NEXT;
    if (ident != TAG_IDENT_PVD)
        return udf_fail(sb, AVERROR_INVALIDDATA, "the primary volume descriptor at block %"PRIu32" is unreadable", block);

    udf_log(sb, AV_LOG_DEBUG, "recording time %04u/%02u/%02u %02u:%02u (%x)",
            AV_RL16(bh + 376 + 2), bh[376 + 4], bh[376 + 5], bh[376 + 6], bh[376 + 7], AV_RL16(bh + 376));
    if (!sb->pvd && !(sb->pvd = av_malloc(sb->blocksize)))
        return AVERROR(ENOMEM);
    memcpy(sb->pvd, bh, DISCIO_BLOCK_SIZE);
    return 0;
}

/* udf_load_metadata_files: the metadata file (else the mirror) and, if
 * recorded, the bitmap file (a missing bitmap is fine read-only) */
static int udf_load_metadata_files(UDFLinux *sb, int partition, int type1_index)
{
    PartMap *map = &sb->partmaps[partition];
    UDFInode *fe;

    map->phys_partition_ref = type1_index;

    /* metadata address */
    udf_log(sb, AV_LOG_DEBUG, "Metadata file location: block = %"PRIu32" part = %d",
            map->meta_file_loc, type1_index);

    fe = udf_find_metadata_inode_efe(sb, map->meta_file_loc, type1_index);
    if (!fe) {
        /* mirror file entry */
        udf_log(sb, AV_LOG_DEBUG, "Mirror metadata file location: block = %"PRIu32" part = %d",
                map->mirror_file_loc, type1_index);

        fe = udf_find_metadata_inode_efe(sb, map->mirror_file_loc, type1_index);
        if (!fe) {
            udf_log(sb, AV_LOG_WARNING, "Both metadata and mirror metadata inode efe can not found");
            return udf_fail(sb, AVERROR_INVALIDDATA, "neither the metadata file nor its mirror could be read");
        }
        map->mirror_fe = fe;
    } else
        map->metadata_fe = fe;

    /*
     * bitmap file entry
     * Note:
     * Load only if bitmap file location differs from 0xFFFFFFFF (DCN-5102)
     */
    if (map->bitmap_file_loc != 0xFFFFFFFF) {
        LbAddr addr = { map->bitmap_file_loc, type1_index };

        udf_log(sb, AV_LOG_DEBUG, "Bitmap file location: block = %"PRIu32" part = %d",
                addr.lbn, type1_index);
        if (udf_iget(sb, addr, 1, &fe) < 0)
            udf_log(sb, AV_LOG_WARNING,
                    "bitmap inode efe not found but it's ok since the disc is mounted read-only");
    }

    udf_log(sb, AV_LOG_DEBUG, "udf_load_metadata_files Ok");
    return 0;
}

/* check_partition_desc on a read-only mount: anything that cannot be
 * written marks the volume as not writable */
static void check_partition_desc(UDFLinux *sb, const uint8_t *p, const PartMap *map)
{
    uint32_t access = AV_RL32(p + 184);
    const uint8_t *contents = p + 25;       /* partitionContents.ident */
    /* partitionHeaderDesc in partitionContentsUse (offset 56) */
    uint32_t utable = AV_RL32(p + 56), umap = AV_RL32(p + 64);
    uint32_t ftable = AV_RL32(p + 80), fmap = AV_RL32(p + 88);

    switch (access) {
    case PD_ACCESS_TYPE_READ_ONLY:
    case PD_ACCESS_TYPE_WRITE_ONCE:
    case PD_ACCESS_TYPE_NONE:
        goto force_ro;
    }

    /* No Partition Header Descriptor? */
    if (memcmp(contents, "+NSR02", 7) && memcmp(contents, "+NSR03", 7))
        goto force_ro;

    /* No allocation info? */
    if (!utable && !umap && !ftable && !fmap)
        goto force_ro;

    /* We don't support blocks that require erasing before overwrite */
    if (ftable || fmap)
        goto force_ro;
    /* UDF 2.60: 2.3.3 - no mixing of tables & bitmaps, no VAT. */
    if (utable && umap)
        goto force_ro;

    if (map->ptype == PART_VIRTUAL15 || map->ptype == PART_VIRTUAL20 || map->ptype == PART_METADATA)
        goto force_ro;

    return;
force_ro:
    sb->rw_incompat = 1;
}

static int udf_fill_partdesc_info(UDFLinux *sb, const uint8_t *p, int p_index)
{
    PartMap *map = &sb->partmaps[p_index];
    uint32_t len  = AV_RL32(p + 192);   /* partitionLength (blocks) */
    uint32_t root = AV_RL32(p + 188);   /* partitionStartingLocation */

    if (root > UINT32_MAX - len) {
        udf_log(sb, AV_LOG_WARNING, "Partition %d has invalid location %"PRIu32" + %"PRIu32,
                p_index, root, len);
        return udf_fail(sb, AVERROR_INVALIDDATA, "partition %d has an invalid location", p_index);
    }
    map->len  = len;
    map->root = root;

    udf_log(sb, AV_LOG_DEBUG, "Partition (%d type %d) starts at physical %"PRIu32", block length %"PRIu32,
            p_index, map->ptype, root, len);

    check_partition_desc(sb, p, map);

    /*
     * Skip loading allocation info it we cannot ever write to the fs.
     * This is a correctness thing as we may have decided to force ro mount
     * to avoid allocation info we don't support.
     */
    if (sb->rw_incompat)
        return 0;

    if (AV_RL32(p + 56)) {     /* unallocSpaceTable.extLength */
        LbAddr loc = { AV_RL32(p + 60), p_index };
        UDFInode *inode;
        int ret;

        if ((ret = udf_iget(sb, loc, 1, &inode)) < 0) {
            udf_log(sb, AV_LOG_DEBUG, "cannot load unallocSpaceTable (part %d)", p_index);
            return ret;
        }
    }

    if (AV_RL32(p + 64)) {     /* unallocSpaceBitmap.extLength */
        /* Check whether math over bitmap won't overflow. */
        if (map->len > UINT32_MAX - (SIZEOF_SPACE_BITMAP << 3)) {
            udf_log(sb, AV_LOG_WARNING, "Partition %d is too long (%"PRIu32")", p_index, map->len);
            return udf_fail(sb, AVERROR_INVALIDDATA, "partition %d is too long", p_index);
        }
    }
    return 0;
}

/* udf_find_vat_block: the VAT file entry in the last recorded block or up
 * to three blocks before it */
static void udf_find_vat_block(UDFLinux *sb, int p_index, int type1_index, uint32_t start_block)
{
    uint32_t root = sb->partmaps[p_index].root;
    uint32_t vat_block;

    /*
     * VAT file entry is in the last recorded block. Some broken disks have
     * it a few blocks before so try a bit harder...
     */
    for (vat_block = start_block; vat_block >= root && vat_block >= start_block - 3; vat_block--) {
        LbAddr ino = { vat_block - root, type1_index };
        UDFInode *inode;

        if (!udf_iget(sb, ino, 1, &inode)) {
            sb->vat_inode = inode;
            break;
        }
        /* structural guard: do not wrap below block 0 */
        if (!vat_block)
            break;
    }
}

static int udf_load_vat(UDFLinux *sb, int p_index, int type1_index)
{
    PartMap *map = &sb->partmaps[p_index];
    uint64_t blocks = udf_nr_blocks(sb);
    const UDFInode *vat;

    udf_find_vat_block(sb, p_index, type1_index, sb->last_block);
    if (!sb->vat_inode && sb->last_block != blocks - 1) {
        udf_log(sb, AV_LOG_INFO,
                "Failed to read VAT inode from the last recorded block (%"PRIu32"), retrying with the last block of the device (%"PRIu64").",
                sb->last_block, blocks - 1);
        udf_find_vat_block(sb, p_index, type1_index, (uint32_t)(blocks - 1));
    }
    if (!(vat = sb->vat_inode))
        return udf_fail(sb, AVERROR_INVALIDDATA, "the VAT could not be read");

    if (map->ptype == PART_VIRTUAL15) {
        map->vat_start_offset = 0;
        if (vat->size < 36) {
            udf_log(sb, AV_LOG_WARNING, "Too short VAT inode size %"PRIu64, vat->size);
            return udf_fail(sb, AVERROR_INVALIDDATA, "the VAT could not be read");
        }
        map->vat_num_entries = (uint32_t)((vat->size - 36) >> 2);
    } else {
        uint8_t bh[UDF_MAX_BLOCKSIZE];
        const uint8_t *vat20;

        if (vat->alloc_type != ICBTAG_FLAG_AD_IN_ICB) {
            int ret = udf_bread(sb, vat, 0, bh);

            if (ret < 0)
                return ret;
            if (!ret)
                return udf_fail(sb, AVERROR_INVALIDDATA, "the VAT's first block is not recorded");
            vat20 = bh;
        } else {
            /* read from the start of i_data (the extended attribute area is
             * not skipped here, as upstream) */
            vat20 = vat->data;
        }

        map->vat_start_offset = AV_RL16(vat20);   /* lengthHeader */
        if (map->vat_start_offset > vat->size) {
            udf_log(sb, AV_LOG_WARNING, "Corrupted VAT header length %"PRIu32" (VAT inode size %"PRIu64")",
                    map->vat_start_offset, vat->size);
            return udf_fail(sb, AVERROR_INVALIDDATA, "the VAT could not be read");
        }
        map->vat_num_entries = (uint32_t)((vat->size - map->vat_start_offset) >> 2);
    }
    udf_log(sb, AV_LOG_DEBUG, "VAT with %"PRIu32" entries", map->vat_num_entries);
    return 0;
}

/*
 * Load partition descriptor block
 *
 * Returns <0 on error, 0 on success, UDF_TRY_NEXT is special - try next
 * descriptor sequence.
 */
static int udf_load_partdesc(UDFLinux *sb, uint32_t block)
{
    uint8_t bh[UDF_MAX_BLOCKSIZE];
    uint16_t ident, partition_number;
    int i, type1_idx, ret;

    if (udf_read_tagged(sb, block, block, bh, &ident) < 0)
        return UDF_TRY_NEXT;
    if (ident != TAG_IDENT_PD)
        return 0;

    partition_number = AV_RL16(bh + 22);

    /* First scan for TYPE1 and SPARABLE partitions */
    for (i = 0; i < sb->nb_partmaps; i++) {
        const PartMap *map = &sb->partmaps[i];
        udf_log(sb, AV_LOG_TRACE, "Searching map: (%u == %u)", map->partition_num, partition_number);
        if (map->partition_num == partition_number &&
            (map->ptype == PART_TYPE1 || map->ptype == PART_SPARABLE))
            break;
    }

    if (i >= sb->nb_partmaps) {
        udf_log(sb, AV_LOG_DEBUG, "Partition (%u) not found in partition map", partition_number);
        return 0;
    }

    if ((ret = udf_fill_partdesc_info(sb, bh, i)) < 0)
        return ret;

    /*
     * Now rescan for VIRTUAL or METADATA partitions when SPARABLE and
     * PHYSICAL partitions are already set up
     */
    type1_idx = i;
    for (i = 0; i < sb->nb_partmaps; i++) {
        const PartMap *map = &sb->partmaps[i];

        if (map->partition_num == partition_number &&
            (map->ptype == PART_VIRTUAL15 || map->ptype == PART_VIRTUAL20 || map->ptype == PART_METADATA))
            break;
    }

    if (i >= sb->nb_partmaps)
        return 0;

    if ((ret = udf_fill_partdesc_info(sb, bh, i)) < 0)
        return ret;

    if (sb->partmaps[i].ptype == PART_METADATA) {
        if ((ret = udf_load_metadata_files(sb, i, type1_idx)) < 0) {
            udf_log(sb, AV_LOG_WARNING, "error loading MetaData partition map %d", i);
            return ret;
        }
    } else {
        /*
         * If we have a partition with virtual map, we don't handle
         * writing to it (we overwrite blocks instead of relocating
         * them).
         */
        sb->rw_incompat = 1;
        if ((ret = udf_load_vat(sb, i, type1_idx)) < 0)
            return ret;
    }
    return 0;
}

/* a field of a partition map (gpm, len bytes of the map table from its
 * start), 0 when it lies past the table */
static uint16_t gpm_rl16(const uint8_t *gpm, unsigned len, unsigned at)
{
    return at + 2 <= len ? AV_RL16(gpm + at) : 0;
}

static uint32_t gpm_rl32(const uint8_t *gpm, unsigned len, unsigned at)
{
    return at + 4 <= len ? AV_RL32(gpm + at) : 0;
}

static int gpm_ident(const uint8_t *gpm, unsigned len, const char *id)
{
    size_t n = strlen(id);

    /* partIdent.ident: bytes 5-27 of the map */
    return len >= 28 && !memcmp(gpm + 5, id, n);
}

static int udf_load_sparable_map(UDFLinux *sb, PartMap *map, const uint8_t *spm, unsigned len)
{
    uint8_t st[UDF_MAX_BLOCKSIZE];
    uint16_t packet_len;
    unsigned n;

    map->ptype = PART_SPARABLE;
    packet_len = gpm_rl16(spm, len, 40);    /* packetLength */
    if (!packet_len || (packet_len & (packet_len - 1))) {
        udf_log(sb, AV_LOG_WARNING, "error loading logical volume descriptor: Invalid packet length %u",
                packet_len);
        return udf_fail(sb, AVERROR_INVALIDDATA, "invalid sparing packet length %u", packet_len);
    }
    map->packet_len = packet_len;
    n = len > 42 ? spm[42] : 0;             /* numSparingTables */
    if (n > 4) {
        udf_log(sb, AV_LOG_WARNING, "error loading logical volume descriptor: Too many sparing tables (%u)", n);
        return udf_fail(sb, AVERROR_INVALIDDATA, "too many sparing tables (%u)", n);
    }
    if (gpm_rl32(spm, len, 44) > sb->blocksize) {   /* sizeSparingTable */
        udf_log(sb, AV_LOG_WARNING, "error loading logical volume descriptor: Too big sparing table size (%"PRIu32")",
                gpm_rl32(spm, len, 44));
        return udf_fail(sb, AVERROR_INVALIDDATA, "sparing table too big");
    }

    for (unsigned i = 0; i < n; i++) {
        uint32_t loc = gpm_rl32(spm, len, 48 + 4 * i);   /* locSparingTable[i] */
        uint16_t ident;

        if (udf_read_tagged(sb, loc, loc, st, &ident) < 0)
            continue;

        if (ident != 0 || memcmp(st + 17, "*UDF Sparing Table", 18) ||
            SIZEOF_SPARING_TABLE + SIZEOF_SPARING_ENTRY * (uint64_t)AV_RL16(st + 48) > sb->blocksize)
            continue;

        if (!(map->spar_map[i] = av_malloc(sb->blocksize)))
            return AVERROR(ENOMEM);
        memcpy(map->spar_map[i], st, sb->blocksize);
    }
    return 0;
}

static int udf_lvid_valid(const UDFLinux *sb, const uint8_t *lvid)
{
    uint32_t parts = AV_RL32(lvid + 72), impuselen = AV_RL32(lvid + 76);

    if (parts >= sb->blocksize || impuselen >= sb->blocksize ||
        SIZEOF_LVID + (uint64_t)impuselen + 8ULL * parts > sb->blocksize)
        return 0;
    return 1;
}

/*
 * Find the prevailing Logical Volume Integrity Descriptor.
 */
static int udf_load_logicalvolint(UDFLinux *sb, uint32_t len, uint32_t loc)
{
    uint8_t bh[UDF_MAX_BLOCKSIZE], final_bh[UDF_MAX_BLOCKSIZE];
    int indirections = 0;

    while (++indirections <= UDF_MAX_LVID_NESTING) {
        uint16_t ident;
        int found = 0;

        while (len > 0 && !udf_read_tagged(sb, loc, loc, bh, &ident)) {
            if (ident != TAG_IDENT_LVID)
                break;

            memcpy(final_bh, bh, sb->blocksize);
            found = 1;

            len -= sb->blocksize;
            loc++;
        }

        if (!found)
            return 0;

        if (udf_lvid_valid(sb, final_bh)) {
            if (!sb->lvid && !(sb->lvid = av_malloc(sb->blocksize)))
                return AVERROR(ENOMEM);
            memcpy(sb->lvid, final_bh, sb->blocksize);
        } else {
            udf_log(sb, AV_LOG_WARNING, "Corrupted LVID (parts=%"PRIu32", impuselen=%"PRIu32"), ignoring.",
                    AV_RL32(final_bh + 72), AV_RL32(final_bh + 76));
        }

        if (!AV_RL32(final_bh + 32))    /* nextIntegrityExt.extLength */
            return 0;

        len = AV_RL32(final_bh + 32);
        loc = AV_RL32(final_bh + 36);
    }

    udf_log(sb, AV_LOG_WARNING, "Too many LVID indirections (max %u), ignoring.", UDF_MAX_LVID_NESTING);
    av_freep(&sb->lvid);
    return 0;
}

static int udf_load_logicalvol(UDFLinux *sb, uint32_t block, LbAddr *fileset)
{
    uint8_t bh[UDF_MAX_BLOCKSIZE];
    unsigned table_len, part_map_count, offset;
    uint16_t ident;
    int i, ret;

    if (udf_read_tagged(sb, block, block, bh, &ident) < 0)
        return UDF_TRY_NEXT;
    table_len = AV_RL32(bh + 264);  /* mapTableLength */
    if (table_len > sb->blocksize - SIZEOF_LVD) {
        udf_log(sb, AV_LOG_WARNING, "error loading logical volume descriptor: Partition table too long (%u > %u)",
                table_len, (unsigned)(sb->blocksize - SIZEOF_LVD));
        return udf_fail(sb, AVERROR_INVALIDDATA, "partition table too long (%u)", table_len);
    }

    udf_verify_domain_identifier(sb, bh + 216, "logical volume");

    part_map_count = AV_RL32(bh + 268);  /* numPartitionMaps */
    if (part_map_count > table_len / SIZEOF_GPM1) {
        udf_log(sb, AV_LOG_WARNING, "error loading logical volume descriptor: Too many partition maps (%u > %u)",
                part_map_count, table_len / SIZEOF_GPM1);
        return udf_fail(sb, AVERROR_INVALIDDATA, "too many partition maps (%u)", part_map_count);
    }
    udf_sb_free_partitions(sb);
    if (part_map_count && !(sb->partmaps = av_calloc(part_map_count, sizeof(*sb->partmaps))))
        return AVERROR(ENOMEM);
    sb->nb_partmaps = part_map_count;

    for (i = 0, offset = 0; i < sb->nb_partmaps && offset < table_len; i++) {
        PartMap *map = &sb->partmaps[i];
        /* the map's bytes are bounded by the map table */
        const uint8_t *gpm = bh + SIZEOF_LVD + offset;
        unsigned glen = table_len - offset;
        uint8_t type = gpm[0];
        unsigned map_len = glen > 1 ? gpm[1] : 0;    /* partitionMapLength */

        if (type == 1) {
            map->ptype = PART_TYPE1;
            map->partition_num = gpm_rl16(gpm, glen, 4);
        } else if (type == 2) {
            if (gpm_ident(gpm, glen, "*UDF Virtual Partition")) {
                uint16_t suf = gpm_rl16(gpm, glen, 28);
                map->ptype = suf < 0x0200 ? PART_VIRTUAL15 : PART_VIRTUAL20;
            } else if (gpm_ident(gpm, glen, "*UDF Sparable Partition")) {
                if ((ret = udf_load_sparable_map(sb, map, gpm, glen)) < 0)
                    return ret;
            } else if (gpm_ident(gpm, glen, "*UDF Metadata Partition")) {
                map->ptype = PART_METADATA;
                map->meta_file_loc   = gpm_rl32(gpm, glen, 40);
                map->mirror_file_loc = gpm_rl32(gpm, glen, 44);
                map->bitmap_file_loc = gpm_rl32(gpm, glen, 48);
                udf_log(sb, AV_LOG_DEBUG, "Metadata file loc=%"PRIu32", Mirror file loc=%"PRIu32", Bitmap file loc=%"PRIu32,
                        map->meta_file_loc, map->mirror_file_loc, map->bitmap_file_loc);
            } else {
                udf_log(sb, AV_LOG_DEBUG, "Unknown ident in partition map %d", i);
                offset += map_len;
                continue;
            }
            map->partition_num = gpm_rl16(gpm, glen, 38);
        }
        udf_log(sb, AV_LOG_DEBUG, "Partition (%d:%u) type %u", i, map->partition_num, type);
        offset += map_len;
    }

    fileset->lbn  = AV_RL32(bh + 252);   /* logicalVolContentsUse: long_ad extLocation */
    fileset->part = AV_RL16(bh + 256);
    udf_log(sb, AV_LOG_DEBUG, "FileSet found in LogicalVolDesc at block=%"PRIu32", partition=%u",
            fileset->lbn, fileset->part);

    if (!sb->lvd && !(sb->lvd = av_malloc(sb->blocksize)))
        return AVERROR(ENOMEM);
    memcpy(sb->lvd, bh, DISCIO_BLOCK_SIZE);
    if (AV_RL32(bh + 432) &&    /* integritySeqExt.extLength */
        (ret = udf_load_logicalvolint(sb, AV_RL32(bh + 432), AV_RL32(bh + 436))) < 0)
        return ret;

    /* We can't generate unique IDs without a valid LVID */
    if (!sb->lvid)
        sb->rw_incompat = 1;
    return 0;
}

typedef struct VdsRecord {
    uint32_t seq;
    uint32_t block;
} VdsRecord;

typedef struct PartDescRecord {
    VdsRecord rec;
    uint16_t  partnum;
} PartDescRecord;

/* the record of a descriptor of the sequence: highest sequence number wins */
static void vds_prevail(VdsRecord *curr, uint32_t vdsn, uint32_t block)
{
    if (vdsn >= curr->seq) {
        curr->seq   = vdsn;
        curr->block = block;
    }
}

/*
 * Process a main/reserve volume descriptor sequence.
 *   @block         First block of first extent of the sequence.
 *   @lastblock     Lastblock of first extent of the sequence.
 *   @fileset       There we store extent containing root fileset
 *
 * Returns <0 on error, 0 on success. UDF_TRY_NEXT is special - try next
 * descriptor sequence
 */
static int udf_process_sequence(UDFLinux *sb, uint32_t block, uint32_t lastblock, LbAddr *fileset)
{
    uint8_t bh[UDF_MAX_BLOCKSIZE];
    VdsRecord pvd = { 0 }, lvd = { 0 }, iuvd = { 0 }, usd = { 0 };
    PartDescRecord *pds = NULL;
    unsigned nb_pds = 0, indirections = 0;
    int done = 0, ret;
    uint16_t ident;

    /*
     * Read the main descriptor sequence and find which descriptors
     * are in it.
     */
    while (!done && block <= lastblock) {
        uint32_t vdsn;

        if (udf_read_tagged(sb, block, block, bh, &ident) < 0)
            break;

        /* Process each descriptor (ISO 13346 3/8.3-8.4) */
        vdsn = AV_RL32(bh + 16);    /* volDescSeqNum */
        switch (ident) {
        case TAG_IDENT_VDP: /* ISO 13346 3/10.3 */
            if (++indirections > UDF_MAX_TD_NESTING) {
                udf_log(sb, AV_LOG_WARNING, "too many Volume Descriptor Pointers (max %u supported)",
                        UDF_MAX_TD_NESTING);
                ret = udf_fail(sb, AVERROR_INVALIDDATA, "too many volume descriptor pointers");
                goto out;
            }
            block     = AV_RL32(bh + 24);   /* nextVolDescSeqExt.extLocation */
            lastblock = (AV_RL32(bh + 20) >> sb->bits) + block - 1;
            continue;
        case TAG_IDENT_PVD: /* ISO 13346 3/10.1 */
            vds_prevail(&pvd, vdsn, block);
            break;
        case TAG_IDENT_IUVD: /* ISO 13346 3/10.4 */
            vds_prevail(&iuvd, vdsn, block);
            break;
        case TAG_IDENT_LVD: /* ISO 13346 3/10.6 */
            vds_prevail(&lvd, vdsn, block);
            break;
        case TAG_IDENT_USD: /* ISO 13346 3/10.8 */
            vds_prevail(&usd, vdsn, block);
            break;
        case TAG_IDENT_PD: { /* ISO 13346 3/10.5 */
            uint16_t partnum = AV_RL16(bh + 22);
            unsigned i;

            for (i = 0; i < nb_pds; i++)
                if (pds[i].partnum == partnum)
                    break;
            if (i == nb_pds) {
                PartDescRecord *n = av_realloc_array(pds, nb_pds + 1, sizeof(*pds));
                if (!n) {
                    ret = AVERROR(ENOMEM);
                    goto out;
                }
                pds = n;
                memset(&pds[nb_pds], 0, sizeof(*pds));
                pds[nb_pds++].partnum = partnum;
            }
            vds_prevail(&pds[i].rec, vdsn, block);
            break;
        }
        case TAG_IDENT_TD: /* ISO 13346 3/10.9 */
            done = 1;
            break;
        }
        block++;
    }
    /*
     * Now read interesting descriptors again and process them
     * in a suitable order
     */
    if (!pvd.block) {
        udf_log(sb, AV_LOG_WARNING, "Primary Volume Descriptor not found!");
        udf_fail(sb, AVERROR_INVALIDDATA, "no primary volume descriptor");
        ret = UDF_TRY_NEXT;
        goto out;
    }
    if ((ret = udf_load_pvoldesc(sb, pvd.block)))
        goto out;

    /* kept for the UDF revision of the volume */
    av_freep(&sb->iuvd);
    if (iuvd.block && !udf_read_tagged(sb, iuvd.block, iuvd.block, bh, &ident)) {
        if (!(sb->iuvd = av_malloc(DISCIO_BLOCK_SIZE))) {
            ret = AVERROR(ENOMEM);
            goto out;
        }
        memcpy(sb->iuvd, bh, DISCIO_BLOCK_SIZE);
    }

    if (lvd.block && (ret = udf_load_logicalvol(sb, lvd.block, fileset)))
        goto out;

    /* Now handle prevailing Partition Descriptors */
    for (unsigned i = 0; i < nb_pds; i++)
        if ((ret = udf_load_partdesc(sb, pds[i].rec.block)))
            goto out;
    ret = 0;
out:
    av_free(pds);
    return ret;
}

/*
 * Load Volume Descriptor Sequence described by anchor in bh
 *
 * Returns <0 on error, 0 on success
 */
static int udf_load_sequence(UDFLinux *sb, const uint8_t *anchor, LbAddr *fileset)
{
    uint32_t main_s, main_e, reserve_s, reserve_e;
    int ret;

    /* Locate the main sequence */
    main_s = AV_RL32(anchor + 20);
    main_e = (AV_RL32(anchor + 16) >> sb->bits) + main_s - 1;

    /* Locate the reserve sequence */
    reserve_s = AV_RL32(anchor + 28);
    reserve_e = (AV_RL32(anchor + 24) >> sb->bits) + reserve_s - 1;

    /* Process the main & reserve sequences */
    /* responsible for finding the PartitionDesc(s) */
    ret = udf_process_sequence(sb, main_s, main_e, fileset);
    if (ret != UDF_TRY_NEXT)
        return ret;
    udf_log(sb, AV_LOG_WARNING, "main volume descriptor sequence unusable; reading the reserve sequence");
    udf_sb_free_partitions(sb);
    ret = udf_process_sequence(sb, reserve_s, reserve_e, fileset);
    if (ret) {
        udf_sb_free_partitions(sb);
        /* No sequence was OK, return -EIO */
        if (ret == UDF_TRY_NEXT)
            ret = udf_fail(sb, AVERROR_INVALIDDATA,
                           "the volume descriptor sequence could not be read (main and reserve copy)");
    }
    return ret;
}

/*
 * Check whether there is an anchor block in the given block and
 * load Volume Descriptor Sequence if so.
 *
 * Returns <0 on error, 0 on success, UDF_TRY_NEXT is special - try next
 * anchor block
 */
static int udf_check_anchor_block(UDFLinux *sb, uint32_t block, LbAddr *fileset)
{
    uint8_t bh[UDF_MAX_BLOCKSIZE];
    uint16_t ident;

    if (udf_read_tagged(sb, block, block, bh, &ident) < 0)
        return UDF_TRY_NEXT;
    if (ident != TAG_IDENT_AVDP)
        return UDF_TRY_NEXT;
    udf_log(sb, AV_LOG_DEBUG, "anchor volume descriptor pointer at block %"PRIu32, block);
    return udf_load_sequence(sb, bh, fileset);
}

/*
 * Search for an anchor volume descriptor pointer.
 *
 * Returns < 0 on error, 0 on success. UDF_TRY_NEXT is special - try next set
 * of anchors.
 */
static int udf_scan_anchors(UDFLinux *sb, uint32_t *lastblock, LbAddr *fileset)
{
    uint32_t last[6];
    int last_count = 0, ret;

    /*
     * according to spec, anchor is in either:
     *     block 256
     *     lastblock-256
     *     lastblock
     *  however, if the disc isn't closed, it could be 512.
     */
    ret = udf_check_anchor_block(sb, 256, fileset);
    if (ret != UDF_TRY_NEXT)
        return ret;
    /*
     * The trouble is which block is the last one. Drives often misreport
     * this so we try various possibilities.
     */
    last[last_count++] = *lastblock;
    if (*lastblock >= 1)
        last[last_count++] = *lastblock - 1;
    last[last_count++] = *lastblock + 1;
    if (*lastblock >= 2)
        last[last_count++] = *lastblock - 2;
    if (*lastblock >= 150)
        last[last_count++] = *lastblock - 150;
    if (*lastblock >= 152)
        last[last_count++] = *lastblock - 152;

    for (int i = 0; i < last_count; i++) {
        if (last[i] >= udf_nr_blocks(sb))
            continue;
        ret = udf_check_anchor_block(sb, last[i], fileset);
        if (ret != UDF_TRY_NEXT) {
            if (!ret)
                *lastblock = last[i];
            return ret;
        }
        if (last[i] < 256)
            continue;
        ret = udf_check_anchor_block(sb, last[i] - 256, fileset);
        if (ret != UDF_TRY_NEXT) {
            if (!ret)
                *lastblock = last[i];
            return ret;
        }
    }

    /* Finally try block 512 in case media is open */
    return udf_check_anchor_block(sb, 512, fileset);
}

/*
 * Check Volume Structure Descriptor, find Anchor block and load Volume
 * Descriptor Sequence.
 *
 * Returns < 0 on error, 0 on success.
 */
static int udf_load_vrs(UDFLinux *sb, LbAddr *fileset)
{
    int nsr, ret;

    sb->last_block = 0;
    /* Check that it is NSR02 compliant */
    nsr = udf_check_vsd(sb);
    if (!nsr) {
        udf_log(sb, AV_LOG_DEBUG, "No VRS found");
        return udf_fail(sb, AVERROR_INVALIDDATA, "no volume recognition sequence (NSR02 / NSR03)");
    }
    if (nsr == -1)
        udf_log(sb, AV_LOG_DEBUG, "Failed to read sector at offset %d. Assuming open disc. Skipping validity check",
                VSD_FIRST_SECTOR_OFFSET);
    sb->last_block = udf_get_last_block(sb);

    /* Look for anchor block and load Volume Descriptor Sequence */
    ret = udf_scan_anchors(sb, &sb->last_block, fileset);
    if (ret == UDF_TRY_NEXT) {
        udf_log(sb, AV_LOG_DEBUG, "No anchor found");
        return udf_fail(sb, AVERROR_INVALIDDATA, "no UDF anchor volume descriptor pointer found");
    }
    return ret;
}

static int udf_fill_super(UDFLinux *sb)
{
    static const uint32_t blocksizes[] = { 2048, 4096 };
    LbAddr fileset = { 0xFFFFFFFF, 0xFFFF };
    UDFInode *root;
    int ret = AVERROR_INVALIDDATA;

    for (int k = 0; k < FF_ARRAY_ELEMS(blocksizes); k++) {
        sb->blocksize = blocksizes[k];
        sb->bits      = blocksizes[k] == 2048 ? 11 : 12;
        av_freep(&sb->lvid);
        udf_sb_free_partitions(sb);
        sb->vat_inode = NULL;
        icache_clear(sb);
        /* the descriptor copies are sized for the block */
        av_freep(&sb->pvd);
        av_freep(&sb->lvd);
        av_freep(&sb->iuvd);
        ret = udf_load_vrs(sb, &fileset);
        if (!ret)
            break;
        if (ret == AVERROR(ENOMEM))
            return ret;
        udf_log(sb, AV_LOG_DEBUG, "Scanning with blocksize %"PRIu32" failed: %s", sb->blocksize, sb->why);
    }
    if (ret < 0)
        return ret;

    udf_log(sb, AV_LOG_DEBUG, "Lastblock=%"PRIu32, sb->last_block);

    if (sb->lvid) {
        /* logicalVolIntegrityDescImpUse after the two tables */
        uint64_t at = SIZEOF_LVID + 8ULL * AV_RL32(sb->lvid + 72);
        uint16_t min_read;

        if (at + 44 > sb->blocksize)
            return udf_fail(sb, AVERROR_INVALIDDATA, "integrity descriptor implementation use missing");
        min_read = AV_RL16(sb->lvid + at + 40);   /* minUDFReadRev */
        if (min_read > UDF_MAX_READ_VERSION) {
            udf_log(sb, AV_LOG_WARNING, "minUDFReadRev=%x (max is %x)", min_read, UDF_MAX_READ_VERSION);
            return udf_fail(sb, AVERROR_INVALIDDATA, "UDF read revision 0x%04x too new", min_read);
        }
    }

    if (!sb->nb_partmaps) {
        udf_log(sb, AV_LOG_WARNING, "No partition found (2)");
        return udf_fail(sb, AVERROR_INVALIDDATA, "no partition found");
    }

    if ((ret = udf_find_fileset(sb, fileset)) < 0) {
        udf_log(sb, AV_LOG_WARNING, "No fileset found");
        return ret;
    }

    if ((ret = udf_iget(sb, sb->rootdir, 0, &root)) < 0) {
        udf_log(sb, AV_LOG_WARNING, "Error in udf_iget, block=%"PRIu32", partition=%u",
                sb->rootdir.lbn, sb->rootdir.part);
        return ret;
    }
    return 0;
}

/* ---- unicode.c (no NLS character set: UTF-8 output) ---- */

#define PLANE_SIZE          0x10000
#define UNICODE_MAX         0x10ffff
#define SURROGATE_MASK      0xfffff800
#define SURROGATE_PAIR      0x0000d800
#define SURROGATE_LOW       0x00000400
#define SURROGATE_CHAR_BITS 10
#define SURROGATE_CHAR_MASK ((1 << SURROGATE_CHAR_BITS) - 1)

#define ILLEGAL_CHAR_MARK   '_'
#define EXT_MARK            '.'
#define CRC_MARK            '#'
#define EXT_SIZE            5
/* Number of chars we need to store generated CRC to make filename unique */
#define CRC_LEN             5
#define NLS_MAX_CHARSET_SIZE 6

/* fs/nls/nls_base.c utf32_to_utf8: the UTF-8 bytes of u (at most maxout),
 * or -1 for a value that is not a Unicode scalar or does not fit */
static int utf32_to_utf8(uint32_t u, uint8_t *s, int maxout)
{
    int n = u < 0x80 ? 1 : u < 0x800 ? 2 : u < 0x10000 ? 3 : 4;

    if (u > UNICODE_MAX || (u & SURROGATE_MASK) == SURROGATE_PAIR)
        return -1;
    if (maxout < n)
        return -1;
    switch (n) {
    case 1:
        s[0] = u;
        break;
    case 2:
        s[0] = 0xc0 | u >> 6;
        s[1] = 0x80 | (u & 0x3f);
        break;
    case 3:
        s[0] = 0xe0 | u >> 12;
        s[1] = 0x80 | ((u >> 6) & 0x3f);
        s[2] = 0x80 | (u & 0x3f);
        break;
    default:
        s[0] = 0xf0 | u >> 18;
        s[1] = 0x80 | ((u >> 12) & 0x3f);
        s[2] = 0x80 | ((u >> 6) & 0x3f);
        s[3] = 0x80 | (u & 0x3f);
        break;
    }
    return n;
}

static int get_utf16_char(const uint8_t *str_i, int str_i_max_len, int str_i_idx, int u_ch, uint32_t *ret)
{
    uint32_t c;
    int start_idx = str_i_idx;

    /* Expand OSTA compressed Unicode to Unicode */
    c = str_i[str_i_idx++];
    if (u_ch > 1)
        c = (c << 8) | str_i[str_i_idx++];
    if ((c & SURROGATE_MASK) == SURROGATE_PAIR) {
        uint32_t next;

        /* Trailing surrogate char */
        if (str_i_idx >= str_i_max_len) {
            c = UNICODE_MAX + 1;
            goto out;
        }

        /* Low surrogate must follow the high one... */
        if (c & SURROGATE_LOW) {
            c = UNICODE_MAX + 1;
            goto out;
        }

        next  = (uint32_t)str_i[str_i_idx++] << 8;
        next |= str_i[str_i_idx++];
        if ((next & SURROGATE_MASK) != SURROGATE_PAIR || !(next & SURROGATE_LOW)) {
            c = UNICODE_MAX + 1;
            goto out;
        }

        c = PLANE_SIZE + ((c & SURROGATE_CHAR_MASK) << SURROGATE_CHAR_BITS) + (next & SURROGATE_CHAR_MASK);
    }
out:
    *ret = c;
    return str_i_idx - start_idx;
}

static int udf_name_conv_char(uint8_t *str_o, int str_o_max_len, int *str_o_idx,
                              const uint8_t *str_i, int str_i_max_len, int *str_i_idx,
                              int u_ch, int *needsCRC, int translate)
{
    uint32_t c = 0;
    int illChar = 0;
    int len, gotch = 0;

    while (!gotch && *str_i_idx < str_i_max_len) {
        if (*str_o_idx >= str_o_max_len) {
            *needsCRC = 1;
            return gotch;
        }

        len = get_utf16_char(str_i, str_i_max_len, *str_i_idx, u_ch, &c);
        /* These chars cannot be converted. Replace them. */
        if (c == 0 || c > UNICODE_MAX || (translate && c == '/')) {
            illChar = 1;
            if (!translate)
                gotch = 1;
        } else if (illChar)
            break;
        else
            gotch = 1;
        *str_i_idx += len;
    }
    if (illChar) {
        *needsCRC = 1;
        c = ILLEGAL_CHAR_MARK;
        gotch = 1;
    }
    if (gotch) {
        len = utf32_to_utf8(c, &str_o[*str_o_idx], str_o_max_len - *str_o_idx);
        /* Valid character? */
        if (len >= 0) {
            *str_o_idx += len;
        } else {
            /* -ENAMETOOLONG */
            *needsCRC = 1;
            gotch = 0;
        }
    }
    return gotch;
}

/* Returns the length of the name in str_o, or -1 (unknown compression ID or
 * an odd length). */
static int udf_name_from_CS0(UDFLinux *sb, uint8_t *str_o, int str_max_len,
                             const uint8_t *ocu, int ocu_len, int translate)
{
    uint32_t c;
    uint8_t cmp_id;
    int idx, len;
    int u_ch;
    int needsCRC = 0;
    int ext_i_len, ext_max_len;
    int str_o_len = 0;      /* Length of resulting output */
    int ext_o_len = 0;      /* Extension output length */
    int ext_crc_len = 0;    /* Extension output length if used with CRC */
    int i_ext = -1;         /* Extension position in input buffer */
    int o_crc = 0;          /* Rightmost possible output pos for CRC+ext */
    unsigned short valueCRC;
    uint8_t ext[EXT_SIZE * NLS_MAX_CHARSET_SIZE + 1];
    char crc[CRC_LEN + 1];

    if (str_max_len <= 0)
        return 0;

    if (ocu_len == 0) {
        memset(str_o, 0, str_max_len);
        return 0;
    }

    cmp_id = ocu[0];
    if (cmp_id != 8 && cmp_id != 16) {
        memset(str_o, 0, str_max_len);
        udf_log(sb, AV_LOG_DEBUG, "unknown compression code (%u)", cmp_id);
        return -1;
    }
    u_ch = cmp_id >> 3;

    ocu++;
    ocu_len--;

    if (ocu_len % u_ch) {
        udf_log(sb, AV_LOG_DEBUG, "incorrect filename length (%d)", ocu_len + 1);
        return -1;
    }

    if (translate) {
        /* Look for extension */
        for (idx = ocu_len - u_ch, ext_i_len = 0;
             (idx >= 0) && (ext_i_len < EXT_SIZE);
             idx -= u_ch, ext_i_len++) {
            c = ocu[idx];
            if (u_ch > 1)
                c = (c << 8) | ocu[idx + 1];

            if (c == EXT_MARK) {
                if (ext_i_len)
                    i_ext = idx;
                break;
            }
        }
        if (i_ext >= 0) {
            /* Convert extension */
            ext_max_len = FFMIN((int)sizeof(ext), str_max_len);
            ext[ext_o_len++] = EXT_MARK;
            idx = i_ext + u_ch;
            while (udf_name_conv_char(ext, ext_max_len, &ext_o_len,
                                      ocu, ocu_len, &idx,
                                      u_ch, &needsCRC, translate)) {
                if ((ext_o_len + CRC_LEN) < str_max_len)
                    ext_crc_len = ext_o_len;
            }
        }
    }

    idx = 0;
    while (1) {
        if (translate && (idx == i_ext)) {
            if (str_o_len > (str_max_len - ext_o_len))
                needsCRC = 1;
            break;
        }

        if (!udf_name_conv_char(str_o, str_max_len, &str_o_len,
                                ocu, ocu_len, &idx,
                                u_ch, &needsCRC, translate))
            break;

        if (translate &&
            (str_o_len <= (str_max_len - ext_o_len - CRC_LEN)))
            o_crc = str_o_len;
    }

    if (translate) {
        if (str_o_len > 0 && str_o_len <= 2 && str_o[0] == '.' &&
            (str_o_len == 1 || str_o[1] == '.'))
            needsCRC = 1;
        if (needsCRC) {
            str_o_len = o_crc;
            valueCRC = crc_itu_t(0, ocu, ocu_len);
            snprintf(crc, sizeof(crc), "%c%04X", CRC_MARK, valueCRC);
            len = FFMIN(CRC_LEN, str_max_len - str_o_len);
            memcpy(&str_o[str_o_len], crc, len);
            str_o_len += len;
            ext_o_len = ext_crc_len;
        }
        if (ext_o_len > 0) {
            memcpy(&str_o[str_o_len], ext, ext_o_len);
            str_o_len += ext_o_len;
        }
    }

    return str_o_len;
}

/*
 * Convert CS0 dstring to output charset. Warning: This function may truncate
 * input string if it is too long as it is used for informational strings only
 * and it is better to truncate the string than to refuse mounting a media.
 */
static int udf_dstrCS0toChar(UDFLinux *sb, uint8_t *utf_o, int o_len, const uint8_t *ocu_i, int i_len)
{
    int s_len = 0;

    if (i_len > 0) {
        s_len = ocu_i[i_len - 1];
        if (s_len >= i_len) {
            udf_log(sb, AV_LOG_DEBUG, "incorrect dstring lengths (%d/%d), truncating", s_len, i_len);
            s_len = i_len - 1;
            /* 2-byte encoding? Need to round properly... */
            if (ocu_i[0] == 16)
                s_len -= (s_len - 1) & 2;
        }
    }

    return udf_name_from_CS0(sb, utf_o, o_len, ocu_i, s_len, 0);
}

/* a file identifier as a name (dlen bytes of room); -1 when it cannot be
 * converted or converts to nothing */
static int udf_get_filename(UDFLinux *sb, const uint8_t *sname, int slen, uint8_t *dname, int dlen)
{
    int ret;

    if (!slen)
        return -1;

    if (dlen <= 0)
        return 0;

    ret = udf_name_from_CS0(sb, dname, dlen, sname, slen, 1);
    /* Zero length filename isn't valid... */
    if (ret == 0)
        ret = -1;
    return ret;
}

/* ---- directory.c: the file identifier iterator ---- */

/* struct udf_fileident_iter */
typedef struct FiIter {
    const UDFInode *dir;
    uint64_t       pos;
    ExtentPosition epos;
    LbAddr         eloc;
    uint32_t       elen;
    uint64_t       loffset;
    uint8_t       *bh[2];
    uint8_t        fi[SIZEOF_FID];  /* the current file identifier descriptor (fixed part) */
    uint8_t        name[256];       /* its name bytes */
    int            name_len;        /* -1 at the end of the directory */
} FiIter;

/* udf_dir_entry_len */
static uint32_t udf_dir_entry_len(const uint8_t *fi)
{
    return (SIZEOF_FID + AV_RL16(fi + 36) + fi[19] + UDF_NAME_PAD - 1) & ~(uint32_t)(UDF_NAME_PAD - 1);
}

static void udf_fiiter_release(FiIter *iter)
{
    av_freep(&iter->bh[0]);
    av_freep(&iter->bh[1]);
    epos_release(&iter->epos);
}

static int udf_verify_fi(UDFLinux *sb, const FiIter *iter)
{
    const uint8_t *fi = iter->fi;
    uint32_t len;

    if (AV_RL16(fi) != TAG_IDENT_FID)
        return udf_dir_corrupt(sb, iter->dir, "entry with an incorrect tag");
    len = udf_dir_entry_len(fi);
    if (AV_RL16(fi + 36) & 3)
        return udf_dir_corrupt(sb, iter->dir, "entry with an unaligned implementation-use length");
    /*
     * This is in fact allowed by the spec due to long impUse field but
     * we don't support it. If there is real media with this large impUse
     * field, support can be added.
     */
    if (len > sb->blocksize)
        return udf_dir_corrupt(sb, iter->dir, "entry longer than a block");
    if (iter->pos + len > iter->dir->size)
        return udf_dir_corrupt(sb, iter->dir, "entry past the directory size");
    if (len != SIZEOF_TAG + (uint32_t)AV_RL16(fi + 10))
        return udf_dir_corrupt(sb, iter->dir, "entry whose CRC length does not match its length");
    return 0;
}

static int udf_copy_fi(UDFLinux *sb, FiIter *iter)
{
    uint32_t blksize = sb->blocksize;
    uint32_t off, len, nameoff, l_fi;
    int err;

    /* Skip copying when we are at EOF */
    if (iter->pos >= iter->dir->size) {
        iter->name_len = -1;
        return 0;
    }
    if (iter->dir->size < iter->pos + SIZEOF_FID)
        return udf_dir_corrupt(sb, iter->dir, "entry straddling the end of the directory");

    if (iter->dir->alloc_type == ICBTAG_FLAG_AD_IN_ICB) {
        uint64_t base = (uint64_t)iter->dir->len_eattr + iter->pos;

        if (base + SIZEOF_FID > iter->dir->data_len)
            return udf_dir_corrupt(sb, iter->dir, "embedded entry past the block");
        memcpy(iter->fi, iter->dir->data + base, SIZEOF_FID);
        if ((err = udf_verify_fi(sb, iter)) < 0)
            return err;
        nameoff = base + SIZEOF_FID + AV_RL16(iter->fi + 36);
        l_fi = iter->fi[19];
        if ((uint64_t)nameoff + l_fi > iter->dir->data_len)
            return udf_dir_corrupt(sb, iter->dir, "embedded name past the block");
        memcpy(iter->name, iter->dir->data + nameoff, l_fi);
        iter->name_len = l_fi;
        return 0;
    }

    if (!iter->bh[0])
        return udf_dir_corrupt(sb, iter->dir, "no directory block");
    off = iter->pos & (blksize - 1);
    len = FFMIN(SIZEOF_FID, blksize - off);
    memcpy(iter->fi, iter->bh[0] + off, len);
    if (len < SIZEOF_FID) {
        if (!iter->bh[1])
            return udf_dir_corrupt(sb, iter->dir, "no next directory block");
        memcpy(iter->fi + len, iter->bh[1], SIZEOF_FID - len);
    }
    if ((err = udf_verify_fi(sb, iter)) < 0)
        return err;

    /* Handle directory entry name */
    nameoff = off + SIZEOF_FID + AV_RL16(iter->fi + 36);
    l_fi = iter->fi[19];
    if (off + udf_dir_entry_len(iter->fi) <= blksize) {
        memcpy(iter->name, iter->bh[0] + nameoff, l_fi);
    } else if (nameoff >= blksize) {
        if (!iter->bh[1])
            return udf_dir_corrupt(sb, iter->dir, "no next directory block");
        memcpy(iter->name, iter->bh[1] + (nameoff - blksize), l_fi);
    } else {
        if (!iter->bh[1])
            return udf_dir_corrupt(sb, iter->dir, "no next directory block");
        len = blksize - nameoff;
        memcpy(iter->name, iter->bh[0] + nameoff, len);
        memcpy(iter->name + len, iter->bh[1], l_fi - len);
    }
    iter->name_len = l_fi;
    return 0;
}

static int udf_fiiter_bread_blk(UDFLinux *sb, FiIter *iter, uint8_t **bh)
{
    uint32_t blk = udf_get_pblock(sb, iter->eloc.lbn, iter->eloc.part, (uint32_t)iter->loffset);
    int ret;

    if (!*bh && !(*bh = av_malloc(sb->blocksize)))
        return AVERROR(ENOMEM);
    if ((ret = sb_bread(sb, blk, *bh)) < 0) {
        av_freep(bh);
        return udf_fail(sb, ret, "reading directory block %"PRIu32" failed", blk);
    }
    return 0;
}

/*
 * Updates loffset to point to next directory block; eloc, elen & epos are
 * updated if we need to traverse to the next extent as well.
 */
static int udf_fiiter_advance_blk(UDFLinux *sb, FiIter *iter)
{
    Aext a;
    int err;

    iter->loffset++;
    if (iter->loffset < (iter->elen + (uint64_t)sb->blocksize - 1) / sb->blocksize)
        return 0;

    iter->loffset = 0;
    err = udf_next_aext(sb, iter->dir, &iter->epos, &a, 1);
    if (err < 0)
        return err;
    else if (err == 0 || a.etype != EXT_RECORDED_ALLOCATED) {
        if (iter->pos == iter->dir->size) {
            iter->elen = 0;
            return 0;
        }
        return udf_dir_corrupt(sb, iter->dir, "extent after the position not allocated");
    }
    iter->eloc = a.eloc;
    iter->elen = a.elen;
    return 0;
}

static int udf_fiiter_load_bhs(UDFLinux *sb, FiIter *iter)
{
    uint32_t blksize = sb->blocksize;
    uint32_t off = iter->pos & (blksize - 1);
    int err;

    /* Is there any further extent we can map from? */
    if (!iter->bh[0] && iter->elen) {
        if ((err = udf_fiiter_bread_blk(sb, iter, &iter->bh[0])) < 0)
            return err;
    }
    /* There's no next block so we are done */
    if (iter->pos >= iter->dir->size)
        return 0;
    /* Need to fetch next block as well? */
    if (off + SIZEOF_FID > blksize)
        goto fetch_next;
    if (!iter->bh[0])
        return udf_dir_corrupt(sb, iter->dir, "no directory block");
    /* Need to fetch next block to get name? */
    if (off + udf_dir_entry_len(iter->bh[0] + off) > blksize) {
fetch_next:
        if ((err = udf_fiiter_advance_blk(sb, iter)) < 0)
            return err;
        if ((err = udf_fiiter_bread_blk(sb, iter, &iter->bh[1])) < 0)
            return err;
    }
    return 0;
}

/* an iterator at pos of directory dir (release it with udf_fiiter_release,
 * also after an error) */
static int udf_fiiter_init(UDFLinux *sb, FiIter *iter, const UDFInode *dir, uint64_t pos)
{
    uint64_t lbcount = 0, bcount;
    int err, found = 0;
    Aext a;

    memset(iter, 0, sizeof(*iter));
    iter->dir      = dir;
    iter->pos      = pos;
    iter->name_len = -1;

    if (dir->alloc_type == ICBTAG_FLAG_AD_IN_ICB)
        return udf_copy_fi(sb, iter);

    /* inode_bmap, keeping the extent position for the walk */
    bcount = (pos >> sb->bits) << sb->bits;
    iter->epos.block = dir->location;
    while (1) {
        err = udf_next_aext(sb, dir, &iter->epos, &a, 1);
        if (err < 0)
            return err;
        if (!err)
            break;
        lbcount += a.elen;
        if (lbcount > bcount) {
            found = 1;
            break;
        }
    }
    if (!found || a.etype != EXT_RECORDED_ALLOCATED) {
        if (pos == dir->size)
            return 0;
        return udf_dir_corrupt(sb, dir, "position not allocated");
    }
    iter->eloc    = a.eloc;
    iter->elen    = a.elen;
    iter->loffset = (bcount + a.elen - lbcount) >> sb->bits;
    if ((err = udf_fiiter_load_bhs(sb, iter)) < 0)
        return err;
    return udf_copy_fi(sb, iter);
}

static int udf_fiiter_advance(UDFLinux *sb, FiIter *iter)
{
    uint32_t blksize = sb->blocksize;
    uint32_t oldoff = iter->pos & (blksize - 1);
    uint32_t len = udf_dir_entry_len(iter->fi);
    int err;

    iter->pos += len;
    if (iter->dir->alloc_type != ICBTAG_FLAG_AD_IN_ICB) {
        if (oldoff + len >= blksize) {
            av_freep(&iter->bh[0]);
            /* Next block already loaded? */
            if (iter->bh[1]) {
                iter->bh[0] = iter->bh[1];
                iter->bh[1] = NULL;
            } else {
                if ((err = udf_fiiter_advance_blk(sb, iter)) < 0)
                    return err;
            }
        }
        if ((err = udf_fiiter_load_bhs(sb, iter)) < 0)
            return err;
    }
    return udf_copy_fi(sb, iter);
}

static LbAddr fiiter_icb(const FiIter *iter)
{
    LbAddr l = { AV_RL32(iter->fi + 24), AV_RL16(iter->fi + 28) };   /* icb.extLocation */
    return l;
}

/* ---- namei.c / dir.c: lookup and listing (default mount options) ---- */

/*
 * udf_fiiter_find_entry: the ICB of the FIRST entry named name ("..": the
 * parent entry). A name that cannot be converted fails the lookup.
 * Returns 1 (*loc set), 0 when there is no such entry, or a negative
 * AVERROR code.
 */
static int udf_fiiter_find_entry(UDFLinux *sb, const UDFInode *dir, const uint8_t *name, int name_len,
                                 LbAddr *loc)
{
    uint8_t fname[UDF_NAME_LEN];
    int isdotdot = name_len == 2 && name[0] == '.' && name[1] == '.';
    FiIter iter;
    int ret;

    for (ret = udf_fiiter_init(sb, &iter, dir, 0);
         !ret && iter.pos < dir->size;
         ret = udf_fiiter_advance(sb, &iter)) {
        uint8_t fc = iter.fi[18];   /* fileCharacteristics */
        int flen;

        if (fc & FID_FILE_CHAR_DELETED)
            continue;

        if (fc & FID_FILE_CHAR_HIDDEN)
            continue;

        if ((fc & FID_FILE_CHAR_PARENT) && isdotdot) {
            *loc = fiiter_icb(&iter);
            udf_fiiter_release(&iter);
            return 1;
        }

        if (!iter.fi[19])   /* lengthFileIdent */
            continue;

        flen = udf_get_filename(sb, iter.name, iter.name_len, fname, UDF_NAME_LEN);
        if (flen < 0) {
            ret = udf_fail(sb, AVERROR_INVALIDDATA, "a name in the directory at block %"PRIu32" cannot be converted",
                           dir->location.lbn);
            break;
        }

        if (flen == name_len && !memcmp(fname, name, flen)) {
            *loc = fiiter_icb(&iter);
            udf_fiiter_release(&iter);
            return 1;
        }
    }
    udf_fiiter_release(&iter);
    return ret < 0 ? ret : 0;
}

/* one entry of udf_readdir */
typedef struct DirItem {
    uint8_t name[UDF_NAME_LEN + 1];
    int     name_len;
    LbAddr  icb;
} DirItem;

/*
 * udf_readdir from the start, without the "." entry: deleted and hidden
 * entries skipped, the parent entry as "..", names that cannot be converted
 * skipped.
 */
static int udf_readdir(UDFLinux *sb, const UDFInode *dir, DirItem **items, int *nb_items)
{
    FiIter iter;
    int ret, cap = 0;

    *items    = NULL;
    *nb_items = 0;
    for (ret = udf_fiiter_init(sb, &iter, dir, 0);
         !ret && iter.pos < dir->size;
         ret = udf_fiiter_advance(sb, &iter)) {
        uint8_t fc = iter.fi[18];
        DirItem item;

        if (fc & FID_FILE_CHAR_DELETED)
            continue;

        if (fc & FID_FILE_CHAR_HIDDEN)
            continue;

        if (fc & FID_FILE_CHAR_PARENT) {
            memcpy(item.name, "..", 2);
            item.name_len = 2;
        } else {
            item.name_len = udf_get_filename(sb, iter.name, iter.name_len, item.name, UDF_NAME_LEN);
            if (item.name_len < 0)
                continue;
        }
        item.icb = fiiter_icb(&iter);
        if (*nb_items == cap) {
            int ncap = cap ? cap * 2 : 16;
            DirItem *n = av_realloc_array(*items, ncap, sizeof(*n));

            if (!n) {
                ret = AVERROR(ENOMEM);
                break;
            }
            *items = n;
            cap = ncap;
        }
        (*items)[(*nb_items)++] = item;
    }
    udf_fiiter_release(&iter);
    if (ret < 0) {
        av_freep(items);
        *nb_items = 0;
    }
    return ret;
}

/* ---- the common interface (Linux VFS behaviour) ---- */

/* Walks path from the root; *loc / *inode = its last component. Returns 1,
 * 0 when a component is missing or not a directory, or a negative AVERROR
 * code. */
static int udf_walk(UDFLinux *sb, const char *path, LbAddr *loc, UDFInode **inode)
{
    const char *p = path;
    LbAddr cur_loc = sb->rootdir;
    UDFInode *cur;
    int ret;

    if ((ret = udf_iget(sb, cur_loc, 0, &cur)) < 0)
        return ret;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *comp = p;

        p += n;
        if (*p == '/')
            p++;
        if (!n || (n == 1 && comp[0] == '.'))
            continue;
        if (!inode_is_dir(cur))
            return 0;
        if (n == 2 && comp[0] == '.' && comp[1] == '.' &&
            cur_loc.lbn == sb->rootdir.lbn && cur_loc.part == sb->rootdir.part)
            continue;
        if (n > UDF_NAME_LEN)
            return 0;
        if ((ret = udf_fiiter_find_entry(sb, cur, (const uint8_t *)comp, (int)n, &cur_loc)) <= 0)
            return ret;
        if ((ret = udf_iget(sb, cur_loc, 0, &cur)) < 0)
            return ret;
    }
    *loc   = cur_loc;
    *inode = cur;
    return 1;
}

/* the device sectors of every file block (as udf_map_block maps them), as
 * runs: adjacent pieces joined, sector -1 = not recorded */
static int udf_file_extents(UDFLinux *sb, const UDFInode *inode, DiscIOFile *f)
{
    uint64_t bs = sb->blocksize, per = bs / DISCIO_BLOCK_SIZE;
    uint64_t nblocks = inode->size / bs + (inode->size % bs != 0);
    ExtentPosition pos = { .bh = NULL, .offset = 0, .block = inode->location };
    uint64_t lbcount = 0;
    int have = 0, ret = 0, cap = 0;
    Aext a = { { 0, 0 }, 0, 0 };

    for (uint64_t b = 0; b < nblocks; b++) {
        uint64_t bcount = b << sb->bits, run = per;
        int64_t start = -1;
        DiscIOExtent *last;

        /* inode_bmap for each block in turn: the extent whose running byte
         * count passes the block's start */
        while (lbcount <= bcount) {
            ret = udf_next_aext(sb, inode, &pos, &a, 1);
            if (ret < 0)
                goto out;
            if (!ret) {
                /* past the last allocation descriptor every remaining block
                 * maps to nothing (the descriptors would end here again for
                 * each of them): one run, instead of one step per block of a
                 * possibly huge recorded size */
                have = 0;
                run  = (nblocks - b) * per;
                b    = nblocks - 1;
                break;
            }
            lbcount += a.elen;
            have = 1;
        }
        if (have && lbcount > bcount && a.etype == EXT_RECORDED_ALLOCATED) {
            uint32_t offset = (uint32_t)((bcount + a.elen - lbcount) >> sb->bits);
            start = (int64_t)udf_get_pblock(sb, a.eloc.lbn, a.eloc.part, offset) * per;
        }

        last = f->nb_extents ? &f->extents[f->nb_extents - 1] : NULL;
        if (last && ((last->sector < 0 && start < 0) ||
                     (last->sector >= 0 && start >= 0 && start == last->sector + last->count))) {
            last->count += run;
            continue;
        }
        if (f->nb_extents == cap) {
            int ncap = cap ? cap * 2 : 8;
            DiscIOExtent *n = av_realloc_array(f->extents, ncap, sizeof(*n));

            if (!n) {
                ret = AVERROR(ENOMEM);
                goto out;
            }
            f->extents = n;
            cap = ncap;
        }
        f->extents[f->nb_extents].sector = start;
        f->extents[f->nb_extents].count  = run;
        f->nb_extents++;
    }
    ret = 0;
out:
    epos_release(&pos);
    return ret;
}

static int udf_linux_open_file(DiscIOFS *fs, const char *path, DiscIOFile **out)
{
    UDFLinux *sb = fs->priv;
    UDFInode *inode;
    DiscIOFile *f;
    LbAddr loc;
    int ret;

    *out = NULL;
    sb->why[0] = 0;
    if ((ret = udf_walk(sb, path, &loc, &inode)) < 0) {
        udf_log(sb, AV_LOG_ERROR, "cannot open '%s': %s", path, sb->why);
        return ret;
    }
    if (!ret || !inode_is_reg(inode))
        return AVERROR(ENOENT);

    if (!(f = av_mallocz(sizeof(*f))))
        return AVERROR(ENOMEM);
    f->size = inode->size;
    if (inode->alloc_type == ICBTAG_FLAG_AD_IN_ICB) {
        /* data recorded inside the file entry, after the extended attributes
         * (read_inode checked that it fits) */
        if (!(f->data = av_malloc(FFMAX(inode->size, 1)))) {
            av_free(f);
            return AVERROR(ENOMEM);
        }
        memcpy(f->data, inode->data + inode->len_eattr, inode->size);
        udf_log(sb, AV_LOG_DEBUG, "'%s': %"PRIu64" bytes recorded in the file entry", path, inode->size);
        *out = f;
        return 0;
    }
    if ((ret = udf_file_extents(sb, inode, f)) < 0) {
        udf_log(sb, AV_LOG_ERROR, "cannot map the blocks of '%s': %s", path,
                ret == AVERROR(ENOMEM) ? "out of memory" : sb->why);
        ff_discio_file_free(&f);
        return ret;
    }
    udf_log(sb, AV_LOG_DEBUG, "'%s': %"PRIu64" bytes in %d extent(s)", path, inode->size, f->nb_extents);
    *out = f;
    return 0;
}

typedef struct ListEntry {
    char name[UDF_NAME_LEN + 1];
    int  is_dir;
} ListEntry;

static int udf_linux_list_dir(DiscIOFS *fs, const char *path, DiscIODirCallback cb, void *opaque)
{
    UDFLinux *sb = fs->priv;
    DirItem *items = NULL;
    ListEntry *list = NULL;
    int nb_items = 0, ret;
    UDFInode *dir;
    LbAddr loc;

    sb->why[0] = 0;
    if ((ret = udf_walk(sb, path, &loc, &dir)) < 0)
        goto fail;
    if (!ret || !inode_is_dir(dir))
        return AVERROR(ENOENT);
    if ((ret = udf_readdir(sb, dir, &items, &nb_items)) < 0)
        goto fail;
    if (nb_items && !(list = av_calloc(nb_items, sizeof(*list)))) {
        av_free(items);
        return AVERROR(ENOMEM);
    }
    /* each entry's kind from looking its name up, like `ls -l` */
    for (int i = 0; i < nb_items; i++) {
        LbAddr cloc = items[i].icb;
        UDFInode *child;

        ret = udf_fiiter_find_entry(sb, dir, items[i].name, items[i].name_len, &cloc);
        if (ret < 0 || (ret = udf_iget(sb, cloc, 0, &child)) < 0)
            goto fail;
        memcpy(list[i].name, items[i].name, items[i].name_len);
        list[i].name[items[i].name_len] = 0;
        list[i].is_dir = inode_is_dir(child);
    }
    av_freep(&items);
    ret = 0;
    for (int i = 0; i < nb_items && !ret; i++)
        ret = cb(opaque, list[i].name, list[i].is_dir);
    av_free(list);
    return ret;

fail:
    udf_log(sb, AV_LOG_ERROR, "cannot list '%s': %s", path, ret == AVERROR(ENOMEM) ? "out of memory" : sb->why);
    av_free(items);
    av_free(list);
    return ret;
}

static void udf_linux_free(UDFLinux *sb)
{
    if (!sb)
        return;
    icache_clear(sb);
    udf_sb_free_partitions(sb);
    av_free(sb->lvid);
    av_free(sb->pvd);
    av_free(sb->lvd);
    av_free(sb->iuvd);
    av_free(sb);
}

static void udf_linux_close(DiscIOFS *fs)
{
    udf_linux_free(fs->priv);
    fs->priv = NULL;
}

static const DiscIOFSOps udf_linux_ops = {
    .name      = "UDF (Linux)",
    .open_file = udf_linux_open_file,
    .list_dir  = udf_linux_list_dir,
    .close     = udf_linux_close,
};

int ff_discio_udf_linux_mount(DiscIOSource *src, DiscIOFS **out)
{
    UDFLinux *sb;
    DiscIOFS *fs;
    int ret, len;

    *out = NULL;
    if (!(fs = av_mallocz(sizeof(*fs))) || !(sb = av_mallocz(sizeof(*sb)))) {
        av_free(fs);
        return AVERROR(ENOMEM);
    }
    sb->src     = src;
    sb->sectors = src->size > 0 ? (uint64_t)src->size / DISCIO_BLOCK_SIZE : 0;

    if ((ret = udf_fill_super(sb)) < 0) {
        udf_log(sb, AV_LOG_VERBOSE, "no UDF volume mounted: %s",
                ret == AVERROR(ENOMEM) ? "out of memory" : sb->why);
        udf_linux_free(sb);
        av_free(fs);
        return ret;
    }

    fs->ops  = &udf_linux_ops;
    fs->priv = sb;
    fs->src  = src;
    /* the logical volume identifier (d-string, 128 bytes at 84), decoded
     * without translation */
    len = udf_dstrCS0toChar(sb, (uint8_t *)fs->label, FFMIN(254, DISCIO_LABEL_SIZE - 1), sb->lvd + 84, 128);
    fs->label[len > 0 ? len : 0] = 0;
    /* implementation use volume descriptor holding "*UDF LV Info": the UDF
     * revision is the first two bytes of its identifier suffix */
    if (sb->iuvd && !memcmp(sb->iuvd + 21, "*UDF LV Info", 12)) {
        int zero = 1;

        for (int i = 33; i < 44; i++)
            zero &= !sb->iuvd[i];
        if (zero)
            fs->udf_revision = AV_RL16(sb->iuvd + 44);
    }
    memcpy(fs->udf_recording_time, sb->pvd + 376, 12);

    udf_log(sb, AV_LOG_VERBOSE, "UDF volume mounted: label '%s', UDF revision 0x%04x, recorded %u, block size %"PRIu32,
            fs->label, fs->udf_revision, AV_RL16(fs->udf_recording_time + 2), sb->blocksize);
    *out = fs;
    return 0;
}
