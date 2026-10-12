/*
 * copyright (c) 2008 Paul Kendall <paul@kcbbs.gen.nz>
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
 * AAC LATM parser
 */

#include <stdint.h>
#include "parser.h"
#include "parser_internal.h"

#define LATM_HEADER     0x56e000        // 0x2b7 (11 bits)
#define LATM_MASK       0xFFE000        // top 11 bits
#define LATM_SIZE_MASK  0x001FFF        // bottom 13 bits

typedef struct LATMParseContext{
    ParseContext pc;
    int count;
    FFDiscFrames disc;          ///< disc mode (PARSER_FLAG_DISC)
} LATMParseContext;

/* ---- Disc mode (PARSER_FLAG_DISC): the disc reader's LOAS frame rules ----
 * A frame starts with the LOAS sync (11 bits, 0x2B7) and is as long as its
 * audioMuxLengthBytes + 3. Where no frame starts, the next one is a sync
 * whose next sync (its length on) is there; with more than 0x20000 bytes
 * and none, the kept bytes are given up. (The reference program also reads
 * each frame's AudioMuxElement; not done here: the title timeline's codec
 * rules take the stream values from FFmpeg's LATM decoder.) */

#define DISC_GIVE_UP 0x20000

static int disc_loas(const uint8_t *b)
{
    return b[0] == 0x56 && (b[1] & 0xe0) == 0xe0;
}

static int disc_sync(void *ctx, const uint8_t *b, int len)
{
    if (len < 3)
        return 2;
    return disc_loas(b);
}

static int disc_size(void *ctx, const uint8_t *b, int len)
{
    if (len < 3)
        return -1;
    if (!disc_loas(b))
        return -2;
    return ((b[1] & 0x1f) << 8 | b[2]) + 3;
}

static int disc_check(void *ctx, const uint8_t *b, int len)
{
    return 1;
}

static int disc_resync(void *ctx, const uint8_t *b, int len)
{
    if (len < 3)
        return -2;
    for (int pos = 0; pos < len - 3; pos++) {
        int n;
        if (!disc_loas(b + pos))
            continue;
        n = (b[pos + 1] & 0x1f) << 8 | b[pos + 2];
        if (len < pos + n + 6)
            break;                          /* the next sync is not there yet */
        if (disc_loas(b + pos + n + 3))
            return pos;
    }
    return len > DISC_GIVE_UP ? -1 : -2;
}

static const FFDiscFrameRule disc_rule = { disc_sync, disc_size, disc_check, disc_resync, DISC_GIVE_UP };

/**
 * Find the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or -1
 */
static int latm_find_frame_end(AVCodecParserContext *s1, const uint8_t *buf,
                               int buf_size)
{
    LATMParseContext *s = s1->priv_data;
    ParseContext *pc    = &s->pc;
    int pic_found, i;
    uint32_t state;

    pic_found = pc->frame_start_found;
    state     = pc->state;

    if (!pic_found) {
        for (i = 0; i < buf_size; i++) {
            state = (state<<8) | buf[i];
            if ((state & LATM_MASK) == LATM_HEADER) {
                i++;
                s->count  = -i;
                pic_found = 1;
                break;
            }
        }
    }

    if (pic_found) {
        /* EOF considered as end of frame */
        if (buf_size == 0)
            return 0;
        if ((state & LATM_SIZE_MASK) - s->count <= buf_size) {
            pc->frame_start_found = 0;
            pc->state             = -1;
            return (state & LATM_SIZE_MASK) - s->count;
        }
    }

    s->count             += buf_size;
    pc->frame_start_found = pic_found;
    pc->state             = state;

    return END_NOT_FOUND;
}

static int latm_parse(AVCodecParserContext *s1, AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size,
                      const uint8_t *buf, int buf_size)
{
    LATMParseContext *s = s1->priv_data;
    ParseContext *pc    = &s->pc;
    int next;

    if (s1->flags & PARSER_FLAG_DISC)
        return ff_parser_disc_frames(s1, s, &disc_rule, &s->disc, poutbuf, poutbuf_size, buf, buf_size);
    if (s1->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        next = latm_find_frame_end(s1, buf, buf_size);

        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }
    }
    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return next;
}

static av_cold void latm_close(AVCodecParserContext *s1)
{
    LATMParseContext *s = s1->priv_data;

    ff_parser_disc_cut_close(&s->disc.cut);
    ff_parse_close(s1);
}

const FFCodecParser ff_aac_latm_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_AAC_LATM),
    .priv_data_size = sizeof(LATMParseContext),
    .parse          = latm_parse,
    .close          = latm_close
};
