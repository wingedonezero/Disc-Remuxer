/*
 * Blu-ray (BDMV, BDAV, AVCHD): the format part's structures
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

#ifndef AVFORMAT_BLURAY_INTERNAL_H
#define AVFORMAT_BLURAY_INTERNAL_H

#include <inttypes.h>
#include <stdint.h>

#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavcodec/codec_id.h"

#include "discinfo.h"
#include "discio.h"

/* ---- reading a disc file (bluray_files.c) ---- */

/**
 * A disc file read whole: the main copy, or its BACKUP copy when the main
 * copy is missing or cannot be read (a BACKUP copy of another size than a
 * readable main copy is not used).
 */
typedef struct BlurayFile {
    uint8_t *data;
    uint32_t size;
    char     path[96];   /**< the path read, for the log */
} BlurayFile;

/** The type indicator and version of a file's first 8 bytes: the 4 type
 *  bytes big-endian, the 4 version characters read as hexadecimal ("0200"
 *  -> 0x200). */
typedef struct BlurayHeader {
    uint32_t type;
    uint16_t version;
} BlurayHeader;

/** Bytes [off, off + len) of f into buf; 0 when they are not all in the
 *  file (a read the reference program's file reader refuses). */
int  ff_bluray_read(const BlurayFile *f, uint32_t off, void *buf, uint32_t len);
void ff_bluray_header(const uint8_t h[8], BlurayHeader *out);

/**
 * Load "/<root><dir>/<name>.<ext>" with the first extension variant of exts
 * (NULL-terminated, at most 5) that has a main or BACKUP copy
 * ("/<root>/BACKUP<dir>/<name>.<ext>"); names[k] is the base name tried with
 * exts[k] (names[0] where names has fewer entries).
 * @return 1 loaded, 0 no copy found, < 0 error
 */
int  ff_bluray_load(void *log, DiscIOFS *fs, const char *root, const char *dir,
                    const char *const *names, const char *const *exts, BlurayFile *out);
void ff_bluray_file_free(BlurayFile *f);

/**
 * The 5-digit numbers of the files "NNNNN<suffix>" (suffix with its dot,
 * exact case) of a directory, in the directory's order; names whose first
 * 5 characters are not digits are skipped (logged). A missing directory
 * gives none.
 * @return 0, < 0 error
 */
int  ff_bluray_list_numbered(void *log, DiscIOFS *fs, const char *dir, const char *suffix,
                             uint32_t **out, int *nb);
void ff_bluray_sort_numbers(uint32_t *v, int n);

/* ---- extension data (shared by playlists, clip information, index) ---- */

typedef struct BlurayExtEntry {
    uint16_t id1, id2;
    uint32_t start;      /**< absolute file offset of its data */
    uint32_t length;     /**< as recorded */
    uint32_t copied;     /**< bytes kept in data (0 when the parser reads it itself) */
    uint8_t *data;
} BlurayExtEntry;

/**
 * The extension data block at file offset off: its entries appended to
 * *entries; the data of an entry is copied (clamped to the file) unless
 * parsed_directly(id1, id2).
 * @return 1, 0 a read failed, < 0 error
 */
int ff_bluray_ext_data(void *log, const BlurayFile *f, uint32_t off,
                       int (*parsed_directly)(uint16_t id1, uint16_t id2),
                       BlurayExtEntry **entries, int *nb);
void ff_bluray_ext_free(BlurayExtEntry **entries, int *nb);

/* ---- playlists (bluray_mpls.c: BDMV .mpls, bluray_rpls.c: BDAV .rpls) ---- */

/** One stream of an STN table: its stream entry and stream attributes, as
 *  recorded (payloads truncated to 8 / 32 bytes; the lengths are the
 *  recorded ones). */
typedef struct BlurayStnStream {
    uint8_t entry_type;      /**< stream_entry byte 1 (stream_type) */
    uint8_t entry_len;       /**< length - 1 */
    uint8_t entry[8];        /**< the entry's bytes after its type */
    uint8_t coding_type;     /**< stream_attributes byte 1 */
    uint8_t attr_len;        /**< length - 1 */
    uint8_t attr[32];        /**< the attributes' bytes after the coding type */
} BlurayStnStream;

