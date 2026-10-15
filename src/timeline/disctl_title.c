/*
 * The title timeline: one title through all stages. Per segment each track
 * has a frame step (its codec's rule) and a timing stage (video grid, audio durations, sub-pictures
 * on the video grid); the joiner lays the segments end to end; each audio
 * output track then passes the junction (or the PCM strategy for linear
 * PCM); every output frame is checked and queued for the format demuxer.
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>


#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/vlog.h"

#include "timeline/disctl.h"

typedef struct Out {
    int     track;
    DRFrame f;
} Out;

/* One track: its stages in the current segment, and its output side. */
typedef struct Track {
    DRTitle      *title;
    int           index;
    DRTitleTrack  cfg;
    DRLpcm        lpcm;          /* linear PCM: the headers seen (the frame step's format and the audio stage) */

    /* the current segment */
    DRFrames     *cut;
    DRVideo      *video;
    DRAudio      *audio;
    DRSpu        *spu;
    int64_t       bytes;         /* payload bytes given to the frame step */
    int64_t      *posq;          /* video: chapter positions reached (bytes given by then), oldest first */
    int           nb_posq, posq_cap;
    int64_t      *placeq;        /* video: places reached (bytes given by then), oldest first */
    int           nb_placeq, placeq_cap;

    /* the output side */
    DRAudioHeader header;        /* audio: the stream values of its first segment that gave them */
    int           have_header;
    DRJunction   *junction;
    DRPcm        *pcm;
    DRVerify     *verify;
    DRFrame       held;          /* sub-pictures: a unit without a stop time, waiting for the next one */
    int           holding;
    int64_t       extended;      /* sub-pictures: units without a stop time shown until the next one */
    int64_t       outside;       /* sub-pictures: units drawn outside the canvas */
    int64_t       forced_objects;/* PGS: forced objects shown */
    DRCcx        *ccx;           /* captions: the converter, from the first block on */
    int64_t       cc_blocks;     /* captions: caption blocks given to the converter */
    int64_t       cc_origin;     /* captions: the segment converter's origin (its first block's time) */
    int           cc_segments;   /* captions: segments converted */
    int           cc_lost;       /* captions: captions shown at a segment's end left out */
    int64_t       cc_cues;       /* captions: cues given out */
    DRFrame      *wait;          /* audio frames waiting for the video */
    int           wait_head, nb_wait, wait_cap;
    int64_t       out;           /* frames given out */
    int64_t       warnings;      /* warnings logged about the track */
    /* a second layer: the bytes of the picture being cut, and its pictures
     * waiting for their master video frame */
    uint8_t      *acc;
    int           acc_len, acc_cap;
    int64_t       acc_pos;       /* stream offset of acc[0] */
    int           acc_started;   /* a delimiter was found in this segment */
    int           acc_scan;      /* where the search for the next delimiter goes on in acc */
    int64_t      (*rec)[2];      /* payload times not yet taken: {stream offset of the payload, time} */
    int           nb_rec, rec_cap;
    DRFrame      *lq;
    int           lq_head, lq_n, lq_cap;
    int64_t       layer_dropped[32];  /* NAL units taken out, per type */
    int64_t       layer_stuffing;
    int64_t       layer_alone;   /* pictures without a master video frame (left out) */
    int64_t       stray_n;       /* the run of such pictures being counted, and its first one's disc time */
    int64_t       stray_src;
    DRHevcDropped el_dropped;    /* HEVC second layer: what its clean-up took out */
    /* HEVC: a frame with an end of sequence / end of bitstream unit, waiting
     * for the next picture (ff_disctl_hevc_seq_end) */
    DRFrame       seq_held;
    int           seq_holding;
    int64_t       seq_removed;   /* end units taken out */
    int64_t       seq_kept;      /* end units kept */
    /* a text subtitle track from a clip of its own (DR_SUB_SIDE): its stage
     * (k->cut its frame step, for the whole clip), its clock's offset, and
     * its frames waiting for the first segment's place and the video */
    DRTextst     *textst;
    char         *side_where;
    int64_t       side_offset;
    int           side_open, side_ended;
    DRFrame      *sq;
    int           sq_head, sq_n, sq_cap;
    int64_t       side_early;    /* frames left out: before the title's start */
    int64_t       side_late;     /* frames that start after the video's end (kept) */
} Track;

/* A master video frame given out while the title has a second layer: the
 * values its layer picture leaves with. */
typedef struct LayerKey {
    int64_t  src, time, dur;
    unsigned flags;
    DRFrame  f;                  /* a merged second layer: the master frame itself, waiting for its picture */
} LayerKey;

#define EVENT_KINDS  64
#define EVENT_LOGGED 50          /* structured event lines per kind and track; the rest are counted */

/* ---- the title checks of the master video's segments and joins ----
 * Nothing is changed; every finding is an event (a warning where it may be
 * a fault), and the title's end gives a summary. Per segment: how decoding
 * starts at its first key frame (the codec's marks, DRStart) and how many
 * pictures after it in decoding order are shown before it (they may need,
 * or cannot have, the pictures before it: the end of the segment before, or
 * nothing at the title's start); whether its first picture is a key frame;
 * whether it starts with pictures the segment before ends with, the same
 * bytes in the same order, a key frame among them (a piece of video
 * repeated at the join; single equal pictures are no finding: a still
 * scene codes to equal skipped pictures); the pictures the video stage read
 * and gave out. */

#define JOIN_PICTURES 32         /* pictures compared on each side of a join */

typedef struct JoinCheck {
    /* the current segment */
    int64_t  frames;             /* pictures handed on */
    int      have_key;           /* its first key frame was seen */
    int64_t  key_time;           /* (segment clock) */
    int      start;              /* its DRStart */
    int64_t  leading;            /* pictures after it shown before it */
    int      counting;           /* still counting them */
    uint64_t first[JOIN_PICTURES];   /* the hashes of its first pictures */
    int      first_key[JOIN_PICTURES];
    int64_t  first_time;
    int      nb_first;
    int      rep_done;           /* compared with the end of the segment before */
    uint64_t last[JOIN_PICTURES];    /* and of its last ones (a ring) */
    int      nb_last, last_head;
    /* the segment before: its last pictures' hashes, oldest first */
    uint64_t prev[JOIN_PICTURES];
    int      nb_prev;
    /* the title */
    int64_t  starts[4];          /* segments per DRStart of their first key frame */
    int64_t  start_issues;       /* segments whose start may need, or cannot have, pictures before it */
    int64_t  not_key;            /* segments whose first picture is no key frame */
    int64_t  repeats;            /* joins with repeated pictures */
    int64_t  pictures_off;       /* segments whose pictures read and given out differ */
    DRVideoStats total;          /* the video stages' statistics, summed */
} JoinCheck;

struct DRTitle {
    void         *log;
    DRTitleConfig cfg;
    Track        *t;
    DRJoin       *join;
    DREvent      *pend;          /* events of the segment's timing stages waiting for its place */
    int           nb_pend, pend_cap;
    int64_t      *ev_count;      /* per track and kind */
    int           segments;
    int64_t       vmax;          /* highest video time handed on, INT64_MIN before the first */
    int64_t       vend;          /* end of the video (highest time + duration) */
    int64_t       vpic_end;      /* end of the pictures handed on (markers left aside), INT64_MIN before */
    int64_t      *places;        /* per place: the end of the pictures before it */
    int           nb_places, places_cap, places_timed;
    int           video_done;
    int           finished;
    int64_t       review;        /* untested features met (audio reviews) */
    int           error;         /* the title failed: every later call returns this */
    int           cc_track;      /* the caption track, -1 none */
    char         *cc_helper;     /* the caption converter's path (cfg.cc_helper points here) */
    int64_t       cc_dropped;    /* caption blocks in the video of a title without a caption track */
    int64_t       cc_text;       /* and their entries that carry caption bytes (not the null code) */
    int64_t       user_data;     /* other GOP user data taken out of the video */
    JoinCheck     jc;            /* the checks of the master video's segments and joins */
    Out          *q;             /* output frames ready */
    int           q_head, q_n, q_cap;
    int           layer;         /* the second layer's track, -1 none */
    LayerKey     *bq;            /* master video frames waiting for their layer picture */
    int           bq_head, bq_n, bq_cap;
    int64_t       layer_missing; /* master video frames without a layer picture */
    int64_t       gap_n;         /* the run of such frames being counted (a gap), and its first one's time */
    int64_t       gap_time;
    int           layer_hevc;    /* the second layer is HEVC (a Dolby Vision enhancement layer) */
    int           layer_merged;  /* its pictures go into the master frames (DR_LAYER_MERGED) */
};

static const char *const event_names[] = {
    [DR_EV_START_GAP]            = "start_gap",
    [DR_EV_START_EARLY]          = "start_early",
    [DR_EV_OVERLAP]              = "overlap",
    [DR_EV_GAP_ABSORBED]         = "gap_absorbed",
    [DR_EV_GAP]                  = "gap",
    [DR_EV_DROP]                 = "drop",
    [DR_EV_GAP_MARKER]           = "gap_marker",
    [DR_EV_VIDEO_ENDED]          = "video_ended",
    [DR_EV_TIME_ORDER]           = "time_order",
    [DR_EV_RETIME]               = "retime",
    [DR_EV_VIDEO_TIMECODE]       = "video_timecode",
    [DR_EV_VIDEO_TIMECODE_LIMIT] = "video_timecode_limit",
    [DR_EV_VIDEO_INVALID]        = "video_invalid",
    [DR_EV_VIDEO_REPAIR]         = "video_repair",
    [DR_EV_VIDEO_RATE_CHANGE]    = "video_rate_change",
    [DR_EV_SUB_UNTIMED]          = "sub_untimed",
    [DR_EV_SUB_EARLY]            = "sub_early",
    [DR_EV_SUB_OVERLAP]          = "sub_overlap",
    [DR_EV_VERIFY_UNIT]          = "verify_unit",
    [DR_EV_VERIFY_CRC]           = "verify_crc",
    [DR_EV_VERIFY_ORDER]         = "verify_order",
    [DR_EV_VERIFY_HOLE]          = "verify_hole",
    [DR_EV_VERIFY_OVERLAP]       = "verify_overlap",
    [DR_EV_PCM_SILENCE]          = "pcm_silence",
    [DR_EV_PCM_SKIP]             = "pcm_skip",
    [DR_EV_PCM_TIMECODE]         = "pcm_timecode",
    [DR_EV_JOIN_START]           = "join_start",
    [DR_EV_JOIN_NOT_KEY]         = "join_not_key",
    [DR_EV_JOIN_REPEAT]          = "join_repeat",
    [DR_EV_SEQ_END_INSIDE]       = "seq_end_inside",
    [DR_EV_PICTURES]             = "pictures",
    [DR_EV_SYNC]                 = "sync",
    [DR_EV_AUDIO_COPY]           = "audio_copy",
    [DR_EV_AUDIO_COPY_LONG]      = "audio_copy_long",
    [DR_EV_SAMPLE_JOIN]          = "sample_join",
};

const char *ff_disctl_event_name(int kind)
{
    return kind > 0 && kind < DR_ARRAY_ELEMS(event_names) && event_names[kind] ? event_names[kind] : "unknown";
}

/* A time on the title timeline as h:mm:ss.mmm (truncated). */
static const char *hms(char *buf, int size, int64_t t)
{
    int64_t ms = t < 0 ? 0 : t / (DR_TICKS_PER_SECOND / 1000);

    snprintf(buf, size, "%d:%02d:%02d.%03d", (int)(ms / 3600000), (int)(ms / 60000 % 60), (int)(ms / 1000 % 60),
             (int)(ms % 1000));
    return buf;
}

/* A formatted piece of a log line into buf. */
static const char *strf(char *buf, int size, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return buf;
}

/* Ticks as milliseconds with three decimals (truncated toward zero). */
static const char *msec(char *buf, int size, int64_t t, int sign)
{
    int64_t us = (t < 0 ? -t : t) * 1000 / (DR_TICKS_PER_SECOND / 1000);

    snprintf(buf, size, "%s%"PRId64".%03d", t < 0 ? "-" : sign ? "+" : "", us / 1000, (int)(us % 1000));
    return buf;
}

/* The events a reader of the log must see, as one warning each: what the
 * junction did to the audio (skew, gaps, drops), the video grid's findings,
 * PCM silence. */
