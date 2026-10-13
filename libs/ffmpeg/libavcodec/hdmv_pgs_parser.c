/*
 * HDMV presentation graphics (Blu-ray PGS subtitles) parser
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

/**
 * @file
 * HDMV presentation graphics parser, and the text subtitle (TextST) one.
 * Outside disc mode the packets pass as they come.
 *
 * Disc mode (PARSER_FLAG_DISC): the stream is a run of segments, each a type
 * byte and a 16-bit length (type 0x81, a text subtitle dialog style, has 2
 * more bytes), and is cut into display sets: a presentation composition
 * segment (0x16; a text subtitle dialog presentation segment 0x82 likewise)
 * and the segments after it up to and with the end of display set segment
 * (0x80). What is no display set is left out (disc_status 1): a segment that
 * does not start one, a set that another start cuts short (the start stays
 * for the next set), a set the stream ends in.
 *
 * Text subtitles in disc mode: each segment is one unit (the dialog style
 * segment with the 2 bytes after it); a dialog presentation segment that
 * does not parse is refused (disc_status); bytes the stream ends in that
 * are no whole segment are no unit.
 */

#include "libavutil/intreadwrite.h"
#include "avcodec.h"
#include "parse/hdmv_textst.h"
#include "parser_internal.h"

typedef struct HDMVPGSParseContext {
    FFDiscCut    disc;
    FFDiscFrames text;              ///< text subtitles: the segments
} HDMVPGSParseContext;

static int hdmv_pgs_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                          const uint8_t **poutbuf, int *poutbuf_size,
                          const uint8_t *buf, int buf_size)
{
    HDMVPGSParseContext *pc = s->priv_data;

    if ((s->flags & PARSER_FLAG_DISC) && avctx->codec_id == AV_CODEC_ID_HDMV_TEXT_SUBTITLE)
        return ff_parser_disc_frames(s, NULL, &dr_textst_frames, &pc->text, poutbuf, poutbuf_size, buf, buf_size);
    if (s->flags & PARSER_FLAG_DISC) {
        int ret = ff_parser_disc_cut(s, avctx, &dr_pgs_cut, &pc->disc, poutbuf, poutbuf_size, buf, buf_size);
        if (*poutbuf_size && !s->disc_status && ((PGSDiscCut *)pc->disc.state)->no_unit)
            s->disc_status = PARSER_DISC_SKIP;
        return ret;
    }
    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return buf_size;
}

static av_cold void hdmv_pgs_close(AVCodecParserContext *s)
{
    HDMVPGSParseContext *pc = s->priv_data;

    ff_parser_disc_cut_close(&pc->disc);
    ff_parser_disc_cut_close(&pc->text.cut);
}

const FFCodecParser ff_hdmv_pgs_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_HDMV_PGS_SUBTITLE, AV_CODEC_ID_HDMV_TEXT_SUBTITLE),
    .priv_data_size = sizeof(HDMVPGSParseContext),
    .parse          = hdmv_pgs_parse,
    .close          = hdmv_pgs_close,
};
