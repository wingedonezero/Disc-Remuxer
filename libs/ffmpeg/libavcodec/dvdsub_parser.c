/*
 * DVD subtitle decoding
 * Copyright (c) 2005 Fabrice Bellard
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

#include <string.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "parser_internal.h"

/* parser definition */
typedef struct DVDSubParseContext {
    uint8_t *packet;
    int packet_len;
    int packet_index;
    FFDiscCut disc;
} DVDSubParseContext;

/* Disc mode (PARSER_FLAG_DISC): the stream is cut into units by their own
 * headers, whatever the packets it came in: a unit's header is its size and
 * the offset of its first display control sequence, 16-bit each, or (size
 * field 0: an HD DVD unit) 32-bit each after it. With 10 bytes or more, a
 * unit starts only where the header states a size of at least 9 and a first
 * control sequence that leaves 5 bytes; the unit is as long as its size.
 * Where no unit starts, the bytes kept so far are no unit. */
static int disc_header(const uint8_t *b, int avail, uint32_t *size, uint32_t *dcsq)
{
    if (avail < 4)
        return 0;
    if (AV_RB16(b)) {
        *size = AV_RB16(b);
        *dcsq = AV_RB16(b + 2);
        return 1;
    }
    if (avail < 10)
        return 0;
    *size = AV_RB32(b + 2);
    *dcsq = AV_RB32(b + 6);
    return 1;
}

static int disc_unit_size(void *logctx, const uint8_t *buf, int avail, int final, void *state)
{
    uint32_t size, dcsq;

    if (avail >= 10) {
        disc_header(buf, avail, &size, &dcsq);
        if (size < 9 || dcsq > size - 5)
            return DISC_CUT_NONE;
    }
    if (!disc_header(buf, avail, &size, &dcsq) || size > avail)
        return 0;
    return size;
}

static int disc_resync(const uint8_t *buf, int avail)
{
    return avail;
}

static const FFDiscCutRule disc_rule = { disc_unit_size, disc_resync, 0 };

static int dvdsub_parse(AVCodecParserContext *s,
                        AVCodecContext *avctx,
                        const uint8_t **poutbuf, int *poutbuf_size,
                        const uint8_t *buf, int buf_size)
{
    DVDSubParseContext *pc = s->priv_data;

    if (s->flags & PARSER_FLAG_DISC)
        return ff_parser_disc_cut(s, avctx, &disc_rule, &pc->disc, poutbuf, poutbuf_size, buf, buf_size);

    *poutbuf      = buf;
    *poutbuf_size = buf_size;

    if (pc->packet_index == 0) {
        if (buf_size < 2 || (AV_RB16(buf) == 0 && buf_size < 6)) {
            if (buf_size)
                av_log(avctx, AV_LOG_DEBUG, "Parser input %d too small\n", buf_size);
            return buf_size;
        }
        pc->packet_len = AV_RB16(buf);
        if (pc->packet_len == 0) /* HD-DVD subpicture packet */
            pc->packet_len = AV_RB32(buf+2);
        av_freep(&pc->packet);
        if ((unsigned)pc->packet_len > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
            av_log(avctx, AV_LOG_ERROR, "packet length %d is invalid\n", pc->packet_len);
            return buf_size;
        }
        pc->packet = av_malloc(pc->packet_len + AV_INPUT_BUFFER_PADDING_SIZE);
    }
    if (pc->packet) {
        if (buf_size <= pc->packet_len - pc->packet_index) {
            memcpy(pc->packet + pc->packet_index, buf, buf_size);
            pc->packet_index += buf_size;
            if (pc->packet_index >= pc->packet_len) {
                *poutbuf = pc->packet;
                *poutbuf_size = pc->packet_len;
                pc->packet_index = 0;
                return buf_size;
            }
        } else {
            /* erroneous size */
            pc->packet_index = 0;
        }
    }
    *poutbuf = NULL;
    *poutbuf_size = 0;
    return buf_size;
}

static av_cold void dvdsub_parse_close(AVCodecParserContext *s)
{
    DVDSubParseContext *pc = s->priv_data;
    av_freep(&pc->packet);
    ff_parser_disc_cut_close(&pc->disc);
}

const FFCodecParser ff_dvdsub_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_DVD_SUBTITLE),
    .priv_data_size = sizeof(DVDSubParseContext),
    .parse          = dvdsub_parse,
    .close          = dvdsub_parse_close,
};
