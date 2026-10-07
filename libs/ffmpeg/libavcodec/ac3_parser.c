/*
 * AC-3 parser
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2003 Michael Niedermayer
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

#include "config_components.h"

#include "libavutil/channel_layout.h"
#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "parser.h"
#include "ac3defs.h"
#include "ac3tab.h"
#include "ac3_parser.h"
#include "ac3_parser_internal.h"
#include "aac_ac3_parser.h"
#include "get_bits.h"
#include "parser_internal.h"


#define AC3_HEADER_SIZE 7

#if CONFIG_AC3_PARSER

static const uint8_t eac3_blocks[4] = {
    1, 2, 3, 6
};

/**
 * Table for center mix levels
 * reference: Section 5.4.2.4 cmixlev
 */
static const uint8_t center_levels[4] = { 4, 5, 6, 5 };

/**
 * Table for surround mix levels
 * reference: Section 5.4.2.5 surmixlev
 */
static const uint8_t surround_levels[4] = { 4, 6, 7, 6 };

int ff_ac3_find_syncword(const uint8_t *buf, int buf_size)
{
    int i;

    for (i = 1; i < buf_size; i += 2) {
        if (buf[i] == 0x77 || buf[i] == 0x0B) {
            if ((buf[i] ^ buf[i-1]) == (0x77 ^ 0x0B)) {
                i--;
                break;
            } else if ((buf[i] ^ buf[i+1]) == (0x77 ^ 0x0B)) {
                break;
            }
        }
    }
    if (i >= buf_size)
        return AVERROR_INVALIDDATA;

    return i;
}

/**
 * Parse the 'sync info' and 'bit stream info' from the AC-3 bitstream.
 * GetBitContext within AC3DecodeContext must point to
 * the start of the synchronized AC-3 bitstream.
 */
static int ac3_parse_header(GetBitContext *gbc, AC3HeaderInfo *hdr)
{
    /* read the rest of the bsi. read twice for dual mono mode. */
    for (int i = 0; i < (hdr->channel_mode ? 1 : 2); i++) {
        hdr->dialog_normalization[i] = -get_bits(gbc, 5);
        hdr->compression_exists[i] = get_bits1(gbc);
        if (hdr->compression_exists[i])
            hdr->heavy_dynamic_range[i] = get_bits(gbc, 8);
        if (get_bits1(gbc))
            skip_bits(gbc, 8); //skip language code
        if (get_bits1(gbc))
            skip_bits(gbc, 7); //skip audio production information
    }

    skip_bits(gbc, 2); //skip copyright bit and original bitstream bit

    /* skip the timecodes or parse the Alternate Bit Stream Syntax */
    if (hdr->bitstream_id != 6) {
        if (get_bits1(gbc))
            skip_bits(gbc, 14); //skip timecode1
        if (get_bits1(gbc))
            skip_bits(gbc, 14); //skip timecode2
    } else {
        if (get_bits1(gbc)) {
            hdr->preferred_downmix       = get_bits(gbc, 2);
            hdr->center_mix_level_ltrt   = get_bits(gbc, 3);
            hdr->surround_mix_level_ltrt = av_clip(get_bits(gbc, 3), 3, 7);
            hdr->center_mix_level        = get_bits(gbc, 3);
            hdr->surround_mix_level      = av_clip(get_bits(gbc, 3), 3, 7);
        }
        if (get_bits1(gbc)) {
            hdr->dolby_surround_ex_mode = get_bits(gbc, 2);
            hdr->dolby_headphone_mode   = get_bits(gbc, 2);
            skip_bits(gbc, 10); // skip adconvtyp (1), xbsi2 (8), encinfo (1)
        }
    }

    /* skip additional bitstream info */
    if (get_bits1(gbc)) {
        int i = get_bits(gbc, 6);
        do {
            skip_bits(gbc, 8);
        } while (i--);
    }

    return 0;
}

