/*
 * The title timeline: the codec table.
 */

#include <stddef.h>
#include <string.h>


#include "base/crc16.h"
#include "parse/rules.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/vlog.h"

#include "timeline/disctl.h"

enum {
    REVIEW_FRAME_LENGTH = 1,   /* frame length or rate changes within the stream */
    REVIEW_DEPENDENT,          /* E-AC-3 dependent substream (extra channels) */
    REVIEW_SUBSTREAM,          /* E-AC-3 second program (independent substream > 0) */
    REVIEW_DTS_SUBSTREAM_ONLY, /* DTS-HD substream without a core, lossless only (XLL) */
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

/* H.264 (ITU-T H.264 Annex B): the first NAL unit of the access unit that is
 * a sequence or picture parameter set, a coded slice (types 1, 5) or a
 * coded slice extension (20) takes the PES time (at its start code); a
 * delimiter, SEI, filler or end unit does not. */
static int anchor_h264(const uint8_t *data, int size)
{
    for (int i = 0; i + 3 < size; i++)
        if (!data[i] && !data[i + 1] && data[i + 2] == 1) {
            int t = data[i + 3] & 0x1f;
            if (t == 1 || t == 5 || t == 7 || t == 8 || t == 20)
                return i;
            i += 2;
        }
    return -1;
}

/* HEVC (ITU-T H.265 Annex B): the first NAL unit of the access unit that is
 * a video, sequence or picture parameter set or a coded slice segment
 * (types 0-9, 16-21) takes the PES time (at its start code); a delimiter,
 * SEI, filler or end unit does not. */
static int hevc_timed(int t)
{
    return t <= 9 || (t >= 16 && t <= 21) || (t >= 32 && t <= 34);
}

static int anchor_hevc(const uint8_t *data, int size)
{
    for (int i = 0; i + 3 < size; i++)
        if (!data[i] && !data[i + 1] && data[i + 2] == 1) {
            if (hevc_timed((data[i + 3] >> 1) & 0x3f))
                return i;
            i += 2;
        }
    return -1;
}

/* ---- AC-3 / E-AC-3 (ATSC A/52, Annex E for E-AC-3) ----
 * A syncframe starts with 0x0B77. bsid 10..16 = E-AC-3 syntax, else AC-3
 * (the split the HD DVD track probe uses too). An AC-3 track keeps only AC-3
 * syncframes, an E-AC-3 track only E-AC-3 ones. */

static int ac3_header(const uint8_t *data, int size, DrAc3Header *h)
{
    if (size < 7 || data[0] != 0x0B || data[1] != 0x77)
        return -1;
    return dr_ac3_header(data, size, h) < 0 ? -1 : 0;
}

static int is_eac3(const DrAc3Header *h)
{
    return h->bitstream_id >= 10 && h->bitstream_id <= 16;
}

static int check_ac3(const uint8_t *data, int size)
{
    DrAc3Header h;
    return ac3_header(data, size, &h) >= 0 && !is_eac3(&h);
}

static int check_eac3(const uint8_t *data, int size)
{
    DrAc3Header h;
    return ac3_header(data, size, &h) >= 0 && is_eac3(&h);
}

static int header_ac3(DRAudio *a, const DRFrame *f, DRAudioHeader *out)
{
    DrAc3Header h;

    if (ac3_header(f->data, f->size, &h) < 0)
        return -1;
    out->rate    = h.rate;
    out->samples = h.blocks * 256;
    return 0;
}

static int sync_always(const uint8_t *data, int size)
{
    return 1;
}

/* Each unit is timed by its own first syncframe. A unit whose length or rate
 * is not the stream's (the first sync unit's) is timed by its own values and
 * reported for review. */
static int duration_ac3(DRAudio *a, DRFrame *f)
{
    const DRAudioHeader *s = ff_disctl_audio_header(a);
    DrAc3Header h;

    if (ac3_header(f->data, f->size, &h) < 0 || !h.rate)
        return DR_ERR_INVALID;
    if (h.rate != s->rate || h.blocks * 256 != s->samples)
        ff_disctl_audio_review(a, REVIEW_FRAME_LENGTH, "a frame's length or sample rate differs from the "
                                "stream's first frame (timed by its own values)");
    f->dur = (int64_t)((uint64_t)h.blocks * 256 * DR_TICKS_PER_SECOND / h.rate);
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

        if (strmtyp == 1)                   /* a dependent substream */
            ff_disctl_audio_review(a, REVIEW_DEPENDENT, "E-AC-3 dependent substream (extra channels, e.g. 7.1): "
                                    "kept in the same unit as its independent frame");
        else if (sub)
            ff_disctl_audio_review(a, REVIEW_SUBSTREAM, "E-AC-3 independent substream > 0 (a further program): "
                                    "kept in the same unit as substream 0");
        off += size;
    }
}

