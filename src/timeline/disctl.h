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

#ifndef DISC_TIMELINE_DISCTL_H
#define DISC_TIMELINE_DISCTL_H

#include <stdint.h>

#include "base/buf.h"
#include "parse/rules.h"
#include "parse/split.h"

/* Track codec ids. The values are the processing library's own, so the ids
 * the format parts and the tests pass cross this seam unchanged until the
 * disc model becomes ours (checked at build time in src/analyze/host.c). */
typedef enum DRCodecID {
    DR_CODEC_NONE               = 0,
    DR_CODEC_MPEG1VIDEO         = 1,
    DR_CODEC_MPEG2VIDEO         = 2,
    DR_CODEC_H264               = 27,
    DR_CODEC_VC1                = 70,
    DR_CODEC_HEVC               = 172,
    DR_CODEC_PCM_S16LE          = 65536,
    DR_CODEC_PCM_S24LE          = 65548,
    DR_CODEC_PCM_DVD            = 65555,
    DR_CODEC_PCM_BLURAY         = 65560,
    DR_CODEC_MP2                = 86016,
    DR_CODEC_MP3                = 86017,
    DR_CODEC_AAC                = 86018,
    DR_CODEC_AC3                = 86019,
    DR_CODEC_DTS                = 86020,
    DR_CODEC_MLP                = 86045,
    DR_CODEC_EAC3               = 86056,
    DR_CODEC_MP1                = 86058,
    DR_CODEC_TRUEHD             = 86060,
    DR_CODEC_AAC_LATM           = 86065,
    DR_CODEC_DVD_SUBTITLE       = 94208,
    DR_CODEC_EIA_608            = 94218,
    DR_CODEC_HDMV_PGS_SUBTITLE  = 94214,
    DR_CODEC_SUBRIP             = 94225,
    DR_CODEC_HDMV_TEXT_SUBTITLE = 94231,
} DRCodecID;

/** A codec's name in the codec table, or "?". */
const char *ff_disctl_codec_id_name(DRCodecID id);

/** A missing time. */
#define DR_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))

/* ---- The host: what the timeline borrows from the processing library
 * until src/analyze takes it over (step 6): the stock frame parser for the
 * codecs without a rule of their own (MPEG video, E-AC-3), the LATM stream
 * header (one decoded frame) and the TrueHD / MLP end-of-stream decode.
 * Registered once by the program and the test bridge
 * (ff_disctl_set_host). ---- */
typedef struct DRTimelineHost {
    /** The library's frame parser for codec, or NULL when it has none. */
    void *(*parser_open)(DRCodecID codec);
    /** Bytes in, at most one chunk out (*out / *out_size; *frame_offset its
     *  stream offset, *duration / *rate what the parser read of it);
     *  returns the bytes consumed. pos: the stream offset of data. */
    int (*parser_parse)(void *p, const uint8_t *data, int size, int64_t pos,
                        uint8_t **out, int *out_size, int64_t *frame_offset,
                        int *duration, int *rate);
    void (*parser_close)(void **p);
    /** One LATM / LOAS frame decoded: the stream's rate and samples. */
    int (*latm_header)(const uint8_t *data, int size, int *rate, int *samples);
    /** TrueHD / MLP: the n access units decoded in order (a major sync
     *  leads); *count = the samples of the last one. Logs its own failures
     *  (debug). */
    int (*mlp_last_au_samples)(void *logctx, const uint8_t *const *aus, const int *sizes,
                               int n, int *count);
} DRTimelineHost;

void ff_disctl_set_host(const DRTimelineHost *host);
const DRTimelineHost *ff_disctl_host(void);

/* Time: one unit = 1/1,080,000,000 s (27 MHz x 40). Exact for 90 kHz PES
 * times, for 1001-based frame and field durations and for 48 / 96 / 192 kHz
 * samples. A missing time is DR_NOPTS_VALUE. */
#define DR_TICKS_PER_SECOND 1080000000LL
#define DR_TICKS_PER_PTS    12000          /* 90 kHz -> ticks */

/* ---- Frames: the units the stages after stage 1 hand on ---- */