static int eac3_parse_header(GetBitContext *gbc, AC3HeaderInfo *hdr)
{
    if (hdr->frame_type == EAC3_FRAME_TYPE_RESERVED)
        return AC3_PARSE_ERROR_FRAME_TYPE;
    if (hdr->substreamid)
        return AC3_PARSE_ERROR_FRAME_TYPE;

    skip_bits(gbc, 5); // skip bitstream id

    /* volume control params */
    for (int i = 0; i < (hdr->channel_mode ? 1 : 2); i++) {
        hdr->dialog_normalization[i] = -get_bits(gbc, 5);
        hdr->compression_exists[i] = get_bits1(gbc);
        if (hdr->compression_exists[i])
            hdr->heavy_dynamic_range[i] = get_bits(gbc, 8);
    }

    /* dependent stream channel map */
    if (hdr->frame_type == EAC3_FRAME_TYPE_DEPENDENT) {
        hdr->channel_map_present = get_bits1(gbc);
        if (hdr->channel_map_present) {
            int64_t channel_layout = 0;
            int channel_map = get_bits(gbc, 16);

            for (int i = 0; i < 16; i++)
                if (channel_map & (1 << (EAC3_MAX_CHANNELS - i - 1)))
                    channel_layout |= ff_eac3_custom_channel_map_locations[i][1];

            if (av_popcount64(channel_layout) > EAC3_MAX_CHANNELS) {
                return AC3_PARSE_ERROR_CHANNEL_MAP;
            }
            hdr->channel_map = channel_map;
        }
    }

    /* mixing metadata */
    if (get_bits1(gbc)) {
        /* center and surround mix levels */
        if (hdr->channel_mode > AC3_CHMODE_STEREO) {
            hdr->preferred_downmix = get_bits(gbc, 2);
            if (hdr->channel_mode & 1) {
                /* if three front channels exist */
                hdr->center_mix_level_ltrt = get_bits(gbc, 3);
                hdr->center_mix_level      = get_bits(gbc, 3);
            }
            if (hdr->channel_mode & 4) {
                /* if a surround channel exists */
                hdr->surround_mix_level_ltrt = av_clip(get_bits(gbc, 3), 3, 7);
                hdr->surround_mix_level      = av_clip(get_bits(gbc, 3), 3, 7);
            }
        }

        /* lfe mix level */
        if (hdr->lfe_on && (hdr->lfe_mix_level_exists = get_bits1(gbc))) {
            hdr->lfe_mix_level = get_bits(gbc, 5);
        }

        /* info for mixing with other streams and substreams */
        if (hdr->frame_type == EAC3_FRAME_TYPE_INDEPENDENT) {
            for (int i = 0; i < (hdr->channel_mode ? 1 : 2); i++) {
                // TODO: apply program scale factor
                if (get_bits1(gbc)) {
                    skip_bits(gbc, 6);  // skip program scale factor
                }
            }
            if (get_bits1(gbc)) {
                skip_bits(gbc, 6);  // skip external program scale factor
            }
            /* skip mixing parameter data */
            switch(get_bits(gbc, 2)) {
                case 1: skip_bits(gbc, 5);  break;
                case 2: skip_bits(gbc, 12); break;
                case 3: {
                    int mix_data_size = (get_bits(gbc, 5) + 2) << 3;
                    skip_bits_long(gbc, mix_data_size);
                    break;
                }
            }
            /* skip pan information for mono or dual mono source */
            if (hdr->channel_mode < AC3_CHMODE_STEREO) {
                for (int i = 0; i < (hdr->channel_mode ? 1 : 2); i++) {
                    if (get_bits1(gbc)) {
                        /* note: this is not in the ATSC A/52B specification
                           reference: ETSI TS 102 366 V1.1.1
                                      section: E.1.3.1.25 */
                        skip_bits(gbc, 8);  // skip pan mean direction index
                        skip_bits(gbc, 6);  // skip reserved paninfo bits
                    }
                }
            }
            /* skip mixing configuration information */
            if (get_bits1(gbc)) {
                for (int i = 0; i < hdr->num_blocks; i++) {
                    if (hdr->num_blocks == 1 || get_bits1(gbc)) {
                        skip_bits(gbc, 5);
                    }
                }
            }
        }
    }

    /* informational metadata */
    if (get_bits1(gbc)) {
        hdr->bitstream_mode = get_bits(gbc, 3);
        skip_bits(gbc, 2); // skip copyright bit and original bitstream bit
        if (hdr->channel_mode == AC3_CHMODE_STEREO) {
            hdr->dolby_surround_mode  = get_bits(gbc, 2);
            hdr->dolby_headphone_mode = get_bits(gbc, 2);
        }
        if (hdr->channel_mode >= AC3_CHMODE_2F2R) {
            hdr->dolby_surround_ex_mode = get_bits(gbc, 2);
        }
        for (int i = 0; i < (hdr->channel_mode ? 1 : 2); i++) {
            if (get_bits1(gbc)) {
                skip_bits(gbc, 8); // skip mix level, room type, and A/D converter type
            }
        }
        if (hdr->sr_code != EAC3_SR_CODE_REDUCED) {
            skip_bits1(gbc); // skip source sample rate code
        }
    }

    /* converter synchronization flag
       If frames are less than six blocks, this bit should be turned on
       once every 6 blocks to indicate the start of a frame set.
       reference: RFC 4598, Section 2.1.3  Frame Sets */
    if (hdr->frame_type == EAC3_FRAME_TYPE_INDEPENDENT && hdr->num_blocks != 6) {
        skip_bits1(gbc); // skip converter synchronization flag
    }

    /* original frame size code if this stream was converted from AC-3 */
    if (hdr->frame_type == EAC3_FRAME_TYPE_AC3_CONVERT &&
            (hdr->num_blocks == 6 || get_bits1(gbc))) {
        skip_bits(gbc, 6); // skip frame size code
    }

    /* additional bitstream info */
    if (get_bits1(gbc)) {
        int addbsil = get_bits(gbc, 6);
        for (int i = 0; i < addbsil + 1; i++) {
            if (i == 0) {
                /* In this 8 bit chunk, the LSB is equal to flag_ec3_extension_type_a
                   which can be used to detect Atmos presence */
                skip_bits(gbc, 7);
                hdr->eac3_extension_type_a = get_bits1(gbc);
                if (hdr->eac3_extension_type_a) {
                    hdr->complexity_index_type_a = get_bits(gbc, 8);
                    i++;
                }
            } else {
                skip_bits(gbc, 8); // skip additional bit stream info
            }
        }
    }

    return 0;
}