/* An AC-3 track keeps each unit's AC-3 frame alone: the parser joins the
 * E-AC-3 frames after an AC-3 frame to it (they belong to the same time),
 * and FFmpeg's eac3_core filter takes them off. */
static DRCoreFilter filter_ac3(int flags)
{
    return dr_eac3_core;
}

static const DRAudioRules audio_ac3 = {
    .header   = header_ac3,
    .sync     = sync_always,
    .duration = duration_ac3,
    .filter   = filter_ac3,
};

static const DRAudioRules audio_eac3 = {
    .header   = header_ac3,
    .sync     = sync_always,
    .duration = duration_ac3,
    .inspect  = inspect_eac3,
};

/* ---- DTS (ETSI TS 102 114) ----
 * A core frame starts with the sync word 7F FE 80 01; a DTS-HD extension
 * substream (ETSI TS 102 114 7.4) with 64 58 20 25. The DTS rule keeps a
 * core frame and the extension substream after it in one unit (DTS-HD); a
 * stream of extension substreams only (DTS Express / LBR) has no core. */

static int dts_core_sync(const uint8_t *d, int size)
{
    return size >= 4 && dr_rb32(d) == 0x7FFE8001;
}

static int dts_exss_sync(const uint8_t *d, int size)
{
    return size >= 4 && dr_rb32(d) == 0x64582025;
}

static int dts_core(const uint8_t *d, int size, DrDcaHeader *h)
{
    if (!dts_core_sync(d, size) || size < 16 || dr_dca_core_header(d, size, h) < 0)
        return -1;
    return h->rate ? 0 : -1;
}

static int check_dts(const uint8_t *data, int size)
{
    DrDcaHeader h;
    return dts_core(data, size, &h) >= 0 || dts_exss_sync(data, size);
}

/* A frame of a DTS-HD stream without a core (DR_AUDIO_EXT_ONLY): its rate
 * and samples from its LBR (DTS Express) or XLL header; the track's last LBR
 * rate code is the audio stage's state. DR_DCA_EXSS_LBR / _XLL, or < 0. */
static int exss_timing(DRAudio *a, const DRFrame *f, int *rate, int *samples)
{
    int *lbr_code = ff_disctl_audio_state(a);

    if (!(ff_disctl_audio_flags(a) & DR_AUDIO_EXT_ONLY) || !lbr_code)
        return DR_ERR_INVALID;
    return dr_dca_exss_timing(f->data, f->size, lbr_code, rate, samples);
}

static int header_dts(DRAudio *a, const DRFrame *f, DRAudioHeader *out)
{
    DrDcaHeader h;

    if (dts_core(f->data, f->size, &h) >= 0) {
        out->rate    = h.rate;
        out->samples = h.samples;
        return 0;
    }
    if (exss_timing(a, f, &out->rate, &out->samples) > 0)
        return 0;
    return -1;
}

/* A unit with a core frame is timed by its core frame header (npcmblocks x
 * 32 samples). A stream without a core (DR_AUDIO_EXT_ONLY) has extension
 * substream frames only, each timed by its LBR (DTS Express) or XLL header;
 * no other track takes such a unit. */