#define DR_F_KEY      0x0001   /**< key frame / random access point */
#define DR_F_SYNC     0x0002   /**< a unit at which audio may be dropped or joined */
#define DR_F_MARKER   0x0004   /**< empty marker: carries a time, no bytes */
#define DR_F_DISCARD  0x0008   /**< not referenced (B picture) */
#define DR_F_CHAPTER  0x0010   /**< a chapter starts here */
#define DR_F_BATCH    0x0040   /**< video: the last frame of a batch the video timing handed on */
#define DR_F_NO_STOP  0x0080   /**< sub-picture: shows a picture but has no stop time: out of
                                    its stage it lasts one delay step; the title pipeline makes
                                    it last until the track's next unit starts (the last one:
                                    until the video's end) */
#define DR_F_SEQ_END  0x0100   /**< video (HEVC): the frame carries an end of sequence / end of
                                    bitstream unit; the title timeline decides on it when it
                                    knows the next picture (ff_disctl_hevc_seq_end) */
#define DR_F_POSITION 0x0200   /**< video: the first frame at or after a chapter's position in the
                                    source (DVD-Video): the next open chapter mark takes its time */
/* video key frames: how decoding can start there (DRPicture.start) */
#define DR_F_START_CLEAN  0x0400 /**< nothing before it is needed */
#define DR_F_START_BROKEN 0x0800 /**< marked as a broken link: the pictures shown before it are not decodable */
#define DR_F_START_OPEN   0x1000 /**< the pictures shown before it may need pictures before it in the stream */
#define DR_F_CTR      0x2000   /**< audio: ctr holds the stream's own number of the frame's first sample */
#define DR_F_PLACE    0x4000   /**< video: the first picture at or after one or more places of the source
                                    (ff_disctl_title_place()); ctr holds how many */

