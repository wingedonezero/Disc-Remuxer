/*
 * The title timeline: sub-picture units (DVD-Video and HD DVD), as FFmpeg's
 * DVD subtitle parser finds them in disc mode: each unit's check, its
 * duration from its display control sequences, and its time on the video
 * grid of its segment. Blu-ray PGS display sets, as FFmpeg's HDMV PGS
 * parser finds them in disc mode, the same way: each set lasts one video
 * field period (the next set replaces or clears it); a forced-only track's
 * sets show only their forced objects (disctl_pgs.c).
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

#define DELAY_TICKS 12288000LL   /* one SP_DCSQ_STM step: 1024 / 90000 s */
#define MAX_DCSQ    40000        /* control sequences walked per unit */

/* ---- the unit ----
 * DVD-Video: SPDSZ (16 bits, the unit's size) and SP_DCSQTA (16 bits, the
 * offset of the first display control sequence). HD DVD: a zero 16-bit word,
 * then both as 32 bits. A display control sequence SP_DCSQ: SP_DCSQ_STM (16
 * bits, delay), SP_NXT_DCSQ_SA (16 or 32 bits), commands up to CMD_END
 * (0xFF); the last sequence points to itself. */

typedef struct Header {
    uint32_t size, dcsq;
    int      wide;               /* the 32-bit form */
} Header;

typedef struct Control {
    int stop;                    /* SP_DCSQ_STM of the sequence with STP_DSP, 0 = none */
    int dspxa;                   /* SET_DSPXA seen: the unit shows a picture */
    int forced;                  /* FSTA_DSP seen */
    int colcon;                  /* CHG_COLCON seen */
    int area;                    /* SET_DAREA seen: x2 / y2 the largest end column / line */
    int x2, y2;
} Control;

/* 1 when the header can be read from avail bytes. */
static int header(const uint8_t *b, int64_t avail, Header *h)
{
    if (avail < 4)
        return 0;
    if (dr_rb16(b)) {
        *h = (Header){ dr_rb16(b), dr_rb16(b + 2), 0 };
        return 1;
    }
    if (avail < 10)
        return 0;
    *h = (Header){ dr_rb32(b + 2), dr_rb32(b + 6), 1 };
    return 1;
}

/* Walks the display control sequences of a unit of len bytes; 0 when they
 * do not parse: a command that is not one of the known ones, a command or an
 * address beyond the unit, a sequence without CMD_END, a chain longer than
 * MAX_DCSQ. Command lengths: FSTA_DSP / STA_DSP / STP_DSP 1, SET_COLOR /
 * SET_CONTR 3, SET_DAREA 7, SET_DSPXA 5 (both field addresses inside the
 * unit), CHG_COLCON its 16-bit size + 3; the HD DVD commands SET_COLOR 0x83
 * 769, SET_CONTR 0x84 257, SET_DAREA 0x85 7, SET_DSPXA 0x86 9 (32-bit
 * addresses inside the unit), CHG_COLCON 0x87 as 0x07. STP_DSP takes its
 * sequence's delay (the last one counts). SET_DAREA: start and end column
 * (12 bits each), start and end line (12 bits each). */
static int control(const uint8_t *b, int64_t len, Control *c)
{
    Header h;
    int64_t cur;

    memset(c, 0, sizeof(*c));
    if (!header(b, len, &h))
        return 0;
    cur = h.dcsq;
    for (int n = 0;; ) {
        int64_t next, p;

        if (cur + 5 > len)
            return 0;
        next = h.wide ? dr_rb32(b + cur + 2) : dr_rb16(b + cur + 2);
        p    = cur + (h.wide ? 6 : 4);
        if (p >= len)
            return 0;
        while (b[p] != 0xFF) {
            int64_t k;

            switch (b[p]) {
            case 0x00: c->forced = 1; k = 1; break;
            case 0x01:                k = 1; break;
            case 0x02: c->stop = dr_rb16(b + cur); k = 1; break;
            case 0x03:
            case 0x04:                k = 3; break;
            case 0x05:
            case 0x85:
                if (len - p < 7)
                    return 0;
                c->area = 1;
                c->x2 = DR_MAX(c->x2, (b[p + 2] & 0x0F) << 8 | b[p + 3]);
                c->y2 = DR_MAX(c->y2, (b[p + 5] & 0x0F) << 8 | b[p + 6]);
                k = 7;
                break;
            case 0x06:
                c->dspxa = 1;
                if (len - p < 5 || dr_rb16(b + p + 1) >= len || dr_rb16(b + p + 3) >= len)
                    return 0;
                k = 5;
                break;
            case 0x86:
                c->dspxa = 1;
                if (len - p < 9 || dr_rb32(b + p + 1) >= len || dr_rb32(b + p + 5) >= len)
                    return 0;
                k = 9;
                break;
            case 0x07:
            case 0x87:
                c->colcon = 1;
                if (len - p < 3 || dr_rb16(b + p + 1) < 2)
                    return 0;
                k = dr_rb16(b + p + 1) + 3;
                break;
            case 0x83:                k = 0x301; break;
            case 0x84:                k = 0x101; break;
            default:
                return 0;
            }
            p += k;
            if (p >= len)
                return 0;
        }
        if (next == cur)
            return 1;
        cur = next;
        if (++n == MAX_DCSQ)
            return 0;
    }
}

