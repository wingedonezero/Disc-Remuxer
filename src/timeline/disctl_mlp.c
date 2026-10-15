/*
 * The title timeline: the rules for MLP and Dolby TrueHD access units.
 */

#include <string.h>


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

    int has_ctr;          /* ctr is known (a major sync's restart header was read) */
    int ctr;              /* sample number of the next AU's first sample, modulo 65536 */

    DrBuf *hist[HISTORY_MAX];   /* AUs since the last major sync */
    int          hist_size[HISTORY_MAX];
    int          nb_hist;
} MlpState;

/* CRC-16, polynomial 0x002D, most significant bit first (MLP's major sync
 * checksum). */
static uint16_t mlp_crc(uint16_t crc, const uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = crc & 0x8000 ? (crc << 1) ^ 0x002D : crc << 1;
    }
    return crc;
}

/* The sample rate a major sync's rate bits state (0xF: none). */
static int mlp_samplerate(int in)
{
    if (in == 0xF)
        return 0;
    return (in & 8 ? 44100 : 48000) << (in & 7);
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
    crc = mlp_crc(0, p + 4, hs - 4) ^ dr_rb16(p + hs);
    return crc == dr_rb16(p + hs + 2);
}

/* A unit of the stream: an AU of at least 8 bytes; a major-sync AU only with
 * a valid major sync. */
int ff_disctl_mlp_check(const uint8_t *p, int n)
{
    if (n < 8)
        return 0;
    return !major_sync(p, n) || major_sync_ok(p, n);
}

/* An output AU: as long as its length field, a major sync with a valid
 * checksum. */
int ff_disctl_mlp_verify(const uint8_t *p, int n)
{
    if (n < 8 || au_length(p) != n)
        return DR_UNIT_BAD;
    return ff_disctl_mlp_check(p, n) ? DR_UNIT_OK : DR_UNIT_CRC;
}

static int sync_mlp(const uint8_t *p, int n)
{
    return major_sync(p, n);
}

static int output_timing(const MlpState *m, const uint8_t *p, int n);

static int header_mlp(DRAudio *a, const DRFrame *f, DRAudioHeader *h)
{
    MlpState *m = ff_disctl_audio_priv(a);
    const uint8_t *p = f->data;
    int n = f->size;
    int ratebits;

    if (!major_sync(p, n) || !major_sync_ok(p, n))
        return -1;
    m->truehd     = p[7] == 0xBA;
    ratebits      = m->truehd ? p[8] >> 4 : p[9] >> 4;
    m->rate       = mlp_samplerate(ratebits);
    m->samples    = 40 << (ratebits & 7);
    m->nsub       = p[0x14] >> 4;
    m->nsub_check = DR_MIN(m->nsub, 3);
    m->ext        = m->truehd && (p[0x1d] & 1) ? p[0x1e] >> 4 : 0;
    h->rate       = m->rate;
    h->samples    = m->samples;
    /* The AUs before this first major sync (a segment that starts between
     * two major syncs: they waited for the stream values) take their sample
     * numbers counted back from its restart header: each holds the stream's
     * samples per AU, and they lead up to it without a gap. */
    if (m->rate > 0 && ff_disctl_audio_waiting(a)) {
        int t = output_timing(m, p, n);
        m->has_ctr = t >= 0;
        m->ctr     = (t - ff_disctl_audio_waiting(a) * m->samples) & 0xFFFF;
    }
    return m->rate > 0 ? 0 : -1;
}

static void history_clear(MlpState *m)
{
    for (int i = 0; i < m->nb_hist; i++)
        dr_buf_unref(&m->hist[i]);
    m->nb_hist = 0;
}

static void close_mlp(void *priv)
{
    history_clear(priv);
}

