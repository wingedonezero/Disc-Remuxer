/*
 * The shared rip core: the rules for MLP and Dolby TrueHD access units.
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

#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/mlp_parse.h"
#include "libavutil/crc.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"

#include "discrip.h"

/* An access unit (AU) starts with a 4-byte header: check nibble, AU length
 * in 16-bit words (12 bits), input timing. A major-sync AU then holds the
 * major sync F8 72 6F BA (TrueHD) / BB (MLP) and the stream's parameters,
 * followed by the substream directory: one 16-bit word per substream (bit 15:
 * an extra word follows; bit 13: 2 check bytes end the substream; bits 11..0:
 * the substream's end in 16-bit words). A substream that ends the stream
 * finishes with the marker D2 34 and, in TrueHD, a word whose bit 13 says
 * that its last block is shortened by the low 13 bits (samples). */

#define HISTORY_MAX 256   /* AUs kept since the last major sync */

typedef struct MlpState {
    int truehd;           /* 0xBA major sync */
    int nsub;             /* substreams */
    int nsub_check;       /* substreams that must end the stream: min(nsub, 3) */
    int ext;              /* TrueHD extended substream info, 0 = none */
    int samples;          /* samples per AU */
    int rate;

    AVBufferRef *hist[HISTORY_MAX];   /* AUs since the last major sync */
    int          hist_size[HISTORY_MAX];
    int          nb_hist;
} MlpState;

static AVCRC crc_2d[1024];
static AVOnce crc_once = AV_ONCE_INIT;

static void crc_init(void)
{
    av_crc_init(crc_2d, 0, 16, 0x002D, sizeof(crc_2d));
}

static int major_sync(const uint8_t *p, int n)
{
    return n >= 8 && p[4] == 0xF8 && p[5] == 0x72 && p[6] == 0x6F && (p[7] & 0xFE) == 0xBA;
}

static int au_length(const uint8_t *p)
{
    return (((p[0] & 0xF) << 8) | p[1]) * 2;
}

/* Size of the major sync block from byte 4: 28 bytes, more with TrueHD
 * extended substream info. */
static int major_sync_size(const uint8_t *p)
{
    return p[7] == 0xBA && (p[0x1d] & 1) ? 30 + 2 * (p[0x1e] >> 4) : 28;
}

/* The major sync's checksum: CRC-16 (polynomial 0x2D) over bytes 4..hs-1,
 * xor the 16 bits at hs, equal to the 16 bits at hs + 2. */
static int major_sync_ok(const uint8_t *p, int n)
{
    int hs;
    uint16_t crc;

    if (n < 32)
        return 0;
    hs = major_sync_size(p);
    if (au_length(p) < hs + 4 || n < hs + 4)
        return 0;
    ff_thread_once(&crc_once, crc_init);
    crc = av_crc(crc_2d, 0, p + 4, hs - 4) ^ AV_RL16(p + hs);
    return crc == AV_RL16(p + hs + 2);
}

/* A unit of the stream: an AU of at least 8 bytes; a major-sync AU only with
 * a valid major sync. */
int ff_discrip_mlp_check(const uint8_t *p, int n)
{
    if (n < 8)
        return 0;
    return !major_sync(p, n) || major_sync_ok(p, n);
}

/* The core's cutter for AUs: an AU is as long as its length field says;
 * FFmpeg's parser instead drops every AU up to the next major sync when one
 * AU's parity or checksum fails. */
int ff_discrip_mlp_unit_size(const uint8_t *p, int avail)
{
    int len;

    if (avail < 8)
        return 0;
    len = au_length(p);
    if (len < 8)
        return -1;
    return len <= avail ? len : 0;
}

/* The next AU that starts with a major sync (its sync word 4 bytes in). */
int ff_discrip_mlp_resync(const uint8_t *p, int avail)
{
    for (int i = 0; i + 8 <= avail; i++)
        if (major_sync(p + i, avail - i))
            return i;
    return -1;
}

static int sync_mlp(const uint8_t *p, int n)
{
    return major_sync(p, n);
}

static int header_mlp(DRAudio *a, const uint8_t *p, int n, DRAudioHeader *h)
{
    MlpState *m = ff_discrip_audio_priv(a);
    int ratebits;

    if (!major_sync(p, n) || !major_sync_ok(p, n))
        return -1;
    m->truehd     = p[7] == 0xBA;
    ratebits      = m->truehd ? p[8] >> 4 : p[9] >> 4;
    m->rate       = mlp_samplerate(ratebits);
    m->samples    = 40 << (ratebits & 7);
    m->nsub       = p[0x14] >> 4;
    m->nsub_check = FFMIN(m->nsub, 3);
    m->ext        = m->truehd && (p[0x1d] & 1) ? p[0x1e] >> 4 : 0;
    h->rate       = m->rate;
    h->samples    = m->samples;
    return m->rate > 0 ? 0 : -1;
}

static void history_clear(MlpState *m)
{
    for (int i = 0; i < m->nb_hist; i++)
        av_buffer_unref(&m->hist[i]);
    m->nb_hist = 0;
}

static void close_mlp(void *priv)
{
    history_clear(priv);
}

/* The number of samples an AU that ends the stream decodes to: the AUs since
 * the last major sync, then this one, through FFmpeg's decoder; the samples
 * of this AU's frame. */