static int duration_dts(DRAudio *a, DRFrame *f)
{
    const DRAudioHeader *s = ff_disctl_audio_header(a);
    DrDcaHeader h;
    int rate, samples, kind;

    if (dts_core(f->data, f->size, &h) >= 0) {
        rate    = h.rate;
        samples = h.samples;
    } else if ((kind = exss_timing(a, f, &rate, &samples)) > 0) {
        /* DTS Express (LBR) is checked on real discs; a lossless-only
         * stream (XLL without a core) is not yet */
        if (kind == DR_DCA_EXSS_XLL)
            ff_disctl_audio_review(a, REVIEW_DTS_SUBSTREAM_ONLY, "DTS-HD lossless stream without a core, each "
                                    "frame timed by its XLL header");
    } else
        return DR_ERR_INVALID;
    if (rate != s->rate || samples != s->samples)
        ff_disctl_audio_review(a, REVIEW_FRAME_LENGTH, "a frame's length or sample rate differs from the "
                                "stream's first frame (timed by its own values)");
    f->dur = (int64_t)((uint64_t)samples * DR_TICKS_PER_SECOND / (uint64_t)rate);
    return 0;
}

/* A DTS track taken out of a DTS-HD stream keeps each unit's DTS frame
 * alone (FFmpeg's dca_core filter takes the DTS-HD extension off). */
static DRCoreFilter filter_dts(int flags)
{
    return flags & DR_AUDIO_EXTRACTED ? dr_dca_core : NULL;
}

static const DRAudioRules audio_dts = {
    .header   = header_dts,
    .sync     = sync_always,
    .duration = duration_dts,
    .filter   = filter_dts,
};

/* ---- MPEG-1 / MPEG-2 audio (ISO/IEC 11172-3, 13818-3) ----
 * Every frame stands alone and lasts its own samples: 384 (Layer I), 1152
 * (Layer II, Layer III of MPEG-1), 576 (Layer III of MPEG-2 / 2.5). Frames
 * are never sync units (the reference never drops them for audio skew).
 * Every MPEG audio track is reported for review (user rule 2026-10-05). */

static int check_mpa(const uint8_t *data, int size)
{
    DrMpaHeader h;
    return dr_mpa_header(data, size, &h) >= 0;
}

static int header_mpa(DRAudio *a, const DRFrame *f, DRAudioHeader *out)
{
    DrMpaHeader h;

    if (dr_mpa_header(f->data, f->size, &h) < 0)
        return -1;
    out->rate    = h.rate;
    out->samples = h.samples;
    return 0;
}

static int duration_mpa(DRAudio *a, DRFrame *f)
{
    DrMpaHeader h;

    if (dr_mpa_header(f->data, f->size, &h) < 0 || !h.rate)
        return DR_ERR_INVALID;
    ff_disctl_audio_review(a, REVIEW_MPEG_AUDIO, "MPEG audio track");
    f->dur = (int64_t)((uint64_t)h.samples * DR_TICKS_PER_SECOND / h.rate);
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
        return DR_ERR_INVALID;
    if (!(f->data[1] & 0x08))
        ff_disctl_audio_review(a, REVIEW_AAC_MPEG4_ADTS, "AAC ADTS frames with the MPEG-4 ID");
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
    const DRTimelineHost *host = ff_disctl_host();

    if (!host || !host->latm_header ||
        host->latm_header(f->data, f->size, &out->rate, &out->samples) < 0 ||
        out->samples <= 0 || out->rate <= 0)
        return -1;
    return 0;
}

static void inspect_latm(DRAudio *a, const DRFrame *f)
{
    ff_disctl_audio_review(a, REVIEW_AAC_LATM, "AAC LATM / LOAS track");
}

static const DRAudioRules audio_latm = {
    .header  = header_latm,
    .sync    = sync_latm,
    .inspect = inspect_latm,
};

/* ---- linear PCM (DVD-Video audio_attr / the audio frame header of each
 * packet; Blu-ray: the header of each unit; disctl_lpcm.c) ----
 * A unit is one frame of the size the headers state (no sync word: the
 * frames follow each other); every unit is a key frame and never a sync
 * unit; it lasts its samples (rate / 600 on DVD-Video, rate / 1200 on HD
 * DVD, the payload's on Blu-ray) and leaves as little-endian PCM (Blu-ray:
 * without its header, the channels in the channel mask's order without the
 * padding channel). */

static int header_lpcm(DRAudio *a, const DRFrame *f, DRAudioHeader *out)
{
    DRLpcm *p = ff_disctl_audio_state(a);

    if (p && p->bd && !p->init && ff_disctl_lpcm_bd_header(p, ff_disctl_audio_log(a), f->data, f->size) < 0)
        return -1;
    if (!p || !p->init)
        return -1;
    out->rate    = p->rate;
    out->samples = p->spf;
    return 0;
}

