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
    int64_t ends_dropped;      /* units of end-of-sequence codes only, left out */
    int64_t headers_dropped;   /* units of headers without a frame, left out */
    int     said_user_data;    /* the untested note on frame user data was logged */
    int     said_skipped_tfcntr; /* the untested note on skipped frames with TFCNTR was logged */
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

/* ---- the cutter (DRCodec.unit_size) ----
 * A unit is one frame with what belongs to it, in start-code units (BDUs,
 * Annex E):
 *   [sequence header]* [sequence user data]* [entry point]*
 *   [sequence / entry-point user data]* frame [field / frame user data]*
 *   [field [field user data]* [end of sequence]*]
 * User data after the frame header stays with that frame (FFmpeg's parser
 * starts the next unit there), so does an end of sequence after a second
 * field. Units without a frame take no time and are left out by the video
 * stage: an end of sequence without a second field before it, and headers
 * followed by another sequence header or an end of sequence instead of a
 * frame. A slice, a field or user data where none can be (or a reserved
 * start code) stops the track. */

#define SC_SLICE  0x0B
#define SC_UD_SLICE 0x1B
#define SC_UD_FIELD 0x1C
#define SC_UD_FRAME 0x1D
#define SC_UD_ENTRY 0x1E
#define SC_UD_SEQ   0x1F

enum { PART_HEADERS, PART_FRAME, PART_FIELD, PART_END_AFTER_FIELD, PART_END };

static int is_sc(const uint8_t *p)
{
    return !p[0] && !p[1] && p[2] == 1;
}

static void cut_fail(void *log, int code, const char *where)
{
    av_log(log, AV_LOG_ERROR, "Rip core: VC-1: start code 0x%02X %s: the track cannot be ripped\n", code, where);
}

/* The part a unit's first BDU begins, or < 0. */
static int first_part(void *log, int code, DRVc1Cut *st)
{
    switch (code) {
    case SC_SEQ:     st->stage = 0; return PART_HEADERS;
    case SC_UD_SEQ:  st->stage = 1; return PART_HEADERS;
    case SC_ENTRY:   st->stage = 2; return PART_HEADERS;
    case SC_UD_ENTRY: st->stage = 3; return PART_HEADERS;
    case SC_FRAME:   return PART_FRAME;
    case SC_END:     return PART_END;
    }
    cut_fail(log, code, "where a frame or its headers must start");
    return AVERROR_INVALIDDATA;
}

/* 1 when a BDU of code continues the unit, 0 when it starts the next one,
 * < 0 when it cannot be there. */
static int continues(void *log, int code, DRVc1Cut *st)
{
    switch (st->part) {
    case PART_HEADERS:
        switch (code) {
        case SC_SEQ:
            if (st->stage > 0)
                return 0;       /* the headers before it have no frame */
            return 1;
        case SC_UD_SEQ:
            st->stage = st->stage <= 1 ? 1 : 3;
            return 1;
        case SC_ENTRY:
            if (st->stage > 2) {
                cut_fail(log, code, "(entry point) after the entry-point user data");
                return AVERROR_INVALIDDATA;
            }
            st->stage = 2;
            return 1;
        case SC_UD_ENTRY:
            st->stage = 3;
            return 1;
        case SC_FRAME:
            st->part = PART_FRAME;
            return 1;
        case SC_END:
            st->part = PART_END;  /* the headers and the end are left out together */
            return 1;
        }
        cut_fail(log, code, "before a frame start code");
        return AVERROR_INVALIDDATA;
    case PART_FRAME:
        if (code == SC_UD_FIELD || code == SC_UD_FRAME)
            return 1;
        if (code == SC_FIELD) {
            st->part = PART_FIELD;
            return 1;
        }
        return 0;
    case PART_FIELD:
        if (code == SC_UD_FIELD)
            return 1;
        if (code == SC_END) {
            st->part = PART_END_AFTER_FIELD;
            return 1;
        }
        return 0;
    default:                    /* PART_END_AFTER_FIELD, PART_END */
        return code == SC_END;
    }
}

int ff_discrip_vc1_unit_size(void *log, const uint8_t *buf, int avail, int final, void *state)
{
    DRVc1Cut *st = state;
    int i = st->scan;

    if (!i) {
        int part;

        if (avail < 4)
            return final ? DR_CUT_NONE : 0;
        if (!is_sc(buf))
            return DR_CUT_NONE;
        if ((part = first_part(log, buf[3], st)) < 0) {
            *st = (DRVc1Cut){ 0 };
            return part;
        }
        st->part = part;
        i = 4;
    }
    for (; i + 3 < avail; i++) {
        int r;

        if (buf[i + 2] > 1) {
            i += 2;
            continue;
        }
        if (!is_sc(buf + i))
            continue;
        if ((r = continues(log, buf[i + 3], st)) <= 0) {
            *st = (DRVc1Cut){ 0 };
            return r < 0 ? r : i;
        }
        i += 3;
    }
    if (final) {
        *st = (DRVc1Cut){ 0 };
        return avail;
    }
    st->scan = i;
    return 0;
}

