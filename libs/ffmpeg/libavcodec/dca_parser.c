/*
 * DCA parser
 * Copyright (C) 2004 Gildas Bazin
 * Copyright (C) 2004 Benjamin Zores
 * Copyright (C) 2006 Benjamin Larsson
 * Copyright (C) 2007 Konstantin Shishkov
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/intreadwrite.h"

#include "dca.h"
#include "dca_core.h"
#include "dca_exss.h"
#include "dca_lbr.h"
#include "dca_syncwords.h"
#include "get_bits.h"
#include "parser.h"
#include "parser_internal.h"

typedef struct DCAParseContext {
    ParseContext pc;
    uint32_t lastmarker;
    int size;
    int framesize;
    unsigned int startpos;
    DCAExssParser exss;
    unsigned int sr_code;

    /* disc mode (PARSER_FLAG_DISC) */
    FFDiscFrames disc;
    int disc_started;
    int disc_undecided;     /**< DTS-HD rules: whether an extension substream follows each core frame is still open */
    int disc_ext;           /**< and it does */
} DCAParseContext;

#define IS_CORE_MARKER(state) \
    (((state & 0xFFFFFFFFF0FF) == (((uint64_t)DCA_SYNCWORD_CORE_14B_LE << 16) | 0xF007)) || \
     ((state & 0xFFFFFFFFFFF0) == (((uint64_t)DCA_SYNCWORD_CORE_14B_BE << 16) | 0x07F0)) || \
     ((state & 0xFFFFFFFF00FC) == (((uint64_t)DCA_SYNCWORD_CORE_LE     << 16) | 0x00FC)) || \
     ((state & 0xFFFFFFFFFC00) == (((uint64_t)DCA_SYNCWORD_CORE_BE     << 16) | 0xFC00)))

#define IS_EXSS_MARKER(state)   ((state & 0xFFFFFFFF) == DCA_SYNCWORD_SUBSTREAM)

#define IS_MARKER(state)        (IS_CORE_MARKER(state) || IS_EXSS_MARKER(state))

#define CORE_MARKER(state)      ((state >> 16) & 0xFFFFFFFF)
#define EXSS_MARKER(state)      (state & 0xFFFFFFFF)

#define STATE_LE(state)     (((state & 0xFF00FF00) >> 8) | ((state & 0x00FF00FF) << 8))
#define STATE_14(state)     (((state & 0x3FFF0000) >> 8) | ((state & 0x00003FFF) >> 6))

#define CORE_FRAMESIZE(state)   (((state >> 4) & 0x3FFF) + 1)
#define EXSS_FRAMESIZE(state)   ((state & 0x2000000000) ? \
                                 ((state >>  5) & 0xFFFFF) + 1 : \
                                 ((state >> 13) & 0x0FFFF) + 1)

/**
 * Find the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or -1
 */