static int duration_lpcm(DRAudio *a, DRFrame *f)
{
    DRLpcm *p = ff_disctl_audio_state(a);
    const uint8_t *in;
    DrBuf *buf;
    int n, bps, size, ret;

    if (!p || !p->init)
        return DR_ERR_INVALID;
    in   = f->data;
    size = f->size;
    if (p->bd) {
        if ((ret = ff_disctl_lpcm_bd_header(p, ff_disctl_audio_log(a), f->data, f->size)) < 0)
            return ret;
        in   += 4;
        size -= 4;
    }
    if (!(buf = dr_buf_alloc(size * 6 / 5 + 16 + DR_BUF_PADDING)))
        return DR_ERR(ENOMEM);
    n = ff_disctl_lpcm_convert(p, in, size, dr_buf_data(buf));
    memset(dr_buf_data(buf) + n, 0, DR_BUF_PADDING);
    dr_buf_unref(&f->buf);
    f->buf  = buf;
    f->data = dr_buf_data(buf);
    f->size = n;
    bps     = p->channels * (p->bits == 16 ? 2 : 3);
    f->dur  = (int64_t)((uint64_t)(n / bps) * DR_TICKS_PER_SECOND / (uint64_t)p->rate);
    return 0;
}

static const DRAudioRules audio_lpcm = {
    .header   = header_lpcm,
    .duration = duration_lpcm,
};

/* ---- output unit checks (stage 5) ---- */

/* MPEG video: the unit starts with a start code and holds a picture start
 * code. VC-1: a start code and a frame start code. */
static int verify_mpegvideo(const uint8_t *d, int size)
{
    return size >= 4 && !d[0] && !d[1] && d[2] == 1 && anchor_mpegvideo(d, size) >= 0 ? DR_UNIT_OK : DR_UNIT_BAD;
}

static int verify_vc1(const uint8_t *d, int size)
{
    return size >= 4 && !d[0] && !d[1] && d[2] == 1 && anchor_vc1(d, size) >= 0 ? DR_UNIT_OK : DR_UNIT_BAD;
}

/* H.264: the unit starts with a start code and holds a coded slice. */
static int verify_h264(const uint8_t *d, int size)
{
    int i = 0;

    while (i < size && !d[i])
        i++;
    if (i < 2 || i + 1 >= size || d[i] != 1)
        return DR_UNIT_BAD;
    for (; i + 3 < size; i++)
        if (!d[i] && !d[i + 1] && d[i + 2] == 1 && ((d[i + 3] & 0x1f) == 1 || (d[i + 3] & 0x1f) == 5))
            return DR_UNIT_OK;
    return DR_UNIT_BAD;
}

/* HEVC: the unit starts with a start code and holds a coded slice
 * segment. */
static int verify_hevc(const uint8_t *d, int size)
{
    int i = 0;

    while (i < size && !d[i])
        i++;
    if (i < 2 || i + 1 >= size || d[i] != 1)
        return DR_UNIT_BAD;
    for (; i + 3 < size; i++)
        if (!d[i] && !d[i + 1] && d[i + 2] == 1) {
            int t = (d[i + 3] >> 1) & 0x3f;
            if (t <= 9 || (t >= 16 && t <= 21))
                return DR_UNIT_OK;
        }
    return DR_UNIT_BAD;
}

/* AC-3 / E-AC-3: the unit is whole syncframes; each one's CRC over the
 * frame after its sync word comes out 0 (as FFmpeg's decoder checks it). */
static int verify_ac3(const uint8_t *d, int size)
{
    int ret = DR_UNIT_OK;

    if (!size)
        return DR_UNIT_BAD;
    for (int o = 0; o < size; ) {
        DrAc3Header h;
        if (ac3_header(d + o, size - o, &h) < 0 || h.frame_size < 7 || h.frame_size > size - o)
            return DR_UNIT_BAD;
        if (dr_crc16_ansi(0, d + o + 2, h.frame_size - 2))
            ret = DR_UNIT_CRC;
        o += h.frame_size;
    }
    return ret;
}