int ff_ac3_parse_header(GetBitContext *gbc, AC3HeaderInfo *hdr)
{
    int frame_size_code;

    memset(hdr, 0, sizeof(*hdr));

    hdr->sync_word = get_bits(gbc, 16);
    if(hdr->sync_word != 0x0B77)
        return AC3_PARSE_ERROR_SYNC;

    /* read ahead to bsid to distinguish between AC-3 and E-AC-3 */
    hdr->bitstream_id = show_bits_long(gbc, 29) & 0x1F;
    if(hdr->bitstream_id > 16)
        return AC3_PARSE_ERROR_BSID;

    hdr->num_blocks = 6;
    hdr->ac3_bit_rate_code = -1;

    /* set default mix levels */
    hdr->center_mix_level   = 5;  // -4.5dB
    hdr->surround_mix_level = 6;  // -6.0dB

    /* set default dolby surround mode */
    hdr->dolby_surround_mode = AC3_DSURMOD_NOTINDICATED;

    if(hdr->bitstream_id <= 10) {
        /* Normal AC-3 */
        hdr->crc1 = get_bits(gbc, 16);
        hdr->sr_code = get_bits(gbc, 2);
        if(hdr->sr_code == 3)
            return AC3_PARSE_ERROR_SAMPLE_RATE;

        frame_size_code = get_bits(gbc, 6);
        if(frame_size_code > 37)
            return AC3_PARSE_ERROR_FRAME_SIZE;

        hdr->ac3_bit_rate_code = (frame_size_code >> 1);

        skip_bits(gbc, 5); // skip bsid, already got it

        hdr->bitstream_mode = get_bits(gbc, 3);
        hdr->channel_mode = get_bits(gbc, 3);

        if(hdr->channel_mode == AC3_CHMODE_STEREO) {
            hdr->dolby_surround_mode = get_bits(gbc, 2);
        } else {
            if((hdr->channel_mode & 1) && hdr->channel_mode != AC3_CHMODE_MONO)
                hdr->  center_mix_level =   center_levels[get_bits(gbc, 2)];
            if(hdr->channel_mode & 4)
                hdr->surround_mix_level = surround_levels[get_bits(gbc, 2)];
        }
        hdr->lfe_on = get_bits1(gbc);

        hdr->sr_shift = FFMAX(hdr->bitstream_id, 8) - 8;
        hdr->sample_rate = ff_ac3_sample_rate_tab[hdr->sr_code] >> hdr->sr_shift;
        hdr->bit_rate = (ff_ac3_bitrate_tab[hdr->ac3_bit_rate_code] * 1000) >> hdr->sr_shift;
        hdr->channels = ff_ac3_channels_tab[hdr->channel_mode] + hdr->lfe_on;
        hdr->frame_size = ff_ac3_frame_size_tab[frame_size_code][hdr->sr_code] * 2;
        hdr->frame_type = EAC3_FRAME_TYPE_AC3_CONVERT; //EAC3_FRAME_TYPE_INDEPENDENT;
        hdr->substreamid = 0;

        int ret = ac3_parse_header(gbc, hdr);
        if (ret < 0)
            return ret;
    } else {
        /* Enhanced AC-3 */
        hdr->crc1 = 0;
        hdr->frame_type = get_bits(gbc, 2);
        if(hdr->frame_type == EAC3_FRAME_TYPE_RESERVED)
            return AC3_PARSE_ERROR_FRAME_TYPE;

        hdr->substreamid = get_bits(gbc, 3);

        hdr->frame_size = (get_bits(gbc, 11) + 1) << 1;
        if(hdr->frame_size < AC3_HEADER_SIZE)
            return AC3_PARSE_ERROR_FRAME_SIZE;

        hdr->sr_code = get_bits(gbc, 2);
        if (hdr->sr_code == 3) {
            int sr_code2 = get_bits(gbc, 2);
            if(sr_code2 == 3)
                return AC3_PARSE_ERROR_SAMPLE_RATE;
            hdr->sample_rate = ff_ac3_sample_rate_tab[sr_code2] / 2;
            hdr->sr_shift = 1;
        } else {
            hdr->num_blocks = eac3_blocks[get_bits(gbc, 2)];
            hdr->sample_rate = ff_ac3_sample_rate_tab[hdr->sr_code];
            hdr->sr_shift = 0;
        }

        hdr->channel_mode = get_bits(gbc, 3);
        hdr->lfe_on = get_bits1(gbc);

        hdr->bit_rate = 8LL * hdr->frame_size * hdr->sample_rate /
                        (hdr->num_blocks * 256);
        hdr->channels = ff_ac3_channels_tab[hdr->channel_mode] + hdr->lfe_on;

        int ret = eac3_parse_header(gbc, hdr);
        if (ret < 0)
            return ret;
    }
    hdr->channel_layout = ff_ac3_channel_layout_tab[hdr->channel_mode];
    if (hdr->lfe_on)
        hdr->channel_layout |= AV_CH_LOW_FREQUENCY;

    return 0;
}

