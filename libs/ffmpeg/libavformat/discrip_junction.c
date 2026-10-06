/*
 * The shared rip core, stage 4 for audio: the junction of one output track.
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

#include "discrip.h"

#define MS            1080000LL                 /* ticks per millisecond */
#define REPORT        MS                        /* changes above 1 ms are events */
#define START_KEEP    (3 * MS)                  /* a first frame up to 3 ms late is kept as it is */
#define START_REPORT  540000LL                  /* 0.5 ms */
#define GAP_FLOOR     12960000LL                /* 12 ms */
#define MARKER_AHEAD  4320000000LL              /* 4 s */
#define MARKER_STEP   DR_TICKS_PER_SECOND       /* 1 s */
#define TAIL_LOOK     540000000LL               /* 0.5 s */
#define TAIL_UNITS    99
#define MATCH         0xF3333333U              /* a correlation above 0.95 (Q32) */

enum { HAVE = 1, NONE = 0, WAIT = 2 };

/* The state a step changes; committed only when the step completes. */
typedef struct State {
    int64_t exp;       /* end of the previous input frame */
    int64_t skew;      /* added to every frame that leaves */
    int64_t base;      /* the start shift (lead-in kept) */
    int64_t adv;       /* gap marker advance */
    int64_t drop_n, drop_d;
} State;

struct DRJunction {
    void            *log;
    DRJunctionConfig cfg;
    int64_t          c;            /* lead-in tolerance, at least 1 ms */

    DRFrame         *q;            /* input frames q[head .. head + n) */
    int              head, n, cap;
    int              finished, started, ended;
    int64_t          tail_done;    /* input index of the frame the tail check last ran for */

    State            s;
    int64_t          last_out;     /* time of the last frame handed on, -1 = none */

