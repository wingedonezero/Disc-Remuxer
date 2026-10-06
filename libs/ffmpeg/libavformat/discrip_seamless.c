/*
 * The shared rip core: the audio of the seamless overlap search. Units are
 * decoded, turned into 32-bit samples, mixed down to one channel, and two
 * stretches of it are compared (a Pearson correlation in Q32, all integer).
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

#include <math.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discrip.h"

/* the mono mix: center 1, surround sqrt(2) / 8, LFE 0, normalised, as
 * Dolby Pro Logic II matrix encoding (only stereo outputs use it) */
#define MIX_CENTER   1.0
#define MIX_SURROUND 0.1767766952966369
#define MIX_LFE      0.0

typedef struct Mono {
    int32_t *s;
    int      n, cap;
} Mono;

static int mono_grow(Mono *m, int add)
{
    if (m->n + add > m->cap) {
        int cap = FFMAX(2 * m->cap, m->n + add);
        int32_t *p = av_realloc_array(m->s, cap, sizeof(*p));
        if (!p)
            return AVERROR(ENOMEM);
        m->s   = p;
        m->cap = cap;
    }
    return 0;
}

/* One sample of channel c as 32 bits (16-bit samples shifted up). */
static int32_t sample32(const AVFrame *fr, int c, int i, int channels)
{
    switch (fr->format) {
    case AV_SAMPLE_FMT_S16:  return (int32_t)((const int16_t *)fr->data[0])[i * channels + c] * 65536;
    case AV_SAMPLE_FMT_S16P: return (int32_t)((const int16_t *)fr->data[c])[i] * 65536;
    case AV_SAMPLE_FMT_S32:  return ((const int32_t *)fr->data[0])[i * channels + c];
    default:                 return ((const int32_t *)fr->data[c])[i];
    }
}

/* A decoded frame onto the mono samples: each output sample the input
 * samples times the Q30 coefficients, summed in 64 bits, shifted down by 30,
 * clipped. */
static int mono_add(Mono *m, const AVFrame *fr, const int32_t *q30, int channels)
{
    int ret = mono_grow(m, fr->nb_samples);

    if (ret < 0)
        return ret;
    for (int i = 0; i < fr->nb_samples; i++) {
        int64_t sum = 0;
        if (channels == 1) {
            m->s[m->n++] = sample32(fr, 0, i, 1);
            continue;
        }
        for (int c = 0; c < channels; c++)
            if (q30[c])
                sum += (int64_t)sample32(fr, c, i, channels) * q30[c];
        m->s[m->n++] = av_clipl_int32(sum >> 30);
    }
    return 0;
}

