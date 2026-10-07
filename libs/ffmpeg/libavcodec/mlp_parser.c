/*
 * MLP parser
 * Copyright (c) 2007 Ian Caulfield
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

/**
 * @file
 * MLP parser
 */

#include <stdint.h>

#include "libavutil/internal.h"
#include "get_bits.h"
#include "parser.h"
#include "mlp_parse.h"
#include "mlp.h"
#include "parser_internal.h"

typedef struct MLPParseContext
{
    ParseContext pc;

    int bytes_left;

    int in_sync;

    int num_substreams;

    FFDiscCut disc;
} MLPParseContext;

/* Disc mode (PARSER_FLAG_DISC): an access unit is as long as its length
 * field says (12 bits, in 16-bit words, after the check nibble), at least 8
 * bytes; where none starts, the next one is looked for at a major sync (its
 * sync word F8 72 6F BA / BB 4 bytes in). Without disc mode an AU whose
 * parity or checksum fails drops every AU up to the next major sync. */
static int disc_major_sync(const uint8_t *p, int n)
{
    return n >= 8 && p[4] == 0xF8 && p[5] == 0x72 && p[6] == 0x6F && (p[7] & 0xFE) == 0xBA;
}

static int disc_unit_size(void *logctx, const uint8_t *p, int avail, int final, void *state)
{
    int len;

    if (avail < 8)
        return 0;
    len = (((p[0] & 0xF) << 8) | p[1]) * 2;
    if (len < 8)
        return DISC_CUT_NONE;
    return len <= avail ? len : 0;
}

static int disc_resync(const uint8_t *p, int avail)
{
    for (int i = 0; i + 8 <= avail; i++)
        if (disc_major_sync(p + i, avail - i))
            return i;
    return -1;
}

static const FFDiscCutRule disc_rule = { disc_unit_size, disc_resync, 0 };

static av_cold int mlp_init(AVCodecParserContext *s)
{
    ff_mlp_init_crc();
    return 0;
}

