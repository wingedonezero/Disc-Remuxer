/*
 * The shared rip core: one title through all stages. Per segment each track
 * has a cutter and a timing stage (video grid, audio durations, sub-pictures
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

#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discrip.h"

typedef struct Out {
    int     track;
    DRFrame f;
} Out;

/* One track: its stages in the current segment, and its output side. */
typedef struct Track {
    DRTitle      *title;
    int           index;
    DRTitleTrack  cfg;
    DRLpcm        lpcm;          /* linear PCM: the headers seen (shared by cutter and audio stage) */

    /* the current segment */
    DRCutter     *cut;
    DRVideo      *video;
    DRAudio      *audio;
    DRSpu        *spu;
    int64_t       bytes;         /* payload bytes given to the cutter */
    int64_t       tail_from;     /* stream offset where the payloads from the source's tail begin, -1 */

    /* the output side */
    DRAudioHeader header;        /* audio: the stream values of its first segment that gave them */
    int           have_header;
    DRJunction   *junction;
    DRPcm        *pcm;
    DRVerify     *verify;
    DRFrame      *wait;          /* audio frames waiting for the video */
    int           wait_head, nb_wait, wait_cap;
    int64_t       out;           /* frames given out */
} Track;

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
    Out          *q;             /* output frames ready */
    int           q_head, q_n, q_cap;
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

const char *ff_discrip_event_name(int kind)
{
    return kind > 0 && kind < FF_ARRAY_ELEMS(event_names) && event_names[kind] ? event_names[kind] : "unknown";
}

/* Every event: one structured line at debug level (the first ones of each
 * kind and track), then the caller's callback. */
static void on_event(void *opaque, const DREvent *e)
{
    DRTitle *t = opaque;
    int64_t *n = e->track >= 0 && e->track < t->cfg.nb_tracks && e->kind > 0 && e->kind < EVENT_KINDS ?
                 &t->ev_count[e->track * EVENT_KINDS + e->kind] : NULL;

    if (!n || ++*n <= EVENT_LOGGED)
        av_log(t->log, AV_LOG_DEBUG, "Rip core: event %s track=%d pos=%"PRId64" dur=%"PRId64" skew=%"PRId64
               " count=%"PRId64"\n", ff_discrip_event_name(e->kind), e->track, e->pos, e->dur, e->skew, e->count);
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
    ff_discrip_join_stats(t->join, &js);
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
            av_log(t->log, AV_LOG_ERROR, "Rip core: no memory for an event: given on its segment's clock\n");
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

    if (k->verify && (ret = ff_discrip_verify_frame(k->verify, f)) < 0) {
        ff_discrip_frame_unref(f);
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
                ff_discrip_frame_unref(f);
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
        av_log(t->log, AV_LOG_DEBUG, "Rip core: the junction asks for video at %"PRId64", handed on up to %"
               PRId64"\n", target, t->vmax);
    *ended = t->video_done && t->vmax < target;
    return 0;
}

/* The output stage of an audio track, opened with its first frame (the
 * stream values are known then). */
static int audio_out_open(DRTitle *t, Track *k)
{
    DRVideoRef vref = { t, video_max, video_advance };

    if (k->cfg.codec == AV_CODEC_ID_PCM_DVD) {
        DRPcmConfig pc = {
            .track = k->index, .rate = k->lpcm.rate, .bits = k->lpcm.bits > 16 ? 24 : 16,
            .bytes_per_sample_frame = k->lpcm.spf ? k->lpcm.out_frame_bytes / k->lpcm.spf : 0,
            .video = vref, .out = track_out, .out_opaque = k, .event = on_event, .event_opaque = t,
        };
        if (!k->lpcm.init || !pc.bytes_per_sample_frame)
            return AVERROR_BUG;
        return ff_discrip_pcm_open(&k->pcm, t->log, &pc);
    } else {
        DRJunctionConfig jc = {
            .track = k->index, .tolerance = t->cfg.tolerance, .video = vref,
            .out = track_out, .out_opaque = k, .event = on_event, .event_opaque = t,
            .codec = k->cfg.codec, .rate = k->have_header ? k->header.rate : 0,
        };
        if (k->have_header && k->header.rate > 0)
            jc.frame_dur = (int64_t)((uint64_t)k->header.samples * DR_TICKS_PER_SECOND / (uint64_t)k->header.rate);
        return ff_discrip_junction_open(&k->junction, t->log, &jc);
    }
}

static int audio_push(DRTitle *t, Track *k, DRFrame *f)
{
    int ret;

    if (!k->junction && !k->pcm && (ret = audio_out_open(t, k)) < 0) {
        ff_discrip_frame_unref(f);
        return ret;
    }
    return k->pcm ? ff_discrip_pcm_push(k->pcm, f) : ff_discrip_junction_push(k->junction, f);
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
        if ((ret = emit(t, k, f)) < 0)
            return ret;
        return release(t);
    case DR_KIND_AUDIO:
        if ((ret = wait_add(k, f)) < 0) {
            ff_discrip_frame_unref(f);
            return ret;
        }
        return release(t);
    default:
        return emit(t, k, f);
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
        const DRAudioHeader *h = ff_discrip_audio_header(k->audio);
        if (h && h->rate > 0) {
            k->header      = *h;
            k->have_header = 1;
        }
    }
    return ff_discrip_join_push(k->title->join, k->index, f);
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
        switch (k->cfg.kind) {
        case DR_KIND_VIDEO:
            ret   = ff_discrip_video_open(&k->video, t->log, k->cfg.codec, i, seg_frame, k, on_seg_event, t);
            unit  = ff_discrip_video_unit;
            stage = k->video;
            break;
        case DR_KIND_AUDIO:
            ret   = ff_discrip_audio_open(&k->audio, t->log, k->cfg.codec, k->cfg.audio_flags, seg_frame, k);
            unit  = ff_discrip_audio_unit;
            stage = k->audio;
            break;
        default:
            ret   = ff_discrip_spu_open(&k->spu, t->log, i, t->t[0].video, seg_frame, k, on_seg_event, t);
            unit  = ff_discrip_spu_unit;
            stage = k->spu;
            break;
        }
        if (ret < 0)
            return ret;
        if ((ret = ff_discrip_cutter_open(&k->cut, t->log, k->cfg.codec, unit, stage)) < 0)
            return ret;
        if (k->cfg.codec == AV_CODEC_ID_PCM_DVD) {
            ff_discrip_cutter_set_state(k->cut, &k->lpcm);
            ff_discrip_audio_set_state(k->audio, &k->lpcm);
        }
    }
    return 0;
}

