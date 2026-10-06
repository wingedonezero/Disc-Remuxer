/*
 * The shared rip core: the rules for MPEG-1 / MPEG-2 video (ISO/IEC 11172-2,
 * 13818-2) pictures.
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

#include "libavutil/log.h"

#include "discrip.h"

/* frame_rate_code 1..8 (ISO/IEC 13818-2 Table 6-4) */
static const int rates[8][2] = {
    { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 },
};

typedef struct MpvState {
    int have_rate;        /* the first sequence header gave the frame rate */
    int mpeg2;            /* a sequence extension follows the sequence header */
    int progressive_seq;  /* sequence_extension progressive_sequence */
} MpvState;

/* The next start code 00 00 01 xx at or after i, or -1. */
static int next_sc(const uint8_t *p, int n, int i)
{
    for (; i + 3 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1)
            return i;
    return -1;
}

/* User data right after a GOP header (ISO/IEC 13818-2 6.2.2.6: user_data
 * at the GOP level; DVD-Video line-21 captions are carried there) leaves
 * the video: each user_data from its start code up to the next start code
 * becomes a side unit. User data after a sequence header or a picture
 * header stays in the video. */
static int split_mpv(DRVideo *v, DRFrame *f)
{
    uint8_t *p = f->data;
    int n = f->size, i = 0, ret;

    while ((i = next_sc(p, n, i)) >= 0) {
        int e;

        if (p[i + 3] != 0xB8) {
            i += 4;
            continue;
        }
        i += 8;                                  /* group_of_pictures_header: 8 bytes */
        while ((i = next_sc(p, n, i)) >= 0 && p[i + 3] == 0xB2) {
            e = next_sc(p, n, i + 4);
            if (e < 0)
                e = n;
            if ((ret = ff_discrip_video_side(v, p + i, e - i, f->pos + i)) < 0)
                return ret;
            memmove(p + i, p + e, n - e);
            n -= e - i;
        }
        if (i < 0)
            break;
    }
    f->size = n;
    return 0;
}

/* A unit as FFmpeg's parser cuts it: sequence header (with its extension and
 * user data), GOP header (and its user data), picture header (with its
 * coding extension and user data) and slices; FFmpeg keeps both field
 * pictures of a frame in one unit. */
static int picture_mpv(DRVideo *v, const DRFrame *f, DRPicture *pic)
{
    MpvState *s = ff_discrip_video_priv(v);
    const uint8_t *p = f->data;
    int n = f->size, i = 0, pictures = 0, structure = 3, tff = 0, rff = 0, prog_frame = 0, have_ext = 0;
    int pct = 0, tr = 0;

    while ((i = next_sc(p, n, i)) >= 0) {
        int code = p[i + 3];

        if (code == 0xB3 && i + 12 <= n) {
            int frc = p[i + 7] & 0xF;
            int e   = next_sc(p, n, i + 12);
            int num = 0, den = 0, mpeg2 = 0;

            if (frc >= 1 && frc <= 8) {
                num = rates[frc - 1][0];
                den = rates[frc - 1][1];
            }
            /* the sequence extension (id 1) right after it, past any quantiser matrices */
            while (e >= 0 && e + 10 <= n && p[e + 3] == 0xB5 && (p[e + 4] & 0xF0) != 0x10)
                e = next_sc(p, n, e + 4);
            if (e >= 0 && e + 10 <= n && p[e + 3] == 0xB5 && (p[e + 4] & 0xF0) == 0x10) {
                mpeg2 = 1;
                num *= ((p[e + 9] >> 5) & 3) + 1;
                den *= (p[e + 9] & 0x1F) + 1;
                if (!s->have_rate)
                    s->progressive_seq = (p[e + 5] >> 3) & 1;
            }
            if (num && den) {
                if (!s->have_rate) {
                    s->have_rate = 1;
                    s->mpeg2     = mpeg2;
                }
                ff_discrip_video_set_rate(v, num, den);
            }
        } else if (code == 0x00 && i + 6 <= n) {
            if (!pictures++) {
                pct = (p[i + 5] >> 3) & 7;
                tr  = (p[i + 4] << 2) | (p[i + 5] >> 6);
            }
        } else if (code == 0xB5 && i + 9 <= n && (p[i + 4] & 0xF0) == 0x80 && pictures == 1 && !have_ext) {
            have_ext   = 1;
            structure  = p[i + 6] & 3;
            tff        = p[i + 7] >> 7;
            rff        = (p[i + 7] >> 1) & 1;
            prog_frame = p[i + 8] >> 7;
        }
        i += 4;
    }
    if (!pictures) {
        pic->type = DR_PIC_OTHER;
        return 0;
    }
    if (pct < 1 || pct > 3 || !structure) {
        av_log(NULL, AV_LOG_WARNING, "Rip core: MPEG video: a picture of coding type %d (structure %d) is not a "
               "picture of the stream: left out\n", pct, structure);
        pic->type = DR_PIC_OTHER;
        return 0;
    }
    pic->type  = pct + 1;
    pic->order = tr;
    if (!s->mpeg2)
        pic->fields = 2;
    else if (structure != 3)
        pic->fields = pictures >= 2 ? 2 : 1;           /* a field pair, or a lone field */
    else if (s->progressive_seq) {
        static const int prog[4] = { 2, 2, 4, 6 };     /* repeated frames of a progressive sequence */
        pic->fields = prog[rff * 2 + tff];
    } else
        pic->fields = (prog_frame && rff) ? 3 : 2;
    return 0;
}

const DRVideoRules ff_discrip_video_mpv = {
    .picture      = picture_mpv,
    .split        = split_mpv,
    .order_period = 0x400,
    .priv_size    = sizeof(MpvState),
};
