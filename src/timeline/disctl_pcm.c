/*
 * The title timeline, stage 4 for PCM tracks: the samples of a track's
 * frames (on the title timeline) leave in fixed frames timed by a sample
 * counter. A gap in the times is filled with silence, a frame whose time is
 * broken but whose samples go on is appended, overlapping frames are cut so
 * that every sample is written once.
 */

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

#include "parse/rational.h"

#include "timeline/disctl.h"

#define MS          1080000LL
#define DRIFT       10800               /* ticks: a frame this far off the sample count is off it */
#define LOOK        32                  /* frames looked at for a broken time */

enum { HAVE = 1, NONE = 0, WAIT = 2 };

struct DRPcm {
    void        *log;
    DRPcmConfig  cfg;
    uint32_t     in, out;               /* ticks = samples x out / in */
    int          n;                     /* samples per output frame */
    int          bpf;                   /* bytes per sample frame */
    uint8_t     *buf;                   /* the output frame being filled */
    int          fill;                  /* samples in it */
    int64_t      emitted;               /* samples of the frames handed on (and skipped at the start) */
    int64_t      carry;                 /* silence samples still to write */
    int64_t      origin;                /* the sample clock = title time + origin: a track that starts before
                                           the video starts its clock (and its output times) at its first frame */

    DRFrame     *q;                     /* input frames q[head .. head + nq) */
    int          head, nq, cap;
    DRFrame      held;                  /* the rest of a frame that overlapped the one before it */
    int          have_held;
    int          started, finished, ended;
    int          seg;                   /* the segment of the frame whose samples were taken last */
    DRPcmStats   st;
};

static void event(DRPcm *p, int kind, int64_t pos, int64_t dur, int64_t count)
{
    if (p->cfg.event)
        p->cfg.event(p->cfg.event_opaque, &(DREvent){ kind, p->cfg.track, pos, dur, 0, count });
}

static DRFrame *q_get(DRPcm *p, int i)
{
    return &p->q[p->head + i];
}

static void q_pop(DRPcm *p)
{
    ff_disctl_frame_unref(q_get(p, 0));
    p->head++;
    p->nq--;
}

static int need(DRPcm *p, int k)
{
    return k < p->nq ? HAVE : p->finished ? NONE : WAIT;
}

static int samples_of(const DRPcm *p, const DRFrame *f)
{
    return f->size / p->bpf;
}

static int is_marker(const DRFrame *f)
{
    return !f->dur && !f->size;
}

/* ---- output ---- */

static int emit(DRPcm *p)
{
    DRFrame f = { .flags = DR_F_KEY, .pos = -1 };
    int bytes = p->fill * p->bpf;

    if (!p->fill)
        return 0;
    if (!(f.buf = dr_buf_alloc(bytes + DR_BUF_PADDING)))
        return DR_ERR(ENOMEM);
    memcpy(dr_buf_data(f.buf), p->buf, bytes);
    memset(dr_buf_data(f.buf) + bytes, 0, DR_BUF_PADDING);
    f.data = dr_buf_data(f.buf);
    f.size = bytes;
    f.time = (int64_t)((uint64_t)p->emitted * p->out / p->in) - p->origin;
    f.dur  = (int64_t)((uint64_t)p->fill * p->out / p->in);
    f.src  = f.time;
    f.seg  = p->seg;
    p->emitted += p->fill;
    p->fill     = 0;
    p->st.out++;
    return p->cfg.out(p->cfg.out_opaque, &f);
}

/* Samples into the output frames; 1 when a frame was handed on. */
static int append(DRPcm *p, const uint8_t *data, int64_t n, int *emitted)
{
    while (n > 0) {
        int k = DR_MIN(n, p->n - p->fill), ret;
        memcpy(p->buf + p->fill * p->bpf, data, k * p->bpf);
        p->fill += k;
        data    += k * p->bpf;
        n       -= k;
        if (p->fill == p->n) {
            if ((ret = emit(p)) < 0)
                return ret;
            *emitted = 1;
        }
    }
    return 0;
}