static void seg_close(DRTitle *t)
{
    for (int i = t->cfg.nb_tracks - 1; i >= 0; i--) {
        Track *k = &t->t[i];
        ff_discrip_cutter_close(&k->cut);
        ff_discrip_spu_close(&k->spu);
        ff_discrip_audio_close(&k->audio);
        ff_discrip_video_close(&k->video);
    }
}

/* The end of the segment: the video first (sub-pictures wait for its grid),
 * then the sub-pictures and the audio. */
static int seg_end(DRTitle *t)
{
    Track *v = &t->t[0];
    int ret;

    if ((ret = ff_discrip_cutter_flush(v->cut)) < 0 || (ret = ff_discrip_video_flush(v->video)) < 0)
        return ret;
    for (int i = 1; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        if ((ret = ff_discrip_cutter_flush(k->cut)) < 0)
            return ret;
        ret = k->spu ? ff_discrip_spu_flush(k->spu) : ff_discrip_audio_flush(k->audio);
        if (ret < 0)
            return ret;
    }
    seg_close(t);
    /* a segment whose video gave no frame is never placed */
    return seg_event_dispatch(t, 1);
}

/* ---- the title ---- */

int ff_discrip_title_open(DRTitle **tp, void *logctx, const DRTitleConfig *cfg)
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
    t->vmax = INT64_MIN;
    t->vend = 0;
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
        kinds[i]    = k->cfg.kind;
        if ((ret = ff_discrip_verify_open(&k->verify, logctx, k->cfg.codec, i, k->cfg.kind, on_event, t)) < 0)
            goto fail;
    }
    {
        DRJoinConfig jc = {
            .nb_tracks = cfg->nb_tracks, .kinds = kinds, .marks = cfg->marks, .nb_marks = cfg->nb_marks,
            .out = joined, .out_opaque = t, .event = on_event, .event_opaque = t,
        };
        if ((ret = ff_discrip_join_open(&t->join, logctx, &jc)) < 0)
            goto fail;
    }
    av_free(kinds);
    *tp = t;
    return 0;
fail:
    av_free(kinds);
    ff_discrip_title_close(&t);
    return ret;
}

int ff_discrip_title_segment(DRTitle *t)
{
    int ret;

    if (t->finished)
        return AVERROR(EINVAL);
    if (t->segments && (ret = seg_end(t)) < 0)
        return ret;
    if ((ret = ff_discrip_join_segment(t->join)) < 0)
        return ret;
    t->segments++;
    return seg_open(t);
}