/* The next start code; the last two bytes may begin one. */
int ff_discrip_vc1_resync(const uint8_t *buf, int avail)
{
    for (int i = 0; i + 2 < avail; i++)
        if (is_sc(buf + i))
            return i;
    return avail > 2 ? avail - 2 : -1;
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
    ff_discrip_video_set_rate(v, num, den);
    if (s->have_seq)
        return;                    /* the flags come from the first usable header */
    s->have_seq  = 1;
    s->profile   = profile;
    s->interlace = interlace;
    s->tfcntr    = tfcntr;
    s->pulldown  = pulldown;
    s->psf       = psf;
}

/* A unit as the cutter cuts it holds at most one frame (start code 0x0D)
 * and its second field (0x0C), with the headers before it and the user data
 * after it; a unit without a frame is left out. */
static int picture_vc1(DRVideo *v, const DRFrame *f, DRPicture *pic)
{
    Vc1State *s = ff_discrip_video_priv(v);
    const uint8_t *p = f->data;
    int n = f->size, i = 0, frame = -1, has_seq = 0, has_entry = 0, field2 = 0, user_data = 0, other = 0;

    while ((i = next_sc(p, n, i)) >= 0) {
        int code = p[i + 3], e = next_sc(p, n, i + 4);
        if (e < 0)
            e = n;
        if (code == SC_SEQ) {
            has_seq = 1;
            sequence(v, s, p + i + 4, p + e);
        } else if (code == SC_ENTRY)
            has_entry = 1;
        else if (code == SC_FRAME && frame < 0)
            frame = i;
        else if (code == SC_FIELD && frame >= 0)
            field2 = 1;
        else if ((code == SC_UD_FIELD || code == SC_UD_FRAME) && frame >= 0)
            user_data = 1;
        if (code != SC_END)
            other = 1;
        i += 4;
    }
    if (frame < 0) {
        pic->type = DR_PIC_OTHER;
        if (!other) {
            /* an end of sequence without a second field before it */
            if (!s->ends_dropped++)
                av_log(NULL, AV_LOG_INFO, "Rip core: VC-1: an end-of-sequence code without a "
                       "second field before it is left out [untested on real discs]\n");
            av_log(NULL, AV_LOG_DEBUG, "Rip core: VC-1: end of sequence at stream offset %"PRId64" left out\n",
                   f->pos);
        } else {
            s->headers_dropped++;
            av_log(NULL, AV_LOG_WARNING, "Rip core: VC-1: %d bytes of headers at stream offset %"PRId64" without "
                   "a frame after them: left out [untested on real discs]\n", f->size, f->pos);
        }
        return 0;
    }
    if (user_data && !s->said_user_data) {
        s->said_user_data = 1;
        av_log(NULL, AV_LOG_INFO, "Rip core: VC-1: user data after a frame header is kept with that frame "
               "[untested on real discs]\n");
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
            /* a skipped frame has no TFCNTR here (FFmpeg's decoder reads
             * one), but its repeat fields like every frame picture */
            if (ptype != 0x0F && s->tfcntr)
                u(&b, 8);
            else if (ptype == 0x0F && s->tfcntr && !s->said_skipped_tfcntr) {
                s->said_skipped_tfcntr = 1;
                av_log(NULL, AV_LOG_WARNING, "Rip core: VC-1: a skipped frame in a stream with frame counters, "
                       "read without one [untested on real discs]\n");
            }
            if (s->pulldown) {
                if (s->interlace && !s->psf) {
                    u1(&b);                     /* TFF */
                    if (u1(&b))                 /* RFF */
                        fields = 3;
                } else
                    fields *= u(&b, 2) + 1;     /* RPTFRM */
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

static void close_vc1(void *priv)
{
    const Vc1State *s = priv;

    if (s->ends_dropped || s->headers_dropped)
        av_log(NULL, AV_LOG_INFO, "Rip core: VC-1: left out: %"PRId64" lone end-of-sequence codes, %"PRId64
               " header groups without a frame\n", s->ends_dropped, s->headers_dropped);
}

const DRVideoRules ff_discrip_video_vc1 = {
    .picture       = picture_vc1,
    .close         = close_vc1,
    .order_period  = 0x1000000,
    .counted_order = 1,
    .priv_size     = sizeof(Vc1State),
};
