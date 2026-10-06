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

#include "libavcodec/codec_id.h"
#include "libavutil/avutil.h"

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
    int      playlist;     /**< the highest playlist (index) whose clips name it, 0 = none / the first */
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

/* ---- titles ---- */

/** One extent of an EVOB: stream blocks mapped to blocks of its EVO file. */
typedef struct HDDVDExtent {
    uint32_t stream;                /**< first 2048-byte block in the EVOB's stream */
    uint32_t file;                  /**< first block in the EVO file */
    uint32_t count;                 /**< blocks */
    /* AACS key information, from the first navigation pack found for it */
    int      key_resolved;
    int      key_mode;              /**< 1: its scrambled packs are decrypted */
    uint8_t  key_offset;            /**< title key id - the clip's key base */
    uint8_t  key_byte;              /**< navigation pack byte 0x0f (kept, not used) */
    uint8_t  seed[12];              /**< 12 bytes of each pack's key seed */
} HDDVDExtent;

/**
 * An EVOB as titles use it: its record and, from its time map
 * (<name>.MAP, "HDDVD_TMAP00"), its blocks. Shared by every title using it.
 */
typedef struct HDDVDClip {
    const HDDVDEvob *evob;
    int              state;         /**< 0 not read yet, 1 usable, -1 not usable */
    uint32_t         keybase;       /**< playlist index << 8: base of its title key ids */
    DiscIOFile      *evo;           /**< its EVO file, open once usable */
    uint64_t         size;          /**< bytes of the stream: every count of the first table x 2048 */
    HDDVDExtent     *extents;       /**< sorted by stream block, unique */
    int              nb_extents;
    /* audio streams whose content was probed: their codec (NONE: left out) */
    uint8_t          probed[HDDVD_VTI_MAX_AUDIO];
    enum AVCodecID   probe_codec[HDDVD_VTI_MAX_AUDIO];
    uint8_t          probe_core[HDDVD_VTI_MAX_AUDIO];   /**< DTS-HD: also carries a DTS core */
} HDDVDClip;

typedef struct HDDVDChapterMark {
    const char *name;               /**< the chapter's displayName ("" when not given) */
    uint32_t    ms;                 /**< from the title's start */
} HDDVDChapterMark;

/** A track of a title: one stream of its EVOBs. */
typedef struct HDDVDTrack {
    enum AVMediaType type;
    enum AVCodecID   codec;
    int              index;         /**< stream index in the EVOB: 0 video, 1.. audio, then sub-pictures */
    int              number;        /**< audio: stream number 0..7; sub-picture: stream number 0..31 */
    int              coding;        /**< audio / sub-picture coding mode from the attribute record */
    char             lang[4];       /**< ISO 639-2 when known, else as written; "" none */
    int              width, height; /**< sub-pictures */
    const uint32_t  *palette;       /**< sub-pictures: 16 entries of the attribute record */
    int              core;          /**< audio: 1 = holds a core that the next track carries
                                         on its own (DTS-HD), 2 = that core track */
} HDDVDTrack;

typedef struct HDDVDTitle {
    HDDVDClip       **clips;        /**< its EVOBs, in playing order */
    int               nb_clips;
    HDDVDChapterMark *marks;
    int               nb_marks;
    const char       *lang;         /**< TitleSet defaultLanguage as an ISO 639-2 code when known,
                                         else as written */
    const char       *name;         /**< the playlist Title's name, else the first EVOB's base name */
    uint64_t          duration;     /**< 90 kHz: the EVOBs' end - start times added up */
    uint64_t          size;         /**< bytes: the clips' sizes added up */
    int               from_playlist;/**< 1: a playlist title, 0: an EVOB no playlist title uses */
    int               not_selected; /**< 1: shorter than the minimum length */
    HDDVDTrack       *tracks;       /**< video, audio, sub-pictures (ff_hddvd_tracks_build) */
    int               nb_tracks;
    int               segment;      /**< the clip the tracks come from */
} HDDVDTitle;

typedef struct HDDVDTitlePlan {
    HDDVDTitle *titles;
    int         nb_titles;
    HDDVDClip  *clips[HDDVD_VTI_MAX_EVOBS];     /**< by EVOB slot - 1, NULL = not looked at */
    char      **strings;
    int         nb_strings;
} HDDVDTitlePlan;

/**
 * Mark every EVOB with the highest playlist (index in xpls) one of whose
 * clips names it: playlists from the last to the first, in each its
 * FirstPlayTitle's clips, then every Title's clips (PrimaryAudioVideoClip src
 * "file:///dvddisc/HVDVD_TS/" or "HDDVD_TS/" + the EVOB's base name + ".map",
 * case not regarded; the EVOB's name must end in ".evo"). Each playlist must
 * have exactly one TitleSet to count. The mark is the base of the EVOB's
 * title key ids (mark * 0x100).
 */
void ff_hddvd_evob_marks(void *logctx, HDDVDVTI *vti, HDDVDXpl *const *xpls, int nb_xpls);