static void event_text(DRTitle *t, const DREvent *e)
{
    char a[32], b[32], c[32];
    int k = e->track;

    switch (e->kind) {
    case DR_EV_START_EARLY:
        /* no fault: the track keeps its frames before the video (a negative delay) */
        dr_vlog(t->log, DR_VLOG_INFO, "Track %d: the audio starts %s ms before the video on the disc: every frame "
               "kept at its time (a negative delay)\n", k, msec(a, 32, e->dur, 0));
        return;
    case DR_EV_DROP:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: %"PRId64" frame(s) (%s ms) dropped to bring the audio skew "
               "to %s ms\n", k, hms(a, 32, e->pos), e->count, msec(b, 32, e->dur, 0), msec(c, 32, e->skew, 1));
        break;
    case DR_EV_AUDIO_COPY:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: the next segment starts with a copy of the last frame (%s ms; "
               "the stamps overlap by %s ms): the copy is left out\n", k, hms(a, 32, e->pos), msec(b, 32, e->dur, 0),
               msec(c, 32, e->count, 0));
        break;
    case DR_EV_AUDIO_COPY_LONG:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: the next segment starts with copies of the last %"PRId64
               " frames (the stamps overlap by %s ms): found only, nothing left out\n", k, hms(a, 32, e->pos),
               e->count, msec(b, 32, e->dur, 0));
        break;
    case DR_EV_SAMPLE_JOIN:
        switch (e->count) {
        case DR_JOIN_CONTINUES:
            dr_vlog(t->log, DR_VLOG_INFO, "Track %d at %s: join: the next segment continues at the sample where this "
                   "one ends: everything kept\n", k, hms(a, 32, e->pos));
            return;
        case DR_JOIN_SAME:
            dr_vlog(t->log, DR_VLOG_INFO, "Track %d at %s: join: the next segment's first unit holds the last %"PRId64
                   " samples of this one again: left out\n", k, hms(a, 32, e->pos), e->dur);
            return;
        case DR_JOIN_SAME_KEPT:
            dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: join: the next segment's first unit holds the last %"PRId64
                   " samples of this one again, but the unit after it is no major sync: kept\n", k,
                   hms(a, 32, e->pos), e->dur);
            return;
        case DR_JOIN_APART:
            dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: join: the sample numbers do not connect (%+"PRId64
                   " samples; a piece on another timeline): the timing rule decides\n", k, hms(a, 32, e->pos), e->dur);
            return;
        default:
            dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: join: a side has no sample numbers: the timing rule "
                   "decides\n", k, hms(a, 32, e->pos));
            return;
        }
    case DR_EV_OVERLAP:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: frames overlap by %s ms; audio skew now %s ms\n", k,
               hms(a, 32, e->pos), msec(b, 32, e->dur, 0), msec(c, 32, e->skew, 1));
        break;
    case DR_EV_GAP_ABSORBED:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: short audio gap of %s ms closed; audio skew now %s ms\n", k,
               hms(a, 32, e->pos), msec(b, 32, e->dur, 0), msec(c, 32, e->skew, 1));
        break;
    case DR_EV_GAP:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: audio gap of %s ms (%"PRId64".%03d frames missing), "
               "left open\n", k, hms(a, 32, e->pos), msec(b, 32, e->dur, 0), e->count / 1000, (int)(e->count % 1000));
        break;
    case DR_EV_VIDEO_ENDED:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: the video ends before this audio: the rest of the track is "
               "left out\n", k, hms(a, 32, e->pos));
        break;
    case DR_EV_VIDEO_TIMECODE:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: a picture's disc time is %s ms off the frame grid\n", k,
               hms(a, 32, e->pos), msec(b, 32, e->dur, 1));
        break;
    case DR_EV_VIDEO_INVALID:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d: %"PRId64" picture(s) with disc times off the frame grid\n", k,
               e->count);
        break;
    case DR_EV_VIDEO_REPAIR:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: the frame grid follows a jump of the disc times (%"PRId64
               " placeholder(s))\n", k, hms(a, 32, e->pos), e->count);
        break;
    case DR_EV_PCM_SILENCE:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: a gap of %s ms filled with silence\n", k,
               hms(a, 32, e->pos), msec(b, 32, e->dur, 0));
        break;
    case DR_EV_PCM_SKIP:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d: the audio starts %s ms after the video: skipped on the output "
               "clock\n", k, msec(b, 32, e->dur, 0));
        break;
    case DR_EV_JOIN_START:
        if (e->skew == DR_START_CLEAN || !e->count)
            return;
        if (e->skew == DR_START_BROKEN)
            dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: segment %"PRId64" starts at a broken link: %"PRId64
                   " picture(s) shown before its first key frame cannot be decoded\n", k, hms(a, 32, e->pos),
                   e->dur, e->count);
        else
            dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: segment %"PRId64" starts with %s key frame: %"PRId64
                   " picture(s) shown before it may need pictures from %s\n", k, hms(a, 32, e->pos), e->dur,
                   e->skew == DR_START_OPEN ? "an open" : "a", e->count,
                   e->dur > 1 ? "the end of the segment before (the join)" : "before the title's start");
        break;
    case DR_EV_JOIN_NOT_KEY:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: segment %"PRId64" starts with a picture that is no key "
               "frame\n", k, hms(a, 32, e->pos), e->dur);
        break;
    case DR_EV_JOIN_REPEAT:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: segment %"PRId64" starts with the %"PRId64" picture(s) the "
               "segment before ends with (the same bytes, in the same order): repeated at the join\n", k,
               hms(a, 32, e->pos), e->skew, e->count);
        break;
    case DR_EV_SEQ_END_INSIDE:
        if (e->dur) {
            /* a sequence change: no warning */
            dr_vlog(t->log, DR_VLOG_INFO, "Track %d at %s: an end of sequence code, then a new sequence (its sequence "
                   "header) in the same segment: kept as it is\n", k, hms(a, 32, e->pos));
            return;
        }
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: an end of sequence code with pictures after it in the "
               "same segment and no new sequence header\n", k, hms(a, 32, e->pos));
        break;
    case DR_EV_PICTURES:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d: segment %"PRId64": %"PRId64" picture(s) read but not given out\n",
               k, e->dur, e->count);
        break;
    case DR_EV_SYNC:
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: segment %"PRId64": the audio is %s ms off its time on the "
               "disc as a stream file plays it\n", k, hms(a, 32, e->pos), e->count + 1, msec(b, 32, e->dur, 1));
        break;
    default:
        return;
    }
    if (k >= 0 && k < t->cfg.nb_tracks)
        t->t[k].warnings++;
}

/* Every event: one structured line at debug level (the first ones of each
 * kind and track), the readable warning where there is one, then the
 * caller's callback. */
static void on_event(void *opaque, const DREvent *e)
{
    DRTitle *t = opaque;
    int64_t *n = e->track >= 0 && e->track < t->cfg.nb_tracks && e->kind > 0 && e->kind < EVENT_KINDS ?
                 &t->ev_count[e->track * EVENT_KINDS + e->kind] : NULL;

    if (!n || ++*n <= EVENT_LOGGED)
        dr_vlog(t->log, DR_VLOG_DEBUG, "Title timeline: event %s track=%d pos=%"PRId64" dur=%"PRId64" skew=%"PRId64
               " count=%"PRId64"\n", ff_disctl_event_name(e->kind), e->track, e->pos, e->dur, e->skew, e->count);
    event_text(t, e);
    if (e->kind >= DR_EV_VERIFY_UNIT && e->kind <= DR_EV_VERIFY_OVERLAP && e->track >= 0 &&
        e->track < t->cfg.nb_tracks)
        t->t[e->track].warnings++;
    if (t->cfg.event)
        t->cfg.event(t->cfg.event_opaque, e);
}

/* Events of the video and sub-picture stages carry times of the segment's
 * own clock (a sub-picture without a time: its byte offset); they wait
 * until the joiner knows the segment's place and then move with it onto
 * the title timeline. */
static int seg_event_dispatch(DRTitle *t, int force)
{
    DRJoinStats js;

    if (!t->nb_pend)
        return 0;
    ff_disctl_join_stats(t->join, &js);
    if (!js.placed && !force)
        return 0;
    for (int i = 0; i < t->nb_pend; i++) {
        DREvent e = t->pend[i];
        if (js.placed && e.kind != DR_EV_SUB_UNTIMED)
            e.pos = e.pos - js.start + js.offset;
        on_event(t, &e);
    }
    t->nb_pend = 0;
    return 0;
}

static void on_seg_event(void *opaque, const DREvent *e)
{
    DRTitle *t = opaque;

    if (t->nb_pend == t->pend_cap) {
        int cap = t->pend_cap ? 2 * t->pend_cap : 64;
        DREvent *p = dr_realloc_array(t->pend, cap, sizeof(*p));
        if (!p) {
            dr_vlog(t->log, DR_VLOG_ERROR, "Title timeline: no memory for an event: given on its segment's clock\n");
            on_event(t, e);
            return;
        }
        t->pend     = p;
        t->pend_cap = cap;
    }
    t->pend[t->nb_pend++] = *e;
}

/* ---- the output side ---- */

/* A frame leaves the core: checked, then queued for the demuxer. */
static int emit(DRTitle *t, Track *k, DRFrame *f)
{
    int ret;

    if (k->verify && (ret = ff_disctl_verify_frame(k->verify, f)) < 0) {
        ff_disctl_frame_unref(f);
        return ret;
    }
    if (t->q_head + t->q_n == t->q_cap) {
        if (t->q_head) {
            memmove(t->q, t->q + t->q_head, t->q_n * sizeof(*t->q));
            t->q_head = 0;
        } else {
            int cap = t->q_cap ? 2 * t->q_cap : 256;
            Out *q = dr_realloc_array(t->q, cap, sizeof(*q));
            if (!q) {
                ff_disctl_frame_unref(f);
                return DR_ERR(ENOMEM);
            }
            t->q     = q;
            t->q_cap = cap;
        }
    }
    t->q[t->q_head + t->q_n++] = (Out){ k->index, *f };
    memset(f, 0, sizeof(*f));
    k->out++;
    return 0;
}

static int track_out(void *opaque, DRFrame *f)
{
    Track *k = opaque;
    return emit(k->title, k, f);
}

static int layer_key(DRTitle *t, DRFrame *f);
static int layer_match(DRTitle *t, int final);

/* A video frame on its way out: its second-layer key taken, given out,
 * second-layer pictures matched. */
static int video_out(DRTitle *t, Track *k, DRFrame *f)
{
    int ret;

    if (t->layer >= 0 && (f->dur || f->size)) {
        if ((ret = layer_key(t, f)) < 0) {
            ff_disctl_frame_unref(f);
            return ret;
        }
        if (t->layer_merged)            /* it leaves with its layer picture */
            return layer_match(t, 0);
    }
    if ((ret = emit(t, k, f)) < 0)
        return ret;
    return layer_match(t, 0);
}

/* The held HEVC frame with an end of sequence / end of bitstream unit, now
 * that the next frame is known (next: NULL at the title's end): decided
 * and given out. */
static int seq_end_out(DRTitle *t, Track *k, const DRFrame *next)
{
    DRFrame f = k->seq_held;
    char a[32];
    int ret;

    k->seq_holding = 0;
    memset(&k->seq_held, 0, sizeof(k->seq_held));
    if ((ret = ff_disctl_hevc_seq_end(&f, next)) < 0) {
        ff_disctl_frame_unref(&f);
        return ret;
    }
    if (!ret) {
        k->seq_removed++;
        dr_vlog(t->log, DR_VLOG_VERBOSE, "Title timeline: track %d: end of sequence unit at %s taken out (%s: it "
               "changes nothing there)\n", k->index, hms(a, 32, f.time),
               next ? "an IDR or BLA picture follows" : "the title ends");
    } else {
        k->seq_kept++;
        k->warnings++;
        dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: track %d: end of sequence unit at %s kept: %s [untested on "
               "real discs]\n", k->index, hms(a, 32, f.time), ret == 1 ?
               "a CRA picture follows, whose leading pictures it marks as not to be shown (H.265 7.4.3.6)" :
               "the next picture is no random access point (a stream fault, H.265 7.4.2.4.3)");
    }
    return video_out(t, k, &f);
}

/* The video as the junction sees it: what the joiner has handed on. */
static int64_t video_max(void *opaque)
{
    return ((DRTitle *)opaque)->vmax;
}

