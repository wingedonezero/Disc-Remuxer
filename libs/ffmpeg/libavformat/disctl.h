/*
 * The title timeline for disc formats (DVD-Video, HD DVD, Blu-ray): units,
 * timing, joining and checks of a title's tracks. Design: docs/TITLE_TIMELINE.md
 * of the Disc-Remuxer repository.
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

#ifndef AVFORMAT_DISCTL_H
#define AVFORMAT_DISCTL_H

#include <stdint.h>

#include "libavcodec/codec_id.h"
#include "libavutil/channel_layout.h"
#include "libavutil/buffer.h"

/* Time: one unit = 1/1,080,000,000 s (27 MHz x 40). Exact for 90 kHz PES
 * times, for 1001-based frame and field durations and for 48 / 96 / 192 kHz
 * samples. A missing time is AV_NOPTS_VALUE. */
#define DR_TICKS_PER_SECOND 1080000000LL
#define DR_TICKS_PER_PTS    12000          /* 90 kHz -> ticks */

/* ---- Frames: the units the stages after stage 1 hand on ---- */

#define DR_F_KEY      0x0001   /**< key frame / random access point */
#define DR_F_SYNC     0x0002   /**< a unit at which audio may be dropped or joined */
#define DR_F_MARKER   0x0004   /**< empty marker: carries a time, no bytes */
#define DR_F_DISCARD  0x0008   /**< not referenced (B picture) */
#define DR_F_CHAPTER  0x0010   /**< a chapter starts here */
#define DR_F_TAIL     0x0020   /**< read from the last 64 MiB of its segment's source */
#define DR_F_BATCH    0x0040   /**< video: the last frame of a batch the video timing handed on */
#define DR_F_NO_STOP  0x0080   /**< sub-picture: shows a picture but has no stop time: out of
                                    its stage it lasts one delay step; the title pipeline makes
                                    it last until the track's next unit starts (the last one:
                                    until the video's end) */
#define DR_F_SEQ_END  0x0100   /**< video (HEVC): the frame carries an end of sequence / end of
                                    bitstream unit; the title timeline decides on it when it
                                    knows the next picture (ff_disctl_hevc_seq_end) */

typedef struct DRFrame {
    AVBufferRef *buf;      /**< owns the bytes (NULL for a marker) */
    uint8_t     *data;
    int          size;
    int64_t      time;     /**< ticks, or AV_NOPTS_VALUE */
    int64_t      dur;      /**< ticks */
    int64_t      pos;      /**< byte offset of the unit in its track's stream */
    unsigned     flags;    /**< DR_F_* */
    int          samples;  /**< samples FFmpeg's parser gave the unit, 0 = none */
    int          rate;     /**< and their sample rate */
    int64_t      src;      /**< from the joiner on: the frame's own time on the title timeline (its
                                PES-derived time, before the joiner moves it or the junction shifts it) */
} DRFrame;

/** Hands a frame on; the callee owns it (ff_disctl_frame_unref). */
typedef int (*DRFrameCb)(void *opaque, DRFrame *frame);

void ff_disctl_frame_unref(DRFrame *frame);

typedef struct DRAudio DRAudio;
typedef struct DRVideo DRVideo;

/* A video unit as the video timing sees it. */
enum DRPictureType {
    DR_PIC_I     = 2,   /**< starts a group, key frame */
    DR_PIC_P     = 3,   /**< reference picture shown after the B pictures before it */
    DR_PIC_B     = 4,   /**< shown before the preceding reference picture */
    DR_PIC_OTHER = 5,   /**< not a picture (left out of the video track) */
};

typedef struct DRPicture {
    int type;           /**< DRPictureType */
    int order;          /**< display-order number (wraps at the codec's period) */
    int fields;         /**< duration in fields (2 = one frame) */
    int key;            /**< a key frame even when not an I picture (e.g. VC-1 sequence header + entry point) */
    int rate_num, rate_den;     /**< the frame rate the unit's headers give (0: none) */
} DRPicture;

/* What the core needs to know about a video codec. */
typedef struct DRVideoRules {
    /** The unit's picture values (pic comes filled in with what the parser
     *  gave, DRCodec.parser_picture); may set the frame rate
     *  (ff_disctl_video_set_rate) from headers it carries; < 0 on failure. */
    int  (*picture)(DRVideo *v, const DRFrame *unit, DRPicture *pic);
    int    order_period;   /**< wrap period of the display-order number */
    /** 1: the codec carries no display-order number; it is counted: B
     *  pictures after a reference picture are numbered before it, I
     *  pictures restart the count (VC-1). */
    int    counted_order;
    /** 1: the key frames are the pictures the codec rule marks (DRPicture.key),
     *  not every I picture (H.264: IDR pictures and recovery points). */
    int    codec_keys;
    /** Takes bytes out of a unit before picture() sees it (shrinking
     *  unit->size): pieces that leave the video as their own track go to
     *  ff_disctl_video_side(), others are dropped; < 0 on failure. NULL:
     *  none. */
    int  (*split)(DRVideo *v, DRFrame *unit);
    size_t priv_size;
    void (*close)(void *priv);
} DRVideoRules;

/* The stream values every unit's duration is computed from. */
typedef struct DRAudioHeader {
    int rate;              /**< samples per second */
    int samples;           /**< samples per unit */
} DRAudioHeader;

/* What the core needs to know about an audio codec. */
typedef struct DRAudioRules {
    /** The stream's values from a unit; < 0 when the unit gives none. The
     *  first unit that is a sync unit and gives them sets them for the
     *  segment. */
    int  (*header)(DRAudio *a, const DRFrame *unit, DRAudioHeader *h);
    /** 1 when the unit is a sync unit (DR_F_SYNC). NULL: never. */
    int  (*sync)(const uint8_t *data, int size);
    /** The unit's duration in ticks; < 0 on failure. NULL: samples / rate
     *  of the header. */
    int  (*duration)(DRAudio *a, DRFrame *frame);
    /** The FFmpeg bitstream filter each unit goes through after its
     *  duration, for a track of these audio flags (e.g. eac3_core: an AC-3
     *  frame without the E-AC-3 frames after it), NULL: none. */
    const char *(*filter)(int flags);
    /** Checks a unit for features that are implemented but not met on real
     *  discs yet (ff_disctl_audio_review). NULL: none. */
    void (*inspect)(DRAudio *a, const DRFrame *frame);
    /** 1: only sync units are key frames (DR_F_KEY); 0: every unit is. */
    int    key_on_sync;
    /** Rule state (ff_disctl_audio_priv), freed with close(). */
    size_t priv_size;
    void (*close)(void *priv);
} DRAudioRules;

