/*
 * The disc layer's inner model (the public face is disc/disc.h): the disc
 * context, the format parts' vtable and what a format part tells the title
 * reader of an opened title. The disc layer opens a source once (disc
 * readers), tells which kind of disc it is and hands it to that kind's
 * format part, which reads the disc's structure, opens the chosen title and
 * gives its blocks.
 */

#ifndef DISC_DISC_MODEL_H
#define DISC_DISC_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "disc/disc.h"
#include "disc/info.h"
#include "io/discio.h"
#include "timeline/disctl.h"

typedef struct DrDisc DrDisc;

struct DRTitleTrack;

/** What of a Blu-ray clip's stream data a track takes: the payloads of
 *  its PID with one of its PES stream keys (stream_id, or 0xFD |
 *  stream_id_extension << 8; key 0: any key of the PID). A track with two
 *  keys takes both in the order they arrive (a core frame and its
 *  extension: they alternate, the first key first). */
typedef struct DiscTsFeed {
    int      pid;
    uint16_t key[2];
    int      nb_keys;
    int      side;          /**< 0: the title's clips; k + 1: side clip k (DiscTitleSource.nb_sides) */
} DiscTsFeed;

/** A track of a title as its format part describes it: one shape for every
 *  format. What the disc info shows of it, where its data is and how the
 *  title reader takes it; the disc layer makes the disc info's stream and,
 *  for the opened title, its public track (disc/disc.h) and title-reader
 *  track from it. */
typedef struct DiscTrack {
    DiscStreamInfo  info;           /**< the disc info's stream (the stream details as far as known) */
    int             kind;           /**< DR_KIND_VIDEO / _AUDIO / _SUBTITLE / _CAPTION (disctl.h) */
    int             id;             /**< program stream: the id the PS clip demuxer (src/mux/ps_disc.c) gives its payloads; -1 none (a caption
                                         track: carried in the video) */
    DiscTsFeed      feed;           /**< transport stream: its PID and PES stream keys */
    unsigned        flags;          /**< DISC_TRACK_* */
    int             stream;         /**< the format's own number of its stream, for the log */
    int             number;         /**< its place in the format's list of the title's streams (the duplicate
                                         rule's log) */
    DiscTsFeed      layer_feed;     /**< DISC_TRACK_LAYER2: the second layer's PID and keys */
    const uint32_t *palette;        /**< DVD sub-pictures: 16 Y'CrCb entries for the VobSub header */
    const char     *picture;        /**< for the caller's log: what the subtitle units are, or NULL */
    const char     *caption;        /**< for the caller's log: what the caption track is made from, or NULL */
    const char     *line21;         /**< video: the line-21 caption fields its attributes mark, or NULL */
    const char     *eye;            /**< 3-D video: the eye it shows when both views are read, or NULL */
    const char     *not_read;       /**< the title reader does not read it yet (NULL: it does): left out */
    const char     *untested;       /**< read, but not met on a real disc of the format yet: what, for the log */
} DiscTrack;

/** The second view of a 3-D video (read only with option second_view, as a
 *  second layer of the video). */
#define DISC_TRACK_SECOND_VIEW 0x0001
/** A video read with a second layer on its own PID (layer_feed): a Dolby
 *  Vision enhancement layer, merged or a stream of its own (option
 *  dolby_vision). */
#define DISC_TRACK_LAYER2      0x0002
/** Read from a clip of its own (Blu-ray text subtitles): not in the stream
 *  pass. */
#define DISC_TRACK_SIDE        0x0004

/** The segment a title's stream pass reads (its largest), as its format
 *  part reads it. */
typedef struct DiscPassSource {
    int       index;                /**< the segment's number in the title (from 0), for the log */
    int       ts;                   /**< Blu-ray stream data (6144-byte units, MPEG-TS), else 2048-byte sectors */
    int       lpcm_header;          /**< program stream: the LPCM audio frame header's size */
    int       read_all;             /**< read on to the segment's end while a subtitle track has had no packet
                                         (to report an empty one) */
    uint64_t  key;                  /**< the segment's identity: titles with the same key share its results;
                                         0: none */
    /** Block of the segment into buf: 1 read, 0 not usable (left out),
     *  DR_ERR_EOF the end, < 0 cannot be read. */
    int     (*read)(void *opaque, uint32_t block, uint8_t *buf);
    void     *opaque;
} DiscPassSource;

