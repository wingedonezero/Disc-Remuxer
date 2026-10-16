/*
 * MPEG audio (ISO/IEC 11172-3 / 13818-3): the disc reader's frame rules.
 * Moved out of our FFmpeg's mpegaudio_parser.c disc mode; the header decode
 * and the Layer II allocation tables are our own copies of the
 * specification's values.
 *
 * The Layer II allocation tables are laid out as in FFmpeg's
 * libavcodec/mpegaudiodata.c, copyright (c) 2002 Fabrice Bellard;
 * LGPL-2.1-or-later.
 */

#include <stdint.h>

#include "base/bytes.h"
#include "base/err.h"

#include "parse/bits.h"
#include "parse/rules.h"

#define MODE_STEREO  0
#define MODE_JSTEREO 1
#define MODE_DUAL    2
#define MODE_MONO    3

static const int sblimit_table[5] = { 27, 30, 8, 12, 30 };

static const unsigned char alloc_table_1[] = {
 4,  0,  2,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16,
 4,  0,  2,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16,
 4,  0,  2,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 16,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 16,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 16,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 16,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 16,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 16,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 16,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 3,  0,  1,  2,  3,  4,  5, 16,
 2,  0,  1, 16,
 2,  0,  1, 16,
 2,  0,  1, 16,
 2,  0,  1, 16,
 2,  0,  1, 16,
 2,  0,  1, 16,
 2,  0,  1, 16,
};

static const unsigned char alloc_table_3[] = {
 4,  0,  1,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
 4,  0,  1,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
};

static const unsigned char alloc_table_4[] = {
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
 4,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 3,  0,  1,  3,  4,  5,  6,  7,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
 2,  0,  1,  3,
};

static const unsigned char *const alloc_tables[5] =
{ alloc_table_1, alloc_table_1, alloc_table_3, alloc_table_3, alloc_table_4, };

/* ---- The disc reader's MPEG audio frame rules ----
 * A frame starts with a header FFmpeg's header check accepts (ISO/IEC 11172-3
 * 2.4.1.3; not the reserved version); on a DVD-Video disc only Layer II at
 * 48 kHz that is not dual channel. Its size is the header's; a free-format
 * frame (bitrate index 0) is as long as the distance L (16 to 6143 bytes) at
 * which the headers at L, 2L, 3L, 4L and 5L all match it (sync, ID, layer,
 * sampling frequency), found once and kept. Where no frame starts, the next
 * one is a header that passes the same checks and, when it has a CRC, whose
 * CRC is right (CRC-16, polynomial 0x8005 from 0xFFFF, over header bytes 2
 * and 3 and the bits of the side information the CRC covers: Layer I
 * 128 + 4 x bound x (channels - 1); Layer II the bit allocation and the
 * scale factor selection; Layer III 17 / 32 bytes (MPEG-1) or 9 / 17 bytes
 * (MPEG-2) for one / two channels); with more than 0x10000 bytes and none,
 * the kept bytes are given up. */

#define DISC_FREE_MIN   16
#define DISC_FREE_MAX   0x1800
#define DISC_GIVE_UP    0x10000

typedef struct DiscMpa {
    int layer, lsf, channels, sample_rate, bit_rate, mode, mode_ext, crc, size;
} DiscMpa;

/* The header fields and the frame size (ISO/IEC 11172-3 2.4.1.3 and
 * 2.4.2.3; the free-format size, bitrate index 0, stays 0). 1 a header the
 * rules accept, 0 not. */
