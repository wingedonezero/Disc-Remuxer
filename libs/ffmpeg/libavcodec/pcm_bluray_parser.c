/*
 * Blu-ray linear PCM parser
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
 * Blu-ray LPCM: every unit is a 4-byte header (audio data payload size
 * (16 bits), channel assignment (4), sampling frequency (4), bits per
 * sample (2), reserved (6)) and the payload it states. Outside disc mode
 * packets pass as they come; in disc mode (PARSER_FLAG_DISC) the stream is
 * cut into units. A header that states no known format, or no payload,
 * starts no unit: the bytes kept up to it are given up.
 */

#include "libavutil/intreadwrite.h"

#include "avcodec.h"
#include "parser_internal.h"

typedef struct PCMBlurayParseContext {
    FFDiscCut disc;
} PCMBlurayParseContext;

/* Whether hdr states a format: channel assignment 1 (mono) or 3 to 11,
 * sampling frequency 1, 4 or 5 (48, 96, 192 kHz), bits per sample 1 to 3
 * (16, 20, 24). */
static int known_format(const uint8_t *hdr)
{
    int ca = hdr[2] >> 4, fs = hdr[2] & 0x0F, bits = hdr[3] >> 6;

    return (ca == 1 || (ca >= 3 && ca <= 11)) && (fs == 1 || fs == 4 || fs == 5) && bits;
}

static int disc_unit_size(void *logctx, const uint8_t *buf, int avail, int final, void *state)
{
    int size;

    if (avail < 4)
        return 0;
    size = AV_RB16(buf);
    if (!size || !known_format(buf))
        return DISC_CUT_NONE;
    return avail >= 4 + size ? 4 + size : 0;
}

/* No search for the next header: the bytes kept are given up. */
static int disc_resync(const uint8_t *buf, int avail)
{
    return avail;
}

static const FFDiscCutRule disc_rule = { disc_unit_size, disc_resync, 0 };

static int pcm_bluray_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                            const uint8_t **poutbuf, int *poutbuf_size,
                            const uint8_t *buf, int buf_size)
{
    PCMBlurayParseContext *pc = s->priv_data;

    if (s->flags & PARSER_FLAG_DISC)
        return ff_parser_disc_cut(s, avctx, &disc_rule, &pc->disc, poutbuf, poutbuf_size, buf, buf_size);
    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return buf_size;
}

static av_cold void pcm_bluray_parse_close(AVCodecParserContext *s)
{
    PCMBlurayParseContext *pc = s->priv_data;

    ff_parser_disc_cut_close(&pc->disc);
}

const FFCodecParser ff_pcm_bluray_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_PCM_BLURAY),
    .priv_data_size = sizeof(PCMBlurayParseContext),
    .parse          = pcm_bluray_parse,
    .close          = pcm_bluray_parse_close,
};