typedef struct DiscPassCache DiscPassCache;

/** A damaged range as the disc's own reader names it (DiscTitleSource.damage). */
typedef struct DiscDamage {
    int       kind;                     /**< DISC_DAMAGE_* */
    uint32_t  blocks;                   /**< its blocks: the last ones of the clip left out before it was given */
    char      file[32], last_file[32];  /**< the files of its first and last block ("": not known) */
    int64_t   offset, last_offset;      /**< the byte offsets there (-1: not known) */
} DiscDamage;

enum {
    DISC_DAMAGE_ZERO = 1,               /**< every byte is zero */
    DISC_DAMAGE_NO_MPEG,                /**< no MPEG pack with a packet */
};

/** What the title reader needs of the opened title; the format part fills
 *  DiscContext.src in its open (the arrays and strings stay its own until
 *  its close). Track i is stream i; track 0 is the video. Program-stream
 *  clips (ts 0): 2048-byte sectors through the clip demuxer; a track takes
 *  the payloads it gives the id of its stream (the start code 0x1xx of an
 *  MPEG stream, the sub-stream id of a private stream 1 stream, 0xFD55 for
 *  VC-1). Blu-ray clips (ts 1): 6144-byte units of 192-byte source packets
 *  through the clip demuxer; a track takes what its feed says. */
typedef struct DiscTitleSource {
    int          nb_clips;
    const char  *clip_kind;                                  /**< for the log: "EVOB" */
    const char *(*clip_name)(DrDisc *d, int clip);
    uint32_t    (*clip_blocks)(DrDisc *d, int clip);  /**< blocks of the clip (2048 / 6144 bytes) */
    /** Block of the clip into buf (decrypted): 1 read, 0 not usable (left
     *  out), < 0 cannot be read (the rest of the clip is not read). */
    int         (*read_block)(DrDisc *d, int clip, uint32_t block, uint8_t *buf);
    /** Where block of the clip lies on the disc, for the log: the file
     *  (name into file, room for size) and the byte offset in it; < 0 not
     *  known. NULL: none. */
    int         (*block_place)(DrDisc *d, int clip, uint32_t block, char *file, int size,
                               int64_t *offset);
    /** The disc's own reader names the damaged ranges (DVD: libdvdread):
     *  the range the latest read_block of the clip ended (flush: the clip's
     *  reading ends, the range being read ends too) into *out; 1 one given,
     *  0 none. NULL: the title reader finds the ranges itself (blocks left
     *  out one after the other and of one kind, block_place). */
    int         (*damage)(DrDisc *d, int clip, int flush, DiscDamage *out);
    /** Where the disc's own structure starts the clip: its first presentation
     *  time (90 kHz, on the clip's clock), checked against the first picture
     *  read from it; AV_NOPTS_VALUE when the structure gives none. NULL: no
     *  check. */
    int64_t     (*clip_start)(DrDisc *d, int clip);
    const char  *clip_start_name;   /**< for the log: "the playlist's IN time" */
    /** The disc's own check of the stream data read (Blu-ray: its AACS
     *  content hashes), at the title's end: logged; returns the damaged
     *  units found (each logged as a warning). NULL: none. */
    int64_t     (*integrity)(DrDisc *d);

    int          ts;                /**< Blu-ray stream data (6144-byte units, MPEG-TS) */
    const int   *pids;              /**< ts: the PIDs read */
    int          nb_pids;
    const DiscTsFeed *feeds;        /**< ts: what each track takes */

    int          lpcm_header;       /**< bytes of a PES packet's LPCM audio frame header (HD DVD 5) */
    const struct DRTitleTrack *tracks;
    int          nb_tracks;
    const int64_t     *chapter_times;   /**< chapter records (ticks from the title start) */
    const char *const *chapter_names;   /**< their names (NULL or "": none), or NULL */
    int          nb_chapters;
    /** Chapter records that are positions (DVD-Video), or NULL (times): each
     *  record's clip (-1: none) and byte offset in it; a chapter starts at
     *  the first video frame from that position (chapter_times stay the
     *  records' times, for the tail check). */
    const int         *chapter_clips;
    const int64_t     *chapter_offsets;

    /** ts: clips of their own (Blu-ray text subtitles), each read whole
     *  before the title's clips; a track whose feed names one (feed.side)
     *  takes its payloads from it. */
    int          nb_sides;
    const char *(*side_name)(DrDisc *d, int side);       /**< for the log: "text subtitle clip ..." */
    uint32_t    (*side_blocks)(DrDisc *d, int side);
    /** Block of the side clip into buf (decrypted): 1 read, 0 not usable
     *  (left out), < 0 cannot be read (the rest of the clip is not read). */
    int         (*side_read)(DrDisc *d, int side, uint32_t block, uint8_t *buf);
    /** How far the side clip's clock lies ahead of the clock of the title's
     *  clip first (the first clip read): *offset in ticks; 1 as the disc
     *  says, 0 not placed that way (*offset 0; the format part says why). */
    int         (*side_offset)(DrDisc *d, int side, int first, int64_t *offset);
} DiscTitleSource;