/* One entry of the codec table: what the core needs to know about a codec.
 * A codec without an entry is refused (the track is not ripped). Entries
 * grow with the stages; a missing function means the rule is not known yet
 * and is never guessed. */
typedef struct DRCodec {
    enum AVCodecID id;
    const char    *name;
    /** Offset inside a unit where the part that takes a PES timestamp starts
     *  (MPEG-2 video: the picture start code; VC-1: the frame start code),
     *  or -1 when the unit has none. NULL: the unit's first byte (audio
     *  frames and sub-picture units: every unit can take a time). */
    int (*anchor)(const uint8_t *data, int size);
    /** 1 when the parser's output is a unit of the codec, 0 when it is not
     *  (bytes before the first sync word, bytes between units): such bytes
     *  are left out and take no time. NULL: every output is a unit. */
    int (*check)(const uint8_t *data, int size);
    /** Audio rules; NULL for video / subtitles, or while not implemented
     *  (the track is then refused by ff_disctl_audio_open). */
    const DRAudioRules *audio;
    /** Video rules; NULL for audio / subtitles (or while not implemented). */
    const DRVideoRules *video;
    /** 1: cut by FFmpeg's parser in disc mode (PARSER_FLAG_DISC), which
     *  knows the reference's cut rule and says which of its chunks are no
     *  unit (AVCodecParserContext.disc_status). */
    int disc_parser;
    /** 1: the disc-mode parser gives each unit's picture values
     *  (pict_type, key_frame, output_picture_number, disc_fields, the frame
     *  rate): they come with the unit (DRUnit.pic). */
    int parser_picture;
    /** The check of an output unit (stage 5): DR_UNIT_OK, DR_UNIT_BAD (sync
     *  word or size wrong), DR_UNIT_CRC (a checksum fails). NULL: not
     *  checked. */
    int (*verify)(const uint8_t *data, int size);
} DRCodec;

enum { DR_UNIT_OK = 0, DR_UNIT_BAD = 1, DR_UNIT_CRC = 2 };


/** The codec table entry of a codec, or NULL (the codec is not supported). */
const DRCodec *ff_disctl_codec(enum AVCodecID id);

/* ---- Stage 1: units and timestamp records (disctl_frames.c) ---- */

typedef struct DRUnit {
    const uint8_t *data;   /**< the unit's bytes, valid during the callback */
    int            size;
    int64_t        time;   /**< ticks, or AV_NOPTS_VALUE */
    int64_t        pos;    /**< byte offset of the unit's first byte in the track's stream */
    int            samples;/**< samples FFmpeg's parser gives the unit, 0 = none */
    int            rate;   /**< and their sample rate */
    const DRPicture *pic;  /**< the picture values the parser gives (DRCodec.parser_picture), or NULL */
} DRUnit;

/** Called for every unit, in stream order. A negative return stops the
 *  frame step and is returned by the call that produced the unit. */
typedef int (*DRUnitCb)(void *opaque, const DRUnit *unit);

typedef struct DRFramesStats {
    int64_t bytes;         /**< payload bytes written */
    int64_t records;       /**< payloads written with a time */
    int64_t units;         /**< units given to the callback */
    int64_t timed;         /**< of them with a time */
    int64_t records_unused;/**< records dropped without giving their time to a unit */
    int64_t skipped;       /**< parser outputs that are not units (left out) */
    int64_t skipped_bytes;
} DRFramesStats;

typedef struct DRFrames DRFrames;

/**
 * The frame step for one track of one segment: payloads in, units out (found by
 * FFmpeg's parser, each with its PES time).
 * Units are cut by FFmpeg's parser for the codec; a unit takes the time of a
 * payload when the unit's anchor (see DRCodec) is the first one that lies in
 * that payload's bytes (ISO/IEC 13818-1 2.4.3.7: a PES packet's PTS belongs
 * to the first access unit that starts in it).
 *
 * @param logctx for av_log
 * @return 0, AVERROR(ENOSYS) when the codec has no table entry or no parser
 */
int ff_disctl_frames_open(DRFrames **frames, void *logctx, enum AVCodecID codec,
                           DRUnitCb cb, void *opaque);

/** One payload; time in ticks or AV_NOPTS_VALUE. */
int ff_disctl_frames_write(DRFrames *c, const uint8_t *data, int size, int64_t time);

/** The end of the segment: the units still buffered in the parser. */
int ff_disctl_frames_flush(DRFrames *c);

void ff_disctl_frames_stats(const DRFrames *c, DRFramesStats *stats);

/** The stream carries an extension (DTS-HD): FFmpeg's parser takes every
 *  variant of the codec (PARSER_FLAG_DISC_ANY). */
void ff_disctl_frames_take_any(DRFrames *c);

/** Linear PCM: the frame format for FFmpeg's parser (from the packets'
 *  audio frame headers): channels, coded bits per sample, samples per
 *  frame. */
void ff_disctl_frames_set_pcm_format(DRFrames *c, int channels, int bits, int samples_per_frame);

void ff_disctl_frames_close(DRFrames **frames);

/* ---- Stage 2, audio: units timed within one segment (disctl_audio.c) ---- */

#define DR_AUDIO_EXTENDED  0x0002  /**< the stream carries an extension (DTS-HD): its frames are found
                                        with the extension's rules (the track whose stream it is, and
                                        the one taken out of it) */
#define DR_AUDIO_EXTRACTED 0x0001  /**< the track is a stream taken out of another one (DTS out of
                                        DTS-HD): the codec's filter keeps only its frames */

typedef struct DRAudioStats {
    int64_t units;         /**< units written */
    int64_t frames;        /**< frames handed on (units and markers) */
    int64_t markers;
    int64_t cut_bytes;     /**< bytes removed by core cuts */
    int64_t review;        /**< untested features met: the job must be looked at */
    int64_t continuity;    /**< unit times that differ from the previous unit's end */
    DRAudioHeader header;
} DRAudioStats;

/**
 * Audio timing of one track in one segment. Units come from the track's
 * frame step (ff_disctl_audio_unit as its callback); timed frames go to cb.
 * @return 0, AVERROR(ENOSYS) when the codec's audio rules are missing
 */
int ff_disctl_audio_open(DRAudio **audio, void *logctx, enum AVCodecID codec, int flags,
                          DRFrameCb cb, void *opaque);

