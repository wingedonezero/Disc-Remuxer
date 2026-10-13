/*
 * The title timeline: Blu-ray text subtitles (HDMV TextST, read from a clip
 * of their own), as FFmpeg's HDMV parser cuts them in disc mode: one
 * segment per unit (libavcodec/hdmv_textst.h). The first unit must be the
 * dialog style segment: it styles every dialog and is the track's header
 * (without the 2 bytes after it on the disc, the number of dialogs). Each
 * dialog presentation segment is one frame: its time is its unit's (the
 * PES time; the title reader gives a PES without one the dialog's start),
 * its duration the dialog's end minus its start. A forced-only track keeps
 * only what a player shows with subtitles off: the regions with
 * forced_on_flag (a dialog without one is left out). Whatever is left out
 * is said.
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
#include "parse/hdmv_textst.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "disctl.h"

struct DRTextst {
    void       *log;
    int         track;
    int         forced_only;
    const char *where;              /* the clip, for the log */
    DRFrameCb   cb;
    void       *opaque;
    uint8_t    *style;              /* the dialog style segment (without the dialog count) */
    int         style_size;
    int         failed;             /* the stream does not start with a style segment: nothing is given out */
    DRTextstStats st;
};

int ff_disctl_textst_anchor(const uint8_t *data, int size)
{
    return size > 0 && data[0] == HDMV_TEXTST_DPS ? 0 : -1;
}

int ff_disctl_textst_check(const uint8_t *data, int size)
{
    return size >= 3 && (data[0] == HDMV_TEXTST_DSS || data[0] == HDMV_TEXTST_DPS) &&
           ff_hdmv_textst_segment_size(data, size) == size;
}

int ff_disctl_textst_verify(const uint8_t *data, int size)
{
    HDMVTextstDialog d;

    if (!ff_disctl_textst_check(data, size))
        return DR_UNIT_BAD;
    return data[0] != HDMV_TEXTST_DPS || ff_hdmv_textst_dialog(data, size, &d) ? DR_UNIT_OK : DR_UNIT_BAD;
}

int ff_disctl_textst_forced_only(const uint8_t *in, int size, uint8_t *out, int *kept)
{
    HDMVTextstDialog d;
    int o;

    *kept = 0;
    if (!ff_hdmv_textst_dialog(in, size, &d))
        return AVERROR_INVALIDDATA;
    /* the fields up to number_of_regions, then the forced regions; what
     * follows the regions (none in the format) stays */
    memcpy(out, in, d.regions_at);
    o = d.regions_at + 1;
    for (int r = 0; r < d.nb_regions; r++)
        if (in[d.region_at[r]] & HDMV_TEXTST_FORCED_ON) {
            memcpy(out + o, in + d.region_at[r], d.region_size[r]);
            o += d.region_size[r];
            (*kept)++;
        }
    {
        int tail = d.region_at[d.nb_regions - 1] + d.region_size[d.nb_regions - 1];
        memcpy(out + o, in + tail, d.size - tail);
        o += d.size - tail;
    }
    out[d.regions_at] = *kept;
    AV_WB16(out + 1, o - 3);
    return o;
}

int ff_disctl_textst_open(DRTextst **x, void *logctx, int track, int forced_only, const char *where,
                          DRFrameCb cb, void *opaque)
{
    DRTextst *t = av_mallocz(sizeof(*t));

    *x = t;
    if (!t)
        return AVERROR(ENOMEM);
    t->log         = logctx;
    t->track       = track;
    t->forced_only = forced_only;
    t->where       = where ? where : "its clip";
    t->cb          = cb;
    t->opaque      = opaque;
    return 0;
}

static int style_unit(DRTextst *t, const DRUnit *u)
{
    int n = u->size - 2;            /* the dialog count after it is no part of it */

    if (!(t->style = av_memdup(u->data, n)))
        return AVERROR(ENOMEM);
    t->style_size = n;
    av_log(t->log, AV_LOG_VERBOSE, "Title timeline: TextST track %d: dialog style segment of %d bytes; the disc "
           "counts %d dialogs\n", t->track, n, AV_RB16(u->data + n));
    t->st.disc_count = AV_RB16(u->data + n);
    return 0;
}

