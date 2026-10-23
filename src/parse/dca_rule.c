/*
 * DTS / DTS-HD (ETSI TS 102 114): the disc reader's frame rules, and the
 * first-frame check (dr_dca_first_frame). Moved out of our FFmpeg's
 * dca_parser.c disc mode; the core and extension substream header math is
 * our own (the sync words are the specification's).
 */

#include <stdint.h>
#include <string.h>

#include "base/bytes.h"
#include "base/crc16.h"
#include "base/err.h"

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

/* ---- The timeline's header read and the DTS track's core filter ----
 * dr_dca_core_header() reads what the timeline uses of a core frame's
 * header (ETSI TS 102 114 5.4), accepting exactly the frames the processing
 * library's header parser accepted: the sync word, a deficit of 32 samples,
 * PCM blocks a multiple of 8, a frame size of at least 96 bytes, a known
 * audio mode, sample rate, LFE flag and PCM resolution, the reserved bit 0. */

int dr_dca_core_header(const uint8_t *b, int len, DrDcaHeader *h)
{
    /* core sample rates by SFREQ (Table 5-5) and source resolutions (5.4.1) */
    static const int core_rates[16] = { 0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100,
                                        0, 0, 12000, 24000, 48000, 0, 0 };
    static const uint8_t pcm_bits[8] = { 16, 16, 20, 20, 0, 24, 24, 0 };
    DrBits g;
    int npcmblocks, amode, sr_code, crc_present, lfe, pcmr;

    h->rate = h->samples = h->frame_size = 0;
    if (dr_bits_init8(&g, b, len) < 0 || dr_bits_get(&g, 32) != 0x7FFE8001U)
        return DR_ERR_INVALID;
    dr_bits_get1(&g);                       /* normal frame flag */
    if (dr_bits_get(&g, 5) + 1 != 32)       /* deficit samples */
        return DR_ERR_INVALID;
    crc_present = dr_bits_get1(&g);
    npcmblocks  = dr_bits_get(&g, 7) + 1;
    if (npcmblocks & 7)                     /* not a multiple of 8 subband samples */
        return DR_ERR_INVALID;
    h->frame_size = dr_bits_get(&g, 14) + 1;
    if (h->frame_size < 96)
        return DR_ERR_INVALID;
    amode = dr_bits_get(&g, 6);
    if (amode >= 16)
        return DR_ERR_INVALID;
    sr_code = dr_bits_get(&g, 4);
    if (!core_rates[sr_code])
        return DR_ERR_INVALID;
    dr_bits_get(&g, 5);                     /* bit rate code */
    if (dr_bits_get1(&g))                   /* reserved bit */
        return DR_ERR_INVALID;
    dr_bits_skip(&g, 1 + 1 + 1 + 1 + 3 + 1 + 1);    /* drc, timestamp, aux, hdcd, ext type, ext, ssf sync */
    lfe = dr_bits_get(&g, 2);
    if (lfe == 3)
        return DR_ERR_INVALID;
    dr_bits_get1(&g);                       /* predictor history */
    if (crc_present)
        dr_bits_skip(&g, 16);
    dr_bits_skip(&g, 1 + 4 + 2);            /* filter, encoder revision, copy history */
    pcmr = dr_bits_get(&g, 3);
    if (!pcm_bits[pcmr])
        return DR_ERR_INVALID;
    h->rate    = core_rates[sr_code];
    h->samples = npcmblocks * 32;
    return 0;
}

int dr_dca_core(const uint8_t *data, int size, int *off, int *out_size)
{
    *off      = 0;
    *out_size = size;
    if (size >= 8 && dr_rb32(data) == 0x7FFE8001U) {
        int core_size = (dr_rb24(data + 5) >> 4 & 0x3fff) + 1;
        if (core_size <= size)
            *out_size = core_size;
    }
    return 0;
}

