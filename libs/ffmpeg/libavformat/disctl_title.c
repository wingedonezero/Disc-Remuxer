/*
 * The title timeline: one title through all stages. Per segment each track
 * has a frame step (FFmpeg's parser) and a timing stage (video grid, audio durations, sub-pictures
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

#include <stdio.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "disctl.h"

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
    int64_t       tail_from;     /* stream offset where the payloads from the source's tail begin, -1 */

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
    int64_t       cc_last_ms;    /* captions: the last block's time (ms) */
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
} Track;

/* A master video frame given out while the title has a second layer: the
 * values its layer picture leaves with. */
typedef struct LayerKey {
    int64_t  src, time, dur;
    unsigned flags;
} LayerKey;

#define EVENT_KINDS  64
#define EVENT_LOGGED 50          /* structured event lines per kind and track; the rest are counted */

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
    int           video_done;
    int           finished;
    int64_t       review;        /* untested features met (audio reviews) */
    int           error;         /* the title failed: every later call returns this */
    int           cc_track;      /* the caption track, -1 none */
    char         *cc_helper;     /* the caption converter's path (cfg.cc_helper points here) */
    int64_t       cc_dropped;    /* caption blocks in the video of a title without a caption track */
    int64_t       cc_text;       /* and their entries that carry caption bytes (not the null code) */
    int64_t       user_data;     /* other GOP user data taken out of the video */
    Out          *q;             /* output frames ready */
    int           q_head, q_n, q_cap;
    int           layer;         /* the second layer's track, -1 none */
    LayerKey     *bq;            /* master video frames waiting for their layer picture */
    int           bq_head, bq_n, bq_cap;
    int64_t       layer_missing; /* master video frames without a layer picture */
};

static const char *const event_names[] = {
    [DR_EV_START_GAP]            = "start_gap",
    [DR_EV_START_DROP]           = "start_drop",
    [DR_EV_START_SHIFT]          = "start_shift",
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
    [DR_EV_SEAMLESS_SEARCH]      = "seamless_search",
    [DR_EV_SEAMLESS_DROP]        = "seamless_drop",
    [DR_EV_PCM_SILENCE]          = "pcm_silence",
    [DR_EV_PCM_SKIP]             = "pcm_skip",
    [DR_EV_PCM_TIMECODE]         = "pcm_timecode",
};

const char *ff_disctl_event_name(int kind)
{
    return kind > 0 && kind < FF_ARRAY_ELEMS(event_names) && event_names[kind] ? event_names[kind] : "unknown";
}

