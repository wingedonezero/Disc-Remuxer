/*
 * HD DVD (Advanced Content): structures shared by the HD DVD orchestrator's
 * parts.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifndef AVFORMAT_HDDVD_INTERNAL_H
#define AVFORMAT_HDDVD_INTERNAL_H

#include <stdint.h>

#include "discio.h"

/* ---- the Advanced VTS information file (HVDVD_TS/HVA00001.VTI) ---- */

#define HDDVD_VTI_HEADER_SIZE   0xc0
#define HDDVD_VTI_ATTR_SIZE     0x206   /* one attribute record */
#define HDDVD_VTI_ATTR_KEPT     0x186   /* its bytes kept as recorded */
#define HDDVD_VTI_EVOB_SIZE     0x140   /* one EVOB record */
#define HDDVD_VTI_MAX_ATTRS     0x1ff   /* attribute records: 1 .. 511 */
#define HDDVD_VTI_MAX_EVOBS     1998    /* EVOB records: < 1999; slots 1 .. 1998 */
#define HDDVD_VTI_MAX_AUDIO     8
#define HDDVD_VTI_MAX_SUBPIC    32

/**
 * One record of the attribute table: the attributes an EVOB's streams have.
 * Bytes 0x00..0x0d hold the video attribute, the audio stream count is at
 * 0x0e with 4-byte audio entries from 0x10, the sub-picture stream count at
 * 0xe4 with 5-byte entries from 0xe6.
 */
typedef struct HDDVDEvobAttr {
    uint8_t  raw[HDDVD_VTI_ATTR_KEPT];  /**< the record's first bytes, as recorded */
    int      nb_audio;                  /**< 0 .. 8 */
    int      nb_subpic;                 /**< 0 .. 32 */
    uint32_t words[32];                 /**< the 32 big-endian words at 0x186 */
} HDDVDEvobAttr;

/**
 * One record of the EVOB table: an enhanced video object file of the title
 * set and where it lies in time. On the corpus (6 discs, 132 records)
 * sectors * 2048 is the EVO file's size every time, and start_ptm <= end_ptm.
 */
typedef struct HDDVDEvob {
    char     name[256];    /**< file name in the title-set folder, e.g. "FEATURE_1.EVO" */
    char     base[256];    /**< name without its last ".ext" */
    int      attr;         /**< 1-based index into HDDVDVTI.attrs */
    uint32_t start_ptm;    /**< presentation start, 90 kHz */
    uint32_t end_ptm;      /**< presentation end, 90 kHz */
    uint32_t sectors;      /**< size in 2048-byte sectors */
    int      slot;         /**< 1 .. 1998, unique */
    uint8_t  raw[HDDVD_VTI_EVOB_SIZE];  /**< the record as recorded */
} HDDVDEvob;

typedef struct HDDVDVTI {
    char           folder[9];   /**< "HVDVD_TS" or "HDDVD_TS": where the VTI was found */
    int            nb_attrs;
    HDDVDEvobAttr *attrs;
    int            nb_evobs;    /**< records read (= the table's count) */
    HDDVDEvob     *evobs[HDDVD_VTI_MAX_EVOBS];  /**< by slot - 1; NULL = no record */
} HDDVDVTI;

/** Reads len bytes at pos; 0 or a negative AVERROR code (also past the end). */
typedef int (*HDDVDReadFn)(void *opaque, int64_t pos, uint8_t *buf, int len);

/**
 * Parse an Advanced VTS information file read through read(). Every check
 * is logged with the values seen; the first failing one ends the parse.
 * @return 0, AVERROR_INVALIDDATA for a file that fails a check, or the
 *         read error
 */
int ff_hddvd_vti_parse(void *logctx, HDDVDReadFn read, void *opaque, HDDVDVTI **out);

/**
 * Find /HVDVD_TS/HVA00001.VTI, else /HDDVD_TS/HVA00001.VTI, on fs and parse
 * it.
 * @return 0, AVERROR(ENOENT) when neither file exists, or as
 *         ff_hddvd_vti_parse()
 */
int ff_hddvd_vti_open(void *logctx, DiscIOFS *fs, HDDVDVTI **out);

void ff_hddvd_vti_free(HDDVDVTI **vti);

#endif /* AVFORMAT_HDDVD_INTERNAL_H */