/* DTS: a core frame whose size fits, the rest an extension substream; or
 * an extension substream alone. */
static int verify_dts(const uint8_t *d, int size)
{
    DrDcaHeader h;

    if (dts_core(d, size, &h) >= 0) {
        if (h.frame_size > size)
            return DR_UNIT_BAD;
        return h.frame_size == size || dts_exss_sync(d + h.frame_size, size - h.frame_size) ? DR_UNIT_OK : DR_UNIT_BAD;
    }
    return dts_exss_sync(d, size) ? DR_UNIT_OK : DR_UNIT_BAD;
}

/* DTS: the checksums of the extension substream (after the core frame, or
 * the unit alone); a core frame carries none that is checked. */
static int sums_dts(void *state, const uint8_t *d, int size, const char **why)
{
    DrDcaHeader h;
    int off = dts_core(d, size, &h) >= 0 ? h.frame_size : 0;

    if (off >= size || !dts_exss_sync(d + off, size - off))
        return DR_UNIT_OK;
    return dr_dca_exss_checksums(d + off, size - off, why) ? DR_UNIT_OK : DR_UNIT_CRC;
}

/* MPEG audio: one frame of its header's size (free format: not sized). */
static int verify_mpa(const uint8_t *d, int size)
{
    DrMpaHeader h;

    if (dr_mpa_header(d, size, &h) < 0)
        return DR_UNIT_BAD;
    return !h.frame_size || h.frame_size == size ? DR_UNIT_OK : DR_UNIT_BAD;
}

/* AAC ADTS: one frame of its header's frame_length. LATM / LOAS: one
 * AudioMuxElement of its audioMuxLengthBytes + 3. */
static int verify_adts(const uint8_t *d, int size)
{
    if (!check_adts(d, size))
        return DR_UNIT_BAD;
    return (((d[3] & 3) << 11) | (d[4] << 3) | (d[5] >> 5)) == size ? DR_UNIT_OK : DR_UNIT_BAD;
}

static int verify_latm(const uint8_t *d, int size)
{
    if (!loas(d, size))
        return DR_UNIT_BAD;
    return (((d[1] & 0x1F) << 8) | d[2]) + 3 == size ? DR_UNIT_OK : DR_UNIT_BAD;
}

/* VC-1 (SMPTE 421M Annex G): the sequence header (start code 0x0F) and
 * what follows it up to the first frame (0x0D), the entry point among it. */
static int vc1_headers(const uint8_t *h, int size, int *start, int *end)
{
    int seq = -1;

    for (int i = 0; i + 3 < size; i++)
        if (!h[i] && !h[i + 1] && h[i + 2] == 1) {
            if (h[i + 3] == 0x0f && seq < 0)
                seq = i;
            else if (h[i + 3] == 0x0d && seq >= 0) {
                *start = seq;
                *end   = i;
                return 1;
            }
        }
    return 0;
}