/** A clip of an angle (multi-angle PlayItem) or of a multi-clip SubPlayItem. */
typedef struct BlurayAngleClip {
    char    clip[6];         /**< Clip_Information_file_name, 5 characters */
    char    codec[5];        /**< Clip_codec_identifier, 4 characters */
    uint8_t stc_id;          /**< ref_to_STC_id */
    uint32_t pos;            /**< file offset of its codec identifier */
} BlurayAngleClip;

/** The stereoscopic (3-D) part of a PlayItem (extension 2/1, STN_table_SS). */
typedef struct BlurayStnSS {
    int             present;
    uint8_t         fixed_offset_popup;  /**< first byte bit 7 */
    BlurayStnStream dep_video;           /**< the dependent view's video stream */
    uint8_t         nb_offset_seq;       /**< low 6 bits */
    uint8_t       (*pg)[23];             /**< one record per PG stream of the PlayItem */
} BlurayStnSS;

typedef struct BlurayPlayItem {
    int             multi_angle;     /**< byte 0x0c bit 4 */
    char            clip[6];
    char            codec[5];
    uint32_t        pos;             /**< file offset of its codec identifier */
    uint8_t         connection;      /**< connection_condition (byte 0x0c low 4 bits) */
    uint8_t         stc_id;
    uint32_t        in_time, out_time;   /**< 45 kHz */
    BlurayAngleClip *angles;         /**< the further angles (angle 1 is the item's own clip) */
    int             nb_angles;
    uint8_t         nb_ig;           /**< STN: interactive graphics streams (not read) */
    BlurayStnStream video;           /**< STN: the one primary video stream */
    BlurayStnStream *audio;
    int             nb_audio;
    BlurayStnStream *pg;
    int             nb_pg;
    BlurayStnSS     ss;
} BlurayPlayItem;

typedef struct BluraySubPlayItem {
    int             multi_clip;      /**< byte 0x0e bit 0 */
    char            clip[6];
    char            codec[5];
    uint32_t        pos;
    uint8_t         connection;      /**< byte 0x0e bits 1-4 */
    uint8_t         stc_id;
    uint32_t        in_time, out_time;
    uint16_t        sync_play_item;
    uint32_t        sync_start_pts;
    BlurayAngleClip *clips;          /**< the further clips of a multi-clip item */
    int             nb_clips;
} BluraySubPlayItem;

typedef struct BluraySubPath {
    uint8_t            type;         /**< SubPath_type */
    BluraySubPlayItem *items;
    int                nb_items;
} BluraySubPath;

typedef struct BlurayMark {
    uint8_t  type;
    uint16_t play_item;
    uint32_t time;                   /**< 45 kHz */
    uint16_t pid;
    uint32_t duration;
} BlurayMark;

typedef struct BlurayPlaylist {
    uint32_t        number;          /**< file number (NNNNN.mpls) */
    BlurayHeader    hdr;
    int             bdav;            /**< a BDAV .rpls playlist */
    /* AppInfoPlayList */
    uint8_t         playback_type;
    uint16_t        playback_count;
    uint16_t        app_flags;
    BlurayPlayItem *items;
    int             nb_items;
    BluraySubPath  *subpaths;
    int             nb_subpaths;
    BlurayMark     *marks;
    int             nb_marks;
    BlurayExtEntry *ext;
    int             nb_ext;
    BluraySubPath   ext_subpath;     /**< extension 2/2: its first SubPath */
    int             ext_subpath_type;/**< 0xffff: none */
    /* BDAV only */
    uint8_t         bdav_type;       /**< PlayList type (low 4 bits) */
    uint8_t         bdav_hdr[0x158]; /**< the file's first 0x158 bytes (UIAppInfoPlayList: character set at
                                          0x2e, recording time 0x32..0x38, number 0x40, names 0x43 / 0x58) */
    uint8_t         name_len;
    char            name[256];       /**< the recorded name bytes (0x59..) */
} BlurayPlaylist;

/** Parse an .mpls file. @return 1, 0 not a valid playlist (logged), < 0 error */
int  ff_bluray_mpls_parse(void *log, const BlurayFile *f, BlurayPlaylist *pl);
/** Parse a BDAV .rpls file. @return 1, 0 not a valid playlist, < 0 error */
int  ff_bluray_rpls_parse(void *log, const BlurayFile *f, BlurayPlaylist *pl);
void ff_bluray_playlist_free(BlurayPlaylist *pl);

/* ---- clip information (bluray_clpi.c) ---- */