/** A DRUnitCb: opaque = the DRAudio. */
int ff_disctl_audio_unit(void *audio, const DRUnit *unit);

/** An empty marker at a time (no bytes); it keeps its place in the stream. */
int ff_disctl_audio_marker(DRAudio *a, int64_t time);

/** The end of the segment. Fails when no unit ever gave the stream's header. */
int ff_disctl_audio_flush(DRAudio *a);

/** A feature implemented but not met on real discs: logged once per kind as
 *  an error, counted in the statistics (the job is then marked failed). */
void ff_disctl_audio_review(DRAudio *a, unsigned kind, const char *what);

void ff_disctl_audio_stats(const DRAudio *a, DRAudioStats *stats);

void ff_disctl_audio_close(DRAudio **audio);
/** A codec state the audio rules read (LPCM: the DRLpcm of the track);
 *  owned by the caller. */
void ff_disctl_audio_set_state(DRAudio *a, void *state);
void *ff_disctl_audio_state(const DRAudio *a);

/* the audio stream values the rules use (for rules that keep state) */
const DRAudioHeader *ff_disctl_audio_header(const DRAudio *a);
int ff_disctl_audio_flags(const DRAudio *a);
void *ff_disctl_audio_priv(DRAudio *a);
void *ff_disctl_audio_log(const DRAudio *a);

/* ---- Stage 4, audio: the junction of one output track (disctl_junction.c) ----
 * Title start (lead-in shift / drop), then per frame: overlaps grow the
 * audio skew, short gaps are absorbed into it, real gaps reset it (nothing is
 * inserted), whole frames are dropped (only up to a sync unit) once the skew
 * reaches the start shift + one frame. Every frame leaves at time + skew. */

enum DREventKind {
    DR_EV_START_GAP = 1,   /**< first frame 0.5..3 ms after the video start: kept */
    DR_EV_START_DROP,      /**< frames before the lead-in tolerance dropped (count may be 0) */
    DR_EV_START_SHIFT,     /**< lead-in kept, the whole track delayed by it */
    DR_EV_OVERLAP,         /**< a frame starts before the previous one ended: skew grows */
    DR_EV_GAP_ABSORBED,    /**< a short gap taken from the skew */
    DR_EV_GAP,             /**< a real gap: missing frames, skew reset, nothing inserted */
    DR_EV_DROP,            /**< frames dropped to reduce the skew */
    DR_EV_GAP_MARKER,      /**< a marker 1 s further into a long gap while the video goes on */
    DR_EV_VIDEO_ENDED,     /**< the video ends before this frame: the rest of the track is left out */
    DR_EV_TIME_ORDER,      /**< a frame leaves at or before the previous one (logged only) */
    DR_EV_RETIME,          /**< joiner: a frame early by more than its duration moved to the expected time */
    DR_EV_VIDEO_TIMECODE,  /**< a picture's PES time differs from its grid time by >= 0.1 ms (dur = difference) */
    DR_EV_VIDEO_TIMECODE_LIMIT, /**< 40 different differences reported: no more */
    DR_EV_VIDEO_INVALID,   /**< end of the segment: count = pictures whose PES time was off the grid */
    DR_EV_VIDEO_REPAIR,    /**< the grid followed a jump of the PES times (count = placeholders) */
    DR_EV_VIDEO_RATE_CHANGE, /**< a later header states another frame rate (count = num, dur = den): warning only */
    DR_EV_SUB_UNTIMED,     /**< a sub-picture unit without a time of its own left out (pos = its byte offset) */
    DR_EV_SUB_EARLY,       /**< a sub-picture unit before the video's first field left out (pos = its PES time) */
    DR_EV_SUB_OVERLAP,     /**< joiner: a sub-picture starts before the one before it ends, by more than
                                its own duration (dur = by how much): it keeps its time */
    /* stage 5 checks (findings, nothing is changed) */
    DR_EV_VERIFY_UNIT,     /**< an output unit does not parse (sync word, size) */
    DR_EV_VERIFY_CRC,      /**< an output unit fails its checksum */
    DR_EV_VERIFY_ORDER,    /**< a unit without a duration, or (audio, subtitles) not after the one before it */
    DR_EV_VERIFY_HOLE,     /**< video in display order: no picture for dur from pos (count = placeholders in it) */
    DR_EV_VERIFY_OVERLAP,  /**< video in display order: a picture starts dur before the one before it ends */
    DR_EV_SEAMLESS_SEARCH, /**< the overlap search at a join (pos = the next segment's first sync unit on
                                the input clock): count = the best correlation of any candidate (Q32,
                                0 = not compared), dur = that candidate's frame count, skew = 1 when a
                                match was accepted */
    DR_EV_SEAMLESS_DROP,   /**< a match: count frames (dur ticks) at the end of the earlier segment dropped,
                                skew = the new skew */
    DR_EV_PCM_SILENCE,     /**< PCM: a gap filled with silence (pos = the output time, dur = the gap in ticks,
                                count = samples) */
    DR_EV_PCM_SKIP,        /**< PCM: a start 5 s or more after the video skipped on the output clock (dur ticks,
                                count samples), nothing written */
    DR_EV_PCM_TIMECODE,    /**< PCM: frames with a broken time appended where they follow on (pos = the first
                                one's time, dur = the apparent skew, count = frames) */
};

typedef struct DREvent {
    int     kind;          /**< DREventKind */
    int     track;
    int64_t pos;           /**< ticks: where (title timeline) */
    int64_t dur;           /**< ticks: how much (overlap, gap, dropped duration, lead-in) */
    int64_t skew;          /**< ticks: the audio skew after the event */
    int64_t count;         /**< frames (DROP / START_DROP); x1000 missing frames (GAP) */
} DREvent;

typedef void (*DREventCb)(void *opaque, const DREvent *ev);

/* The video track as the audio junction sees it. */
typedef struct DRVideoRef {
    void    *opaque;
    /** the highest video time handed on so far */
    int64_t (*max_time)(void *opaque);
    /** read the video on until max_time() >= target or the video ends;
     *  *ended = 1 when it ended before target; < 0 on error */
    int     (*advance)(void *opaque, int64_t target, int *ended);
} DRVideoRef;

typedef struct DRJunctionConfig {
    int        track;          /**< output track number (events) */
    int64_t    frame_dur;      /**< the stream's nominal frame duration (ticks) */
    int64_t    tolerance;      /**< format's audio lead-in tolerance (ticks); at least 1 ms is used */
    DRVideoRef video;
    DRFrameCb  out;  void *out_opaque;
    DREventCb  event; void *event_opaque;
    /** The stream's codec and sample rate: the seamless overlap search
     *  decodes TrueHD / MLP to compare the two sides of a join (other
     *  codecs, or rate 0: no comparison, durations only). */
    enum AVCodecID codec;
    int        rate;
} DRJunctionConfig;

