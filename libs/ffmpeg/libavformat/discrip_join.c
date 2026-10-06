/*
 * The shared rip core, stage 3: a title's segments joined on one timeline.
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

#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discrip.h"

#define MARK_AHEAD   432000000LL   /* a chapter mark goes on a video key frame up to 0.4 s before it */
#define SEED_UNITS   1000          /* audio frames searched for the first one with a time */

typedef struct Track {
    int      kind;
    DRFrame *wait;          /* frames of the current segment not placed yet */
    int      nb_wait, cap;
    int      seeded;        /* e is known */
    int64_t  e;             /* expected next time in the current segment */
    int64_t  last_gap;      /* the early-frame value last reported (audio) */
} Track;

struct DRJoin {
    void        *log;
    DRJoinConfig cfg;
    Track       *t;

    int          seg;       /* current segment, -1 before the first */
    int          placed;    /* the current segment has its start and offset */
    int64_t      start, offset;
    int64_t      prev_e0, prev_start, prev_offset;
    int64_t      batch_min; /* earliest video time of the segment's first batch so far */
    int          mark;      /* next chapter mark */
    int64_t     *chap;      /* times of the frames that start chapters */
    int          nb_chap, chap_cap;
    DRJoinStats  st;
};

static int wait_add(Track *t, DRFrame *f)
{
    if (t->nb_wait == t->cap) {
        int cap = t->cap ? 2 * t->cap : 64;
        DRFrame *w = av_realloc_array(t->wait, cap, sizeof(*w));
        if (!w)
            return AVERROR(ENOMEM);
        t->wait = w;
        t->cap  = cap;
    }
    t->wait[t->nb_wait++] = *f;
    memset(f, 0, sizeof(*f));
    return 0;
}

static int give(DRJoin *j, int track, DRFrame *f, int64_t ts, int64_t src)
{
    f->time = ts + j->offset - j->start;
    f->src  = src + j->offset - j->start;
    /* an empty marker takes no chapter */
    if (!track && (f->flags & DR_F_KEY) && (f->dur || f->size) && j->mark < j->cfg.nb_marks) {
        uint64_t m = j->cfg.marks[j->mark], o = f->time;
        if (m <= o || m - o < MARK_AHEAD) {
            if (j->nb_chap == j->chap_cap) {
                int cap = j->chap_cap ? 2 * j->chap_cap : 32;
                int64_t *c = av_realloc_array(j->chap, cap, sizeof(*c));
                if (!c) {
                    ff_discrip_frame_unref(f);
                    return AVERROR(ENOMEM);
                }
                j->chap     = c;
                j->chap_cap = cap;
            }
            j->chap[j->nb_chap++] = f->time;
            f->flags |= DR_F_CHAPTER;
            j->mark++;
            j->st.chapters++;
        }
    }
    j->st.frames++;
    return j->cfg.out(j->cfg.out_opaque, track, f);
}

static void event(DRJoin *j, int kind, int track, int64_t pos, int64_t dur, int64_t skew)
{
    if (j->cfg.event)
        j->cfg.event(j->cfg.event_opaque, &(DREvent){ kind, track, pos, dur, skew, 0 });
}