/* A unit whose display control sequences do not parse is left out. */
int ff_disctl_spu_check(const uint8_t *data, int size)
{
    Control c;

    return control(data, size, &c);
}

/* An output unit: as long as its header says, its control sequences
 * parse. */
int ff_disctl_spu_verify(const uint8_t *data, int size)
{
    Header h;

    if (!header(data, size, &h) || h.size != size)
        return DR_UNIT_BAD;
    return ff_disctl_spu_check(data, size) ? DR_UNIT_OK : DR_UNIT_BAD;
}

/* ---- timing ---- */

typedef struct Held {
    DRFrame f;
    int64_t pes;                 /* the PES time before the snap */
} Held;

struct DRSpu {
    void       *log;
    int         track;
    int         width, height;   /* the canvas the units are drawn on, 0 = not known */
    DRVideo    *video;
    DRFrameCb   cb;    void *opaque;
    DREventCb   event; void *event_opaque;
    Held       *q;               /* units waiting for the video's grid */
    int         head, n, cap;
    int         warned_colcon, warned_outside;
    int         pgs, forced_only, said_forced;
    DRSpuStats  st;
};

static void event(DRSpu *s, int kind, int64_t pos, int64_t dur)
{
    if (s->event)
        s->event(s->event_opaque, &(DREvent){ kind, s->track, pos, dur, 0, 1 });
}

/* Hands on the waiting units once the video's grid is known: each time
 * becomes the nearest grid point; a unit with no grid point (more than
 * 0.1 ms before the video's first field) is left out. */
static int drain(DRSpu *s)
{
    while (s->n) {
        Held *h = &s->q[s->head];
        int64_t t;
        int ret = ff_disctl_video_snap(s->video, h->pes, &t);

        if (ret == DR_ERR(EAGAIN))
            return 0;
        if (ret < 0)
            return ret;
        s->head++;
        s->n--;
        if (t == DR_NOPTS_VALUE || t < 0) {
            dr_vlog(s->log, DR_VLOG_WARNING, "Title timeline: sub-picture track %d: a unit at %"PRId64" ticks is before "
                   "the video's first field: left out\n", s->track, h->pes);
            event(s, DR_EV_SUB_EARLY, h->pes, h->f.dur);
            s->st.early++;
            ff_disctl_frame_unref(&h->f);
            continue;
        }
        s->st.max_shift = DR_MAX(s->st.max_shift, DR_ABS(t - h->pes));
        h->f.time = t;
        if (s->pgs && (ret = ff_disctl_video_field(s->video, &h->f.dur)) < 0)
            return ret;
        s->st.out++;
        if ((ret = s->cb(s->opaque, &h->f)) < 0)
            return ret;
    }
    s->head = 0;
    return 0;
}

/* Room for one more held unit. */
static int hold(DRSpu *s, Held *h)
{
    if (s->head && s->head + s->n == s->cap) {
        memmove(s->q, s->q + s->head, s->n * sizeof(*s->q));
        s->head = 0;
    }
    if (s->head + s->n == s->cap) {
        int cap = s->cap ? 2 * s->cap : 16;
        Held *q = dr_realloc_array(s->q, cap, sizeof(*q));
        if (!q)
            return DR_ERR(ENOMEM);
        s->q   = q;
        s->cap = cap;
    }
    s->q[s->head + s->n++] = *h;
    return 0;
}

/* A PGS display set: its bytes (a forced-only track: cut to its forced
 * objects), its time on the video grid once it is known. */
