/*
 * A track's content as one checksum (content_sum.h).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/codec_id.h>
#include <libavutil/mem.h>
#include <libavutil/murmur3.h>

#include "analyze/content_sum.h"
#include "base/err.h"

#define AC3_HEAD 16          /* the frame's first bytes, summed with the changed fields cleared */
#define AC3_MAX  3840        /* the longest AC-3 frame (ETSI TS 102 366 table 4.13: 1920 words) */
#define SETS_MAX 4096        /* distinct parameter sets of one stream; more: the sum cannot be compared */

/* A distinct parameter set: its bytes' checksum and its length. */
typedef struct ParamSet {
    uint8_t sum[16];
    int64_t len;
} ParamSet;

struct DrContentSum {
    struct AVMurMur3 *h;
    int      kind;
    int64_t  bytes, units;
    int      odd, done;
    char     digest[33];     /* once done */
    /* NAL units out of a byte stream */
    int      in_nal;         /* a start code was met: unit bytes follow */
    int      zeros;          /* zero bytes held back: a unit's own, or the next start code's */
    int64_t  nal_len;
    /* the stream's parameter sets, each distinct one once */
    struct AVMurMur3 *u;     /* the one in hand */
    int      in_set;         /* the unit in hand is a parameter set */
    ParamSet *sets;
    int      nb_sets;
    /* AC-3 frames: one being gathered */
    uint8_t  frame[AC3_MAX];
    int      have, need;     /* bytes held, the frame's size (0: not known yet) */
};

int dr_content_kind(int codec, int dialnorm_removed)
{
    switch (codec) {
    case AV_CODEC_ID_H264:
        return DR_CONTENT_NAL;
    case AV_CODEC_ID_AC3:
        return dialnorm_removed ? DR_CONTENT_AC3_NO_DIALNORM : DR_CONTENT_BYTES;
    case AV_CODEC_ID_TRUEHD:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_FLAC:
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        return DR_CONTENT_BYTES;
    default:
        return DR_CONTENT_NONE;
    }
}

const char *dr_content_kind_name(int kind)
{
    return kind == DR_CONTENT_BYTES ? "its bytes" : kind == DR_CONTENT_NAL ? "its NAL units" :
           kind == DR_CONTENT_AC3_NO_DIALNORM ? "its frames without the dialogue normalization fields" : "none";
}

int dr_content_sum_open(DrContentSum **ps, int kind)
{
    DrContentSum *s;

    *ps = NULL;
    if (kind != DR_CONTENT_BYTES && kind != DR_CONTENT_NAL && kind != DR_CONTENT_AC3_NO_DIALNORM)
        return DR_ERR(EINVAL);
    if (!(s = av_mallocz(sizeof(*s))) || !(s->h = av_murmur3_alloc()) || !(s->u = av_murmur3_alloc())) {
        if (s)
            av_free(s->h);
        av_free(s);
        return DR_ERR(ENOMEM);
    }
    av_murmur3_init(s->h);
    s->kind = kind;
    *ps = s;
    return 0;
}

static void sum(DrContentSum *s, const uint8_t *p, size_t n)
{
    if (n) {
        av_murmur3_update(s->h, p, n);
        s->bytes += n;
    }
}

/* ---- NAL units ---- */

/* A parameter set (ITU-T H.264 table 7-1: sequence parameter set, picture
 * parameter set, sequence parameter set extension, subset sequence parameter
 * set). A stream repeats them, and a Matroska muxer may repeat them once
 * more in front of a key picture that comes without them: how often and
 * where one stands is not the track's content, which sets there are is. */
static int param_set(uint8_t first)
{
    int type = first & 0x1f;

    return type == 7 || type == 8 || type == 13 || type == 15;
}

static int set_cmp(const void *a, const void *b)
{
    const ParamSet *x = a, *y = b;
    int c = memcmp(x->sum, y->sum, sizeof(x->sum));

    return c ? c : (x->len > y->len) - (x->len < y->len);
}

/* The parameter set in hand ends: kept when the stream had no such one yet. */
static void set_end(DrContentSum *s)
{
    ParamSet ps = { .len = s->nal_len };

    av_murmur3_final(s->u, ps.sum);
    for (int i = 0; i < s->nb_sets; i++)
        if (!set_cmp(&s->sets[i], &ps))
            return;
    if (s->nb_sets >= SETS_MAX || av_reallocp_array(&s->sets, s->nb_sets + 1, sizeof(*s->sets)) < 0) {
        if (s->nb_sets < SETS_MAX)
            s->nb_sets = 0;                     /* the array is gone */
        s->odd = 1;
        return;
    }
    s->sets[s->nb_sets++] = ps;
}