static int video_advance(void *opaque, int64_t target, int *ended)
{
    DRTitle *t = opaque;

    /* audio frames are held until the video is past them or has ended, so
     * the video is never needed further than it is */
    if (!t->video_done && t->vmax < target)
        dr_vlog(t->log, DR_VLOG_DEBUG, "Title timeline: the junction asks for video at %"PRId64", handed on up to %"
               PRId64"\n", target, t->vmax);
    *ended = t->video_done && t->vmax < target;
    return 0;
}

/* The output stage of an audio track, opened with its first frame (the
 * stream values are known then). */
static int audio_out_open(DRTitle *t, Track *k)
{
    DRVideoRef vref = { t, video_max, video_advance };

    if (k->cfg.codec == DR_CODEC_PCM_DVD || k->cfg.codec == DR_CODEC_PCM_BLURAY) {
        DRPcmConfig pc = {
            .track = k->index, .rate = k->lpcm.rate, .bits = k->lpcm.bits > 16 ? 24 : 16,
            .bytes_per_sample_frame = k->lpcm.spf ? k->lpcm.out_frame_bytes / k->lpcm.spf : 0,
            .video = vref, .out = track_out, .out_opaque = k, .event = on_event, .event_opaque = t,
        };
        if (!k->lpcm.init || !pc.bytes_per_sample_frame)
            return DR_ERR_BUG;
        return ff_disctl_pcm_open(&k->pcm, t->log, &pc);
    } else {
        DRJunctionConfig jc = {
            .track = k->index, .video = vref,
            .out = track_out, .out_opaque = k, .event = on_event, .event_opaque = t,
            .codec = k->cfg.codec, .rate = k->have_header ? k->header.rate : 0,
        };
        if (k->have_header && k->header.rate > 0)
            jc.frame_dur = (int64_t)((uint64_t)k->header.samples * DR_TICKS_PER_SECOND / (uint64_t)k->header.rate);
        return ff_disctl_junction_open(&k->junction, t->log, &jc);
    }
}

static int audio_push(DRTitle *t, Track *k, DRFrame *f)
{
    int ret;

    if (!k->junction && !k->pcm && (ret = audio_out_open(t, k)) < 0) {
        ff_disctl_frame_unref(f);
        return ret;
    }
    return k->pcm ? ff_disctl_pcm_push(k->pcm, f) : ff_disctl_junction_push(k->junction, f);
}

static int side_release(DRTitle *t);

/* Audio frames the video has passed (all of them once it has ended) go on. */
static int release(DRTitle *t)
{
    int ret;

    if ((ret = side_release(t)) < 0)
        return ret;
    for (int i = 1; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];

        while (k->nb_wait) {
            DRFrame f = k->wait[k->wait_head];

            if (!t->video_done && f.time > t->vmax)
                break;
            k->wait_head++;
            k->nb_wait--;
            if ((ret = audio_push(t, k, &f)) < 0)
                return ret;
        }
        if (!k->nb_wait)
            k->wait_head = 0;
    }
    return 0;
}

static int wait_add(Track *k, DRFrame *f)
{
    if (k->wait_head + k->nb_wait == k->wait_cap) {
        if (k->wait_head) {
            memmove(k->wait, k->wait + k->wait_head, k->nb_wait * sizeof(*k->wait));
            k->wait_head = 0;
        } else {
            int cap = k->wait_cap ? 2 * k->wait_cap : 256;
            DRFrame *w = dr_realloc_array(k->wait, cap, sizeof(*w));
            if (!w)
                return DR_ERR(ENOMEM);
            k->wait     = w;
            k->wait_cap = cap;
        }
    }
    k->wait[k->wait_head + k->nb_wait++] = *f;
    memset(f, 0, sizeof(*f));
    return 0;
}

/* Sub-pictures: a unit without a stop time stays on screen until the next
 * unit of its track replaces it (the disc's decoder model), so it waits for
 * that unit and lasts until its start; a unit with a stop time keeps it. The
 * last unit of the title without a stop time lasts until the video's end. */
static int spu_held_out(DRTitle *t, Track *k, int64_t until)
{
    DRFrame *h = &k->held;
    char b[32];

    k->holding = 0;
    if (until > h->time) {
        h->dur = until - h->time;
        k->extended++;
    } else {
        dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: a sub-picture without a stop time is followed by one "
               "that does not start later: it keeps its one delay step\n", k->index, hms(b, sizeof(b), h->time));
        k->warnings++;
    }
    return emit(t, k, h);
}

static int spu_joined(DRTitle *t, Track *k, DRFrame *f)
{
    int ret;

    if (k->holding && f->time != DR_NOPTS_VALUE && (ret = spu_held_out(t, k, f->time)) < 0) {
        ff_disctl_frame_unref(f);
        return ret;
    }
    if (!(f->flags & DR_F_NO_STOP) || f->time == DR_NOPTS_VALUE)
        return emit(t, k, f);
    if (k->holding && (ret = emit(t, k, &k->held)) < 0) {
        ff_disctl_frame_unref(f);
        return ret;
    }
    k->held    = *f;
    k->holding = 1;
    memset(f, 0, sizeof(*f));
    return 0;
}

/* The end of the title: a sub-picture still waiting lasts until the video's end. */
static int spu_finish(DRTitle *t, Track *k)
{
    char b[32], e[32];
    int ret;

    if (!k->holding)
        return 0;
    dr_vlog(t->log, DR_VLOG_WARNING, "Track %d at %s: the last sub-picture has no stop time: shown until the "
           "video's end at %s [untested on real discs]\n", k->index, hms(b, sizeof(b), k->held.time),
           hms(e, sizeof(e), t->vend));
    t->review++;
    if ((ret = spu_held_out(t, k, t->vend)) < 0)
        return ret;
    return 0;
}

/* ---- text subtitles from a clip of their own (DR_SUB_SIDE) ----
 * The clip is read on its own: its frames wait on its clock until the
 * title's first segment is placed, then go out at their time - the clip's
 * offset - that segment's start (where the reference places them); one
 * that comes out before the title's start is left out (said); they leave
 * as the video passes their time (the rest at the title's end; those that
 * start after the video's end are kept and counted). */

static int is_side(const Track *k)
{
    return k->cfg.kind == DR_KIND_SUBTITLE && (k->cfg.sub_flags & DR_SUB_SIDE);
}

static int side_frame(void *opaque, DRFrame *f)
{
    Track *k = opaque;

    if (k->sq_head + k->sq_n == k->sq_cap) {
        if (k->sq_head) {
            memmove(k->sq, k->sq + k->sq_head, k->sq_n * sizeof(*k->sq));
            k->sq_head = 0;
        } else {
            int cap = k->sq_cap ? 2 * k->sq_cap : 64;
            DRFrame *q = dr_realloc_array(k->sq, cap, sizeof(*q));
            if (!q) {
                ff_disctl_frame_unref(f);
                return DR_ERR(ENOMEM);
            }
            k->sq     = q;
            k->sq_cap = cap;
        }
    }
    k->sq[k->sq_head + k->sq_n++] = *f;
    memset(f, 0, sizeof(*f));
    return 0;
}

static int side_release(DRTitle *t)
{
    int64_t start0;
    char b[32];
    int ret;

    if (ff_disctl_join_segment_start(t->join, 0, &start0) < 0)
        return 0;
    for (int i = 1; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];

        if (!is_side(k))
            continue;
        while (k->sq_n) {
            DRFrame f = k->sq[k->sq_head];
            int64_t at = f.time - k->side_offset - start0;

            if (at >= 0 && !t->video_done && at > t->vmax)
                break;
            k->sq_head++;
            k->sq_n--;
            if (at < 0) {
                dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: TextST track %d: the dialog at stream offset %"PRId64
                       " of %s starts %s before the title's start: left out\n", i, f.pos, k->side_where,
                       hms(b, sizeof(b), -at));
                k->side_early++;
                k->warnings++;
                ff_disctl_frame_unref(&f);
                continue;
            }
            if (t->video_done && at >= t->vend)
                k->side_late++;
            f.time = f.src = at;
            f.seg  = 0;
            if ((ret = emit(t, k, &f)) < 0)
                return ret;
        }
        if (!k->sq_n)
            k->sq_head = 0;
    }
    return 0;
}

/* The end of the title: what the side tracks gave out and left out. */
static void side_finish(DRTitle *t, Track *k)
{
    DRTextstStats st;
    char b[32];

    if (k->sq_n) {
        dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: TextST track %d: %d dialogs left out: the title's first "
               "segment has no place (no video)\n", k->index, k->sq_n);
        k->warnings++;
        for (int j = 0; j < k->sq_n; j++)
            ff_disctl_frame_unref(&k->sq[k->sq_head + j]);
        k->sq_n = k->sq_head = 0;
    }
    if (!k->textst)
        return;
    ff_disctl_textst_stats(k->textst, &st);
    k->warnings += st.warnings;
    dr_vlog(t->log, DR_VLOG_INFO, "Title timeline: TextST track %d: %"PRId64" of %"PRId64" dialogs of %s given out%s"
           "\n", k->index, k->out, st.dialogs, k->side_where,
           (k->cfg.sub_flags & DR_SUB_FORCED_ONLY) ? " (forced subtitles only)" : "");
    if (k->cfg.sub_flags & DR_SUB_FORCED_ONLY)
        dr_vlog(t->log, DR_VLOG_INFO, "Title timeline: TextST track %d: forced subtitles: %"PRId64" dialogs without a "
               "forced region not shown, %"PRId64" regions without forced_on_flag taken out of the others, as a "
               "player shows them with subtitles off [untested on real discs]\n", k->index, st.not_forced,
               st.regions_cut);
    /* the dialogs the style segment counts: those read and those the
     * parser refused (segments that do not parse) */
    if (ff_disctl_textst_style(k->textst, &(int){ 0 })) {
        DRFramesStats fs;
        ff_disctl_frames_stats(k->cut, &fs);
        if (st.disc_count != st.dialogs + fs.skipped) {
            dr_vlog(t->log, DR_VLOG_WARNING, "Title check: TextST track %d: the dialog style segment counts %d dialogs, "
                   "%s has %"PRId64" (and %"PRId64" pieces that are no segment)\n", k->index, st.disc_count,
                   k->side_where, st.dialogs, fs.skipped);
            k->warnings++;
        } else
            dr_vlog(t->log, DR_VLOG_VERBOSE, "Title check: TextST track %d: %d dialogs, as the dialog style segment "
                   "counts: checked\n", k->index, st.disc_count);
    }
    if (k->side_late) {
        dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: TextST track %d: %"PRId64" dialogs start after the video's "
               "end at %s: kept [untested on real discs]\n", k->index, k->side_late, hms(b, sizeof(b), t->vend));
        k->warnings++;
        t->review++;
    }
}

/* ---- closed captions ----
 * The MPEG-2 rules take GOP-level user data out of the video as side units,
 * timed on the picture they belong to. A DVD caption block among them goes
 * to the caption track's converter as its CEA-608 entries; each segment has
 * a converter of its own (the reference's way), started at the segment's
 * first caption block, whose time is the converter's origin: a block goes
 * in at its whole milliseconds since then (truncated). At the segment's end
 * the screen is cleared half a second past 71 hours after the origin (erase
 * displayed memory, sent twice as control codes are): a caption still shown
 * then ends after 71 h and is left out (the reference loses it the same way;
 * the converter writes a cue's end 1 ms before the clearing, so the clearing
 * must come later than 71 h + 1 ms); the converter's other cues go into the
 * joiner at the origin plus their times, like a subtitle track's units.
 * LOOK INTO AGAIN LATER: a caption shown across a segment's end is lost. */

#define CC_END_MS   255600000LL         /* 71 h */
#define CC_CLEAR_MS (CC_END_MS + 500)

