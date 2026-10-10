/*
 * The title timeline, stage 4 for audio: the junction of one output track.
 * Title start, overlaps, gaps and drops; every frame leaves at time + skew.
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

#include "libavcodec/defs.h"
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "disctl.h"

#define MS            1080000LL                 /* ticks per millisecond */
#define REPORT        MS                        /* changes above 1 ms are events */
#define START_KEEP    (3 * MS)                  /* a first frame up to 3 ms late is kept as it is */
#define START_REPORT  540000LL                  /* 0.5 ms */
#define GAP_FLOOR     12960000LL                /* 12 ms */
#define MARKER_AHEAD  4320000000LL              /* 4 s */
#define MARKER_STEP   DR_TICKS_PER_SECOND       /* 1 s */
#define TICK          DR_TICKS_PER_PTS          /* one 90 kHz tick: the disc's time resolution */
#define COPY_TAIL     8                         /* frames of the earlier segment kept for the copy check */

enum { HAVE = 1, NONE = 0, WAIT = 2 };

/* The state a step changes; committed only when the step completes. */
typedef struct State {
    int64_t exp;       /* end of the previous input frame */
    int64_t skew;      /* added to every frame that leaves */
    int64_t base;      /* the skew a join decided by sample numbers left (0 at the start): the skew
                          rule drops frames only for skew beyond it */
    int64_t adv;       /* gap marker advance */
    int64_t drop_n, drop_d;
} State;

struct DRJunction {
    void            *log;
    DRJunctionConfig cfg;

    DRFrame         *q;            /* input frames q[head .. head + n) */
    int              head, n, cap;
    int              finished, started, ended;

    State            s;
    int64_t          last_out;     /* time of the last frame handed on */
    int              have_out;     /* a frame was handed on */

    DRFrame          tail[COPY_TAIL]; /* the last frames handed on (the copy check at a join), a ring */
    int              nb_tail, tail_pos;
    int              out_seg;      /* segment of the last frame handed on */
    int              checked_seg;  /* the segment whose join was checked (once per join) */
    int              has_end;      /* end: the sample number just past the last frame handed on */
    int              end;

    DREvent          ev[4];        /* events of the running step */
    int              nb_ev;
    DRJunctionStats  st;
};

static DRFrame *q_get(DRJunction *j, int i)
{
    return &j->q[j->head + i];
}

static void q_drop_head(DRJunction *j)
{
    ff_disctl_frame_unref(q_get(j, 0));
    j->head++;
    j->n--;
}

/* Whether frame k is there: HAVE, NONE (the track has ended) or WAIT. */
static int need(DRJunction *j, int k)
{
    return k < j->n ? HAVE : j->finished ? NONE : WAIT;
}

static void event(DRJunction *j, int kind, int64_t pos, int64_t dur, int64_t skew, int64_t count)
{
    if (j->nb_ev < FF_ARRAY_ELEMS(j->ev))
        j->ev[j->nb_ev++] = (DREvent){ kind, j->cfg.track, pos, dur, skew, count };
}

static void events_flush(DRJunction *j)
{
    for (int i = 0; i < j->nb_ev; i++)
        if (j->cfg.event)
            j->cfg.event(j->cfg.event_opaque, &j->ev[i]);
    j->nb_ev = 0;
}

/* TrueHD / MLP: joins are decided by the streams' own sample counters, not
 * by comparing bytes. */
static int is_mlp(const DRJunction *j)
{
    return j->cfg.codec == AV_CODEC_ID_TRUEHD || j->cfg.codec == AV_CODEC_ID_MLP;
}

/* A reference to a frame's bytes (a copy when it owns none). */
static int frame_ref(DRFrame *dst, const DRFrame *f)
{
    *dst = (DRFrame){ .data = f->data, .size = f->size, .time = f->time, .dur = f->dur, .flags = f->flags,
                      .seg = f->seg };
    if (f->buf)
        dst->buf = av_buffer_ref(f->buf);
    else if ((dst->buf = av_buffer_alloc(f->size + AV_INPUT_BUFFER_PADDING_SIZE))) {
        memcpy(dst->buf->data, f->data, f->size);
        dst->data = dst->buf->data;
    }
    return dst->buf ? 0 : AVERROR(ENOMEM);
}