static int dca_find_frame_end(DCAParseContext *pc1, const uint8_t *buf,
                              int buf_size)
{
    int start_found, size, i;
    uint64_t state;
    ParseContext *pc = &pc1->pc;

    start_found = pc->frame_start_found;
    state       = pc->state64;
    size        = pc1->size;

    i = 0;
    if (!start_found) {
        for (; i < buf_size; i++) {
            size++;
            state = (state << 8) | buf[i];

            if (IS_MARKER(state) &&
                (!pc1->lastmarker ||
                  pc1->lastmarker == CORE_MARKER(state) ||
                  pc1->lastmarker == DCA_SYNCWORD_SUBSTREAM)) {
                if (!pc1->lastmarker)
                    pc1->startpos = IS_EXSS_MARKER(state) ? size - 4 : size - 6;

                if (IS_EXSS_MARKER(state))
                    pc1->lastmarker = EXSS_MARKER(state);
                else
                    pc1->lastmarker = CORE_MARKER(state);

                start_found = 1;
                size        = 0;

                i++;
                break;
            }
        }
    }

    if (start_found) {
        for (; i < buf_size; i++) {
            size++;
            state = (state << 8) | buf[i];

            if (start_found == 1) {
                switch (pc1->lastmarker) {
                case DCA_SYNCWORD_CORE_BE:
                    if (size == 2) {
                        pc1->framesize = CORE_FRAMESIZE(state);
                        start_found    = 2;
                    }
                    break;
                case DCA_SYNCWORD_CORE_LE:
                    if (size == 2) {
                        pc1->framesize = CORE_FRAMESIZE(STATE_LE(state));
                        start_found    = 4;
                    }
                    break;
                case DCA_SYNCWORD_CORE_14B_BE:
                    if (size == 4) {
                        pc1->framesize = CORE_FRAMESIZE(STATE_14(state));
                        start_found    = 4;
                    }
                    break;
                case DCA_SYNCWORD_CORE_14B_LE:
                    if (size == 4) {
                        pc1->framesize = CORE_FRAMESIZE(STATE_14(STATE_LE(state)));
                        start_found    = 4;
                    }
                    break;
                case DCA_SYNCWORD_SUBSTREAM:
                    if (size == 6) {
                        pc1->framesize = EXSS_FRAMESIZE(state);
                        start_found    = 4;
                    }
                    break;
                default:
                    av_assert0(0);
                }
                continue;
            }

            if (start_found == 2 && IS_EXSS_MARKER(state) &&
                pc1->framesize <= size + 2) {
                pc1->framesize  = size + 2;
                start_found     = 3;
                continue;
            }

            if (start_found == 3) {
                if (size == pc1->framesize + 4) {
                    pc1->framesize += EXSS_FRAMESIZE(state);
                    start_found     = 4;
                }
                continue;
            }

            if (pc1->framesize > size)
                continue;

            if (IS_MARKER(state) &&
                (pc1->lastmarker == CORE_MARKER(state) ||
                 pc1->lastmarker == DCA_SYNCWORD_SUBSTREAM)) {
                pc->frame_start_found = 0;
                pc->state64           = -1;
                pc1->size             = 0;
                return IS_EXSS_MARKER(state) ? i - 3 : i - 5;
            }
        }
    }

    pc->frame_start_found = start_found;
    pc->state64           = state;
    pc1->size             = size;
    return END_NOT_FOUND;
}

static av_cold int dca_parse_init(AVCodecParserContext *s)
{
    DCAParseContext *pc1 = s->priv_data;

    pc1->lastmarker = 0;
    pc1->sr_code = -1;
    return 0;
}