/**
 * The titles of the disc:
 * 1. every playlist, from the last to the first, gives candidates: its
 *    FirstPlayTitle's clips and each Title's clips (PrimaryAudioVideoClip), split
 *    into runs at every clip that is not seamless; a candidate is the list of
 *    its clips' time-map names, its TitleSet's defaultLanguage and the
 *    chapters of its Title's first ChapterList that lie in the run; the
 *    candidates are kept sorted (number of names, then the names without
 *    regard to case) and a candidate whose names are already there is dropped;
 * 2. from the last candidate to the first, a title: each name is matched to
 *    the first EVOB record (slot order) with the same name up to its first '.'
 *    (case not regarded); a name that matches none, or an EVOB whose time map
 *    cannot be used, loses the candidate;
 * 3. then, when there is a title at all, every EVOB (slot order) whose time
 *    map can be used and that no title plays becomes a title of its own.
 * Each playlist must have exactly one TitleSet. Titles shorter than
 * min_length seconds are kept, not selected.
 * @return 0, AVERROR_INVALIDDATA (a playlist without exactly one TitleSet),
 *         AVERROR(ENOMEM)
 */
int ff_hddvd_titles_plan(void *logctx, DiscIOFS *fs, const HDDVDVTI *vti, HDDVDXpl *const *xpls,
                         int nb_xpls, int min_length, HDDVDTitlePlan **out);
void ff_hddvd_titles_free(HDDVDTitlePlan **plan);

/** The clip of EVOB slot (1..1998) when the plan looked at it, else NULL. */
HDDVDClip *ff_hddvd_titles_clip(const HDDVDTitlePlan *plan, int slot);

/**
 * Whether a clip's src names EVOB evo ("file:///dvddisc/HVDVD_TS/" or
 * "HDDVD_TS/" + evo's base name + ".map", case not regarded; evo must end in
 * ".evo").
 */
int ff_hddvd_src_names_evob(void *logctx, const char *src, const char *evo);

/**
 * Read an EVOB's time map and EVO file (once; later calls return the first
 * result). The time map must start "HDDVD_TMAP00", must not have bit 1 of byte
 * 0x14 set and must have at least one table.
 * @return 1 usable, 0 not usable (logged), AVERROR(ENOMEM)
 */
int ff_hddvd_clip_load(void *logctx, DiscIOFS *fs, const char *folder, HDDVDClip *clip);

/**
 * The plan as text: per title its name, language, selection, duration
 * (90 kHz), size, EVOB names and chapters; then every EVOB looked at with its
 * time map's result, size and extents (stream:file:count); for the tests.
 * @return a string to free with av_free(), NULL when out of memory
 */
char *ff_hddvd_titles_dump(const HDDVDTitlePlan *plan);

/* ---- AACS ---- */

typedef struct HDDVDAACS HDDVDAACS;

/**
 * Open the disc's AACS layer when it has an /AACS directory (files read
 * from /AACS, else /AACS_BAK): disc ID, title keys of the VTKF files of every
 * playlist, the volume unique key from the key files (KEYDB.cfg) in order.
 * Without /AACS: 0 and *out = NULL (the disc is not encrypted).
 * @return 0, or a negative AVERROR code when the keys cannot be had
 */
int ff_hddvd_aacs_open(void *logctx, DiscIOFS *fs, const char *const *key_files, int nb_key_files,
                       int nb_playlists, HDDVDAACS **out);
void ff_hddvd_aacs_close(HDDVDAACS **aacs);

/**
 * Make a 2048-byte EVOB sector of clip usable, in place: a pack whose first
 * PES packet is scrambled (not private stream 2) is decrypted with the key
 * information of the clip's extent at byte position pos (from that extent's
 * first navigation pack: this sector when it is one; with search, the
 * extent's first EVO block, then up to 9999 blocks back from pos) and its
 * scrambling bits cleared. aacs may be NULL (no decryption available).
 * @return 1 usable, 0 not (logged)
 */
int ff_hddvd_aacs_sector(void *logctx, HDDVDAACS *aacs, DiscIOFS *fs, HDDVDClip *clip,
                         int64_t pos, uint8_t *sec, int search);

/**
 * Read block (2048 bytes) of clip's stream from its EVO file (through its
 * extents) and make it usable (ff_hddvd_aacs_sector, with search).
 * @return 1 usable, 0 not usable (logged), < 0 read error / no such block
 */
int ff_hddvd_clip_block(void *logctx, HDDVDAACS *aacs, DiscIOFS *fs, HDDVDClip *clip, uint32_t block, uint8_t *buf);

/* ---- tracks ---- */

/**
 * The tracks of every title, from the attribute record of its clip with the
 * most bytes (the first on a tie): the video stream, then the audio streams,
 * then the sub-picture streams (a stream of the same codec and number as an
 * earlier one is left out), languages from the playlists. Dolby Digital Plus
 * streams are probed (from the clip's start: a PES packet with a PTS must
 * start one of its first 2000 packs, the first frame must be a valid header;
 * 4 E-AC-3 frames -> E-AC-3, 4 AC-3 frames -> AC-3, else the stream is left
 * out). A title whose probe fails or that has no video stream is taken out
 * of the plan.
 * @return 0 or a negative AVERROR code (out of memory)
 */
int ff_hddvd_tracks_build(void *logctx, DiscIOFS *fs, HDDVDAACS *aacs, const HDDVDVTI *vti,
                          HDDVDXpl *const *xpls, int nb_xpls, HDDVDTitlePlan *plan);

#endif /* AVFORMAT_HDDVD_INTERNAL_H */