typedef struct DRFrame {
    DrBuf *buf;      /**< owns the bytes (NULL for a marker) */
    uint8_t     *data;
    int          size;
    int64_t      time;     /**< ticks, or DR_NOPTS_VALUE */
    int64_t      dur;      /**< ticks */
    int64_t      pos;      /**< byte offset of the unit in its track's stream */
    unsigned     flags;    /**< DR_F_* */
    int          samples;  /**< samples the frame cutter gave the unit, 0 = none */
    int          rate;     /**< and their sample rate */
    int64_t      src;      /**< from the joiner on: the frame's own time on the title timeline (its
                                PES-derived time, before the joiner moves it or the junction shifts it) */
    int          seg;      /**< from the joiner on: the segment the frame comes from (from 0) */
    int          ctr;      /**< with DR_F_CTR: the stream's own sample number of the frame's first sample,
                                modulo 65536 (TrueHD / MLP: a restart header's output timing, counted on
                                by the samples of an access unit); with DR_F_PLACE: the places it marks */
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

/* How decoding can start at a key frame (the codec's own marks). */
enum DRStart {
    DR_START_UNKNOWN = 0,   /**< not a key frame, or the codec rule does not say */
    DR_START_CLEAN,         /**< nothing before it is needed (MPEG-2 closed GOP, H.264 / HEVC IDR, VC-1
                                 closed entry point) */
    DR_START_BROKEN,        /**< a broken link (MPEG-2 / VC-1 broken_link, H.264 recovery point with
                                 broken_link_flag, HEVC BLA): the pictures shown before it cannot be decoded */
    DR_START_OPEN,          /**< the pictures shown before it may use pictures before it in the stream (MPEG-2
                                 open GOP, H.264 recovery point, HEVC CRA, VC-1 open entry point) */
};

/* A picture's scan as its headers state it (DRPicture.scan); 0: not read. */
#define DR_SCAN_KNOWN       0x01   /**< the bits below are the picture's */
#define DR_SCAN_PROGRESSIVE 0x02   /**< a progressive frame (else interlaced) */
#define DR_SCAN_TFF         0x04   /**< top field first */
#define DR_SCAN_RFF         0x08   /**< repeat first field (or repeated frames) */

typedef struct DRPicture {
    int type;           /**< DRPictureType */
    int order;          /**< display-order number (wraps at the codec's period) */
    int fields;         /**< duration in fields (2 = one frame) */
    int key;            /**< a key frame even when not an I picture (e.g. VC-1 sequence header + entry point) */
    int rate_num, rate_den;     /**< the frame rate the unit's headers give (0: none) */
    int start;          /**< key frames: DRStart */
    int scan;           /**< DR_SCAN_*, 0 = not read */
    int seq_end;        /**< the unit carries an end of sequence code (MPEG-2, VC-1) */
    int seq_start;      /**< the unit starts a sequence: a sequence header before its picture (MPEG-2, VC-1) */
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
/** A core filter (parse/rules.h): which bytes of a unit stay. */
typedef int (*DRCoreFilter)(const uint8_t *data, int size, int *off, int *out_size);

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
    /** The core filter each unit goes through after its duration, for a
     *  track of these audio flags (e.g. dr_eac3_core: an AC-3 frame without
     *  the E-AC-3 frames after it; parse/rules.h), NULL: none. */
    DRCoreFilter (*filter)(int flags);
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
    DRCodecID id;
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
    /** The codec's own cut (the splitter, parse/split.h), which knows the
     *  reference's rule and says which of its chunks are no unit; 0 (MPEG
     *  video, E-AC-3): cut by FFmpeg's stock parser. */
    enum DrSplitCodec split;
    /** 1: the splitter gives each unit's picture values (type, key, display
     *  order, field periods, the frame rate): they come with the unit
     *  (DRUnit.pic). */
    int parser_picture;
    /** The check of an output unit (stage 5): DR_UNIT_OK, DR_UNIT_BAD (sync
     *  word or size wrong), DR_UNIT_CRC (a checksum fails). NULL: not
     *  checked. */
    int (*verify)(const uint8_t *data, int size);

    /* What the disc layer's stream pass (pass.c) needs to know. */
    /** 1: a stream of the codec is described without data (sub-pictures,
     *  PGS, caption text): its track stays when the stream has no packet. */
    int info_without_data;
    /** The bytes the decoder opens with as extradata (VC-1: the sequence
     *  header and entry point, up to the first frame): 1 found (*start,
     *  *end in buf), 0 not yet. NULL: the decoder opens at once. */
    int (*decoder_headers)(const uint8_t *buf, int size, int *start, int *end);
    /** Video: the bit rate shown: 0 none, DR_RATE_STATED the sequence
     *  header's (MPEG-1), DR_RATE_MAX its maximum (MPEG-2). Audio: the
     *  decoder's always. */
    int video_bit_rate;
    /** 1: no display aspect (MPEG-1: its aspect code is a pixel shape). */
    int no_display_aspect;
    /** 1: the bits per sample are the coded ones (LPCM). */
    int bits_coded;
} DRCodec;

enum { DR_RATE_STATED = 1, DR_RATE_MAX = 2 };

enum { DR_UNIT_OK = 0, DR_UNIT_BAD = 1, DR_UNIT_CRC = 2 };


/** The codec table entry of a codec, or NULL (the codec is not supported). */
const DRCodec *ff_disctl_codec(DRCodecID id);

/* ---- Stage 1: units and timestamp records (disctl_frames.c) ---- */

typedef struct DRUnit {
    const uint8_t *data;   /**< the unit's bytes, valid during the callback */
    int            size;
    int64_t        time;   /**< ticks, or DR_NOPTS_VALUE */
    int64_t        pos;    /**< byte offset of the unit's first byte in the track's stream */
    int            samples;/**< samples the frame cutter gives the unit, 0 = none */
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
 * The frame step for one track of one segment: payloads in, units out, each
 * with its PES time. Units are cut by the codec's rule (FFmpeg's stock parser
 * for MPEG video and E-AC-3); a unit takes the time of a
 * payload when the unit's anchor (see DRCodec) is the first one that lies in
 * that payload's bytes (ISO/IEC 13818-1 2.4.3.7: a PES packet's PTS belongs
 * to the first access unit that starts in it).
 *
 * @param logctx for av_log
 * @return 0, DR_ERR(ENOSYS) when the codec has no table entry or no parser
 */
int ff_disctl_frames_open(DRFrames **frames, void *logctx, DRCodecID codec,
                           DRUnitCb cb, void *opaque);

/** One payload; time in ticks or DR_NOPTS_VALUE. */
int ff_disctl_frames_write(DRFrames *c, const uint8_t *data, int size, int64_t time);

/** Where a block of the segment lies, for the log: the place in words into
 *  buf ("sector at VTS_03_1.VOB byte 349859840, 12288 bytes into the
 *  title"); 0, or < 0 when not known. */
typedef int (*DRPlaceCb)(void *opaque, int seg, int64_t block, char *buf, int size);

/** How the track's left-out bytes are told: label (e.g. "Track 1"), the
 *  part being read in words (e.g. "title", "segment 2"), segment seg and
 *  where its blocks lie (place may be NULL). Every run of bytes left out is
 *  said, with what it is, where it lies and what is done with it. */
