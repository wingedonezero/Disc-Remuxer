/*
 * The shared rip core for disc formats (DVD-Video, HD DVD, Blu-ray): units,
 * timing, joining and checks of a title's tracks. Design: docs/RIP_CORE.md
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

#ifndef AVFORMAT_DISCRIP_H
#define AVFORMAT_DISCRIP_H

#include <stdint.h>

#include "libavcodec/codec_id.h"
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
} DRFrame;

/** Hands a frame on; the callee owns it (ff_discrip_frame_unref). */
typedef int (*DRFrameCb)(void *opaque, DRFrame *frame);

void ff_discrip_frame_unref(DRFrame *frame);

typedef struct DRAudio DRAudio;

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
    /** The unit's duration in ticks, and its final bytes (a core cut
     *  shortens frame->size); < 0 on failure. NULL: samples / rate of the
     *  header. */
    int  (*duration)(DRAudio *a, DRFrame *frame);
    /** Checks a unit for features that are implemented but not met on real
     *  discs yet (ff_discrip_audio_review). NULL: none. */
    void (*inspect)(DRAudio *a, const DRFrame *frame);
    /** 1: only sync units are key frames (DR_F_KEY); 0: every unit is. */
    int    key_on_sync;
    /** Rule state (ff_discrip_audio_priv), freed with close(). */
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
     *  (MPEG-2 video: the picture start code; VC-1: the frame start code;
     *  audio and sub-pictures: 0), or -1 when the unit has none. */
    int (*anchor)(const uint8_t *data, int size);
    /** 1 when the parser's output is a unit of the codec, 0 when it is not
     *  (bytes before the first sync word, bytes between units): such bytes
     *  are left out and take no time. NULL: every output is a unit. */
    int (*check)(const uint8_t *data, int size);
    /** Audio rules; NULL for video / subtitles, or while not implemented
     *  (the track is then refused by ff_discrip_audio_open). */
    const DRAudioRules *audio;
    /** The core's own unit cutter, used instead of FFmpeg's parser where the
     *  parser drops bytes the reference keeps. unit_size: the size of the
     *  unit at buf (> 0), 0 when more bytes are needed, < 0 when no unit
     *  starts there; resync: the offset of the next possible unit start in
     *  buf, -1 when none. NULL: FFmpeg's parser. */
    int (*unit_size)(const uint8_t *buf, int avail);
    int (*resync)(const uint8_t *buf, int avail);
} DRCodec;

/** The codec table entry of a codec, or NULL (the codec is not supported). */
const DRCodec *ff_discrip_codec(enum AVCodecID id);

/* ---- Stage 1: units and timestamp records (discrip_units.c) ---- */

typedef struct DRUnit {
    const uint8_t *data;   /**< the unit's bytes, valid during the callback */
    int            size;
    int64_t        time;   /**< ticks, or AV_NOPTS_VALUE */
    int64_t        pos;    /**< byte offset of the unit's first byte in the track's stream */
    int            samples;/**< samples FFmpeg's parser gives the unit, 0 = none */
    int            rate;   /**< and their sample rate */
} DRUnit;

/** Called for every unit, in stream order. A negative return stops the
 *  cutter and is returned by the call that produced the unit. */
typedef int (*DRUnitCb)(void *opaque, const DRUnit *unit);

typedef struct DRCutterStats {
    int64_t bytes;         /**< payload bytes written */
    int64_t records;       /**< payloads written with a time */
    int64_t units;         /**< units given to the callback */
    int64_t timed;         /**< of them with a time */
    int64_t records_unused;/**< records dropped without giving their time to a unit */
    int64_t skipped;       /**< parser outputs that are not units (left out) */
    int64_t skipped_bytes;
    int64_t joined;        /**< parser outputs added to the unit before them (DRCodec.continues) */
} DRCutterStats;

typedef struct DRCutter DRCutter;

