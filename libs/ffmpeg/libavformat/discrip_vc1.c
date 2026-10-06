/*
 * The shared rip core: the rules for VC-1 (SMPTE 421M, Advanced Profile in
 * Annex E start-code form) pictures.
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

#include "libavutil/log.h"

#include "discrip.h"

#define SC_END    0x0A   /* end of sequence */
#define SC_FIELD  0x0C
#define SC_FRAME  0x0D
#define SC_ENTRY  0x0E
#define SC_SEQ    0x0F

/* FRAMERATENR 1..7 and FRAMERATEDR 1..2 (SMPTE 421M 6.1.14.4) */
static const int rate_nr[7] = { 24000, 25000, 30000, 50000, 60000, 48000, 72000 };
static const int rate_dr[2] = { 1000, 1001 };

typedef struct Vc1State {
    int have_seq;              /* a usable sequence header was read */
    int profile, interlace, tfcntr, pulldown, psf;
} Vc1State;

/* MSB-first bit reader over one start-code unit's bytes after its start
 * code, without the emulation-prevention bytes (00 00 03). */
typedef struct Bits {
    const uint8_t *p, *end;
    int zeros, bit, over;
    unsigned cur;
} Bits;

static void bits_init(Bits *b, const uint8_t *p, const uint8_t *end)
{
    *b = (Bits){ .p = p, .end = end, .bit = 8 };
}

static unsigned u1(Bits *b)
{
    if (b->bit == 8) {
        if (b->p >= b->end) {
            b->over = 1;
            return 0;
        }
        if (b->zeros >= 2 && *b->p == 3) {
            b->zeros = 0;
            if (++b->p >= b->end) {
                b->over = 1;
                return 0;
            }
        }
        b->cur   = *b->p++;
        b->zeros = b->cur ? 0 : b->zeros + 1;
        b->bit   = 0;
    }
    return (b->cur >> (7 - b->bit++)) & 1;
}

static unsigned u(Bits *b, int n)
{
    unsigned v = 0;
    while (n--)
        v = (v << 1) | u1(b);
    return v;
}

/* The next start code 00 00 01 xx at or after i, or -1. */
static int next_sc(const uint8_t *p, int n, int i)
{
    for (; i + 3 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1)
            return i;
    return -1;
}

/* The sequence header (Advanced Profile): profile, interlace, pulldown,
 * TFCNTR and PSF flags, and the frame rate. A header without the display
 * extension or without a frame rate is not usable (the reference skips it). */
static void sequence(DRVideo *v, Vc1State *s, const uint8_t *p, const uint8_t *end)
{
    Bits b;
    int profile, pulldown, interlace, tfcntr, psf, num, den;

    bits_init(&b, p, end);
    profile   = u(&b, 2);
    u(&b, 3 + 11);                 /* level; colordiff, postproc fields */
    u(&b, 24);                     /* max coded width / height */
    pulldown  = u1(&b);
    interlace = u1(&b);
    tfcntr    = u1(&b);
    u(&b, 2);                      /* finterpflag, reserved */
    psf       = u1(&b);
    if (!u1(&b))                   /* display extension */
        return;
    u(&b, 28);                     /* display size */
    if (u1(&b) && u(&b, 4) == 15)  /* aspect ratio */
        u(&b, 16);
    if (!u1(&b))                   /* frame rate flag */
        return;
    if (!u1(&b)) {
        int nr = u(&b, 8), dr = u(&b, 4);
        if (nr < 1 || nr > 7 || dr < 1 || dr > 2)
            return;
        num = rate_nr[nr - 1];
        den = rate_dr[dr - 1];
    } else {
        num = u(&b, 16) + 1;
        den = 32;
    }
    if (b.over)
        return;
    if (num % den == 0) {
        num /= den;
        den  = 1;
    }
    s->have_seq  = 1;
    s->profile   = profile;
    s->interlace = interlace;
    s->tfcntr    = tfcntr;
    s->pulldown  = pulldown;
    s->psf       = psf;
    ff_discrip_video_set_rate(v, num, den);
}