/** One disc format's part. */
typedef struct DiscFormat {
    const char             *name;       /**< the disc's kind for the caller ("HD DVD") */
    const char             *log_name;   /**< the prefix of its log lines ("DVD") */
    enum DiscIODiscFormat   kind;
    size_t                  priv_size;  /**< the part's own context, zeroed before load */

    /* The title entries, the same for every format; the disc layer runs
     * the flow (disc.c): load, each title's probe, tracks and stream
     * pass (pass.c), the titles left out, the disc info, the opened
     * title. Titles are numbered from 0 in the part's current list. */

    /** Read the disc's structure from DiscContext.fs (its keys, the title
     *  list with each title's tracks by the format's own rules); the disc
     *  info's disc-wide fields (name, label, encrypted). */
    int  (*load)(DrDisc *d);
    int  (*nb_titles)(DrDisc *d);
    /** Title i's name for the log. */
    const char *(*title_name)(DrDisc *d, int i);
    /** Title i's own probe before its stream pass, or NULL: < 0 the title
     *  cannot be used. */
    int  (*title_probe)(DrDisc *d, int i);
    /** Title i's tracks into out (room for out_size): their count, < 0
     *  error. */
    int  (*title_tracks)(DrDisc *d, int i, DiscTrack *out, int out_size);
    /** The segment title i's stream pass reads (its largest) into src: 1,
     *  0 none (no pass), < 0 error. Released with segment_close (may be
     *  NULL). */
    int  (*title_segment)(DrDisc *d, int i, DiscPassSource *src);
    void (*segment_close)(DrDisc *d, DiscPassSource *src);
    /** Title i left out of the part's list (the titles after it move up). */
    void (*remove_title)(DrDisc *d, int i);
    /** The disc info's disc-wide fields known once every title's stream
     *  pass has read (what protection the reading met), or NULL. */
    int  (*disc_done)(DrDisc *d);
    /** Title i's disc info but its streams: name, length, size, files,
     *  chapters, parts. */
    int  (*title_info)(DrDisc *d, int i, DiscTitleInfo *dt);
    /** Open title i (with part >= 0: that part of it alone): DiscContext.src
     *  but its tracks (clips, block reader, chapters, LPCM header, ts),
     *  s->duration; *second_view: the second view of a 3-D
     *  title is read. */
    int  (*open_title)(DrDisc *d, int i, int part, int *second_view);
    /** The opened title's own state given back (another title can be
     *  opened after it); also after a failed open_title. */
    void (*close_title)(DrDisc *d);
    /** Also after a failed load (the context as far as it came). */
    void (*close)(DrDisc *d);
} DiscFormat;

