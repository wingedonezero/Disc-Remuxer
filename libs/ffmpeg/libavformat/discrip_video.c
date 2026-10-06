/*
 * The shared rip core, stage 2 for video: pictures of one segment timed on a
 * fixed grid.
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
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#include "discrip.h"

#define THR           108000LL      /* 0.1 ms: a PES time this far from the grid is off it */
#define GROUP_MAX     450           /* pictures of one group / of one batch scan */
#define VOTE_UNITS    1000
#define RATE_UNITS    5000
#define UNITS_FRAME   2             /* grid unit = one field */

enum { HAVE = 1, NONE = 0, WAIT = 2 };

typedef struct VUnit {
    DRFrame  f;                     /* f.time = PES time (or none) until the unit leaves */
    int      type, order, fields;
    int64_t  pos;                   /* presentation position in fields */
    int64_t  dec;                   /* decode position in fields */
    int64_t  rank;
    int      adjusted;              /* position already includes the offset (repair) */
    int      key;                   /* key frame by the codec's own rule */
} VUnit;

typedef struct VQ {
    VUnit *u;
    int    head, n, cap;
} VQ;

struct DRVideo {
    void               *log;
    const DRCodec      *codec;
    const DRVideoRules *rules;
    int                 track;
    DRFrameCb           cb;   void *opaque;
    DREventCb           event; void *event_opaque;
    void               *priv;

    VUnit               held;      /* a field picture waiting for its second field */
    int                 have_held;
    VQ                  c;         /* units before the counted-order stage (counted_order codecs) */
    uint32_t            ctr;       /* the display-order counter */
    VQ                  g;         /* units before the group stage */
    VQ                  q;         /* units with positions, before timing */
    int                 finished;

    int64_t             t_pres, t_dec;

    int                 num, den;  /* frame rate */
    uint64_t            v70, v78;  /* ticks per field = v70 / v78 */
    int64_t             base;
    int                 started;
    int64_t             a8;        /* running grid offset (fields) */
    int64_t             b0;        /* placeholder position */
    uint32_t            b8, bc;    /* placeholders pending, offset still to add */
    int64_t             e8;        /* highest position handed on */
    uint32_t            emitted;
    uint32_t            invalid, ndiff;
    int64_t             lastdiff;
    int                 have_lastdiff;
    DRVideoStats        st;
};

/* ---- queues ---- */

static VUnit *at(VQ *q, int i)
{
    return &q->u[q->head + i];
}

static int q_push(VQ *q, const VUnit *u)
{
    if (q->head && q->head + q->n == q->cap) {
        memmove(q->u, q->u + q->head, q->n * sizeof(*q->u));
        q->head = 0;
    }
    if (q->head + q->n == q->cap) {
        int cap = q->cap ? 2 * q->cap : 256;
        VUnit *n = av_realloc_array(q->u, cap, sizeof(*n));
        if (!n)
            return AVERROR(ENOMEM);
        q->u   = n;
        q->cap = cap;
    }
    q->u[q->head + q->n++] = *u;
    return 0;
}

static void q_pop(VQ *q)
{
    q->head++;
    q->n--;
}

static void q_free(VQ *q)
{
    for (int i = 0; i < q->n; i++)
        ff_discrip_frame_unref(&at(q, i)->f);
    av_freep(&q->u);
    q->head = q->n = 0;
}

static int is_pic(int type)
{
    return type >= DR_PIC_I && type <= DR_PIC_B;
}

static void event(DRVideo *v, int kind, int64_t pos, int64_t dur, int64_t count)
{
    if (v->event)
        v->event(v->event_opaque, &(DREvent){ kind, v->track, pos, dur, 0, count });
}

/* ---- the group stage: display order and positions ---- */

/* Display order (comparator): order numbers with wrap-around, then PES
 * times, then the rank from the decode-order pattern. */
