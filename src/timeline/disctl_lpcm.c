/*
 * The title timeline: linear PCM of DVD-Video, HD DVD and Blu-ray. The
 * private header of each PES packet (Blu-ray: of each unit) gives the
 * format (checked against the first), a unit is one frame, and the samples
 * are turned into little-endian PCM.
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
            dr_vlog(log, DR_VLOG_ERROR, "Title timeline: LPCM: the header %02x %02x %02x states no %s\n", b0, b1, b2,
                   q == 3 ? "sample size" : "sampling frequency");
            return DR_ERR_INVALID;
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
        dr_vlog(log, DR_VLOG_DEBUG, "Title timeline: LPCM: %d bits, %d Hz, %d channels, %d samples per frame (%d bytes)\n",
               p->bits, p->rate, p->channels, p->spf, p->frame_bytes);
        if (p->bits != 16 || p->channels > 2)
            dr_vlog(log, DR_VLOG_WARNING, "Title timeline: LPCM: %d-bit %d-channel samples are converted by the DVD-Video "
                   "sample layout, the channels kept in the disc's order [untested on real discs]\n", p->bits,
                   p->channels);
        if (hd)
            dr_vlog(log, DR_VLOG_WARNING, "Title timeline: LPCM: HD DVD LPCM [untested on real discs]\n");
    } else if ((b0 ^ p->b0) >= 0x20 || b1 != p->b1) {
        dr_vlog(log, DR_VLOG_ERROR, "Title timeline: LPCM: the header %02x %02x %02x does not match the stream's first "
               "(%02x %02x): the format changed\n", b0, b1, b2, p->b0, p->b1);
        return DR_ERR_INVALID;
    }
    p->drc = b2;
    return 0;
}

int ff_disctl_lpcm_header(DRLpcm *p, void *log, const uint8_t *h, int len, int hd)
{
    if (!hd) {
        if (len != 3)
            return DR_ERR_INVALID;
        return parse(p, log, h[0], h[1], h[2], 0, 0);
    }
    /* the HD DVD header (5 bytes) in the DVD layout: quantisation from bit 0
     * of the first byte and bit 7 of the second, the frequency code (bits
     * 6..4, below 4), the channel code (bits 3..0, below 8) */
    {
        int q = (h[1] >> 4) & 7, f = h[1] & 0x0F;
        uint8_t b0, b1;

        if (len != 5 || q >= 4 || f >= 8) {
            dr_vlog(log, DR_VLOG_ERROR, "Title timeline: LPCM: an HD DVD header that cannot be read\n");
            return DR_ERR_INVALID;
        }
        b0 = (h[0] & 0xC0) | ((h[0] >> 1) & 0x1F);
        b1 = ((h[0] & 1) << 7) | (((h[1] >> 7) & 1) << 6) | (q << 4) | f;
        return parse(p, log, b0, b1, h[2], 1, h[4] & 0x1F);
    }
}

/* Blu-ray channel assignments 1 and 3 to 11: the channels shown, the
 * channels stored (an odd count is padded to even), and each stored
 * channel's place in the channel mask (0: the padding channel; none for
 * mono and stereo). Disc order of 3/4: L R C Ls Rls Rrs Rs (+ LFE). */
static const struct {
    uint8_t  channels, coded;
    uint16_t pos[8];
} bd_assign[12] = {
    [1]  = { 1, 2 },
    [3]  = { 2, 2 },
    [4]  = { 3, 4, { 0x001, 0x002, 0x004, 0 } },
    [5]  = { 3, 4, { 0x001, 0x002, 0x100, 0 } },
    [6]  = { 4, 4, { 0x001, 0x002, 0x004, 0x100 } },
    [7]  = { 4, 4, { 0x001, 0x002, 0x200, 0x400 } },
    [8]  = { 5, 6, { 0x001, 0x002, 0x004, 0x200, 0x400, 0 } },
    [9]  = { 6, 6, { 0x001, 0x002, 0x004, 0x200, 0x400, 0x008 } },
    [10] = { 7, 8, { 0x001, 0x002, 0x004, 0x200, 0x010, 0x020, 0x400, 0 } },
    [11] = { 8, 8, { 0x001, 0x002, 0x004, 0x200, 0x010, 0x020, 0x400, 0x008 } },
};

static int bd_rate(int fs)
{
    return fs == 1 ? 48000 : fs == 4 ? 96000 : fs == 5 ? 192000 : 0;
}