    DRFrame         *hist;         /* frames handed on since the last sync unit (decoder preroll) */
    int              nb_hist, cap_hist;

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
    ff_discrip_frame_unref(q_get(j, 0));
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

static int decodable(const DRJunction *j)
{
    return (j->cfg.codec == AV_CODEC_ID_TRUEHD || j->cfg.codec == AV_CODEC_ID_MLP) && j->cfg.rate > 0;
}

static void hist_clear(DRJunction *j)
{
    for (int i = 0; i < j->nb_hist; i++)
        ff_discrip_frame_unref(&j->hist[i]);
    j->nb_hist = 0;
}

/* Keeps a frame handed on: the frames since the last sync unit are the
 * decoder's preroll for the next search. */
static int hist_add(DRJunction *j, const DRFrame *f)
{
    DRFrame h = { .data = f->data, .size = f->size, .dur = f->dur, .flags = f->flags };

    if (f->flags & DR_F_SYNC)
        hist_clear(j);
    if (j->nb_hist == j->cap_hist) {
        int cap = j->cap_hist ? 2 * j->cap_hist : 64;
        DRFrame *p = av_realloc_array(j->hist, cap, sizeof(*p));
        if (!p)
            return AVERROR(ENOMEM);
        j->hist     = p;
        j->cap_hist = cap;
    }
    if (f->buf)
        h.buf = av_buffer_ref(f->buf);
    else if ((h.buf = av_buffer_alloc(f->size + AV_INPUT_BUFFER_PADDING_SIZE))) {
        memcpy(h.buf->data, f->data, f->size);
        h.data = h.buf->data;
    }
    if (!h.buf)
        return AVERROR(ENOMEM);
    j->hist[j->nb_hist++] = h;
    return 0;
}

/* Hands a frame on (its time already final). */
static int out(DRJunction *j, DRFrame *f)
{
    int ret;

    if (decodable(j) && (ret = hist_add(j, f)) < 0) {
        ff_discrip_frame_unref(f);
        return ret;
    }
    if (j->last_out >= 0 && f->time <= j->last_out) {
        event(j, DR_EV_TIME_ORDER, f->time, 0, j->s.skew, 0);
        events_flush(j);
    }
    j->last_out = f->time;
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

/* ---- title start ---- */

static int start(DRJunction *j)
{
    int64_t t, a, dropped = 0;
    int ndrop = 0, r;

    if ((r = need(j, 0)) != HAVE)
        return r == WAIT ? WAIT : HAVE;
    t = q_get(j, 0)->time;
    if (t >= 0) {
        /* the track starts after the video: its delay is kept */
        if (t <= START_KEEP && t > START_REPORT)
            event(j, DR_EV_START_GAP, 0, t, j->s.skew, 0);
        j->s.exp = t;
        return HAVE;
    }
    a = -t;
    if ((uint64_t)a > (uint64_t)j->c) {
        /* frames earlier than the tolerance + one frame are dropped */
        int64_t limit = -(j->c + j->cfg.frame_dur);
        int k = 0;

        for (;;) {
            r = need(j, k);
            if (r == WAIT)
                return WAIT;
            if (r == NONE || q_get(j, k)->time >= limit)
                break;
            dropped += q_get(j, k)->dur;
            k++;
        }
        for (int i = 0; i < k; i++)
            q_drop_head(j);
        ndrop = k;
        j->st.dropped     += k;
        j->st.dropped_dur += dropped;
        if (!j->n)
            return HAVE;    /* nothing left: no event (the reference reports only with a frame left) */
        t = q_get(j, 0)->time;
        if (FFABS(t) > START_KEEP)
            event(j, DR_EV_START_DROP, 0, dropped, -t, ndrop);
        if (t >= 0) {
            events_flush(j);
            return start(j);
        }
        a = -t;
    }
    j->s.skew = j->s.base = a;
    j->s.exp  = t;
    if (dropped + a > START_REPORT)
        event(j, DR_EV_START_SHIFT, 0, dropped + a, a, 0);
    return HAVE;
}

/* ---- the seamless overlap search at the tail of a segment ----
 * The next segment's first sync unit U (at idx) starts inside the run of
 * frames that ends at end. Candidate counts of frames duplicated at the end
 * of side A (the frames before U) are tried around the overlap / the frame
 * duration (+-5, at least 1, at most idx). TrueHD / MLP: both sides are
 * decoded (A after the frames since the last sync unit as preroll, B = U and
 * the idx + 1 frames after it), mixed down to one channel, and for each
 * candidate the end of A from where those frames start is compared with the
 * start of B; the last candidate above 0.95 is the match. k = the smallest
 * candidate whose frames fit the overlap (within frame / 16). A match close
 * to k (below k + 2) drops its frames: side A's frames before them go on,
 * the skew becomes where they ended against U. Otherwise: k = 0, side A goes
 * on unchanged; else the k frames are re-timed to end at U and the normal
 * path follows. */
static int junction(DRJunction *j, State *s, int idx, int64_t end)
{
    DRFrame *u = q_get(j, idx);
    uint64_t over = (uint64_t)(end + s->skew - (u->time + s->base));
    uint64_t nom  = j->cfg.frame_dur ? over / (uint64_t)j->cfg.frame_dur : 0;
    uint64_t lo   = nom > 6 ? nom - 5 : 1, bestd = UINT64_MAX;
    int k = 0, best = 0, top_c = 0, na = 0, nb = 0, ret;
    uint32_t best_r = 0, top_r = 0;
    int32_t *a = NULL, *b = NULL;

    if (nom + 5 >= lo) {
        if (decodable(j)) {
            int r = need(j, 2 * idx + 1), nB;
            if (r == WAIT)
                return WAIT;
            nB = FFMIN(2 * idx + 2, j->n) - idx;
            ret = ff_discrip_seamless_decode(j->log, j->cfg.codec, j->cfg.rate, j->hist, j->nb_hist,
                                             q_get(j, 0), idx, &a, &na);
            if (ret > 0)
                ret = ff_discrip_seamless_decode(j->log, j->cfg.codec, j->cfg.rate, NULL, 0, u, nB, &b, &nb);
            if (ret < 0) {
                av_free(a);
                return ret;
            }
            if (!ret) {
                av_freep(&a);
                na = nb = 0;
            }
        }
        for (uint64_t cand = lo; cand <= nom + 5 && cand <= (uint64_t)idx; cand++) {
            int n;
            int64_t d;
            if (drop_count(j, idx - (int)cand, (int64_t)over, &n, &d) == WAIT) {
                av_free(a);
                av_free(b);
                return WAIT;
            }
            if (na) {
                uint32_t off = 0, len, r;
                for (int m = 0; m < idx - (int)cand; m++)
                    off += (uint32_t)((int64_t)j->cfg.rate * q_get(j, m)->dur / DR_TICKS_PER_SECOND);
                len = (uint32_t)na - off;
                if ((uint32_t)nb <= len)
                    len = nb;
                r = len ? ff_discrip_seamless_corr(a + off, b, len) : 0;
                if (r > top_r) {
                    top_r = r;
                    top_c = (int)cand;
                }
                if (r >= MATCH) {
                    best   = (int)cand;
                    best_r = r;
                }
            }
            if ((uint64_t)n == cand && (uint64_t)d < bestd) {
                bestd = d;
                k     = (int)cand;
            }
        }
        av_freep(&a);
        av_freep(&b);
    }
    event(j, DR_EV_SEAMLESS_SEARCH, u->time, top_c, best_r && best < k + 2, top_r);
    if (best_r && best < k + 2) {
        int64_t dropped = 0, old;

        av_log(j->log, AV_LOG_DEBUG, "Rip core: audio track %d: seamless audio overlap frame_count=%d "
               "covariance=0.%06u\n", j->cfg.track, best, (unsigned)((uint64_t)best_r * 1000000 >> 32));
        j->s = *s;
        events_flush(j);
        for (int i = 0; i < idx - best; i++) {
            DRFrame *f = q_get(j, 0);
            j->s.exp = f->time + f->dur;
            if ((ret = out_head(j, f->time + j->s.skew)) < 0)
                return ret;
        }
        for (int i = 0; i < best; i++) {
            dropped += q_get(j, 0)->dur;
            j->st.dropped++;
            j->st.dropped_dur += q_get(j, 0)->dur;
            q_drop_head(j);
        }
        old        = j->s.exp;
        u          = q_get(j, 0);
        j->s.exp   = u->time;
        j->s.skew += old - u->time;
        j->s.base  = j->s.skew;
        event(j, DR_EV_SEAMLESS_DROP, u->time + j->s.skew, dropped, j->s.skew, best);
        events_flush(j);
        return HAVE;
    }
    if (!k) {
        /* side A unchanged, frame by frame */
        av_log(j->log, AV_LOG_DEBUG, "Rip core: audio track %d: no overlap count fits (estimate %"PRIu64
               " frames): the earlier segment goes on unchanged\n", j->cfg.track, nom);
        j->s = *s;
        events_flush(j);
        for (int i = 0; i < idx; i++) {
            DRFrame *f = q_get(j, 0);
            j->s.exp = f->time + f->dur;
            if ((ret = out_head(j, f->time + j->s.skew)) < 0)
                return ret;
        }
        return HAVE;
    }
    events_flush(j);
    for (int m = idx - 1; m >= idx - k; m--)
        q_get(j, m)->time = q_get(j, m + 1)->time - q_get(j, m)->dur;
    return -1;   /* go on with the normal path */
}

/* ---- one input frame ---- */

static int step(DRJunction *j)
{
    State s = j->s;
    DRFrame *f = q_get(j, 0);
    int r, ret;

    j->nb_ev = 0;
    if (f->time == AV_NOPTS_VALUE) {
        av_log(j->log, AV_LOG_ERROR, "Rip core: audio track %d: a frame without a time reached the junction\n",
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

    if (f->time == s.exp && (f->flags & (DR_F_TAIL | DR_F_SYNC)) == DR_F_TAIL && j->cfg.frame_dur &&
        j->tail_done != j->st.in - j->n) {
        int64_t end = f->time + f->dur;
        for (int i = 1; i <= TAIL_UNITS; i++) {
            DRFrame *u;
            if (end - f->time > TAIL_LOOK)
                break;
            if ((r = need(j, i)) == WAIT)
                return WAIT;
            if (r == NONE)
                break;
            u = q_get(j, i);
            if (u->flags & DR_F_SYNC) {
                if (end <= u->time)
                    break;
                r = junction(j, &s, i, end);
                if (r != -1)
                    return r;
                j->tail_done = j->st.in - j->n;   /* re-timed: not searched again */
                break;
            }
            if (u->time != end)
                break;
            end += u->dur;
        }
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
            if (ns > -FFMAX(GAP_FLOOR, f->dur)) {
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
    if (f->dur + s.base <= s.skew) {
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

int ff_discrip_junction_open(DRJunction **jp, void *logctx, const DRJunctionConfig *cfg)
{
    DRJunction *j = av_mallocz(sizeof(*j));

    *jp = NULL;
    if (!j)
        return AVERROR(ENOMEM);
    j->log      = logctx;
    j->cfg      = *cfg;
    j->c        = FFMAX(MS, cfg->tolerance);
    j->last_out = -1;
    j->tail_done = -1;
    *jp = j;
    return 0;
}

int ff_discrip_junction_push(DRJunction *j, DRFrame *f)
{
    if (j->ended) {
        ff_discrip_frame_unref(f);
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

int ff_discrip_junction_finish(DRJunction *j)
{
    j->finished = 1;
    return run(j);
}

void ff_discrip_junction_stats(const DRJunction *j, DRJunctionStats *st)
{
    *st      = j->st;
    st->skew = j->s.skew;
    st->base = j->s.base;
}

void ff_discrip_junction_close(DRJunction **jp)
{
    DRJunction *j = *jp;

    if (!j)
        return;
    while (j->n)
        q_drop_head(j);
    hist_clear(j);
    av_freep(&j->hist);
    av_freep(&j->q);
    av_freep(jp);
}
