/*
 * The disc layer's public face: open a disc once (disc kind, title list with
 * each title's streams = the disc info), open a title (or one part of it)
 * on it any number of times, read the title timeline's frames, and take
 * each track's typed result at the end. The model the overview shows is
 * disc/info.h; the frames come straight from the title timeline (base
 * buffers, times in DR_TICKS_PER_SECOND ticks).
 */

#ifndef DISC_DISC_DISC_H
#define DISC_DISC_DISC_H

#include <stdint.h>

#include "base/buf.h"
#include "disc/info.h"
#include "timeline/disctl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DrDisc DrDisc;

/** How a disc is opened (the caller fills it from its settings; zeroed =
 *  the defaults in words below). */
typedef struct DrDiscOptions {
    int         title;           /**< the title to open (-1 = none: only the disc info) */
    int         part;            /**< with title: only that part of it, from 0; -1 = the whole title */
    int         details;         /**< the title list: read each track's details from the stream */
    int         dolby_vision;    /**< 0 enhancement layer merged into the video's frames, 1 a track of its own */
    int         second_view;     /**< 3-D titles: read the second view as a track of its own */
    int         min_length;      /**< titles shorter than this (seconds) are left out of the title list */
    int         dvd_cell_mode;   /**< DVD_CELL_MODE_* (dvd_internal.h): 0 auto */
    int         dvd_cell_check;  /**< DVD_CELL_CHECK_*: 0 all */
    const char *cc_helper;       /**< the caption converter program; NULL: the one next to the program */
    const char *video_language;  /**< video tracks the disc names no language for: an ISO 639 code, or
                                      "market"; NULL / "": none */
    const char *const *keydb;    /**< AACS key files (KEYDB.cfg), read in this order */
    int         nb_keydb;
    int         read_attempts;   /**< read attempts per request on the disc; 0: the readers' default */
    int         udf_reader;      /**< DISCIO_UDF_* (io/discio.h) */
    int         prefer_iso;      /**< disc images: an old UDF 1.02 read through ISO 9660 */
} DrDiscOptions;

/** A track of the opened title, as its stream file is written: track i of
 *  dr_disc_nb_tracks() gets frame i's bytes. The result fields hold once
 *  the title has been read to its end (dr_disc_read() gave DR_ERR_EOF). */
typedef struct DrDiscTrack {
    int         number;          /**< the disc info's track number (file names, logs) */
    int         type;            /**< enum DiscMediaKind */
    int         codec;           /**< DRCodecID of what leaves (LPCM leaves as PCM_S16LE / _S24LE) */
    int         profile;         /**< the codec's profile, DISC_PROFILE_UNKNOWN */
    char        language[4];     /**< ISO 639-2, "" none */
    int         width, height;   /**< subtitles: the canvas the units are drawn on */
    /* audio (LPCM: set when its first frame comes) */
    int         channels, sample_rate, bits;
    uint64_t    layout;          /**< channel layout mask, 0 unknown */
    /* the stream's header for its file (VobSub: the index header text;
     * text subtitles: the dialog style segment, set when the first dialog
     * comes) */
    const uint8_t *extradata;
    int            extradata_size;
    /* what the track is */
    unsigned    forced:1;        /**< forced subtitles (only those, or marked forced) */
    unsigned    commentary:1, visually_impaired:1;
    unsigned    second_view:1;   /**< the second view of a 3-D video (written as MVC) */
    const char *eye;             /**< 3-D video: the eye it shows, or NULL */
    const char *dolby_vision;    /**< "BL+EL+RPU" (merged), "BL", "EL", or NULL */
    const char *picture;         /**< subtitles: what the units are, or NULL */
    const char *caption;         /**< a caption track: what it is made from, or NULL */
    const char *line21;          /**< video: the line-21 caption fields the disc marks, or NULL */
    unsigned    caption_left:1;  /**< video: its caption track was not selected (the data is dropped as asked) */
    /* the results (after DR_ERR_EOF) */
    int64_t     frames;          /**< frames (subtitles: units; captions: blocks) given out */
    int64_t     warnings;
    int64_t     delay_ticks;     /**< audio: its first frame's time (the start delay) */
    int64_t     cc_blocks, cc_text;      /**< video: line-21 data taken out and dropped */
    int64_t     cues, cc_lost;           /**< a caption track: SRT cues made, captions lost at segment ends */
    int64_t     no_stop, outside;        /**< subtitles: units without a stop time, units outside the canvas */
    unsigned    has_cc:1, has_spu:1, has_forced_objects:1;
    int64_t     forced_objects;          /**< PGS: forced objects shown (with has_forced_objects) */
} DrDiscTrack;