int ff_discrip_seamless_decode(void *log, enum AVCodecID codec, int rate, const DRFrame *pre, int nb_pre,
                               const DRFrame *range, int nb_range, int32_t **out, int *nb_out)
{
    const AVCodec *dec;
    AVCodecContext *ctx = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    Mono m = { 0 };
    int64_t *fdur = NULL, pre_dur = 0, range_dur = 0;
    int *fsamp = NULL, nb_f = 0, units = nb_pre + nb_range, ret = 0, channels = 0;
    int32_t q30[32] = { 0 };

    *out    = NULL;
    *nb_out = 0;
    if (!pkt || !fr) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((codec != AV_CODEC_ID_TRUEHD && codec != AV_CODEC_ID_MLP) || rate <= 0 || !units ||
        !(dec = avcodec_find_decoder(codec))) {
        av_log(log, AV_LOG_DEBUG, "Rip core: seamless search: %s is not decoded\n", avcodec_get_name(codec));
        goto end;
    }
    if (!(ctx = avcodec_alloc_context3(dec)) || !(fdur = av_calloc(units + 1, sizeof(*fdur))) ||
        !(fsamp = av_calloc(units + 1, sizeof(*fsamp)))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ctx->request_sample_fmt = AV_SAMPLE_FMT_S32;
    if (avcodec_open2(ctx, dec, NULL) < 0) {
        av_log(log, AV_LOG_DEBUG, "Rip core: seamless search: the %s decoder does not open\n", dec->name);
        goto end;
    }
    for (int k = 0; k <= units; k++) {
        const DRFrame *u = k < nb_pre ? &pre[k] : k < units ? &range[k - nb_pre] : NULL;
        int r;

        if (u) {
            if (av_new_packet(pkt, u->size) < 0) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            memcpy(pkt->data, u->data, u->size);
            if (k < nb_pre)
                pre_dur += u->dur;
            else
                range_dur += u->dur;
        }
        r = avcodec_send_packet(ctx, u ? pkt : NULL);
        av_packet_unref(pkt);
        if (r < 0 && r != AVERROR_EOF) {
            av_log(log, AV_LOG_DEBUG, "Rip core: seamless search: unit %d does not decode\n", k);
            goto end;
        }
        while ((r = avcodec_receive_frame(ctx, fr)) >= 0) {
            if (nb_f == units) {
                nb_f++;
                break;
            }
            if (!channels) {
                uint64_t layout = fr->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? fr->ch_layout.u.mask : 0;
                double mat[32];
                channels = fr->ch_layout.nb_channels;
                if (fr->format != AV_SAMPLE_FMT_S16 && fr->format != AV_SAMPLE_FMT_S16P &&
                    fr->format != AV_SAMPLE_FMT_S32 && fr->format != AV_SAMPLE_FMT_S32P) {
                    av_log(log, AV_LOG_DEBUG, "Rip core: seamless search: decoded samples are %s\n",
                           av_get_sample_fmt_name(fr->format));
                    goto end;
                }
                if (channels > 1) {
                    if (channels > 32 || ff_discrip_mix_matrix(layout, AV_CH_FRONT_CENTER, MIX_CENTER, MIX_SURROUND,
                                                               MIX_LFE, 1, mat, channels,
                                                               AV_MATRIX_ENCODING_DPLII) < 0) {
                        av_log(log, AV_LOG_DEBUG, "Rip core: seamless search: no mono mix for %d channels\n",
                               channels);
                        goto end;
                    }
                    for (int c = 0; c < channels; c++)
                        q30[c] = av_clipl_int32(llrint(1073741824.0 * mat[c]));
                }
            }
            if (fr->ch_layout.nb_channels != channels) {
                av_log(log, AV_LOG_DEBUG, "Rip core: seamless search: the channel count changes\n");
                goto end;
            }
            fsamp[nb_f] = m.n;
            fdur[nb_f]  = (int64_t)((uint64_t)fr->nb_samples * DR_TICKS_PER_SECOND / (uint64_t)fr->sample_rate);
            nb_f++;
            if ((ret = mono_add(&m, fr, q30, channels)) < 0)
                goto end;
            av_frame_unref(fr);
        }
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
            av_log(log, AV_LOG_DEBUG, "Rip core: seamless search: decoding fails after unit %d\n", k);
            goto end;
        }
    }
    /* one frame per unit, each as long as its unit */
    if (nb_f != units) {
        av_log(log, AV_LOG_DEBUG, "Rip core: seamless search: %d frames decoded from %d units\n", nb_f, units);
        goto end;
    }
    for (int k = 0; k < units; k++)
        if (fdur[k] != (k < nb_pre ? pre[k].dur : range[k - nb_pre].dur)) {
            av_log(log, AV_LOG_DEBUG, "Rip core: seamless search: frame %d lasts %"PRId64" ticks, its unit %"
                   PRId64"\n", k, fdur[k], k < nb_pre ? pre[k].dur : range[k - nb_pre].dur);
            goto end;
        }
    {
        /* the preroll's frames are dropped; of each other frame its first
         * rate x duration samples are kept, at most rate x the range's
         * duration in all */
        int64_t s = 0;
        int k = 0, o = 0;
        uint64_t want = (uint64_t)rate * (uint64_t)range_dur / DR_TICKS_PER_SECOND;
        int32_t *res;

        if (pre_dur)
            do
                s += fdur[k++];
            while (k < units && s < pre_dur);
        if (!(res = av_malloc_array(want + 1, sizeof(*res)))) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        for (; k < units && (uint64_t)o < want; k++) {
            int64_t take = (int64_t)rate * fdur[k] / DR_TICKS_PER_SECOND;
            take = FFMIN(take, (int64_t)want - o);
            take = FFMIN(take, (int64_t)((k + 1 < units ? fsamp[k + 1] : m.n) - fsamp[k]));
            memcpy(res + o, m.s + fsamp[k], take * sizeof(*res));
            o += take;
        }
        *out    = res;
        *nb_out = o;
        ret     = 1;
    }
end:
    av_free(m.s);
    av_free(fdur);
    av_free(fsamp);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return ret;
}

