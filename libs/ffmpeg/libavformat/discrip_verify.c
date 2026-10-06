/*
 * The shared rip core, stage 5: checks of a track's output. Every unit is
 * parsed again, the times are checked, and the sync error of each frame is
 * measured both as a stream file would play it (frames back to back from the
 * start delay) and as the output times say.
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
#include <stdlib.h>

#include "libavcodec/avcodec.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discrip.h"

#define LOG_MAX  10              /* findings of one kind logged one by one */
#define GRID_TOL 1               /* ticks: grid times and durations are each truncated */

typedef struct VTime {
    int64_t time, dur;
} VTime;

struct DRVerify {
    void          *log;
    const DRCodec *codec;
    int            track, kind;
    DREventCb      event;  void *event_opaque;

    int            have_prev;
    int64_t        prev_time, prev_end;
    int64_t        es_pos;       /* audio: where the next frame starts in a stream file */

    VTime         *v;            /* video frames (display order is checked at the end) */
    int64_t       *ph;           /* video placeholders' times */
    int            nb_v, cap_v, nb_ph, cap_ph;

    int            logged[DR_EV_VERIFY_OVERLAP - DR_EV_VERIFY_UNIT + 1];
    DRVerifyStats  st;
};

static void finding(DRVerify *v, int kind, int64_t pos, int64_t dur, int64_t count, const char *fmt, ...)
{
    int *n = &v->logged[kind - DR_EV_VERIFY_UNIT];

    if (v->event)
        v->event(v->event_opaque, &(DREvent){ kind, v->track, pos, dur, 0, count });
    if (*n < LOG_MAX) {
        va_list ap;
        char buf[256];

        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        av_log(v->log, AV_LOG_WARNING, "Rip core: check: track %d (%s): %s%s\n", v->track, v->codec->name, buf,
               ++*n == LOG_MAX ? " (further ones of this kind are only counted)" : "");
    }
}

static double ms(int64_t t)
{
    return t / (DR_TICKS_PER_SECOND / 1000.0);
}

int ff_discrip_verify_open(DRVerify **vp, void *logctx, enum AVCodecID codec, int track, int kind,
                           DREventCb ev, void *ev_opaque)
{
    const DRCodec *c = ff_discrip_codec(codec);
    DRVerify *v;

    *vp = NULL;
    if (!c)
        return AVERROR(ENOSYS);
    if (!(v = av_mallocz(sizeof(*v))))
        return AVERROR(ENOMEM);
    v->log   = logctx;
    v->codec = c;
    v->track = track;
    v->kind  = kind;
    v->event = ev;
    v->event_opaque = ev_opaque;
    v->st.es_err_at = v->st.mkv_err_at = AV_NOPTS_VALUE;
    *vp = v;
    return 0;
}

/* Room for one more element in the array at *a. */
static int grow(void *a, int n, int *cap, size_t size)
{
    void **pa = a;

    if (n == *cap) {
        int c = *cap ? 2 * *cap : 1024;
        void *p = av_realloc_array(*pa, c, size);
        if (!p)
            return AVERROR(ENOMEM);
        *pa  = p;
        *cap = c;
    }
    return 0;
}

static void sync_error(DRVerify *v, const DRFrame *f)
{
    int64_t es = v->es_pos - f->src, mkv = f->time - f->src;

    if (v->st.es_err_at == AV_NOPTS_VALUE || FFABS(es) > FFABS(v->st.es_err_max)) {
        v->st.es_err_max = es;
        v->st.es_err_at  = f->src;
    }
    if (v->st.mkv_err_at == AV_NOPTS_VALUE || FFABS(mkv) > FFABS(v->st.mkv_err_max)) {
        v->st.mkv_err_max = mkv;
        v->st.mkv_err_at  = f->src;
    }
    v->st.es_err_end = es;
}