/** A chapter of the read title (after DR_ERR_EOF): ticks on the title
 *  timeline. */
typedef struct DrDiscChapter {
    int64_t     start, end;      /**< DR_TICKS_PER_SECOND ticks */
    const char *name;            /**< NULL: none */
} DrDiscChapter;

/** A frame of the title timeline: data/size point into buf (one reference,
 *  given to the caller: dr_disc_frame_unref(), or keep your own with
 *  dr_buf_ref()). */
typedef struct DrDiscFrame {
    DrBuf      *buf;
    uint8_t    *data;
    int         size;
    int         track;           /**< its track (dr_disc_track()) */
    int64_t     time;            /**< DR_TICKS_PER_SECOND ticks, DR_NOPTS_VALUE none */
    int64_t     duration;        /**< ticks, 0 not known */
    unsigned    key:1, discard:1;
} DrDiscFrame;

/** Opens the disc at source (an image file, or a disc folder): the disc
 *  info is read (and with o->title >= 0 that title opened). log: the source
 *  name its log lines carry (base/vlog.h; NULL: "disc"). 0, or a negative
 *  base error (the disc is closed). */
int  dr_disc_open(DrDisc **disc, const char *source, const DrDiscOptions *o, const char *log);
void dr_disc_close(DrDisc **disc);

/** The disc info; valid until the disc is closed. */
const DiscInfo *dr_disc_info(const DrDisc *d);

/** Opens title `title` (part >= 0: only that part) for reading; a title
 *  open before is closed first. The disc stays loaded. second_view: a 3-D
 *  title's second view is read too (when one of its tracks is kept). keep:
 *  NULL every track, else keep[i] 0 leaves track i of the title's streams
 *  out (track 0, the video, is always read). 0, or a negative error (the
 *  title is closed). */
int  dr_disc_title_open(DrDisc *d, int title, int part, int second_view, const uint8_t *keep);
void dr_disc_title_close(DrDisc *d);

/** The opened title's tracks (frame i's bytes belong to track i). The
 *  pointer stays valid until the title is closed; LPCM details and text
 *  subtitle headers fill in as frames come. */
int  dr_disc_nb_tracks(const DrDisc *d);
const DrDiscTrack *dr_disc_track(const DrDisc *d, int i);

/** The next frame of the title timeline: 0 (f filled; unref it), DR_ERR_EOF
 *  at the title's end (the chapters and every result are set then), or a
 *  negative error. */
int  dr_disc_read(DrDisc *d, DrDiscFrame *f);
void dr_disc_frame_unref(DrDiscFrame *f);

/** How far the title is read (1/10000). */
int64_t dr_disc_progress(const DrDisc *d);

/** After DR_ERR_EOF: the title's chapters, its length (ticks), and the
 *  title-wide results. */
int  dr_disc_nb_chapters(const DrDisc *d);
const DrDiscChapter *dr_disc_chapter(const DrDisc *d, int i);
int64_t dr_disc_title_duration(const DrDisc *d);
/** Untested features met reading the title (said in the log). */
int64_t dr_disc_untested(const DrDisc *d);
/** Damaged data left out: the ranges, their bytes, and the ranges in words
 *  (one per line; "" none). */
int64_t     dr_disc_damaged_ranges(const DrDisc *d);
int64_t     dr_disc_damaged_bytes(const DrDisc *d);
const char *dr_disc_damage(const DrDisc *d);

#ifdef __cplusplus
}
#endif

#endif /* DISC_DISC_DISC_H */