static int silence(DRPcm *p, int64_t n, int *emitted)
{
    uint8_t z[512];

    memset(z, p->cfg.bits == 8 ? 0x80 : 0x00, sizeof(z));
    p->st.silence += n;
    while (n > 0) {
        int64_t m = DR_MIN(n, (int64_t)(sizeof(z) / p->bpf));
        int ret = append(p, z, m, emitted);
        if (ret < 0)
            return ret;
        n -= m;
    }
    return 0;
}

/* ---- the start ---- */

/* The track's start: one before the video keeps every frame (the sample
 * clock starts at its first frame, a negative delay); then a start after 0
 * is filled with silence (below 5 s) or skipped on the output clock. */
static int start(DRPcm *p)
{
    DRFrame *f;
    int64_t t, k;
    int r;

    if ((r = need(p, 0)) != HAVE)
        return r == WAIT ? WAIT : HAVE;
    t = q_get(p, 0)->time;
    if (t < 0) {
        p->origin = -t;
        if (-t > 540000)
            event(p, DR_EV_START_EARLY, 0, -t, 0);
    }
    f = q_get(p, 0);
    t = p->origin + f->time;
    if (t < 0)
        return DR_ERR_BUG;
    k = (int64_t)((uint64_t)t * p->cfg.rate / DR_TICKS_PER_SECOND);
    if (k < (int64_t)p->cfg.rate * 5) {
        p->carry = k;
        if (t > 3 * MS)
            event(p, DR_EV_PCM_SILENCE, 0, t, k);
    } else {
        p->emitted = k;
        event(p, DR_EV_PCM_SKIP, 0, t, k);
    }
    return HAVE;
}

/* ---- per frame ---- */

/* The frame consumed: the held one, or the queue's head. */
static void done(DRPcm *p)
{
    if (p->have_held) {
        ff_disctl_frame_unref(&p->held);
        p->have_held = 0;
    } else
        q_pop(p);
}

/* A frame after a gap whose next frames go on from where this one would end
 * without the gap carried a broken time: it and those frames are appended. */
static int broken_time(DRPcm *p, const DRFrame *f, int idx, int64_t spos, int *handled)
{
    int64_t opos = p->fill + p->emitted, end, gpos;
    int k = 0, r, e = 0, ret;

    *handled = 0;
    if (!(opos < spos))
        return HAVE;
    end = opos + samples_of(p, f);
    for (int i = 0; i < LOOK; i++) {
        const DRFrame *g;
        int64_t gts;

        if ((r = need(p, idx)) == WAIT)
            return WAIT;
        if (r == NONE)
            return HAVE;
        g = q_get(p, idx);
        if (is_marker(g))
            return HAVE;
        gts = g->time + p->origin;
        if (gts < 0)
            return DR_ERR_INVALID;
        gpos = (int64_t)((uint64_t)gts * p->in / p->out);
        if (gpos == (spos - opos) + end) {
            end += samples_of(p, g);
            k++;
            idx++;
            continue;
        }
        if (gpos != end)
            return HAVE;
        event(p, DR_EV_PCM_TIMECODE, f->time + p->origin, (int64_t)((uint64_t)(spos - opos) * p->out / p->in),
              k + 1);
        p->st.broken += k + 1;
        if ((ret = append(p, f->data, samples_of(p, f), &e)) < 0)
            return ret;
        done(p);
        for (int m = 0; m < k; m++) {
            g = q_get(p, 0);
            if ((ret = append(p, g->data, samples_of(p, g), &e)) < 0)
                return ret;
            q_pop(p);
        }
        *handled = 1;
        return HAVE;
    }
    return HAVE;
}

/* Keeps frame g (the queue's frame idx) as the held frame, without its first
 * skip samples. */
static int hold(DRPcm *p, int idx, int64_t gpos, int skip)
{
    DRFrame *g = q_get(p, idx);
    int keep = samples_of(p, g) - skip;

    p->held = *g;
    memset(g, 0, sizeof(*g));
    p->held.data += skip * p->bpf;
    p->held.size  = keep * p->bpf;
    p->held.time  = (int64_t)((uint64_t)(skip + gpos) * p->out / p->in) - p->origin;
    p->held.dur   = (int64_t)((uint64_t)keep * p->out / p->in);
    p->have_held  = 1;
    /* the queue slot is empty now: remove it */
    memmove(p->q + p->head + idx, p->q + p->head + idx + 1, (p->nq - idx - 1) * sizeof(*p->q));
    p->nq--;
    return 0;
}

