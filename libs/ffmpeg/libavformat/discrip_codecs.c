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

#include "libavcodec/ac3_parser_internal.h"
#include "libavcodec/ac3defs.h"

#include "discrip.h"

enum {
    REVIEW_FRAME_LENGTH = 1,   /* frame length or rate changes within the stream */
    REVIEW_DEPENDENT,          /* E-AC-3 dependent substream (extra channels) */
    REVIEW_SUBSTREAM,          /* E-AC-3 second program (independent substream > 0) */
};

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

/* ---- AC-3 / E-AC-3 (ATSC A/52, Annex E for E-AC-3) ----
 * A syncframe starts with 0x0B77. bsid 10..16 = E-AC-3 syntax, else AC-3
 * (the split the HD DVD track probe uses too). An AC-3 track keeps only AC-3
 * syncframes, an E-AC-3 track only E-AC-3 ones. */

static int ac3_header(const uint8_t *data, int size, AC3HeaderInfo *h)
{
    AC3HeaderInfo *p = h;

    if (size < 7 || data[0] != 0x0B || data[1] != 0x77)
        return -1;
    return avpriv_ac3_parse_header(&p, data, size) < 0 ? -1 : 0;
}

static int is_eac3(const AC3HeaderInfo *h)
{
    return h->bitstream_id >= 10 && h->bitstream_id <= 16;
}

static int check_ac3(const uint8_t *data, int size)
{
    AC3HeaderInfo h;
    return ac3_header(data, size, &h) >= 0 && !is_eac3(&h);
}

static int check_eac3(const uint8_t *data, int size)
{
    AC3HeaderInfo h;
    return ac3_header(data, size, &h) >= 0 && is_eac3(&h);
}

static int header_ac3(DRAudio *a, const uint8_t *data, int size, DRAudioHeader *out)
{
    AC3HeaderInfo h;

    if (ac3_header(data, size, &h) < 0)
        return -1;
    out->rate    = h.sample_rate;
    out->samples = h.num_blocks * 256;
    return 0;
}

static int sync_always(const uint8_t *data, int size)
{
    return 1;
}

/* Each unit is timed by its own first syncframe. A unit whose length or rate
 * is not the stream's (the first sync unit's) is timed by its own values and
 * reported for review. An AC-3 unit is cut to its first syncframe: the bytes
 * after it are E-AC-3 frames the parser joined to it, not part of an AC-3
 * track. */
static int duration_ac3(DRAudio *a, DRFrame *f)
{
    const DRAudioHeader *s = ff_discrip_audio_header(a);
    AC3HeaderInfo h;

    if (ac3_header(f->data, f->size, &h) < 0 || !h.sample_rate)
        return AVERROR_INVALIDDATA;
    if (!is_eac3(&h) && h.frame_size < f->size)
        f->size = h.frame_size;
    if (h.sample_rate != s->rate || h.num_blocks * 256 != s->samples)
        ff_discrip_audio_review(a, REVIEW_FRAME_LENGTH, "a frame's length or sample rate differs from the "
                                "stream's first frame (timed by its own values)");
    f->dur = (int64_t)((uint64_t)h.num_blocks * 256 * DR_TICKS_PER_SECOND / h.sample_rate);
    return 0;
}

/* FFmpeg's parser keeps every frame up to the next independent frame of
 * substream 0 in one unit: dependent frames (extra channels of the same time,
 * e.g. 7.1) and independent frames of substreams 1..7 (further programs of the
 * same time; FFmpeg's header parser does not accept them). The frames of a
 * unit are walked by their syncinfo / frame size fields (A/52 Annex E E.1.2.1). */
static void inspect_eac3(DRAudio *a, const DRFrame *f)
{
    int off = 0;

    while (off + 4 <= f->size && f->data[off] == 0x0B && f->data[off + 1] == 0x77) {
        int strmtyp = f->data[off + 2] >> 6;
        int sub     = (f->data[off + 2] >> 3) & 7;
        int size    = ((((f->data[off + 2] & 7) << 8) | f->data[off + 3]) + 1) * 2;

        if (strmtyp == EAC3_FRAME_TYPE_DEPENDENT)
            ff_discrip_audio_review(a, REVIEW_DEPENDENT, "E-AC-3 dependent substream (extra channels, e.g. 7.1): "
                                    "kept in the same unit as its independent frame");
        else if (sub)
            ff_discrip_audio_review(a, REVIEW_SUBSTREAM, "E-AC-3 independent substream > 0 (a further program): "
                                    "kept in the same unit as substream 0");
        off += size;
    }
}

static const DRAudioRules audio_ac3 = {
    .header   = header_ac3,
    .sync     = sync_always,
    .duration = duration_ac3,
};

static const DRAudioRules audio_eac3 = {
    .header   = header_ac3,
    .sync     = sync_always,
    .duration = duration_ac3,
    .inspect  = inspect_eac3,
};

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
    { AV_CODEC_ID_AC3,          "ac3",          anchor_unit, check_ac3,  &audio_ac3  },
    { AV_CODEC_ID_EAC3,         "eac3",         anchor_unit, check_eac3, &audio_eac3 },
    { AV_CODEC_ID_TRUEHD,       "truehd",       anchor_unit, ff_discrip_mlp_check, &ff_discrip_audio_mlp,
      ff_discrip_mlp_unit_size, ff_discrip_mlp_resync },
    { AV_CODEC_ID_MLP,          "mlp",          anchor_unit, ff_discrip_mlp_check, &ff_discrip_audio_mlp,
      ff_discrip_mlp_unit_size, ff_discrip_mlp_resync },
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
