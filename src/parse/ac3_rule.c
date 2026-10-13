/*
 * AC-3 / E-AC-3 (ATSC A/52): the disc reader's frame rules, and the stream
 * header check (dr_ac3_stream_header). Moved out of our FFmpeg's
 * ac3_parser.c disc mode; the header math is our own (A/52).
 */

#include <stdint.h>

#include "base/bytes.h"
#include "base/crc16.h"

#include "parse/bits.h"
#include "parse/rules.h"

/* ---- Disc mode (PARSER_FLAG_DISC): the disc reader's AC-3 frame rules ----
 * A syncframe starts with 0x0B77. bsid 10..16: E-AC-3 (the frame size field
 * only); bsid 0..8: AC-3 (sample rate code 0..2, frame size code 0..37, as
 * the header parse here); bsid 9 and 17..: no frame. The frames taken: AC-3
 * only for an AC-3 stream, E-AC-3 only for an E-AC-3 one, both with
 * PARSER_FLAG_DISC_ANY. Where no frame starts, the next one is a 0x0B77 whose
 * header is valid, whose bytes are there, whose stream header is valid
 * (E-AC-3: through its channel map) and, for AC-3, whose two CRCs give 0;
 * with 0xA000 bytes or more and none, the kept bytes are given up. At most
 * 0x4000 bytes are kept while looking for a frame. */

#define DISC_LIMIT      0x4000
#define DISC_GIVE_UP    0xa000

static int disc_is_eac3(int bsid)
{
    return bsid >= 10 && bsid <= 16;
}

/* AC-3 frame sizes in 16-bit words (A/52 Table 5.18) by frame size code
 * and sample rate code. */
static const uint16_t frame_sizes[38][3] = {
    { 64,   69,   96   }, { 64,   70,   96   }, { 80,   87,   120  }, { 80,   88,   120  },
    { 96,   104,  144  }, { 96,   105,  144  }, { 112,  121,  168  }, { 112,  122,  168  },
    { 128,  139,  192  }, { 128,  140,  192  }, { 160,  174,  240  }, { 160,  175,  240  },
    { 192,  208,  288  }, { 192,  209,  288  }, { 224,  243,  336  }, { 224,  244,  336  },
    { 256,  278,  384  }, { 256,  279,  384  }, { 320,  348,  480  }, { 320,  349,  480  },
    { 384,  417,  576  }, { 384,  418,  576  }, { 448,  487,  672  }, { 448,  488,  672  },
    { 512,  557,  768  }, { 512,  558,  768  }, { 640,  696,  960  }, { 640,  697,  960  },
    { 768,  835,  1152 }, { 768,  836,  1152 }, { 896,  975,  1344 }, { 896,  976,  1344 },
    { 1024, 1114, 1536 }, { 1024, 1115, 1536 }, { 1152, 1253, 1728 }, { 1152, 1254, 1728 },
    { 1280, 1393, 1920 }, { 1280, 1394, 1920 },
};

/* The frame size and bsid: 1 a valid header, 0 not. AC-3 (A/52 5.4.1): the
 * sample rate code must not be 3 and the frame size code at most 37. */
static int disc_header(const uint8_t *b, int len, int *size, int *bsid)
{
    int fscod, frmsizecod;

    if (len < 7)
        return 0;
    *bsid = b[5] >> 3;
    if (disc_is_eac3(*bsid)) {
        *size = ((dr_rb16(b + 2) & 0x7ff) + 1) * 2;
        return 1;
    }
    if (b[5] >= 0x48)
        return 0;
    fscod      = b[4] >> 6;
    frmsizecod = b[4] & 0x3f;
    if (fscod == 3 || frmsizecod > 37)
        return 0;
    *size = frame_sizes[frmsizecod][fscod] * 2;
    return 1;
}

/* A/52 crc1 and crc2: bytes 2 .. 5/8 of the frame, then the remaining 3/8,
 * each give a remainder of 0. */
static int disc_crc_ok(const uint8_t *b, int size)
{
    int n58 = size * 5 >> 3;

    return !dr_crc16_ansi(0, b + 2, n58 - 2) && !dr_crc16_ansi(0, b + n58, size * 3 >> 3);
}