static int cc_side(void *opaque, DRFrame *f)
{
    DRTitle *t = opaque;
    uint8_t tri[63 * 3];
    int n = ff_disctl_cc_check(f->data, f->size), m, ret = 0;
    int64_t ms;
    Track *k;

    if (!n) {
        t->user_data++;
        ff_disctl_frame_unref(f);
        return 0;
    }
    m = ff_disctl_cc_triplets(f->data, n, tri);
    if (t->cc_track < 0) {
        /* left out: counted, with the entries that carry caption bytes
         * (parity bits aside, not 0: the null code 80 80 and padding 00 00
         * carry nothing) */
        t->cc_dropped++;
        for (int i = 0; i < m; i++)
            t->cc_text += (tri[3 * i + 1] & 0x7F) || (tri[3 * i + 2] & 0x7F);
        ff_disctl_frame_unref(f);
        return 0;
    }
    k = &t->t[t->cc_track];
    if (!m || f->time == DR_NOPTS_VALUE) {
        ff_disctl_frame_unref(f);
        return 0;
    }
    if (!k->ccx) {
        if (!t->cfg.cc_helper || !*t->cfg.cc_helper) {
            dr_vlog(t->log, DR_VLOG_ERROR, "Captions: track %d: the converter program (%s) is not known: the "
                   "captions cannot be made into text\n", k->index, DR_CCX_PROGRAM);
            ret = DR_ERR(ENOENT);
        } else {
            ret = ff_disctl_ccx_start(&k->ccx, t->log, t->cfg.cc_helper);
            k->cc_origin = f->time;
        }
    }
    ms = (f->time - k->cc_origin) / (DR_TICKS_PER_SECOND / 1000);
    if (ret >= 0 && ms < 0) {
        /* the converter's times only go forward from its origin */
        dr_vlog(t->log, DR_VLOG_WARNING, "Captions: track %d: a caption block %"PRId64" ms before the segment's "
               "first one is left out\n", k->index, -ms);
        k->warnings++;
    } else if (ret >= 0 && (ret = ff_disctl_ccx_block(k->ccx, ms, tri, m)) >= 0) {
        k->cc_blocks++;
    }
    ff_disctl_frame_unref(f);
    return ret;
}

/* The end of a segment: its converter's cues into the joiner. */
static int cc_segment_end(DRTitle *t)
{
    static const uint8_t edm[6] = { 0x04, 0x94, 0x2C, 0x04, 0x94, 0x2C };
    int64_t tick = DR_TICKS_PER_SECOND / 1000;
    DRCue *cues = NULL;
    Track *k;
    int n = 0, lost = 0, ret;

    if (t->cc_track < 0 || !(k = &t->t[t->cc_track])->ccx)
        return 0;
    if ((ret = ff_disctl_ccx_block(k->ccx, CC_CLEAR_MS, edm, 2)) >= 0)
        ret = ff_disctl_ccx_finish(k->ccx, &cues, &n);
    ff_disctl_ccx_close(&k->ccx);
    k->cc_segments++;
    for (int i = 0; i < n && ret >= 0; i++) {
        DRFrame f = { .time = k->cc_origin + cues[i].start_ms * tick, .flags = DR_F_KEY, .pos = -1 };
        size_t len = strlen(cues[i].text);

        if (cues[i].start_ms > CC_END_MS)
            continue;
        if (cues[i].end_ms > CC_END_MS) {
            dr_vlog(t->log, DR_VLOG_VERBOSE, "Captions: track %d: a caption still shown at the end of segment %d "
                   "(from %"PRId64" ms after its first caption block) is left out\n", k->index, t->segments,
                   cues[i].start_ms);
            lost++;
            continue;
        }
        f.src = f.time;
        f.dur = DR_MAX(cues[i].end_ms - cues[i].start_ms, 0) * tick;
        if (!(f.buf = dr_buf_alloc(len + DR_BUF_PADDING))) {
            ret = DR_ERR(ENOMEM);
            break;
        }
        memcpy(dr_buf_data(f.buf), cues[i].text, len);
        memset(dr_buf_data(f.buf) + len, 0, DR_BUF_PADDING);
        f.data = dr_buf_data(f.buf);
        f.size = len;
        ret = ff_disctl_join_push(t->join, k->index, &f);
    }
    k->cc_lost += lost;
    ff_disctl_cues_free(&cues, n);
    return ret;
}

/* A cue on the title timeline: out. */
static int cc_joined(DRTitle *t, Track *k, DRFrame *f)
{
    k->cc_cues++;
    return emit(t, k, f);
}

/* The end of the title: what the converters made. */
static int cc_finish(DRTitle *t, Track *k)
{
    if (!k->cc_blocks) {
        dr_vlog(t->log, DR_VLOG_WARNING, "Captions: track %d: the video carries no caption block: no cues\n",
               k->index);
        k->warnings++;
        return 0;
    }
    dr_vlog(t->log, DR_VLOG_VERBOSE, "Captions: track %d: %"PRId64" caption blocks in %d segment(s) -> %"PRId64" cues "
           "(CCExtractor %s), %d caption(s) shown at a segment's end left out\n", k->index, k->cc_blocks,
           k->cc_segments, k->cc_cues, DR_CCX_VERSION, k->cc_lost);
    return 0;
}

static int seg_frame(void *opaque, DRFrame *f);

/* ---- a second layer (the dependent view of a 3-D video) ----
 * Its pictures and the master video's frames come in the same (decoding)
 * order; a picture belongs to the master frame with the same time from the
 * disc (within LAYER_TOLERANCE) and leaves with that frame's time, duration
 * and key flag. A picture without a master frame is left out; a master frame
 * without a picture is written without it (the reference program's way; the
 * layer's stream misses that picture). Each run of either is one warning. */

#define LAYER_TOLERANCE (DR_TICKS_PER_SECOND / 10000)   /* 0.1 ms, the video grid's threshold */
#define LAYER_WINDOW    64              /* pictures looked at for a master frame's partner */

static int layer_key(DRTitle *t, DRFrame *f)
{
    if (t->bq_head + t->bq_n == t->bq_cap) {
        if (t->bq_head) {
            memmove(t->bq, t->bq + t->bq_head, t->bq_n * sizeof(*t->bq));
            t->bq_head = 0;
        } else {
            int cap = t->bq_cap ? 2 * t->bq_cap : 64;
            LayerKey *q = dr_realloc_array(t->bq, cap, sizeof(*q));
            if (!q)
                return DR_ERR(ENOMEM);
            t->bq     = q;
            t->bq_cap = cap;
        }
    }
    t->bq[t->bq_head + t->bq_n++] = (LayerKey){ f->src, f->time, f->dur, f->flags };
    if (t->layer_merged) {
        t->bq[t->bq_head + t->bq_n - 1].f = *f;
        memset(f, 0, sizeof(*f));
    }
    return 0;
}

static int layer_add(Track *k, DRFrame *f)
{
    if (k->lq_head + k->lq_n == k->lq_cap) {
        if (k->lq_head) {
            memmove(k->lq, k->lq + k->lq_head, k->lq_n * sizeof(*k->lq));
            k->lq_head = 0;
        } else {
            int cap = k->lq_cap ? 2 * k->lq_cap : 64;
            DRFrame *q = dr_realloc_array(k->lq, cap, sizeof(*q));
            if (!q) {
                ff_disctl_frame_unref(f);
                return DR_ERR(ENOMEM);
            }
            k->lq     = q;
            k->lq_cap = cap;
        }
    }
    k->lq[k->lq_head + k->lq_n++] = *f;
    memset(f, 0, sizeof(*f));
    return 0;
}

/* A run of second-layer pictures without a video frame of their time:
 * one warning when it ends. */
static void layer_stray_flush(Track *k)
{
    if (!k->stray_n)
        return;
    dr_vlog(k->title->log, DR_VLOG_WARNING, "Title timeline: track %d: %"PRId64" %s picture%s from the disc time %.3f ms "
           "on %s no video frame of %s time: left out\n", k->index, k->stray_n,
           k->title->layer_hevc ? "Dolby Vision enhancement" : "second-view", k->stray_n == 1 ? "" : "s",
           k->stray_src / (double)(DR_TICKS_PER_SECOND / 1000), k->stray_n == 1 ? "has" : "have",
           k->stray_n == 1 ? "its" : "their");
    k->warnings++;
    k->stray_n = 0;
}

/* A run of video frames without their second-layer picture (a gap): one
 * warning when it ends. They go on without it, as the reference program
 * writes them. */
static void layer_gap_flush(DRTitle *t)
{
    if (!t->gap_n)
        return;
    if (t->layer_hevc)
        dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: at %s: %"PRId64" video frame%s without %s Dolby Vision "
               "enhancement picture: written without it (no Dolby Vision metadata for %s)\n",
               hms((char[32]){ 0 }, 32, t->gap_time), t->gap_n, t->gap_n == 1 ? "" : "s",
               t->gap_n == 1 ? "its" : "their", t->gap_n == 1 ? "it" : "them");
    else
        dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: at %s: %"PRId64" video frame%s without %s second-view "
               "picture: written without it (the second view's stream misses %s from here on, it is out of step "
               "with the video after this) [untested on real discs]\n",
               hms((char[32]){ 0 }, 32, t->gap_time), t->gap_n, t->gap_n == 1 ? "" : "s",
               t->gap_n == 1 ? "its" : "their", t->gap_n == 1 ? "it" : "them");
    t->t[0].warnings++;
    t->gap_n = 0;
}

static void layer_drop_head(Track *k)
{
    DRFrame *f = &k->lq[k->lq_head];
    if (!k->stray_n++)
        k->stray_src = f->src;
    k->layer_alone++;
    ff_disctl_frame_unref(f);
    k->lq_head++;
    if (!--k->lq_n)
        k->lq_head = 0;
}

static int layer_miss_head(DRTitle *t)
{
    LayerKey *b = &t->bq[t->bq_head];
    int ret = 0;

    /* the base frame goes on without its picture (Dolby Vision: without its
     * metadata, the RPU travels in the enhancement layer) */
    t->layer_missing++;
    if (!t->gap_n++)
        t->gap_time = b->time;
    if (t->layer_merged) {
        DRFrame f = b->f;
        memset(&b->f, 0, sizeof(b->f));
        ret = emit(t, &t->t[0], &f);
    }
    t->bq_head++;
    if (!--t->bq_n)
        t->bq_head = 0;
    return ret;
}

/* A Dolby Vision enhancement picture (its end of sequence units decided as
 * its base frame's were) into its base frame: one frame of the merged
 * track. */
static int layer_merge(DRTitle *t, LayerKey *b, DRFrame *el)
{
    DRFrame f = b->f;
    DrBuf *buf;
    int ret, o;

    memset(&b->f, 0, sizeof(b->f));
    if (!(buf = dr_buf_alloc(f.size + ff_disctl_hevc_wrap_size(el->data, el->size) +
                                DR_BUF_PADDING))) {
        ff_disctl_frame_unref(&f);
        return DR_ERR(ENOMEM);
    }
    memcpy(dr_buf_data(buf), f.data, f.size);
    o = f.size + ff_disctl_hevc_wrap_layer(el->data, el->size, dr_buf_data(buf) + f.size);
    memset(dr_buf_data(buf) + o, 0, DR_BUF_PADDING);
    dr_buf_unref(&f.buf);
    f.buf  = buf;
    f.data = dr_buf_data(buf);
    f.size = o;
    ret = emit(t, &t->t[0], &f);
    return ret;
}

/* Pairs the waiting master frames with the waiting pictures; final: what
 * is left has no partner. */
static int layer_match(DRTitle *t, int final)
{
    Track *k;
    int ret;

    if (t->layer < 0)
        return 0;
    k = &t->t[t->layer];
    while (t->bq_n) {
        const LayerKey *b = &t->bq[t->bq_head];
        int m = -1;
        for (int i = 0; i < k->lq_n && i < LAYER_WINDOW && m < 0; i++)
            if (DR_ABS(k->lq[k->lq_head + i].src - b->src) < LAYER_TOLERANCE)
                m = i;
        if (m < 0) {
            if (!final && k->lq_n < LAYER_WINDOW)
                return 0;                   /* its picture may still come */
            if ((ret = layer_miss_head(t)) < 0)
                return ret;
            continue;
        }
        layer_gap_flush(t);                 /* a gap before this frame ends */
        while (m-- > 0)
            layer_drop_head(k);
        layer_stray_flush(k);
        {
            DRFrame f = k->lq[k->lq_head];
            k->lq_head++;
            if (!--k->lq_n)
                k->lq_head = 0;
            /* HEVC: the picture's end of sequence units go as its base
             * frame's went (taken out there: taken out here too) */
            if (t->layer_hevc && (f.flags & DR_F_SEQ_END) && !(b->flags & DR_F_SEQ_END) &&
                (ret = ff_disctl_hevc_seq_end(&f, NULL)) < 0) {
                ff_disctl_frame_unref(&f);
                return ret;
            }
            f.time  = b->time;
            f.dur   = b->dur;
            f.flags = (f.flags & ~DR_F_KEY) | (b->flags & DR_F_KEY);
            if (t->layer_merged) {
                ret = layer_merge(t, &t->bq[t->bq_head], &f);
                ff_disctl_frame_unref(&f);
            }
            t->bq_head++;
            if (!--t->bq_n)
                t->bq_head = 0;
            if (t->layer_merged) {
                if (ret < 0)
                    return ret;
            } else if ((ret = emit(t, k, &f)) < 0)
                return ret;
        }
    }
    while (final && k->lq_n)
        layer_drop_head(k);
    if (final) {
        layer_gap_flush(t);
        layer_stray_flush(k);
    }
    return 0;
}