/* A unit as FFmpeg's parser cuts it holds at most one frame (start code 0x0D)
 * and its second field (0x0C), with the sequence header / entry point /
 * user data before it. */
static int picture_vc1(DRVideo *v, const DRFrame *f, DRPicture *pic)
{
    Vc1State *s = ff_discrip_video_priv(v);
    const uint8_t *p = f->data;
    int n = f->size, i = 0, frame = -1, has_seq = 0, has_entry = 0, field2 = 0;

    while ((i = next_sc(p, n, i)) >= 0) {
        int code = p[i + 3], e = next_sc(p, n, i + 4);
        if (e < 0)
            e = n;
        if (code == SC_SEQ) {
            has_seq = 1;
            if (!s->have_seq)
                sequence(v, s, p + i + 4, p + e);
        } else if (code == SC_ENTRY)
            has_entry = 1;
        else if (code == SC_FRAME && frame < 0)
            frame = i;
        else if (code == SC_FIELD && frame >= 0)
            field2 = 1;
        i += 4;
    }
    if (frame < 0) {
        pic->type = DR_PIC_OTHER;
        return 0;
    }
    if (!s->have_seq) {
        av_log(NULL, AV_LOG_WARNING, "Rip core: VC-1: a frame before any usable sequence header: left out\n");
        pic->type = DR_PIC_OTHER;
        return 0;
    }
    {
        Bits b;
        int fcm = 0, type, fields = 2;
        int e = next_sc(p, n, frame + 4);

        bits_init(&b, p + frame + 4, p + (e < 0 ? n : e));
        if (s->profile == 3 && s->interlace)
            fcm = u1(&b) ? 2 + u1(&b) : 0;      /* 0 progressive, 2 frame interlace, 3 field interlace */
        if (fcm == 3) {
            /* field pair: FPTYPE, a 3-bit code (Table 105): I/I, I/P, P/I,
             * P/P, B/B, B/BI, BI/B, BI/BI (the reference reads it with the
             * frame PTYPE code) */
            static const int fp[8] = { DR_PIC_I, DR_PIC_I, DR_PIC_P, DR_PIC_P, DR_PIC_B, DR_PIC_B, DR_PIC_B, DR_PIC_B };
            type   = fp[u(&b, 3)];
            fields = 1 + field2;
        } else {
            /* PTYPE: P 0, B 10, I 110, BI 1110, skipped 1111 */
            int ptype = 0, k = 0;
            while (k < 4 && u1(&b)) {
                ptype = (ptype << 1) | 1;
                k++;
            }
            ptype <<= (k < 4);
            if (ptype == 0x0F)
                type = DR_PIC_P;                /* skipped: a repeat of the reference picture */
            else if (ptype == 0)
                type = DR_PIC_P;
            else if (ptype == 2 || ptype == 0x0E)
                type = DR_PIC_B;
            else
                type = DR_PIC_I;                /* 6 */
            if (ptype != 0x0F) {
                if (s->tfcntr)
                    u(&b, 8);
                if (s->pulldown) {
                    if (s->interlace && !s->psf) {
                        u1(&b);                 /* TFF */
                        if (u1(&b))             /* RFF */
                            fields = 3;
                    } else
                        fields *= u(&b, 2) + 1; /* RPTFRM */
                }
            }
        }
        if (b.over) {
            av_log(NULL, AV_LOG_WARNING, "Rip core: VC-1: a frame header cut short: left out\n");
            pic->type = DR_PIC_OTHER;
            return 0;
        }
        pic->type   = type;
        pic->fields = fields;
        pic->key    = has_seq && has_entry;
    }
    return 0;
}

const DRVideoRules ff_discrip_video_vc1 = {
    .picture       = picture_vc1,
    .order_period  = 0x1000000,
    .counted_order = 1,
    .priv_size     = sizeof(Vc1State),
};