// TODO: Better way to pass AC3HeaderInfo fields to mov muxer.
int avpriv_ac3_parse_header(AC3HeaderInfo **phdr, const uint8_t *buf,
                            size_t size)
{
    GetBitContext gb;
    AC3HeaderInfo *hdr;
    int err;

    if (!*phdr)
        *phdr = av_mallocz(sizeof(AC3HeaderInfo));
    if (!*phdr)
        return AVERROR(ENOMEM);
    hdr = *phdr;

    err = init_get_bits8(&gb, buf, size);
    if (err < 0)
        return AVERROR_INVALIDDATA;
    err = ff_ac3_parse_header(&gb, hdr);
    if (err < 0)
        return AVERROR_INVALIDDATA;

    return get_bits_count(&gb);
}

int av_ac3_parse_header(const uint8_t *buf, size_t size,
                        uint8_t *bitstream_id, uint16_t *frame_size)
{
    GetBitContext gb;
    AC3HeaderInfo hdr;
    uint8_t tmp[32 + AV_INPUT_BUFFER_PADDING_SIZE];
    int err;

    size = FFMIN(32, size);
    memcpy(tmp, buf, size);
    memset(tmp + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    err = init_get_bits8(&gb, tmp, size);
    if (err < 0)
        return AVERROR_INVALIDDATA;
    err = ff_ac3_parse_header(&gb, &hdr);
    if (err < 0)
        return AVERROR_INVALIDDATA;

    *bitstream_id = hdr.bitstream_id;
    *frame_size   = hdr.frame_size;

    return 0;
}

static int ac3_sync(uint64_t state, int *need_next_header, int *new_frame_start)
{
    int err;
    union {
        uint64_t u64;
        uint8_t  u8[8 + AV_INPUT_BUFFER_PADDING_SIZE];
    } tmp = { av_be2ne64(state) };
    AC3HeaderInfo hdr;
    GetBitContext gbc;

    if (tmp.u8[1] == 0x77 && tmp.u8[2] == 0x0b) {
        FFSWAP(uint8_t, tmp.u8[1], tmp.u8[2]);
        FFSWAP(uint8_t, tmp.u8[3], tmp.u8[4]);
        FFSWAP(uint8_t, tmp.u8[5], tmp.u8[6]);
    }

    init_get_bits(&gbc, tmp.u8+8-AC3_HEADER_SIZE, 54);
    err = ff_ac3_parse_header(&gbc, &hdr);

    if(err < 0)
        return 0;

    *new_frame_start  = (hdr.frame_type != EAC3_FRAME_TYPE_DEPENDENT);
    *need_next_header = *new_frame_start || (hdr.frame_type != EAC3_FRAME_TYPE_AC3_CONVERT);
    return hdr.frame_size;
}

static av_cold int ac3_parse_init(AVCodecParserContext *s1)
{
    AACAC3ParseContext *s = s1->priv_data;
    s->header_size = AC3_HEADER_SIZE;
    s->crc_ctx = av_crc_get_table(AV_CRC_16_ANSI);
    s->sync = ac3_sync;
    return 0;
}


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

/* The frame size and bsid: 1 a valid header, 0 not. */
static int disc_header(const uint8_t *b, int len, int *size, int *bsid)
{
    AC3HeaderInfo info, *pi = &info;

    if (len < 7)
        return 0;
    *bsid = b[5] >> 3;
    if (disc_is_eac3(*bsid)) {
        *size = ((AV_RB16(b + 2) & 0x7ff) + 1) * 2;
        return 1;
    }
    if (b[5] >= 0x48 || avpriv_ac3_parse_header(&pi, b, len) < 0)
        return 0;
    *size = info.frame_size;
    return 1;
}

/* A/52 crc1 and crc2: bytes 2 .. 5/8 of the frame, then the remaining 3/8,
 * each give a remainder of 0. */
static int disc_crc_ok(const uint8_t *b, int size)
{
    const AVCRC *crc = av_crc_get_table(AV_CRC_16_ANSI);
    int n58 = size * 5 >> 3;

    return !av_crc(crc, 0, b + 2, n58 - 2) && !av_crc(crc, 0, b + n58, size * 3 >> 3);
}

/* The stream header from a frame: E-AC-3 bitstream information up to the
 * channel map (the frame must hold it; a dependent stream's channel map must
 * give the channels its audio coding mode and LFE give); AC-3: the syncinfo. */
static int disc_stream_header(const uint8_t *b, int len)
{
    static const uint8_t acmod_channels[8] = { 2, 1, 2, 3, 3, 4, 4, 5 };
    /* bit 0 (first) .. 15: L C R Ls Rs Lc/Rc Lrs/Rrs Cs Ts Lsd/Rsd Lw/Rw Lvh/Rvh Cvh Lts/Rts LFE2 LFE */
    static const uint8_t weight[16] = { 1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1, 2, 1, 1 };
    GetBitContext gb;
    unsigned strmtyp, acmod, lfeon, chanmap = 0, chanmape = 0;
    int size, bsid, expect = 0;

    if (!disc_header(b, len, &size, &bsid))
        return 0;
    if (!disc_is_eac3(bsid))
        return 1;
    if (init_get_bits8(&gb, b + 2, len - 2) < 0)
        return 0;
    strmtyp = get_bits(&gb, 2);
    skip_bits_long(&gb, 3 + 11 + 2 + 2);        /* substreamid, frmsiz, fscod, fscod2 / numblkscod */
    acmod = get_bits(&gb, 3);
    lfeon = get_bits1(&gb);
    skip_bits_long(&gb, 5 + 5);                 /* bsid, dialnorm */
    if (get_bits1(&gb))                         /* compre */
        skip_bits_long(&gb, 8);
    if (!acmod) {
        skip_bits_long(&gb, 5);
        if (get_bits1(&gb))
            skip_bits_long(&gb, 8);
    }
    if (strmtyp == 1 && (chanmape = get_bits1(&gb)))
        chanmap = get_bits(&gb, 16);
    if (get_bits_left(&gb) < 0)
        return 0;
    if (!chanmape)
        return 1;
    for (int i = 0; i < 16; i++)
        if (chanmap & (0x8000 >> i))
            expect += weight[i];
    return expect == acmod_channels[acmod] + lfeon;
}

static int disc_kind_ok(const AACAC3ParseContext *c, int bsid)
{
    return !c->disc_kind || disc_is_eac3(bsid) + 1 == c->disc_kind;
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
        if (!disc_stream_header(b + pos, size))
            return pos + 1;
        if (disc_is_eac3(bsid))
            return pos;
        return pos + !disc_crc_ok(b + pos, size);
    }
    return len >= DISC_GIVE_UP + 1 ? -1 : -2;
}