void ff_disctl_frames_set_report(DRFrames *c, const char *label, const char *part, int seg, DRPlaceCb place,
                                 void *opaque);

/** A payload that comes from block (of the segment, from 0; -1 not known). */
int ff_disctl_frames_write_at(DRFrames *c, const uint8_t *data, int size, int64_t time, int64_t block);

/** The end of the segment: the units still buffered in the parser. */
int ff_disctl_frames_flush(DRFrames *c);

void ff_disctl_frames_stats(const DRFrames *c, DRFramesStats *stats);

/** The stream carries an extension (DTS-HD): the DTS rule takes every
 *  variant of the codec (dr_split_take_any). */
void ff_disctl_frames_take_any(DRFrames *c);

/** The track is a DVD-Video disc's: its parser follows the DVD's frame
 *  rules (dr_split_dvd). */
void ff_disctl_frames_dvd(DRFrames *c);

/** Linear PCM: the frame format for the LPCM rule (from the packets'
 *  audio frame headers): channels, coded bits per sample, samples per
 *  frame. */
void ff_disctl_frames_set_pcm_format(DRFrames *c, int channels, int bits, int samples_per_frame);

void ff_disctl_frames_close(DRFrames **frames);

/* ---- Stage 2, audio: units timed within one segment (disctl_audio.c) ---- */

#define DR_AUDIO_DVD       0x0004  /**< the track is a DVD-Video disc's (its frame rules: the DVD's) */
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
    int64_t review;        /**< untested features met (each kind once): said in the log */
    int64_t continuity;    /**< unit times that differ from the previous unit's end */
    DRAudioHeader header;
} DRAudioStats;

/**
 * Audio timing of one track in one segment. Units come from the track's
 * frame step (ff_disctl_audio_unit as its callback); timed frames go to cb.
 * @return 0, DR_ERR(ENOSYS) when the codec's audio rules are missing
 */