typedef struct DiscTitleReader DiscTitleReader;

/** The disc context (the public face's handle). */
struct DrDisc {
    const char       *source;           /**< the path the disc was opened at */
    const char       *log;              /**< the source name its log lines carry (base/vlog.h) */

    /* options, for every format part */
    int               opt_read_attempts;
    int               opt_udf_reader;
    int               opt_prefer_iso;   /**< DVD images: an old UDF 1.02 read through ISO 9660 (discio.h) */
    int               opt_title;        /**< -1: every title listed, none read; then the opened title
                                             (disc_title_open()) */
    int               opt_part;         /**< -1: the whole title, else that part of it (DiscTitleInfo.parts) */
    int               opt_second_view;  /**< 3-D titles: the second view is read (a track of its own) */
    const uint8_t    *keep;             /**< the opened title's selected tracks (disc_title_open()), NULL all */
    int               opt_details;      /**< the title list (info) reads each track's details from the stream */
    int               opt_dolby_vision; /**< Dolby Vision enhancement layer: 0 merged into the video's frames,
                                             1 a track of its own */
    int               opt_min_length;
    int               opt_dvd_cell_mode;  /**< DVD: a title's cells (DVD_CELL_MODE_*) */
    int               opt_dvd_cell_check; /**< DVD: the VOBUs a full cell check reads (DVD_CELL_CHECK_*) */
    char             *opt_cc_helper;    /**< the caption converter program (NULL: the one next to the program) */
    char             *opt_video_language; /**< the language of video tracks the disc names none for: an ISO
                                             639 code, "market" (the disc's market language, else eng),
                                             NULL / "": none */
    char              market[4];        /**< the disc's market language (its format part, at load), "" none */
    const char       *market_from;      /**< where it comes from (for the disc info) */
    char            **opt_keydb;
    unsigned          nb_opt_keydb;
    int64_t           progress;         /**< exported: how far the title is read (1/10000) */

    /* the disc */
    int               dvd_flat;         /**< DVD: VIDEO_TS.IFO at the top of the folder (a VIDEO_TS or .dvdmedia
                                             folder opened itself) */
    DiscIOSource     *image;
    DiscIOFS         *fs;
    const DiscFormat *fmt;
    void             *part;             /**< the format part's context */
    DiscInfo          info;             /**< the disc info (disc_info()) */

    /* the opened title */
    DiscTitleSource   src;              /**< filled by the format part (nb_clips 0: no title); its tracks, feeds
                                             and PIDs by ff_disc_open_tracks() */
    struct DRTitleTrack *tracks;        /**< src.tracks, the disc layer's own */
    int              *track_ids;        /**< per track: PS its stream id, TS PID | key << 16 */
    DiscTsFeed       *feeds;            /**< src.feeds */
    int              *pids;             /**< src.pids */
    DiscPassCache    *pass_cache;       /**< the stream pass's results per segment */
    int               untested;         /**< the opened title's tracks not met on a real disc yet */
    DiscTrack       **title_tracks;     /**< each title's tracks after its stream pass */
    int              *nb_title_tracks;
    int               nb_title_slots;   /**< the arrays' size */
    DiscTitleReader  *reader;

    /* the opened title's public face (disc/disc.h): track i of the title
     * reader is out_tracks[i]; the results fill at the title's end */
    DrDiscTrack      *out_tracks;
    int               nb_out_tracks;
    uint8_t         **track_extradata;  /**< what out_tracks' extradata points at (owned) */
    DrDiscChapter    *chapters;         /**< after the title was read (names: the format part's records) */
    int               nb_chapters;
    int64_t           duration;         /**< the read title's length (ticks) */
    int64_t           damaged_ranges, damaged_bytes;
    char            **damage;           /**< the damaged ranges in words, one each (owned) */
    int               nb_damage;
    int64_t           untested_total;   /**< untested features met (the timeline's + the tracks') */
};

/** The format part's context of d. */
static inline void *ff_disc_part(DrDisc *d)
{
    return d->part;
}

