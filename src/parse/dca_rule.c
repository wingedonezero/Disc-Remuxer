/*
 * DTS / DTS-HD (ETSI TS 102 114): the disc reader's frame rules, and the
 * first-frame check (dr_dca_first_frame). Moved out of our FFmpeg's
 * dca_parser.c disc mode; the core and extension substream header math is
 * our own (the sync words are the specification's).
 */

#include <stdint.h>

#include "base/bytes.h"

#include "parse/bits.h"
#include "parse/rules.h"

/* ---- The disc reader's DTS frame rules ----
 * DTS: a frame is a core frame (sync word 7F FE 80 01, 11 bytes or more; as
 * long as its FSIZE + 1). DTS-HD (dr_dts_hd_frames): whether an extension
 * substream (64 58 20 25, ETSI TS 102 114 7.4) follows is decided once, after
 * the first core frame; then a frame is a core frame with the extension
 * substream after it (its header valid through the first asset's
 * descriptor). Where no frame starts, the kept bytes are given up. At most
 * 0x4000 (DTS) / 0x14000 (DTS-HD) bytes are kept while looking for a frame. */

static int disc_core_ok(const uint8_t *b, int len)
{
    return len >= 11 && dr_rb32(b) == 0x7FFE8001U;
}

static int disc_core_size(const uint8_t *b)
{
    return (dr_rb24(b + 5) >> 4 & 0x3fff) + 1;
}

/* Extension substream header: the fields the rules need. */
typedef struct DiscExtSS {
    int hdr_size;           /**< header bytes */
    int frame_size;         /**< bytes of the substream frame */
    int nb_assets;
    int asset_size[8];      /**< bytes of each asset */
} DiscExtSS;

/* The start of the header: user bits, substream index, header size, frame
 * size. 0 without the sync word, with fewer than 10 bytes or past the end. */
static int disc_extss_start(DiscExtSS *h, DrBits *r, const uint8_t *b, int len, int *idx, int *blong)
{
    if (len < 10 || dr_rb32(b) != 0x64582025U || dr_bits_init8(r, b + 4, len - 4) < 0)
        return 0;
    dr_bits_skip(r, 8);                            /* UserDefinedBits */
    *idx   = dr_bits_get(r, 2);                    /* nExtSSIndex */
    *blong = dr_bits_get1(r);                      /* bHeaderSizeType */
    if (dr_bits_left(r) < 0)
        return 0;
    h->hdr_size   = dr_bits_get(r, *blong ? 12 : 8) + 1;
    h->frame_size = dr_bits_get(r, *blong ? 20 : 16) + 1;
    return dr_bits_left(r) >= 0;
}

/* The whole header (through the first asset's descriptor when the static
 * fields are present): 1 valid, 0 not (also when it runs past the data). */
static int disc_extss_header(DiscExtSS *ph, const uint8_t *b, int len)
{
    DiscExtSS h;
    DrBits gb;
    int idx, blong, stat, nb_pres, chans;
    unsigned ss_mask[8] = { 0 };

#define OVERRUN (dr_bits_left(&gb) < 0)
    if (!disc_extss_start(&h, &gb, b, len, &idx, &blong) || h.hdr_size > len)
        return 0;
    stat = dr_bits_get1(&gb);                      /* bStaticFieldsPresent */
    if (OVERRUN)
        return 0;
    nb_pres = h.nb_assets = 1;
    if (stat) {
        dr_bits_skip(&gb, 2);                 /* reference clock */
        dr_bits_skip(&gb, 3);                 /* frame duration */
        if (dr_bits_get1(&gb)) {                   /* time stamp */
            dr_bits_skip(&gb, 32);
            dr_bits_skip(&gb, 4);
        }
        nb_pres     = dr_bits_get(&gb, 3) + 1;
        h.nb_assets = dr_bits_get(&gb, 3) + 1;
        if (OVERRUN)
            return 0;
        for (int i = 0; i < nb_pres; i++)
            ss_mask[i] = dr_bits_get(&gb, idx + 1);
        /* active asset masks: read in steps of 2 substreams, each time as bit 0
         * of the presentation's substream mask says */
        for (int i = 0; i < nb_pres; i++)
            for (int n = 0; n < idx + 1; n += 2)
                if (ss_mask[i] & 1)
                    dr_bits_skip(&gb, 8);
        if (dr_bits_get1(&gb)) {                   /* mixing metadata */
            int a, c;
            if (OVERRUN)
                return 0;
            dr_bits_skip(&gb, 2);
            a = dr_bits_get(&gb, 2);
            c = dr_bits_get(&gb, 2);
            if (OVERRUN)
                return 0;
            dr_bits_skip(&gb, (a + 1) * 4 * (c + 1));
        } else if (OVERRUN) {
            return 0;
        }
    }
    if (OVERRUN)
        return 0;
    for (int k = 0; k < h.nb_assets; k++)
        h.asset_size[k] = dr_bits_get(&gb, blong ? 20 : 16) + 1;
    for (int k = 0; k < h.nb_assets; k++) {
        dr_bits_skip(&gb, 9);                 /* descriptor size */
        dr_bits_skip(&gb, 3);                 /* asset index */
        if (!stat)
            continue;
        if (dr_bits_get1(&gb))                     /* asset type */
            dr_bits_skip(&gb, 4);
        if (dr_bits_get1(&gb))                     /* language */
            dr_bits_skip(&gb, 24);
        if (dr_bits_get1(&gb)) {                   /* info text */
            int n;
            if (OVERRUN)
                return 0;
            n = dr_bits_get(&gb, 10) + 1;
            dr_bits_skip(&gb, 8 * n);
        } else if (OVERRUN) {
            return 0;
        }
        dr_bits_skip(&gb, 5);                 /* bit resolution */
        dr_bits_skip(&gb, 4);                 /* maximum sample rate */
        chans = dr_bits_get(&gb, 8) + 1;
        if (dr_bits_get1(&gb)) {                   /* one-to-one map */
            if (chans >= 3)
                dr_bits_skip(&gb, 1);
            if (chans >= 7)
                dr_bits_skip(&gb, 1);
            if (dr_bits_get1(&gb))                 /* speaker mask */
                dr_bits_skip(&gb, (dr_bits_get(&gb, 2) + 1) * 4);
        }
        break;                                  /* only the first asset's descriptor */
    }
    *ph = h;
    return !OVERRUN;
#undef OVERRUN
}

