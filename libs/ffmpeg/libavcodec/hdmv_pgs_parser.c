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
#include "hdmv_textst.h"
#include "parser_internal.h"

#define SEG_PCS 0x16
#define SEG_END 0x80
#define SEG_DSS 0x81
#define SEG_DPS 0x82

typedef struct PGSDiscCut {
    int no_unit;            ///< the chunk given out is no display set
} PGSDiscCut;

typedef struct HDMVPGSParseContext {
    FFDiscCut    disc;
    FFDiscFrames text;              ///< text subtitles: the segments
} HDMVPGSParseContext;

static int disc_seg_size(const uint8_t *b, int avail)
{
    if (avail < 3)
        return -1;
    return AV_RB16(b + 1) + 3 + (b[0] == SEG_DSS ? 2 : 0);
}

static int disc_is_start(int t)
{
    return t == SEG_PCS || t == SEG_DPS;
}

static int disc_unit_size(void *logctx, const uint8_t *buf, int avail, int final, void *state)
{
    PGSDiscCut *st = state;
    int off = 0, size;

    st->no_unit = 0;
    if ((size = disc_seg_size(buf, avail)) < 0)
        return final && avail ? (st->no_unit = 1, avail) : 0;
    if (!disc_is_start(buf[0])) {
        av_log(logctx, AV_LOG_DEBUG, "Title timeline: PGS: a segment of type 0x%02x that starts no display set: "
               "left out\n", buf[0]);
        st->no_unit = 1;
        return size <= avail ? size : final ? avail : 0;
    }
    for (;;) {
        if ((size = disc_seg_size(buf + off, avail - off)) < 0)
            break;
        if (off && disc_is_start(buf[off])) {
            av_log(logctx, AV_LOG_WARNING, "Title timeline: PGS: a display set without its end segment, cut short "
                   "by the next one: left out\n");
            st->no_unit = 1;
            return off;
        }
        if (size > avail - off)
            break;
        off += size;
        if (buf[off - size] == SEG_END)
            return off;
    }
    if (!final)
        return 0;
    av_log(logctx, AV_LOG_WARNING, "Title timeline: PGS: a display set the stream ends in: left out\n");
    st->no_unit = 1;
    return avail;
}

static int disc_resync(const uint8_t *buf, int avail)
{
    return avail;
}

static const FFDiscCutRule disc_rule = { disc_unit_size, disc_resync, sizeof(PGSDiscCut) };

/* ---- text subtitles: one segment per unit ---- */

static int text_sync(void *ctx, const uint8_t *buf, int avail)
{
    return 1;                       /* every segment starts where the one before ends */
}

static int text_size(void *ctx, const uint8_t *buf, int avail)
{
    int size = ff_hdmv_textst_segment_size(buf, avail);
    return size < 0 ? 0 : size;
}

static int text_check(void *ctx, const uint8_t *buf, int size)
{
    HDMVTextstDialog d;

    return buf[0] != HDMV_TEXTST_DPS || ff_hdmv_textst_dialog(buf, size, &d);
}

static int text_resync(void *ctx, const uint8_t *buf, int avail)
{
    return -1;
}

/* a segment is at most 3 + 65535 + 2 bytes */
static const FFDiscFrameRule text_rule = { text_sync, text_size, text_check, text_resync, 0x20000 };

static int hdmv_pgs_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                          const uint8_t **poutbuf, int *poutbuf_size,
                          const uint8_t *buf, int buf_size)
{
    HDMVPGSParseContext *pc = s->priv_data;

    if ((s->flags & PARSER_FLAG_DISC) && avctx->codec_id == AV_CODEC_ID_HDMV_TEXT_SUBTITLE)
        return ff_parser_disc_frames(s, NULL, &text_rule, &pc->text, poutbuf, poutbuf_size, buf, buf_size);
    if (s->flags & PARSER_FLAG_DISC) {
        int ret = ff_parser_disc_cut(s, avctx, &disc_rule, &pc->disc, poutbuf, poutbuf_size, buf, buf_size);
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