/* The next MVC delimiter (00 00 01 + nal_unit_type 24, the start of a
 * dependent-view access unit) at or after i in p, or -1. */
static int layer_next(const uint8_t *p, int n, int i)
{
    for (; i + 3 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 24)
            return i;
    return -1;
}

/* One picture of the second layer: acc[0..n), cleaned as the master
 * video's units are (ff_disctl_h264_clean), into the joiner with the time
 * of the last payload that starts at or before it (and after the picture
 * before it: each time is taken once). */
static int layer_picture(Track *k, int n)
{
    DRFrame f = { .time = DR_NOPTS_VALUE, .pos = k->acc_pos };
    int changed, out, used = 0;

    while (used < k->nb_rec && k->rec[used][0] <= k->acc_pos)
        f.time = k->rec[used++][1];
    memmove(k->rec, k->rec + used, (k->nb_rec - used) * sizeof(*k->rec));
    k->nb_rec -= used;
    if (!(f.buf = dr_buf_alloc(n + DR_BUF_PADDING)))
        return DR_ERR(ENOMEM);
    out = ff_disctl_h264_clean(k->acc, n, dr_buf_data(f.buf), k->layer_dropped, &k->layer_stuffing, &changed);
    if (!changed) {
        memcpy(dr_buf_data(f.buf), k->acc, n);
        out = n;
    }
    memset(dr_buf_data(f.buf) + out, 0, DR_BUF_PADDING);
    f.data = dr_buf_data(f.buf);
    f.size = out;
    memmove(k->acc, k->acc + n, k->acc_len - n);
    k->acc_len -= n;
    k->acc_pos += n;
    k->acc_scan = 4;
    if (!out) {
        ff_disctl_frame_unref(&f);
        return 0;
    }
    return seg_frame(k, &f);
}

/* Bytes of acc up to the delimiter found at d, with the zero bytes in
 * front of it (they go with the delimiter). */
static int layer_cut(const Track *k, int d)
{
    while (d > 0 && !k->acc[d - 1])
        d--;
    return d;
}

/* A second-layer payload: its bytes are cut into pictures at the MVC
 * delimiters; a picture takes the time of the payload it starts in
 * (ISO/IEC 13818-1 2.4.3.7). Bytes before a segment's first delimiter start
 * no picture. */
static int layer_payload(DRTitle *t, Track *k, const uint8_t *data, int size, int64_t time)
{
    int ret, d;

    if (k->acc_len + size > k->acc_cap) {
        int cap = DR_MAX(2 * k->acc_cap, k->acc_len + size);
        uint8_t *a = dr_realloc(k->acc, cap);
        if (!a)
            return DR_ERR(ENOMEM);
        k->acc     = a;
        k->acc_cap = cap;
    }
    if (time != DR_NOPTS_VALUE) {
        if (k->nb_rec == k->rec_cap) {
            int cap = k->rec_cap ? 2 * k->rec_cap : 16;
            int64_t (*r)[2] = dr_realloc_array(k->rec, cap, sizeof(*r));
            if (!r)
                return DR_ERR(ENOMEM);
            k->rec     = r;
            k->rec_cap = cap;
        }
        k->rec[k->nb_rec][0] = k->acc_pos + k->acc_len;
        k->rec[k->nb_rec][1] = time;
        k->nb_rec++;
    }
    memcpy(k->acc + k->acc_len, data, size);
    k->acc_len += size;
    k->bytes   += size;
    if (!k->acc_started) {
        if ((d = layer_next(k->acc, k->acc_len, 0)) < 0)
            return 0;
        if ((d = layer_cut(k, d)) > 0) {
            dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: track %d: %d bytes before the segment's first MVC "
                   "delimiter start no picture: left out [untested on real discs]\n", k->index, d);
            memmove(k->acc, k->acc + d, k->acc_len - d);
            k->acc_len -= d;
            k->acc_pos += d;
        }
        k->acc_started = 1;
        k->acc_scan    = 4;
    }
    /* the search goes on where it stopped (the last 3 bytes again: a start
     * code may span two payloads) */
    while ((d = layer_next(k->acc, k->acc_len, k->acc_scan)) > 0 && (d = layer_cut(k, d)) > 0)
        if ((ret = layer_picture(k, d)) < 0)
            return ret;
    k->acc_scan = DR_MAX(4, k->acc_len - 3);
    return 0;
}

/* An HEVC second layer (a Dolby Vision enhancement layer): its access
 * units as the HEVC rule cuts them (the base layer's
 * rules), each with the time of the payload with its picture, cleaned as
 * the base layer's are, into the joiner. */
static int layer_unit(void *opaque, const DRUnit *u)
{
    Track *k = opaque;
    DRFrame f = { .time = u->time, .pos = u->pos };
    int changed, seq_end, out;

    if (!(f.buf = dr_buf_alloc(u->size + DR_BUF_PADDING)))
        return DR_ERR(ENOMEM);
    out = ff_disctl_hevc_clean(u->data, u->size, dr_buf_data(f.buf), &k->el_dropped, &changed, &seq_end);
    if (!changed) {
        memcpy(dr_buf_data(f.buf), u->data, u->size);
        out = u->size;
    }
    memset(dr_buf_data(f.buf) + out, 0, DR_BUF_PADDING);
    f.data = dr_buf_data(f.buf);
    f.size = out;
    if (seq_end)
        f.flags |= DR_F_SEQ_END;
    if (!out) {
        ff_disctl_frame_unref(&f);
        return 0;
    }
    return seg_frame(k, &f);
}

/* The segment's end: the last picture of the second layer. */
static int layer_flush(Track *k)
{
    int ret = k->acc_started && k->acc_len ? layer_picture(k, k->acc_len) : 0;
    k->acc_len     = 0;
    k->acc_pos     = 0;
    k->acc_started = 0;
    k->acc_scan    = 4;
    k->nb_rec      = 0;
    return ret;
}

/* A frame on the title timeline from the joiner. */
static int joined(void *opaque, int track, DRFrame *f)
{
    DRTitle *t = opaque;
    Track *k = &t->t[track];
    int ret;

    seg_event_dispatch(t, 0);
    switch (k->cfg.kind) {
    case DR_KIND_VIDEO:
        /* the places this picture marks: the pictures before it end there */
        if ((f->flags & DR_F_PLACE) && f->time != DR_NOPTS_VALUE)
            for (int n = 0; n < f->ctr && t->places_timed < t->nb_places; n++)
                t->places[t->places_timed++] = t->vpic_end == INT64_MIN ? f->time : t->vpic_end;
        if (f->time != DR_NOPTS_VALUE && f->size && !(f->flags & DR_F_MARKER))
            t->vpic_end = t->vpic_end == INT64_MIN ? f->time + f->dur : DR_MAX(t->vpic_end, f->time + f->dur);
        if (f->time != DR_NOPTS_VALUE) {
            t->vmax = DR_MAX(t->vmax, f->time);
            t->vend = DR_MAX(t->vend, f->time + f->dur);
        }
        if (k->seq_holding && f->size && !(f->flags & DR_F_MARKER) && (ret = seq_end_out(t, k, f)) < 0) {
            ff_disctl_frame_unref(f);
            return ret;
        }
        if (f->flags & DR_F_SEQ_END) {
            k->seq_held    = *f;
            k->seq_holding = 1;
            memset(f, 0, sizeof(*f));
            return release(t);
        }
        if ((ret = video_out(t, k, f)) < 0)
            return ret;
        return release(t);
    case DR_KIND_LAYER:
        if ((ret = layer_add(k, f)) < 0)
            return ret;
        return layer_match(t, 0);
    case DR_KIND_AUDIO:
        if ((ret = wait_add(k, f)) < 0) {
            ff_disctl_frame_unref(f);
            return ret;
        }
        return release(t);
    case DR_KIND_CAPTION:
        return cc_joined(t, k, f);
    default:
        return spu_joined(t, k, f);
    }
}

/* ---- the title checks of the master video (JoinCheck) ---- */

