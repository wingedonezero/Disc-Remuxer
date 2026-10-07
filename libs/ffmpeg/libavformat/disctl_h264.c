/*
 * The title timeline: the rules for H.264 / AVC (ITU-T H.264, Annex B byte
 * stream) pictures.
 *
 * FFmpeg's H.264 parser cuts the access units in disc mode and gives each
 * picture's values (h264_parser.c): its type (the first slice's
 * slice_type), whether it is a random access point, its picture order count
 * (the display order, period 2^28) and its duration in field periods. The
 * key frames are the random access points: IDR pictures and pictures with a
 * recovery point SEI (an I picture without one is no key frame: pictures
 * after it may refer to pictures before it).
 *
 * A frame keeps the NAL units of its access unit as they are on the disc,
 * each with its start code (a 00 00 00 01 start code stays one), access unit
 * delimiters included, except what is no part of a picture: filler data
 * (12), end of sequence (10), end of stream (11), the nal_unit_types that
 * start no unit (0, 16-18, 21-23, 25-27, 29, 31) and zero stuffing
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

typedef struct H264State {
    int64_t dropped[32];       /* NAL units taken out, per nal_unit_type */
    int64_t stuffing;          /* zero bytes taken out */
    int     said_field;        /* the untested note on field pictures was logged */
} H264State;

/* Whether a NAL unit of type t leaves the frame. */
static int dropped(int t)
{
    return (t >= 10 && t <= 12) || !t || (t >= 16 && t <= 18) || (t >= 21 && t <= 23) ||
           (t >= 25 && t <= 27) || t == 29 || t == 31;
}

/* The next start code 00 00 01 at or after i, or -1. */
static int next_sc(const uint8_t *p, int n, int i)
{
    for (; i + 2 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1)
            return i;
    return -1;
}

/* Rebuild the unit without the NAL units that leave it and without zero
 * stuffing; each kept unit keeps its own start code (with one zero byte in
 * front when the disc has one or more there). */
static int split_h264(DRVideo *v, DRFrame *f)
{
    H264State *s = ff_disctl_video_priv(v);
    const uint8_t *p = f->data;
    int n = f->size, i = next_sc(p, n, 0), changed = 0, out = 0;
    AVBufferRef *buf;

    if (i < 0)
        return 0;                       /* no NAL unit: picture() leaves it out */
    if (!(buf = av_buffer_alloc(n + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);
    while (i >= 0) {
        int zeros = 0, t = i + 3 < n ? p[i + 3] & 0x1f : -1, e = next_sc(p, n, i + 3), end;

        for (int k = i; k > 0 && !p[k - 1]; k--)
            zeros++;
        end = e < 0 ? n : e;
        while (end > i + 3 && !p[end - 1])
            end--;                      /* zero bytes go with the next start code */
        if (t < 0 || dropped(t)) {
            if (t >= 0)
                s->dropped[t]++;
            changed = 1;
        } else {
            int z = zeros ? 1 : 0;
            if (zeros > 1) {
                s->stuffing += zeros - 1;
                changed = 1;
            }
            memset(buf->data + out, 0, z);
            memcpy(buf->data + out + z, p + i, end - i);
            out += z + end - i;
        }
        i = e;
    }
    if (!changed || out == n) {
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

/* The picture values come from the parser; a unit without a slice is no
 * picture. */
static int picture_h264(DRVideo *v, const DRFrame *f, DRPicture *pic)
{
    H264State *s = ff_disctl_video_priv(v);

    if (pic->rate_num > 0 && pic->rate_den > 0)
        ff_disctl_video_set_rate(v, pic->rate_num, pic->rate_den);
    if (pic->fields <= 0 || pic->type < DR_PIC_I || pic->type > DR_PIC_B) {
        pic->type = DR_PIC_OTHER;
        return 0;
    }
    if (pic->fields == 1 && !s->said_field) {
        s->said_field = 1;
        av_log(NULL, AV_LOG_INFO, "Title timeline: H.264: field pictures: the two fields of a frame are joined "
               "into one frame [untested on real discs]\n");
    }
    return 0;
}

static void close_h264(void *priv)
{
    const H264State *s = priv;
    static const char *const names[32] = { [10] = "ends of sequence",
                                           [11] = "ends of stream", [12] = "filler data units" };

    for (int t = 0; t < 32; t++)
        if (s->dropped[t] && names[t])
            av_log(NULL, AV_LOG_VERBOSE, "Title timeline: H.264: %"PRId64" %s taken out of the frames\n",
                   s->dropped[t], names[t]);
    for (int t = 0; t < 32; t++)
        if (s->dropped[t] && !names[t])
            av_log(NULL, AV_LOG_WARNING, "Title timeline: H.264: %"PRId64" NAL units of type %d (they start no "
                   "unit) taken out of the frames [untested on real discs]\n", s->dropped[t], t);
    if (s->stuffing)
        av_log(NULL, AV_LOG_VERBOSE, "Title timeline: H.264: %"PRId64" zero stuffing bytes taken out\n",
               s->stuffing);
}

const DRVideoRules ff_disctl_video_h264 = {
    .picture      = picture_h264,
    .split        = split_h264,
    .close        = close_h264,
    .order_period = 0x10000000,
    .codec_keys   = 1,
    .priv_size    = sizeof(H264State),
};