typedef struct DRJunctionStats {
    int64_t in, out, dropped, dropped_dur, markers;
    int64_t skew, base;        /**< current skew and start shift */
    int     ended;             /**< the video ended before the track */
} DRJunctionStats;

typedef struct DRJunction DRJunction;

int  ff_disctl_junction_open(DRJunction **j, void *logctx, const DRJunctionConfig *cfg);
/** A frame of the track on the title timeline (taken over); processes as
 *  far as the frames already given allow. */
int  ff_disctl_junction_push(DRJunction *j, DRFrame *frame);
/** The end of the track: processes the rest. */
int  ff_disctl_junction_finish(DRJunction *j);
void ff_disctl_junction_stats(const DRJunction *j, DRJunctionStats *st);
void ff_disctl_junction_close(DRJunction **j);

/* ---- Stage 3: the joiner (disctl_join.c) ----
 * A title's segments one after another on the title timeline. Track 0 is the
 * master video. Segment 0 starts the timeline at its first video time;
 * segment k is placed where the video of segment k-1 really ended. Frames are
 * given segment by segment, in order. Audio earlier than expected by more
 * than its duration is moved to the expected time; sub-pictures and the
 * pictures of a second layer keep their times (a sub-picture replaces the
 * one shown before it; a layer's pictures come in decoding order). */

/** DR_KIND_CAPTION: line-21 closed captions taken out of the title's MPEG-2
 *  video (no payloads of its own), given out as text cues. DR_KIND_LAYER:
 *  a second layer of the master video (the dependent view of a 3-D video):
 *  each payload one picture, cleaned as the master video's units are, given
 *  out with the master video frame that has its time (ff_disctl_title_*). */
enum DRTrackKind { DR_KIND_VIDEO = 1, DR_KIND_AUDIO = 2, DR_KIND_SUBTITLE = 3, DR_KIND_CAPTION = 4,
                   DR_KIND_LAYER = 5 };

/** Hands a joined frame of a track on (taken over). */
typedef int (*DRJoinOutCb)(void *opaque, int track, DRFrame *frame);

typedef struct DRJoinConfig {
    int            nb_tracks;
    const int     *kinds;        /**< DRTrackKind per track; track 0 must be video */
    const int64_t *marks;        /**< chapter mark times, ascending (ticks; DRChapterPlan.marks), or NULL */
    int            nb_marks;
    DRJoinOutCb    out;   void *out_opaque;
    DREventCb      event; void *event_opaque;
} DRJoinConfig;

typedef struct DRJoinStats {
    int     segments;
    int64_t frames, retimed, chapters;
    int64_t offset, start;       /**< the current segment's place and first video time */
    int     placed;              /**< the current segment's place is known */
} DRJoinStats;

typedef struct DRJoin DRJoin;

int  ff_disctl_join_open(DRJoin **j, void *logctx, const DRJoinConfig *cfg);
/** The next segment begins (call before its first frame). */
int  ff_disctl_join_segment(DRJoin *j);
/** A frame of a track in the current segment (taken over). */
int  ff_disctl_join_push(DRJoin *j, int track, DRFrame *frame);
/** The end of the title. */
int  ff_disctl_join_finish(DRJoin *j);
void ff_disctl_join_stats(const DRJoin *j, DRJoinStats *st);
/** The times of the frames marked as chapter starts so far (valid until
 *  the joiner is closed); returns their number. */
int  ff_disctl_join_chapters(const DRJoin *j, const int64_t **starts);
void ff_disctl_join_close(DRJoin **j);

/* ---- Chapters (disctl_chapters.c) ----
 * Time-mark chapters (HD DVD, Blu-ray): the disc's chapter records (ticks
 * from the title start, in the disc's order) become the mark times the
 * joiner puts on video key frames (DRJoinConfig.marks); the k-th marked
 * frame starts the k-th chapter. */

#define DR_CHAPTER_00 (-1)     /**< an added first chapter that is no record of the disc */

typedef struct DRChapterPlan {
    int64_t *marks;            /**< ascending mark times (ticks, title timeline) */
    int      nb_marks;
    int     *atoms;            /**< per mark: the record it comes from, or DR_CHAPTER_00 */
    int      nb_atoms;
} DRChapterPlan;

/**
 * The marks of a title's chapter records. A broken tail (a last record at 0
 * after one that is not, or one earlier than the record before it) is cut
 * off; a record earlier than the one before it otherwise refuses the title
 * (AVERROR_INVALIDDATA). Records before skip (leading segments left out) are
 * dropped, the others move back by skip; the first kept one within 0.1 s of
 * the start is at 0; a record at the time of the one before it is dropped.
 * When the first kept record does not start the title, a chapter at 0 is
 * added: record 0 when it was dropped, else (chapter00 set) DR_CHAPTER_00.
 */
int  ff_disctl_chapter_plan(void *logctx, const int64_t *records, int nb_records, int64_t skip, int chapter00,
                             DRChapterPlan *plan);
void ff_disctl_chapter_plan_free(DRChapterPlan *plan);

typedef struct DRChapter {
    int64_t start, end;        /**< ticks, title timeline */
    int     record;            /**< the disc's record, or DR_CHAPTER_00 */
} DRChapter;

/** The chapters of a ripped title: the k-th start time (of the k-th frame
 *  the joiner marked) starts plan atom k; each ends where the next starts,
 *  the last at duration. Fewer than two starts: no chapters (*nb_out = 0).
 *  *out is freed with av_free(). */
int  ff_disctl_chapter_list(const DRChapterPlan *plan, const int64_t *starts, int nb_starts, int64_t duration,
                             DRChapter **out, int *nb_out);

/* ---- Stage 2, video: pictures timed on a fixed grid (disctl_video.c) ----
 * Every picture's time is base + position x field duration; positions are
 * sums of field durations in display order. The base is voted from the
 * pictures' PES times; PES times then only check the grid (0.1 ms), and a
 * jump that most pictures follow moves the grid (placeholders fill it). */

typedef struct DRVideoStats {
    int64_t units, pictures, out, placeholders, invalid;
    int     num, den;          /**< frame rate */
    int64_t base;              /**< grid base (ticks) */
    int64_t side;              /**< side units handed on (or dropped without a side callback) */
} DRVideoStats;