int ff_disctl_lpcm_bd_header(DRLpcm *p, void *log, const uint8_t *u, int size)
{
    int payload, ca, fs, q;

    if (size < 4) {
        dr_vlog(log, DR_VLOG_ERROR, "Title timeline: LPCM: a %d-byte unit has no header\n", size);
        return DR_ERR_INVALID;
    }
    payload = dr_rb16(u);
    if (p->init) {
        if (u[2] != p->h2 || (u[3] ^ p->h3) >= 0x40 || payload != p->payload) {
            dr_vlog(log, DR_VLOG_ERROR, "Title timeline: LPCM: the header %02x %02x %02x %02x does not match the "
                   "stream's first (payload %d bytes, %02x %02x): the format changed [untested on real discs]\n",
                   u[0], u[1], u[2], u[3], p->payload, p->h2, p->h3);
            return DR_ERR_INVALID;
        }
        return 0;
    }
    ca = u[2] >> 4;
    fs = u[2] & 0x0F;
    q  = u[3] >> 6;
    if (ca >= DR_ARRAY_ELEMS(bd_assign) || !bd_assign[ca].channels || !bd_rate(fs) || !q || !payload) {
        dr_vlog(log, DR_VLOG_ERROR, "Title timeline: LPCM: the header %02x %02x %02x %02x states no format\n",
               u[0], u[1], u[2], u[3]);
        return DR_ERR_INVALID;
    }
    p->bd       = 1;
    p->h2       = u[2];
    p->h3       = u[3];
    p->payload  = payload;
    p->bits     = 12 + 4 * q;
    p->rate     = bd_rate(fs);
    p->channels = bd_assign[ca].channels;
    p->coded    = bd_assign[ca].coded;
    p->chmask   = 0;
    for (int k = 0; k < p->coded; k++)
        p->chmask |= bd_assign[ca].pos[k];
    /* the channels leave in the order of their places in the mask */
    if (!p->chmask) {
        for (int c = 0; c < p->channels; c++)
            p->map[c] = c;
    } else {
        int c = 0;
        for (uint64_t bit = 1; c < p->channels; bit <<= 1)
            for (int k = 0; k < p->coded; k++)
                if (bd_assign[ca].pos[k] == bit)
                    p->map[c++] = k;
    }
    p->frame_bytes     = 4 + payload;
    p->spf             = payload / ((p->bits == 16 ? 2 : 3) * p->coded);
    p->out_frame_bytes = (p->bits == 16 ? 2 : 3) * p->channels * p->spf;
    p->init            = 1;
    dr_vlog(log, DR_VLOG_DEBUG, "Title timeline: LPCM: Blu-ray, %d bits, %d Hz, %d channels (%d stored, mask 0x%"PRIx64
           "), %d samples per %d-byte unit\n", p->bits, p->rate, p->channels, p->coded, p->chmask, p->spf,
           p->frame_bytes);
    /* the formats real discs have shown, each = the reference's samples:
     * 24-bit and 16-bit stereo at 48 kHz */
    if ((p->bits != 24 && p->bits != 16) || p->channels != 2 || p->rate != 48000)
        dr_vlog(log, DR_VLOG_WARNING, "Title timeline: LPCM: Blu-ray %d-bit %d-channel %d Hz [untested on real discs]\n",
               p->bits, p->channels, p->rate);
    if (p->payload % ((p->bits == 16 ? 2 : 3) * p->coded))
        dr_vlog(log, DR_VLOG_WARNING, "Title timeline: LPCM: a %d-byte payload is no whole number of samples: its "
               "last %d bytes are left out [untested on real discs]\n", p->payload,
               p->payload % ((p->bits == 16 ? 2 : 3) * p->coded));
    return 0;
}

int ff_disctl_lpcm_convert(const DRLpcm *p, const uint8_t *in, int size, uint8_t *out)
{
    int o = 0;

    if (p->bd) {
        /* sample frames of coded channels, big-endian 2 bytes (16 bits) or 3
         * (20 bits left-aligned, 24 bits) */
        int bps = p->bits == 16 ? 2 : 3, fb = bps * p->coded;

        for (int i = 0; i + fb <= size; i += fb)
            for (int c = 0; c < p->channels; c++) {
                const uint8_t *s = in + i + bps * p->map[c];
                for (int b = bps - 1; b >= 0; b--)
                    out[o++] = s[b];
            }
        return o;
    }

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
