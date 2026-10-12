/*
 * Audio and Video frame extraction
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

#include "parser.h"
#include "aac_ac3_parser.h"
#include "adts_header.h"
#include "adts_parser.h"
#include "parser_internal.h"
#include "libavutil/intreadwrite.h"

static int aac_sync(uint64_t state, int *need_next_header, int *new_frame_start)
{
    uint8_t tmp[8 + AV_INPUT_BUFFER_PADDING_SIZE];
    AACADTSHeaderInfo hdr;
    int size;

    AV_WB64(tmp, state);

    size = ff_adts_header_parse_buf(tmp + 8 - AV_AAC_ADTS_HEADER_SIZE, &hdr);
    if (size < 0)
        return 0;
    *need_next_header = 0;
    *new_frame_start  = 1;
    return size;
}

/* ---- Disc mode (PARSER_FLAG_DISC): the disc reader's ADTS frame rules ----
 * A frame starts with the ADTS sync (12 bits) and layer 0; the reference
 * program takes only the MPEG-2 ID, here the MPEG-4 ID counts the same (its
 * frames are reviewed by the title timeline). A frame is as long as its
 * frame_length. Where no frame starts, the next one is a header whose next
 * header (frame_length on) has the same ID, layer and protection and the
 * same first four bits of byte 2 (profile, the sampling frequency index's
 * high bits); with more than 0x20000 bytes and none, the kept bytes are
 * given up. */

#define DISC_GIVE_UP 0x20000

static int disc_adts(const uint8_t *b)
{
    return b[0] == 0xff && (b[1] & 0xf6) == 0xf0;
}

static int disc_sync(void *ctx, const uint8_t *b, int len)
{
    if (len < 2)
        return 2;
    return disc_adts(b);
}

static int disc_size(void *ctx, const uint8_t *b, int len)
{
    if (len < 7)
        return -1;
    if (!disc_adts(b))
        return -2;
    return (b[3] & 3) << 11 | b[4] << 3 | b[5] >> 5;
}

static int disc_check(void *ctx, const uint8_t *b, int len)
{
    return len >= 7;
}

static int disc_resync(void *ctx, const uint8_t *b, int len)
{
    if (len < 7)
        return -2;
    for (int pos = 0; pos < len - 7; pos++) {
        int n;
        if (!disc_adts(b + pos))
            continue;
        n = (b[pos + 3] & 3) << 11 | b[pos + 4] << 3 | b[pos + 5] >> 5;
        if (len < pos + n + 7)
            break;                          /* the next header is not there yet */
        if (b[pos + n] == 0xff && b[pos + n + 1] == b[pos + 1] && (b[pos + 2] ^ b[pos + n + 2]) < 0x10)
            return pos;
    }
    return len > DISC_GIVE_UP ? -1 : -2;
}

static const FFDiscFrameRule disc_rule = { disc_sync, disc_size, disc_check, disc_resync, DISC_GIVE_UP };

static int aac_parse(AVCodecParserContext *s1, AVCodecContext *avctx, const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    AACAC3ParseContext *s = s1->priv_data;

    if (s1->flags & PARSER_FLAG_DISC)
        return ff_parser_disc_frames(s1, s, &disc_rule, &s->disc, poutbuf, poutbuf_size, buf, buf_size);
    return ff_aac_ac3_parse(s1, avctx, poutbuf, poutbuf_size, buf, buf_size);
}

static av_cold void aac_parse_close(AVCodecParserContext *s1)
{
    AACAC3ParseContext *s = s1->priv_data;

    ff_parser_disc_cut_close(&s->disc.cut);
    ff_parse_close(s1);
}

static av_cold int aac_parse_init(AVCodecParserContext *s1)
{
    AACAC3ParseContext *s = s1->priv_data;
    s->header_size = AV_AAC_ADTS_HEADER_SIZE;
    s->sync = aac_sync;
    return 0;
}


const FFCodecParser ff_aac_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_AAC),
    .priv_data_size = sizeof(AACAC3ParseContext),
    .init           = aac_parse_init,
    .parse          = aac_parse,
    .close          = aac_parse_close,
};
