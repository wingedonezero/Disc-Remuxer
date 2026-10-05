/*
 * The shared rip core for disc formats (DVD-Video, HD DVD, Blu-ray): units,
 * timing, joining and checks of a title's tracks. Design: docs/RIP_CORE.md
 * of the Disc-Remuxer repository.
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

#ifndef AVFORMAT_DISCRIP_H
#define AVFORMAT_DISCRIP_H

#include <stdint.h>

#include "libavcodec/codec_id.h"

/* Time: one unit = 1/1,080,000,000 s (27 MHz x 40). Exact for 90 kHz PES
 * times, for 1001-based frame and field durations and for 48 / 96 / 192 kHz
 * samples. A missing time is AV_NOPTS_VALUE. */
#define DR_TICKS_PER_SECOND 1080000000LL
#define DR_TICKS_PER_PTS    12000          /* 90 kHz -> ticks */

/* One entry of the codec table: what the core needs to know about a codec.
 * A codec without an entry is refused (the track is not ripped). Entries
 * grow with the stages; a missing function means the rule is not known yet
 * and is never guessed. */
typedef struct DRCodec {
    enum AVCodecID id;
    const char    *name;
    /** Offset inside a unit where the part that takes a PES timestamp starts
     *  (MPEG-2 video: the picture start code; VC-1: the frame start code;
     *  audio and sub-pictures: 0), or -1 when the unit has none. */
    int (*anchor)(const uint8_t *data, int size);
    /** 1 when the parser's output is a unit of the codec, 0 when it is not
     *  (bytes before the first sync word, bytes between units): such bytes
     *  are left out and take no time. NULL: every output is a unit. */
    int (*check)(const uint8_t *data, int size);
} DRCodec;

/** The codec table entry of a codec, or NULL (the codec is not supported). */
const DRCodec *ff_discrip_codec(enum AVCodecID id);

/* ---- Stage 1: units and timestamp records (discrip_units.c) ---- */

typedef struct DRUnit {
    const uint8_t *data;   /**< the unit's bytes, valid during the callback */
    int            size;
    int64_t        time;   /**< ticks, or AV_NOPTS_VALUE */
    int64_t        pos;    /**< byte offset of the unit's first byte in the track's stream */
} DRUnit;

/** Called for every unit, in stream order. A negative return stops the
 *  cutter and is returned by the call that produced the unit. */
typedef int (*DRUnitCb)(void *opaque, const DRUnit *unit);

typedef struct DRCutterStats {
    int64_t bytes;         /**< payload bytes written */
    int64_t records;       /**< payloads written with a time */
    int64_t units;         /**< units given to the callback */
    int64_t timed;         /**< of them with a time */
    int64_t records_unused;/**< records dropped without giving their time to a unit */
    int64_t skipped;       /**< parser outputs that are not units (left out) */
    int64_t skipped_bytes;
} DRCutterStats;

typedef struct DRCutter DRCutter;

/**
 * A unit cutter for one track of one segment: payloads in, units out.
 * Units are cut by FFmpeg's parser for the codec; a unit takes the time of a
 * payload when the unit's anchor (see DRCodec) is the first one that lies in
 * that payload's bytes (ISO/IEC 13818-1 2.4.3.7: a PES packet's PTS belongs
 * to the first access unit that starts in it).
 *
 * @param logctx for av_log
 * @return 0, AVERROR(ENOSYS) when the codec has no table entry or no parser
 */
int ff_discrip_cutter_open(DRCutter **cutter, void *logctx, enum AVCodecID codec,
                           DRUnitCb cb, void *opaque);

/** One payload; time in ticks or AV_NOPTS_VALUE. */
int ff_discrip_cutter_write(DRCutter *c, const uint8_t *data, int size, int64_t time);

/** The end of the segment: the units still buffered in the parser. */
int ff_discrip_cutter_flush(DRCutter *c);

void ff_discrip_cutter_stats(const DRCutter *c, DRCutterStats *stats);

void ff_discrip_cutter_close(DRCutter **cutter);

#endif /* AVFORMAT_DISCRIP_H */