/* ---- DTS-HD without a core (DTS Express, a lossless-only stream) ----
 * The disc says a stream has no core substream: each frame is one extension
 * substream frame (64 58 20 25, as long as its header's frame size), valid
 * when its header reads through the first asset's descriptor (as an
 * extension substream after a core frame). Where no frame starts, the kept
 * bytes are given up. */

static int disc_exss_sync(void *ctx, const uint8_t *b, int len)
{
    return disc_extss_sync(b, len);
}

static int disc_exss_size(void *ctx, const uint8_t *b, int len)
{
    DiscExtSS h;
    DrBits r;
    int idx, blong;

    if (len < 10 || !disc_extss_start(&h, &r, b, len, &idx, &blong))
        return -1;
    return h.frame_size;
}

static int disc_exss_check(void *ctx, const uint8_t *b, int len)
{
    DiscExtSS h;

    return disc_extss_header(&h, b, len) && h.frame_size <= len;
}

const DrFrameRule dr_dts_exss_frames = {
    disc_exss_sync, disc_exss_size, disc_exss_check, disc_give_up, 0x14000,
};

/* ---- A core-less frame's timing ----
 * Ported from FFmpeg 9.0.2's libavcodec/dca_exss.c (ff_dca_exss_parse,
 * parse_descriptor, set_exss_offsets: the first asset's component offsets)
 * and dca_parser.c (dca_parse_params: the LBR and XLL headers' rate and
 * frame length), with the tables of dca.c. */

enum {
    EXSS_CORE = 0x010, EXSS_XBR = 0x020, EXSS_XXCH = 0x040, EXSS_X96 = 0x080,
    EXSS_LBR  = 0x100, EXSS_XLL = 0x200, EXSS_RSV1 = 0x400, EXSS_RSV2 = 0x800,
};

static const uint32_t sampling_freqs[16] = {
      8000,  16000, 32000, 64000, 128000, 22050,  44100,  88200,
    176400, 352800, 12000, 24000,  48000, 96000, 192000, 384000,
};

static const uint8_t freq_ranges[16] = { 0, 1, 2, 3, 4, 1, 2, 3, 4, 4, 0, 1, 2, 3, 4, 4 };

/* ff_dca_count_chs_for_mask */
static int count_chs_for_mask(unsigned mask)
{
    return __builtin_popcount((mask & 0xffff) | ((mask & 0xae66) << 16));
}

/* ff_dca_seek_bits: to bit p, not back and not past the end */
static int seek_bits(DrBits *g, int p)
{
    if (p < g->pos || p > g->size_bits)
        return -1;
    g->pos = p;
    return 0;
}

typedef struct ExssAsset {
    int offset, size;
    unsigned mask;
    int asset_type;                         /* the asset type descriptor, -1 not present */
    int max_sample_rate;
    int core_size, xbr_size, xxch_size, x96_size, lbr_size, xll_size;
    int lbr_offset, xll_offset;
} ExssAsset;

static void lbr_parameters(DrBits *g, ExssAsset *a)
{
    a->lbr_size = dr_bits_get(g, 14) + 1;
    if (dr_bits_get1(g))                    /* LBR sync word present */
        dr_bits_skip(g, 2);
}

static void xll_parameters(DrBits *g, ExssAsset *a, int size_nbits)
{
    a->xll_size = dr_bits_get(g, size_nbits) + 1;
    if (dr_bits_get1(g)) {                  /* XLL sync word present */
        int delay_nbits;
        dr_bits_skip(g, 4);                 /* peak bit rate smoothing buffer size */
        delay_nbits = dr_bits_get(g, 5) + 1;
        dr_bits_skip(g, delay_nbits);       /* initial decoding delay */
        dr_bits_skip(g, size_nbits);        /* offset to the XLL sync */
    }
}

/* The extension substream header through the first asset's descriptor and
 * its components' offsets: 0, or -1 when the processing library would not
 * read it (more than one presentation or asset, sizes out of bounds). */