typedef struct BlurayStc {
    uint16_t pcr_pid;
    uint32_t spn_stc_start;          /**< source packet number */
    uint32_t start, end;             /**< presentation start / end, 45 kHz */
} BlurayStc;

typedef struct BlurayAtc {
    uint32_t   spn_atc_start;
    uint8_t    offset_stc_id;
    BlurayStc *stc;
    int        nb_stc;
} BlurayAtc;

typedef struct BlurayClipStream {
    uint16_t pid;
    uint8_t  coding_type;
    uint8_t  info_len;               /**< recorded length - 1 */
    uint8_t  info[32];
    /* what the stream probe found (bluray_tracks.c) */
    uint32_t probe;                  /**< BLURAY_PROBE_* */
    char     probe_lang[4];          /**< with BLURAY_PROBE_LANG */
    uint8_t  subtype;                /**< with BLURAY_PROBE_SUBTYPE: a private stream's kind */
} BlurayClipStream;

#define BLURAY_PROBE_DONE     0x10   /**< probed */
#define BLURAY_PROBE_NO_CORE  0x20   /**< E-AC-3 / DTS-HD without a core substream */
#define BLURAY_PROBE_LANG     0x40   /**< probe_lang found in the stream */
#define BLURAY_PROBE_SUBTYPE  0x80   /**< subtype found in the program map */
#define BLURAY_PROBE_M2TS     0x100  /**< a stream of a BDAV (M2TS) clip */
#define BLURAY_PROBE_HEVC_EL  0x200  /**< HEVC with a second layer on PID 0x1015 */
#define BLURAY_PROBE_AVC_ID   0x400  /**< AVC video on PES stream id 0xE0 + (probe & 0xf) */

typedef struct BlurayProgram {
    uint32_t          spn_start;
    uint16_t          pmt_pid;
    BlurayClipStream *streams;
    int               nb_streams;
} BlurayProgram;

typedef struct BlurayAtcDelta {
    uint32_t delta;
    char     clip[6];
    char     codec[5];
} BlurayAtcDelta;

typedef struct BlurayClip {
    uint32_t        number;          /**< file number (NNNNN.clpi) */
    BlurayHeader    hdr;
    /* ClipInfo */
    uint32_t        stream_type;     /**< Clip_stream_type (1 on old / BDAV clips) */
    uint32_t        app_type;        /**< application_type */
    int             atc_delta;       /**< the ATC delta list is present */
    uint32_t        rate;            /**< TS_recording_rate */
    uint32_t        packets;         /**< number_of_source_packets */
    uint8_t         ts_validity;
    char            format_id[5];
    BlurayAtcDelta *deltas;
    int             nb_deltas;
    /* SequenceInfo: the first ATC sequence (a second one is dropped on BDMV clips) */
    BlurayAtc      *atc;
    int             nb_atc;
    /* ProgramInfo */
    BlurayProgram   program;
    /* CPI */
    uint8_t         cpi_type;
    /* extension data */
    BlurayExtEntry *ext;
    int             nb_ext;
    uint32_t       *ss_extents;      /**< extension 2/4: extent start points */
    int             nb_ss_extents;
    /* the stream file */
    char            stream_path[48]; /**< "/BDMV/STREAM/NNNNN.m2ts" as found, "" none */
    int64_t         stream_size;
    int             stc_gap;         /**< its STC ids are not contiguous: no STC lookup succeeds */
    int             interleave;      /**< 3-D pairing with it: 0 not decided, 1 fails, 2 this one first, 3 the other first */
    /* the program map read from the stream (BDAV clips, bluray_probe.c) */
    uint8_t        *pmt;
    int             pmt_len;
    int             pmt_tried;
    /* AACS (bluray_aacs.c) */
    int             key_unit;        /**< CPS unit whose key decrypts it: 1.., BLURAY_KEY_NONE, BLURAY_KEY_UNKNOWN */
} BlurayClip;

int  ff_bluray_clpi_parse(void *log, const BlurayFile *f, BlurayClip *c);
/** The clip's stream file: "/<root>/STREAM/NNNNN.m2ts" (".fmts" for stream
 *  type 2, else ".MTS"); a size other than packets x 192 is logged.
 *  @return 1 found, 0 not found */
int  ff_bluray_clip_stream_file(void *log, DiscIOFS *fs, BlurayClip *c);
void ff_bluray_clip_free(BlurayClip *c);