static int mpa_header(const uint8_t *b, DiscMpa *h)
{
    static const uint16_t bitrates[2][3][15] = {
        { { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
          { 0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384 },
          { 0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320 } },
        { { 0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256 },
          { 0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160 },
          { 0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160 } },
    };
    static const uint16_t freqs[3] = { 44100, 48000, 32000 };
    uint32_t head = dr_rb32(b);
    int mpeg25, sr_index, br_index, padding, kbit;

    if ((head & 0xffe00000) != 0xffe00000 ||    /* sync */
        (head & (3 << 19)) == 1 << 19 ||        /* reserved version */
        (head & (3 << 17)) == 0 ||              /* reserved layer */
        (head & (0xf << 12)) == 0xf << 12 ||    /* reserved bit rate */
        (head & (3 << 10)) == 3 << 10)          /* reserved frequency */
        return 0;
    if (head & (1 << 20)) {
        h->lsf = (head & (1 << 19)) ? 0 : 1;
        mpeg25 = 0;
    } else {
        h->lsf = 1;
        mpeg25 = 1;
    }
    h->layer       = 4 - ((head >> 17) & 3);
    sr_index       = (head >> 10) & 3;
    h->sample_rate = freqs[sr_index] >> (h->lsf + mpeg25);
    h->crc         = ((head >> 16) & 1) ^ 1;
    br_index       = (head >> 12) & 0xf;
    padding        = (head >> 9) & 1;
    h->mode        = (head >> 6) & 3;
    h->mode_ext    = (head >> 4) & 3;
    h->channels    = h->mode == MODE_MONO ? 1 : 2;
    h->bit_rate    = 0;
    h->size        = 0;
    if (br_index) {
        kbit        = bitrates[h->lsf][h->layer - 1][br_index];
        h->bit_rate = kbit * 1000;
        switch (h->layer) {
        case 1:  h->size = (kbit * 12000 / h->sample_rate + padding) * 4;             break;
        case 2:  h->size =  kbit * 144000 / h->sample_rate + padding;                 break;
        default: h->size =  kbit * 144000 / (h->sample_rate << h->lsf) + padding;     break;
        }
    }
    return 1;
}

/* The frame header at b: 1 a header the rules accept, 0 not. */
static int disc_header(const DrMpaRuleCtx *c, const uint8_t *b, DiscMpa *h)
{
    if (!mpa_header(b, h))
        return 0;
    if (c->dvd && (h->layer != 2 || h->sample_rate != 48000 || h->mode == MODE_DUAL))
        return 0;
    return 1;
}

static int disc_same(uint32_t a, uint32_t b)
{
    return !((a ^ b) & 0xfffe0c00);     /* sync, ID, layer, sampling frequency */
}

static int disc_sync(void *ctx, const uint8_t *b, int len)
{
    DiscMpa h;

    if (len < 4)
        return 2;
    return disc_header(ctx, b, &h);
}

static int disc_size(void *ctx, const uint8_t *b, int len)
{
    DrMpaRuleCtx *c = ctx;
    DiscMpa h;

    if (len < 4)
        return -1;
    if (!disc_header(c, b, &h))
        return -2;
    if (h.bit_rate || h.size)
        return h.size;
    if (c->free_size)
        return c->free_size;
    for (int l = DISC_FREE_MIN; l < DISC_FREE_MAX && 5 * l + 4 <= len; l++) {
        uint32_t head = dr_rb32(b);
        int k;
        for (k = 1; k <= 5 && disc_same(head, dr_rb32(b + k * l)); k++)
            ;
        if (k > 5)
            return c->free_size = l;
    }
    return -1;
}

static int disc_check(void *ctx, const uint8_t *b, int len)
{
    return 1;
}

/* The bits of side information a frame's CRC covers (0: none known). */
static int disc_crc_bits(const DiscMpa *h, const uint8_t *b, int len)
{
    int bound = h->mode == MODE_JSTEREO ? 4 + 4 * h->mode_ext : 32;
    DrBits gb;
    int table, sblimit, bits = 0, ch_rate;
    const unsigned char *alloc;
    uint8_t al[2][32] = { { 0 } };

    if (h->layer == 1)
        return 128 + 4 * bound * (h->channels - 1);
    if (h->layer == 3)
        return h->lsf ? (h->channels == 1 ? 72 : 136) : (h->channels == 1 ? 136 : 256);
    /* Layer II: the allocation table as the bitrate per channel selects it */
    if (h->lsf) {
        table = 4;
    } else if (!h->bit_rate) {
        table = h->sample_rate != 48000;
    } else {
        ch_rate = h->channels == 2 ? h->bit_rate / 2 : h->bit_rate;
        if (ch_rate > 192000)
            return 0;
        table = ch_rate <= 48000 ? 2 + (h->sample_rate == 32000) : ch_rate > 80000 ? h->sample_rate != 48000 : 0;
    }
    sblimit = sblimit_table[table];
    alloc   = alloc_tables[table];
    if (dr_bits_init8(&gb, b + 6, len - 6) < 0)
        return 0;
    for (int sb = 0, j = 0; sb < sblimit; sb++) {
        int nbal = alloc[j];
        for (int ch = 0; ch < (sb < bound ? h->channels : 1); ch++) {
            al[ch][sb] = dr_bits_get(&gb, nbal);
            bits += nbal;
        }
        if (sb >= bound)
            al[1][sb] = al[0][sb];
        j += 1 << nbal;
    }
    if (dr_bits_left(&gb) < 0)
        return 0;
    for (int sb = 0; sb < sblimit; sb++)
        for (int ch = 0; ch < h->channels; ch++)
            bits += al[ch][sb] ? 2 : 0;
    return bits;
}

static unsigned disc_crc_add(unsigned crc, unsigned bit)
{
    unsigned top = crc >> 15 & 1;
    crc = crc << 1 & 0xffff;
    return top != bit ? crc ^ 0x8005 : crc;
}

/* The frame's CRC (after its header) against the one it carries. */
static int disc_crc_ok(const DiscMpa *h, const uint8_t *b, int len)
{
    int bits = disc_crc_bits(h, b, len);
    unsigned crc = 0xffff;

    if (!bits || 6 + (bits + 7) / 8 > len)
        return 0;
    for (int i = 0; i < 16; i++)
        crc = disc_crc_add(crc, b[2 + i / 8] >> (7 - i % 8) & 1);
    for (int i = 0; i < bits; i++)
        crc = disc_crc_add(crc, b[6 + i / 8] >> (7 - i % 8) & 1);
    return crc == dr_rb16(b + 4);
}

static int disc_resync(void *ctx, const uint8_t *b, int len)
{
    if (len < 4)
        return -2;
    for (int pos = 0; pos < len - 4; pos++) {
        DiscMpa h;
        if (b[pos] != 0xff || b[pos + 1] < 0xe0 || !disc_header(ctx, b + pos, &h))
            continue;
        if (!h.crc)
            return pos;
        if (len - pos > 6 && disc_crc_ok(&h, b + pos, len - pos))
            return pos;
    }
    return len > DISC_GIVE_UP ? -1 : -2;
}

const DrFrameRule dr_mpa_frames = { disc_sync, disc_size, disc_check, disc_resync, DISC_GIVE_UP };

/* The timeline's header read: the fields it uses of a frame header, with
 * the acceptance of the rules above. A frame lasts 384 samples (Layer I),
 * 1152 (Layer II, or Layer III of MPEG-1), 576 (Layer III of MPEG-2 / 2.5);
 * a free-format frame's size stays 0. */
int dr_mpa_header(const uint8_t *b, int len, DrMpaHeader *h)
{
    DiscMpa m;

    h->rate = h->samples = h->frame_size = 0;
    if (len < 4 || !mpa_header(b, &m))
        return DR_ERR_INVALID;
    h->rate       = m.sample_rate;
    h->samples    = m.layer == 1 ? 384 : m.layer == 2 || !m.lsf ? 1152 : 576;
    h->frame_size = m.size;
    return 0;
}
