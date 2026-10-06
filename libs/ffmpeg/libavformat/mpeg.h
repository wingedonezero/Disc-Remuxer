/*
 * MPEG-1/2 muxer and demuxer common defines
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
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

#ifndef AVFORMAT_MPEG_H
#define AVFORMAT_MPEG_H

#include <stdint.h>
#include "libavutil/intreadwrite.h"

#define PACK_START_CODE             ((unsigned int)0x000001ba)
#define SYSTEM_HEADER_START_CODE    ((unsigned int)0x000001bb)
#define SEQUENCE_END_CODE           ((unsigned int)0x000001b7)
#define PACKET_START_CODE_MASK      ((unsigned int)0xffffff00)
#define PACKET_START_CODE_PREFIX    ((unsigned int)0x00000100)
#define ISO_11172_END_CODE          ((unsigned int)0x000001b9)

/* mpeg2 */
#define PROGRAM_STREAM_MAP 0x1bc
#define PRIVATE_STREAM_1   0x1bd
#define PADDING_STREAM     0x1be
#define PRIVATE_STREAM_2   0x1bf

#define AUDIO_ID 0xc0
#define VIDEO_ID 0xe0
#define H264_ID  0xe2
#define AC3_ID   0x80
#define DTS_ID   0x88
#define LPCM_ID  0xa0
#define SUB_ID   0x20

#define STREAM_TYPE_VIDEO_MPEG1     0x01
#define STREAM_TYPE_VIDEO_MPEG2     0x02
#define STREAM_TYPE_AUDIO_MPEG1     0x03
#define STREAM_TYPE_AUDIO_MPEG2     0x04
#define STREAM_TYPE_PRIVATE_SECTION 0x05
#define STREAM_TYPE_PRIVATE_DATA    0x06
#define STREAM_TYPE_AUDIO_AAC       0x0f
#define STREAM_TYPE_VIDEO_MPEG4     0x10
#define STREAM_TYPE_VIDEO_H264      0x1b
#define STREAM_TYPE_VIDEO_HEVC      0x24
#define STREAM_TYPE_VIDEO_VVC       0x33
#define STREAM_TYPE_VIDEO_CAVS      0x42

#define STREAM_TYPE_AUDIO_AC3       0x81

static const int lpcm_freq_tab[4] = { 48000, 96000, 44100, 32000 };

/* ---- Disc sectors (mpeg.c, option disc): DVD-Video VOBs, HD DVD EVOBs ----
 * One 2048-byte sector = one MPEG-2 program stream pack, read with the rules
 * of the reference's disc reader. */

#define MPEGPS_DISC_SECTOR 2048

typedef struct MpegPsDiscPacket {
    int         len;            /**< PES_packet_length + 6; 0 when the field is 0 */
    int         key;            /**< stream_id; private stream 1: sub-stream id << 8 | 0xbd; with a PES
                                     extension 2: stream_id_extension << 8 | stream_id */
    int         pts_present;    /**< PTS_DTS_flags say there is a PTS */
    int         pts_valid;      /**< and its marker bits are set (pts is 0 otherwise) */
    uint64_t    pts;            /**< 33 bits, 90 kHz */
    int         payload;        /**< payload offset in the packet (after the sub-stream header and an
                                     LPCM audio frame header) */
    int         first_au;       /**< offset of the first access unit in the payload, -1 none, -2 sub-picture */
    int         extra;          /**< LPCM: bytes of the audio frame header just before payload */
    int         at;             /**< offset of the packet in the sector */
    const char *why;            /**< when the packet does not parse: why */
} MpegPsDiscPacket;

/** The packet at *off of sector s (0 = the sector's start: the pack header
 *  first). *off becomes the next packet's offset, MPEGPS_DISC_SECTOR (or more)
 *  when the sector is done: after private stream 2, at fill bytes (0x00 or
 *  0xff to the end) and after padding packets. lpcm5: the LPCM audio frame
 *  header is 5 bytes (HD DVD), else 3 (DVD-Video). 0 when it does not parse
 *  (p->why says why): an LPCM packet shorter than its header or whose first
 *  access unit starts inside the header does not parse; one that holds only
 *  the header is a padding packet. */
int ff_mpegps_disc_packet(const uint8_t *s, int *off, MpegPsDiscPacket *p, int lpcm5);

/** A packet of a sector: < 0 stops the sector with that error. */
typedef int (*MpegPsDiscPacketCb)(void *opaque, const uint8_t *sec, const MpegPsDiscPacket *p);

enum MpegPsDiscStatus {
    MPEGPS_DISC_OK,               /**< every packet given to the callback */
    MPEGPS_DISC_EMPTY,            /**< all 0x00 or all 0xff: no packets */
    MPEGPS_DISC_BAD,              /**< its first packet runs past its end (a short header): left out */
    MPEGPS_DISC_REST_DROPPED,     /**< no packet starts after a video / private stream 1 packet: the rest
                                     of the sector is left out */
    MPEGPS_DISC_FAIL,             /**< not readable as a disc pack */
};

/** The packets of one sector to cb (padding, private stream 2 and system
 *  header packets too), with the reader's rules: an empty sector has none;
 *  at most two packets unless one is MPEG audio (stream_id 0xD0-0xDF); a
 *  packet that runs past the sector's end leaves the sector out (a short
 *  header) or fails; bytes that are no packet start after a video (0xE0)
 *  or private stream 1 packet drop the rest of the sector, elsewhere they
 *  fail. Returns a MpegPsDiscStatus (*why, *at: what and where), or cb's
 *  error. */
int ff_mpegps_disc_sector(const uint8_t *sec, int lpcm5, MpegPsDiscPacketCb cb, void *opaque, const char **why,
                           int *at);


/**
 * Parse MPEG-PES five-byte timestamp
 */
static inline int64_t ff_parse_pes_pts(const uint8_t *buf) {
    return (int64_t)(*buf & 0x0e) << 29 |
            (AV_RB16(buf+1) >> 1) << 15 |
             AV_RB16(buf+3) >> 1;
}

#endif /* AVFORMAT_MPEG_H */