static int exss_first_asset(const uint8_t *b, int len, ExssAsset *a)
{
    DrBits g;
    int idx, wide, header_size, size_nbits, exss_size, stat, mix = 0, nmixout = 0, nmixoutchs[4] = { 0 };
    int nch = 0, estereo = 0, e6 = 0, drc, descr_pos, descr_size, offs, left;

    memset(a, 0, sizeof(*a));
    a->asset_type = -1;
    if (dr_bits_init8(&g, b, len) < 0)
        return -1;
    dr_bits_skip(&g, 32 + 8);               /* sync word, user defined bits */
    idx         = dr_bits_get(&g, 2);
    wide        = dr_bits_get1(&g);
    header_size = dr_bits_get(&g, 8 + 4 * wide) + 1;
    size_nbits  = 16 + 4 * wide;
    exss_size   = dr_bits_get(&g, size_nbits) + 1;
    if (exss_size > len)
        return -1;
    if ((stat = dr_bits_get1(&g))) {        /* static fields */
        int npresents, nassets, active[8];
        dr_bits_skip(&g, 2 + 3);            /* reference clock, frame duration */
        if (dr_bits_get1(&g))
            dr_bits_skip(&g, 36);           /* timecode */
        if ((npresents = dr_bits_get(&g, 3) + 1) > 1)
            return -1;
        if ((nassets = dr_bits_get(&g, 3) + 1) > 1)
            return -1;
        for (int i = 0; i < npresents; i++)
            active[i] = dr_bits_get(&g, idx + 1);
        for (int i = 0; i < npresents; i++)
            dr_bits_skip(&g, __builtin_popcount(active[i]) * 8);
        if ((mix = dr_bits_get1(&g))) {
            int mask_nbits;
            dr_bits_skip(&g, 2);
            mask_nbits = (dr_bits_get(&g, 2) + 1) << 2;
            nmixout = dr_bits_get(&g, 2) + 1;
            for (int i = 0; i < nmixout; i++)
                nmixoutchs[i] = count_chs_for_mask(dr_bits_get(&g, mask_nbits));
        }
    }
    a->offset = header_size;
    a->size   = dr_bits_get(&g, size_nbits) + 1;
    if (a->offset + a->size > exss_size)
        return -1;

    /* the asset descriptor */
    descr_pos  = g.pos;
    descr_size = dr_bits_get(&g, 9) + 1;
    dr_bits_skip(&g, 3);                    /* asset index */
    if (stat) {
        if (dr_bits_get1(&g))
            a->asset_type = dr_bits_get(&g, 4);     /* asset type */
        if (dr_bits_get1(&g))
            dr_bits_skip(&g, 24);           /* language */
        if (dr_bits_get1(&g)) {             /* additional text */
            int text = dr_bits_get(&g, 10) + 1;
            if (dr_bits_left(&g) < text * 8)
                return -1;
            dr_bits_skip(&g, text * 8);
        }
        dr_bits_skip(&g, 5);                /* PCM bit resolution */
        a->max_sample_rate = sampling_freqs[dr_bits_get(&g, 4)];
        nch = dr_bits_get(&g, 8) + 1;
        if (dr_bits_get1(&g)) {             /* one-to-one channel to speaker map */
            int mask_nbits = 0, nsets, nspeakers[8];
            estereo = nch > 2 && dr_bits_get1(&g);
            e6      = nch > 6 && dr_bits_get1(&g);
            if (dr_bits_get1(&g)) {         /* speaker mask */
                mask_nbits = (dr_bits_get(&g, 2) + 1) << 2;
                dr_bits_skip(&g, mask_nbits);
            }
            if ((nsets = dr_bits_get(&g, 3)) && !mask_nbits)
                return -1;
            for (int i = 0; i < nsets; i++)
                nspeakers[i] = count_chs_for_mask(dr_bits_get(&g, mask_nbits));
            for (int i = 0; i < nsets; i++) {
                int nch_remap = dr_bits_get(&g, 5) + 1;
                for (int j = 0; j < nspeakers[i]; j++)
                    dr_bits_skip(&g, __builtin_popcount(dr_bits_get(&g, nch_remap)) * 5);
            }
        } else {
            dr_bits_skip(&g, 3);            /* representation type */
        }
    }
    drc = dr_bits_get1(&g);
    if (drc)
        dr_bits_skip(&g, 8);
    if (dr_bits_get1(&g))
        dr_bits_skip(&g, 5);                /* dialog normalization */
    if (drc && estereo)
        dr_bits_skip(&g, 8);
    if (mix && dr_bits_get1(&g)) {          /* mixing metadata */
        int ndmix;
        dr_bits_skip(&g, 1 + 6);
        if (dr_bits_get(&g, 2) == 3)
            dr_bits_skip(&g, 8);
        else
            dr_bits_skip(&g, 3);
        if (dr_bits_get1(&g))
            for (int i = 0; i < nmixout; i++)
                dr_bits_skip(&g, 6 * nmixoutchs[i]);
        else
            dr_bits_skip(&g, 6 * nmixout);
        ndmix = nch + (e6 ? 6 : 0) + (estereo ? 2 : 0);
        for (int i = 0; i < nmixout; i++) {
            if (!nmixoutchs[i])
                return -1;
            for (int j = 0; j < ndmix; j++)
                dr_bits_skip(&g, __builtin_popcount(dr_bits_get(&g, nmixoutchs[i])) * 6);
        }
    }
    switch (dr_bits_get(&g, 2)) {           /* coding mode */
    case 0:                                 /* components */
        a->mask = dr_bits_get(&g, 12);
        if (a->mask & EXSS_CORE) {
            a->core_size = dr_bits_get(&g, 14) + 1;
            if (dr_bits_get1(&g))
                dr_bits_skip(&g, 2);
        }
        if (a->mask & EXSS_XBR)
            a->xbr_size = dr_bits_get(&g, 14) + 1;
        if (a->mask & EXSS_XXCH)
            a->xxch_size = dr_bits_get(&g, 14) + 1;
        if (a->mask & EXSS_X96)
            a->x96_size = dr_bits_get(&g, 12) + 1;
        if (a->mask & EXSS_LBR)
            lbr_parameters(&g, a);
        if (a->mask & EXSS_XLL)
            xll_parameters(&g, a, size_nbits);
        if (a->mask & EXSS_RSV1)
            dr_bits_skip(&g, 16);
        if (a->mask & EXSS_RSV2)
            dr_bits_skip(&g, 16);
        break;
    case 1:                                 /* lossless only */
        a->mask = EXSS_XLL;
        xll_parameters(&g, a, size_nbits);
        break;
    case 2:                                 /* low bit rate */
        a->mask = EXSS_LBR;
        lbr_parameters(&g, a);
        break;
    case 3:                                 /* auxiliary */
        a->mask = 0;
        dr_bits_skip(&g, 14 + 8);
        if (dr_bits_get1(&g))
            dr_bits_skip(&g, 3);
        break;
    }
    if (a->mask & EXSS_XLL)
        dr_bits_skip(&g, 3);                /* DTS-HD stream id */
    if (seek_bits(&g, descr_pos + descr_size * 8) < 0)
        return -1;

    /* the components' places in the asset (set_exss_offsets) */
    offs = a->offset;
    left = a->size;
#define PLACE(bit, sz, at)              \
    if (a->mask & (bit)) {              \
        at = offs;                      \
        if ((sz) > left)                \
            return -1;                  \
        offs += (sz);                   \
        left -= (sz);                   \
    }
    {
        int unused;
        PLACE(EXSS_CORE, a->core_size, unused)
        PLACE(EXSS_XBR, a->xbr_size, unused)
        PLACE(EXSS_XXCH, a->xxch_size, unused)
        PLACE(EXSS_X96, a->x96_size, unused)
        (void)unused;
    }
    PLACE(EXSS_LBR, a->lbr_size, a->lbr_offset)
    PLACE(EXSS_XLL, a->xll_size, a->xll_offset)
#undef PLACE
    return seek_bits(&g, header_size * 8) < 0 ? -1 : 0;
}