/**
 * Video timing of one track in one segment. Units come from the frame step
 * (ff_disctl_video_unit as its callback); frames go to cb in decode order
 * with grid times; the last frame of each batch carries DR_F_BATCH.
 */
int  ff_disctl_video_open(DRVideo **v, void *logctx, enum AVCodecID codec, int track,
                           DRFrameCb cb, void *opaque, DREventCb event, void *event_opaque);
int  ff_disctl_video_unit(void *video, const DRUnit *unit);
int  ff_disctl_video_flush(DRVideo *v);
/** A frame rate a header states: the first one times the segment; a later
 *  different one is a warning (logged, event), the timing keeps the first. */
void ff_disctl_video_set_rate(DRVideo *v, int num, int den);
/** The grid's field period (ticks, truncated): AVERROR(EAGAIN) while the grid
 *  is not known yet. */
int  ff_disctl_video_field(const DRVideo *v, int64_t *ticks);
/** Bytes taken out of the unit being read (from DRVideoRules.split), at
 *  stream byte pos: a side unit, handed on (ff_disctl_video_set_side) with
 *  the grid time of the next picture after that unit in its batch, else of
 *  the batch's last picture, lasting one field. */
int  ff_disctl_video_side(DRVideo *v, const uint8_t *data, int size, int64_t pos);
/** Where side units go; without a callback they are dropped. */
void ff_disctl_video_set_side(DRVideo *v, DRFrameCb cb, void *opaque);
/** The grid point nearest to a time t (ticks, the segment's own clock):
 *  *out = that point, or AV_NOPTS_VALUE when t is more than 0.1 ms before
 *  the grid base. AVERROR(EAGAIN) while the grid is not known yet. */
int  ff_disctl_video_snap(const DRVideo *v, int64_t t, int64_t *out);
void *ff_disctl_video_priv(DRVideo *v);
void ff_disctl_video_stats(const DRVideo *v, DRVideoStats *st);
void ff_disctl_video_close(DRVideo **v);

/* ---- Stage 2, sub-pictures: units on the video grid (disctl_spu.c) ----
 * DVD-Video / HD DVD sub-picture units. Each unit lasts until its stop
 * command (STP_DSP delay x 1024 / 90000 s; one delay step when it has none),
 * and is timed at the video grid point nearest to its PES time. Left out: a
 * unit without a time of its own, a unit more than 0.1 ms before the
 * video's first field. Units wait until the segment's video knows its grid:
 * flush the video before the sub-pictures at the end of a segment. A unit
 * drawn outside the track's canvas is counted and warned about (kept). */

typedef struct DRSpuStats {
    int64_t units, out;
    int64_t untimed;           /**< left out: no time of its own */
    int64_t early;             /**< left out: before the video's first field */
    int64_t no_stop;           /**< shown without a stop time (DR_F_NO_STOP) */
    int64_t forced;            /**< with a forced start (FSTA_DSP) */
    int64_t colcon;            /**< with a colour / contrast change (CHG_COLCON) */
    int64_t max_shift;         /**< largest move to the video grid (ticks) */
    int64_t outside;           /**< drawn outside the canvas (SET_DAREA beyond width x height) */
    int64_t forced_objects;    /**< PGS: forced objects shown (a forced-only track: the ones it keeps) */
} DRSpuStats;

typedef struct DRSpu DRSpu;

/** Sub-picture timing of one track in one segment, on the grid of the
 *  segment's video v (which must outlive it); width x height the canvas the
 *  units are drawn on (0 x 0: not known, not checked). */
/* Blu-ray PGS (ff_disctl_spu_set_pgs): the units are display sets, each
 * lasting one video field period (the next set replaces or clears it), timed
 * at the video grid point nearest to its composition segment's time; a
 * forced-only track keeps every set (later sets may use what earlier ones
 * define) with each set's composition objects cut to the forced ones. */
int  ff_disctl_spu_open(DRSpu **s, void *logctx, int track, DRVideo *v, int width, int height,
                         DRFrameCb cb, void *opaque, DREventCb event, void *event_opaque);
/** A DRUnitCb: opaque = the DRSpu. */
int  ff_disctl_spu_unit(void *spu, const DRUnit *unit);
/** The track is PGS (forced_only: DR_SUB_FORCED_ONLY track). */
void ff_disctl_spu_set_pgs(DRSpu *s, int forced_only);
/** The end of the segment (after the video's flush). */
int  ff_disctl_spu_flush(DRSpu *s);
void ff_disctl_spu_stats(const DRSpu *s, DRSpuStats *st);
void ff_disctl_spu_close(DRSpu **s);

/* ---- DVD-Video line-21 closed captions (disctl_cc.c) ----
 * The captions travel as MPEG-2 GOP user data, which the MPEG-2 rules take
 * out of the video as side units (ff_disctl_video_set_side). */

/** The size of the caption block at data (9 + 3 x its entry count, the
 *  bytes after it are not part of it), or 0 when the user data is not a
 *  DVD caption block (00 00 01 B2 'C' 'C' 01 F8, then a count byte with
 *  bit 6 clear) or is shorter than its count says. */
int ff_disctl_cc_check(const uint8_t *data, int size);

/** The block's CEA-608 entries as the caption decoder takes them: 3 bytes
 *  each, a field marker (4 = field 1, 5 = field 2) and the byte pair, in
 *  the block's order; out has room for 63 entries. Returns their number;
 *  an entry whose markers are not a known pattern ends the block. */
int ff_disctl_cc_triplets(const uint8_t *data, int size, uint8_t *out);

/* ---- Closed captions to text (disctl_ccx.c) ----
 * CCExtractor runs as a helper program next to ours: the caption blocks go
 * in as its raw caption format (each block: a time in milliseconds and its
 * CEA-608 entries), SRT text comes out and is read back strictly. */

#define DR_CCX_PROGRAM "ccextractor"
#define DR_CCX_VERSION "0.94"

typedef struct DRCue {
    int64_t start_ms, end_ms;
    char   *text;             /**< the cue's lines joined by \n (no \n at the end) */
} DRCue;

typedef struct DRCcx DRCcx;

/** The helper next to the running program (buf), or NULL when it is not
 *  there. */
const char *ff_disctl_ccx_default(char *buf, int size);
int  ff_disctl_ccx_start(DRCcx **c, void *logctx, const char *program);
/** One caption block at ms: count entries of 3 bytes (field marker 4 / 5,
 *  the byte pair). A block without entries is not written. */