/* ---- index.bdmv, id.bdmv (bluray_index.c) ---- */

typedef struct BlurayIndexEntry {
    uint8_t  object_type;            /**< byte 0 bits 7-6 (1 HDMV, 2 BD-J) */
    uint8_t  access_type;            /**< byte 0 bits 5-4 */
    uint8_t  playback_type;          /**< byte 4 bits 7-6 */
    uint32_t ref;                    /**< object type 1: movie object id; 2: BD-J object number */
} BlurayIndexEntry;

typedef struct BlurayIndex {
    BlurayHeader      hdr;
    BlurayIndexEntry  first_play, top_menu;
    BlurayIndexEntry *titles;
    int               nb_titles;
    BlurayExtEntry   *ext;
    int               nb_ext;
} BlurayIndex;

typedef struct BlurayId {
    BlurayHeader hdr;
    uint32_t     org_id;
    uint8_t      disc_id[16];
} BlurayId;

int  ff_bluray_index_parse(void *log, const BlurayFile *f, BlurayIndex *ix);
void ff_bluray_index_free(BlurayIndex *ix);
int  ff_bluray_id_parse(void *log, const BlurayFile *f, BlurayId *id);

/* ---- disc names (bluray_meta.c) ---- */

/** Recorded text of the given character set as UTF-8 (av_free()), or NULL
 *  (none, or a character set that is not decoded). */
char *ff_bluray_text(void *log, uint8_t charset, const uint8_t *text, uint32_t len);
/** The disc library title with the suffixes the reference program removes
 *  (" Blu-ray 3D", " 4K Ultra HD", ", The", ...) taken off (av_free()), or
 *  NULL when nothing is left. */
char *ff_bluray_clean_title(const char *s);
/**
 * A disc library file (bdmt_LLL.xml): the name of its first discinfo's
 * first title, cleaned (av_free(); NULL when none), and the text of its
 * first language element (as written, cut to 7 bytes; "" when none). A file
 * that does not parse is tried again with the reference program's repairs.
 * @return 1, 0 not readable XML, < 0 error
 */
int ff_bluray_meta_read(void *log, const BlurayFile *f, char **name, char lang[8]);

/* ---- titles (bluray_titles.c) ---- */

/** One piece of a title: an STC sequence of a clip (the whole sequence), with
 *  its 3-D dependent-view clip when the title is stereoscopic. */
typedef struct BluraySegItem {
    BlurayClip           *clip;
    const BlurayStc      *stc;
    BlurayClip           *dep;          /**< dependent-view clip (3-D), or NULL */
    const BlurayPlayItem *item;         /**< the PlayItem it comes from, or NULL (a clip title) */
    int                   connection;   /**< the PlayItem's connection condition (1 for clip titles) */
    int64_t               start, length;/**< bytes: in the clip's stream file, or (interleaved) in the
                                             base + dependent-view pair */
} BluraySegItem;

/** Items read as one stream: an item and the seamlessly connected
 *  (connection condition 6) items after it. */
typedef struct BluraySegment {
    BluraySegItem *items;
    int            nb_items;
    int            interleaved;         /**< base and dependent view read interleaved (3-D) */
    char           path[192];           /**< "/BDMV/STREAM/00001.m2ts+00002.m2ts", for the log */
    char           desc[256];           /**< "(1:0/2,3)": clips, STC index, dependent clip */
} BluraySegment;

typedef struct BlurayAACS BlurayAACS;   /**< bluray_aacs.c */

/** A text subtitle (TextST) stream of a title, read from its own clip. */
typedef struct BluraySide {
    BlurayClip           *clip;
    const BlurayStc      *stc;
    const BlurayStnStream *entry;
    BlurayClip           *sync_clip;
    const BlurayStc      *sync_stc;
    uint32_t              sync_pts;
    uint16_t              sync_item;
    BluraySegment         seg;
} BluraySide;

/** A track of a title (bluray_tracks.c): one of the parts a clip stream
 *  gives (TrueHD and its AC-3, a subtitle and its forced-only copy, ...). */
