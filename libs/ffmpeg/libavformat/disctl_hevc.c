/*
 * The title timeline: the rules for HEVC (ITU-T H.265, Annex B byte stream)
 * pictures.
 *
 * FFmpeg's HEVC parser cuts the access units in disc mode and gives each
 * picture's values (hevc/parser.c): its type (the first slice segment's
 * slice_type), whether it is a random access point, its picture order count
 * (the display order) and its duration in field periods. The key frames
 * are the random access points: IRAP pictures (IDR, CRA, BLA) and pictures
 * with a recovery point SEI.
 *
 * A frame keeps the NAL units of its access unit as they are on the disc,
 * each with its start code (a 00 00 00 01 start code stays one), access unit
 * delimiters included, except what is no part of a picture: end of sequence
 * (36), end of bitstream (37), filler data (38), the reserved nal_unit_types
 * (10-15, 22-31, 41-61), units of another layer (nuh_layer_id other than 0),
 * units too short for their 2-byte header and zero stuffing
 * (trailing_zero_8bits). A stream file and a Matroska file of a title carry
 * the same NAL units.
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
#include "libavutil/buffer.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "disctl.h"

typedef struct HEVCState {
    DRHevcDropped dropped;      /* what was taken out */
    int     said_field;         /* the untested note on field pictures was logged */
} HEVCState;

/* Whether a NAL unit of type t leaves the frame. */
static int dropped(int t)
{
    return (t >= 10 && t <= 15) || (t >= 22 && t <= 31) || (t >= 36 && t <= 38) || (t >= 41 && t <= 61);
}

/* The next start code 00 00 01 at or after i, or -1. */
static int next_sc(const uint8_t *p, int n, int i)
{
    for (; i + 2 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1)
            return i;
    return -1;
}

int ff_disctl_hevc_clean(const uint8_t *p, int n, uint8_t *out, DRHevcDropped *d, int *changed)
{
    int i = next_sc(p, n, 0), o = 0;

    *changed = 0;
    while (i >= 0) {
        int zeros = 0, e = next_sc(p, n, i + 3), end, t, layer;

        for (int k = i; k > 0 && !p[k - 1]; k--)
            zeros++;
        end = e < 0 ? n : e;
        while (end > i + 3 && !p[end - 1])
            end--;                      /* zero bytes go with the next start code */
        t     = end - i - 3 >= 1 ? (p[i + 3] >> 1) & 0x3f : -1;
        layer = end - i - 3 >= 2 ? (p[i + 3] & 1) << 5 | p[i + 4] >> 3 : -1;
        if (layer < 0 || layer || dropped(t)) {
            if (layer < 0)
                d->short_units++;
            else if (layer)
                d->other_layer++;
            else
                d->units[t]++;
            *changed = 1;
        } else {
            int z = zeros ? 1 : 0;
            if (zeros > 1) {
                d->stuffing += zeros - 1;
                *changed = 1;
            }
            memset(out + o, 0, z);
            memcpy(out + o + z, p + i, end - i);
            o += z + end - i;
        }
        i = e;
    }
    return o;
}

void ff_disctl_hevc_dropped_log(const DRHevcDropped *d, const char *what)
{
    static const char *const names[64] = { [36] = "ends of sequence", [37] = "ends of bitstream",
                                           [38] = "filler data units" };

    for (int t = 0; t < 64; t++)
        if (d->units[t] && names[t])
            av_log(NULL, AV_LOG_VERBOSE, "Title timeline: %s: %"PRId64" %s taken out of the frames\n", what,
                   d->units[t], names[t]);
    for (int t = 0; t < 64; t++)
        if (d->units[t] && !names[t])
            av_log(NULL, AV_LOG_WARNING, "Title timeline: %s: %"PRId64" NAL units of the reserved type %d taken "
                   "out of the frames [untested on real discs]\n", what, d->units[t], t);
    if (d->other_layer)
        av_log(NULL, AV_LOG_WARNING, "Title timeline: %s: %"PRId64" NAL units of another layer (nuh_layer_id "
               "not 0) taken out of the frames [untested on real discs]\n", what, d->other_layer);
    if (d->short_units)
        av_log(NULL, AV_LOG_WARNING, "Title timeline: %s: %"PRId64" NAL units too short for their header taken "
               "out of the frames [untested on real discs]\n", what, d->short_units);
    if (d->stuffing)
        av_log(NULL, AV_LOG_VERBOSE, "Title timeline: %s: %"PRId64" zero stuffing bytes taken out\n", what,
               d->stuffing);
}

/* Rebuild the unit without the NAL units that leave it and without zero
 * stuffing (ff_disctl_hevc_clean). */
static int split_hevc(DRVideo *v, DRFrame *f)
{
    HEVCState *s = ff_disctl_video_priv(v);
    AVBufferRef *buf;
    int out, changed;

    if (next_sc(f->data, f->size, 0) < 0)
        return 0;                       /* no NAL unit: picture() leaves it out */
    if (!(buf = av_buffer_alloc(f->size + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);
    out = ff_disctl_hevc_clean(f->data, f->size, buf->data, &s->dropped, &changed);
    if (!changed || out == f->size) {
        av_buffer_unref(&buf);
        return 0;
    }
    memset(buf->data + out, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    av_buffer_unref(&f->buf);
    f->buf  = buf;
    f->data = buf->data;
    f->size = out;
    return 0;
}

/* The picture values come from the parser; a unit without a slice segment
 * is no picture. */
static int picture_hevc(DRVideo *v, const DRFrame *f, DRPicture *pic)
{
    HEVCState *s = ff_disctl_video_priv(v);

    if (pic->rate_num > 0 && pic->rate_den > 0)
        ff_disctl_video_set_rate(v, pic->rate_num, pic->rate_den);
    if (pic->fields <= 0 || pic->type < DR_PIC_I || pic->type > DR_PIC_B) {
        pic->type = DR_PIC_OTHER;
        return 0;
    }
    if (pic->fields == 1 && !s->said_field) {
        s->said_field = 1;
        av_log(NULL, AV_LOG_INFO, "Title timeline: HEVC: field pictures: the two fields of a frame are joined "
               "into one frame [untested on real discs]\n");
    }
    return 0;
}

static void close_hevc(void *priv)
{
    const HEVCState *s = priv;

    ff_disctl_hevc_dropped_log(&s->dropped, "HEVC");
}

const DRVideoRules ff_disctl_video_hevc = {
    .picture      = picture_hevc,
    .split        = split_hevc,
    .close        = close_hevc,
    .order_period = 0x10000000,
    .codec_keys   = 1,
    .priv_size    = sizeof(HEVCState),
};