int  ff_disctl_ccx_block(DRCcx *c, int64_t ms, const uint8_t *triplets, int count);
/** The end of the caption data: waits for the helper and reads its SRT
 *  (fails when the helper fails or its SRT does not parse). */
int  ff_disctl_ccx_finish(DRCcx *c, DRCue **cues, int *nb_cues);
void ff_disctl_ccx_close(DRCcx **c);
/** SRT as the helper writes it: an optional BOM, cues numbered from 1
 *  without a gap, a timing line of exactly HH:MM:SS,mmm --> HH:MM:SS,mmm,
 *  text lines up to an empty line; CR LF or LF. Anything else fails. */
int  ff_disctl_srt_parse(void *logctx, const char *srt, int size, DRCue **cues, int *nb_cues);
void ff_disctl_cues_free(DRCue **cues, int nb);

/* ---- Stage 5: checks of a track's output (disctl_verify.c) ----
 * Fed with every frame of a track as it leaves the core (in output order);
 * nothing is changed, every finding is an event and counted. */

typedef struct DRVerifyStats {
    int64_t frames, markers;
    int64_t unchecked;         /**< units of a codec that has no unit check */
    int64_t bad_units;         /**< units that do not parse again (sync word, size) */
    int64_t crc_errors;        /**< units that fail their checksum */
    int64_t order_errors;      /**< units without a duration; audio / subtitles: not after the one before */
    int64_t holes, hole_dur;   /**< video in display order: gaps and their total (ticks) */
    int64_t overlaps, overlap_dur; /**< frames starting before the one before them ends; the largest (ticks) */
    int64_t delay;             /**< audio: the first frame's output time (a stream file's start delay) */
    int64_t es_err_max, es_err_at; /**< audio: stream-file position - own time, the largest by size (signed), and
                                        the own time where it is */
    int64_t es_err_end;        /**< audio: the same for the last frame */
    int64_t mkv_err_max, mkv_err_at; /**< audio: output time - own time, the largest by size, where */
} DRVerifyStats;

typedef struct DRVerify DRVerify;

/** Checks of one output track (kind: DRTrackKind). */
int  ff_disctl_verify_open(DRVerify **v, void *logctx, enum AVCodecID codec, int track, int kind,
                            DREventCb event, void *event_opaque);
/** A frame as it leaves the core (not taken over). */
int  ff_disctl_verify_frame(DRVerify *v, const DRFrame *frame);
/** The end of the track: the video order check and the summary lines. */
int  ff_disctl_verify_finish(DRVerify *v);
void ff_disctl_verify_stats(const DRVerify *v, DRVerifyStats *st);
void ff_disctl_verify_close(DRVerify **v);

/* ---- Stage 4 for PCM tracks (disctl_pcm.c) ----
 * Instead of the junction: the samples of a track's frames (on the title
 * timeline) leave in fixed frames (1/30 s at 48 / 44.1 kHz, else about
 * 32 ms rounded up to a divisor of the rate) timed by a sample counter from
 * 0. Frames more than 1 ms before the title start are dropped, a lead-in
 * within it is taken as the origin; a later start below 5 s is filled with
 * silence, from 5 s on skipped on the output clock. A frame more than 10800
 * ticks and 2 samples off the sample count: if the frames after it go on
 * from where it would end without the gap (within 32 frames), it carried a
 * broken time and is appended; else silence fills the gap (at most two
 * output frames per step, the rest pending). A frame that runs more than
 * 10800 ticks and 2 samples into the next one is cut at its start; the next
 * one starting before this frame: when it ends after it, its rest is kept
 * for the next step; when inside, it is dropped. The video ending before a
 * frame leaves the rest of the track out. */

typedef struct DRPcmConfig {
    int        track;
    int        rate;                    /**< samples per second */
    int        bits;                    /**< bits per sample (8-bit silence is 0x80) */
    int        bytes_per_sample_frame;  /**< bytes of one sample of all channels */
    DRVideoRef video;
    DRFrameCb  out;  void *out_opaque;
    DREventCb  event; void *event_opaque;
} DRPcmConfig;

typedef struct DRPcmStats {
    int64_t in, out;          /**< frames in, frames out */
    int64_t dropped;          /**< frames dropped at the start */
    int64_t silence;          /**< silence samples written */
    int64_t broken;           /**< frames appended over a broken time */
    int64_t overlap;          /**< frames cut, held or dropped for overlapping */
} DRPcmStats;

typedef struct DRPcm DRPcm;

int  ff_disctl_pcm_open(DRPcm **p, void *logctx, const DRPcmConfig *cfg);
/** A frame of the track on the title timeline (taken over). */
int  ff_disctl_pcm_push(DRPcm *p, DRFrame *frame);
/** The end of the track: the rest, and the frame being filled. */
int  ff_disctl_pcm_finish(DRPcm *p);
void ff_disctl_pcm_stats(const DRPcm *p, DRPcmStats *st);
void ff_disctl_pcm_close(DRPcm **p);

/* ---- the seamless overlap search's audio (disctl_seamless.c) ---- */

/** Decodes TrueHD / MLP units (pre: decoded first and dropped) to one
 *  channel of 32-bit samples: 16-bit samples x 65536; channels mixed by
 *  ff_disctl_seamless_mix to front centre (centre 1, surround sqrt(2) / 8,
 *  LFE 0, normalised; Q30 coefficients, 64-bit sums shifted down by 30,
 *  clipped). Each unit must give one frame of its own duration; of each range
 *  frame its first rate x duration samples are kept. Returns 1 with *out
 *  (av_free()), 0 when the units do not decode as asked (logged), < 0 on
 *  error. */
int ff_disctl_seamless_decode(void *logctx, enum AVCodecID codec, int rate, const DRFrame *pre, int nb_pre,
                               const DRFrame *range, int nb_range, int32_t **out, int *nb_out);
/** How alike n samples of a and b are: the Pearson correlation in Q32
 *  (0xFFFFFFFF = 1 or more), in integers; 0 when both are silent (no sample
 *  beyond +-99), 1 when they do not correlate positively. */
uint32_t ff_disctl_seamless_corr(const int32_t *a, const int32_t *b, uint32_t n);
/** The mono mix of a decoded layout (a native channel mask) as Q30
 *  coefficients, one per channel in mask order (llrint(2^30 c), clipped):
 *  libswresample's matrix to front centre (centre 1, surround sqrt(2) / 8,
 *  LFE 0, normalised to 1, Pro Logic II encoding) of the layout without its
 *  height, wide, surround direct and second LFE channels, which get 0.
 *  AVERROR(EINVAL) when no mix is built (no front speaker, more than 32
 *  channels, a layout libswresample refuses). */