static int display_less(const DRVideo *v, const VUnit *a, const VUnit *b)
{
    int P = v->rules->order_period;
    int A = a->order, B = b->order;

    if (P >= 0x200) {
        A += (B > P - 128 && (unsigned)A < 128) ? P : 0;
        B += (A > P - 128 && (unsigned)B < 128) ? P : 0;
    }
    if (A >= 0 && B >= 0 && A != B)
        return (unsigned)A < (unsigned)B;
    if (A <= 0 && B <= 0 && A != B)
        return A < B;
    if (A >= -127 && A <= 127 && B >= -127 && B <= 127 && A != B)
        return A < B;
    if (P >= 0x200 && P <= 0x40000000) {
        if (P - 128 < A && A < P && B < 0 && P <= B + 2 * P && B + 2 * P < P + 128 && A != B + 2 * P)
            return A < B + 2 * P;
        if (P - 128 < B && B < P && A < 0 && P <= A + 2 * P && A + 2 * P < P + 128 && A + 2 * P != B)
            return A + 2 * P < B;
    }
    if (a->f.time != AV_NOPTS_VALUE && b->f.time != AV_NOPTS_VALUE && a->f.time > 0 && b->f.time > 0 &&
        a->f.time != b->f.time)
        return (uint64_t)a->f.time < (uint64_t)b->f.time;
    return (uint64_t)a->rank < (uint64_t)b->rank;
}

/* Next picture of the group from g[*consumed] (units that are no picture are
 * consumed with it). */
static int collect(DRVideo *v, int *pend, int *count, int *consumed)
{
    while (*consumed < v->g.n) {
        int i = (*consumed)++;
        if (is_pic(at(&v->g, i)->type)) {
            if (*count == GROUP_MAX) {
                av_log(v->log, AV_LOG_ERROR, "Rip core: video: more than %d pictures in one group\n", GROUP_MAX);
                return AVERROR_INVALIDDATA;
            }
            pend[(*count)++] = i;
            return HAVE;
        }
    }
    return v->finished && !v->c.n ? NONE : WAIT;
}

/* One group: the pictures from one I picture to the next (at most 448), with
 * decode positions in arrival order and presentation positions in display
 * order; then every consumed unit moves on. */
static int group(DRVideo *v)
{
    int pend[GROUP_MAX], sorted[GROUP_MAX];
    int count = 0, consumed = 0, c, k, r, ret;

    if ((r = collect(v, pend, &count, &consumed)) == WAIT || r < 0)
        return r;
    if (count) {
        for (k = 2;; k++) {
            if ((r = collect(v, pend, &count, &consumed)) == WAIT || r < 0)
                return r;
            c = count;
            if (c < k || at(&v->g, pend[c - 1])->type == DR_PIC_I || k > 0x1c0)
                break;
        }
        if (c >= 2 && at(&v->g, pend[c - 1])->type == DR_PIC_I) {
            count    = c - 1;
            consumed = pend[c - 1];   /* the next group's I picture (and what follows) stays */
        }
        for (int i = 0; i < count; i++) {
            VUnit *u = at(&v->g, pend[i]);
            u->dec    = v->t_dec;
            v->t_dec += u->fields;
        }
        /* rank: B pictures following a reference picture are shown before it */
        {
            int64_t rk = 0;
            int j = 0;
            while (j < count && at(&v->g, pend[j])->type == DR_PIC_B)
                at(&v->g, pend[j++])->rank = rk++;
            while (j < count) {
                int n = j + 1;
                while (n < count && at(&v->g, pend[n])->type == DR_PIC_B)
                    at(&v->g, pend[n++])->rank = rk++;
                at(&v->g, pend[j])->rank = rk++;
                j = n;
            }
        }
        /* stable insertion sort into display order */
        for (int i = 0; i < count; i++) {
            int x = pend[i], j = i;
            while (j > 0 && display_less(v, at(&v->g, x), at(&v->g, sorted[j - 1]))) {
                sorted[j] = sorted[j - 1];
                j--;
            }
            sorted[j] = x;
        }
        for (int i = 0; i < count; i++) {
            VUnit *u = at(&v->g, sorted[i]);
            u->pos     = v->t_pres;
            v->t_pres += u->fields;
        }
        v->st.pictures += count;
    } else if (!consumed)
        return NONE;
    for (int i = 0; i < consumed; i++) {
        if ((ret = q_push(&v->q, at(&v->g, 0))) < 0)
            return ret;
        q_pop(&v->g);
    }
    return HAVE;
}