static int decode_count(DRAudio *a, MlpState *m, const uint8_t *p, int n, int *count)
{
    const uint8_t *aus[HISTORY_MAX + 1];
    int sizes[HISTORY_MAX + 1];
    const DRTimelineHost *host = ff_disctl_host();
    int ret;

    if (!major_sync(m->nb_hist ? dr_buf_data(m->hist[0]) : p, m->nb_hist ? m->hist_size[0] : n) ||
        !host || !host->mlp_last_au_samples) {
        ret = DR_ERR_INVALID;
        goto end;
    }
    for (int i = 0; i < m->nb_hist; i++) {
        aus[i]   = dr_buf_data(m->hist[i]);
        sizes[i] = m->hist_size[i];
    }
    aus[m->nb_hist]   = p;
    sizes[m->nb_hist] = n;
    ret = host->mlp_last_au_samples(ff_disctl_audio_log(a), aus, sizes, m->nb_hist + 1, count);
    if (ret >= 0)
        ret = *count > m->samples ? DR_ERR_INVALID : 0;
end:
    if (ret < 0)
        dr_vlog(ff_disctl_audio_log(a), DR_VLOG_DEBUG, "Title timeline: MLP / TrueHD: the AU that ends the stream "
               "could not be decoded (%s)\n", dr_err_text(ret));
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
        dir[i] = dr_rb16(p + d);
        d += 2 + 2 * (dir[i] >> 15);
    }
    end = (dir[m->nsub - 1] * 2) & 0x1FFE;
    if (len < end + d)
        return m->samples;
    if (len > end + d && !m->ext &&
        (end + d + 2 > n || len - (end + d) - 2 != ((dr_rb16(p + d + end) * 2) & 0x1FFE)))
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
        if (d + e + 4 > n || dr_rl16(p + d + e) != 0x34D2)
            return m->samples;
        w = dr_rb16(p + d + e + 2);
        if (!m->truehd) {
            if (dr_rl16(p + d + e + 2) != 0x34D2)
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

/* The output timing of a major-sync AU: the sample number of its first
 * sample, modulo 65536, from substream 0's restart header (Dolby TrueHD
 * bitstream description, restart_header: after the block's 2 flag bits
 * (parameters, restart header present), the 13-bit restart sync 0x18F5
 * (0x31EA / 0x31EB without its last bit) and 1 bit, the 16-bit
 * output_timing). -1 when it cannot be read. */
static int output_timing(const MlpState *m, const uint8_t *p, int n)
{
    int d, w;

    if (n < 32 || !m->nsub || m->nsub > 16)
        return -1;
    d = p[7] == 0xBA && (p[0x1d] & 1) ? ((p[0x1e] >> 3) & 0x1E) + 0x22 : 0x20;
    for (int i = 0; i < m->nsub; i++) {
        if (d + 2 > n)
            return -1;
        d += 2 + 2 * (dr_rb16(p + d) >> 15);
    }
    if (d + 4 > n)
        return -1;
    w = dr_rb16(p + d);
    if (w >> 14 != 3 || ((w >> 1) & 0x1FFF) != 0x18F5)
        return -1;
    return dr_rb16(p + d + 2);
}

static int duration_mlp(DRAudio *a, DRFrame *f)
{
    MlpState *m = ff_disctl_audio_priv(a);
    int samples;

    if (f->size < 8)
        return DR_ERR_INVALID;
    if (major_sync(f->data, f->size)) {
        int t = output_timing(m, f->data, f->size);
        history_clear(m);
        m->has_ctr = t >= 0;
        m->ctr     = t;
    }
    if (m->has_ctr) {
        /* minor syncs: one AU's samples on from the last restart */
        f->ctr    = m->ctr;
        f->flags |= DR_F_CTR;
        m->ctr    = (m->ctr + m->samples) & 0xFFFF;
    }
    samples = au_samples(a, m, f->data, f->size);
    f->dur  = (int64_t)((uint64_t)samples * DR_TICKS_PER_SECOND / (uint64_t)m->rate);
    if (m->nb_hist < HISTORY_MAX) {
        if (!(m->hist[m->nb_hist] = dr_buf_ref(f->buf)))
            return DR_ERR(ENOMEM);
        m->hist_size[m->nb_hist++] = f->size;
    }
    return 0;
}

const DRAudioRules ff_disctl_audio_mlp = {
    .header      = header_mlp,
    .sync        = sync_mlp,
    .duration    = duration_mlp,
    .key_on_sync = 1,
    .priv_size   = sizeof(MlpState),
    .close       = close_mlp,
};