typedef struct BlurayTrack {
    int              code;          /**< the kind of track (internal codes, bluray_tracks.c) */
    enum AVMediaType type;
    enum AVCodecID   codec;
    uint16_t         pid;
    uint32_t         sel1, sel2;    /**< what of the PID it takes (PES stream id extension, ...) */
    uint32_t         flags;         /**< BLURAY_TRACK_* */
    char             lang[4];
    int              stream;        /**< the stream it comes from (the title's stream numbering) */
    int              from_track;    /**< the track it is taken out of, -1 none */
    int              inner_track;   /**< the track taken out of it, -1 none */
    int              offset_seq;    /**< 3-D subtitle: offset sequence id, -1 none */
    uint32_t         side_clip;     /**< text subtitle from its own clip: the clip number */
    int              side_stc;      /**< and the STC sequence (index over the clip's ATC sequences) */
} BlurayTrack;

#define BLURAY_TRACK_SECONDARY 0x200    /**< secondary audio */
#define BLURAY_TRACK_HAS_CORE  0x400    /**< a core track is taken out of it */
#define BLURAY_TRACK_CORE      0x100    /**< the core of another track */
#define BLURAY_TRACK_DERIVED   0x800    /**< taken out of another track */
#define BLURAY_TRACK_FORCED    0x1000   /**< only the forced subtitles */
#define BLURAY_TRACK_3D_BASE   0x2000   /**< the base view of a stereoscopic (3-D) video */
#define BLURAY_TRACK_3D_PLANE  0x8000   /**< a 3-D subtitle with an offset sequence (offset_seq) */
#define BLURAY_TRACK_LAYER2    0x10000  /**< HEVC read with its second layer (PID sel1) */
#define BLURAY_TRACK_SIDE      0x1000000/**< read from its own clip (text subtitle) */

typedef struct BlurayTitle {
    char                  name[32];     /**< "00800.mpls", "00800.mpls(1)", "00012.m2ts", "01001.rpls(1)" */
    BluraySegment        *segs;
    int                   nb_segs;
    BluraySide           *side;
    int                   nb_side;
    uint64_t             *marks;        /**< chapter positions, 1/1,080,000,000 s, ascending */
    int                   nb_marks;
    uint16_t             *pids;         /**< the streams the title keeps (ascending) */
    int                   nb_pids;
    const BlurayPlaylist *pl;           /**< NULL for a clip title */
    int                   angle;        /**< 1-based angle of a multi-angle playlist, 0 none */
    char                 *title_name;   /**< BDAV / AVCHD title name (av_free()), or NULL */
    char                 *alt_name;     /**< BDAV: the number or name part kept apart */
    int                   year, month, day, hour, minute, second;  /**< recording time (BDAV / AVCHD), 0 none */
    /* tracks (bluray_tracks.c) */
    BlurayTrack          *tracks;
    int                   nb_tracks;
    int                   track_seg;    /**< the segment the tracks are described from (the largest) */
} BlurayTitle;

/**
 * Build the disc's titles with the reference program's rules: play items
 * merged where they join cleanly, each playlist split into parts at its
 * non-seamless connections, repeated items trimmed, one title per angle,
 * titles for clips no title uses fully, recordable / AVCHD titles,
 * duplicates and titles shorter than min_length seconds left out.
 * The playlists' items may be changed (merged, connection conditions set).
 */
struct BlurayDisc;
int  ff_bluray_titles_build(void *log, struct BlurayDisc *d, int min_length);   /* with the tracks (d->fs, d->keys) */
void ff_bluray_titles_free(struct BlurayDisc *d);
/** Title i left out of the list (freed; the titles after it move up). */
void ff_bluray_title_remove(struct BlurayDisc *d, int i);
/** The title's duration, 45 kHz ticks (its segments' STC sequences). */
/**
 * The tracks of title t (t->tracks): its streams described from the
 * largest segment's first clip (the title's streams only), the second view
 * of a 3-D title, its text subtitles; video first, then audio, then
 * subtitles, an identical audio / subtitle stream left out.
 * @return 0, AVERROR_INVALIDDATA the title has no video, < 0 error
 */
int ff_bluray_title_tracks(void *log, DiscIOFS *fs, BlurayAACS *aacs, BlurayTitle *t);

/**
 * The PES stream keys a track's PID is read with (stream_id, or 0xFD |
 * stream_id_extension << 8; 0 any key), the reference program's per kind:
 * video 0xE0 (AVC 0xE0 + its probed id, VC-1 0x55FD, HEVC 0xE1), AC-3 /
 * DTS / E-AC-3 alone extension sel1 (any on BDAV clips), DTS-HD alone and
 * TrueHD extension sel1, DTS-HD / E-AC-3 with a core both sel1 and sel2,
 * LPCM and PGS 0xBD, AAC any, MPEG audio 0xC0 + sel1 (any on BDAV clips).
 * bdav: the title's clips are BDAV (M2TS) clips.
 * @return the number of keys (1 or 2), 0 a kind with none (text subtitles)
 */