/* ---- the grid ---- */

static int64_t grid(const DRVideo *v, int64_t pos)
{
    return (int64_t)((uint64_t)pos * v->v70 / v->v78) + v->base;
}

static int64_t offset_of(const DRVideo *v, const VUnit *u)
{
    return u->adjusted ? 0 : v->a8;
}

static void set_scale(DRVideo *v)
{
    uint64_t a = (uint64_t)v->den * DR_TICKS_PER_SECOND, b = (uint64_t)v->num * UNITS_FRAME;
    uint64_t g = av_gcd(a, b);

    v->v70 = a / g;
    v->v78 = b / g;
    v->st.num = v->num;
    v->st.den = v->den;
}

/* Units in q, or whether more will come: HAVE / NONE / WAIT for unit k. */
static int need(DRVideo *v, int k)
{
    return k < v->q.n ? HAVE : (v->finished && !v->g.n && !v->c.n) ? NONE : WAIT;
}

/* The frame rate from the pictures' PES times when the codec gives none:
 * I pictures with times, over up to 5000 units, snapped to the usual rates. */
static int measure_rate(DRVideo *v)
{
    int i0 = -1, i1 = -1, r;
    int64_t t0 = 0, t1 = 0;
    uint64_t R;

    for (int i = 0; i < RATE_UNITS; i++) {
        VUnit *u;
        if ((r = need(v, i)) == WAIT)
            return WAIT;
        if (r == NONE)
            break;
        u = at(&v->q, i);
        if (u->type != DR_PIC_I || u->f.time == AV_NOPTS_VALUE || u->f.time < 0)
            continue;
        if (i0 < 0) {
            i0 = i;
            t0 = u->f.time;
            continue;
        }
        i1 = i;
        t1 = u->f.time;
        if (i - i0 < 501 || t1 <= t0)
            continue;
        R = (uint64_t)(i - i0) * 1080000000000ULL / (uint64_t)(t1 - t0);
        if ((R >= 23975 && R <= 23977) || (R >= 23999 && R <= 24001) || (R >= 24999 && R <= 25001))
            break;
    }
    if (i0 < 0 || i1 < 0 || t1 <= t0) {
        av_log(v->log, AV_LOG_ERROR, "Rip core: video: no frame rate in the stream and too few I pictures with "
               "times to measure one\n");
        return AVERROR_INVALIDDATA;
    }
    R = (uint64_t)(i1 - i0) * 1080000000000ULL / (uint64_t)(t1 - t0);
    if (R >= 22800 && R <= 23979)      { v->num = 24000; v->den = 1001; }
    else if (R >= 24000 && R <= 24119) { v->num = 24;    v->den = 1;    }
    else if (R >= 24100 && R <= 25899) { v->num = 25;    v->den = 1;    }
    else if (R >= 29800 && R <= 30899) { v->num = 30000; v->den = 1001; }
    else                               { v->num = R / 1000; v->den = 1; }
    av_log(v->log, AV_LOG_WARNING, "Rip core: video: no frame rate in the stream; measured %d/%d from the "
           "pictures' times\n", v->num, v->den);
    return HAVE;
}

typedef struct Vote {
    int64_t key;
    unsigned votes;
} Vote;

/* The grid base: the most-voted value of (PES time - position x field
 * duration) over the first 1000 pictures with times; near values (1/500
 * field) merge. */