static uint64_t picture_hash(const uint8_t *p, int n)
{
    uint64_t h = 0xcbf29ce484222325ULL;         /* FNV-1a */
    for (int i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* The segment's first key frame and the pictures shown before it: one event. */
static void join_start_done(DRTitle *t)
{
    JoinCheck *c = &t->jc;

    c->counting = 0;
    c->starts[c->start]++;
    if (c->start != DR_START_CLEAN && c->leading)
        c->start_issues++;
    on_seg_event(t, &(DREvent){ DR_EV_JOIN_START, 0, c->key_time, t->segments, c->start, c->leading });
}

/* The segment's first pictures against the last ones of the segment
 * before: the longest run that ends the one and starts the other, with a key
 * frame in it, is one event. */
static void join_repeat_done(DRTitle *t)
{
    JoinCheck *c = &t->jc;
    int run = 0;

    if (c->rep_done)
        return;
    c->rep_done = 1;
    for (int m = DR_MIN(c->nb_prev, c->nb_first); m > 0 && !run; m--) {
        int same = 1, key = 0;
        for (int i = 0; i < m && same; i++) {
            same = c->prev[c->nb_prev - m + i] == c->first[i];
            key |= c->first_key[i];
        }
        if (same && key)
            run = m;
    }
    if (!run)
        return;
    c->repeats++;
    on_seg_event(t, &(DREvent){ DR_EV_JOIN_REPEAT, 0, c->first_time, 0, t->segments, run });
}

/* A picture of the master video in its segment (decoding order, the
 * segment's clock). */
static void join_frame(DRTitle *t, const DRFrame *f)
{
    JoinCheck *c = &t->jc;
    uint64_t h = picture_hash(f->data, f->size);

    if (!c->frames++ && !(f->flags & DR_F_KEY)) {
        c->not_key++;
        on_seg_event(t, &(DREvent){ DR_EV_JOIN_NOT_KEY, 0, f->time, t->segments, 0, 0 });
    }
    if (c->counting) {
        if (f->time < c->key_time)
            c->leading++;
        else
            join_start_done(t);
    }
    if (!c->have_key && (f->flags & DR_F_KEY)) {
        c->have_key = 1;
        c->key_time = f->time;
        c->start    = f->flags & DR_F_START_CLEAN  ? DR_START_CLEAN  :
                      f->flags & DR_F_START_BROKEN ? DR_START_BROKEN :
                      f->flags & DR_F_START_OPEN   ? DR_START_OPEN   : DR_START_UNKNOWN;
        c->leading  = 0;
        c->counting = 1;
    }
    if (c->nb_first < JOIN_PICTURES) {
        if (!c->nb_first)
            c->first_time = f->time;
        c->first_key[c->nb_first] = !!(f->flags & DR_F_KEY);
        c->first[c->nb_first++]   = h;
        if (c->nb_first == JOIN_PICTURES)
            join_repeat_done(t);
    }
    c->last[c->last_head] = h;
    c->last_head = (c->last_head + 1) % JOIN_PICTURES;
    c->nb_last   = DR_MIN(c->nb_last + 1, JOIN_PICTURES);
}

/* The end of a segment (its video flushed): the start check if still open,
 * the video stage's pictures read and given out, the statistics summed, the
 * segment's last pictures kept for the next join. */
static void join_segment_end(DRTitle *t, const DRVideo *video)
{
    JoinCheck *c = &t->jc;
    DRVideoStats vs;

    if (c->counting)
        join_start_done(t);
    join_repeat_done(t);
    ff_disctl_video_stats(video, &vs);
    if (vs.pictures != vs.out) {
        c->pictures_off++;
        on_seg_event(t, &(DREvent){ DR_EV_PICTURES, 0, 0, t->segments, 0, vs.pictures - vs.out });
    }
    dr_vlog(t->log, DR_VLOG_VERBOSE, "Title check: segment %d: %"PRId64" pictures read, %"PRId64" given out, %"PRId64
           " placeholders; its first key frame: %s, %"PRId64" picture(s) after it shown before it\n",
           t->segments, vs.pictures, vs.out, vs.placeholders, !c->have_key ? "none" :
           c->start == DR_START_CLEAN ? "a clean start" : c->start == DR_START_BROKEN ? "a broken link" :
           c->start == DR_START_OPEN ? "an open start" : "its start is not known for this codec", c->leading);
    c->total.units          += vs.units;
    c->total.pictures       += vs.pictures;
    c->total.out            += vs.out;
    c->total.placeholders   += vs.placeholders;
    c->total.invalid        += vs.invalid;
    c->total.side           += vs.side;
    c->total.side_bytes     += vs.side_bytes;
    c->total.other          += vs.other;
    c->total.other_bytes    += vs.other_bytes;
    c->total.stuffing       += vs.stuffing;
    for (int i = 0; i < DR_ARRAY_ELEMS(vs.removed); i++)
        c->total.removed[i] += vs.removed[i];
    c->total.seq_end        += vs.seq_end;
    c->total.seq_end_inside += vs.seq_end_inside;
    c->total.seq_new        += vs.seq_new;
    c->total.scan_known     += vs.scan_known;
    c->total.progressive    += vs.progressive;
    c->total.tff            += vs.tff;
    c->total.rff            += vs.rff;
    /* the segment's last pictures, oldest first */
    c->nb_prev = c->nb_last;
    for (int i = 0; i < c->nb_last; i++)
        c->prev[i] = c->last[(c->last_head - c->nb_last + i + JOIN_PICTURES) % JOIN_PICTURES];
    c->frames   = 0;
    c->have_key = 0;
    c->key_time = 0;
    c->start    = DR_START_UNKNOWN;
    c->leading  = 0;
    c->nb_first = 0;
    c->rep_done = 0;
    c->nb_last  = 0;
    c->last_head = 0;
}

/* The end of the title: what the checks found, in a few lines. */
static void title_summary(DRTitle *t)
{
    JoinCheck *c = &t->jc;
    const DRVideoStats *v = &c->total;
    char a[32], b[32];

    /* the title checks: a line in the log when one fails (a warning) or
     * needs a look, the passing ones as detail lines */
    dr_vlog(t->log, c->start_issues || c->not_key || c->repeats ? DR_VLOG_INFO : DR_VLOG_VERBOSE,
           "Title check: %d segment(s); their first key frames: %"PRId64" clean, %"PRId64
           " open, %"PRId64" broken link%s; %s\n", t->segments, c->starts[DR_START_CLEAN], c->starts[DR_START_OPEN],
           c->starts[DR_START_BROKEN], c->starts[DR_START_UNKNOWN] ?
           strf(a, sizeof(a), ", %"PRId64" not known", c->starts[DR_START_UNKNOWN]) : "",
           c->start_issues || c->not_key || c->repeats ? "see the warnings above" :
           "no picture at a segment's start needs one before it, none repeated at a join");
    dr_vlog(t->log, c->pictures_off ? DR_VLOG_WARNING : DR_VLOG_VERBOSE, "Title check: video: %"PRId64" pictures read, %"
           PRId64" given out%s\n", v->pictures, v->out, c->pictures_off ? " (see the warnings above)" : "");
    /* what the codec rules took out of the video: one line (in the standard
     * log when anything was), naming only what was */
    {
        static const char *const what[] = { "end(s) of sequence", "end(s) of stream", "filler data unit(s)" };
        char line[512] = "";
        int len = 0;
        for (int i = 0; i < 3; i++)
            if (v->removed[i])
                len += snprintf(line + len, sizeof(line) - DR_MIN(len, sizeof(line)), "%s%"PRId64" %s",
                                len ? ", " : "", v->removed[i], what[i]);
        if (v->stuffing)
            len += snprintf(line + len, sizeof(line) - DR_MIN(len, sizeof(line)), "%s%"PRId64" zero stuffing bytes",
                            len ? ", " : "", v->stuffing);
        if (len)
            len += snprintf(line + len, sizeof(line) - DR_MIN(len, sizeof(line)), " (none of them changes a picture)");
        if (v->side)
            len += snprintf(line + len, sizeof(line) - DR_MIN(len, sizeof(line)), "%s%"PRId64" user data units (%"
                            PRId64" bytes: captions go to the caption track)", len ? ", " : "", v->side, v->side_bytes);
        if (v->other)
            len += snprintf(line + len, sizeof(line) - DR_MIN(len, sizeof(line)), "%s%"PRId64" units that are no "
                            "picture (%"PRId64" bytes)", len ? ", " : "", v->other, v->other_bytes);
        dr_vlog(t->log, len ? DR_VLOG_INFO : DR_VLOG_VERBOSE, "Title check: track 0: taken out of the video: %s\n",
               len ? line : "nothing");
    }
    if (v->scan_known)
        dr_vlog(t->log, DR_VLOG_VERBOSE, "Title check: video scan as the pictures state it (kept as it is): %"PRId64
               " progressive, %"PRId64" interlaced; top field first %"PRId64", repeat first field %"PRId64"%s\n",
               v->progressive, v->scan_known - v->progressive, v->tff, v->rff, v->scan_known < v->out ?
               strf(b, sizeof(b), "; %"PRId64" not read", v->out - v->scan_known) : "");
    else
        dr_vlog(t->log, DR_VLOG_VERBOSE, "Title check: video scan: not read for this codec (kept as it is)\n");
    if (v->seq_end)
        dr_vlog(t->log, DR_VLOG_VERBOSE, "Title check: video: %"PRId64" end of sequence code(s), %"PRId64" with pictures "
               "after them in their segment, %"PRId64" of those followed by a new sequence (kept as they are)\n",
               v->seq_end, v->seq_end_inside, v->seq_new);
    for (int i = 0; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        DRVerifyStats st;
        int64_t issues;

        if (!k->verify)
            continue;
        ff_disctl_verify_stats(k->verify, &st);
        issues = st.bad_units + st.crc_errors + st.order_errors + st.holes + (k->cfg.kind == DR_KIND_VIDEO) *
                 st.overlaps;
        dr_vlog(t->log, issues ? DR_VLOG_WARNING : DR_VLOG_VERBOSE, "Title check: track %d (%s): %"PRId64" frames; %"
               PRId64" units that do not parse, %"PRId64" checksum failures, %"PRId64" order errors, %"PRId64
               " holes%s%s\n", i, ff_disctl_codec_id_name(k->cfg.codec), st.frames - st.markers, st.bad_units,
               st.crc_errors, st.order_errors, st.holes, st.unchecked ? " (units of this codec are not checked)" : "",
               issues ? "" : ": PASS");
        if (k->cfg.kind == DR_KIND_AUDIO && st.frames > st.markers)
            dr_vlog(t->log, st.sync_segments ? DR_VLOG_WARNING : DR_VLOG_VERBOSE, "Title check: track %d: as a stream "
                   "file (start delay %s ms) the audio is %s %s ms of its time on the disc%s%s\n", i,
                   msec(a, 32, st.delay, 1), st.sync_segments ? "off by up to" : "within",
                   msec(b, 32, DR_ABS(st.es_err_max), 0), st.sync_segments ?
                   strf((char[96]){ 0 }, 96, ": %d of %d segment(s) more than 1 ms off, the most in "
                                   "segment %d", st.sync_segments, st.segments, st.sync_seg + 1) :
                   strf((char[64]){ 0 }, 64, " in every segment (%d)", st.segments),
                   st.sync_segments ? "" : ": PASS");
    }
}

/* ---- the current segment ---- */

/* A frame from a track's timing stage, then joined. */
static int seg_frame(void *opaque, DRFrame *f)
{
    Track *k = opaque;

    if (k->nb_posq && f->pos >= k->posq[0]) {
        f->flags |= DR_F_POSITION;
        memmove(k->posq, k->posq + 1, --k->nb_posq * sizeof(*k->posq));
    }
    /* places: the first picture with bytes from their position on */
    if (k->nb_placeq && f->size && !(f->flags & DR_F_MARKER) && f->pos >= k->placeq[0]) {
        int n = 0;
        while (n < k->nb_placeq && f->pos >= k->placeq[n])
            n++;
        f->flags |= DR_F_PLACE;
        f->ctr    = n;
        memmove(k->placeq, k->placeq + n, (k->nb_placeq -= n) * sizeof(*k->placeq));
    }
    if (!k->index && f->size)
        join_frame(k->title, f);
    if (k->audio && !k->have_header) {
        const DRAudioHeader *h = ff_disctl_audio_header(k->audio);
        if (h && h->rate > 0) {
            k->header      = *h;
            k->have_header = 1;
        }
    }
    return ff_disctl_join_push(k->title->join, k->index, f);
}

/* How a track's left-out bytes are told: "Track 1", the part read. */
static void frames_report(DRTitle *t, DRFrames *f, int track)
{
    char label[32], part[32];
    int seg = t->segments - 1;

    snprintf(label, sizeof(label), "Track %d", track);
    if (t->cfg.nb_segments > 1)
        snprintf(part, sizeof(part), "segment %d", seg + 1);
    else
        snprintf(part, sizeof(part), "title");
    ff_disctl_frames_set_report(f, label, part, seg, t->cfg.place, t->cfg.place_opaque);
}

static int seg_open(DRTitle *t)
{
    int ret;

    for (int i = 0; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        DRUnitCb unit;
        void *stage;

        k->bytes     = 0;
        if (k->nb_posq)
            dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: %d chapter position(s) at the end of the segment "
                   "before: the next video frame starts them [untested on real discs]\n", k->nb_posq);
        for (int q = 0; q < k->nb_posq; q++)
            k->posq[q] = 0;
        for (int q = 0; q < k->nb_placeq; q++)  /* places at the end of the segment before: its next picture */
            k->placeq[q] = 0;
        if (k->cfg.kind == DR_KIND_CAPTION || is_side(k))
            continue;                    /* fed by the video's side units / by a clip of its own */
        if (k->cfg.kind == DR_KIND_LAYER && k->cfg.codec == DR_CODEC_HEVC) {
            if ((ret = ff_disctl_frames_open(&k->cut, t->log, DR_CODEC_HEVC, layer_unit, k)) < 0)
                return ret;
            frames_report(t, k->cut, i);
            continue;
        }
        if (k->cfg.kind == DR_KIND_LAYER) {
            k->acc_len     = 0;          /* its pictures go to the joiner as they are cut */
            k->acc_pos     = 0;
            k->acc_started = 0;
            k->acc_scan    = 4;
            k->nb_rec      = 0;
            continue;
        }
        switch (k->cfg.kind) {
        case DR_KIND_VIDEO:
            ret   = ff_disctl_video_open(&k->video, t->log, k->cfg.codec, i, seg_frame, k, on_seg_event, t);
            unit  = ff_disctl_video_unit;
            stage = k->video;
            break;
        case DR_KIND_AUDIO:
            ret   = ff_disctl_audio_open(&k->audio, t->log, k->cfg.codec, k->cfg.audio_flags, seg_frame, k);
            unit  = ff_disctl_audio_unit;
            stage = k->audio;
            break;
        default:
            ret   = ff_disctl_spu_open(&k->spu, t->log, i, t->t[0].video, k->cfg.width, k->cfg.height, seg_frame, k, on_seg_event, t);
            unit  = ff_disctl_spu_unit;
            stage = k->spu;
            if (ret >= 0 && k->cfg.codec == DR_CODEC_HDMV_PGS_SUBTITLE)
                ff_disctl_spu_set_pgs(k->spu, !!(k->cfg.sub_flags & DR_SUB_FORCED_ONLY));
            break;
        }
        if (ret < 0)
            return ret;
        if ((ret = ff_disctl_frames_open(&k->cut, t->log, k->cfg.codec, unit, stage)) < 0)
            return ret;
        frames_report(t, k->cut, i);
        if (k->cfg.codec == DR_CODEC_PCM_DVD || k->cfg.codec == DR_CODEC_PCM_BLURAY) {
            k->lpcm.bd = k->cfg.codec == DR_CODEC_PCM_BLURAY;
            ff_disctl_audio_set_state(k->audio, &k->lpcm);
        }
        if (k->cfg.kind == DR_KIND_AUDIO && (k->cfg.audio_flags & DR_AUDIO_EXTENDED))
            ff_disctl_frames_take_any(k->cut);
        if (k->cfg.kind == DR_KIND_AUDIO && (k->cfg.audio_flags & DR_AUDIO_DVD))
            ff_disctl_frames_dvd(k->cut);
        if (k->cfg.kind == DR_KIND_VIDEO)
            ff_disctl_video_set_side(k->video, cc_side, t);
    }
    return 0;
}

static void seg_close(DRTitle *t)
{
    for (int i = t->cfg.nb_tracks - 1; i >= 0; i--) {
        Track *k = &t->t[i];
        if (is_side(k))
            continue;                    /* its frame step lasts the whole title */
        ff_disctl_frames_close(&k->cut);
        ff_disctl_spu_close(&k->spu);
        ff_disctl_audio_close(&k->audio);
        ff_disctl_video_close(&k->video);
    }
}

/* The end of the segment: the video first (sub-pictures wait for its grid),
 * then the sub-pictures and the audio. */
static int seg_end(DRTitle *t)
{
    Track *v = &t->t[0];
    int ret;

    if ((ret = ff_disctl_frames_flush(v->cut)) < 0 || (ret = ff_disctl_video_flush(v->video)) < 0 ||
        (ret = cc_segment_end(t)) < 0)
        return ret;
    join_segment_end(t, v->video);
    for (int i = 1; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        if (k->cfg.kind == DR_KIND_LAYER &&
            (ret = k->cfg.codec == DR_CODEC_HEVC ? ff_disctl_frames_flush(k->cut) : layer_flush(k)) < 0)
            return ret;
        if (k->cfg.kind == DR_KIND_CAPTION || k->cfg.kind == DR_KIND_LAYER || is_side(k))
            continue;
        if ((ret = ff_disctl_frames_flush(k->cut)) < 0)
            return ret;
        ret = k->spu ? ff_disctl_spu_flush(k->spu) : ff_disctl_audio_flush(k->audio);
        if (ret < 0)
            return ret;
        if (k->spu) {
            DRSpuStats ss;
            ff_disctl_spu_stats(k->spu, &ss);
            k->outside  += ss.outside;
            k->forced_objects += ss.forced_objects;
            k->warnings += ss.outside > 0;  /* the stage warns once per segment */
        }
        if (k->audio) {
            DRAudioStats as;
            ff_disctl_audio_stats(k->audio, &as);
            t->review += as.review;
        }
    }
    seg_close(t);
    /* a segment whose video gave no frame is never placed */
    return seg_event_dispatch(t, 1);
}

/* ---- the title ---- */

int ff_disctl_title_open(DRTitle **tp, void *logctx, const DRTitleConfig *cfg)
{
    DRTitle *t;
    int *kinds = NULL, ret;

    *tp = NULL;
    if (cfg->nb_tracks < 1 || cfg->tracks[0].kind != DR_KIND_VIDEO)
        return DR_ERR(EINVAL);
    if (!(t = dr_mallocz(sizeof(*t))))
        return DR_ERR(ENOMEM);
    t->log  = logctx;
    t->cfg  = *cfg;
    /* the title keeps its own copy of the converter's path */
    t->cfg.cc_helper = NULL;
    if (cfg->cc_helper && !(t->cc_helper = dr_strdup(cfg->cc_helper))) {
        dr_free(t);
        return DR_ERR(ENOMEM);
    }
    t->cfg.cc_helper = t->cc_helper;
    t->vmax = INT64_MIN;
    t->vend = 0;
    t->vpic_end = INT64_MIN;
    t->cc_track = -1;
    t->layer = -1;
    if (!(t->t = dr_calloc(cfg->nb_tracks, sizeof(*t->t))) ||
        !(t->ev_count = dr_calloc(cfg->nb_tracks * EVENT_KINDS, sizeof(*t->ev_count))) ||
        !(kinds = dr_calloc(cfg->nb_tracks, sizeof(*kinds)))) {
        ret = DR_ERR(ENOMEM);
        goto fail;
    }
    for (int i = 0; i < cfg->nb_tracks; i++) {
        Track *k = &t->t[i];
        k->title    = t;
        k->index    = i;
        k->cfg      = cfg->tracks[i];
        k->lpcm.hd  = cfg->lpcm_hd;
        kinds[i]    = k->cfg.kind == DR_KIND_CAPTION ? DR_KIND_SUBTITLE : k->cfg.kind;  /* each keeps its time */
        if (k->cfg.kind == DR_KIND_CAPTION) {
            if (t->cc_track >= 0) {
                ret = DR_ERR(EINVAL);   /* one caption track per title */
                goto fail;
            }
            t->cc_track = i;
        }
        if (k->cfg.kind == DR_KIND_LAYER) {
            if (t->layer >= 0 || (k->cfg.codec != DR_CODEC_H264 && k->cfg.codec != DR_CODEC_HEVC) ||
                ((k->cfg.layer_flags & DR_LAYER_MERGED) && k->cfg.codec != DR_CODEC_HEVC)) {
                ret = DR_ERR(EINVAL);   /* one second layer per title: H.264 (MVC) or HEVC (Dolby Vision) */
                goto fail;
            }
            t->layer        = i;
            t->layer_hevc   = k->cfg.codec == DR_CODEC_HEVC;
            t->layer_merged = !!(k->cfg.layer_flags & DR_LAYER_MERGED);
            continue;
        }
        if ((ret = ff_disctl_verify_open(&k->verify, logctx, k->cfg.codec, i, k->cfg.kind, on_event, t)) < 0)
            goto fail;
    }
    {
        DRJoinConfig jc = {
            .nb_tracks = cfg->nb_tracks, .kinds = kinds, .marks = cfg->marks, .nb_marks = cfg->nb_marks,
            .open_marks = cfg->open_marks,
            .out = joined, .out_opaque = t, .event = on_event, .event_opaque = t,
        };
        if ((ret = ff_disctl_join_open(&t->join, logctx, &jc)) < 0)
            goto fail;
    }
    dr_free(kinds);
    *tp = t;
    return 0;
fail:
    dr_free(kinds);
    ff_disctl_title_close(&t);
    return ret;
}

static int title_segment(DRTitle *t)
{
    int ret;

    if (t->finished)
        return DR_ERR(EINVAL);
    if (t->segments && (ret = seg_end(t)) < 0)
        return ret;
    if ((ret = ff_disctl_join_segment(t->join)) < 0)
        return ret;
    t->segments++;
    return seg_open(t);
}

static int title_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time, int64_t block)
{
    Track *k;
    int ret;

    if (track < 0 || track >= t->cfg.nb_tracks || !t->segments || t->finished)
        return DR_ERR(EINVAL);
    k = &t->t[track];
    if (k->cfg.kind == DR_KIND_CAPTION || is_side(k))
        return DR_ERR(EINVAL);          /* its data comes from the video / a clip of its own */
    if (k->cfg.kind == DR_KIND_LAYER && k->cfg.codec == DR_CODEC_HEVC) {
        k->bytes += size;
        return ff_disctl_frames_write_at(k->cut, data, size, time, block);
    }
    if (k->cfg.kind == DR_KIND_LAYER)
        return layer_payload(t, k, data, size, time);
    if (k->cfg.codec == DR_CODEC_PCM_DVD) {
        int h = t->cfg.lpcm_hd ? 5 : 3;
        if (size < h) {
            dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: track %d: an LPCM payload of %d bytes has no audio frame "
                   "header: left out\n", track, size);
            return 0;
        }
        if ((ret = ff_disctl_lpcm_header(&k->lpcm, t->log, data, h, t->cfg.lpcm_hd)) < 0)
            return ret;
        /* the frame format for the LPCM rule (DVD-Video / HD DVD frames) */
        ff_disctl_frames_set_pcm_format(k->cut, k->lpcm.channels, k->lpcm.bits, k->lpcm.spf);
        data += h;
        size -= h;
    }
    k->bytes += size;
    return ff_disctl_frames_write_at(k->cut, data, size, time, block);
}