/* One frame of a placed segment, with its track's expected time known. */
static int place_frame(DRJoin *j, int track, DRFrame *f)
{
    Track *t = &j->t[track];
    int64_t ts = f->time, src = ts;

    if (!f->dur && !f->size)                       /* an empty marker keeps its time */
        return give(j, track, f, ts, ts);
    if (t->kind == DR_KIND_VIDEO) {
        if (ts < 0) {
            av_log(j->log, AV_LOG_ERROR, "Rip core: joiner: a video frame without a time\n");
            return AVERROR_BUG;
        }
        if (!t->seeded || t->e < ts + f->dur)
            t->e = ts + f->dur;                    /* running maximum of the frame ends */
        t->seeded = 1;
        return give(j, track, f, ts, ts);
    }
    if (ts == AV_NOPTS_VALUE) {
        ts = t->e;                                 /* no time of its own: where the track is */
        src = ts;
    } else {
        int64_t gap = t->e - ts;
        if (gap > 0 && gap > f->dur && t->kind == DR_KIND_SUBTITLE) {
            /* a sub-picture replaces the one shown before it: it keeps its
             * time (the reference moves it to the end of the one before) */
            av_log(j->log, AV_LOG_DEBUG, "Rip core: joiner: subtitle track %d: a unit at %"PRId64" ticks starts "
                   "%"PRId64" ticks before the one before it ends: kept at its time\n", track,
                   j->offset + ts - j->start, gap);
            event(j, DR_EV_SUB_OVERLAP, track, j->offset + ts - j->start, gap, 0);
        } else if (gap > 0 && gap > f->dur) {
            /* earlier than expected by more than its own duration */
            if (t->kind == DR_KIND_AUDIO && gap != t->last_gap) {
                event(j, DR_EV_RETIME, track, j->offset + t->e - j->start, f->dur, -gap);
                t->last_gap = gap;
            }
            ts = t->e;
            j->st.retimed++;
        }
    }
    t->e = ts + f->dur;
    return give(j, track, f, ts, src);
}

/* The expected time of an audio / subtitle track at the start of its frames
 * in this segment: audio: the first frame with a time, minus the durations
 * of the frames before it; subtitles: the first frame's time. 0 = not yet. */