static int start(DRVideo *v)
{
    Vote h[VOTE_UNITS];
    int nh = 0, r;
    uint64_t frames7s;

    if (!v->num || !v->den) {
        if ((r = measure_rate(v)) != HAVE)
            return r;
    }
    set_scale(v);
    frames7s = (uint64_t)v->num * 0x70 / ((uint64_t)v->den << 4);
    for (int i = 0; i < VOTE_UNITS; i++) {
        VUnit *u;
        int64_t key, tol;
        int k;

        if ((r = need(v, i)) == WAIT)
            return WAIT;
        if (r == NONE)
            break;
        u = at(&v->q, i);
        if (!is_pic(u->type) || u->f.time == AV_NOPTS_VALUE)
            continue;
        key = u->f.time - (int64_t)(v->v70 * (uint64_t)u->pos / v->v78);
        for (k = 0; k < nh && h[k].key < key; k++)
            ;
        if (k < nh && h[k].key == key)
            h[k].votes++;
        else {
            memmove(h + k + 1, h + k, (nh - k) * sizeof(*h));
            h[k] = (Vote){ key, 1 };
            nh++;
        }
        tol = (int64_t)(v->v70 / v->v78) / 500;
        for (;;) {
            int m = -1;
            for (k = 0; k + 1 < nh; k++)
                if (FFABS(h[k + 1].key - h[k].key) < tol) {
                    m = k;
                    break;
                }
            if (m < 0)
                break;
            h[m + 1].votes += h[m].votes;   /* the larger key keeps the votes */
            memmove(h + m, h + m + 1, (nh - m - 1) * sizeof(*h));
            nh--;
        }
        if ((uint64_t)i > frames7s && nh) {
            int best = 0, prev = -1;
            for (k = 1; k < nh; k++)
                if (h[k].votes > h[best].votes) {
                    prev = best;
                    best = k;
                }
            if (nh == 1 && h[best].votes >= 12) {
                v->base = h[best].key;
                return HAVE;
            }
            if (prev >= 0 && h[best].key < h[prev].key && h[best].votes >= 12 &&
                h[best].votes > 8 * h[prev].votes) {
                v->base = h[best].key;
                return HAVE;
            }
        }
    }
    if (!nh) {
        av_log(v->log, AV_LOG_DEBUG, "Rip core: video: no picture with a time: grid base 0\n");
        v->base = 0;
        return HAVE;
    }
    {
        int best = 0;
        for (int k = 1; k < nh; k++)
            if (h[k].votes > h[best].votes)
                best = k;
        v->base = h[best].key;
    }
    return HAVE;
}

/* The grid unit nearest to a time; -1 when more than 0.1 ms before the base
 * or farther than 0.1 s from any unit. */
static int64_t time_to_unit(const DRVideo *v, int64_t t)
{
    int64_t rel, i0, best = 0, err;
    uint64_t q;

    if (t < v->base) {
        if (v->base - t >= THR)
            return -1;
        t = v->base;
    }
    rel = t - v->base;
    q   = v->v78 * (uint64_t)rel / v->v70;
    i0  = q < 3 ? 0 : (int64_t)q - 3;
    err = FFABS((int64_t)((uint64_t)i0 * v->v70 / v->v78) - rel);
    for (int k = 1; k <= 6 && err; k++) {
        int64_t e = FFABS((int64_t)((uint64_t)(i0 + k) * v->v70 / v->v78) - rel);
        if (e < err) {
            err  = e;
            best = k;
        }
    }
    if (err < THR || err < (int64_t)(v->v70 / v->v78) || err < THR * 1000)
        return i0 + best;
    return -1;
}

/* A picture's PES time off its grid time: counted, reported once per new
 * difference, at most 40 different ones. */
static void timecode(DRVideo *v, int64_t t_ref, int64_t t_other)
{
    int64_t d = t_other - t_ref;

    v->invalid++;
    if (v->have_lastdiff && v->lastdiff == d) {
        if (v->ndiff == 40) {
            event(v, DR_EV_VIDEO_TIMECODE_LIMIT, t_ref, 0, 0);
            v->ndiff++;
        }
        return;
    }
    v->ndiff++;
    v->lastdiff      = d;
    v->have_lastdiff = 1;
    if (v->ndiff > 40)
        return;
    event(v, DR_EV_VIDEO_TIMECODE, t_ref, d, 0);
    if (v->ndiff == 40) {
        event(v, DR_EV_VIDEO_TIMECODE_LIMIT, t_ref, 0, 0);
        v->ndiff++;
    }
}

/* Hands n units on: pictures with their grid times and durations (key / B
 * flags), the last one marked as the end of the batch; other units are left
 * out. The run is first extended by the units shown before its end. */