int ff_discrip_verify_frame(DRVerify *v, const DRFrame *f)
{
    int marker = !f->size && !f->dur;

    v->st.frames++;
    if (marker) {
        v->st.markers++;
        if (v->kind == DR_KIND_VIDEO) {
            int ret = grow(&v->ph, v->nb_ph, &v->cap_ph, sizeof(*v->ph));
            if (ret < 0)
                return ret;
            v->ph[v->nb_ph++] = f->time;
        }
        return 0;
    }
    /* the unit itself */
    if (!v->codec->verify)
        v->st.unchecked++;
    else {
        int r = v->codec->verify(f->data, f->size);
        if (r == DR_UNIT_BAD) {
            v->st.bad_units++;
            finding(v, DR_EV_VERIFY_UNIT, f->time, 0, 1, "the %d-byte unit at %.3f ms does not parse", f->size,
                    ms(f->time));
        } else if (r == DR_UNIT_CRC) {
            v->st.crc_errors++;
            finding(v, DR_EV_VERIFY_CRC, f->time, 0, 1, "the unit at %.3f ms fails its checksum", ms(f->time));
        }
    }
    if (f->dur <= 0) {
        v->st.order_errors++;
        finding(v, DR_EV_VERIFY_ORDER, f->time, f->dur, 1, "the unit at %.3f ms has no duration", ms(f->time));
    }
    if (v->kind == DR_KIND_VIDEO) {
        int ret = grow(&v->v, v->nb_v, &v->cap_v, sizeof(*v->v));
        if (ret < 0)
            return ret;
        v->v[v->nb_v++] = (VTime){ f->time, f->dur };
        return 0;
    }
    /* audio and subtitles: output order is time order */
    if (v->have_prev && f->time <= v->prev_time) {
        v->st.order_errors++;
        finding(v, DR_EV_VERIFY_ORDER, f->time, f->time - v->prev_time, 1, "the unit at %.3f ms does not come "
                "after the one before it (%.3f ms)", ms(f->time), ms(v->prev_time));
    } else if (v->have_prev && f->time < v->prev_end) {
        v->st.overlaps++;
        v->st.overlap_dur = FFMAX(v->st.overlap_dur, v->prev_end - f->time);
    }
    if (v->kind == DR_KIND_AUDIO) {
        if (!v->have_prev) {
            v->st.delay = f->time;
            v->es_pos   = f->time;
        }
        sync_error(v, f);
        v->es_pos += f->dur;
    }
    v->have_prev = 1;
    v->prev_time = f->time;
    v->prev_end  = f->time + f->dur;
    return 0;
}

static int vtime_cmp(const void *a, const void *b)
{
    const VTime *x = a, *y = b;
    return x->time < y->time ? -1 : x->time > y->time;
}

static int ph_cmp(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* Video in display order: each frame starts where the one before it ends. */
static void video_order(DRVerify *v)
{
    int p = 0;

    qsort(v->v, v->nb_v, sizeof(*v->v), vtime_cmp);
    qsort(v->ph, v->nb_ph, sizeof(*v->ph), ph_cmp);
    for (int i = 0; i + 1 < v->nb_v; i++) {
        int64_t end = v->v[i].time + v->v[i].dur, d = v->v[i + 1].time - end;
        int filled = 0;

        if (FFABS(d) <= GRID_TOL)
            continue;
        if (d > 0) {
            while (p < v->nb_ph && v->ph[p] < end)
                p++;
            while (p < v->nb_ph && v->ph[p] < v->v[i + 1].time) {
                p++;
                filled++;
            }
            v->st.holes++;
            v->st.hole_dur += d;
            finding(v, DR_EV_VERIFY_HOLE, end, d, filled, "no picture from %.3f ms for %.3f ms (%d placeholders)",
                    ms(end), ms(d), filled);
        } else {
            v->st.overlaps++;
            v->st.overlap_dur = FFMAX(v->st.overlap_dur, -d);
            finding(v, DR_EV_VERIFY_OVERLAP, v->v[i + 1].time, -d, 1, "the picture at %.3f ms starts %.3f ms "
                    "before the one before it ends", ms(v->v[i + 1].time), ms(-d));
        }
    }
}

int ff_discrip_verify_finish(DRVerify *v)
{
    const DRVerifyStats *s = &v->st;

    if (v->kind == DR_KIND_VIDEO)
        video_order(v);
    /* the summary goes to the debug log; every finding was a warning already */
    av_log(v->log, AV_LOG_VERBOSE,
           "Rip core: check: track %d (%s): %"PRId64" frames, %"PRId64" units that do not parse, %"PRId64
           " checksum failures, %"PRId64" time-order errors, %"PRId64" holes (%.3f ms), %"PRId64" overlaps "
           "(largest %.3f ms)%s\n", v->track, v->codec->name, s->frames - s->markers, s->bad_units, s->crc_errors,
           s->order_errors, s->holes, ms(s->hole_dur), s->overlaps, ms(s->overlap_dur),
           s->unchecked ? ", units not checked (no check for this codec)" : "");
    if (v->kind == DR_KIND_AUDIO && v->have_prev) {
        av_log(v->log, AV_LOG_VERBOSE, "Rip core: check: track %d (%s): start delay %.3f ms; sync error as a stream "
               "file %+.3f ms at its largest (at %.3f ms), %+.3f ms at the end; by the output times %+.3f ms at "
               "its largest (at %.3f ms)\n", v->track, v->codec->name, ms(s->delay), ms(s->es_err_max),
               ms(s->es_err_at), ms(s->es_err_end), ms(s->mkv_err_max), ms(s->mkv_err_at));
    }
    return 0;
}

void ff_discrip_verify_stats(const DRVerify *v, DRVerifyStats *st)
{
    *st = v->st;
}

void ff_discrip_verify_close(DRVerify **vp)
{
    DRVerify *v = *vp;

    if (!v)
        return;
    av_freep(&v->v);
    av_freep(&v->ph);
    av_freep(vp);
}