int ff_disctl_audio_open(DRAudio **audio, void *logctx, DRCodecID codec, int flags,
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
/* units that wait for the stream values: those before the first sync unit
 * that gives them (rules->header() is called with that unit) */
int ff_disctl_audio_waiting(const DRAudio *a);
int ff_disctl_audio_flags(const DRAudio *a);
void *ff_disctl_audio_priv(DRAudio *a);
void *ff_disctl_audio_log(const DRAudio *a);

/* ---- Stage 4, audio: the junction of one output track (disctl_junction.c) ----
 * Title start: every frame keeps its own time, also when the track starts
 * before the video (a stream file's negative delay; nothing is dropped or
 * moved). At each join first (codecs other than TrueHD / MLP): a segment
 * whose first frame is a byte copy of the frame the segment before ends
 * with, while the stamps overlap, starts without the copy. Then per frame:
 * overlaps grow the audio skew, short gaps are absorbed into it, real gaps
 * reset it (nothing is inserted), whole frames are dropped (only up to a
 * sync unit) once the skew reaches one frame (within one 90 kHz tick).
 * Every frame leaves at time + skew. */

enum DREventKind {
    DR_EV_START_GAP = 1,   /**< first frame 0.5..3 ms after the video start: kept */
    DR_EV_START_EARLY,     /**< the track starts dur before the video: kept as it is (a negative delay) */
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
    DR_EV_PCM_SILENCE,     /**< PCM: a gap filled with silence (pos = the output time, dur = the gap in ticks,
                                count = samples) */
    DR_EV_PCM_SKIP,        /**< PCM: a start 5 s or more after the video skipped on the output clock (dur ticks,
                                count samples), nothing written */
    DR_EV_PCM_TIMECODE,    /**< PCM: frames with a broken time appended where they follow on (pos = the first
                                one's time, dur = the apparent skew, count = frames) */
    /* title checks of the segments and their joins (findings, nothing is changed) */
    DR_EV_JOIN_START,      /**< a segment's first key frame (pos = its time; skew = its DRStart; count = the
                                pictures after it in decoding order that are shown before it; dur = the
                                segment's number): one per segment, a warning when the start is not clean
                                and such pictures follow */
    DR_EV_JOIN_NOT_KEY,    /**< a segment's first picture is no key frame (pos = its time, dur = the segment) */
    DR_EV_JOIN_REPEAT,     /**< a segment starts with the pictures the segment before ends with (the same
                                bytes in the same order, a key frame among them; up to 32): count = how many,
                                pos = the segment's first picture's time, skew = the segment */
    DR_EV_SEQ_END_INSIDE,  /**< an end of sequence code with a picture after it in its segment (pos = the
                                time of the picture that carries it; dur 1: that picture starts a new
                                sequence with its sequence header, a sequence change, not a fault) */
    DR_EV_PICTURES,        /**< a segment's pictures read and given out differ (count = read - given out,
                                dur = the segment) */
    DR_EV_SYNC,            /**< audio: as a stream file plays it, a frame is more than DR_SYNC_LIMIT off its
                                own time (pos = that time, dur = the largest error in the segment, count = the
                                segment): one per segment */
    DR_EV_AUDIO_COPY,      /**< a segment's first frame is a byte copy of the frame the segment before ends
                                with, and the stamps overlap: the copy is left out (pos = its time, dur = its
                                duration, skew = the skew, count = the overlap in ticks) */
    DR_EV_AUDIO_COPY_LONG, /**< a segment's first count frames are byte copies of the count frames the
                                segment before ends with (the stamps overlap by dur, more than one frame):
                                found only, nothing left out (pos = the first one's time) */
    DR_EV_SAMPLE_JOIN,     /**< TrueHD / MLP at a join, by the streams' sample numbers (pos = the next
                                segment's first frame's time, dur = the earlier segment's end less that
                                frame's sample number, in samples; count = DRSampleJoin) */
};

/** Audio sync error (stream file position - own time) beyond which a
 *  segment is reported (DR_EV_SYNC). */
#define DR_SYNC_LIMIT (DR_TICKS_PER_SECOND / 1000)

typedef struct DREvent {
    int     kind;          /**< DREventKind */
    int     track;
    int64_t pos;           /**< ticks: where (title timeline) */
    int64_t dur;           /**< ticks: how much (overlap, gap, dropped duration, lead-in) */
    int64_t skew;          /**< ticks: the audio skew after the event */
    int64_t count;         /**< frames (DROP); x1000 missing frames (GAP) */
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
    DRVideoRef video;
    DRFrameCb  out;  void *out_opaque;
    DREventCb  event; void *event_opaque;
    /** The stream's codec and sample rate. */
    DRCodecID codec;
    int        rate;
} DRJunctionConfig;

/** How a TrueHD / MLP join was decided (DR_EV_SAMPLE_JOIN count). */
enum DRSampleJoin {
    DR_JOIN_CONTINUES = 1, /**< the next segment starts where the earlier one ends: everything kept */
    DR_JOIN_SAME,          /**< its first unit holds the earlier one's last samples again: left out */
    DR_JOIN_SAME_KEPT,     /**< the same, but the unit after it is no major sync: kept (no clean restart) */
    DR_JOIN_APART,         /**< the sample numbers do not connect: the skew rule decides */
    DR_JOIN_NO_COUNT,      /**< a side without sample numbers: the skew rule decides */
    DR_JOIN_KINDS
};

typedef struct DRJunctionStats {
    int64_t in, out, dropped, dropped_dur, markers;
    int64_t joins[DR_JOIN_KINDS]; /**< TrueHD / MLP joins by DRSampleJoin */
    int64_t copies;            /**< of the dropped: copies at a join left out (DR_EV_AUDIO_COPY) */
    int64_t skew;              /**< current skew */
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
    int64_t       *open_marks;   /**< the same marks when some get their time from a DR_F_POSITION frame
                                      (those are INT64_MAX until then), or NULL */
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
/** Segment seg's start (its first video time, ticks, its own clock) once
 *  placed; DR_ERR(EAGAIN) before. */
int  ff_disctl_join_segment_start(const DRJoin *j, int seg, int64_t *start);
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
 * (DR_ERR_INVALID). Records before skip (leading segments left out) are
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
 *  *out is freed with dr_free(). */
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
    int64_t side_bytes;        /**< and their bytes */
    int64_t other, other_bytes;/**< units that are no picture, left out */
    int64_t stuffing;          /**< zero stuffing bytes a codec rule took out (ff_disctl_video_stuffing) */
    int64_t removed[3];        /**< units a codec rule took out (ff_disctl_video_removed): DR_REMOVED_* */
    int64_t seq_end;           /**< units with an end of sequence code */
    int64_t seq_end_inside;    /**< of them before the segment's last picture (DR_EV_SEQ_END_INSIDE) */
    int64_t seq_new;           /**< of those: followed by a new sequence (its sequence header) */
    int64_t scan_known;        /**< pictures whose scan was read (DRPicture.scan) */
    int64_t progressive, tff, rff; /**< of them: progressive frames, top field first, repeat first field */
} DRVideoStats;

/**
 * Video timing of one track in one segment. Units come from the frame step
 * (ff_disctl_video_unit as its callback); frames go to cb in decode order
 * with grid times; the last frame of each batch carries DR_F_BATCH.
 */
int  ff_disctl_video_open(DRVideo **v, void *logctx, DRCodecID codec, int track,
                           DRFrameCb cb, void *opaque, DREventCb event, void *event_opaque);
int  ff_disctl_video_unit(void *video, const DRUnit *unit);
int  ff_disctl_video_flush(DRVideo *v);
/** A frame rate a header states: the first one times the segment; a later
 *  different one is a warning (logged, event), the timing keeps the first. */
void ff_disctl_video_set_rate(DRVideo *v, int num, int den);
/** The grid's field period (ticks, truncated): DR_ERR(EAGAIN) while the grid
 *  is not known yet. */
int  ff_disctl_video_field(const DRVideo *v, int64_t *ticks);
/** Bytes taken out of the unit being read (from DRVideoRules.split), at
 *  stream byte pos: a side unit, handed on (ff_disctl_video_set_side) with
 *  the grid time of the next picture after that unit in its batch, else of
 *  the batch's last picture, lasting one field. */
int  ff_disctl_video_side(DRVideo *v, const uint8_t *data, int size, int64_t pos);
/** Where side units go; without a callback they are dropped. */
void ff_disctl_video_set_side(DRVideo *v, DRFrameCb cb, void *opaque);
/** Zero stuffing bytes a codec rule took out of the unit being read (for
 *  the statistics). */
void ff_disctl_video_stuffing(DRVideo *v, int64_t bytes);
/** Units a codec rule took out of the unit being read, of kind
 *  DR_REMOVED_* (for the statistics and the title's summary). */
enum { DR_REMOVED_SEQ_END, DR_REMOVED_STREAM_END, DR_REMOVED_FILLER };
void ff_disctl_video_removed(DRVideo *v, int kind, int64_t n);
/** The grid point nearest to a time t (ticks, the segment's own clock):
 *  *out = that point, or DR_NOPTS_VALUE when t is more than 0.1 ms before
 *  the grid base. DR_ERR(EAGAIN) while the grid is not known yet. */
int  ff_disctl_video_snap(const DRVideo *v, int64_t t, int64_t *out);
void *ff_disctl_video_priv(DRVideo *v);
void ff_disctl_video_stats(const DRVideo *v, DRVideoStats *st);
void ff_disctl_video_close(DRVideo **v);

/* ---- Stage 2, text subtitles (disctl_textst.c) ----
 * Blu-ray text subtitles (HDMV TextST), read from a clip of their own: one
 * segment per unit. The first unit must be the dialog style segment (the
 * track's header, ff_disctl_textst_style(); without it the track gives
 * nothing); each dialog presentation segment is a frame at its unit's time
 * lasting its end minus its start (no video grid: they keep their own
 * times). A forced-only track keeps the regions with forced_on_flag (a
 * dialog without one is left out). Units left out are said (warnings). */

typedef struct DRTextstStats {
    int64_t units;             /**< units given */
    int64_t dialogs;           /**< dialog presentation segments read (given out or not) */
    int64_t forced;            /**< of them with a forced region */
    int64_t out;               /**< frames given out */
    int64_t left_out;          /**< units left out (no style segment first, another type, no time, ends early) */
    int64_t not_forced;        /**< forced-only track: dialogs without a forced region (not shown) */
    int64_t regions_cut;       /**< forced-only track: regions taken out of the dialogs given out */
    int64_t warnings;
    int     disc_count;        /**< the number of dialogs the disc gives after the style segment */
} DRTextstStats;

typedef struct DRTextst DRTextst;

/** A text subtitle track's stage: where names its clip (for the log). */
int  ff_disctl_textst_open(DRTextst **t, void *logctx, int track, int forced_only, const char *where,
                           DRFrameCb cb, void *opaque);
/** A DRUnitCb: opaque = the DRTextst. */
int  ff_disctl_textst_unit(void *textst, const DRUnit *unit);
/** The dialog style segment (NULL before it was read). */
const uint8_t *ff_disctl_textst_style(const DRTextst *t, int *size);
void ff_disctl_textst_stats(const DRTextst *t, DRTextstStats *st);
void ff_disctl_textst_close(DRTextst **t);
/** The codec table's rules: a dialog presentation segment takes the PES
 *  time (offset 0), a style segment none (-1); a unit is one whole segment
 *  of the two types; the output check (a dialog parses). */
int  ff_disctl_textst_anchor(const uint8_t *data, int size);
int  ff_disctl_textst_check(const uint8_t *data, int size);
int  ff_disctl_textst_verify(const uint8_t *data, int size);
/** A dialog presentation segment (size bytes) with only its forced regions
 *  into out (room for size): its bytes, or DR_ERR_INVALID; *kept: the
 *  regions kept. */
int  ff_disctl_textst_forced_only(const uint8_t *in, int size, uint8_t *out, int *kept);

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
    int     segments;          /**< audio: segments the track's frames came from */
    int     sync_segments;     /**< audio: of them with a stream-file error beyond DR_SYNC_LIMIT (DR_EV_SYNC) */
    int     sync_seg;          /**< audio: the segment of es_err_max */
} DRVerifyStats;