/* Frame f (the held frame, idx 0, or the queue's head, idx 1) on the
 * timeline of the samples written. */
static int timeline(DRPcm *p, DRFrame *f, int idx)
{
    int64_t pts = p->origin + f->time, opos = p->fill + p->emitted;
    int64_t exp = (int64_t)((uint64_t)opos * p->out / p->in);
    int64_t spos = (int64_t)((uint64_t)pts * p->in / p->out);
    int64_t n = samples_of(p, f), copy = n;
    int e = 0, r, ret;

    p->seg = f->seg;
    if (p->carry)
        return DR_ERR_BUG;
    if (DR_ABS(exp - pts) > DRIFT && DR_ABS(spos - opos) > 2) {
        int ended = 0, handled;

        if (p->cfg.video.advance) {
            if ((ret = p->cfg.video.advance(p->cfg.video.opaque, f->time, &ended)) < 0)
                return ret;
            if (ended) {
                /* the video ends before this frame: the rest of the track is left out */
                event(p, DR_EV_VIDEO_ENDED, f->time, 0, 0);
                p->ended = 1;
                if (p->have_held) {
                    ff_disctl_frame_unref(&p->held);
                    p->have_held = 0;
                }
                while (p->nq)
                    q_pop(p);
                return emit(p) < 0 ? DR_ERR(ENOMEM) : HAVE;
            }
        }
        if (p->have_held)
            return DR_ERR_BUG;
        if ((r = broken_time(p, f, idx, spos, &handled)) != HAVE)
            return r;
        if (handled)
            return HAVE;
        if (spos < opos) {
            dr_vlog(p->log, DR_VLOG_ERROR, "Title timeline: PCM track %d: a frame %"PRId64" samples before the samples "
                   "written reaches the silence path: copied as it is\n", p->cfg.track, opos - spos);
        } else {
            int64_t gap = spos - opos, fill = DR_MIN(gap, 2 * (int64_t)p->n);
            p->carry += gap - fill;
            event(p, DR_EV_PCM_SILENCE, exp, pts - exp, gap);
            if ((ret = silence(p, fill, &e)) < 0)
                return ret;
            if (e)
                return HAVE;            /* the frame goes on at the next step */
            opos = p->fill + p->emitted;
        }
    }
    /* the next frame cuts this one */
    if ((r = need(p, idx)) == WAIT)
        return WAIT;
    if (r == HAVE && !is_marker(q_get(p, idx))) {
        const DRFrame *g = q_get(p, idx);
        int64_t gts = p->origin + g->time, gpos, gn = samples_of(p, g);

        if (gts < 0)
            return DR_ERR_INVALID;
        gpos = (int64_t)((uint64_t)gts * p->in / p->out);
        if (gpos < spos) {
            int64_t end_g = gpos + gn;
            if (spos + n < end_g) {
                /* g starts before this frame and ends after it: this frame,
                 * then the rest of g */
                int64_t ov = end_g - (spos + n);
                if ((ret = append(p, f->data, n, &e)) < 0)
                    return ret;
                done(p);
                p->st.overlap++;
                return hold(p, idx == 1 ? 0 : idx, gpos, (int)(gn - ov));
            }
            /* g lies inside this frame: g is dropped */
            p->st.overlap++;
            if (idx == 1 && !p->have_held) {
                p->held = *q_get(p, 0);
                memset(q_get(p, 0), 0, sizeof(DRFrame));
                p->head++;
                p->nq--;
                p->have_held = 1;
                q_pop(p);
                return HAVE;
            }
            if (idx == 0 && p->have_held) {
                q_pop(p);
                return HAVE;
            }
            return DR_ERR_BUG;
        }
        {
            int64_t end_out = n + opos;
            int64_t end_ticks = (int64_t)((uint64_t)end_out * p->out / p->in);
            if (end_ticks > gts && end_ticks - gts >= DRIFT + 1 && DR_ABS(end_out - gpos) >= 3) {
                /* this frame runs into the next one: cut at its start */
                p->st.overlap++;
                copy = opos < gpos ? gpos - opos : 0;
            }
        }
    }
    if ((ret = append(p, f->data, copy, &e)) < 0)
        return ret;
    done(p);
    return HAVE;
}