static int pgs_unit(DRSpu *s, const DRUnit *u)
{
    Held h = { .f = { .time = u->time, .pos = u->pos, .flags = DR_F_KEY }, .pes = u->time };
    int n, forced, ret;

    s->st.units++;
    if (!ff_disctl_pgs_check(u->data, u->size)) {
        dr_vlog(s->log, DR_VLOG_ERROR, "Title timeline: PGS track %d: a display set the parser passed does not parse\n",
               s->track);
        return DR_ERR_BUG;
    }
    if (u->time == DR_NOPTS_VALUE || u->time < 0) {
        dr_vlog(s->log, DR_VLOG_WARNING, "Title timeline: PGS track %d: the display set at byte %"PRId64" has no time: "
               "left out\n", s->track, u->pos);
        event(s, DR_EV_SUB_UNTIMED, u->pos, 0);
        s->st.untimed++;
        return 0;
    }
    if (!(h.f.buf = dr_buf_alloc(u->size + DR_BUF_PADDING)))
        return DR_ERR(ENOMEM);
    n = ff_disctl_pgs_forced_only(u->data, u->size, dr_buf_data(h.f.buf), &forced);
    if (n < 0) {
        dr_vlog(s->log, DR_VLOG_ERROR, "Title timeline: PGS track %d: the composition of the display set at byte %"PRId64
               " does not parse\n", s->track, u->pos);
        ff_disctl_frame_unref(&h.f);
        return n;
    }
    s->st.forced_objects += forced;
    if (s->forced_only) {
        if (forced && !s->said_forced) {
            s->said_forced = 1;
            dr_vlog(s->log, DR_VLOG_INFO, "Title timeline: PGS track %d: forced subtitles: each display set shows only "
                   "its forced objects, as a player does with subtitles off [untested on real discs]\n", s->track);
        }
    } else {
        memcpy(dr_buf_data(h.f.buf), u->data, u->size);
        n = u->size;
    }
    memset(dr_buf_data(h.f.buf) + n, 0, DR_BUF_PADDING);
    h.f.data = dr_buf_data(h.f.buf);
    h.f.size = n;
    if ((ret = hold(s, &h)) < 0) {
        ff_disctl_frame_unref(&h.f);
        return ret;
    }
    return drain(s);
}

void ff_disctl_spu_set_pgs(DRSpu *s, int forced_only)
{
    s->pgs         = 1;
    s->forced_only = forced_only;
}

int ff_disctl_spu_unit(void *opaque, const DRUnit *u)
{
    DRSpu *s = opaque;
    Control c;
    Held h = { .f = { .time = u->time, .pos = u->pos, .flags = DR_F_KEY }, .pes = u->time };
    int ret;

    if (s->pgs)
        return pgs_unit(s, u);
    s->st.units++;
    if (!control(u->data, u->size, &c)) {
        dr_vlog(s->log, DR_VLOG_ERROR, "Title timeline: sub-picture track %d: a unit the parser passed does not parse\n",
               s->track);
        return DR_ERR_BUG;
    }
    if (c.colcon && !s->warned_colcon) {
        dr_vlog(s->log, DR_VLOG_WARNING, "Title timeline: sub-picture track %d: a unit changes colour and contrast "
               "per line (CHG_COLCON) [untested on real discs]\n", s->track);
        s->warned_colcon = 1;
    }
    s->st.colcon += c.colcon;
    if (s->width && c.area && (c.x2 >= s->width || c.y2 >= s->height)) {
        if (!s->warned_outside)
            dr_vlog(s->log, DR_VLOG_WARNING, "Title timeline: sub-picture track %d: the unit at byte %"PRId64" is drawn "
                   "up to column %d, line %d, outside the track's %dx%d canvas\n", s->track, u->pos, c.x2, c.y2,
                   s->width, s->height);
        s->warned_outside = 1;
        s->st.outside++;
    }
    s->st.forced += c.forced;
    h.f.dur = (c.stop ? c.stop : 1) * DELAY_TICKS;
    if (c.dspxa && !c.stop) {
        h.f.flags |= DR_F_NO_STOP;
        s->st.no_stop++;
    }
    if (u->time == DR_NOPTS_VALUE || u->time < 0) {
        dr_vlog(s->log, DR_VLOG_WARNING, "Title timeline: sub-picture track %d: the unit at byte %"PRId64" has no time "
               "of its own (not the first unit of its PES packet): left out\n", s->track, u->pos);
        event(s, DR_EV_SUB_UNTIMED, u->pos, h.f.dur);
        s->st.untimed++;
        return 0;
    }
    if (!(h.f.buf = dr_buf_alloc(u->size + DR_BUF_PADDING)))
        return DR_ERR(ENOMEM);
    memcpy(dr_buf_data(h.f.buf), u->data, u->size);
    memset(dr_buf_data(h.f.buf) + u->size, 0, DR_BUF_PADDING);
    h.f.data = dr_buf_data(h.f.buf);
    h.f.size = u->size;
    if ((ret = hold(s, &h)) < 0) {
        ff_disctl_frame_unref(&h.f);
        return ret;
    }
    return drain(s);
}