int ff_bluray_track_keys(const BlurayTrack *tk, int bdav, uint16_t key[2]);

/**
 * The stream probe of clip stream e (once per stream) from the segment's
 * stream data: no core substream, the AVC PES stream id, the HEVC second
 * layer, BDAV program map kinds and languages (BLURAY_PROBE_*). c is the
 * segment's first clip (e may belong to its dependent view); code and
 * nb_parts: e's description before the probe (its first track kind, its
 * number of tracks).
 * @return 0, < 0 the stream data cannot be read (logged)
 */
int ff_bluray_probe_stream(void *log, DiscIOFS *fs, BlurayAACS *aacs, const BluraySegment *s,
                           BlurayClip *c, BlurayClipStream *e, int code, int nb_parts);

/**
 * The PES stream keys seen in pid of the segment's stream data, up to
 * limit PES headers of one key (the start of the stream).
 * @return the number of keys seen, < 0 the stream data cannot be read
 */
int ff_bluray_pid_scan(void *log, DiscIOFS *fs, BlurayAACS *aacs, const BluraySegment *s, uint16_t pid, int limit);

/** One run of a 3-D item's two views read interleaved: bytes [pos, pos +
 *  len) of the item are bytes [off, off + len) of the base (view 0) or the
 *  dependent-view (view 1) clip's stream file. */
typedef struct BlurayChunk {
    int64_t pos, off, len;
    int     view;
} BlurayChunk;

/** A segment's stream data as 6144-byte aligned units (32 source packets):
 *  its items' byte ranges one after another (a 3-D item: its base-view clip
 *  only, or with both its views interleaved at their extent start points),
 *  each unit made usable by ff_bluray_aacs_unit (bluray_probe.c). */
typedef struct BluraySegReader {
    void                *log;
    DiscIOFS            *fs;
    BlurayAACS          *aacs;
    const BluraySegment *s;
    int                  both;         /**< 3-D items: both views */
    int                  item;         /**< the open item, -1 none */
    DiscIOFile          *f;            /**< its (base-view) stream file */
    DiscIOFile          *f2;           /**< both: its dependent view's stream file */
    BlurayChunk         *chunks;       /**< both: the open 3-D item's runs */
    int                  nb_chunks;
} BluraySegReader;

#define BLURAY_UNIT 6144

/** Bytes of the segment's stream data (3-D items: the base view only). */
int64_t ff_bluray_seg_size(const BluraySegment *s);
/** The same with both views of its 3-D items (both 1). */
int64_t ff_bluray_seg_size_views(const BluraySegment *s, int both);
void    ff_bluray_seg_reader_init(BluraySegReader *u, void *log, DiscIOFS *fs, BlurayAACS *aacs,
                                  const BluraySegment *s);
/** The reader reads both views of the segment's 3-D items (before the first read). */
void    ff_bluray_seg_reader_both(BluraySegReader *u);
/** The unit at pos (a multiple of 6144) into buf: 0, AVERROR_EOF past the
 *  end (also for a part unit at the end), < 0 error (logged). */
int     ff_bluray_seg_read(BluraySegReader *u, int64_t pos, uint8_t *buf);
void    ff_bluray_seg_reader_close(BluraySegReader *u);

int64_t ff_bluray_title_duration(const BlurayTitle *t);
/** The title's size, bytes. */
int64_t ff_bluray_title_size(const BlurayTitle *t);

/**
 * The parts of title t: the runs of its segments between the places where
 * whole stream files are played back to back (a non-seamless connection,
 * connection condition 1, that joined into the title because both items
 * cover their whole STC sequences: episodes of a series one after another).
 * first (nb_segs + 1 entries, or NULL): the first segment of each part, and
 * nb_segs after the last. Returns the number of parts (1: the title is one
 * piece).
 */