typedef struct DRVerify DRVerify;

/** Checks of one output track (kind: DRTrackKind). */
int  ff_disctl_verify_open(DRVerify **v, void *logctx, DRCodecID codec, int track, int kind,
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
 * the track's start: a track that starts before the video keeps every frame
 * and starts at its own (negative) time; a later start below 5 s is filled
 * with silence, from 5 s on skipped on the output clock. A frame more than 10800
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

/* ---- Linear PCM of DVD-Video, HD DVD and Blu-ray (disctl_lpcm.c) ----
 * DVD-Video / HD DVD: the packet source hands each PES packet's audio frame
 * header to ff_disctl_lpcm_header() before its payload goes to the frame
 * step (which gives the frame format to the LPCM rule). Blu-ray: the
 * header is part of each unit the LPCM rule cuts; the audio stage reads it
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
 *  frame number and the dynamic range is an error (DR_ERR_INVALID). */
int ff_disctl_lpcm_header(DRLpcm *p, void *logctx, const uint8_t *hdr, int len, int hd);
/** A Blu-ray LPCM unit's header (its first 4 of size bytes): the first sets
 *  the format; a later one must state the same channel assignment, sampling
 *  frequency, bits per sample and payload size (DR_ERR_INVALID). */
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
    DRCodecID codec;
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
 *  only the objects marked forced (PGS: object_forced_on_flag; text
 *  subtitles: the regions with forced_on_flag). */
#define DR_SUB_FORCED_ONLY 0x0001
/** A text subtitle track read from a clip of its own (Blu-ray TextST): its
 *  payloads come through ff_disctl_title_side_*(), on that clip's clock, and
 *  are placed with the title's first segment (no segments of its own). */
#define DR_SUB_SIDE        0x0002

typedef struct DRTitleConfig {
    int                 nb_tracks;
    const DRTitleTrack *tracks;    /**< track 0: the master video */
    int                 lpcm_hd;   /**< LPCM payloads start with HD DVD's 5-byte header (else DVD-Video's 3) */
    const int64_t      *marks;     /**< chapter marks (DRChapterPlan.marks) or NULL */
    int                 nb_marks;
    int64_t            *open_marks;/**< the marks again when some are positions (DRJoinConfig.open_marks) */
    DREventCb           event; void *event_opaque;   /**< every event (also logged at debug level) */
    const char         *cc_helper; /**< the caption converter program (for a DR_KIND_CAPTION track) */
    int                 nb_segments; /**< the segments the title is read in (0: not known) */
    DRPlaceCb           place; void *place_opaque;   /**< where a segment's block lies (NULL: not told) */
} DRTitleConfig;

typedef struct DRTitle DRTitle;

/** Once segment / payload / finish fail (not EAGAIN / EINVAL), the title
 *  stays failed: those calls and ff_disctl_title_frame return the same
 *  error from then on. */
int  ff_disctl_title_open(DRTitle **t, void *logctx, const DRTitleConfig *cfg);
/** The next segment begins (ends the one before). */
int  ff_disctl_title_segment(DRTitle *t);
/** A payload of a track in the current segment: time in ticks or
 *  DR_NOPTS_VALUE; tail = read from the last 64 MiB of the segment's source.
 *  LPCM payloads start with their audio frame header. */
int  ff_disctl_title_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time);
/** The same, for a payload from block (of the segment, from 0; -1 not
 *  known): bytes of it that are left out are said with their place. */