static int step(DRPcm *p)
{
    int e = 0, ret;

    if (p->nq) {
        DRFrame *f = q_get(p, 0);
        if (p->origin + f->time < 0)
            return DR_ERR_INVALID;
        if (is_marker(f)) {
            q_pop(p);
            return HAVE;
        }
    }
    if (p->carry) {
        int64_t m = DR_MIN(p->carry, (int64_t)p->n + 1);
        p->carry -= m;
        if ((ret = silence(p, m, &e)) < 0)
            return ret;
        if (e)
            return HAVE;
    }
    return p->have_held ? timeline(p, &p->held, 0) : timeline(p, q_get(p, 0), 1);
}

static int run(DRPcm *p)
{
    int r;

    if (p->ended) {
        while (p->nq)
            q_pop(p);
        return 0;
    }
    if (!p->started) {
        if ((r = start(p)) == WAIT)
            return 0;
        if (r < 0)
            return r;
        p->started = 1;
    }
    while ((p->nq || p->have_held) && !p->ended) {
        r = step(p);
        if (r == WAIT)
            break;
        if (r < 0)
            return r;
    }
    if (p->head > 64 && p->head > p->nq) {
        memmove(p->q, p->q + p->head, p->nq * sizeof(*p->q));
        p->head = 0;
    }
    return 0;
}

int ff_disctl_pcm_open(DRPcm **pp, void *logctx, const DRPcmConfig *cfg)
{
    DRPcm *p;
    uint64_t g;

    *pp = NULL;
    if (cfg->rate <= 0 || cfg->bytes_per_sample_frame <= 0 || cfg->bytes_per_sample_frame > 512)
        return DR_ERR(EINVAL);
    if (!(p = dr_mallocz(sizeof(*p))))
        return DR_ERR(ENOMEM);
    p->log = logctx;
    p->cfg = *cfg;
    g      = dr_gcd64(cfg->rate, DR_TICKS_PER_SECOND);
    p->in  = cfg->rate / g;
    p->out = DR_TICKS_PER_SECOND / g;
    /* 1/30 s at 48 / 44.1 kHz; else about 32 ms rounded up to a divisor of the rate */
    p->n   = cfg->rate == 48000 ? 1600 : cfg->rate == 44100 ? 1470 : (int)((int64_t)cfg->rate * 32 / 1000);
    while (cfg->rate % p->n)
        p->n++;
    p->bpf = cfg->bytes_per_sample_frame;
    if (!(p->buf = dr_malloc((size_t)p->n * p->bpf))) {
        dr_free(p);
        return DR_ERR(ENOMEM);
    }
    *pp = p;
    return 0;
}

int ff_disctl_pcm_push(DRPcm *p, DRFrame *f)
{
    if (p->ended) {
        ff_disctl_frame_unref(f);
        return 0;
    }
    if (p->head + p->nq == p->cap) {
        int cap = p->cap ? 2 * p->cap : 64;
        DRFrame *q;
        if (p->head) {
            memmove(p->q, p->q + p->head, p->nq * sizeof(*p->q));
            p->head = 0;
        }
        if (p->nq == p->cap) {
            if (!(q = dr_realloc_array(p->q, cap, sizeof(*q)))) {
                ff_disctl_frame_unref(f);
                return DR_ERR(ENOMEM);
            }
            p->q   = q;
            p->cap = cap;
        }
    }
    p->q[p->head + p->nq++] = *f;
    memset(f, 0, sizeof(*f));
    p->st.in++;
    return run(p);
}

int ff_disctl_pcm_finish(DRPcm *p)
{
    int ret;

    p->finished = 1;
    if ((ret = run(p)) < 0)
        return ret;
    /* the frame being filled leaves as it is */
    return p->ended ? 0 : emit(p);
}

void ff_disctl_pcm_stats(const DRPcm *p, DRPcmStats *st)
{
    *st = p->st;
}

void ff_disctl_pcm_close(DRPcm **pp)
{
    DRPcm *p = *pp;

    if (!p)
        return;
    while (p->nq)
        q_pop(p);
    if (p->have_held)
        ff_disctl_frame_unref(&p->held);
    dr_freep(&p->q);
    dr_freep(&p->buf);
    dr_freep(pp);
}
