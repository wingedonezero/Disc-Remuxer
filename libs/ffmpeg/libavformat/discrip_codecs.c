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
#include <string.h>

#include "libavcodec/ac3_parser_internal.h"
#include "libavcodec/ac3defs.h"
#include "libavcodec/dca.h"
#include "libavcodec/mpegaudiodecheader.h"
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/intreadwrite.h"

#include "discrip.h"

enum {
    REVIEW_FRAME_LENGTH = 1,   /* frame length or rate changes within the stream */
    REVIEW_DEPENDENT,          /* E-AC-3 dependent substream (extra channels) */
    REVIEW_SUBSTREAM,          /* E-AC-3 second program (independent substream > 0) */
    REVIEW_DTS_SUBSTREAM_ONLY, /* DTS-HD substream without a core (DTS Express / LBR) */
    REVIEW_MPEG_AUDIO,         /* any MPEG audio track (user rule: look at it) */
    REVIEW_AAC_MPEG4_ADTS,     /* ADTS with the MPEG-4 ID */
    REVIEW_AAC_LATM,           /* any LATM / LOAS track */
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

static int header_ac3(DRAudio *a, const DRFrame *f, DRAudioHeader *out)
{
    AC3HeaderInfo h;

    if (ac3_header(f->data, f->size, &h) < 0)
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

/* ---- DTS (ETSI TS 102 114) ----
 * A core frame starts with the sync word 7F FE 80 01; a DTS-HD extension
 * substream (ETSI TS 102 114 7.4) with 64 58 20 25. FFmpeg's parser keeps a
 * core frame and the extension substream after it in one unit (DTS-HD); a
 * stream of extension substreams only (DTS Express / LBR) has no core. */

/* core sampling frequencies, SFREQ (Table 5-5) */
static const int dts_rates[16] = { 0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0 };

static int dts_core_sync(const uint8_t *d, int size)
{
    return size >= 4 && AV_RB32(d) == 0x7FFE8001;
}

static int dts_exss_sync(const uint8_t *d, int size)
{
    return size >= 4 && AV_RB32(d) == 0x64582025;
}

static int dts_core(const uint8_t *d, int size, DCACoreFrameHeader *h)
{
    if (!dts_core_sync(d, size) || size < 16 || avpriv_dca_parse_core_frame_header(h, d, size) < 0)
        return -1;
    return dts_rates[h->sr_code] ? 0 : -1;
}

static int check_dts(const uint8_t *data, int size)
{
    DCACoreFrameHeader h;
    return dts_core(data, size, &h) >= 0 || dts_exss_sync(data, size);
}

static int header_dts(DRAudio *a, const DRFrame *f, DRAudioHeader *out)
{
    DCACoreFrameHeader h;

    if (dts_core(f->data, f->size, &h) >= 0) {
        out->rate    = dts_rates[h.sr_code];
        out->samples = h.npcmblocks * 32;
        return 0;
    }
    if (dts_exss_sync(f->data, f->size) && f->samples > 0 && f->rate > 0) {
        out->rate    = f->rate;
        out->samples = f->samples;
        return 0;
    }
    return -1;
}

/* A unit with a core is timed by its core frame header (npcmblocks x 32
 * samples); a core-only track keeps only the core frame. A unit without a
 * core is timed by FFmpeg's parser (extension substream asset) and reviewed. */
static int duration_dts(DRAudio *a, DRFrame *f)
{
    const DRAudioHeader *s = ff_discrip_audio_header(a);
    DCACoreFrameHeader h;
    int rate, samples;

    if (dts_core(f->data, f->size, &h) >= 0) {
        if ((ff_discrip_audio_flags(a) & DR_AUDIO_CORE_ONLY) && h.frame_size < f->size)
            f->size = h.frame_size;
        rate    = dts_rates[h.sr_code];
        samples = h.npcmblocks * 32;
    } else if (dts_exss_sync(f->data, f->size) && !(ff_discrip_audio_flags(a) & DR_AUDIO_CORE_ONLY) &&
               f->samples > 0 && f->rate > 0) {
        ff_discrip_audio_review(a, REVIEW_DTS_SUBSTREAM_ONLY, "DTS-HD extension substream without a core "
                                "(DTS Express / LBR), timed by FFmpeg's parser");
        rate    = f->rate;
        samples = f->samples;
    } else
        return AVERROR_INVALIDDATA;
    if (rate != s->rate || samples != s->samples)
        ff_discrip_audio_review(a, REVIEW_FRAME_LENGTH, "a frame's length or sample rate differs from the "
                                "stream's first frame (timed by its own values)");
    f->dur = (int64_t)((uint64_t)samples * DR_TICKS_PER_SECOND / (uint64_t)rate);
    return 0;
}

static const DRAudioRules audio_dts = {
    .header   = header_dts,
    .sync     = sync_always,
    .duration = duration_dts,
};

/* ---- MPEG-1 / MPEG-2 audio (ISO/IEC 11172-3, 13818-3) ----
 * Every frame stands alone and lasts its own samples: 384 (Layer I), 1152
 * (Layer II, Layer III of MPEG-1), 576 (Layer III of MPEG-2 / 2.5). Frames
 * are never sync units (the reference never drops them for audio skew).
 * Every MPEG audio track is reported for review (user rule 2026-10-05). */

static int mpa_header(const uint8_t *d, int size, MPADecodeHeader *h)
{
    uint32_t head;

    if (size < 4)
        return -1;
    head = AV_RB32(d);
    if (ff_mpa_check_header(head) < 0 || avpriv_mpegaudio_decode_header(h, head) != 0)
        return -1;
    return 0;
}

static int mpa_samples(const MPADecodeHeader *h)
{
    return h->layer == 1 ? 384 : h->layer == 2 || !h->lsf ? 1152 : 576;
}

static int check_mpa(const uint8_t *data, int size)
{
    MPADecodeHeader h;
    return mpa_header(data, size, &h) >= 0;
}

static int header_mpa(DRAudio *a, const DRFrame *f, DRAudioHeader *out)
{
    MPADecodeHeader h;

    if (mpa_header(f->data, f->size, &h) < 0)
        return -1;
    out->rate    = h.sample_rate;
    out->samples = mpa_samples(&h);
    return 0;
}

static int duration_mpa(DRAudio *a, DRFrame *f)
{
    MPADecodeHeader h;

    if (mpa_header(f->data, f->size, &h) < 0 || !h.sample_rate)
        return AVERROR_INVALIDDATA;
    ff_discrip_audio_review(a, REVIEW_MPEG_AUDIO, "MPEG audio track");
    f->dur = (int64_t)((uint64_t)mpa_samples(&h) * DR_TICKS_PER_SECOND / h.sample_rate);
    return 0;
}

static const DRAudioRules audio_mpa = {
    .header   = header_mpa,
    .duration = duration_mpa,
};

/* ---- AAC (ISO/IEC 13818-7, 14496-3) ----
 * ADTS: a frame of 1 + number_of_raw_data_blocks raw blocks of 1024 samples
 * at the sampling frequency index's rate (HE-AAC doubles rate and samples,
 * the duration stays). A sync unit is an ADTS frame whose channel
 * configuration is set or whose first raw element is a program config
 * element. The reference requires the MPEG-2 ID as well, so a stream with the
 * MPEG-4 ID never gets its values there and its track stays empty; here such
 * frames count the same (only the ID bit differs) and are reviewed. LATM / LOAS: a sync unit carries its
 * StreamMuxConfig (useSameStreamMux 0); the stream's values come from FFmpeg's
 * LATM decoder on that unit; every LATM track is reviewed. */

static const int aac_rates[16] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                   16000, 12000, 11025, 8000, 7350, 0, 0, 0 };

static int adts(const uint8_t *d, int size)
{
    return size >= 7 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0;
}

static int check_adts(const uint8_t *d, int size)
{
    return adts(d, size) && aac_rates[(d[2] >> 2) & 0xF];
}

static int sync_adts(const uint8_t *d, int size)
{
    int chcfg = ((d[2] & 1) << 2) | (d[3] >> 6);
    int raw   = (d[1] & 1) ? 7 : 9;   /* header without / with CRC */

    if (!adts(d, size))
        return 0;
    return chcfg || (size > raw && (d[raw] >> 5) == 5);
}

static int header_adts(DRAudio *a, const DRFrame *f, DRAudioHeader *out)
{
    if (!check_adts(f->data, f->size))
        return -1;
    out->rate    = aac_rates[(f->data[2] >> 2) & 0xF];
    out->samples = 1024 * ((f->data[6] & 3) + 1);
    return 0;
}

static int duration_adts(DRAudio *a, DRFrame *f)
{
    int rate, samples;

    if (!check_adts(f->data, f->size))
        return AVERROR_INVALIDDATA;
    if (!(f->data[1] & 0x08))
        ff_discrip_audio_review(a, REVIEW_AAC_MPEG4_ADTS, "AAC ADTS frames with the MPEG-4 ID");
    rate    = aac_rates[(f->data[2] >> 2) & 0xF];
    samples = 1024 * ((f->data[6] & 3) + 1);
    f->dur  = (int64_t)((uint64_t)samples * DR_TICKS_PER_SECOND / rate);
    return 0;
}

static const DRAudioRules audio_adts = {
    .header   = header_adts,
    .sync     = sync_adts,
    .duration = duration_adts,
};

static int loas(const uint8_t *d, int size)
{
    return size >= 4 && d[0] == 0x56 && (d[1] & 0xE0) == 0xE0;
}

static int sync_latm(const uint8_t *d, int size)
{
    return loas(d, size) && !(d[3] & 0x80);   /* useSameStreamMux */
}

static int header_latm(DRAudio *a, const DRFrame *f, DRAudioHeader *out)
{
    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_AAC_LATM);
    AVCodecContext *ctx = dec ? avcodec_alloc_context3(dec) : NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int ret = -1;