/* The second layer's result at the title's end: what was taken out, and
 * an error when video frames have no picture. */
static int layer_report(DRTitle *t)
{
    static const char *const names[32] = { [10] = "ends of sequence", [11] = "ends of stream",
                                           [12] = "filler data units" };
    Track *k = &t->t[t->layer];
    const char *what = t->layer_hevc ? "Dolby Vision enhancement" : "second-view";

    if (t->layer_hevc) {
        ff_disctl_hevc_dropped_log(&k->el_dropped, "Dolby Vision enhancement layer");
    } else {
        for (int n = 0; n < 32; n++) {
            if (!k->layer_dropped[n])
                continue;
            if (names[n])
                dr_vlog(t->log, DR_VLOG_INFO, "Title timeline: track %d: %"PRId64" %s taken out of the second "
                       "layer\n", t->layer, k->layer_dropped[n], names[n]);
            else
                dr_vlog(t->log, DR_VLOG_WARNING, "Title timeline: track %d: %"PRId64" NAL units of type %d (they "
                       "start no unit) taken out of the second layer [untested on real discs]\n", t->layer,
                       k->layer_dropped[n], n);
        }
        if (k->layer_stuffing)
            dr_vlog(t->log, DR_VLOG_INFO, "Title timeline: track %d: %"PRId64" zero stuffing bytes taken out of the "
                   "second layer\n", t->layer, k->layer_stuffing);
    }
    /* each gap and each run of stray pictures was a warning of its own:
     * the totals */
    if (k->layer_alone)
        dr_vlog(t->log, DR_VLOG_INFO, "Title timeline: track %d: %"PRId64" %s picture%s in all without a video frame "
               "of %s time, left out\n", t->layer, k->layer_alone, what, k->layer_alone == 1 ? "" : "s",
               k->layer_alone == 1 ? "its" : "their");
    if (t->layer_missing)
        dr_vlog(t->log, DR_VLOG_INFO, "Title timeline: %"PRId64" video frame%s in all without %s %s picture, written "
               "without it\n", t->layer_missing, t->layer_missing == 1 ? "" : "s",
               t->layer_missing == 1 ? "its" : "their", what);
    return 0;
}

static int title_finish(DRTitle *t)
{
    int ret;

    if (t->finished)
        return 0;
    if (t->segments && (ret = seg_end(t)) < 0)
        return ret;
    if ((ret = ff_disctl_join_finish(t->join)) < 0)
        return ret;
    for (int i = 0; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        if (k->seq_holding && (ret = seq_end_out(t, k, NULL)) < 0)
            return ret;
        if (k->seq_removed)
            dr_vlog(t->log, DR_VLOG_INFO, "Title timeline: track %d: %"PRId64" end of sequence units taken out (at "
                   "the ends of stream files, before an IDR picture or at the title's end: they change nothing "
                   "there)\n", i, k->seq_removed);
    }
    t->video_done = 1;
    if ((ret = release(t)) < 0 || (ret = layer_match(t, 1)) < 0)
        return ret;
    if (t->layer >= 0 && (ret = layer_report(t)) < 0)
        return ret;
    for (int i = 1; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        if (k->pcm && (ret = ff_disctl_pcm_finish(k->pcm)) < 0)
            return ret;
        if (k->junction && (ret = ff_disctl_junction_finish(k->junction)) < 0)
            return ret;
        if (k->junction) {
            DRJunctionStats js;
            int64_t n = 0;
            ff_disctl_junction_stats(k->junction, &js);
            for (int w = 1; w < DR_JOIN_KINDS; w++)
                n += js.joins[w];
            if (n)
                dr_vlog(t->log, DR_VLOG_INFO, "Title timeline: track %d: %"PRId64" join(s) by the stream's sample numbers: "
                       "%"PRId64" continue, %"PRId64" with a repeated unit left out, %"PRId64" with a repeated unit "
                       "kept, %"PRId64" not connected, %"PRId64" without sample numbers\n", i, n,
                       js.joins[DR_JOIN_CONTINUES], js.joins[DR_JOIN_SAME], js.joins[DR_JOIN_SAME_KEPT],
                       js.joins[DR_JOIN_APART], js.joins[DR_JOIN_NO_COUNT]);
        }
        if ((ret = spu_finish(t, k)) < 0)
            return ret;
        if (is_side(k))
            side_finish(t, k);
        if (k->cfg.kind == DR_KIND_CAPTION && (ret = cc_finish(t, k)) < 0)
            return ret;
        if (k->extended)
            dr_vlog(t->log, DR_VLOG_VERBOSE, "Title timeline: track %d: %"PRId64" sub-pictures without a stop time shown "
                   "until the next one starts\n", i, k->extended);
    }
    for (int i = 0; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        if (k->verify && (ret = ff_disctl_verify_finish(k->verify)) < 0)
            return ret;
        dr_vlog(t->log, DR_VLOG_VERBOSE, "Title timeline: track %d (%s): %"PRId64" frames\n", i,
               ff_disctl_codec_id_name(k->cfg.codec), k->out);
        for (int e = 1; e < EVENT_KINDS; e++)
            if (t->ev_count[i * EVENT_KINDS + e] > EVENT_LOGGED)
                dr_vlog(t->log, DR_VLOG_VERBOSE, "Title timeline: track %d: %"PRId64" %s events, the first %d logged\n",
                       i, t->ev_count[i * EVENT_KINDS + e], ff_disctl_event_name(e), EVENT_LOGGED);
    }
    if (t->cc_text) {
        dr_vlog(t->log, DR_VLOG_WARNING, "Captions: the video carries %"PRId64" caption blocks with %"PRId64" caption "
               "byte pairs, and the title has no caption track: taken out of the video and left out\n",
               t->cc_dropped, t->cc_text);
        t->t[0].warnings++;
    } else if (t->cc_dropped) {
        dr_vlog(t->log, DR_VLOG_VERBOSE, "Captions: the video carries %"PRId64" caption blocks without caption text: "
               "taken out of the video\n", t->cc_dropped);
    }
    dr_vlog(t->log, DR_VLOG_DEBUG, "Title timeline: GOP user data taken out of the video: %"PRId64" caption blocks, %"
           PRId64" other\n", t->cc_track >= 0 ? t->t[t->cc_track].cc_blocks : t->cc_dropped, t->user_data);
    title_summary(t);
    t->finished = 1;
    return 0;
}