/** A new title at the end of the disc info (zeroed but for its index);
 *  NULL: out of memory. Strings are the format part's, kept until close;
 *  a title is finished before the next one is added. */
DiscTitleInfo *ff_disc_add_title(DrDisc *d);
/** A new stream (zeroed, no linked streams) / chapter / file at the end of
 *  title t; NULL / < 0: out of memory. */
DiscStreamInfo *ff_disc_title_add_stream(DiscTitleInfo *t);
DiscChapterInfo *ff_disc_title_add_chapter(DiscTitleInfo *t);
DiscPartInfo *ff_disc_title_add_part(DiscTitleInfo *t);
int ff_disc_title_add_file(DiscTitleInfo *t, const char *name);
/** A property of title t (DiscTitleInfo.props): what it is (key, its name
 *  disc_prop_name(key)), the value formatted; < 0: out of memory. */
int ff_disc_title_add_prop(DiscTitleInfo *t, DiscPropKey key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/** The disc's market language (DiscContext.market) from the languages its
 *  format names for the disc itself (Blu-ray: its disc library files; DVD:
 *  its menus; HD DVD: its title sets' default language), ISO 639 codes:
 *  one language: that one; several: the first as the disc lists them
 *  (ordered), else (an unordered list) English when it is among them, else
 *  the first. from: where they come from (for the disc info). */
void ff_disc_set_market(DrDisc *d, const char (*codes)[4], int n, int ordered, const char *from);
/** A property of the disc (DiscInfo.props), as ff_disc_title_add_prop(). */
int ff_disc_add_prop(DrDisc *d, DiscPropKey key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/** A stream as ff_disc_title_add_stream() starts it: zeroed, no linked
 *  streams, no offset sequence, profile and level unknown. */
void ff_disc_stream_init(DiscStreamInfo *k);
/** The disc info's streams of title t: each track's info, in order. */
int  ff_disc_title_add_tracks(DiscTitleInfo *t, const DiscTrack *tracks, int nb_tracks);
/** The opened title's streams and the title reader's tracks
 *  (DiscContext.src: tracks, nb_tracks; transport stream: ts, feeds, pids)
 *  from its tracks. Stream i is track i of what is read: a track left out
 *  (not selected; not read yet; a second view not read) has none and is
 *  logged; a Dolby Vision enhancement layer comes last (its own stream only
 *  with option dolby_vision = separate). title: its name, for the log; ts:
 *  transport stream; second_view: the second view of a 3-D video is read;
 *  keep: NULL every track, else keep[i] 0: track i not selected (track 0,
 *  the video, and a video with a Dolby Vision layer are always read). */
int  ff_disc_open_tracks(DrDisc *d, const char *title, const DiscTrack *tracks, int nb_tracks, int ts,
                         int second_view, const uint8_t *keep);

/* ---- pass.c ---- */

/** The stream pass of a title's tracks over src (its largest segment, see
 *  pass.c): duplicates left out; with details, each video and audio
 *  track's details from its stream; an audio track without data left out.
 *  AVERROR_INVALIDDATA: the video has no data (the title cannot be used);
 *  < 0 otherwise: a block cannot be read. */
int  ff_disc_stream_pass(DrDisc *d, const char *title, const DiscPassSource *src, DiscTrack *tracks,
                         int *nb_tracks, int details);
void ff_disc_pass_cache_free(DiscPassCache **pc);

/* ---- title_reader.c ---- */

/** The title reader (DrDisc.reader) over src: the title timeline for
 *  its tracks and chapters; reading starts at its first clip. */
int  ff_disc_title_reader_open(DrDisc *d, const DiscTitleSource *src);
/** The next frame of the title timeline; at the end (DR_ERR_EOF) the
 *  chapters, the duration and each track's result are set. */
int  ff_disc_title_reader_frame(DrDisc *d, DrDiscFrame *f);
void ff_disc_title_reader_close(DiscTitleReader **reader);

extern const DiscFormat ff_disc_dvd;
extern const DiscFormat ff_disc_hddvd;
extern const DiscFormat ff_disc_bluray;

#endif /* DISC_DISC_MODEL_H */