static const DRCodec codecs[] = {
    { .id = DR_CODEC_MPEG1VIDEO, .name = "mpeg1video", .anchor = anchor_mpegvideo, .video = &ff_disctl_video_mpv,
      .verify = verify_mpegvideo, .video_bit_rate = DR_RATE_STATED, .no_display_aspect = 1 },
    { .id = DR_CODEC_MPEG2VIDEO, .name = "mpeg2video", .anchor = anchor_mpegvideo, .video = &ff_disctl_video_mpv,
      .verify = verify_mpegvideo, .video_bit_rate = DR_RATE_MAX },
    { .id = DR_CODEC_VC1, .name = "vc1", .anchor = anchor_vc1, .video = &ff_disctl_video_vc1, .verify = verify_vc1,
      .split = DR_SPLIT_VC1, .decoder_headers = vc1_headers },
    { .id = DR_CODEC_H264, .name = "h264", .anchor = anchor_h264, .video = &ff_disctl_video_h264,
      .verify = verify_h264, .split = DR_SPLIT_H264, .parser_picture = 1 },
    { .id = DR_CODEC_HEVC, .name = "hevc", .anchor = anchor_hevc, .video = &ff_disctl_video_hevc,
      .verify = verify_hevc, .split = DR_SPLIT_HEVC, .parser_picture = 1 },
    { .id = DR_CODEC_AC3, .name = "ac3", .check = check_ac3, .audio = &audio_ac3,
      .verify = verify_ac3, .split = DR_SPLIT_AC3 },
    { .id = DR_CODEC_EAC3, .name = "eac3", .check = check_eac3, .audio = &audio_eac3,
      .verify = verify_ac3 },
    { .id = DR_CODEC_TRUEHD, .name = "truehd", .check = ff_disctl_mlp_check,
      .audio = &ff_disctl_audio_mlp, .split = DR_SPLIT_MLP,
      .verify = ff_disctl_mlp_verify, .verify_sums = ff_disctl_mlp_checksums },
    { .id = DR_CODEC_MLP, .name = "mlp", .check = ff_disctl_mlp_check,
      .audio = &ff_disctl_audio_mlp, .split = DR_SPLIT_MLP,
      .verify = ff_disctl_mlp_verify, .verify_sums = ff_disctl_mlp_checksums },
    { .id = DR_CODEC_DTS, .name = "dts", .check = check_dts, .audio = &audio_dts,
      .verify = verify_dts, .verify_sums = sums_dts, .split = DR_SPLIT_DTS },
    { .id = DR_CODEC_PCM_DVD, .name = "pcm_dvd", .audio = &audio_lpcm,
      .split = DR_SPLIT_PCM_DVD, .bits_coded = 1 },
    { .id = DR_CODEC_PCM_BLURAY, .name = "pcm_bluray", .audio = &audio_lpcm,
      .split = DR_SPLIT_PCM_BLURAY, .bits_coded = 1 },
    { .id = DR_CODEC_MP1, .name = "mp1", .check = check_mpa, .audio = &audio_mpa,
      .verify = verify_mpa, .split = DR_SPLIT_MPA },
    { .id = DR_CODEC_MP2, .name = "mp2", .check = check_mpa, .audio = &audio_mpa,
      .verify = verify_mpa, .split = DR_SPLIT_MPA },
    { .id = DR_CODEC_MP3, .name = "mp3", .check = check_mpa, .audio = &audio_mpa,
      .verify = verify_mpa, .split = DR_SPLIT_MPA },
    { .id = DR_CODEC_AAC, .name = "aac", .check = check_adts, .audio = &audio_adts,
      .verify = verify_adts, .split = DR_SPLIT_ADTS },
    { .id = DR_CODEC_AAC_LATM, .name = "aac_latm", .check = loas, .audio = &audio_latm,
      .verify = verify_latm, .split = DR_SPLIT_LATM },
    { .id = DR_CODEC_DVD_SUBTITLE, .name = "dvd_subtitle",
      .check = ff_disctl_spu_check, .split = DR_SPLIT_DVDSUB,
      .verify = ff_disctl_spu_verify, .info_without_data = 1 },
    { .id = DR_CODEC_HDMV_PGS_SUBTITLE, .name = "hdmv_pgs_subtitle",
      .check = ff_disctl_pgs_check, .split = DR_SPLIT_PGS, .verify = ff_disctl_pgs_verify, .info_without_data = 1 },
    /* Blu-ray text subtitles: one segment per unit; a dialog takes the PES time */
    { .id = DR_CODEC_HDMV_TEXT_SUBTITLE, .name = "hdmv_text_subtitle", .anchor = ff_disctl_textst_anchor,
      .check = ff_disctl_textst_check, .split = DR_SPLIT_TEXTST, .verify = ff_disctl_textst_verify,
      .info_without_data = 1 },
    /* closed captions as text cues (made by the caption converter) */
    { .id = DR_CODEC_SUBRIP, .name = "subrip", .info_without_data = 1 },
};

static const DRTimelineHost *disctl_host;

void ff_disctl_set_host(const DRTimelineHost *host)
{
    disctl_host = host;
}

const DRTimelineHost *ff_disctl_host(void)
{
    return disctl_host;
}

const char *ff_disctl_codec_id_name(DRCodecID id)
{
    const DRCodec *c = ff_disctl_codec(id);
    return c ? c->name : "?";
}

const DRCodec *ff_disctl_codec(DRCodecID id)
{
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++)
        if (codecs[i].id == id)
            return &codecs[i];
    return NULL;
}