static const FFDiscFrameRule disc_rule = { disc_sync, disc_size, disc_check, disc_resync, DISC_LIMIT };

static int ac3_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    AACAC3ParseContext *c = s->priv_data;

    if (!(s->flags & PARSER_FLAG_DISC))
        return ff_aac_ac3_parse(s, avctx, poutbuf, poutbuf_size, buf, buf_size);
    c->disc_kind = s->flags & PARSER_FLAG_DISC_ANY ? 0 : avctx->codec_id == AV_CODEC_ID_EAC3 ? 2 : 1;
    return ff_parser_disc_frames(s, c, &disc_rule, &c->disc, poutbuf, poutbuf_size, buf, buf_size);
}

static av_cold void ac3_parse_close(AVCodecParserContext *s)
{
    AACAC3ParseContext *c = s->priv_data;

    ff_parser_disc_cut_close(&c->disc.cut);
    ff_parse_close(s);
}

const FFCodecParser ff_ac3_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_AC3, AV_CODEC_ID_EAC3),
    .priv_data_size = sizeof(AACAC3ParseContext),
    .init           = ac3_parse_init,
    .parse          = ac3_parse,
    .close          = ac3_parse_close,
};

#else

int avpriv_ac3_parse_header(AC3HeaderInfo **phdr, const uint8_t *buf,
                            size_t size)
{
    return AVERROR(ENOSYS);
}

int av_ac3_parse_header(const uint8_t *buf, size_t size,
                        uint8_t *bitstream_id, uint16_t *frame_size)
{
    return AVERROR(ENOSYS);
}
#endif