/**
 * A unit cutter for one track of one segment: payloads in, units out.
 * Units are cut by FFmpeg's parser for the codec; a unit takes the time of a
 * payload when the unit's anchor (see DRCodec) is the first one that lies in
 * that payload's bytes (ISO/IEC 13818-1 2.4.3.7: a PES packet's PTS belongs
 * to the first access unit that starts in it).
 *
 * @param logctx for av_log
 * @return 0, AVERROR(ENOSYS) when the codec has no table entry or no parser
 */
int ff_discrip_cutter_open(DRCutter **cutter, void *logctx, enum AVCodecID codec,
                           DRUnitCb cb, void *opaque);

/** One payload; time in ticks or AV_NOPTS_VALUE. */
int ff_discrip_cutter_write(DRCutter *c, const uint8_t *data, int size, int64_t time);

/** The end of the segment: the units still buffered in the parser. */
int ff_discrip_cutter_flush(DRCutter *c);

void ff_discrip_cutter_stats(const DRCutter *c, DRCutterStats *stats);

void ff_discrip_cutter_close(DRCutter **cutter);

/* ---- Stage 2, audio: units timed within one segment (discrip_audio.c) ---- */

#define DR_AUDIO_CORE_ONLY 0x0001  /**< the track is the core of a stream that also
                                        carries an extension: units are cut to the core */

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
 * cutter (ff_discrip_audio_unit as its callback); timed frames go to cb.
 * @return 0, AVERROR(ENOSYS) when the codec's audio rules are missing
 */
int ff_discrip_audio_open(DRAudio **audio, void *logctx, enum AVCodecID codec, int flags,
                          DRFrameCb cb, void *opaque);

/** A DRUnitCb: opaque = the DRAudio. */
int ff_discrip_audio_unit(void *audio, const DRUnit *unit);

/** An empty marker at a time (no bytes); it keeps its place in the stream. */
int ff_discrip_audio_marker(DRAudio *a, int64_t time);

/** The end of the segment. Fails when no unit ever gave the stream's header. */
int ff_discrip_audio_flush(DRAudio *a);

/** A feature implemented but not met on real discs: logged once per kind as
 *  an error, counted in the statistics (the job is then marked failed). */
void ff_discrip_audio_review(DRAudio *a, unsigned kind, const char *what);

void ff_discrip_audio_stats(const DRAudio *a, DRAudioStats *stats);

void ff_discrip_audio_close(DRAudio **audio);

/* the audio stream values the rules use (for rules that keep state) */
const DRAudioHeader *ff_discrip_audio_header(const DRAudio *a);
int ff_discrip_audio_flags(const DRAudio *a);
void *ff_discrip_audio_priv(DRAudio *a);
void *ff_discrip_audio_log(const DRAudio *a);

/* ---- Stage 4, audio: the junction of one output track (discrip_junction.c) ----
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
} DRJunctionConfig;

typedef struct DRJunctionStats {
    int64_t in, out, dropped, dropped_dur, markers;
    int64_t skew, base;        /**< current skew and start shift */
    int     ended;             /**< the video ended before the track */
} DRJunctionStats;

typedef struct DRJunction DRJunction;

int  ff_discrip_junction_open(DRJunction **j, void *logctx, const DRJunctionConfig *cfg);
/** A frame of the track on the title timeline (taken over); processes as
 *  far as the frames already given allow. */
int  ff_discrip_junction_push(DRJunction *j, DRFrame *frame);
/** The end of the track: processes the rest. */
int  ff_discrip_junction_finish(DRJunction *j);
void ff_discrip_junction_stats(const DRJunction *j, DRJunctionStats *st);
void ff_discrip_junction_close(DRJunction **j);

/* rules of codecs in their own files */
extern const DRAudioRules ff_discrip_audio_mlp;
int ff_discrip_mlp_check(const uint8_t *data, int size);
int ff_discrip_mlp_unit_size(const uint8_t *buf, int avail);
int ff_discrip_mlp_resync(const uint8_t *buf, int avail);

#endif /* AVFORMAT_DISCRIP_H */
