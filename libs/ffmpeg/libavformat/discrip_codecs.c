/*
 * The shared rip core: the codec table.
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

#include <stddef.h>

#include "discrip.h"

/* Offset of the first start code 00 00 01 <code> in a unit, or -1. */
static int start_code(const uint8_t *data, int size, uint8_t code)
{
    for (int i = 0; i + 3 < size; i++)
        if (!data[i] && !data[i + 1] && data[i + 2] == 1 && data[i + 3] == code)
            return i;
    return -1;
}

/* MPEG-1 / MPEG-2 video (ISO/IEC 13818-2): the picture header (start code
 * 0x00) takes the PES time; sequence, GOP and extension headers do not. */
static int anchor_mpegvideo(const uint8_t *data, int size)
{
    return start_code(data, size, 0x00);
}

/* VC-1 (SMPTE 421M Annex E): the frame start code 0x0D takes the PES time;
 * sequence header, entry point, field and slice start codes do not. */
static int anchor_vc1(const uint8_t *data, int size)
{
    return start_code(data, size, 0x0D);
}

/* Audio frames and sub-picture units: every unit can take a time. */
static int anchor_unit(const uint8_t *data, int size)
{
    return 0;
}

/* AC-3 / E-AC-3 syncframes start with the sync word 0x0B77 (ATSC A/52). */
static int check_ac3(const uint8_t *data, int size)
{
    return size >= 2 && data[0] == 0x0B && data[1] == 0x77;
}

/* DTS core frames start with the sync word 0x7FFE8001 (ETSI TS 102 114). */
static int check_dts(const uint8_t *data, int size)
{
    return size >= 4 && data[0] == 0x7F && data[1] == 0xFE && data[2] == 0x80 && data[3] == 0x01;
}

/* MPEG audio frames start with the 11-bit frame sync (ISO/IEC 11172-3). */
static int check_mpa(const uint8_t *data, int size)
{
    return size >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0;
}

static const DRCodec codecs[] = {
    { AV_CODEC_ID_MPEG1VIDEO,   "mpeg1video",   anchor_mpegvideo },
    { AV_CODEC_ID_MPEG2VIDEO,   "mpeg2video",   anchor_mpegvideo },
    { AV_CODEC_ID_VC1,          "vc1",          anchor_vc1       },
    { AV_CODEC_ID_AC3,          "ac3",          anchor_unit, check_ac3 },
    { AV_CODEC_ID_EAC3,         "eac3",         anchor_unit, check_ac3 },
    { AV_CODEC_ID_TRUEHD,       "truehd",       anchor_unit      },
    { AV_CODEC_ID_MLP,          "mlp",          anchor_unit      },
    { AV_CODEC_ID_DTS,          "dts",          anchor_unit, check_dts },
    { AV_CODEC_ID_PCM_DVD,      "pcm_dvd",      anchor_unit      },
    { AV_CODEC_ID_MP1,          "mp1",          anchor_unit, check_mpa },
    { AV_CODEC_ID_MP2,          "mp2",          anchor_unit, check_mpa },
    { AV_CODEC_ID_MP3,          "mp3",          anchor_unit, check_mpa },
    { AV_CODEC_ID_DVD_SUBTITLE, "dvd_subtitle", anchor_unit      },
};

const DRCodec *ff_discrip_codec(enum AVCodecID id)
{
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++)
        if (codecs[i].id == id)
            return &codecs[i];
    return NULL;
}