/* Keeps a frame handed on in the ring of the last COPY_TAIL ones. */
static int tail_add(DRJunction *j, const DRFrame *f)
{
    DRFrame *slot = &j->tail[j->tail_pos];

    ff_disctl_frame_unref(slot);
    j->tail_pos = (j->tail_pos + 1) % COPY_TAIL;
    if (j->nb_tail < COPY_TAIL)
        j->nb_tail++;
    return frame_ref(slot, f);
}

/* The k-th last frame handed on (1 = the last one); NULL when not kept. */
static const DRFrame *tail_get(const DRJunction *j, int k)
{
    if (k < 1 || k > j->nb_tail)
        return NULL;
    return &j->tail[(j->tail_pos - k + COPY_TAIL) % COPY_TAIL];
}

static int same_bytes(const DRFrame *a, const DRFrame *b)
{
    return a && b && a->size == b->size && a->size > 0 && !memcmp(a->data, b->data, a->size);
}

/* Hands a frame on (its time already final). */
static int out(DRJunction *j, DRFrame *f)
{
    int ret;

    if (!is_mlp(j) && f->size && f->data && (ret = tail_add(j, f)) < 0) {
        ff_disctl_frame_unref(f);
        return ret;
    }
    j->out_seg = f->seg;
    j->has_end = !!(f->flags & DR_F_CTR) && j->cfg.rate > 0;
    if (j->has_end)
        j->end = (f->ctr + (int)(j->cfg.frame_dur * j->cfg.rate / DR_TICKS_PER_SECOND)) & 0xFFFF;
    if (j->have_out && f->time <= j->last_out) {
        event(j, DR_EV_TIME_ORDER, f->time, 0, j->s.skew, 0);
        events_flush(j);
    }
    j->last_out = f->time;
    j->have_out = 1;
    j->st.out++;
    return j->cfg.out(j->cfg.out_opaque, f);
}

static int out_head(DRJunction *j, int64_t time)
{
    DRFrame f = *q_get(j, 0);

    f.time = time;
    j->head++;
    j->n--;
    return out(j, &f);
}

static int marker(DRJunction *j, int64_t time)
{
    DRFrame f = { .time = time, .flags = DR_F_KEY | DR_F_MARKER, .pos = -1, .src = time };

    j->st.markers++;
    return j->cfg.out(j->cfg.out_opaque, &f);
}

/* The contiguous frames from q[first] whose summed duration stays within
 * target + frame_dur / 16 (exact start = previous end). NONE when the track
 * ends inside the run (no frames counted). */
static int drop_count(DRJunction *j, int first, int64_t target, int *n, int64_t *d)
{
    uint64_t tol = (uint64_t)target + ((uint64_t)j->cfg.frame_dur >> 4);
    int64_t acc = 0, prev_end = 0;
    int i = 0;

    for (;;) {
        DRFrame *u;
        int r = need(j, first + i);
        uint64_t s;

        if (r == WAIT)
            return WAIT;
        if (r == NONE) {
            *n = 0;
            *d = 0;
            return HAVE;
        }
        u = q_get(j, first + i);
        if (i && u->time != prev_end)
            break;
        s = (uint64_t)acc + (uint64_t)u->dur;
        if (s > (uint64_t)target && s > tol)
            break;
        prev_end = u->time + u->dur;
        acc      = s;
        i++;
    }
    *n = i;
    *d = acc;
    return HAVE;
}

/* ---- title start ----
 * Every frame keeps its own time: a track that starts after the video keeps
 * its delay; one that starts before it keeps the frames before the video,
 * at their times (a stream file's negative delay: the frames before the
 * video are sound from before its first picture; moving them later instead
 * would put the whole track out of sync by the moved amount). */

static int start(DRJunction *j)
{
    int64_t t;
    int r;

    if ((r = need(j, 0)) != HAVE)
        return r == WAIT ? WAIT : HAVE;
    t = q_get(j, 0)->time;
    if (t >= 0 && t <= START_KEEP && t > START_REPORT)
        event(j, DR_EV_START_GAP, 0, t, j->s.skew, 0);
    else if (t < -START_REPORT)
        event(j, DR_EV_START_EARLY, 0, -t, 0, 0);
    j->s.exp = t;
    return HAVE;
}