/* ---- The lossless extension (XLL, ETSI TS 102 114 clause 8) walked
 * without decoding: the common header, the channel set sub-headers (each
 * its own size and CRC-16), the NAVI table (every band's segment sizes, its
 * CRC-16 after it), and where the band data the NAVI table sizes ends: the
 * DTS:X extension data starts there, at the next 32-bit boundary from the
 * frame's start, with its marker. Each CRC-16 is CCITT from 0xFFFF over the
 * part with its CRC at the end (the remainder 0). ---- */

#define XLL_SYNC 0x41A29547U

enum { XLL_WALKED = 1, XLL_SKIPPED = 0, XLL_BAD = -1 };

/* xll: the XLL frame (its sync word first), n the bytes there are of it.
 * XLL_WALKED (*marker: the DTS:X marker or 0), XLL_SKIPPED (the frame runs
 * past the bytes there are, or a channel set above 96 kHz may have more
 * frequency bands than its rate says), XLL_BAD (*why: the check that failed) */
static int xll_walk(const uint8_t *xll, int n, uint32_t *marker, const char **why)
{
    DrBits g;
    int hs, frame_size, nchsets, nsegs, seg_nbits, pos, bands[16], max_bands = 1, navi_bits, navi_bytes;
    int64_t data = 0;

    *marker = 0;
    if (n < 16 || dr_rb32(xll) != XLL_SYNC || dr_bits_init8(&g, xll, n) < 0)
        return XLL_SKIPPED;
    dr_bits_skip(&g, 32 + 4);               /* sync word, version */
    hs         = dr_bits_get(&g, 8) + 1;
    frame_size = dr_bits_get(&g, dr_bits_get(&g, 5) + 1) + 1;
    nchsets    = dr_bits_get(&g, 4) + 1;
    nsegs      = 1 << dr_bits_get(&g, 4);
    dr_bits_skip(&g, 4);                    /* samples in a segment, log2 */
    seg_nbits  = dr_bits_get(&g, 5) + 1;
    if (frame_size > n || hs < 6 || hs > frame_size)
        return XLL_SKIPPED;
    /* the channel set sub-headers: size (10 bits) + 1 bytes each, its CRC
     * the last 16 bits; its sampling frequency after the channel count,
     * the residual type of each channel and two 5-bit resolutions */
    pos = hs;
    for (int i = 0; i < nchsets; i++) {
        int size, nch, rate;
        DrBits c;
        if (pos + 2 > frame_size)
            return XLL_SKIPPED;
        size = (xll[pos] << 2 | xll[pos + 1] >> 6) + 1;
        if (size < 4 || pos + size > frame_size)
            return XLL_SKIPPED;
        if (dr_crc16_ccitt(0xFFFF, xll + pos, size)) {
            *why = "a DTS-HD lossless (XLL) channel set sub-header's CRC";
            return XLL_BAD;
        }
        dr_bits_init8(&c, xll + pos, size);
        dr_bits_skip(&c, 10);
        nch  = dr_bits_get(&c, 4) + 1;
        dr_bits_skip(&c, nch + 5 + 5);
        rate = sampling_freqs[dr_bits_get(&c, 4)];
        if (rate > 96000)
            return XLL_SKIPPED;             /* 2 bands, or more with the extra bands flag */
        bands[i] = 1;
        pos += size;
    }
    /* the NAVI table: per band, segment and channel set (that has the
     * band) a segment size - 1; byte aligned; its CRC-16 */
    if (dr_bits_init8(&g, xll + pos, frame_size - pos) < 0)
        return XLL_SKIPPED;
    for (int band = 0; band < max_bands; band++)
        for (int seg = 0; seg < nsegs; seg++)
            for (int i = 0; i < nchsets; i++)
                if (bands[i] > band)
                    data += dr_bits_get(&g, seg_nbits) + 1;
    navi_bits  = g.pos;
    navi_bytes = (navi_bits + 7) / 8 + 2;
    if (pos + navi_bytes > frame_size)
        return XLL_SKIPPED;
    /* the CRC before the sizes are used: a damaged table fails here */
    if (dr_crc16_ccitt(0xFFFF, xll + pos, navi_bytes)) {
        *why = "the DTS-HD lossless (XLL) NAVI table's CRC";
        return XLL_BAD;
    }
    /* the band data, then (at a 32-bit boundary) the extension data */
    pos += navi_bytes;
    if (pos + data > frame_size)
        return XLL_SKIPPED;
    pos = (int)((pos + data + 3) & ~3);
    if (pos + 4 <= frame_size) {
        uint32_t w = dr_rb32(xll + pos);
        if (w == 0x02000850U || (w >= 0xF14000D0U && w <= 0xF14000D4U))
            *marker = w;
    }
    return XLL_WALKED;
}