/* A time on the title timeline as h:mm:ss.mmm (truncated). */
static const char *hms(char *buf, int size, int64_t t)
{
    int64_t ms = t < 0 ? 0 : t / (DR_TICKS_PER_SECOND / 1000);

    snprintf(buf, size, "%d:%02d:%02d.%03d", (int)(ms / 3600000), (int)(ms / 60000 % 60), (int)(ms / 1000 % 60),
             (int)(ms % 1000));
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
    case DR_EV_START_SHIFT:
        av_log(t->log, AV_LOG_WARNING, "Track %d: the audio starts %s ms before the video: kept, the track "
               "delayed by it\n", k, msec(a, 32, e->dur, 0));
        break;
    case DR_EV_START_DROP:
    case DR_EV_DROP:
        av_log(t->log, AV_LOG_WARNING, "Track %d at %s: %"PRId64" frame(s) (%s ms) dropped to bring the audio skew "
               "to %s ms\n", k, hms(a, 32, e->pos), e->count, msec(b, 32, e->dur, 0), msec(c, 32, e->skew, 1));
        break;
    case DR_EV_OVERLAP:
        av_log(t->log, AV_LOG_WARNING, "Track %d at %s: frames overlap by %s ms; audio skew now %s ms\n", k,
               hms(a, 32, e->pos), msec(b, 32, e->dur, 0), msec(c, 32, e->skew, 1));
        break;
    case DR_EV_GAP_ABSORBED:
        av_log(t->log, AV_LOG_WARNING, "Track %d at %s: short audio gap of %s ms closed; audio skew now %s ms\n", k,
               hms(a, 32, e->pos), msec(b, 32, e->dur, 0), msec(c, 32, e->skew, 1));
        break;
    case DR_EV_GAP:
        av_log(t->log, AV_LOG_WARNING, "Track %d at %s: audio gap of %s ms (%"PRId64".%03d frames missing), "
               "left open\n", k, hms(a, 32, e->pos), msec(b, 32, e->dur, 0), e->count / 1000, (int)(e->count % 1000));
        break;
    case DR_EV_VIDEO_ENDED:
        av_log(t->log, AV_LOG_WARNING, "Track %d at %s: the video ends before this audio: the rest of the track is "
               "left out\n", k, hms(a, 32, e->pos));
        break;
    case DR_EV_VIDEO_TIMECODE:
        av_log(t->log, AV_LOG_WARNING, "Track %d at %s: a picture's disc time is %s ms off the frame grid\n", k,
               hms(a, 32, e->pos), msec(b, 32, e->dur, 1));
        break;
    case DR_EV_VIDEO_INVALID:
        av_log(t->log, AV_LOG_WARNING, "Track %d: %"PRId64" picture(s) with disc times off the frame grid\n", k,
               e->count);
        break;
    case DR_EV_VIDEO_REPAIR:
        av_log(t->log, AV_LOG_WARNING, "Track %d at %s: the frame grid follows a jump of the disc times (%"PRId64
               " placeholder(s))\n", k, hms(a, 32, e->pos), e->count);
        break;
    case DR_EV_PCM_SILENCE:
        av_log(t->log, AV_LOG_WARNING, "Track %d at %s: a gap of %s ms filled with silence\n", k,
               hms(a, 32, e->pos), msec(b, 32, e->dur, 0));
        break;
    case DR_EV_PCM_SKIP:
        av_log(t->log, AV_LOG_WARNING, "Track %d: the audio starts %s ms after the video: skipped on the output "
               "clock\n", k, msec(b, 32, e->dur, 0));
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
        av_log(t->log, AV_LOG_DEBUG, "Title timeline: event %s track=%d pos=%"PRId64" dur=%"PRId64" skew=%"PRId64
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
        DREvent *p = av_realloc_array(t->pend, cap, sizeof(*p));
        if (!p) {
            av_log(t->log, AV_LOG_ERROR, "Title timeline: no memory for an event: given on its segment's clock\n");
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
            Out *q = av_realloc_array(t->q, cap, sizeof(*q));
            if (!q) {
                ff_disctl_frame_unref(f);
                return AVERROR(ENOMEM);
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
        av_log(t->log, AV_LOG_DEBUG, "Title timeline: the junction asks for video at %"PRId64", handed on up to %"
               PRId64"\n", target, t->vmax);
    *ended = t->video_done && t->vmax < target;
    return 0;
}

/* The output stage of an audio track, opened with its first frame (the
 * stream values are known then). */
static int audio_out_open(DRTitle *t, Track *k)
{
    DRVideoRef vref = { t, video_max, video_advance };

    if (k->cfg.codec == AV_CODEC_ID_PCM_DVD || k->cfg.codec == AV_CODEC_ID_PCM_BLURAY) {
        DRPcmConfig pc = {
            .track = k->index, .rate = k->lpcm.rate, .bits = k->lpcm.bits > 16 ? 24 : 16,
            .bytes_per_sample_frame = k->lpcm.spf ? k->lpcm.out_frame_bytes / k->lpcm.spf : 0,
            .video = vref, .out = track_out, .out_opaque = k, .event = on_event, .event_opaque = t,
        };
        if (!k->lpcm.init || !pc.bytes_per_sample_frame)
            return AVERROR_BUG;
        return ff_disctl_pcm_open(&k->pcm, t->log, &pc);
    } else {
        DRJunctionConfig jc = {
            .track = k->index, .tolerance = t->cfg.tolerance, .video = vref,
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

/* Audio frames the video has passed (all of them once it has ended) go on. */
static int release(DRTitle *t)
{
    for (int i = 1; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];

        while (k->nb_wait) {
            DRFrame f = k->wait[k->wait_head];
            int ret;

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
            DRFrame *w = av_realloc_array(k->wait, cap, sizeof(*w));
            if (!w)
                return AVERROR(ENOMEM);
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
        av_log(t->log, AV_LOG_WARNING, "Track %d at %s: a sub-picture without a stop time is followed by one "
               "that does not start later: it keeps its one delay step\n", k->index, hms(b, sizeof(b), h->time));
        k->warnings++;
    }
    return emit(t, k, h);
}

static int spu_joined(DRTitle *t, Track *k, DRFrame *f)
{
    int ret;

    if (k->holding && f->time != AV_NOPTS_VALUE && (ret = spu_held_out(t, k, f->time)) < 0) {
        ff_disctl_frame_unref(f);
        return ret;
    }
    if (!(f->flags & DR_F_NO_STOP) || f->time == AV_NOPTS_VALUE)
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
    av_log(t->log, AV_LOG_WARNING, "Track %d at %s: the last sub-picture has no stop time: shown until the "
           "video's end at %s [untested on real discs]\n", k->index, hms(b, sizeof(b), k->held.time),
           hms(e, sizeof(e), t->vend));
    t->review++;
    if ((ret = spu_held_out(t, k, t->vend)) < 0)
        return ret;
    return 0;
}

/* ---- closed captions ----
 * The MPEG-2 rules take GOP-level user data out of the video as side units,
 * timed on the picture they belong to. A DVD caption block among them goes
 * to the caption track as its CEA-608 entries; on the title timeline each
 * block goes to the converter at its time in milliseconds (truncated). */

static int cc_side(void *opaque, DRFrame *f)
{
    DRTitle *t = opaque;
    uint8_t tri[63 * 3];
    int n = ff_disctl_cc_check(f->data, f->size), m;
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
    if (!m) {
        ff_disctl_frame_unref(f);
        return 0;
    }
    /* the frame keeps its time; its bytes become the entries */
    av_buffer_unref(&f->buf);
    if (!(f->buf = av_buffer_alloc(3 * m + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);
    memcpy(f->buf->data, tri, 3 * m);
    memset(f->buf->data + 3 * m, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    f->data = f->buf->data;
    f->size = 3 * m;
    return ff_disctl_join_push(t->join, k->index, f);
}

static int cc_joined(DRTitle *t, Track *k, DRFrame *f)
{
    int64_t ms = f->time / (DR_TICKS_PER_SECOND / 1000);
    int ret = 0;

    if (!k->ccx) {
        if (!t->cfg.cc_helper || !*t->cfg.cc_helper) {
            av_log(t->log, AV_LOG_ERROR, "Captions: track %d: the converter program (%s) is not known: the "
                   "captions cannot be made into text\n", k->index, DR_CCX_PROGRAM);
            ret = AVERROR(ENOENT);
        } else {
            ret = ff_disctl_ccx_start(&k->ccx, t->log, t->cfg.cc_helper);
        }
    }
    if (ret >= 0)
        ret = ff_disctl_ccx_block(k->ccx, ms, f->data, f->size / 3);
    if (ret >= 0) {
        k->cc_blocks++;
        k->cc_last_ms = ms;
    }
    ff_disctl_frame_unref(f);
    return ret;
}

/* The end of the title: the screen is cleared at the end of the video
 * (erase displayed memory, CEA-608, sent twice as control codes are), so a
 * caption still shown lasts until then; the converter's cues go out. */
static int cc_finish(DRTitle *t, Track *k)
{
    static const uint8_t edm[6] = { 0x04, 0x94, 0x2C, 0x04, 0x94, 0x2C };
    int64_t end_ms = t->vend / (DR_TICKS_PER_SECOND / 1000), tick = DR_TICKS_PER_SECOND / 1000;
    DRCue *cues = NULL;
    int n = 0, ret;

    if (!k->ccx) {
        av_log(t->log, AV_LOG_WARNING, "Captions: track %d: the video carries no caption block: no cues\n",
               k->index);
        k->warnings++;
        return 0;
    }
    if (end_ms >= k->cc_last_ms && (ret = ff_disctl_ccx_block(k->ccx, end_ms, edm, 2)) < 0)
        return ret;
    ret = ff_disctl_ccx_finish(k->ccx, &cues, &n);
    ff_disctl_ccx_close(&k->ccx);
    if (ret < 0)
        return ret;
    for (int i = 0; i < n && ret >= 0; i++) {
        DRFrame f = { .time = cues[i].start_ms * tick, .src = cues[i].start_ms * tick, .flags = DR_F_KEY,
                      .dur = FFMAX(cues[i].end_ms - cues[i].start_ms, 0) * tick, .pos = -1 };
        size_t len = strlen(cues[i].text);

        if (!(f.buf = av_buffer_alloc(len + AV_INPUT_BUFFER_PADDING_SIZE))) {
            ret = AVERROR(ENOMEM);
            break;
        }
        memcpy(f.buf->data, cues[i].text, len);
        memset(f.buf->data + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        f.data = f.buf->data;
        f.size = len;
        ret = emit(t, k, &f);
    }
    k->cc_cues = n;
    ff_disctl_cues_free(&cues, n);
    if (ret >= 0)
        av_log(t->log, AV_LOG_VERBOSE, "Captions: track %d: %"PRId64" caption blocks -> %d cues (CCExtractor %s)\n",
               k->index, k->cc_blocks, n, DR_CCX_VERSION);
    return ret;
}

static int seg_frame(void *opaque, DRFrame *f);

/* ---- a second layer (the dependent view of a 3-D video) ----
 * Its pictures and the master video's frames come in the same (decoding)
 * order; a picture belongs to the master frame with the same time from the
 * disc (within LAYER_TOLERANCE) and leaves with that frame's time, duration
 * and key flag. A picture without a master frame is left out; a master frame
 * without a picture is counted (the layer's stream would miss a picture). */

#define LAYER_TOLERANCE (DR_TICKS_PER_SECOND / 1000)
#define LAYER_WINDOW    64              /* pictures looked at for a master frame's partner */

static int layer_key(DRTitle *t, const DRFrame *f)
{
    if (t->bq_head + t->bq_n == t->bq_cap) {
        if (t->bq_head) {
            memmove(t->bq, t->bq + t->bq_head, t->bq_n * sizeof(*t->bq));
            t->bq_head = 0;
        } else {
            int cap = t->bq_cap ? 2 * t->bq_cap : 64;
            LayerKey *q = av_realloc_array(t->bq, cap, sizeof(*q));
            if (!q)
                return AVERROR(ENOMEM);
            t->bq     = q;
            t->bq_cap = cap;
        }
    }
    t->bq[t->bq_head + t->bq_n++] = (LayerKey){ f->src, f->time, f->dur, f->flags };
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
            DRFrame *q = av_realloc_array(k->lq, cap, sizeof(*q));
            if (!q) {
                ff_disctl_frame_unref(f);
                return AVERROR(ENOMEM);
            }
            k->lq     = q;
            k->lq_cap = cap;
        }
    }
    k->lq[k->lq_head + k->lq_n++] = *f;
    memset(f, 0, sizeof(*f));
    return 0;
}

static void layer_drop_head(Track *k)
{
    DRFrame *f = &k->lq[k->lq_head];
    if (!k->layer_alone++)
        av_log(k->title->log, AV_LOG_WARNING, "Title timeline: track %d: a second-layer picture at %"PRId64" ticks "
               "has no video frame of its time: left out\n", k->index, f->src);
    ff_disctl_frame_unref(f);
    k->lq_head++;
    if (!--k->lq_n)
        k->lq_head = 0;
}

static void layer_miss_head(DRTitle *t)
{
    if (!t->layer_missing++)
        av_log(t->log, AV_LOG_ERROR, "Title timeline: the video frame at %"PRId64" ticks has no second-layer "
               "picture\n", t->bq[t->bq_head].src);
    t->bq_head++;
    if (!--t->bq_n)
        t->bq_head = 0;
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
            if (FFABS(k->lq[k->lq_head + i].src - b->src) < LAYER_TOLERANCE)
                m = i;
        if (m < 0) {
            if (!final && k->lq_n < LAYER_WINDOW)
                return 0;                   /* its picture may still come */
            layer_miss_head(t);
            continue;
        }
        while (m-- > 0)
            layer_drop_head(k);
        {
            DRFrame f = k->lq[k->lq_head];
            k->lq_head++;
            if (!--k->lq_n)
                k->lq_head = 0;
            f.time  = b->time;
            f.dur   = b->dur;
            f.flags = (f.flags & ~DR_F_KEY) | (b->flags & DR_F_KEY);
            t->bq_head++;
            if (!--t->bq_n)
                t->bq_head = 0;
            if ((ret = emit(t, k, &f)) < 0)
                return ret;
        }
    }
    while (final && k->lq_n)
        layer_drop_head(k);
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
    DRFrame f = { .time = AV_NOPTS_VALUE, .pos = k->acc_pos };
    int changed, out, used = 0;

    while (used < k->nb_rec && k->rec[used][0] <= k->acc_pos)
        f.time = k->rec[used++][1];
    memmove(k->rec, k->rec + used, (k->nb_rec - used) * sizeof(*k->rec));
    k->nb_rec -= used;
    if (!(f.buf = av_buffer_alloc(n + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);
    out = ff_disctl_h264_clean(k->acc, n, f.buf->data, k->layer_dropped, &k->layer_stuffing, &changed);
    if (!changed) {
        memcpy(f.buf->data, k->acc, n);
        out = n;
    }
    memset(f.buf->data + out, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    f.data = f.buf->data;
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
        int cap = FFMAX(2 * k->acc_cap, k->acc_len + size);
        uint8_t *a = av_realloc(k->acc, cap);
        if (!a)
            return AVERROR(ENOMEM);
        k->acc     = a;
        k->acc_cap = cap;
    }
    if (time != AV_NOPTS_VALUE) {
        if (k->nb_rec == k->rec_cap) {
            int cap = k->rec_cap ? 2 * k->rec_cap : 16;
            int64_t (*r)[2] = av_realloc_array(k->rec, cap, sizeof(*r));
            if (!r)
                return AVERROR(ENOMEM);
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
            av_log(t->log, AV_LOG_WARNING, "Title timeline: track %d: %d bytes before the segment's first MVC "
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
    k->acc_scan = FFMAX(4, k->acc_len - 3);
    return 0;
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
        if (f->time != AV_NOPTS_VALUE) {
            t->vmax = FFMAX(t->vmax, f->time);
            t->vend = FFMAX(t->vend, f->time + f->dur);
        }
        if (t->layer >= 0 && (f->dur || f->size) && (ret = layer_key(t, f)) < 0) {
            ff_disctl_frame_unref(f);
            return ret;
        }
        if ((ret = emit(t, k, f)) < 0 || (ret = layer_match(t, 0)) < 0)
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

/* ---- the current segment ---- */

/* A frame from a track's timing stage: marked when it was read from the
 * source's tail, then joined. */
static int seg_frame(void *opaque, DRFrame *f)
{
    Track *k = opaque;

    if (k->tail_from >= 0 && f->pos >= k->tail_from)
        f->flags |= DR_F_TAIL;
    if (k->audio && !k->have_header) {
        const DRAudioHeader *h = ff_disctl_audio_header(k->audio);
        if (h && h->rate > 0) {
            k->header      = *h;
            k->have_header = 1;
        }
    }
    return ff_disctl_join_push(k->title->join, k->index, f);
}

static int seg_open(DRTitle *t)
{
    int ret;

    for (int i = 0; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        DRUnitCb unit;
        void *stage;

        k->bytes     = 0;
        k->tail_from = -1;
        if (k->cfg.kind == DR_KIND_CAPTION)
            continue;                    /* fed by the video's side units */
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
            if (ret >= 0 && k->cfg.codec == AV_CODEC_ID_HDMV_PGS_SUBTITLE)
                ff_disctl_spu_set_pgs(k->spu, !!(k->cfg.sub_flags & DR_SUB_FORCED_ONLY));
            break;
        }
        if (ret < 0)
            return ret;
        if ((ret = ff_disctl_frames_open(&k->cut, t->log, k->cfg.codec, unit, stage)) < 0)
            return ret;
        if (k->cfg.codec == AV_CODEC_ID_PCM_DVD || k->cfg.codec == AV_CODEC_ID_PCM_BLURAY) {
            k->lpcm.bd = k->cfg.codec == AV_CODEC_ID_PCM_BLURAY;
            ff_disctl_audio_set_state(k->audio, &k->lpcm);
        }
        if (k->cfg.kind == DR_KIND_AUDIO && (k->cfg.audio_flags & DR_AUDIO_EXTENDED))
            ff_disctl_frames_take_any(k->cut);
        if (k->cfg.kind == DR_KIND_VIDEO)
            ff_disctl_video_set_side(k->video, cc_side, t);
    }
    return 0;
}

static void seg_close(DRTitle *t)
{
    for (int i = t->cfg.nb_tracks - 1; i >= 0; i--) {
        Track *k = &t->t[i];
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

    if ((ret = ff_disctl_frames_flush(v->cut)) < 0 || (ret = ff_disctl_video_flush(v->video)) < 0)
        return ret;
    for (int i = 1; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        if (k->cfg.kind == DR_KIND_LAYER && (ret = layer_flush(k)) < 0)
            return ret;
        if (k->cfg.kind == DR_KIND_CAPTION || k->cfg.kind == DR_KIND_LAYER)
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
        return AVERROR(EINVAL);
    if (!(t = av_mallocz(sizeof(*t))))
        return AVERROR(ENOMEM);
    t->log  = logctx;
    t->cfg  = *cfg;
    /* the title keeps its own copy of the converter's path */
    t->cfg.cc_helper = NULL;
    if (cfg->cc_helper && !(t->cc_helper = av_strdup(cfg->cc_helper))) {
        av_free(t);
        return AVERROR(ENOMEM);
    }
    t->cfg.cc_helper = t->cc_helper;
    t->vmax = INT64_MIN;
    t->vend = 0;
    t->cc_track = -1;
    t->layer = -1;
    if (!(t->t = av_calloc(cfg->nb_tracks, sizeof(*t->t))) ||
        !(t->ev_count = av_calloc(cfg->nb_tracks * EVENT_KINDS, sizeof(*t->ev_count))) ||
        !(kinds = av_calloc(cfg->nb_tracks, sizeof(*kinds)))) {
        ret = AVERROR(ENOMEM);
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
                ret = AVERROR(EINVAL);   /* one caption track per title */
                goto fail;
            }
            t->cc_track = i;
        }
        if (k->cfg.kind == DR_KIND_LAYER) {
            if (t->layer >= 0 || k->cfg.codec != AV_CODEC_ID_H264) {
                ret = AVERROR(EINVAL);   /* one second layer per title, H.264 (MVC) so far */
                goto fail;
            }
            t->layer = i;
            continue;
        }
        if ((ret = ff_disctl_verify_open(&k->verify, logctx, k->cfg.codec, i, k->cfg.kind, on_event, t)) < 0)
            goto fail;
    }
    {
        DRJoinConfig jc = {
            .nb_tracks = cfg->nb_tracks, .kinds = kinds, .marks = cfg->marks, .nb_marks = cfg->nb_marks,
            .out = joined, .out_opaque = t, .event = on_event, .event_opaque = t,
        };
        if ((ret = ff_disctl_join_open(&t->join, logctx, &jc)) < 0)
            goto fail;
    }
    av_free(kinds);
    *tp = t;
    return 0;
fail:
    av_free(kinds);
    ff_disctl_title_close(&t);
    return ret;
}

static int title_segment(DRTitle *t)
{
    int ret;

    if (t->finished)
        return AVERROR(EINVAL);
    if (t->segments && (ret = seg_end(t)) < 0)
        return ret;
    if ((ret = ff_disctl_join_segment(t->join)) < 0)
        return ret;
    t->segments++;
    return seg_open(t);
}

static int title_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time, int tail)
{
    Track *k;
    int ret;

    if (track < 0 || track >= t->cfg.nb_tracks || !t->segments || t->finished)
        return AVERROR(EINVAL);
    k = &t->t[track];
    if (k->cfg.kind == DR_KIND_CAPTION)
        return AVERROR(EINVAL);          /* its data comes from the video */
    if (k->cfg.kind == DR_KIND_LAYER)
        return layer_payload(t, k, data, size, time);
    if (k->cfg.codec == AV_CODEC_ID_PCM_DVD) {
        int h = t->cfg.lpcm_hd ? 5 : 3;
        if (size < h) {
            av_log(t->log, AV_LOG_WARNING, "Title timeline: track %d: an LPCM payload of %d bytes has no audio frame "
                   "header: left out\n", track, size);
            return 0;
        }
        if ((ret = ff_disctl_lpcm_header(&k->lpcm, t->log, data, h, t->cfg.lpcm_hd)) < 0)
            return ret;
        /* the frame format for FFmpeg's LPCM parser (DVD-Video / HD DVD frames) */
        ff_disctl_frames_set_pcm_format(k->cut, k->lpcm.channels, k->lpcm.bits, k->lpcm.spf);
        data += h;
        size -= h;
    }
    if (tail && k->tail_from < 0)
        k->tail_from = k->bytes;
    k->bytes += size;
    return ff_disctl_frames_write(k->cut, data, size, time);
}

/* The second layer's result at the title's end: what was taken out, and
 * an error when video frames have no picture. */
static int layer_report(DRTitle *t)
{
    static const char *const names[32] = { [10] = "ends of sequence", [11] = "ends of stream",
                                           [12] = "filler data units" };
    Track *k = &t->t[t->layer];

    for (int n = 0; n < 32; n++) {
        if (!k->layer_dropped[n])
            continue;
        if (names[n])
            av_log(t->log, AV_LOG_VERBOSE, "Title timeline: track %d: %"PRId64" %s taken out of the second layer\n",
                   t->layer, k->layer_dropped[n], names[n]);
        else
            av_log(t->log, AV_LOG_WARNING, "Title timeline: track %d: %"PRId64" NAL units of type %d (they start no "
                   "unit) taken out of the second layer [untested on real discs]\n", t->layer, k->layer_dropped[n], n);
    }
    if (k->layer_stuffing)
        av_log(t->log, AV_LOG_VERBOSE, "Title timeline: track %d: %"PRId64" zero stuffing bytes taken out\n",
               t->layer, k->layer_stuffing);
    if (k->layer_alone) {
        av_log(t->log, AV_LOG_WARNING, "Title timeline: track %d: %"PRId64" second-layer pictures without a video "
               "frame of their time left out\n", t->layer, k->layer_alone);
        k->warnings++;
    }
    if (t->layer_missing) {
        av_log(t->log, AV_LOG_ERROR, "Title timeline: %"PRId64" video frames have no second-layer picture: the "
               "second layer's stream would miss pictures\n", t->layer_missing);
        return AVERROR_INVALIDDATA;
    }
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
        if ((ret = spu_finish(t, k)) < 0)
            return ret;
        if (k->cfg.kind == DR_KIND_CAPTION && (ret = cc_finish(t, k)) < 0)
            return ret;
        if (k->extended)
            av_log(t->log, AV_LOG_VERBOSE, "Title timeline: track %d: %"PRId64" sub-pictures without a stop time shown "
                   "until the next one starts\n", i, k->extended);
    }
    for (int i = 0; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        if (k->verify && (ret = ff_disctl_verify_finish(k->verify)) < 0)
            return ret;
        av_log(t->log, AV_LOG_VERBOSE, "Title timeline: track %d (%s): %"PRId64" frames\n", i,
               avcodec_get_name(k->cfg.codec), k->out);
        for (int e = 1; e < EVENT_KINDS; e++)
            if (t->ev_count[i * EVENT_KINDS + e] > EVENT_LOGGED)
                av_log(t->log, AV_LOG_VERBOSE, "Title timeline: track %d: %"PRId64" %s events, the first %d logged\n",
                       i, t->ev_count[i * EVENT_KINDS + e], ff_disctl_event_name(e), EVENT_LOGGED);
    }
    if (t->cc_text) {
        av_log(t->log, AV_LOG_WARNING, "Captions: the video carries %"PRId64" caption blocks with %"PRId64" caption "
               "byte pairs, and the title has no caption track: taken out of the video and left out\n",
               t->cc_dropped, t->cc_text);
        t->t[0].warnings++;
    } else if (t->cc_dropped) {
        av_log(t->log, AV_LOG_VERBOSE, "Captions: the video carries %"PRId64" caption blocks without caption text: "
               "taken out of the video\n", t->cc_dropped);
    }
    av_log(t->log, AV_LOG_DEBUG, "Title timeline: GOP user data taken out of the video: %"PRId64" caption blocks, %"
           PRId64" other\n", t->cc_track >= 0 ? t->t[t->cc_track].cc_blocks : t->cc_dropped, t->user_data);
    t->finished = 1;
    return 0;
}

int ff_disctl_title_frame(DRTitle *t, int *track, DRFrame *f)
{
    if (t->error)
        return t->error;
    if (!t->q_n)
        return t->finished ? AVERROR_EOF : AVERROR(EAGAIN);
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
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR(EINVAL) && !t->error) {
        t->error = ret;
        av_log(t->log, AV_LOG_ERROR, "Title timeline: the title failed (%s): nothing more is read or given out\n",
               av_err2str(ret));
    }
    return ret;
}

int ff_disctl_title_segment(DRTitle *t)
{
    return t->error ? t->error : sticky(t, title_segment(t));
}

int ff_disctl_title_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time, int tail)
{
    return t->error ? t->error : sticky(t, title_payload(t, track, data, size, time, tail));
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
        return AVERROR(EINVAL);
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

int ff_disctl_title_cc_result(const DRTitle *t, int track, int64_t *blocks, int64_t *cues)
{
    if (track < 0 || track >= t->cfg.nb_tracks || t->t[track].cfg.kind != DR_KIND_CAPTION)
        return AVERROR(EINVAL);
    *blocks = t->t[track].cc_blocks;
    *cues   = t->t[track].cc_cues;
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
        return AVERROR(EINVAL);
    *forced = t->t[track].forced_objects;
    return 0;
}

int ff_disctl_title_spu_result(const DRTitle *t, int track, int64_t *no_stop, int64_t *outside)
{
    if (track < 0 || track >= t->cfg.nb_tracks || t->t[track].cfg.kind != DR_KIND_SUBTITLE)
        return AVERROR(EINVAL);
    *no_stop = t->t[track].extended;
    *outside = t->t[track].outside;
    return 0;
}

const DRLpcm *ff_disctl_title_lpcm(const DRTitle *t, int track)
{
    if (track < 0 || track >= t->cfg.nb_tracks ||
        (t->t[track].cfg.codec != AV_CODEC_ID_PCM_DVD && t->t[track].cfg.codec != AV_CODEC_ID_PCM_BLURAY) ||
        !t->t[track].lpcm.init)
        return NULL;
    return &t->t[track].lpcm;
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
            av_free(k->wait);
            if (k->holding)
                ff_disctl_frame_unref(&k->held);
            ff_disctl_ccx_close(&k->ccx);
            ff_disctl_junction_close(&k->junction);
            ff_disctl_pcm_close(&k->pcm);
            ff_disctl_verify_close(&k->verify);
            for (int j = 0; j < k->lq_n; j++)
                ff_disctl_frame_unref(&k->lq[k->lq_head + j]);
            av_free(k->lq);
            av_free(k->acc);
            av_free(k->rec);
        }
    }
    for (int i = 0; i < t->q_n; i++)
        ff_disctl_frame_unref(&t->q[t->q_head + i].f);
    av_free(t->q);
    av_free(t->bq);
    av_free(t->pend);
    av_free(t->ev_count);
    ff_disctl_join_close(&t->join);
    av_free(t->t);
    av_free(t->cc_helper);
    av_freep(tp);
}