/* ---- the copy check at a join ----
 * A = the segment before, B = the segment whose first frame is q[0]. When the
 * stamps overlap (by more than one 90 kHz tick) and B's first frame is a byte
 * copy of A's last one (a whole unit: DTS-HD core and extension, E-AC-3 with
 * its dependent frames), the copy is left out: the sound is in the stream
 * once and A's end stays where it is. At most one frame per join. A copy of
 * more than one frame (the overlap covers n > 1 frames and B's first n frames
 * are A's last n in order) is only reported. Equal bytes without an overlap
 * (digital silence on both sides of a join) are no copy. */
static int copy_check(DRJunction *j, State *s)
{
    DRFrame *f = q_get(j, 0);
    int64_t ov = s->exp - f->time, n;
    int r;

    if (ov <= TICK) {
        if (same_bytes(f, tail_get(j, 1)))
            av_log(j->log, AV_LOG_DEBUG, "Title timeline: audio track %d: segment %d starts with the bytes of the "
                   "frame before it, but the stamps do not overlap (%"PRId64" ticks): no copy, kept\n", j->cfg.track,
                   f->seg + 1, ov);
        return NONE;
    }
    if (same_bytes(f, tail_get(j, 1))) {
        av_log(j->log, AV_LOG_DEBUG, "Title timeline: audio track %d: segment %d starts with a copy of the frame the "
               "segment before ends with (stamps overlap by %"PRId64" ticks): the copy is left out\n", j->cfg.track,
               f->seg + 1, ov);
        event(j, DR_EV_AUDIO_COPY, f->time, f->dur, s->skew, ov);
        j->st.dropped++;
        j->st.dropped_dur += f->dur;
        j->st.copies++;
        q_drop_head(j);
        j->s = *s;
        events_flush(j);
        return HAVE;
    }
    n = f->dur > 0 ? (ov - TICK + f->dur - 1) / f->dur : 0;
    if (n > j->nb_tail) {
        av_log(j->log, AV_LOG_DEBUG, "Title timeline: audio track %d: segment %d: the stamps overlap by %"PRId64
               " ticks, more frames than the %d kept: not compared\n", j->cfg.track, f->seg + 1, ov, j->nb_tail);
    } else if (n > 1) {
        int i;
        if ((r = need(j, (int)n - 1)) != HAVE)
            return r;
        for (i = 0; i < n; i++) {
            DRFrame *b = q_get(j, i);
            if (b->seg != f->seg || !same_bytes(b, tail_get(j, (int)n - i)))
                break;
        }
        if (i == n)
            event(j, DR_EV_AUDIO_COPY_LONG, f->time, ov, s->skew, n);
    }
    return NONE;
}

/* ---- TrueHD / MLP at a join: the streams' sample numbers ----
 * Each restart header carries the sample number of its first sample
 * (output timing); the access units after it count on. When the next
 * segment's first unit starts where the earlier segment ends, everything is
 * kept; when its second unit starts there, the first holds the earlier
 * segment's last samples again and is left out (its second unit must be a
 * major sync, so decoding restarts there cleanly). Either way the skew this
 * leaves is the right one (base): the stamps place every segment's first
 * unit on its first picture, so their overlap is not drift. Sample numbers
 * that do not connect, or a side without them: the skew rule decides. */