int ff_disctl_textst_unit(void *opaque, const DRUnit *u)
{
    DRTextst *t = opaque;
    HDMVTextstDialog d;
    DRFrame f = { .time = u->time, .pos = u->pos, .flags = DR_F_KEY };
    int n, kept = 0;

    t->st.units++;
    if (t->failed) {
        t->st.left_out++;
        return 0;
    }
    if (!t->style) {
        if (u->data[0] == HDMV_TEXTST_DSS)
            return style_unit(t, u);
        av_log(t->log, AV_LOG_WARNING, "Title timeline: TextST track %d: %s does not start with a dialog style "
               "segment (its first segment, at stream offset %"PRId64", has type 0x%02x): the track has no "
               "subtitles\n", t->track, t->where, u->pos, u->data[0]);
        t->failed = 1;
        t->st.left_out++;
        t->st.warnings++;
        return 0;
    }
    if (u->data[0] != HDMV_TEXTST_DPS) {
        av_log(t->log, AV_LOG_WARNING, "Title timeline: TextST track %d: %s: %s at stream offset %"PRId64": left "
               "out\n", t->track, t->where, u->data[0] == HDMV_TEXTST_DSS ? "a second dialog style segment (the "
               "first one styles every dialog)" : "a segment of another type", u->pos);
        t->st.left_out++;
        t->st.warnings++;
        return 0;
    }
    t->st.dialogs++;
    if (!ff_hdmv_textst_dialog(u->data, u->size, &d)) {
        av_log(t->log, AV_LOG_ERROR, "Title timeline: TextST track %d: a dialog the parser passed does not parse "
               "(%s)\n", t->track, d.error);
        return AVERROR_BUG;
    }
    if (u->time == AV_NOPTS_VALUE || u->time < 0) {
        av_log(t->log, AV_LOG_WARNING, "Title timeline: TextST track %d: %s: the dialog at stream offset %"PRId64
               " has no time: left out\n", t->track, t->where, u->pos);
        t->st.left_out++;
        t->st.warnings++;
        return 0;
    }
    if (d.end < d.start) {
        av_log(t->log, AV_LOG_WARNING, "Title timeline: TextST track %d: %s: the dialog at stream offset %"PRId64
               " ends (PTS %"PRId64") before it starts (PTS %"PRId64"): left out\n", t->track, t->where, u->pos,
               d.end, d.start);
        t->st.left_out++;
        t->st.warnings++;
        return 0;
    }
    t->st.forced += d.forced;
    if (t->forced_only && !d.forced) {
        t->st.not_forced++;
        return 0;
    }
    if (!(f.buf = av_buffer_alloc(d.size + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);
    if (t->forced_only) {
        if ((n = ff_disctl_textst_forced_only(u->data, d.size, f.buf->data, &kept)) < 0) {
            ff_disctl_frame_unref(&f);
            return AVERROR_BUG;
        }
        t->st.regions_cut += d.nb_regions - kept;
    } else {
        memcpy(f.buf->data, u->data, d.size);
        n = d.size;
    }
    memset(f.buf->data + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    f.data = f.buf->data;
    f.size = n;
    f.dur  = (d.end - d.start) * DR_TICKS_PER_PTS;
    t->st.out++;
    return t->cb(t->opaque, &f);
}

const uint8_t *ff_disctl_textst_style(const DRTextst *t, int *size)
{
    *size = t ? t->style_size : 0;
    return t ? t->style : NULL;
}

void ff_disctl_textst_stats(const DRTextst *t, DRTextstStats *st)
{
    *st = t->st;
}

void ff_disctl_textst_close(DRTextst **x)
{
    if (!*x)
        return;
    av_freep(&(*x)->style);
    av_freep(x);
}