static int assign(DRVideo *v, int n, int no_check)
{
    VUnit *prev = at(&v->q, n - 1);
    int64_t lim = prev->pos + offset_of(v, prev);
    int last_pic = -1, r;

    if (n < GROUP_MAX) {
        int c = 0;
        for (int j = n; j < GROUP_MAX; j++) {
            VUnit *g;
            if ((r = need(v, j)) == WAIT)
                return WAIT;
            if (r == NONE)
                break;
            g = at(&v->q, j);
            if (g->type == DR_PIC_OTHER) {
                if (!c)
                    n = j + 1;
            } else if (g->pos + offset_of(v, g) >= lim) {
                if (++c > 16)
                    break;
            } else {
                n = j + 1;
                c = 0;
            }
        }
    }
    for (int k = 0; k < n; k++)
        if (at(&v->q, k)->type != DR_PIC_OTHER)
            last_pic = k;
    for (int k = 0; k < n; k++) {
        VUnit u = *at(&v->q, 0);
        int ret = 0;

        q_pop(&v->q);
        if (u.type == DR_PIC_OTHER) {
            ff_discrip_frame_unref(&u.f);
            continue;
        }
        {
            int64_t p  = u.pos + offset_of(v, &u);
            int64_t ts = grid(v, p);
            if (u.f.time != AV_NOPTS_VALUE && u.f.time >= 0 && !no_check && FFABS(ts - u.f.time) >= THR)
                timecode(v, ts, u.f.time);
            u.f.time = ts;
            u.f.dur  = (int64_t)((uint64_t)u.fields * v->v70 / v->v78);
            if (p >= 0 && p > v->e8)
                v->e8 = p;
        }
        u.f.flags &= ~(DR_F_KEY | DR_F_DISCARD | DR_F_BATCH);
        if (u.type == DR_PIC_I || u.key)
            u.f.flags |= DR_F_KEY;
        else if (u.type == DR_PIC_B)
            u.f.flags |= DR_F_DISCARD;
        if (k == last_pic)
            u.f.flags |= DR_F_BATCH;
        v->emitted++;
        v->st.out++;
        if ((ret = v->cb(v->opaque, &u.f)) < 0) {
            for (k++; k < n; k++) {
                ff_discrip_frame_unref(&at(&v->q, 0)->f);
                q_pop(&v->q);
            }
            return ret;
        }
    }
    return HAVE;
}

/* The grid follows a jump of the PES times: the current picture keeps the old
 * grid (+3/16 s), placeholders fill the rest, later pictures are on the new
 * grid; a small backward drift moves the grid one field per call. */
static int repair(DRVideo *v, int *more, int i, int64_t target)
{
    VUnit *f = at(&v->q, i);
    int64_t limit = f->pos + offset_of(v, f), d, step;
    int n_end = i + 1, r;
    uint32_t P = (uint32_t)(UNITS_FRAME * v->num), D = (uint32_t)v->den << 4;
    int64_t hi = (int32_t)((uint64_t)P * 0x2580 / D), lo = (int32_t)((uint64_t)P * 3 / D);
    int64_t neg = -(int32_t)((uint64_t)P * 0x50 / D);

    d = target - f->pos;
    if (d > hi || d < neg) {
        int m = v->q.n + 12000;
        if ((r = need(v, m - 1)) == WAIT)
            return WAIT;
        if (r == HAVE) {
            av_log(v->log, AV_LOG_ERROR, "Rip core: video: the PES times jump by %"PRId64" fields, beyond what the "
                   "grid follows\n", d);
            timecode(v, f->f.time, f->f.time + (int64_t)((uint64_t)d * v->v70 / v->v78));
            return AVERROR_INVALIDDATA;
        }
        *more = 1;
        return HAVE;
    }
    if (i + 1 <= 0x1c1) {
        int c = 0;
        for (int j = i + 2; j <= 0x1c2; j++) {
            VUnit *g;
            if ((r = need(v, j - 1)) == WAIT)
                return WAIT;
            if (r == NONE)
                break;
            g = at(&v->q, j - 1);
            if (g->type == DR_PIC_OTHER) {
                if (!c)
                    n_end = j;
                continue;
            }
            if (g->pos + offset_of(v, g) < limit) {
                n_end = j;
                c = 0;
                continue;
            }
            if (++c > 16)
                break;
        }
    }
    for (int k = 0; k < n_end; k++) {
        VUnit *g = at(&v->q, k);
        if (is_pic(g->type) && !g->adjusted && v->a8 + g->pos < limit) {
            g->adjusted = 1;
            g->pos     += v->a8;
        }
    }
    step = d >= 0 ? d : -1;
    if (d > lo) {
        v->bc = v->b8 = (uint32_t)(d - lo);
        v->b0 = limit + lo;
        step  = lo;
        event(v, DR_EV_VIDEO_REPAIR, f->f.time, d, (d - lo + 3) / 4);
    }
    v->a8 += step;
    *more  = 0;
    return HAVE;
}

