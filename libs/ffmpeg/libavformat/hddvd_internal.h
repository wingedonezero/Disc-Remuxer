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

/* ---- the playlists (ADV_OBJ/VPLST###.XPL) ---- */

/** Element types of a playlist (every element the reader knows). */
enum HDDVDXplClass {
    HDDVD_XPL_DOCUMENT = 0,     /**< the document itself (parent of the root) */
    HDDVD_XPL_ADVANCED_SUBTITLE_SEGMENT,
    HDDVD_XPL_APERTURE,
    HDDVD_XPL_APPLICATION_RESOURCE,
    HDDVD_XPL_APPLICATION_SEGMENT,
    HDDVD_XPL_AUDIO,
    HDDVD_XPL_AUDIO_ATTRIBUTE_ITEM,
    HDDVD_XPL_AUDIO_TRACK,
    HDDVD_XPL_CHAPTER,
    HDDVD_XPL_CHAPTER_LIST,
    HDDVD_XPL_CONFIGURATION,
    HDDVD_XPL_EVENT,
    HDDVD_XPL_FIRST_PLAY_TITLE,
    HDDVD_XPL_MAIN_VIDEO_DEFAULT_COLOR,
    HDDVD_XPL_MEDIA_ATTRIBUTE_LIST,
    HDDVD_XPL_NETWORK_SOURCE,
    HDDVD_XPL_NETWORK_TIMEOUT,
    HDDVD_XPL_PAUSE_AT,
    HDDVD_XPL_PLAYLIST,
    HDDVD_XPL_PLAYLIST_APPLICATION,
    HDDVD_XPL_PLAYLIST_APPLICATION_RESOURCE,
    HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP,
    HDDVD_XPL_SCHEDULED_CONTROL_LIST,
    HDDVD_XPL_SECONDARY_AUDIO_VIDEO_CLIP,
    HDDVD_XPL_STREAMING_BUFFER,
    HDDVD_XPL_SUB_AUDIO,
    HDDVD_XPL_SUBPICTURE_ATTRIBUTE_ITEM,
    HDDVD_XPL_SUBSTITUTE_AUDIO_CLIP,
    HDDVD_XPL_SUBSTITUTE_AUDIO_VIDEO_CLIP,
    HDDVD_XPL_SUBTITLE,
    HDDVD_XPL_SUBTITLE_TRACK,
    HDDVD_XPL_SUB_VIDEO,
    HDDVD_XPL_TITLE,
    HDDVD_XPL_TITLE_RESOURCE,
    HDDVD_XPL_TITLE_SET,
    HDDVD_XPL_TRACK_NAVIGATION_LIST,
    HDDVD_XPL_VIDEO,
    HDDVD_XPL_VIDEO_ATTRIBUTE_ITEM,
    HDDVD_XPL_VIDEO_TRACK,
    HDDVD_XPL_NB_CLASSES
};

#define HDDVD_XPL_MAX_ATTRS 12
#define HDDVD_XPL_MAX_FILES 1000        /* VPLST000 .. VPLST999 */

/** One element of a playlist. */
typedef struct HDDVDXplNode {
    int                  cls;           /**< enum HDDVDXplClass */
    struct HDDVDXplNode *parent;
    const char          *text;          /**< collected character data, NULL = none */
    /** attribute values in the order of the class's attribute table: strings
     *  (NULL until the element starts), numbers for number / boolean attributes */
    const char          *str[HDDVD_XPL_MAX_ATTRS];
    uint32_t             num[HDDVD_XPL_MAX_ATTRS];
    struct HDDVDXplNode **kids;         /**< child elements in document order */
    int                  nb_kids, kids_size;
} HDDVDXplNode;

/** One parsed playlist file. */
typedef struct HDDVDXpl {
    int           file;                 /**< N of VPLSTNNN.XPL */
    HDDVDXplNode  doc;                  /**< the document node */
    HDDVDXplNode  root;                 /**< the <Playlist> element (exists even when the
                                             root element is something else: then empty) */
    char        **strings;              /**< every string the playlist owns */
    int           nb_strings, strings_size;
} HDDVDXpl;

/**
 * Parse length bytes from offset of a playlist file (read through read())
 * as XML.
 * @return 0, AVERROR_INVALIDDATA for XML that is not well-formed, the read
 *         error, or AVERROR(ENOMEM)
 */
int ff_hddvd_xpl_parse(void *logctx, HDDVDReadFn read, void *opaque,
                       int64_t offset, int64_t length, HDDVDXpl **out);

/**
 * Read /ADV_OBJ/VPLST000.XPL, VPLST001.XPL, ... up to the first missing file
 * (at most 1000). A file that cannot be read or parsed is left out (logged);
 * the others are kept in file order.
 * @return 0 with at least one playlist, AVERROR_INVALIDDATA with none, or
 *         AVERROR(ENOMEM)
 */
int ff_hddvd_xpl_load(void *logctx, DiscIOFS *fs, HDDVDXpl ***out, int *nb_out);

void ff_hddvd_xpl_free(HDDVDXpl **xpl);
void ff_hddvd_xpl_free_all(HDDVDXpl ***xpls, int nb);

/** Name of an element type, e.g. "PrimaryAudioVideoClip". */
const char *ff_hddvd_xpl_class_name(int cls);

/** The value of attribute name of n (string attributes), NULL when n's type has none. */
const char *ff_hddvd_xpl_str(const HDDVDXplNode *n, const char *name);

/** The value of number / boolean attribute name of n, 0 when n's type has none. */
uint32_t ff_hddvd_xpl_num(const HDDVDXplNode *n, const char *name);

/** The i-th child of n of type cls (document order), NULL past the last. */
HDDVDXplNode *ff_hddvd_xpl_child(const HDDVDXplNode *n, int cls, int i);

/** The number of children of n of type cls. */
int ff_hddvd_xpl_count(const HDDVDXplNode *n, int cls);

/**
 * The whole element tree as text, one element per line in document order:
 * indentation, type name, every attribute of the type as name=value (with
 * defaults; strings in quotes, (null) for none), then text="..." when set.
 * @return a string to free with av_free(), NULL when out of memory
 */
char *ff_hddvd_xpl_dump(const HDDVDXpl *xpl);

#endif /* AVFORMAT_HDDVD_INTERNAL_H */