/* The unit in hand ends: its length follows its bytes into the sum, so two
 * streams with the same bytes cut into other units differ. */
static void nal_end(DrContentSum *s)
{
    uint8_t len[8];

    if (s->in_nal && s->nal_len > 0) {
        if (s->in_set) {
            set_end(s);
        } else {
            for (int i = 0; i < 8; i++)
                len[i] = (uint8_t)(s->nal_len >> (8 * i));
            av_murmur3_update(s->h, len, 8);
            s->units++;
        }
    }
    s->nal_len = 0;
    s->in_set  = 0;
}

static void nal_bytes(DrContentSum *s, const uint8_t *p, size_t n)
{
    if (!n)
        return;
    if (!s->nal_len && (s->in_set = param_set(p[0])))
        av_murmur3_init(s->u);                  /* the unit's first byte says what it is */
    if (s->in_set)
        av_murmur3_update(s->u, p, n);
    else
        sum(s, p, n);
    s->nal_len += n;
}

/* The end of the units: the distinct parameter sets follow them into the
 * sum, in the order of their checksums (the same whatever order and however
 * often the stream had them). */
static void sets_end(DrContentSum *s)
{
    uint8_t len[8];

    if (s->nb_sets > 1)
        qsort(s->sets, s->nb_sets, sizeof(*s->sets), set_cmp);
    for (int k = 0; k < s->nb_sets; k++) {
        for (int i = 0; i < 8; i++)
            len[i] = (uint8_t)(s->sets[k].len >> (8 * i));
        av_murmur3_update(s->h, s->sets[k].sum, sizeof(s->sets[k].sum));
        av_murmur3_update(s->h, len, 8);
        s->bytes += s->sets[k].len;
    }
}

/* A byte stream (ITU-T H.264 annex B): a unit lies between two start codes
 * (00 00 01); the zero bytes before a start code are not the unit's
 * (zero_byte, trailing_zero_8bits). Zero bytes are held back until what
 * follows them says whose they are. */
static void nal_stream(DrContentSum *s, const uint8_t *p, size_t n)
{
    static const uint8_t zero[4] = { 0 };
    size_t i = 0;

    while (i < n) {
        if (p[i] == 0) {
            s->zeros++;
            i++;
            continue;
        }
        if (p[i] == 1 && s->zeros >= 2) {       /* a start code: the unit before it ends */
            nal_end(s);
            s->in_nal = 1;
            s->zeros  = 0;
            i++;
            continue;
        }
        /* a unit's byte; the zero bytes held back were its own */
        if (!s->in_nal) {
            s->odd   = 1;                       /* bytes before the first start code */
            s->zeros = 0;
            i++;
            continue;
        }
        while (s->zeros > 0) {
            int k = s->zeros > 4 ? 4 : s->zeros;
            nal_bytes(s, zero, k);
            s->zeros -= k;
        }
        {
            /* up to the next zero byte in one piece */
            const uint8_t *z = memchr(p + i, 0, n - i);
            size_t len = z ? (size_t)(z - (p + i)) : n - i;
            nal_bytes(s, p + i, len);
            i += len;
        }
    }
}

void dr_content_sum_nal(DrContentSum *s, const uint8_t *nal, size_t size)
{
    if (!s || s->done || s->kind != DR_CONTENT_NAL || !size)
        return;
    s->in_nal = 1;
    nal_bytes(s, nal, size);
    nal_end(s);
}

/* ---- AC-3 frames without their dialogue normalization fields ---- */

static unsigned bits_at(const uint8_t *p, int at, int n)
{
    unsigned v = 0;

    for (int i = 0; i < n; i++, at++)
        v = v << 1 | (p[at >> 3] >> (7 - (at & 7)) & 1);
    return v;
}

static void clear_bits(uint8_t *p, int at, int n)
{
    for (int i = 0; i < n; i++, at++)
        p[at >> 3] &= (uint8_t)~(0x80 >> (at & 7));
}

/* The size in bytes of the AC-3 frame whose first 6 bytes are at p (ETSI
 * TS 102 366 4.4.1, table 4.13), 0 when they start no AC-3 frame (also an
 * Enhanced AC-3 one: its fields lie elsewhere). */