    if (ctx && pkt && fr && avcodec_open2(ctx, dec, NULL) >= 0 && av_new_packet(pkt, f->size) >= 0) {
        memcpy(pkt->data, f->data, f->size);
        if (avcodec_send_packet(ctx, pkt) >= 0 && avcodec_receive_frame(ctx, fr) >= 0 &&
            fr->nb_samples > 0 && fr->sample_rate > 0) {
            out->rate    = fr->sample_rate;
            out->samples = fr->nb_samples;
            ret = 0;
        }
    }
    avcodec_free_context(&ctx);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    return ret;
}

static void inspect_latm(DRAudio *a, const DRFrame *f)
{
    ff_discrip_audio_review(a, REVIEW_AAC_LATM, "AAC LATM / LOAS track");
}

static const DRAudioRules audio_latm = {
    .header  = header_latm,
    .sync    = sync_latm,
    .inspect = inspect_latm,
};

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
    { AV_CODEC_ID_DTS,          "dts",          anchor_unit, check_dts,  &audio_dts  },
    { AV_CODEC_ID_PCM_DVD,      "pcm_dvd",      anchor_unit      },
    { AV_CODEC_ID_MP1,          "mp1",          anchor_unit, check_mpa,  &audio_mpa  },
    { AV_CODEC_ID_MP2,          "mp2",          anchor_unit, check_mpa,  &audio_mpa  },
    { AV_CODEC_ID_MP3,          "mp3",          anchor_unit, check_mpa,  &audio_mpa  },
    { AV_CODEC_ID_AAC,          "aac",          anchor_unit, check_adts, &audio_adts },
    { AV_CODEC_ID_AAC_LATM,     "aac_latm",     anchor_unit, loas,       &audio_latm },
    { AV_CODEC_ID_DVD_SUBTITLE, "dvd_subtitle", anchor_unit      },
};

const DRCodec *ff_discrip_codec(enum AVCodecID id)
{
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++)
        if (codecs[i].id == id)
            return &codecs[i];
    return NULL;
}