int ff_disctl_seamless_mix(void *logctx, uint64_t layout, int32_t *q30);

/* ---- Linear PCM of DVD-Video, HD DVD and Blu-ray (disctl_lpcm.c) ----
 * DVD-Video / HD DVD: the packet source hands each PES packet's audio frame
 * header to ff_disctl_lpcm_header() before its payload goes to the frame
 * step (which gives the frame format to FFmpeg's LPCM parser). Blu-ray: the
 * header is part of each unit FFmpeg's parser cuts; the audio stage reads it
 * (ff_disctl_lpcm_bd_header()). The same DRLpcm is the state of the track's
 * audio stage. A unit is one frame; its samples become little-endian PCM
 * (16 bits, or 24 bits for 20- and 24-bit samples). */

typedef struct DRLpcm {
    int      init;             /**< the first header was read */
    uint8_t  b0, b1;           /**< the first header's bytes (DVD layout) */
    int      bits, rate, channels;
    int      spf;              /**< samples per frame (DVD rate / 600, HD DVD rate / 1200) */
    int      frame_bytes;      /**< a frame on the disc */
    int      out_frame_bytes;  /**< a frame converted */
    uint64_t chmask;           /**< HD DVD, Blu-ray: the channel mask of the channel assignment, 0 = none */
    int      drc;              /**< the last header's dynamic range byte */
    int      hd;
    /* Blu-ray (every unit starts with its 4-byte header) */
    int      bd;
    uint8_t  h2, h3;           /**< the first header's channel assignment / frequency byte and bits byte */
    int      payload;          /**< the first header's payload size: every unit's */
    int      coded;            /**< channels stored per sample (an odd count padded to even) */
    uint8_t  map[8];           /**< output channel c = stored channel map[c] (the channel mask's order) */
} DRLpcm;

/** A PES packet's audio frame header: DVD-Video 3 bytes, HD DVD 5 bytes (hd).
 *  The first sets the format; a later one that differs in more than the
 *  frame number and the dynamic range is an error (AVERROR_INVALIDDATA). */
int ff_disctl_lpcm_header(DRLpcm *p, void *logctx, const uint8_t *hdr, int len, int hd);
/** A Blu-ray LPCM unit's header (its first 4 of size bytes): the first sets
 *  the format; a later one must state the same channel assignment, sampling
 *  frequency, bits per sample and payload size (AVERROR_INVALIDDATA). */
int ff_disctl_lpcm_bd_header(DRLpcm *p, void *logctx, const uint8_t *unit, int size);
/** The samples of size disc bytes as little-endian PCM into out (room for
 *  size x 6 / 5); returns the bytes written. Blu-ray: in is a unit's payload
 *  (after its header), the channels leave in map order without the padding
 *  channel. */
int ff_disctl_lpcm_convert(const DRLpcm *p, const uint8_t *in, int size, uint8_t *out);

/* ---- One title through all stages (disctl_title.c) ----
 * The format demuxer hands over each segment's payloads (per track, with the
 * PES time and whether they come from the last 64 MiB of the segment's
 * source); the title runs the frame steps, the timing stages, the joiner, the
 * junction (or the PCM strategy) and the checks, and gives the output frames
 * back. Track 0 is the master video. Audio frames wait until the video has
 * been handed on past their time (the junction compares them with it). */

typedef struct DRTitleTrack {
    enum AVCodecID codec;
    int            kind;           /**< DRTrackKind */
    int            audio_flags;    /**< DR_AUDIO_EXTRACTED, DR_AUDIO_EXTENDED */
    int            width, height;  /**< sub-pictures: the canvas they are drawn on (0 x 0: not known) */
    int            sub_flags;      /**< DR_SUB_FORCED_ONLY */
    int            layer_flags;    /**< DR_LAYER_MERGED */
} DRTitleTrack;

/** A second layer (DR_KIND_LAYER) of HEVC (a Dolby Vision enhancement layer
 *  with its metadata) whose pictures go into the master video's frames
 *  (ff_disctl_hevc_wrap_layer) instead of a track of their own. */
#define DR_LAYER_MERGED 0x0001

/** A subtitle track of what a player shows with subtitles switched off:
 *  only the objects marked forced (PGS: object_forced_on_flag). */
#define DR_SUB_FORCED_ONLY 0x0001

typedef struct DRTitleConfig {
    int                 nb_tracks;
    const DRTitleTrack *tracks;    /**< track 0: the master video */
    int64_t             tolerance; /**< audio lead-in tolerance (ticks; the format profile) */
    int                 lpcm_hd;   /**< LPCM payloads start with HD DVD's 5-byte header (else DVD-Video's 3) */
    const int64_t      *marks;     /**< chapter marks (DRChapterPlan.marks) or NULL */
    int                 nb_marks;
    DREventCb           event; void *event_opaque;   /**< every event (also logged at debug level) */
    const char         *cc_helper; /**< the caption converter program (for a DR_KIND_CAPTION track) */
} DRTitleConfig;

typedef struct DRTitle DRTitle;

/** Once segment / payload / finish fail (not EAGAIN / EINVAL), the title
 *  stays failed: those calls and ff_disctl_title_frame return the same
 *  error from then on. */
int  ff_disctl_title_open(DRTitle **t, void *logctx, const DRTitleConfig *cfg);
/** The next segment begins (ends the one before). */
int  ff_disctl_title_segment(DRTitle *t);
/** A payload of a track in the current segment: time in ticks or
 *  AV_NOPTS_VALUE; tail = read from the last 64 MiB of the segment's source.
 *  LPCM payloads start with their audio frame header. */
int  ff_disctl_title_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time, int tail);
/** The end of the title: every stage finishes. */
int  ff_disctl_title_finish(DRTitle *t);
/** The next output frame (taken over by the caller): 0, AVERROR(EAGAIN)
 *  when none is ready yet, AVERROR_EOF after the last one. */
int  ff_disctl_title_frame(DRTitle *t, int *track, DRFrame *frame);
/** After finish: the chapters of the plan the marks came from (*out freed
 *  with av_free()). */
int  ff_disctl_title_chapters(const DRTitle *t, const DRChapterPlan *plan, DRChapter **out, int *nb_out);
/** PGS: the forced objects the track shows (a forced-only track: none
 *  means it shows nothing). */