int  ff_disctl_title_payload_at(DRTitle *t, int track, const uint8_t *data, int size, int64_t time,
                                int64_t block);
/** A DR_SUB_SIDE track's clip begins (before any segment or with them):
 *  offset = the ticks its clock lies ahead of the clock of the title's
 *  first segment (its frames go out at their time - offset - that
 *  segment's start); where names the clip for the log. */
int  ff_disctl_title_side_open(DRTitle *t, int track, int64_t offset, const char *where);
/** A payload of a DR_SUB_SIDE track: time in ticks (its clip's clock) or
 *  DR_NOPTS_VALUE. */
int  ff_disctl_title_side_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time);
/** The end of a DR_SUB_SIDE track's clip. */
int  ff_disctl_title_side_end(DRTitle *t, int track);
/** A text subtitle track's dialog style segment once read (else NULL). */
const uint8_t *ff_disctl_title_textst_style(const DRTitle *t, int track, int *size);
/** The end of the title: every stage finishes. */
int  ff_disctl_title_finish(DRTitle *t);
/** The next output frame (taken over by the caller): 0, DR_ERR(EAGAIN)
 *  when none is ready yet, DR_ERR_EOF after the last one. */
int  ff_disctl_title_frame(DRTitle *t, int *track, DRFrame *frame);
/** A chapter's position is reached in the video's source (DVD-Video: a
 *  byte offset in the segment): the first video unit from the bytes given
 *  so far gets DR_F_POSITION, and the next open chapter mark its time. */
