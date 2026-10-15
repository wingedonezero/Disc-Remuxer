/*
 * The title timeline: the rules for HEVC (ITU-T H.265, Annex B byte stream)
 * pictures.
 *
 * The HEVC rule (src/parse/hevc_rule.c) cuts the access units and gives each
 * picture's values (hevc/parser.c): its type (the first slice segment's
 * slice_type), whether it is a random access point, its picture order count
 * (the display order) and its duration in field periods. The key frames
 * are the random access points: IRAP pictures (IDR, CRA, BLA) and pictures
 * with a recovery point SEI.
 *
 * A frame keeps the NAL units of its access unit as they are on the disc,
 * each with its start code (a 00 00 00 01 start code stays one), access unit
 * delimiters and the unspecified types (48-63, used by applications)
 * included, except what no decoder uses: filler data (38), the reserved
 * nal_unit_types (10-15, 22-31, 41-47; H.265 7.4.2.2: decoders discard
 * them), units of another layer (nuh_layer_id other than 0: on a disc the
 * second layer is a stream of its own), units too short for their 2-byte
 * header and zero stuffing (trailing_zero_8bits). An end of sequence (36) or
 * end of bitstream (37) unit makes the next picture start a coded video
 * sequence (7.4.3.6): the title timeline takes it out where that changes
 * nothing (before an IDR or BLA picture, at the title's end) and keeps it
 * before a CRA picture, whose leading pictures it marks as not to be shown
 * (ff_disctl_hevc_seq_end). A stream file and a Matroska file of a title
 * carry the same NAL units.
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

typedef struct HEVCState {
    DRHevcDropped dropped;      /* what was taken out */
} HEVCState;

/* Whether a NAL unit of type t leaves the frame. */
static int dropped(int t)
{
    return (t >= 10 && t <= 15) || (t >= 22 && t <= 31) || t == 38 || (t >= 41 && t <= 47);
}

/* The next start code 00 00 01 at or after i, or -1. */
static int next_sc(const uint8_t *p, int n, int i)
{
    for (; i + 2 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1)
            return i;
    return -1;
}

int ff_disctl_hevc_clean(const uint8_t *p, int n, uint8_t *out, DRHevcDropped *d, int *changed, int *seq_end)
{
    int i = next_sc(p, n, 0), o = 0;

    *changed = *seq_end = 0;
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
            if (t == 36 || t == 37)
                *seq_end = 1;
            else if (t >= 48 && t <= 61)
                d->unspecified++;
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
    if (d->units[38])
        dr_vlog(NULL, DR_VLOG_INFO, "Title timeline: %s: %"PRId64" filler data units taken out of the frames (no "
               "decoding meaning, H.265 7.4.3.8)\n", what, d->units[38]);
    for (int t = 0; t < 64; t++)
        if (d->units[t] && t != 38)
            dr_vlog(NULL, DR_VLOG_WARNING, "Title timeline: %s: %"PRId64" NAL units of the reserved type %d taken "
                   "out of the frames (decoders discard them, H.265 7.4.2.2; a badly made stream?) [untested on "
                   "real discs]\n", what, d->units[t], t);
    if (d->other_layer)
        dr_vlog(NULL, DR_VLOG_WARNING, "Title timeline: %s: %"PRId64" NAL units of another layer (nuh_layer_id not "
               "0; a Blu-ray carries a second layer as a stream of its own) taken out of the frames [untested on "
               "real discs]\n", what, d->other_layer);
    if (d->short_units)
        dr_vlog(NULL, DR_VLOG_WARNING, "Title timeline: %s: %"PRId64" NAL units too short for their header (damaged "
               "data) taken out of the frames [untested on real discs]\n", what, d->short_units);
    if (d->unspecified)
        dr_vlog(NULL, DR_VLOG_WARNING, "Title timeline: %s: %"PRId64" NAL units of the unspecified types 48-61 "
               "(application data) kept [untested on real discs]\n", what, d->unspecified);
    if (d->stuffing)
        dr_vlog(NULL, DR_VLOG_INFO, "Title timeline: %s: %"PRId64" zero stuffing bytes taken out\n", what,
               d->stuffing);
}

/* The first coded slice segment's nal_unit_type of an Annex B unit, -1 for
 * none. */
static int first_vcl(const uint8_t *p, int n)
{
    for (int i = next_sc(p, n, 0); i >= 0 && i + 4 < n; i = next_sc(p, n, i + 3)) {
        int t = (p[i + 3] >> 1) & 0x3f;
        if ((p[i + 3] & 1) || p[i + 4] >> 3)
            continue;                   /* another layer */
        if (t <= 31)
            return t;
    }
    return -1;
}