int ff_disctl_title_forced_objects(const DRTitle *t, int track, int64_t *forced);
/** A linear PCM track's format once its first header was read (else NULL). */
const DRLpcm *ff_disctl_title_lpcm(const DRTitle *t, int track);
/** Untested features met so far (audio reviews): the job must be checked,
 *  so it is to be reported as failed. */
int64_t ff_disctl_title_review(const DRTitle *t);
/** After finish: a track's frames given out, the warnings logged about it
 *  (junction events, grid findings, checks) and (audio) its start delay. */
int ff_disctl_title_track_result(const DRTitle *t, int track, int64_t *frames, int64_t *warnings, int64_t *delay);
/** After finish, a sub-picture track: units without a stop time shown until
 *  the next one, units drawn outside the track's canvas. */
int ff_disctl_title_spu_result(const DRTitle *t, int track, int64_t *no_stop, int64_t *outside);
/** After finish, the caption track: caption blocks given to the converter,
 *  cues given out. */
int ff_disctl_title_cc_result(const DRTitle *t, int track, int64_t *blocks, int64_t *cues);
/** After finish, a title without a caption track: the caption blocks taken
 *  out of its video and left out, and their entries that carry caption
 *  bytes (0: the caption slot was empty). */
void ff_disctl_title_cc_dropped(const DRTitle *t, int64_t *blocks, int64_t *text);
/** After finish: the end of the video (title duration, ticks). */
int64_t ff_disctl_title_duration(const DRTitle *t);
/** An event kind's name. */
const char *ff_disctl_event_name(int kind);
void ff_disctl_title_close(DRTitle **t);

/* rules of codecs in their own files */
extern const DRAudioRules ff_disctl_audio_mlp;
extern const DRVideoRules ff_disctl_video_mpv;
extern const DRVideoRules ff_disctl_video_vc1;
extern const DRVideoRules ff_disctl_video_h264;
extern const DRVideoRules ff_disctl_video_hevc;

/** An H.264 unit (n bytes of Annex B NAL units) into out (room for n): the
 *  NAL units that leave a frame taken out (end of sequence, end of stream,
 *  filler data, and the types that start no unit; counted per type in
 *  dropped_units) and zero stuffing in front of a start code cut to one zero
 *  byte (bytes counted in stuffing); bytes before the first start code are
 *  not kept. *changed: 0 when nothing was taken out (out is then the unit's
 *  NAL units as they are). Returns the bytes written. */
int ff_disctl_h264_clean(const uint8_t *p, int n, uint8_t *out, int64_t dropped_units[32], int64_t *stuffing,
                         int *changed);
/** What ff_disctl_hevc_clean took out of HEVC units. */
typedef struct DRHevcDropped {
    int64_t units[64];          /**< NAL units per nal_unit_type */
    int64_t other_layer;        /**< NAL units with a nuh_layer_id other than 0 */
    int64_t short_units;        /**< NAL units too short for their 2-byte header */
    int64_t stuffing;           /**< zero bytes */
    int64_t unspecified;        /**< NAL units of the unspecified types 48-61 (kept) */
} DRHevcDropped;
/** An HEVC unit (n bytes of Annex B NAL units) into out (room for n): the
 *  NAL units that leave a frame taken out (filler data, the reserved types,
 *  units of another layer, units too short for their header) and zero
 *  stuffing in front of a start code cut to one zero byte, all counted in d;
 *  end of sequence / end of bitstream units stay (*seq_end set: the title
 *  timeline decides on them); bytes before the first start code are not
 *  kept. *changed: 0 when nothing was taken out. Returns the bytes written. */
int ff_disctl_hevc_clean(const uint8_t *p, int n, uint8_t *out, DRHevcDropped *d, int *changed, int *seq_end);
/** Log what was taken out (what: the track's words, e.g. "HEVC"). */
void ff_disctl_hevc_dropped_log(const DRHevcDropped *d, const char *what);
/** An HEVC frame f with DR_F_SEQ_END, now that the next picture is known
 *  (next: the next frame, NULL at the title's end): the end of sequence /
 *  end of bitstream units are taken out of f when nothing depends on them
 *  (the title ends, or the next picture is an IDR or BLA picture, which
 *  starts a coded video sequence anyway) and kept otherwise. Returns 0 taken
 *  out, 1 kept before a CRA picture, 2 kept before a picture that is no
 *  random access point (a stream fault), < 0 on failure. */
int ff_disctl_hevc_seq_end(DRFrame *f, const DRFrame *next);
/** A Dolby Vision enhancement-layer picture (n bytes of Annex B NAL units)
 *  as the units that follow its base-layer picture in one track, into out
 *  (room for n + 2 per unit): each unit keeps its start code; a unit of the
 *  types 62 (the metadata, RPU) and 63 stays as it is, every other one goes
 *  behind the 2-byte NAL unit header 7E 01 (nal_unit_type 63, nuh_layer_id
 *  0, TemporalId 0). Returns the bytes written. */
int ff_disctl_hevc_wrap_layer(const uint8_t *p, int n, uint8_t *out);
/** The most bytes ff_disctl_hevc_wrap_layer writes for p. */
int ff_disctl_hevc_wrap_size(const uint8_t *p, int n);
int ff_disctl_mlp_check(const uint8_t *data, int size);
int ff_disctl_spu_check(const uint8_t *data, int size);
/* PGS display sets (disctl_pgs.c): 1 when data is one display set (from a
 * presentation composition segment to the end segment, lengths exact). */
int ff_disctl_pgs_check(const uint8_t *data, int size);
int ff_disctl_pgs_verify(const uint8_t *data, int size);
/** The set with each composition segment's objects cut to the forced ones
 *  into out (at least size bytes): its size, < 0 the set does not parse;
 *  *forced: the forced objects kept. */
int ff_disctl_pgs_forced_only(const uint8_t *in, int size, uint8_t *out, int *forced);
int ff_disctl_spu_verify(const uint8_t *data, int size);
/** The VobSub index header of a sub-picture track (frame size, the 16
 *  palette entries 0x00 Y Cr Cb converted to RGB; NULL = all black) into buf;
 *  returns its length as snprintf does. FFmpeg's DVD subtitle decoder reads
 *  it as extradata; it is the head of a .idx file and of a Matroska S_VOBSUB
 *  track's codec private data. */
int ff_disctl_vobsub_header(char *buf, int size, int width, int height, const uint32_t *ycrcb);
int ff_disctl_mlp_verify(const uint8_t *data, int size);


#endif /* AVFORMAT_DISCTL_H */
