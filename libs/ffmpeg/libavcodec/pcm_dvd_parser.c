/*
 * Linear PCM of DVD-Video and HD DVD (private stream 1 sub-streams 0xA0..):
 * a parser for disc mode only.
 *
 * The samples of a disc's LPCM stream come in audio frames: 1/600 s on
 * DVD-Video, 1/1200 s on HD DVD (the frame count and first access unit
 * pointer of each packet's header count these frames). In disc mode
 * (PARSER_FLAG_DISC) the stream is cut into those frames; the caller gives
 * their format through the codec context: frame_size (samples per channel in
 * a frame), bits_per_coded_sample (16, 20 or 24) and the number of channels;
 * while frame_size is 0 the bytes are kept. Bytes left at the end of the
 * stream that make no whole frame are no frame. Without disc mode the
 * packets pass as they are (each sample stands on its own).
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

#include "avcodec.h"
#include "parser_internal.h"

typedef struct PCMDVDParseContext {
    FFDiscCut disc;
} PCMDVDParseContext;

/* logctx is the codec context (ff_parser_disc_cut passes it on). */
static int disc_unit_size(void *logctx, const uint8_t *buf, int avail, int final, void *state)
{
    const AVCodecContext *avctx = logctx;
    int64_t frame = (int64_t)avctx->bits_per_coded_sample * avctx->ch_layout.nb_channels *
                    avctx->frame_size / 8;

    if (frame <= 0 || frame > INT_MAX)
        return 0;
    return avail >= frame ? frame : 0;
}

static int disc_resync(const uint8_t *buf, int avail)
{
    return avail;
}

static const FFDiscCutRule disc_rule = { disc_unit_size, disc_resync, 0 };

static int pcm_dvd_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                         const uint8_t **poutbuf, int *poutbuf_size,
                         const uint8_t *buf, int buf_size)
{
    PCMDVDParseContext *pc = s->priv_data;

    if (s->flags & PARSER_FLAG_DISC)
        return ff_parser_disc_cut(s, avctx, &disc_rule, &pc->disc, poutbuf, poutbuf_size, buf, buf_size);
    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return buf_size;
}

static av_cold void pcm_dvd_parse_close(AVCodecParserContext *s)
{
    PCMDVDParseContext *pc = s->priv_data;

    ff_parser_disc_cut_close(&pc->disc);
}

const FFCodecParser ff_pcm_dvd_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_PCM_DVD),
    .priv_data_size = sizeof(PCMDVDParseContext),
    .parse          = pcm_dvd_parse,
    .close          = pcm_dvd_parse_close,
};
