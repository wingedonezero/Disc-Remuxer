/*
 * The stream pass's details probe, provided by the analysis module (the
 * program registers it with ff_disc_set_probe(); src/analyze/pass_probe.c):
 * a track's payloads through the library's parser and decoder of its codec
 * until one frame is decoded, its values written into the track's
 * DiscStreamInfo. Without a registration the details stay unknown (the
 * pass says so track by track).
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

#ifndef AVFORMAT_DISCPROBE_H
#define AVFORMAT_DISCPROBE_H

#include <stdint.h>

#include "libavcodec/codec_id.h"

struct DRCodec;
struct DiscStreamInfo;

typedef struct DiscDetailsProbe DiscDetailsProbe;

typedef struct DiscProbeOps {
    /** A probe over one track's payloads (tc: its codec table entry or
     *  NULL; layer: a Dolby Vision enhancement layer, only its metadata is
     *  read; ds: where the values go): 0, AVERROR(ENOMEM), or another
     *  error when the codec has no decoder. */
    int  (*open)(DiscDetailsProbe **p, enum AVCodecID codec, const struct DRCodec *tc,
                 int layer, struct DiscStreamInfo *ds);
    /** A payload: 0, or AVERROR(ENOMEM). */
    int  (*feed)(DiscDetailsProbe *p, const uint8_t *data, int size);
    /** The end of the data read for details (the parser's last frame, the
     *  decoder's delayed one): 0, or AVERROR(ENOMEM). */
    int  (*flush)(DiscDetailsProbe *p);
    /** 1 once the details are known (or cannot be). */
    int  (*done)(const DiscDetailsProbe *p);
    void (*close)(DiscDetailsProbe **p);
} DiscProbeOps;

void ff_disc_set_probe(const DiscProbeOps *ops);
const DiscProbeOps *ff_disc_probe(void);

#endif /* AVFORMAT_DISCPROBE_H */