int dr_dca_exss_frame_size(const uint8_t *buf, int size)
{
    DiscExtSS h;
    DrBits r;
    int idx, blong;

    if (!disc_extss_start(&h, &r, buf, size, &idx, &blong) || h.hdr_size < 7 || h.hdr_size > size ||
        h.frame_size < h.hdr_size || dr_crc16_ccitt(0xFFFF, buf + 5, h.hdr_size - 5))
        return 0;
    return h.frame_size;
}

int dr_dca_dtsx(const uint8_t *buf, int size, uint32_t *marker, int *imax_type)
{
    ExssAsset a;
    const char *why;
    int off = 0;

    *marker    = 0;
    *imax_type = 0;
    /* a core frame first: the extension substream after it */
    if (size >= 10 && dr_rb32(buf) == 0x7FFE8001U) {
        off = disc_core_size(buf);
        if (off <= 0 || off >= size)
            return 0;
    }
    if (size - off < 10 || dr_rb32(buf + off) != 0x64582025U || exss_first_asset(buf + off, size - off, &a) < 0 ||
        !(a.mask & EXSS_XLL) || a.xll_size < 16)
        return 0;
    *imax_type = a.asset_type == 13;
    return xll_walk(buf + off + a.xll_offset, a.xll_size, marker, &why) == XLL_WALKED;
}