static int seed(DRJoin *j, int track, int final)
{
    Track *t = &j->t[track];

    if (t->kind == DR_KIND_SUBTITLE) {
        if (!t->nb_wait)
            return 0;
        if (t->wait[0].time == AV_NOPTS_VALUE || t->wait[0].time < 0) {
            av_log(j->log, AV_LOG_ERROR, "Rip core: joiner: subtitle track %d: its first frame in segment %d "
                   "has no time\n", track, j->seg);
            return AVERROR_INVALIDDATA;
        }
        t->e = t->wait[0].time;
        return 1;
    }
    for (int i = 0; i < t->nb_wait && i < SEED_UNITS; i++) {
        if (t->wait[i].dur <= 0 && t->wait[i].size) {
            av_log(j->log, AV_LOG_ERROR, "Rip core: joiner: audio track %d: a frame without a duration\n", track);
            return AVERROR_INVALIDDATA;
        }
        if (t->wait[i].time != AV_NOPTS_VALUE && t->wait[i].time >= 0) {
            int64_t e = t->wait[i].time;
            for (int k = i - 1; k >= 0; k--)
                e -= t->wait[k].dur;
            t->e = e;
            return 1;
        }
    }
    if (final || t->nb_wait >= SEED_UNITS) {
        av_log(j->log, AV_LOG_ERROR, "Rip core: joiner: audio track %d: none of its first %d frames in segment %d "
               "has a time\n", track, FFMIN(t->nb_wait, SEED_UNITS), j->seg);
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

/* Places the waiting frames of a track once its expected time is known. */
static int drain(DRJoin *j, int track, int final)
{
    Track *t = &j->t[track];
    int ret;

    if (!j->placed || !t->nb_wait)
        return 0;
    if (!t->seeded && t->kind != DR_KIND_VIDEO) {
        if ((ret = seed(j, track, final)) <= 0)
            return ret;
        t->seeded = 1;
    }
    for (int i = 0; i < t->nb_wait; i++)
        if ((ret = place_frame(j, track, &t->wait[i])) < 0) {
            for (i++; i < t->nb_wait; i++)
                ff_discrip_frame_unref(&t->wait[i]);
            t->nb_wait = 0;
            return ret;
        }
    t->nb_wait = 0;
    return 0;
}

/* The segment's start (its first video time) and place on the timeline. */
static int place_segment(DRJoin *j)
{
    if (j->batch_min == AV_NOPTS_VALUE) {
        av_log(j->log, AV_LOG_ERROR, "Rip core: joiner: segment %d has no video frame with a time\n", j->seg);
        return AVERROR_INVALIDDATA;
    }
    j->start  = j->batch_min;
    j->offset = j->seg ? j->prev_e0 - j->prev_start + j->prev_offset : 0;
    j->placed = 1;
    j->st.start  = j->start;
    j->st.offset = j->offset;
    av_log(j->log, AV_LOG_DEBUG, "Rip core: joiner: segment %d starts at %"PRId64" ticks, placed at %"PRId64"\n",
           j->seg, j->start, j->offset);
    for (int k = 0; k < j->cfg.nb_tracks; k++) {
        int ret = drain(j, k, 0);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int end_segment(DRJoin *j)
{
    int ret;

    if (j->seg < 0)
        return 0;
    if (!j->placed && (ret = place_segment(j)) < 0)
        return ret;
    for (int k = 0; k < j->cfg.nb_tracks; k++)
        if ((ret = drain(j, k, 1)) < 0)
            return ret;
    j->prev_e0     = j->t[0].e;
    j->prev_start  = j->start;
    j->prev_offset = j->offset;
    return 0;
}

int ff_discrip_join_segment(DRJoin *j)
{
    int ret = end_segment(j);

    if (ret < 0)
        return ret;
    j->seg++;
    j->placed    = 0;
    j->batch_min = AV_NOPTS_VALUE;
    for (int k = 0; k < j->cfg.nb_tracks; k++)
        j->t[k].seeded = 0;
    j->st.segments = j->seg + 1;
    return 0;
}

int ff_discrip_join_push(DRJoin *j, int track, DRFrame *f)
{
    Track *t;
    int ret;

    if (track < 0 || track >= j->cfg.nb_tracks || j->seg < 0) {
        ff_discrip_frame_unref(f);
        return AVERROR(EINVAL);
    }
    t = &j->t[track];
    if (j->placed && (t->seeded || t->kind == DR_KIND_VIDEO) && !t->nb_wait)
        return place_frame(j, track, f);

    if (!track && !j->placed && f->time != AV_NOPTS_VALUE && f->time >= 0 && (f->dur || f->size))
        j->batch_min = j->batch_min == AV_NOPTS_VALUE ? f->time : FFMIN(j->batch_min, f->time);
    {
        int batch_end = !track && (f->flags & DR_F_BATCH);
        if ((ret = wait_add(t, f)) < 0)
            return ret;
        if (batch_end && !j->placed)
            return place_segment(j);
    }
    return drain(j, track, 0);
}

int ff_discrip_join_finish(DRJoin *j)
{
    return end_segment(j);
}

int ff_discrip_join_open(DRJoin **jp, void *logctx, const DRJoinConfig *cfg)
{
    DRJoin *j;

    *jp = NULL;
    if (cfg->nb_tracks < 1 || cfg->kinds[0] != DR_KIND_VIDEO)
        return AVERROR(EINVAL);
    if (!(j = av_mallocz(sizeof(*j))) || !(j->t = av_calloc(cfg->nb_tracks, sizeof(*j->t)))) {
        av_free(j);
        return AVERROR(ENOMEM);
    }
    j->log = logctx;
    j->cfg = *cfg;
    j->seg = -1;
    for (int k = 0; k < cfg->nb_tracks; k++) {
        j->t[k].kind     = cfg->kinds[k];
        j->t[k].last_gap = INT64_MIN;
    }
    *jp = j;
    return 0;
}

void ff_discrip_join_stats(const DRJoin *j, DRJoinStats *st)
{
    *st = j->st;
}

int ff_discrip_join_chapters(const DRJoin *j, const int64_t **starts)
{
    *starts = j->chap;
    return j->nb_chap;
}

void ff_discrip_join_close(DRJoin **jp)
{
    DRJoin *j = *jp;

    if (!j)
        return;
    for (int k = 0; k < j->cfg.nb_tracks; k++) {
        for (int i = 0; i < j->t[k].nb_wait; i++)
            ff_discrip_frame_unref(&j->t[k].wait[i]);
        av_freep(&j->t[k].wait);
    }
    av_freep(&j->t);
    av_freep(&j->chap);
    av_freep(jp);
}