static int ac3_size(const uint8_t *p)
{
    static const uint16_t rate[19] = { 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
                                       576, 640 };
    int fscod = p[4] >> 6, code = p[4] & 0x3f, bsid = p[5] >> 3;

    if (p[0] != 0x0b || p[1] != 0x77 || fscod == 3 || code >= 38 || bsid > 10)
        return 0;
    if (fscod == 0)
        return 4 * rate[code >> 1];
    if (fscod == 1)
        return 2 * (320 * rate[code >> 1] / 147 + (code & 1));
    return 6 * rate[code >> 1];
}

/* A whole frame: its head with crc1 and the dialnorm values cleared, its
 * middle, its last 3 bytes with the bit next to crc2 and crc2 cleared. */
static void ac3_frame(DrContentSum *s, const uint8_t *f, int size)
{
    uint8_t head[AC3_HEAD], tail[3];
    int acmod, at = 40 + 5 + 3;             /* syncinfo, bsid, bsmod */

    memcpy(head, f, AC3_HEAD);
    head[2] = head[3] = 0;                  /* crc1 */
    acmod = bits_at(f, at, 3);
    at += 3;
    if ((acmod & 1) && acmod != 1)
        at += 2;                            /* cmixlev */
    if (acmod & 4)
        at += 2;                            /* surmixlev */
    if (acmod == 2)
        at += 2;                            /* dsurmod */
    at += 1;                                /* lfeon */
    clear_bits(head, at, 5);                /* dialnorm */
    at += 5;
    if (acmod == 0) {                       /* 1+1: the second channel's value */
        if (bits_at(f, at++, 1))            /* compre */
            at += 8;
        if (bits_at(f, at++, 1))            /* langcode */
            at += 8;
        if (bits_at(f, at++, 1))            /* audprodie */
            at += 7;
        if (at + 5 <= AC3_HEAD * 8)
            clear_bits(head, at, 5);        /* dialnorm2 */
        else
            s->odd = 1;
    }
    tail[0] = f[size - 3] & 0xfe;           /* the bit reserved next to crc2 */
    tail[1] = tail[2] = 0;                  /* crc2 */
    sum(s, head, AC3_HEAD);
    sum(s, f + AC3_HEAD, size - AC3_HEAD - 3);
    sum(s, tail, 3);
    s->units++;
}

static void ac3_stream(DrContentSum *s, const uint8_t *p, size_t n)
{
    while (n && !s->odd) {
        int take;

        if (!s->need && s->have < 6) {          /* the frame's first bytes say its size */
            take = 6 - s->have < (int)n ? 6 - s->have : (int)n;
            memcpy(s->frame + s->have, p, take);
            s->have += take;
            p += take;
            n -= take;
            if (s->have < 6)
                return;
            if ((s->need = ac3_size(s->frame)) < AC3_HEAD + 3 || s->need > AC3_MAX) {
                s->odd = 1;                     /* no AC-3 frame starts here */
                return;
            }
        }
        take = s->need - s->have < (int)n ? s->need - s->have : (int)n;
        memcpy(s->frame + s->have, p, take);
        s->have += take;
        p += take;
        n -= take;
        if (s->have == s->need) {
            ac3_frame(s, s->frame, s->need);
            s->have = s->need = 0;
        }
    }
}

void dr_content_sum_data(DrContentSum *s, const uint8_t *data, size_t size)
{
    if (!s || s->done || !size)
        return;
    if (s->kind == DR_CONTENT_NAL)
        nal_stream(s, data, size);
    else if (s->kind == DR_CONTENT_AC3_NO_DIALNORM)
        ac3_stream(s, data, size);
    else {
        sum(s, data, size);
        s->units++;
    }
}

void dr_content_sum_finish(DrContentSum *s, DrContentResult *r)
{
    uint8_t d[16];

    memset(r, 0, sizeof(*r));
    if (!s)
        return;
    if (!s->done) {
        if (s->kind == DR_CONTENT_NAL) {
            nal_end(s);                         /* the last unit (zero bytes after it are not its own) */
            sets_end(s);
        }
        if (s->kind == DR_CONTENT_AC3_NO_DIALNORM && s->have)
            s->odd = 1;                         /* the stream ends inside a frame */
        av_murmur3_final(s->h, d);
        for (int i = 0; i < 16; i++)
            snprintf(s->digest + 2 * i, 3, "%02x", d[i]);
        s->done = 1;
    }
    memcpy(r->digest, s->digest, sizeof(r->digest));
    r->bytes = s->bytes;
    r->units = s->units;
    r->odd   = s->odd;
}

void dr_content_sum_close(DrContentSum **ps)
{
    if (*ps) {
        av_free((*ps)->h);
        av_free((*ps)->u);
        av_free((*ps)->sets);
    }
    av_freep(ps);
}
