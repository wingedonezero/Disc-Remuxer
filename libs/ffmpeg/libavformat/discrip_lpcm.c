/*
 * The shared rip core: linear PCM of DVD-Video and HD DVD. The private
 * header of each PES packet gives the format (checked against the first),
 * a unit is one frame, and the samples are turned into little-endian PCM.
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

#include "libavutil/error.h"
#include "libavutil/log.h"

#include "discrip.h"

static const int rates[3] = { 48000, 96000, 192000 };

/* HD DVD channel assignment (5-byte header, low 5 bits of its last byte, from
 * 2): the channel mask */
static const uint64_t hd_masks[19] = {
    0x103, 0x33, 0xb, 0x10b, 0x3b, 0x7, 0x107, 0x37, 0xf, 0x10f,
    0x3f, 0x107, 0x37, 0xf, 0x10f, 0x3f, 0x3b, 0x37, 0x3f,
};

/* The DVD-Video audio frame header (3 bytes): emphasis, mute, reserved,
 * frame number (5); quantisation (2: 16 / 20 / 24 bits), sampling frequency
 * (2: 48 / 96 / 192 kHz), reserved, channels - 1 (3); dynamic range. A frame
 * is rate / 600 samples (HD DVD: rate / 1200). After the first header only
 * the frame number and the dynamic range may change. */
static int parse(DRLpcm *p, void *log, uint8_t b0, uint8_t b1, uint8_t b2, int hd, int chan_code)
{
    if (!p->init) {
        int q = b1 >> 6, f = (b1 >> 4) & 3;

        if (q == 3 || f == 3) {
            av_log(log, AV_LOG_ERROR, "Rip core: LPCM: the header %02x %02x %02x states no %s\n", b0, b1, b2,
                   q == 3 ? "sample size" : "sampling frequency");
            return AVERROR_INVALIDDATA;
        }
        p->b0       = b0;
        p->b1       = b1;
        p->bits     = 16 + 4 * q;
        p->rate     = rates[f];
        p->spf      = p->rate / (hd ? 1200 : 600);
        p->channels = (b1 & 7) + 1;
        p->chmask   = hd && chan_code >= 2 && chan_code - 2 < 19 ? hd_masks[chan_code - 2] : 0;
        p->frame_bytes     = p->bits * p->channels * p->spf / 8;
        p->out_frame_bytes = (p->bits == 16 ? 2 : 3) * p->channels * p->spf;
        p->hd       = hd;
        p->init     = 1;
        av_log(log, AV_LOG_DEBUG, "Rip core: LPCM: %d bits, %d Hz, %d channels, %d samples per frame (%d bytes)\n",
               p->bits, p->rate, p->channels, p->spf, p->frame_bytes);
        if (p->bits != 16 || p->channels > 2)
            av_log(log, AV_LOG_WARNING, "Rip core: LPCM: %d-bit %d-channel samples are converted by the DVD-Video "
                   "sample layout, the channels kept in the disc's order [untested on real discs]\n", p->bits,
                   p->channels);
        if (hd)
            av_log(log, AV_LOG_WARNING, "Rip core: LPCM: HD DVD LPCM [untested on real discs]\n");
    } else if ((b0 ^ p->b0) >= 0x20 || b1 != p->b1) {
        av_log(log, AV_LOG_ERROR, "Rip core: LPCM: the header %02x %02x %02x does not match the stream's first "
               "(%02x %02x): the format changed\n", b0, b1, b2, p->b0, p->b1);
        return AVERROR_INVALIDDATA;
    }
    p->drc = b2;
    return 0;
}

int ff_discrip_lpcm_header(DRLpcm *p, void *log, const uint8_t *h, int len, int hd)
{
    if (!hd) {
        if (len != 3)
            return AVERROR_INVALIDDATA;
        return parse(p, log, h[0], h[1], h[2], 0, 0);
    }
    /* the HD DVD header (5 bytes) in the DVD layout: quantisation from bit 0
     * of the first byte and bit 7 of the second, the frequency code (bits
     * 6..4, below 4), the channel code (bits 3..0, below 8) */
    {
        int q = (h[1] >> 4) & 7, f = h[1] & 0x0F;
        uint8_t b0, b1;

        if (len != 5 || q >= 4 || f >= 8) {
            av_log(log, AV_LOG_ERROR, "Rip core: LPCM: an HD DVD header that cannot be read\n");
            return AVERROR_INVALIDDATA;
        }
        b0 = (h[0] & 0xC0) | ((h[0] >> 1) & 0x1F);
        b1 = ((h[0] & 1) << 7) | (((h[1] >> 7) & 1) << 6) | (q << 4) | f;
        return parse(p, log, b0, b1, h[2], 1, h[4] & 0x1F);
    }
}

int ff_discrip_lpcm_convert(const DRLpcm *p, const uint8_t *in, int size, uint8_t *out)
{
    int o = 0;

    if (p->bits == 16) {
        for (int i = 0; i + 1 < size; i += 2) {
            out[o++] = in[i + 1];
            out[o++] = in[i];
        }
        return o;
    }
    /* groups of 4 samples: their high 16 bits (big-endian), then their low
     * bits: 20-bit 2 bytes of 2 nibbles (high nibble first), 24-bit 4 bytes;
     * out: 24-bit little-endian (20-bit samples with 4 zero bits) */
    {
        int g = p->bits == 20 ? 10 : 12;

        for (int i = 0; i + g <= size; i += g) {
            const uint8_t *s = in + i;
            for (int k = 0; k < 4; k++) {
                uint8_t low = p->bits == 20 ? (k & 1 ? s[8 + k / 2] << 4 : s[8 + k / 2] & 0xF0) : s[8 + k];
                out[o++] = low;
                out[o++] = s[2 * k + 1];
                out[o++] = s[2 * k];
            }
        }
        return o;
    }
}