int ff_discrip_title_payload(DRTitle *t, int track, const uint8_t *data, int size, int64_t time, int tail)
{
    Track *k;
    int ret;

    if (track < 0 || track >= t->cfg.nb_tracks || !t->segments || t->finished)
        return AVERROR(EINVAL);
    k = &t->t[track];
    if (k->cfg.codec == AV_CODEC_ID_PCM_DVD) {
        int h = t->cfg.lpcm_hd ? 5 : 3;
        if (size < h) {
            av_log(t->log, AV_LOG_WARNING, "Rip core: track %d: an LPCM payload of %d bytes has no audio frame "
                   "header: left out\n", track, size);
            return 0;
        }
        if ((ret = ff_discrip_lpcm_header(&k->lpcm, t->log, data, h, t->cfg.lpcm_hd)) < 0)
            return ret;
        data += h;
        size -= h;
    }
    if (tail && k->tail_from < 0)
        k->tail_from = k->bytes;
    k->bytes += size;
    return ff_discrip_cutter_write(k->cut, data, size, time);
}

int ff_discrip_title_finish(DRTitle *t)
{
    int ret;

    if (t->finished)
        return 0;
    if (t->segments && (ret = seg_end(t)) < 0)
        return ret;
    if ((ret = ff_discrip_join_finish(t->join)) < 0)
        return ret;
    t->video_done = 1;
    if ((ret = release(t)) < 0)
        return ret;
    for (int i = 1; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        if (k->pcm && (ret = ff_discrip_pcm_finish(k->pcm)) < 0)
            return ret;
        if (k->junction && (ret = ff_discrip_junction_finish(k->junction)) < 0)
            return ret;
    }
    for (int i = 0; i < t->cfg.nb_tracks; i++) {
        Track *k = &t->t[i];
        if ((ret = ff_discrip_verify_finish(k->verify)) < 0)
            return ret;
        av_log(t->log, AV_LOG_VERBOSE, "Rip core: track %d (%s): %"PRId64" frames\n", i,
               avcodec_get_name(k->cfg.codec), k->out);
        for (int e = 1; e < EVENT_KINDS; e++)
            if (t->ev_count[i * EVENT_KINDS + e] > EVENT_LOGGED)
                av_log(t->log, AV_LOG_VERBOSE, "Rip core: track %d: %"PRId64" %s events, the first %d logged\n",
                       i, t->ev_count[i * EVENT_KINDS + e], ff_discrip_event_name(e), EVENT_LOGGED);
    }
    t->finished = 1;
    return 0;
}

int ff_discrip_title_frame(DRTitle *t, int *track, DRFrame *f)
{
    if (!t->q_n)
        return t->finished ? AVERROR_EOF : AVERROR(EAGAIN);
    *track = t->q[t->q_head].track;
    *f     = t->q[t->q_head].f;
    t->q_head++;
    if (!--t->q_n)
        t->q_head = 0;
    return 0;
}

int ff_discrip_title_chapters(const DRTitle *t, const DRChapterPlan *plan, DRChapter **out, int *nb_out)
{
    const int64_t *starts;
    int n = ff_discrip_join_chapters(t->join, &starts);

    return ff_discrip_chapter_list(plan, starts, n, t->vend, out, nb_out);
}

const DRLpcm *ff_discrip_title_lpcm(const DRTitle *t, int track)
{
    if (track < 0 || track >= t->cfg.nb_tracks || t->t[track].cfg.codec != AV_CODEC_ID_PCM_DVD ||
        !t->t[track].lpcm.init)
        return NULL;
    return &t->t[track].lpcm;
}

int64_t ff_discrip_title_duration(const DRTitle *t)
{
    return t->vend;
}

void ff_discrip_title_close(DRTitle **tp)
{
    DRTitle *t = *tp;

    if (!t)
        return;
    if (t->t) {
        seg_close(t);
        for (int i = 0; i < t->cfg.nb_tracks; i++) {
            Track *k = &t->t[i];
            for (int j = 0; j < k->nb_wait; j++)
                ff_discrip_frame_unref(&k->wait[k->wait_head + j]);
            av_free(k->wait);
            ff_discrip_junction_close(&k->junction);
            ff_discrip_pcm_close(&k->pcm);
            ff_discrip_verify_close(&k->verify);
        }
    }
    for (int i = 0; i < t->q_n; i++)
        ff_discrip_frame_unref(&t->q[t->q_head + i].f);
    av_free(t->q);
    av_free(t->pend);
    av_free(t->ev_count);
    ff_discrip_join_close(&t->join);
    av_free(t->t);
    av_freep(tp);
}