static int disc_core_sync(void *ctx, const uint8_t *b, int len)
{
    if (len < 4)
        return 2;
    if (dr_rb32(b) != 0x7FFE8001U)
        return 0;
    return len < 11 ? 2 : 1;
}

static int disc_extss_sync(const uint8_t *b, int len)
{
    DiscExtSS h;
    DrBits r;
    int idx, blong;

    if (len < 4)
        return 2;
    if (dr_rb32(b) != 0x64582025U)
        return 0;
    if (len < 10)
        return 2;
    return disc_extss_start(&h, &r, b, len, &idx, &blong);
}

static int disc_core_frame_size(void *ctx, const uint8_t *b, int len)
{
    return disc_core_ok(b, len) ? disc_core_size(b) : -1;
}

static int disc_core_check(void *ctx, const uint8_t *b, int len)
{
    return disc_core_ok(b, len);
}

static int disc_give_up(void *ctx, const uint8_t *b, int len)
{
    return -1;
}

/* DTS-HD: a core frame and, when one followed the first core frame, the
 * extension substream frame after it. */
static int disc_hd_size(void *ctx, const uint8_t *b, int len)
{
    DrDcaRuleCtx *c = ctx;
    DiscExtSS h;
    DrBits r;
    int n, sy, idx, blong;

    if (!disc_core_ok(b, len))
        return -1;
    n = disc_core_size(b);
    if (c->undecided) {
        if (n >= len)
            return -1;
        if ((sy = disc_extss_sync(b + n, len - n)) == 2)
            return -1;
        c->undecided = 0;
        c->ext       = sy == 1;
    }
    if (!c->ext)
        return n;
    if (n >= len)
        return -1;
    sy = disc_extss_sync(b + n, len - n);
    if (!sy)
        return -2;
    if (sy == 2)
        return -1;
    if (len - n < 10 || !disc_extss_start(&h, &r, b + n, len - n, &idx, &blong))
        return -1;
    return n + h.frame_size;
}

static int disc_hd_check(void *ctx, const uint8_t *b, int len)
{
    DrDcaRuleCtx *c = ctx;
    int n;

    if (!disc_core_ok(b, len))
        return 0;
    if (!c->ext)
        return 1;
    n = disc_core_size(b);
    if (n + 1 > len || disc_extss_sync(b + n, len - n) != 1)
        return 0;
    {
        DiscExtSS h;
        return disc_extss_header(&h, b + n, len - n);
    }
}

int dr_dca_first_frame(const uint8_t *b, int len)
{
    DiscExtSS h;
    int n, off;

    if (!disc_core_ok(b, len))
        return 0;
    n = disc_core_size(b);
    if (len <= n)
        return 1;
    if (!disc_extss_header(&h, b + n, len - n) || h.frame_size > len - n)
        return 0;
    off = h.hdr_size;
    for (int k = 0; k < h.nb_assets; k++)
        off += h.asset_size[k];
    return (unsigned)off <= (unsigned)(len - n) ? 2 : 0;
}

const DrFrameRule dr_dts_frames = {
    disc_core_sync, disc_core_frame_size, disc_core_check, disc_give_up, 0x4000,
};

const DrFrameRule dr_dts_hd_frames = {
    disc_core_sync, disc_hd_size, disc_hd_check, disc_give_up, 0x14000,
};