int dr_dca_exss_checksums(const uint8_t *buf, int size, const char **why)
{
    DiscExtSS h;
    DrBits r;
    ExssAsset a;
    int idx, blong;

    if (size < 10 || !disc_extss_start(&h, &r, buf, size, &idx, &blong) || h.hdr_size > size || h.hdr_size < 7)
        return 1;
    /* the header from its 41st bit (after the sync word and the user
     * defined bits) to its end, its CRC-16 the last 16 bits */
    if (dr_crc16_ccitt(0xFFFF, buf + 5, h.hdr_size - 5)) {
        *why = "the DTS-HD extension substream header's CRC";
        return 0;
    }
    if (h.frame_size > size || exss_first_asset(buf, h.frame_size, &a) < 0)
        return 1;
    if ((a.mask & EXSS_LBR) && a.lbr_size >= 8 && dr_rb32(buf + a.lbr_offset) == 0x0A801921U) {
        /* LBR: sync word, header type (2: an 11-byte decoder init follows),
         * the frame chunk: id (0x04 with a checksum), length (16 bits when
         * bit 7 of the id is set), a 16-bit checksum = the id, the length's
         * two bytes and the chunk's bytes after the checksum, added up */
        const uint8_t *p = buf + a.lbr_offset;
        int n = a.lbr_size, i = 5, id, len;
        unsigned sum;

        if (p[4] == 2)
            i += 11;
        else if (p[4] != 1)
            return 1;
        if (i + 3 > n)
            return 1;
        id = p[i++];
        if (id & 0x80) {
            len = dr_rb16(p + i);
            i  += 2;
        } else
            len = p[i++];
        if ((id & 0x7F) == 0x04) {
            if (len < 2 || i + len > n) {
                *why = "the DTS Express frame chunk runs past the frame";
                return 0;
            }
            sum = id + (len >> 8) + (len & 0xFF);
            for (int k = 2; k < len; k++)
                sum += p[i + k];
            if ((uint16_t)sum != dr_rb16(p + i)) {
                *why = "the DTS Express frame's checksum";
                return 0;
            }
        }
    }
    if ((a.mask & EXSS_XLL) && a.xll_size >= 8 && dr_rb32(buf + a.xll_offset) == 0x41A29547U) {
        /* XLL: sync word, version (4 bits), header size - 1 (8 bits); the
         * header from its 33rd bit to its end, its CRC-16 the last 16 bits */
        const uint8_t *p = buf + a.xll_offset;
        int hs = (((p[4] & 0x0F) << 4) | (p[5] >> 4)) + 1;

        uint32_t marker;
        if (hs >= 6 && hs <= a.xll_size && dr_crc16_ccitt(0xFFFF, p + 4, hs - 4)) {
            *why = "the DTS-HD lossless (XLL) frame header's CRC";
            return 0;
        }
        /* the channel set sub-headers' and the NAVI table's CRCs */
        if (xll_walk(p, a.xll_size, &marker, why) == XLL_BAD)
            return 0;
    }
    return 1;
}