int     ff_bluray_title_parts(const BlurayTitle *t, int *first);
/** 45 kHz ticks of segments [s0, s1) (their STC sequences, as the title's duration). */
int64_t ff_bluray_segs_duration(const BlurayTitle *t, int s0, int s1);
/**
 * The chapter marks of segments [s0, s1) of title t on their own: from the
 * first segment's start, without those in their last second (the rule a
 * title has for its own end). out: room for t->nb_marks. Returns the count.
 */
int     ff_bluray_segs_marks(const BlurayTitle *t, int s0, int s1, uint64_t *out);

/* ---- the disc (bluray_disc.c) ---- */

typedef struct BlurayDisc {
    DiscIOFS       *fs;
    DiscIOFS       *parent_fs;       /**< AVCHD folder: the card's root (names only), or NULL */
    int             bdav;            /**< /BDAV/info.bdav: a BDAV (recorder) disc */
    int             bdplus;          /**< /BDSVM/00000.svm present */
    char           *name;            /**< disc name (av_free()), or NULL */
    char           *name_eng;        /**< its English name when another language was chosen */
    char            name_lang[4];    /**< language of the name's disc library file */
    BlurayClip     *clips;           /**< in listing order; clips that do not load are left out */
    int             nb_clips;
    BlurayPlaylist *playlists;
    int             nb_playlists;
    uint32_t       *playlist_ids;    /**< the playlist listing (also the files that did not load) */
    int             nb_playlist_ids;
    uint32_t       *bdjo;            /**< BD-J object numbers (parsed with the BD-J step) */
    int             nb_bdjo;
    uint32_t       *jars;
    int             nb_jars;
    BlurayIndex     index;
    int             has_index;
    BlurayId        id;
    int             has_id;
    BlurayTitle    *titles;
    int             nb_titles;
    /* AACS (bluray_aacs.c) */
    BlurayAACS     *keys;            /**< the keys (owned by the format part), NULL none */
    int             aacs;            /**< an /AACS or /AACS_bd directory */
    uint8_t         disc_id[20];     /**< SHA-1 of the disc's Unit_Key file */
    int             has_disc_id;
    char            disc_id_from[96];/**< the file it is the digest of */
} BlurayDisc;

/* ---- AACS (bluray_aacs.c) ---- */

#define BLURAY_KEY_UNKNOWN (-1)      /**< not found yet: found on the clip's first encrypted unit */
#define BLURAY_KEY_NONE      0       /**< no key: its units are used as they are */

/**
 * The AACS step of the disc open: an /AACS (or /AACS_bd) directory gives the
 * disc's unit keys (through the key files) and each clip's key, or shows the
 * disc is not encrypted after all; without one the disc is taken as not
 * encrypted (with the checks for discs decrypted by other software). Sets
 * the clips' key_unit and the disc ID.
 * @return 0 (*out NULL when nothing is decrypted), AVERROR(EACCES) no key
 *         for the disc, AVERROR_PATCHWELCOME a kind of AACS not supported,
 *         another < 0 error (the disc cannot be opened)
 */
int  ff_bluray_aacs_open(void *log, BlurayDisc *d, const char *const *key_files, int nb_key_files,
                         BlurayAACS **out);
void ff_bluray_aacs_close(BlurayAACS **a);

/**
 * Make one 6144-byte aligned unit of clip c usable, in place: a unit marked
 * encrypted (copy permission bits) is decrypted with the clip's key (found
 * on its first encrypted unit when not known yet) and its marks cleared; a
 * unit that is not marked, or of a clip without a key, is left as it is.
 * @return 0, < 0 error (the unit cannot be decrypted)
 */
int  ff_bluray_aacs_unit(void *log, BlurayAACS *a, BlurayClip *c, uint8_t *unit);

/**
 * Read a Blu-ray disc's structure: its kind (BDMV / BDAV), name, clip
 * information (with their stream files), playlists, index and id.
 * path: the source as given (an AVCHD folder's card root is mounted for its
 * name files), or NULL.
 * @return 0, < 0 error (the disc cannot be read)
 */
int  ff_bluray_disc_open(void *log, DiscIOFS *fs, const char *path, const char *lang, BlurayDisc *d);
void ff_bluray_disc_free(BlurayDisc *d);
/** The disc's structure as text, one line per clip, playlist, PlayItem,
 *  mark and stream (for the log and for comparisons with other readers). */
struct AVBPrint;
void ff_bluray_disc_dump(const BlurayDisc *d, struct AVBPrint *bp);

#endif /* AVFORMAT_BLURAY_INTERNAL_H */