int ff_disctl_spu_flush(DRSpu *s)
{
    int ret = drain(s);

    if (ret < 0)
        return ret;
    if (s->n) {
        dr_vlog(s->log, DR_VLOG_ERROR, "Title timeline: sub-picture track %d: %d units, but the segment's video has "
               "no grid to time them\n", s->track, s->n);
        return DR_ERR_INVALID;
    }
    dr_vlog(s->log, DR_VLOG_DEBUG, "Title timeline: sub-picture track %d: %"PRId64" units -> %"PRId64" out (%"PRId64
           " without a time, %"PRId64" before the video, %"PRId64" without a stop time), largest move to the "
           "video grid %"PRId64" ticks\n", s->track, s->st.units, s->st.out, s->st.untimed, s->st.early,
           s->st.no_stop, s->st.max_shift);
    if (s->width)
        dr_vlog(s->log, DR_VLOG_VERBOSE, "Title timeline: sub-picture track %d: units inside the %dx%d canvas: "
               "%"PRId64" of %"PRId64": %s\n", s->track, s->width, s->height, s->st.units - s->st.outside,
               s->st.units, s->st.outside ? "FAIL" : "PASS");
    return 0;
}

int ff_disctl_spu_open(DRSpu **sp, void *logctx, int track, DRVideo *video, int width, int height,
                        DRFrameCb cb, void *opaque, DREventCb ev, void *ev_opaque)
{
    DRSpu *s;

    *sp = NULL;
    if (!video)
        return DR_ERR(EINVAL);
    if (!(s = dr_mallocz(sizeof(*s))))
        return DR_ERR(ENOMEM);
    s->log    = logctx;
    s->track  = track;
    s->width  = width;
    s->height = height;
    s->video  = video;
    s->cb     = cb;
    s->opaque = opaque;
    s->event  = ev;
    s->event_opaque = ev_opaque;
    *sp = s;
    return 0;
}

void ff_disctl_spu_stats(const DRSpu *s, DRSpuStats *st)
{
    *st = s->st;
}

void ff_disctl_spu_close(DRSpu **sp)
{
    DRSpu *s = *sp;

    if (!s)
        return;
    for (int i = 0; i < s->n; i++)
        ff_disctl_frame_unref(&s->q[s->head + i].f);
    dr_freep(&s->q);
    dr_freep(sp);
}

/* A palette entry 0x00 Y Cr Cb as RGB, the way the reference converts it:
 * Y scaled from 16..235 (integer division), 16.16 fixed-point products, each
 * sum clamped to 0..0xff0000; green takes 0.714 x Cb and 0.346 x Cr (the
 * reverse of ITU-R BT.601, which takes 0.344 x Cb and 0.714 x Cr). */
static uint32_t ycrcb_to_rgb(uint32_t e)
{
    int y = (e >> 16) & 0xff, cr = ((e >> 8) & 0xff) - 128, cb = (e & 0xff) - 128;
    int32_t yf = (255 * (y - 16) / 219) * 65536;
    int32_t v[3] = { yf + 91894 * cr, yf - 46825 * cb - 22655 * cr, yf + 116064 * cb };

    for (int i = 0; i < 3; i++)
        v[i] = v[i] <= 0 ? 0 : v[i] >= 0xff0000 ? 0xff0000 : v[i];
    return (uint32_t)(v[0] >> 16) << 16 | (uint32_t)(v[1] >> 16) << 8 | (uint32_t)(v[2] >> 16);
}

int ff_disctl_vobsub_header(char *buf, int size, int width, int height, const uint32_t *ycrcb)
{
    uint32_t p[16];

    for (int i = 0; i < 16; i++)
        p[i] = ycrcb ? ycrcb_to_rgb(ycrcb[i]) : 0;
    return snprintf(buf, size,
                    "# VobSub index file, v7 (do not modify this line!)\n"
                    "#\n"
                    "# Written by disc-remuxer.\n"
                    "#\n"
                    "size: %ux%u\n"
                    "org: 0, 0\n"
                    "alpha: 100%%\n"
                    "smooth: OFF\n"
                    "fadein/out: 50, 50\n"
                    "align: OFF at LEFT TOP\n"
                    "time offset: 0\n"
                    "forced subs: OFF\n"
                    "langidx: 0\n"
                    "palette: %06x, %06x, %06x, %06x, %06x, %06x, %06x, %06x, "
                    "%06x, %06x, %06x, %06x, %06x, %06x, %06x, %06x\n"
                    "#\n",
                    width, height, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                    p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
}