int dr_dca_exss_timing(const uint8_t *buf, int size, int *lbr_rate_code, int *rate, int *samples)
{
    ExssAsset a;
    DrBits g;

    if (size < 4 || dr_rb32(buf) != 0x64582025U || exss_first_asset(buf, size, &a) < 0)
        return DR_ERR_INVALID;

    if (a.mask & EXSS_LBR) {
        if (dr_bits_init8(&g, buf + a.lbr_offset, a.lbr_size) < 0 || dr_bits_get(&g, 32) != 0x0A801921U)
            return DR_ERR_INVALID;
        switch (dr_bits_get(&g, 8)) {
        case 2:                             /* decoder init: the rate code */
            *lbr_rate_code = dr_bits_get(&g, 8);
            break;
        case 1:                             /* sync only: the last rate code */
            break;
        default:
            return DR_ERR_INVALID;
        }
        if (*lbr_rate_code < 0 || *lbr_rate_code >= 16 || dr_bits_left(&g) < 0)
            return DR_ERR_INVALID;
        *rate    = sampling_freqs[*lbr_rate_code];
        *samples = 1024 << freq_ranges[*lbr_rate_code];
        return DR_DCA_EXSS_LBR;
    }

    if (a.mask & EXSS_XLL) {
        int nsamples_log2;
        if (dr_bits_init8(&g, buf + a.xll_offset, a.xll_size) < 0 || dr_bits_get(&g, 32) != 0x41A29547U)
            return DR_ERR_INVALID;
        if (dr_bits_get(&g, 4))             /* version */
            return DR_ERR_INVALID;
        dr_bits_skip(&g, 8);                /* header size */
        dr_bits_skip(&g, dr_bits_get(&g, 5) + 1);
        dr_bits_skip(&g, 4);
        nsamples_log2 = dr_bits_get(&g, 4);
        nsamples_log2 += dr_bits_get(&g, 4);
        if (nsamples_log2 > 24 || dr_bits_left(&g) < 0 || !a.max_sample_rate)
            return DR_ERR_INVALID;
        *rate    = a.max_sample_rate;
        *samples = (1 + (*rate > 96000)) << nsamples_log2;
        return DR_DCA_EXSS_XLL;
    }
    return DR_ERR_INVALID;
}