static int sample_join(DRJunction *j, State *s)
{
    DRFrame *f = q_get(j, 0);
    int spu = (int)(j->cfg.frame_dur * j->cfg.rate / DR_TICKS_PER_SECOND);
    int d, r;

    if (!j->has_end || !(f->flags & DR_F_CTR) || spu <= 0) {
        event(j, DR_EV_SAMPLE_JOIN, f->time, 0, s->skew, DR_JOIN_NO_COUNT);
        j->st.joins[DR_JOIN_NO_COUNT]++;
        return NONE;
    }
    d = ((j->end - f->ctr + 0x8000) & 0xFFFF) - 0x8000;
    if (d == 0) {
        s->skew += s->exp - f->time;
        s->exp   = f->time;
        s->base  = s->skew;
        event(j, DR_EV_SAMPLE_JOIN, f->time, d, s->skew, DR_JOIN_CONTINUES);
        j->st.joins[DR_JOIN_CONTINUES]++;
        return NONE;
    }
    if (d == spu) {
        DRFrame *g;
        if ((r = need(j, 1)) == WAIT)
            return WAIT;
        g = r == HAVE ? q_get(j, 1) : NULL;
        if (!g || !(g->flags & DR_F_SYNC)) {
            event(j, DR_EV_SAMPLE_JOIN, f->time, d, s->skew, DR_JOIN_SAME_KEPT);
            j->st.joins[DR_JOIN_SAME_KEPT]++;
            return NONE;
        }
        s->skew += s->exp - g->time;
        s->exp   = g->time;
        s->base  = s->skew;
        event(j, DR_EV_SAMPLE_JOIN, f->time, d, s->skew, DR_JOIN_SAME);
        j->st.joins[DR_JOIN_SAME]++;
        j->st.dropped++;
        j->st.dropped_dur += f->dur;
        q_drop_head(j);
        j->s = *s;
        events_flush(j);
        return HAVE;
    }
    event(j, DR_EV_SAMPLE_JOIN, f->time, d, s->skew, DR_JOIN_APART);
    j->st.joins[DR_JOIN_APART]++;
    return NONE;
}

/* ---- one input frame ---- */

static int step(DRJunction *j)
{
    State s = j->s;
    DRFrame *f = q_get(j, 0);
    int r, ret;

    j->nb_ev = 0;
    if (f->time == AV_NOPTS_VALUE) {
        av_log(j->log, AV_LOG_ERROR, "Title timeline: audio track %d: a frame without a time reached the junction\n",
               j->cfg.track);
        return AVERROR_BUG;
    }
    if (!f->dur && !f->size) {
        /* an empty marker leaves shifted */
        DRFrame m = *f;
        m.time += s.skew;
        j->head++;
        j->n--;
        j->st.markers++;
        return j->cfg.out(j->cfg.out_opaque, &m);
    }

    /* once per join; its decision (event, state) is final at once, so a
     * step that waits for more frames later does not check it again */
    if (j->have_out && f->seg != j->out_seg && f->seg != j->checked_seg) {
        int seg = f->seg;
        r = is_mlp(j) ? sample_join(j, &s) : copy_check(j, &s);
        if (r == WAIT)
            return WAIT;
        j->checked_seg = seg;
        if (r != NONE)
            return r;
        j->s = s;
        events_flush(j);
        f = q_get(j, 0);
    }

    if (f->time != s.exp) {
        if (f->time < s.exp) {
            int64_t ov = s.exp - f->time;
            s.skew += ov;
            s.exp   = f->time;
            if (ov > REPORT || FFABS(s.skew) > REPORT)
                event(j, DR_EV_OVERLAP, f->time, ov, s.skew, 0);
        } else {
            int64_t gap = f->time - s.exp, ns = s.skew - gap;
            /* a gap of one frame (within one tick) is a missing frame, not absorbed */
            if (ns > -(FFMAX(GAP_FLOOR, f->dur) - TICK)) {
                s.skew = ns;
                s.exp  = f->time;
                if (gap > REPORT || FFABS(ns) > REPORT)
                    event(j, DR_EV_GAP_ABSORBED, f->time, gap, ns, 0);
            } else {
                /* a real gap; first see where the video is */
                DRVideoRef *v = &j->cfg.video;
                int64_t ref = v->max_time(v->opaque), old;
                int reached = 1;
                uint64_t q;

                if (ref < s.exp + s.adv) {
                    int ended = 0;
                    if ((ret = v->advance(v->opaque, s.exp + s.adv, &ended)) < 0)
                        return ret;
                    ref     = v->max_time(v->opaque);
                    reached = !ended;
                }
                if (ref < f->time && reached && f->time - ref > MARKER_AHEAD) {
                    /* the video goes on but is still far before this frame */
                    ret = marker(j, s.adv + s.exp + s.skew);
                    event(j, DR_EV_GAP_MARKER, s.adv + s.exp + s.skew, 0, s.skew, 0);
                    s.adv += MARKER_STEP;
                    j->s = s;
                    events_flush(j);
                    return ret < 0 ? ret : HAVE;
                }
                if (v->max_time(v->opaque) < f->time) {
                    int ended = 0;
                    if ((ret = v->advance(v->opaque, f->time, &ended)) < 0)
                        return ret;
                    if (v->max_time(v->opaque) < f->time) {
                        /* the video ends before this frame */
                        event(j, DR_EV_VIDEO_ENDED, f->time, 0, s.skew, 0);
                        j->s = s;
                        events_flush(j);
                        while (j->n)
                            q_drop_head(j);
                        j->ended = j->st.ended = 1;
                        return HAVE;
                    }
                }
                old    = s.skew;
                s.skew = 0;
                q = j->cfg.frame_dur ? (uint64_t)(gap - old) * 1000 / (uint64_t)j->cfg.frame_dur : 0;
                event(j, DR_EV_GAP, s.exp, gap - old, 0, (uint32_t)q);
            }
        }
    }

    /* append */
    s.adv = 0;
    s.exp = f->time + f->dur;
    if (f->dur + s.base <= s.skew + TICK) {
        int n;
        int64_t d;
        if ((r = drop_count(j, 0, s.skew - s.base, &n, &d)) == WAIT)
            return WAIT;
        if (n) {
            if ((r = need(j, n)) == WAIT)
                return WAIT;
            if (r == HAVE && (q_get(j, n)->flags & DR_F_SYNC)) {
                for (int i = 0; i < n; i++) {
                    DRFrame *u = q_get(j, 0);
                    s.skew   -= u->dur;
                    s.drop_n += 1;
                    s.drop_d += u->dur;
                    s.exp     = u->time + u->dur;
                    j->st.dropped++;
                    j->st.dropped_dur += u->dur;
                    q_drop_head(j);
                }
                j->s = s;
                events_flush(j);
                return HAVE;
            }
        }
    }
    {
        int64_t t = f->time + s.skew;
        if (s.drop_n) {
            event(j, DR_EV_DROP, t, s.drop_d, s.skew, s.drop_n);
            s.drop_n = s.drop_d = 0;
        }
        j->s = s;
        events_flush(j);
        return out_head(j, t);
    }
}