/* One step of the video timing (one batch, or one placeholder). */
static int pull(DRVideo *v)
{
    int i, r, more = 0;
    VUnit *u;
    int64_t exp, idx, target;
    int start_case;
    uint64_t w1, w2;

    if (v->b8) {
        uint32_t k = FFMIN(v->b8, 4);
        int64_t old = v->b0;
        DRFrame m = { .time = grid(v, old), .flags = DR_F_KEY | DR_F_MARKER, .pos = -1 };
        v->b0 += k;
        v->b8 -= k;
        v->a8 += v->bc;
        v->bc  = 0;
        v->st.placeholders++;
        return v->cb(v->opaque, &m) < 0 ? AVERROR(EINVAL) : HAVE;
    }
    for (i = 0;; i++) {
        if ((r = need(v, i)) == WAIT)
            return WAIT;
        if (r == NONE)
            goto no_timed;
        u = at(&v->q, i);
        if (u->f.time != AV_NOPTS_VALUE && u->f.time >= 0 && is_pic(u->type))
            break;
        if (i + 1 == GROUP_MAX)
            goto no_timed;
    }
    exp = grid(v, offset_of(v, u) + u->pos);
    if (FFABS(u->f.time - exp) < THR)
        return assign(v, i + 1, 0);

    idx        = time_to_unit(v, u->f.time);
    start_case = v->emitted < 5 && idx == -1;
    if (start_case)
        idx = 0;
    target = idx - v->a8;
    w1 = i + (uint64_t)v->num * 0x2b / ((uint64_t)v->den << 4);
    if ((r = need(v, w1 - 1)) == WAIT)
        return WAIT;
    if (r == NONE)
        return assign(v, v->q.n, 1);           /* near the end: accepted without checks */
    if (start_case)
        return assign(v, i + 1, 0);
    w2 = i + (uint64_t)v->num * 400 / ((uint64_t)v->den << 4);
    if ((r = need(v, w2 - 1)) == WAIT)
        return WAIT;
    {
        uint64_t in = 0, out = 0;
        for (int j = i; j < v->q.n; j++) {
            VUnit *g = at(&v->q, j);
            if (is_pic(g->type) && g->f.time != AV_NOPTS_VALUE && g->f.time >= 0) {
                if (FFABS(g->f.time - grid(v, offset_of(v, g) + g->pos)) < THR)
                    in++;
                else
                    out++;
            }
        }
        if (out * 3 < in)
            return assign(v, i + 1, 0);        /* most pictures are on the grid: keep it */
    }
    if ((r = repair(v, &more, i, target)) != HAVE)
        return r;
    return more ? assign(v, v->q.n, 1) : assign(v, i + 1, 0);

no_timed:
    {
        int k = -1;
        for (int j = 0; j < v->q.n; j++)
            if (at(&v->q, j)->type == DR_PIC_I) {
                k = j;
                break;
            }
        if (k < 0)
            k = v->q.n > 0x1d2 ? 0x1c1 : v->q.n - 1;
        if (k < 0)
            return NONE;
        return assign(v, k + 1, 0);
    }
}

/* Counted display order: from a reference picture on, the B pictures that
 * follow it (at most 36 pictures) are numbered first, then the reference
 * picture; an I picture restarts the count; a leading B picture is numbered
 * on its own. Units that are no picture move on with the pictures. */