static int mlp_parse(AVCodecParserContext *s,
                     AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    MLPParseContext *mp = s->priv_data;
    int sync_present;
    uint8_t parity_bits;
    int next;
    int ret;
    int i, p = 0;

    if (s->flags & PARSER_FLAG_DISC)
        return ff_parser_disc_cut(s, avctx, &disc_rule, &mp->disc, poutbuf, poutbuf_size, buf, buf_size);

    s->key_frame = 0;

    *poutbuf_size = 0;
    *poutbuf = NULL;
    if (buf_size == 0)
        return 0;

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        if (!mp->in_sync) {
            // Not in sync - find a major sync header

            for (i = 0; i < buf_size; i++) {
                mp->pc.state = (mp->pc.state << 8) | buf[i];
                if ((mp->pc.state & 0xfffffffe) == 0xf8726fba &&
                    // ignore if we do not have the data for the start of header
                    mp->pc.index + i >= 7) {
                    mp->in_sync = 1;
                    mp->bytes_left = 0;
                    break;
                }
            }

            if (!mp->in_sync) {
                if (ff_combine_frame(&mp->pc, END_NOT_FOUND, &buf, &buf_size) != -1)
                    av_log(avctx, AV_LOG_WARNING, "ff_combine_frame failed\n");
                return buf_size;
            }

            if ((ret = ff_combine_frame(&mp->pc, i - 7, &buf, &buf_size)) < 0) {
                av_log(avctx, AV_LOG_WARNING, "ff_combine_frame failed\n");
                return ret;
            }

            return i - 7;
        }

        if (mp->bytes_left == 0) {
            // Find length of this packet

            /* Copy overread bytes from last frame into buffer. */
            for(; mp->pc.overread>0; mp->pc.overread--) {
                mp->pc.buffer[mp->pc.index++]= mp->pc.buffer[mp->pc.overread_index++];
            }

            if (mp->pc.index + buf_size < 2) {
                if (ff_combine_frame(&mp->pc, END_NOT_FOUND, &buf, &buf_size) != -1)
                    av_log(avctx, AV_LOG_WARNING, "ff_combine_frame failed\n");
                return buf_size;
            }

            mp->bytes_left = ((mp->pc.index > 0 ? mp->pc.buffer[0] : buf[0]) << 8)
                           |  (mp->pc.index > 1 ? mp->pc.buffer[1] : buf[1-mp->pc.index]);
            mp->bytes_left = (mp->bytes_left & 0xfff) * 2;
            if (mp->bytes_left <= 0) { // prevent infinite loop
                goto lost_sync;
            }
            mp->bytes_left -= mp->pc.index;
        }

        next = (mp->bytes_left > buf_size) ? END_NOT_FOUND : mp->bytes_left;

        if (ff_combine_frame(&mp->pc, next, &buf, &buf_size) < 0) {
            mp->bytes_left -= buf_size;
            return buf_size;
        }

        mp->bytes_left = 0;
    }

    sync_present = buf_size >= 8 && (AV_RB32(buf + 4) & 0xfffffffe) == 0xf8726fba;

    if (!sync_present) {
        /* The first nibble of a frame is a parity check of the 4-byte
         * access unit header and all the 2- or 4-byte substream headers. */
        // Only check when this isn't a sync frame - syncs have a checksum.

        s->key_frame = 0;

        parity_bits = 0;
        for (i = -1; i < mp->num_substreams; i++) {
            parity_bits ^= buf[p++];
            parity_bits ^= buf[p++];

            if (i < 0 || buf[p-2] & 0x80) {
                parity_bits ^= buf[p++];
                parity_bits ^= buf[p++];
            }
        }

        if ((((parity_bits >> 4) ^ parity_bits) & 0xF) != 0xF) {
            av_log(avctx, AV_LOG_INFO, "mlpparse: Parity check failed.\n");
            goto lost_sync;
        }
    } else {
        GetBitContext gb;
        MLPHeaderInfo mh;

        init_get_bits(&gb, buf + 4, (buf_size - 4) << 3);
        if (ff_mlp_read_major_sync(avctx, &mh, &gb) < 0)
            goto lost_sync;

        s->key_frame = 1;

        avctx->bits_per_raw_sample = mh.group1_bits;
        if (avctx->bits_per_raw_sample > 16)
            avctx->sample_fmt = AV_SAMPLE_FMT_S32;
        else
            avctx->sample_fmt = AV_SAMPLE_FMT_S16;
        avctx->sample_rate = mh.group1_samplerate;
        avctx->frame_size =
        s->duration = mh.access_unit_size;

        av_channel_layout_uninit(&avctx->ch_layout);
        if (mh.stream_type == 0xbb) {
            /* MLP stream */
            av_channel_layout_from_mask(&avctx->ch_layout, mh.channel_layout_mlp);
        } else { /* mh.stream_type == 0xba */
            /* TrueHD stream */
            if (!mh.channels_thd_stream2) {
                av_channel_layout_from_mask(&avctx->ch_layout, mh.channel_layout_thd_stream1);
            } else {
                av_channel_layout_from_mask(&avctx->ch_layout, mh.channel_layout_thd_stream2);
            }
        }

        if (!mh.is_vbr) /* Stream is CBR */
            avctx->bit_rate = mh.peak_bitrate;

        mp->num_substreams = mh.num_substreams;
    }

    *poutbuf = buf;
    *poutbuf_size = buf_size;

    return next;

lost_sync:
    mp->in_sync = 0;
    return 1;
}

static av_cold void mlp_parse_close(AVCodecParserContext *s)
{
    MLPParseContext *mp = s->priv_data;

    ff_parser_disc_cut_close(&mp->disc);
    ff_parse_close(s);
}

const FFCodecParser ff_mlp_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_MLP, AV_CODEC_ID_TRUEHD),
    .priv_data_size = sizeof(MLPParseContext),
    .init           = mlp_init,
    .parse          = mlp_parse,
    .close          = mlp_parse_close,
};