/* The stream header from a frame: E-AC-3 bitstream information up to the
 * channel map (the frame must hold it; a dependent stream's channel map must
 * give the channels its audio coding mode and LFE give); AC-3: the syncinfo. */
int dr_ac3_stream_header(const uint8_t *b, int len)
{
    static const uint8_t acmod_channels[8] = { 2, 1, 2, 3, 3, 4, 4, 5 };
    /* bit 0 (first) .. 15: L C R Ls Rs Lc/Rc Lrs/Rrs Cs Ts Lsd/Rsd Lw/Rw Lvh/Rvh Cvh Lts/Rts LFE2 LFE */
    static const uint8_t weight[16] = { 1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1, 2, 1, 1 };
    DrBits gb;
    unsigned strmtyp, acmod, lfeon, chanmap = 0, chanmape = 0;
    int size, bsid, expect = 0;

    if (!disc_header(b, len, &size, &bsid))
        return 0;
    if (!disc_is_eac3(bsid))
        return 1;
    if (dr_bits_init8(&gb, b + 2, len - 2) < 0)
        return 0;
    strmtyp = dr_bits_get(&gb, 2);
    dr_bits_skip(&gb, 3 + 11 + 2 + 2);        /* substreamid, frmsiz, fscod, fscod2 / numblkscod */
    acmod = dr_bits_get(&gb, 3);
    lfeon = dr_bits_get1(&gb);
    dr_bits_skip(&gb, 5 + 5);                 /* bsid, dialnorm */
    if (dr_bits_get1(&gb))                         /* compre */
        dr_bits_skip(&gb, 8);
    if (!acmod) {
        dr_bits_skip(&gb, 5);
        if (dr_bits_get1(&gb))
            dr_bits_skip(&gb, 8);
    }
    if (strmtyp == 1 && (chanmape = dr_bits_get1(&gb)))
        chanmap = dr_bits_get(&gb, 16);
    if (dr_bits_left(&gb) < 0)
        return 0;
    if (!chanmape)
        return 1;
    for (int i = 0; i < 16; i++)
        if (chanmap & (0x8000 >> i))
            expect += weight[i];
    return expect == acmod_channels[acmod] + lfeon;
}

static int disc_kind_ok(const DrAc3RuleCtx *c, int bsid)
{
    return !c->kind || disc_is_eac3(bsid) + 1 == c->kind;
}

static int disc_sync(void *ctx, const uint8_t *b, int len)
{
    int size, bsid;

    if (len < 2)
        return 2;
    if (b[0] != 0x0b || b[1] != 0x77)
        return 0;
    if (len < 7)
        return 2;
    return disc_header(b, len, &size, &bsid) && disc_kind_ok(ctx, bsid);
}

static int disc_size(void *ctx, const uint8_t *b, int len)
{
    int size, bsid;

    if (len < 7)
        return -1;
    return disc_header(b, len, &size, &bsid) ? size : -2;
}

static int disc_check(void *ctx, const uint8_t *b, int len)
{
    int size, bsid;

    return disc_header(b, len, &size, &bsid) && disc_kind_ok(ctx, bsid);
}

static int disc_resync(void *ctx, const uint8_t *b, int len)
{
    if (len < 7)
        return -2;
    for (int pos = 0; pos < len - 2; pos++) {
        int rem = len - pos, size, bsid;

        if (b[pos] != 0x0b || b[pos + 1] != 0x77)
            continue;
        if (rem < 8)
            break;
        if (!disc_header(b + pos, rem, &size, &bsid))
            return pos + 1;
        if (rem < size)
            return -2;
        if (!dr_ac3_stream_header(b + pos, size))
            return pos + 1;
        if (disc_is_eac3(bsid))
            return pos;
        return pos + !disc_crc_ok(b + pos, size);
    }
    return len >= DISC_GIVE_UP + 1 ? -1 : -2;
}

const DrFrameRule dr_ac3_frames = { disc_sync, disc_size, disc_check, disc_resync, DISC_LIMIT };
