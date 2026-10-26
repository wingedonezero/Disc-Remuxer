/*
 * DTS-UHD audio frame parser
 * Copyright (c) 2023 Xperi Corporation / DTS, Inc.
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
 * DTS-UHD (DTS:X Profile 2/3) frame parser.
 *
 * There is no decoder: the parser splits raw streams into frames and exports
 * what the frame headers say (sample rate, duration, channel layout,
 * profile). Sync frames are the only points decoding can start from, so they
 * are flagged as key frames.
 */

#include <string.h>

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "dtsuhd_common.h"
#include "parser_internal.h"

/* Room for a few maximum-size frames: a frame is output as soon as it is
 * complete, so only the data around one frame boundary is ever held. */
#define DTSUHD_BUFFER_SIZE (DTSUHD_MAX_FRAME_SIZE * 4)

typedef struct DTSUHDParseContext {
    DTSUHD *dtsuhd;
    uint8_t *buf;       ///< contiguous raw input, starting at a syncword
    int buf_bytes;      ///< valid bytes in buf
    int output_bytes;   ///< bytes of buf handed out as the previous frame
} DTSUHDParseContext;

static av_cold int dtsuhd_parse_init(AVCodecParserContext *s)
{
    DTSUHDParseContext *pc = s->priv_data;

    pc->dtsuhd = ff_dtsuhd_create();
    pc->buf = av_malloc(DTSUHD_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!pc->dtsuhd || !pc->buf)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void dtsuhd_parse_close(AVCodecParserContext *s)
{
    DTSUHDParseContext *pc = s->priv_data;

    ff_dtsuhd_destroy(pc->dtsuhd);
    pc->dtsuhd = NULL;
    av_freep(&pc->buf);
}

static void export_frame_info(AVCodecParserContext *s, AVCodecContext *avctx,
                              const DTSUHDFrameInfo *fi,
                              const DTSUHDDescriptorInfo *di)
{
    s->key_frame = fi->sync;
    if (fi->sample_rate)
        avctx->sample_rate = fi->sample_rate;
    if (fi->sample_count)
        s->duration = fi->sample_count;

    if (!di->valid)
        return;

    avctx->profile = di->decoder_profile_code + AV_PROFILE_DTSUHD_P2 +
                     (di->imax ? AV_PROFILE_DTSUHD_P2_IMAX - AV_PROFILE_DTSUHD_P2 : 0);
    av_channel_layout_uninit(&avctx->ch_layout);
    if (di->ffmpeg_channel_mask)
        av_channel_layout_from_mask(&avctx->ch_layout, di->ffmpeg_channel_mask);
    else if (di->channel_count > 0) {
        avctx->ch_layout.order       = AV_CHANNEL_ORDER_UNSPEC;
        avctx->ch_layout.nb_channels = di->channel_count;
    }
}

static void drop_bytes(DTSUHDParseContext *pc, int n)
{
    n = FFMIN(n, pc->buf_bytes);
    memmove(pc->buf, pc->buf + n, pc->buf_bytes - n);
    pc->buf_bytes -= n;
}

static int dtsuhd_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                        const uint8_t **poutbuf, int *poutbuf_size,
                        const uint8_t *buf, int buf_size)
{
    DTSUHDParseContext *pc = s->priv_data;
    DTSUHDDescriptorInfo di = { 0 };
    DTSUHDFrameInfo fi;
    int copied;

    s->key_frame = 0;

    /* Containers (MP4, Matroska) deliver whole frames: report, pass through.
     * Frames before the first sync frame cannot be parsed and are passed
     * through unflagged. */
    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        if (buf_size > 0 &&
            ff_dtsuhd_frame(pc->dtsuhd, buf, buf_size, &fi, &di) == DTSUHD_OK)
            export_frame_info(s, avctx, &fi, &di);
        *poutbuf      = buf;
        *poutbuf_size = buf_size;
        return buf_size;
    }

    /* Raw stream: the frame size comes from each frame's own table of
     * contents, so frames are split by size, never by scanning for
     * syncwords inside the payload. */
    drop_bytes(pc, pc->output_bytes);
    pc->output_bytes = 0;

    copied = FFMIN(buf_size, DTSUHD_BUFFER_SIZE - pc->buf_bytes);
    if (copied > 0) {
        memcpy(pc->buf + pc->buf_bytes, buf, copied);
        pc->buf_bytes += copied;
    }

    for (;;) {
        int status, skip = 0;

        while (skip + 4 <= pc->buf_bytes && !dtsuhd_is_syncword(AV_RB32(pc->buf + skip)))
            skip++;
        if (skip + 4 > pc->buf_bytes)
            skip = FFMAX(pc->buf_bytes - 3, 0); // keep a possible partial syncword
        drop_bytes(pc, skip);
        if (pc->buf_bytes < 4)
            break;

        status = ff_dtsuhd_frame(pc->dtsuhd, pc->buf, pc->buf_bytes, &fi, &di);
        if (status == DTSUHD_OK) {
            export_frame_info(s, avctx, &fi, &di);
            pc->output_bytes = fi.frame_bytes;
            *poutbuf         = pc->buf;
            *poutbuf_size    = fi.frame_bytes;
            return copied;
        }
        if (status == DTSUHD_INCOMPLETE && pc->buf_bytes < DTSUHD_BUFFER_SIZE &&
            (copied > 0 || buf_size > 0))
            break; // wait for the rest of the frame

        /* Not a parsable frame start (non-sync frame before the first sync
         * frame, corrupt header, or a syncword that was payload data):
         * resynchronise one byte further on. */
        drop_bytes(pc, 1);
    }

    *poutbuf      = NULL;
    *poutbuf_size = 0;
    return copied;
}

const FFCodecParser ff_dtsuhd_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_DTSUHD),
    .priv_data_size = sizeof(DTSUHDParseContext),
    .init           = dtsuhd_parse_init,
    .parse          = dtsuhd_parse,
    .close          = dtsuhd_parse_close,
};