static int counted(DRVideo *v)
{
    int pend[37], count = 0, consumed = 0, k, ret;

    for (;;) {
        while (consumed < v->c.n && !is_pic(at(&v->c, consumed)->type))
            consumed++;
        if (consumed == v->c.n) {
            if (!v->finished || !consumed)
                return v->finished ? NONE : WAIT;
            goto move;
        }
        break;
    }
    pend[count++] = consumed++;
    if (at(&v->c, pend[0])->type == DR_PIC_B) {
        at(&v->c, pend[0])->order = v->ctr++ & 0xFFFFFF;
        goto move;
    }
    if (at(&v->c, pend[0])->type == DR_PIC_I)
        v->ctr = 0;
    for (k = 2; k != 0x25; k++) {
        while (consumed < v->c.n && !is_pic(at(&v->c, consumed)->type))
            consumed++;
        if (consumed == v->c.n) {
            if (!v->finished)
                return WAIT;
            break;
        }
        pend[count++] = consumed++;
        if (at(&v->c, pend[count - 1])->type != DR_PIC_B)
            break;
    }
    if (count >= 2 && at(&v->c, pend[count - 1])->type != DR_PIC_B) {
        consumed = pend[count - 1];    /* the next reference picture stays */
        count--;
    }
    for (int i = 1; i < count; i++)
        at(&v->c, pend[i])->order = v->ctr++ & 0xFFFFFF;
    at(&v->c, pend[0])->order = v->ctr++ & 0xFFFFFF;
move:
    for (int i = 0; i < consumed; i++) {
        if ((ret = q_push(&v->g, at(&v->c, 0))) < 0)
            return ret;
        q_pop(&v->c);
    }
    return HAVE;
}

static int run(DRVideo *v)
{
    int r;

    while (v->rules->counted_order) {
        r = counted(v);
        if (r < 0)
            return r;
        if (r != HAVE)
            break;
    }
    for (;;) {
        r = group(v);
        if (r < 0)
            return r;
        if (r != HAVE)
            break;
    }
    if (!v->started) {
        if ((r = start(v)) == WAIT)
            return 0;
        if (r < 0)
            return r;
        v->started  = 1;
        v->st.base  = v->base;
        av_log(v->log, AV_LOG_DEBUG, "Rip core: video: %d/%d fps, grid base %"PRId64" ticks\n",
               v->num, v->den, v->base);
    }
    while (v->q.n || v->b8) {
        r = pull(v);
        if (r == WAIT || r == NONE)
            break;
        if (r < 0)
            return r;
    }
    return 0;
}

/* ---- units in ---- */

static int to_next(DRVideo *v, VUnit *u)
{
    return q_push(v->rules->counted_order ? &v->c : &v->g, u);
}