int ff_disctl_title_frame(DRTitle *t, int *track, DRFrame *f)
{
    if (t->error)
        return t->error;
    if (!t->q_n)
        return t->finished ? DR_ERR_EOF : DR_ERR(EAGAIN);
    *track = t->q[t->q_head].track;
    *f     = t->q[t->q_head].f;
    t->q_head++;
    if (!--t->q_n)
        t->q_head = 0;
    return 0;
}

/* A failure leaves a stage in the middle of its work (a payload half given
 * to a frame step, frames half handed on): the title stays failed, every later
 * call returns the same error instead of going on with broken state. */
static int sticky(DRTitle *t, int ret)
{
    if (ret < 0 && ret != DR_ERR(EAGAIN) && ret != DR_ERR(EINVAL) && !t->error) {
        t->error = ret;
        dr_vlog(t->log, DR_VLOG_ERROR, "Title timeline: the title failed (%s): nothing more is read or given out\n",
               dr_err_text(ret));
    }
    return ret;
}

int ff_disctl_title_segment(DRTitle *t)
{
    return t->error ? t->error : sticky(t, title_segment(t));
}

int ff_disctl_title_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time)
{
    return ff_disctl_title_payload_at(t, track, data, size, time, -1);
}

int ff_disctl_title_payload_at(DRTitle *t, int track, const uint8_t *data, int size, int64_t time, int64_t block)
{
    return t->error ? t->error : sticky(t, title_payload(t, track, data, size, time, block));
}

int ff_disctl_title_chapter_position(DRTitle *t)
{
    Track *k = &t->t[0];

    if (t->error)
        return t->error;
    if (k->nb_posq == k->posq_cap) {
        int cap = k->posq_cap ? 2 * k->posq_cap : 8;
        int64_t *q = dr_realloc_array(k->posq, cap, sizeof(*q));
        if (!q)
            return DR_ERR(ENOMEM);
        k->posq     = q;
        k->posq_cap = cap;
    }
    k->posq[k->nb_posq++] = k->bytes;
    return 0;
}

int ff_disctl_title_place(DRTitle *t)
{
    Track *k = &t->t[0];

    if (t->error)
        return t->error;
    if (k->nb_placeq == k->placeq_cap) {
        int cap = k->placeq_cap ? 2 * k->placeq_cap : 8;
        int64_t *q = dr_realloc_array(k->placeq, cap, sizeof(*q));
        if (!q)
            return DR_ERR(ENOMEM);
        k->placeq     = q;
        k->placeq_cap = cap;
    }
    if (t->nb_places == t->places_cap) {
        int cap = t->places_cap ? 2 * t->places_cap : 8;
        void *p = dr_realloc_array(t->places, cap, sizeof(*t->places));
        if (!p)
            return DR_ERR(ENOMEM);
        t->places     = p;
        t->places_cap = cap;
    }
    k->placeq[k->nb_placeq++] = k->bytes;
    return t->nb_places++;
}

int ff_disctl_title_place_time(const DRTitle *t, int p, int64_t *time)
{
    if (p < 0 || p >= t->places_timed)
        return DR_ERR(EINVAL);
    *time = t->places[p];
    return 0;
}

static int side_open(DRTitle *t, int track, int64_t offset, const char *where)
{
    Track *k;
    int ret;

    if (track < 1 || track >= t->cfg.nb_tracks || !is_side(&t->t[track]) || t->t[track].side_open || t->finished)
        return DR_ERR(EINVAL);
    k = &t->t[track];
    k->side_open   = 1;
    k->side_offset = offset;
    if (!(k->side_where = dr_strdup(where ? where : "its clip")))
        return DR_ERR(ENOMEM);
    if ((ret = ff_disctl_textst_open(&k->textst, t->log, track, !!(k->cfg.sub_flags & DR_SUB_FORCED_ONLY),
                                     k->side_where, side_frame, k)) < 0 ||
        (ret = ff_disctl_frames_open(&k->cut, t->log, k->cfg.codec, ff_disctl_textst_unit, k->textst)) < 0)
        return ret;
    {
        char label[32];
        snprintf(label, sizeof(label), "Track %d", track);
        ff_disctl_frames_set_report(k->cut, label, k->side_where, 0, NULL, NULL);
    }
    return 0;
}

static int side_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time)
{
    Track *k;

    if (track < 1 || track >= t->cfg.nb_tracks || !t->t[track].side_open || t->t[track].side_ended)
        return DR_ERR(EINVAL);
    k = &t->t[track];
    k->bytes += size;
    return ff_disctl_frames_write(k->cut, data, size, time);
}

static int side_end(DRTitle *t, int track)
{
    Track *k;
    int ret;

    if (track < 1 || track >= t->cfg.nb_tracks || !t->t[track].side_open || t->t[track].side_ended)
        return DR_ERR(EINVAL);
    k = &t->t[track];
    k->side_ended = 1;
    if ((ret = ff_disctl_frames_flush(k->cut)) < 0)
        return ret;
    return side_release(t);
}

int ff_disctl_title_side_open(DRTitle *t, int track, int64_t offset, const char *where)
{
    return t->error ? t->error : sticky(t, side_open(t, track, offset, where));
}

int ff_disctl_title_side_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time)
{
    return t->error ? t->error : sticky(t, side_payload(t, track, data, size, time));
}

int ff_disctl_title_side_end(DRTitle *t, int track)
{
    return t->error ? t->error : sticky(t, side_end(t, track));
}

const uint8_t *ff_disctl_title_textst_style(const DRTitle *t, int track, int *size)
{
    *size = 0;
    if (track < 0 || track >= t->cfg.nb_tracks)
        return NULL;
    return ff_disctl_textst_style(t->t[track].textst, size);
}

int ff_disctl_title_finish(DRTitle *t)
{
    return t->error ? t->error : sticky(t, title_finish(t));
}

int ff_disctl_title_chapters(const DRTitle *t, const DRChapterPlan *plan, DRChapter **out, int *nb_out)
{
    const int64_t *starts;
    int n = ff_disctl_join_chapters(t->join, &starts);

    return ff_disctl_chapter_list(plan, starts, n, t->vend, out, nb_out);
}

int64_t ff_disctl_title_review(const DRTitle *t)
{
    return t->review;
}

int ff_disctl_title_track_result(const DRTitle *t, int track, int64_t *frames, int64_t *warnings, int64_t *delay)
{
    DRVerifyStats st;

    if (track < 0 || track >= t->cfg.nb_tracks)
        return DR_ERR(EINVAL);
    if (!t->t[track].verify) {           /* a second layer: its frames are its video frames' */
        *frames   = t->t[track].out;
        *warnings = t->t[track].warnings;
        *delay    = 0;
        return 0;
    }
    ff_disctl_verify_stats(t->t[track].verify, &st);
    *frames   = st.frames - st.markers;
    *warnings = t->t[track].warnings;
    *delay    = st.frames ? st.delay : 0;
    return 0;
}

int ff_disctl_title_cc_result(const DRTitle *t, int track, int64_t *blocks, int64_t *cues, int64_t *lost)
{
    if (track < 0 || track >= t->cfg.nb_tracks || t->t[track].cfg.kind != DR_KIND_CAPTION)
        return DR_ERR(EINVAL);
    *blocks = t->t[track].cc_blocks;
    *cues   = t->t[track].cc_cues;
    *lost   = t->t[track].cc_lost;
    return 0;
}

void ff_disctl_title_cc_dropped(const DRTitle *t, int64_t *blocks, int64_t *text)
{
    *blocks = t->cc_dropped;
    *text   = t->cc_text;
}

int ff_disctl_title_forced_objects(const DRTitle *t, int track, int64_t *forced)
{
    if (track < 0 || track >= t->cfg.nb_tracks || t->t[track].cfg.kind != DR_KIND_SUBTITLE)
        return DR_ERR(EINVAL);
    *forced = t->t[track].forced_objects;
    return 0;
}

int ff_disctl_title_spu_result(const DRTitle *t, int track, int64_t *no_stop, int64_t *outside)
{
    if (track < 0 || track >= t->cfg.nb_tracks || t->t[track].cfg.kind != DR_KIND_SUBTITLE)
        return DR_ERR(EINVAL);
    *no_stop = t->t[track].extended;
    *outside = t->t[track].outside;
    return 0;
}

const DRLpcm *ff_disctl_title_lpcm(const DRTitle *t, int track)
{
    if (track < 0 || track >= t->cfg.nb_tracks ||
        (t->t[track].cfg.codec != DR_CODEC_PCM_DVD && t->t[track].cfg.codec != DR_CODEC_PCM_BLURAY) ||
        !t->t[track].lpcm.init)
        return NULL;
    return &t->t[track].lpcm;
}

int ff_disctl_title_segment_start(const DRTitle *t, int seg, int64_t *start)
{
    return ff_disctl_join_segment_start(t->join, seg, start);
}

int64_t ff_disctl_title_duration(const DRTitle *t)
{
    return t->vend;
}

void ff_disctl_title_close(DRTitle **tp)
{
    DRTitle *t = *tp;

    if (!t)
        return;
    if (t->t) {
        seg_close(t);
        for (int i = 0; i < t->cfg.nb_tracks; i++) {
            Track *k = &t->t[i];
            for (int j = 0; j < k->nb_wait; j++)
                ff_disctl_frame_unref(&k->wait[k->wait_head + j]);
            dr_free(k->wait);
            if (k->holding)
                ff_disctl_frame_unref(&k->held);
            if (k->seq_holding)
                ff_disctl_frame_unref(&k->seq_held);
            ff_disctl_ccx_close(&k->ccx);
            ff_disctl_junction_close(&k->junction);
            ff_disctl_pcm_close(&k->pcm);
            ff_disctl_verify_close(&k->verify);
            for (int j = 0; j < k->lq_n; j++)
                ff_disctl_frame_unref(&k->lq[k->lq_head + j]);
            dr_free(k->lq);
            dr_free(k->acc);
            dr_free(k->rec);
            dr_free(k->posq);
            dr_free(k->placeq);
            if (is_side(k))
                ff_disctl_frames_close(&k->cut);
            ff_disctl_textst_close(&k->textst);
            for (int j = 0; j < k->sq_n; j++)
                ff_disctl_frame_unref(&k->sq[k->sq_head + j]);
            dr_free(k->sq);
            dr_free(k->side_where);
        }
    }
    for (int i = 0; i < t->q_n; i++)
        ff_disctl_frame_unref(&t->q[t->q_head + i].f);
    dr_free(t->q);
    for (int i = 0; i < t->bq_n; i++)
        ff_disctl_frame_unref(&t->bq[t->bq_head + i].f);
    dr_free(t->bq);
    dr_free(t->pend);
    dr_free(t->places);
    dr_free(t->ev_count);
    ff_disctl_join_close(&t->join);
    dr_free(t->t);
    dr_free(t->cc_helper);
    dr_freep(tp);
}