static int decode_count(DRAudio *a, MlpState *m, const uint8_t *p, int n, int *count)
{
    const uint8_t *hdr = m->nb_hist ? m->hist[0]->data : p;
    const AVCodec *dec;
    AVCodecContext *ctx = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int ret = AVERROR(ENOMEM);

    if (!pkt || !frame)
        goto end;
    if (!major_sync(hdr, m->nb_hist ? m->hist_size[0] : n)) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    dec = avcodec_find_decoder(hdr[7] == 0xBA ? AV_CODEC_ID_TRUEHD : AV_CODEC_ID_MLP);
    if (!dec || !(ctx = avcodec_alloc_context3(dec))) {
        ret = AVERROR(ENOSYS);
        goto end;
    }
    if ((ret = avcodec_open2(ctx, dec, NULL)) < 0)
        goto end;
    for (int i = 0; i <= m->nb_hist; i++) {
        const uint8_t *d = i < m->nb_hist ? m->hist[i]->data : p;
        int size         = i < m->nb_hist ? m->hist_size[i]   : n;

        av_packet_unref(pkt);
        if ((ret = av_new_packet(pkt, size)) < 0)
            goto end;
        memcpy(pkt->data, d, size);
        if ((ret = avcodec_send_packet(ctx, pkt)) < 0 ||
            (ret = avcodec_receive_frame(ctx, frame)) < 0)
            goto end;
        *count = frame->nb_samples;
        av_frame_unref(frame);
    }
    ret = *count > m->samples ? AVERROR_INVALIDDATA : 0;
end:
    if (ret < 0)
        av_log(ff_discrip_audio_log(a), AV_LOG_DEBUG, "Rip core: MLP / TrueHD: the AU that ends the stream "
               "could not be decoded (%s)\n", av_err2str(ret));
    avcodec_free_context(&ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    return ret;
}

/* Samples of an AU: the samples per AU of the stream, unless every one of the
 * first min(substreams, 3) substreams ends the stream; that AU's samples are
 * those it decodes to (else the shortened count its TrueHD substreams state,
 * else the samples per AU). */
static int au_samples(DRAudio *a, MlpState *m, const uint8_t *p, int n)
{
    uint16_t dir[16];
    int d, len, end, s, cand;

    if (n < 8 || m->nsub > 16 || !m->nsub)
        return m->samples;
    if (major_sync(p, n)) {
        if (n < 32)
            return m->samples;
        d = p[7] == 0xBA && (p[0x1d] & 1) ? ((p[0x1e] >> 3) & 0x1E) + 0x22 : 0x20;
    } else
        d = 4;
    len = au_length(p);
    for (int i = 0; i < m->nsub; i++) {
        if (d + 2 > n)
            return m->samples;
        dir[i] = AV_RB16(p + d);
        d += 2 + 2 * (dir[i] >> 15);
    }
    end = (dir[m->nsub - 1] * 2) & 0x1FFE;
    if (len < end + d)
        return m->samples;
    if (len > end + d && !m->ext &&
        (end + d + 2 > n || len - (end + d) - 2 != ((AV_RB16(p + d + end) * 2) & 0x1FFE)))
        return m->samples;

    /* Each of the first min(substreams, 3) substreams must end with the
     * stream-end marker; a TrueHD substream may state a shortened count.
     * Substream j ends at (dir[j] * 2) & 0x1FFE, minus 2 check bytes when
     * bit 13 is set; the marker is the 4 bytes before that end. */
    s = m->samples;
    for (int j = 0; j < m->nsub_check; j++) {
        int e  = (dir[j] * 2) & 0x1FFE;
        int pe = j ? (dir[j - 1] * 2) & 0x1FFE : 0;
        int w;

        if (j && e < pe)
            return m->samples;
        if (dir[j] & 0x2000) {
            if (!e)
                return m->samples;
            e -= 2;
        }
        if (e < 4)
            return m->samples;
        e -= 4;
        if (j && e < pe)
            return m->samples;
        if (d + e + 4 > n || AV_RL16(p + d + e) != 0x34D2)
            return m->samples;
        w = AV_RB16(p + d + e + 2);
        if (!m->truehd) {
            if (AV_RL16(p + d + e + 2) != 0x34D2)
                return m->samples;
        } else if (w & 0x2000) {
            int t = w & 0x1FFF;
            if (t > m->samples)
                return m->samples;
            if (s == m->samples)
                s = m->samples - t;
            else if (s != m->samples - t)
                s = 0;
        }
    }
    cand = s != m->samples ? s : 0;
    {
        int count;
        if (decode_count(a, m, p, n, &count) >= 0)
            return count;
    }
    return cand ? cand : m->samples;
}

static int duration_mlp(DRAudio *a, DRFrame *f)
{
    MlpState *m = ff_discrip_audio_priv(a);
    int samples;

    if (f->size < 8)
        return AVERROR_INVALIDDATA;
    if (major_sync(f->data, f->size))
        history_clear(m);
    samples = au_samples(a, m, f->data, f->size);
    f->dur  = (int64_t)((uint64_t)samples * DR_TICKS_PER_SECOND / (uint64_t)m->rate);
    if (m->nb_hist < HISTORY_MAX) {
        if (!(m->hist[m->nb_hist] = av_buffer_ref(f->buf)))
            return AVERROR(ENOMEM);
        m->hist_size[m->nb_hist++] = f->size;
    }
    return 0;
}

const DRAudioRules ff_discrip_audio_mlp = {
    .header      = header_mlp,
    .sync        = sync_mlp,
    .duration    = duration_mlp,
    .key_on_sync = 1,
    .priv_size   = sizeof(MlpState),
    .close       = close_mlp,
};