static int unit_in(DRVideo *v, VUnit *u)
{
    v->st.units++;
    if ((u->type == DR_PIC_OTHER) || u->fields != 1) {
        if (v->have_held) {
            /* a lone field: emitted as a frame */
            int ret;
            v->held.fields = 2;
            v->have_held   = 0;
            av_log(v->log, AV_LOG_WARNING, "Rip core: video: a field picture without its second field\n");
            if ((ret = to_next(v, &v->held)) < 0)
                return ret;
        }
        return to_next(v, u);
    }
    if (!v->have_held) {
        v->held      = *u;
        v->have_held = 1;
        return 0;
    }
    /* the second field: one unit with both fields' bytes */
    {
        DRFrame *a = &v->held.f, *b = &u->f;
        AVBufferRef *buf = av_buffer_alloc(a->size + b->size + AV_INPUT_BUFFER_PADDING_SIZE);
        int ret;
        if (!buf)
            return AVERROR(ENOMEM);
        memcpy(buf->data, a->data, a->size);
        memcpy(buf->data + a->size, b->data, b->size);
        memset(buf->data + a->size + b->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        av_buffer_unref(&a->buf);
        a->buf  = buf;
        a->data = buf->data;
        a->size += b->size;
        ff_discrip_frame_unref(b);
        v->held.fields = 2;
        v->have_held   = 0;
        if ((ret = to_next(v, &v->held)) < 0)
            return ret;
    }
    return 0;
}

int ff_discrip_video_unit(void *opaque, const DRUnit *du)
{
    DRVideo *v = opaque;
    VUnit u = { .f = { .time = du->time, .pos = du->pos, .samples = du->samples, .rate = du->rate } };
    DRPicture pic = { 0 };
    int ret;

    if (!(u.f.buf = av_buffer_alloc(du->size + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);
    memcpy(u.f.buf->data, du->data, du->size);
    memset(u.f.buf->data + du->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    u.f.data = u.f.buf->data;
    u.f.size = du->size;
    if ((ret = v->rules->picture(v, &u.f, &pic)) < 0) {
        ff_discrip_frame_unref(&u.f);
        return ret;
    }
    u.type   = pic.type;
    u.order  = pic.order;
    u.fields = pic.fields;
    u.key    = pic.key;
    if (is_pic(u.type) && u.fields <= 0) {
        av_log(v->log, AV_LOG_ERROR, "Rip core: video: a picture without a duration\n");
        ff_discrip_frame_unref(&u.f);
        return AVERROR_INVALIDDATA;
    }
    if ((ret = unit_in(v, &u)) < 0) {
        ff_discrip_frame_unref(&u.f);
        return ret;
    }
    return run(v);
}

int ff_discrip_video_flush(DRVideo *v)
{
    int ret;

    if (v->have_held) {
        v->held.fields = 2;
        v->have_held   = 0;
        if ((ret = to_next(v, &v->held)) < 0)
            return ret;
    }
    v->finished = 1;
    if ((ret = run(v)) < 0)
        return ret;
    if (v->q.n || v->g.n || v->c.n)
        av_log(v->log, AV_LOG_WARNING, "Rip core: video: %d units left at the end of the segment\n",
               v->q.n + v->g.n + v->c.n);
    if (v->invalid && v->started)
        event(v, DR_EV_VIDEO_INVALID, grid(v, v->e8), 0, v->invalid);
    v->st.invalid = v->invalid;
    return 0;
}

int ff_discrip_video_open(DRVideo **vp, void *logctx, enum AVCodecID codec, int track,
                          DRFrameCb cb, void *opaque, DREventCb ev, void *ev_opaque)
{
    const DRCodec *c = ff_discrip_codec(codec);
    DRVideo *v;

    *vp = NULL;
    if (!c || !c->video) {
        av_log(logctx, AV_LOG_ERROR, "Rip core: %s has no video rules in the codec table: its track cannot be "
               "ripped\n", avcodec_get_name(codec));
        return AVERROR(ENOSYS);
    }
    if (!(v = av_mallocz(sizeof(*v))))
        return AVERROR(ENOMEM);
    v->log   = logctx;
    v->codec = c;
    v->rules = c->video;
    v->track = track;
    v->cb    = cb;
    v->opaque = opaque;
    v->event = ev;
    v->event_opaque = ev_opaque;
    if (v->rules->priv_size && !(v->priv = av_mallocz(v->rules->priv_size))) {
        av_free(v);
        return AVERROR(ENOMEM);
    }
    *vp = v;
    return 0;
}

void ff_discrip_video_set_rate(DRVideo *v, int num, int den)
{
    if (!v->started && num > 0 && den > 0) {
        v->num = num;
        v->den = den;
    }
}

void *ff_discrip_video_priv(DRVideo *v)
{
    return v->priv;
}

void ff_discrip_video_stats(const DRVideo *v, DRVideoStats *st)
{
    *st = v->st;
}

void ff_discrip_video_close(DRVideo **vp)
{
    DRVideo *v = *vp;

    if (!v)
        return;
    if (v->have_held)
        ff_discrip_frame_unref(&v->held.f);
    q_free(&v->c);
    q_free(&v->g);
    q_free(&v->q);
    if (v->priv && v->rules->close)
        v->rules->close(v->priv);
    av_freep(&v->priv);
    av_freep(vp);
}