int ff_disctl_hevc_seq_end(DRFrame *f, const DRFrame *next)
{
    int t = next ? first_vcl(next->data, next->size) : -2;
    DrBuf *buf;
    int o = 0, i, from = 0;

    if (t >= 21 && t <= 23)
        return 1;                       /* CRA (or reserved IRAP): its leading pictures depend on it */
    if (t != -2 && (t < 16 || t > 20))
        return 2;                       /* no random access point follows: a stream fault, kept as it is */
    if (!(buf = dr_buf_alloc(f->size + DR_BUF_PADDING)))
        return DR_ERR(ENOMEM);
    /* each NAL unit from the zero bytes in front of its start code to the
     * next one's; the end units left out */
    for (i = next_sc(f->data, f->size, 0); i >= 0; ) {
        int e = next_sc(f->data, f->size, i + 3), z = i, ez = e < 0 ? f->size : e;
        while (z > from && !f->data[z - 1])
            z--;
        while (e >= 0 && ez > i + 3 && !f->data[ez - 1])
            ez--;
        if (i == next_sc(f->data, f->size, 0) && z > 0) {
            memcpy(dr_buf_data(buf), f->data, z);  /* bytes before the first unit stay */
            o = z;
        }
        if (i + 3 >= f->size || (((f->data[i + 3] >> 1) & 0x3f) != 36 && ((f->data[i + 3] >> 1) & 0x3f) != 37)) {
            memcpy(dr_buf_data(buf) + o, f->data + z, ez - z);
            o += ez - z;
        }
        from = ez;
        i = e;
    }
    memset(dr_buf_data(buf) + o, 0, DR_BUF_PADDING);
    dr_buf_unref(&f->buf);
    f->buf   = buf;
    f->data  = dr_buf_data(buf);
    f->size  = o;
    f->flags &= ~DR_F_SEQ_END;
    return 0;
}

int ff_disctl_hevc_wrap_size(const uint8_t *p, int n)
{
    int units = 0;
    for (int i = next_sc(p, n, 0); i >= 0; i = next_sc(p, n, i + 3))
        units++;
    return n + 2 * units;
}

int ff_disctl_hevc_wrap_layer(const uint8_t *p, int n, uint8_t *out)
{
    int i = next_sc(p, n, 0), o = 0;

    if (i < 0)
        return 0;
    memcpy(out, p, i);                  /* the zero bytes in front of the first start code */
    o = i;
    while (i >= 0) {
        int e = next_sc(p, n, i + 3), end = e < 0 ? n : e, t;
        while (e >= 0 && end > i + 3 && !p[end - 1])
            end--;                      /* zero bytes go with the next start code */
        t = i + 3 < n ? (p[i + 3] >> 1) & 0x3f : -1;
        memcpy(out + o, p + i, 3);      /* the start code */
        o += 3;
        if (t != 62 && t != 63) {
            out[o++] = 0x7e;
            out[o++] = 0x01;
        }
        memcpy(out + o, p + i + 3, end - i - 3);
        o += end - i - 3;
        if (e >= 0) {
            memcpy(out + o, p + end, e - end);
            o += e - end;
        }
        i = e;
    }
    return o;
}

/* Rebuild the unit without the NAL units that leave it and without zero
 * stuffing (ff_disctl_hevc_clean). */
static int split_hevc(DRVideo *v, DRFrame *f)
{
    HEVCState *s = ff_disctl_video_priv(v);
    DrBuf *buf;
    int out, changed, seq_end;

    if (next_sc(f->data, f->size, 0) < 0)
        return 0;                       /* no NAL unit: picture() leaves it out */
    if (!(buf = dr_buf_alloc(f->size + DR_BUF_PADDING)))
        return DR_ERR(ENOMEM);
    {
        int64_t filler = s->dropped.units[38], stuffing = s->dropped.stuffing;
        out = ff_disctl_hevc_clean(f->data, f->size, dr_buf_data(buf), &s->dropped, &changed, &seq_end);
        /* said in the title's summary */
        ff_disctl_video_removed(v, DR_REMOVED_FILLER, s->dropped.units[38] - filler);
        ff_disctl_video_stuffing(v, s->dropped.stuffing - stuffing);
    }
    if (seq_end)
        f->flags |= DR_F_SEQ_END;
    if (!changed || out == f->size) {
        dr_buf_unref(&buf);
        return 0;
    }
    memset(dr_buf_data(buf) + out, 0, DR_BUF_PADDING);
    dr_buf_unref(&f->buf);
    f->buf  = buf;
    f->data = dr_buf_data(buf);
    f->size = out;
    return 0;
}

/* The picture values come from the parser; a unit without a slice segment
 * is no picture. How decoding can start at a key frame (H.265 8.1.3,
 * 7.4.2.2): an IDR picture needs nothing before it (its leading pictures
 * are RADL pictures); a BLA picture's RASL pictures are not decoded (a
 * broken link); a CRA picture's RASL pictures use pictures before it, as may
 * the pictures before a recovery point. */
static int picture_hevc(DRVideo *v, const DRFrame *f, DRPicture *pic)
{
    if (pic->rate_num > 0 && pic->rate_den > 0)
        ff_disctl_video_set_rate(v, pic->rate_num, pic->rate_den);
    if (pic->fields <= 0 || pic->type < DR_PIC_I || pic->type > DR_PIC_B) {
        pic->type = DR_PIC_OTHER;
        return 0;
    }
    if (pic->key) {
        int t = first_vcl(f->data, f->size);
        pic->start = t == 19 || t == 20 ? DR_START_CLEAN : t >= 16 && t <= 18 ? DR_START_BROKEN : DR_START_OPEN;
    }
    return 0;
}

/* Filler and stuffing are said in the title's summary; the rest here. */
static void close_hevc(void *priv)
{
    const HEVCState *s = priv;
    DRHevcDropped d = s->dropped;

    d.units[38] = 0;
    d.stuffing  = 0;
    ff_disctl_hevc_dropped_log(&d, "HEVC");
}

const DRVideoRules ff_disctl_video_hevc = {
    .picture      = picture_hevc,
    .split        = split_hevc,
    .close        = close_hevc,
    .order_period = 0x10000000,
    .codec_keys   = 1,
    .priv_size    = sizeof(HEVCState),
};