/* ---- the comparison ---- */

/* The integer square root by Newton steps from a power of two. */
static uint32_t isqrt(uint64_t x)
{
    int h = 63 - __builtin_clzll(x);
    uint64_t r = 1ULL << ((((unsigned)h ^ 0x3E) >> 1 ^ 0x1F) & 63);
    uint64_t d = x > r ? x - r : r - x;

    if (d >= 2)
        do {
            uint64_t q = x / r;
            r = (r + q) >> 1;
            d = q > r ? q - r : r - q;
        } while (d > 1);
    return (uint32_t)r;
}

/* A shift that brings the largest sample below 2^28. */
static int shift_of(uint32_t max)
{
    return max >= 0x10000000 ? 31 - __builtin_clz(max) - 27 : 0;
}

static int32_t mean_of(int64_t sum, uint32_t n)
{
    int64_t m = sum / (int64_t)n;
    return m >= 0x7FFFFFFF ? 0x7FFFFFFF : m < -0x7FFFFFFF ? INT32_MIN : (int32_t)m;
}

/* Variance (sum of squares of the shifted samples less their mean, over n)
 * and its integer square root; 0 when the sum is below n. */
/* A shifted sample less the mean, wrapping in 32 bits. */
static int32_t centred(int32_t x, int s, int32_t mean)
{
    return (int32_t)((uint32_t)(x >> s) - (uint32_t)mean);
}

static uint32_t sd_of(const int32_t *x, uint32_t n, int s, int32_t mean)
{
    uint64_t sum = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = (uint32_t)centred(x[i], s, mean);
        if ((int32_t)d < 0)
            d = -d;                                 /* |INT32_MIN| stays 2^31 */
        sum += (uint64_t)d * d;
    }
    return sum < n ? 0 : isqrt(sum / n);
}

uint32_t ff_discrip_seamless_corr(const int32_t *a, const int32_t *b, uint32_t n)
{
    int64_t sa = 0, sb = 0;
    uint32_t ma = 0, mb = 0, sda, sdb, t, l, h;
    int sha, shb, c;
    int32_t mean_a, mean_b;
    uint64_t pos = 0, neg = 0, num, sd, den;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = a[i] < 0 ? -(uint32_t)a[i] : (uint32_t)a[i];
        uint32_t y = b[i] < 0 ? -(uint32_t)b[i] : (uint32_t)b[i];
        sa += a[i];
        sb += b[i];
        ma = FFMAX(ma, x);
        mb = FFMAX(mb, y);
    }
    if (ma <= 99 && mb <= 99)
        return 0;                                   /* silence */
    sha    = shift_of(ma);
    shb    = shift_of(mb);
    mean_a = mean_of(sa, n) >> sha;
    mean_b = mean_of(sb, n) >> shb;
    sda    = sd_of(a, n, sha, mean_a);
    sdb    = sd_of(b, n, shb, mean_b);
    for (uint32_t i = 0; i < n; i++) {
        int64_t p = (int64_t)centred(a[i], sha, mean_a) * centred(b[i], shb, mean_b);
        if (p < 0)
            neg -= (uint64_t)p;
        else
            pos += (uint64_t)p;
    }
    if (pos <= neg)
        return 1;
    num = pos - neg;
    sd  = (uint64_t)sdb * sda;
    if (!sd)
        return 0xFFFFFFFF;
    /* num / (n x sda x sdb) in Q32 with normalising shifts */
    t    = __builtin_ctz(n);
    l    = n >> t;
    num >>= t;
    c    = (32 - __builtin_clz(l)) - __builtin_clzll(sd);
    if (c < 0)
        c = 0;
    num >>= c;
    sd  >>= c;
    den  = (uint64_t)l * sd;
    if (den <= num)
        return 0xFFFFFFFF;
    h    = 63 - __builtin_clzll(den);
    num <<= 63 - h;
    den  = den >> 32 ? den >> (h - 31) : den << (31 - h);
    if (!den)
        return 0xFFFFFFFF;
    return FFMIN(num / den, 0xFFFFFFFF);
}