static int run(DRJunction *j)
{
    int r;

    if (j->ended) {
        while (j->n)
            q_drop_head(j);
        return 0;
    }
    if (!j->started) {
        j->nb_ev = 0;
        if ((r = start(j)) == WAIT)
            return 0;
        j->started = 1;
        events_flush(j);
    }
    while (j->n && !j->ended) {
        r = step(j);
        if (r == WAIT)
            break;
        if (r < 0)
            return r;
    }
    if (j->head > 64 && j->head > j->n) {
        memmove(j->q, j->q + j->head, j->n * sizeof(*j->q));
        j->head = 0;
    }
    return 0;
}

int ff_disctl_junction_open(DRJunction **jp, void *logctx, const DRJunctionConfig *cfg)
{
    DRJunction *j = av_mallocz(sizeof(*j));

    *jp = NULL;
    if (!j)
        return AVERROR(ENOMEM);
    j->log      = logctx;
    j->cfg      = *cfg;
    j->checked_seg = -1;
    *jp = j;
    return 0;
}

int ff_disctl_junction_push(DRJunction *j, DRFrame *f)
{
    if (j->ended) {
        ff_disctl_frame_unref(f);
        return 0;
    }
    if (j->head + j->n == j->cap) {
        int cap = j->cap ? 2 * j->cap : 64;
        DRFrame *q = av_realloc_array(j->q, cap, sizeof(*q));
        if (!q)
            return AVERROR(ENOMEM);
        j->q   = q;
        j->cap = cap;
    }
    j->q[j->head + j->n++] = *f;
    memset(f, 0, sizeof(*f));
    j->st.in++;
    return run(j);
}

int ff_disctl_junction_finish(DRJunction *j)
{
    j->finished = 1;
    return run(j);
}

void ff_disctl_junction_stats(const DRJunction *j, DRJunctionStats *st)
{
    *st      = j->st;
    st->skew = j->s.skew;
}

void ff_disctl_junction_close(DRJunction **jp)
{
    DRJunction *j = *jp;

    if (!j)
        return;
    while (j->n)
        q_drop_head(j);
    for (int i = 0; i < COPY_TAIL; i++)
        ff_disctl_frame_unref(&j->tail[i]);
    av_freep(&j->q);
    av_freep(jp);
}