static int dca_parse_params(DCAParseContext *pc1, const uint8_t *buf,
                            int buf_size, int *duration, int *sample_rate,
                            int *profile)
{
    DCAExssAsset *asset = &pc1->exss.assets[0];
    GetBitContext gb;
    DCACoreFrameHeader h;
    uint8_t hdr[DCA_CORE_FRAME_HEADER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    int ret, frame_size;

    if (buf_size < DCA_CORE_FRAME_HEADER_SIZE)
        return AVERROR_INVALIDDATA;

    if (AV_RB32(buf) == DCA_SYNCWORD_SUBSTREAM) {
        if ((ret = ff_dca_exss_parse(&pc1->exss, buf, buf_size)) < 0)
            return ret;

        if (asset->extension_mask & DCA_EXSS_LBR) {
            if ((ret = init_get_bits8(&gb, buf + asset->lbr_offset, asset->lbr_size)) < 0)
                return ret;

            if (get_bits_long(&gb, 32) != DCA_SYNCWORD_LBR)
                return AVERROR_INVALIDDATA;

            switch (get_bits(&gb, 8)) {
            case DCA_LBR_HEADER_DECODER_INIT:
                pc1->sr_code = get_bits(&gb, 8);
                break;
            case DCA_LBR_HEADER_SYNC_ONLY:
                break;
            default:
                return AVERROR_INVALIDDATA;
            }

            if (pc1->sr_code >= FF_ARRAY_ELEMS(ff_dca_sampling_freqs))
                return AVERROR_INVALIDDATA;

            *sample_rate = ff_dca_sampling_freqs[pc1->sr_code];
            *duration = 1024 << ff_dca_freq_ranges[pc1->sr_code];
            *profile = AV_PROFILE_DTS_EXPRESS;
            return 0;
        }

        if (asset->extension_mask & DCA_EXSS_XLL) {
            int nsamples_log2;

            if ((ret = init_get_bits8(&gb, buf + asset->xll_offset, asset->xll_size)) < 0)
                return ret;

            if (get_bits_long(&gb, 32) != DCA_SYNCWORD_XLL)
                return AVERROR_INVALIDDATA;

            if (get_bits(&gb, 4))
                return AVERROR_INVALIDDATA;

            skip_bits(&gb, 8);
            skip_bits_long(&gb, get_bits(&gb, 5) + 1);
            skip_bits(&gb, 4);
            nsamples_log2 = get_bits(&gb, 4) + get_bits(&gb, 4);
            if (nsamples_log2 > 24)
                return AVERROR_INVALIDDATA;

            *sample_rate = asset->max_sample_rate;
            *duration = (1 + (*sample_rate > 96000)) << nsamples_log2;
            *profile = AV_PROFILE_DTS_HD_MA;
            return 0;
        }

        return AVERROR_INVALIDDATA;
    }

    if ((ret = avpriv_dca_convert_bitstream(buf, DCA_CORE_FRAME_HEADER_SIZE,
                                            hdr, DCA_CORE_FRAME_HEADER_SIZE)) < 0)
        return ret;
    if (avpriv_dca_parse_core_frame_header(&h, hdr, ret) < 0)
        return AVERROR_INVALIDDATA;

    *duration = h.npcmblocks * DCA_PCMBLOCK_SAMPLES;
    *sample_rate = ff_dca_sample_rates[h.sr_code];
    if (*profile != AV_PROFILE_UNKNOWN)
        return 0;

    *profile = AV_PROFILE_DTS;
    if (h.ext_audio_present) {
        switch (h.ext_audio_type) {
        case DCA_EXT_AUDIO_XCH:
        case DCA_EXT_AUDIO_XXCH:
            *profile = AV_PROFILE_DTS_ES;
            break;
        case DCA_EXT_AUDIO_X96:
            *profile = AV_PROFILE_DTS_96_24;
            break;
        }
    }

    frame_size = FFALIGN(h.frame_size, 4);
    if (buf_size - 4 < frame_size)
        return 0;

    buf      += frame_size;
    buf_size -= frame_size;
    if (AV_RB32(buf) != DCA_SYNCWORD_SUBSTREAM)
        return 0;
    if (ff_dca_exss_parse(&pc1->exss, buf, buf_size) < 0)
        return 0;

    if (asset->extension_mask & DCA_EXSS_XLL)
        *profile = AV_PROFILE_DTS_HD_MA;
    else if (asset->extension_mask & (DCA_EXSS_XBR | DCA_EXSS_XXCH | DCA_EXSS_X96))
        *profile = AV_PROFILE_DTS_HD_HRA;

    return 0;
}

/* ---- Disc mode (PARSER_FLAG_DISC): the disc reader's DTS frame rules ----
 * DTS: a frame is a core frame (sync word 7F FE 80 01, 11 bytes or more; as
 * long as its FSIZE + 1). DTS-HD (PARSER_FLAG_DISC_ANY): whether an extension
 * substream (64 58 20 25, ETSI TS 102 114 7.4) follows is decided once, after
 * the first core frame; then a frame is a core frame with the extension
 * substream after it (its header valid through the first asset's
 * descriptor). Where no frame starts, the kept bytes are given up. At most
 * 0x4000 (DTS) / 0x14000 (DTS-HD) bytes are kept while looking for a frame. */

static int disc_core_ok(const uint8_t *b, int len)
{
    return len >= 11 && AV_RB32(b) == DCA_SYNCWORD_CORE_BE;
}

static int disc_core_size(const uint8_t *b)
{
    return (AV_RB24(b + 5) >> 4 & 0x3fff) + 1;
}

/* Extension substream header: the fields the rules need. */
typedef struct DiscExtSS {
    int hdr_size;           /**< header bytes */
    int frame_size;         /**< bytes of the substream frame */
    int nb_assets;
} DiscExtSS;

/* The start of the header: user bits, substream index, header size, frame
 * size. 0 without the sync word, with fewer than 10 bytes or past the end. */
static int disc_extss_start(DiscExtSS *h, GetBitContext *r, const uint8_t *b, int len, int *idx, int *blong)
{
    if (len < 10 || AV_RB32(b) != DCA_SYNCWORD_SUBSTREAM || init_get_bits8(r, b + 4, len - 4) < 0)
        return 0;
    skip_bits(r, 8);                            /* UserDefinedBits */
    *idx   = get_bits(r, 2);                    /* nExtSSIndex */
    *blong = get_bits1(r);                      /* bHeaderSizeType */
    if (get_bits_left(r) < 0)
        return 0;
    h->hdr_size   = get_bits(r, *blong ? 12 : 8) + 1;
    h->frame_size = get_bits_long(r, *blong ? 20 : 16) + 1;
    return get_bits_left(r) >= 0;
}

/* The whole header (through the first asset's descriptor when the static
 * fields are present): 1 valid, 0 not (also when it runs past the data). */
static int disc_extss_header(const uint8_t *b, int len)
{
    DiscExtSS h;
    GetBitContext gb;
    int idx, blong, stat, nb_pres, chans;
    unsigned ss_mask[8] = { 0 };

#define OVERRUN (get_bits_left(&gb) < 0)
    if (!disc_extss_start(&h, &gb, b, len, &idx, &blong) || h.hdr_size > len)
        return 0;
    stat = get_bits1(&gb);                      /* bStaticFieldsPresent */
    if (OVERRUN)
        return 0;
    nb_pres = h.nb_assets = 1;
    if (stat) {
        skip_bits_long(&gb, 2);                 /* reference clock */
        skip_bits_long(&gb, 3);                 /* frame duration */
        if (get_bits1(&gb)) {                   /* time stamp */
            skip_bits_long(&gb, 32);
            skip_bits_long(&gb, 4);
        }
        nb_pres     = get_bits(&gb, 3) + 1;
        h.nb_assets = get_bits(&gb, 3) + 1;
        if (OVERRUN)
            return 0;
        for (int i = 0; i < nb_pres; i++)
            ss_mask[i] = get_bits_long(&gb, idx + 1);
        /* active asset masks: read in steps of 2 substreams, each time as bit 0
         * of the presentation's substream mask says */
        for (int i = 0; i < nb_pres; i++)
            for (int n = 0; n < idx + 1; n += 2)
                if (ss_mask[i] & 1)
                    skip_bits_long(&gb, 8);
        if (get_bits1(&gb)) {                   /* mixing metadata */
            int a, c;
            if (OVERRUN)
                return 0;
            skip_bits_long(&gb, 2);
            a = get_bits(&gb, 2);
            c = get_bits(&gb, 2);
            if (OVERRUN)
                return 0;
            skip_bits_long(&gb, (a + 1) * 4 * (c + 1));
        } else if (OVERRUN) {
            return 0;
        }
    }
    if (OVERRUN)
        return 0;
    for (int k = 0; k < h.nb_assets; k++)
        skip_bits_long(&gb, blong ? 20 : 16);  /* asset sizes */
    for (int k = 0; k < h.nb_assets; k++) {
        skip_bits_long(&gb, 9);                 /* descriptor size */
        skip_bits_long(&gb, 3);                 /* asset index */
        if (!stat)
            continue;
        if (get_bits1(&gb))                     /* asset type */
            skip_bits_long(&gb, 4);
        if (get_bits1(&gb))                     /* language */
            skip_bits_long(&gb, 24);
        if (get_bits1(&gb)) {                   /* info text */
            int n;
            if (OVERRUN)
                return 0;
            n = get_bits(&gb, 10) + 1;
            skip_bits_long(&gb, 8 * n);
        } else if (OVERRUN) {
            return 0;
        }
        skip_bits_long(&gb, 5);                 /* bit resolution */
        skip_bits_long(&gb, 4);                 /* maximum sample rate */
        chans = get_bits(&gb, 8) + 1;
        if (get_bits1(&gb)) {                   /* one-to-one map */
            if (chans >= 3)
                skip_bits_long(&gb, 1);
            if (chans >= 7)
                skip_bits_long(&gb, 1);
            if (get_bits1(&gb))                 /* speaker mask */
                skip_bits_long(&gb, (get_bits(&gb, 2) + 1) * 4);
        }
        break;                                  /* only the first asset's descriptor */
    }
    return !OVERRUN;
#undef OVERRUN
}

static int disc_core_sync(void *ctx, const uint8_t *b, int len)
{
    if (len < 4)
        return 2;
    if (AV_RB32(b) != DCA_SYNCWORD_CORE_BE)
        return 0;
    return len < 11 ? 2 : 1;
}

static int disc_extss_sync(const uint8_t *b, int len)
{
    DiscExtSS h;
    GetBitContext r;
    int idx, blong;

    if (len < 4)
        return 2;
    if (AV_RB32(b) != DCA_SYNCWORD_SUBSTREAM)
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
    DCAParseContext *c = ctx;
    DiscExtSS h;
    GetBitContext r;
    int n, sy, idx, blong;

    if (!disc_core_ok(b, len))
        return -1;
    n = disc_core_size(b);
    if (c->disc_undecided) {
        if (n >= len)
            return -1;
        if ((sy = disc_extss_sync(b + n, len - n)) == 2)
            return -1;
        c->disc_undecided = 0;
        c->disc_ext       = sy == 1;
    }
    if (!c->disc_ext)
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
    DCAParseContext *c = ctx;
    int n;

    if (!disc_core_ok(b, len))
        return 0;
    if (!c->disc_ext)
        return 1;
    n = disc_core_size(b);
    if (n + 1 > len || disc_extss_sync(b + n, len - n) != 1)
        return 0;
    return disc_extss_header(b + n, len - n);
}

static const FFDiscFrameRule disc_dts_rule = {
    disc_core_sync, disc_core_frame_size, disc_core_check, disc_give_up, 0x4000,
};

static const FFDiscFrameRule disc_dtshd_rule = {
    disc_core_sync, disc_hd_size, disc_hd_check, disc_give_up, 0x14000,
};

static int dca_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    DCAParseContext *pc1 = s->priv_data;
    ParseContext *pc = &pc1->pc;
    int next, duration, sample_rate;

    if (s->flags & PARSER_FLAG_DISC) {
        if (!pc1->disc_started) {
            pc1->disc_started   = 1;
            pc1->disc_undecided = 1;
        }
        return ff_parser_disc_frames(s, pc1, s->flags & PARSER_FLAG_DISC_ANY ? &disc_dtshd_rule : &disc_dts_rule,
                                     &pc1->disc, poutbuf, poutbuf_size, buf, buf_size);
    }

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        next = dca_find_frame_end(pc1, buf, buf_size);

        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }

        /* skip initial padding */
        if (buf_size  > pc1->startpos) {
            buf      += pc1->startpos;
            buf_size -= pc1->startpos;
        }
        pc1->startpos = 0;
    }

    /* read the duration and sample rate from the frame header */
    if (!dca_parse_params(pc1, buf, buf_size, &duration, &sample_rate, &avctx->profile)) {
        if (!avctx->sample_rate)
            avctx->sample_rate = sample_rate;
        s->duration = av_rescale(duration, avctx->sample_rate, sample_rate);
    } else
        s->duration = 0;

    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return next;
}

static av_cold void dca_parse_close(AVCodecParserContext *s)
{
    DCAParseContext *pc1 = s->priv_data;

    ff_parser_disc_cut_close(&pc1->disc.cut);
    ff_parse_close(s);
}

const FFCodecParser ff_dca_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_DTS),
    .priv_data_size = sizeof(DCAParseContext),
    .init           = dca_parse_init,
    .parse          = dca_parse,
    .close          = dca_parse_close,
};