int  ff_disctl_title_chapter_position(DRTitle *t);
/** A place in the video's source (where the title reader left blocks out):
 *  the first picture from the bytes given so far marks it. Its number (from
 *  0), or < 0. */
int  ff_disctl_title_place(DRTitle *t);
/** Place p's time on the title timeline (ticks): the end of the pictures
 *  given out before it; 0, or < 0 while not known (no picture after it). */
int  ff_disctl_title_place_time(const DRTitle *t, int p, int64_t *time);
/** After finish: the chapters of the plan the marks came from (*out freed
 *  with dr_free()). */
int  ff_disctl_title_chapters(const DRTitle *t, const DRChapterPlan *plan, DRChapter **out, int *nb_out);
/** PGS: the forced objects the track shows (a forced-only track: none
 *  means it shows nothing). */
int ff_disctl_title_forced_objects(const DRTitle *t, int track, int64_t *forced);
/** A linear PCM track's format once its first header was read (else NULL). */
const DRLpcm *ff_disctl_title_lpcm(const DRTitle *t, int track);
/** Untested features met so far (each kind once per track): said in the log
 *  ("[untested on real discs]"), for the caller's report. */
int64_t ff_disctl_title_review(const DRTitle *t);
/** After finish: a track's frames given out, the warnings logged about it
 *  (junction events, grid findings, checks) and (audio) its start delay. */
int ff_disctl_title_track_result(const DRTitle *t, int track, int64_t *frames, int64_t *warnings, int64_t *delay);
/** After finish, a sub-picture track: units without a stop time shown until
 *  the next one, units drawn outside the track's canvas. */
int ff_disctl_title_spu_result(const DRTitle *t, int track, int64_t *no_stop, int64_t *outside);
/** After finish, the caption track: caption blocks given to the converter,
 *  cues given out, captions still shown at a segment's end (left out). */
int ff_disctl_title_cc_result(const DRTitle *t, int track, int64_t *blocks, int64_t *cues, int64_t *lost);
/** After finish, a title without a caption track: the caption blocks taken
 *  out of its video and left out, and their entries that carry caption
 *  bytes (0: the caption slot was empty). */
void ff_disctl_title_cc_dropped(const DRTitle *t, int64_t *blocks, int64_t *text);
/** Segment seg's start (from 0) once the joiner placed it: its first video
 *  time (ticks, the segment's own clock); DR_ERR(EAGAIN) before. */
int ff_disctl_title_segment_start(const DRTitle *t, int seg, int64_t *start);
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


#endif /* DISC_TIMELINE_DISCTL_H */
