/*
 * A bit reader for the rules' header walks: most-significant bit first,
 * never reading outside the buffer. Reads past the end return 0 bits and
 * keep advancing, so dr_bits_left() goes negative and the caller's overrun
 * checks fire exactly where they did with the window-based reader the code
 * was written against.
 */

#ifndef DISC_PARSE_BITS_H
#define DISC_PARSE_BITS_H

#include <limits.h>
#include <stdint.h>

typedef struct DrBits {
    const uint8_t *buf;
    int            size_bits;
    int            pos;
} DrBits;

/** 0, or < 0 when bytes cannot be addressed in bits. */
static inline int dr_bits_init8(DrBits *b, const uint8_t *buf, int bytes)
{
    if (bytes < 0 || bytes > INT_MAX / 8)
        return -1;
    b->buf       = buf;
    b->size_bits = bytes * 8;
    b->pos       = 0;
    return 0;
}

static inline unsigned dr_bits_get(DrBits *b, int n)
{
    unsigned v = 0;

    for (int i = 0; i < n; i++) {
        int p   = b->pos++;
        int bit = p < b->size_bits ? (b->buf[p >> 3] >> (7 - (p & 7))) & 1 : 0;
        v = v << 1 | (unsigned)bit;
    }
    return v;
}

static inline unsigned dr_bits_get1(DrBits *b)
{
    return dr_bits_get(b, 1);
}

static inline void dr_bits_skip(DrBits *b, int n)
{
    b->pos += n;
}

/** May be negative: the walk strayed past the data. */
static inline int dr_bits_left(const DrBits *b)
{
    return b->size_bits - b->pos;
}

/** Exp-Golomb ue(v) (H.264 / H.265 9.1): leading zeros, a 1, that many more
 *  bits. 32 zeros without the 1 (only overrun or broken data): a huge value,
 *  so every range check fires. */
static inline uint32_t dr_bits_ue(DrBits *b)
{
    int lz = 0;

    while (lz < 32 && !dr_bits_get1(b))
        lz++;
    if (lz == 32)
        return UINT32_MAX;
    return ((uint32_t)1 << lz) - 1 + dr_bits_get(b, lz);
}

/** ue(v) of a field whose legal values are below 31, read as the table-based
 *  reader the code was written against read it: it decides from the next 9
 *  bits, so a code of 5 or more leading zeros (values above 30: broken data)
 *  skips the code's 2 * zeros + 1 bits and reads as 31 (exactly value 31
 *  with a zero visible suffix) or 32. */
static inline int dr_bits_ue31(DrBits *b)
{
    unsigned idx = 0;
    int lz = 0;

    for (int i = 0; i < 9; i++) {
        int p   = b->pos + i;
        int bit = p < b->size_bits ? (b->buf[p >> 3] >> (7 - (p & 7))) & 1 : 0;
        idx = idx << 1 | (unsigned)bit;
    }
    for (unsigned m = 1u << 8; m && !(idx & m); m >>= 1)
        lz++;
    b->pos += 2 * lz + 1;
    if (lz <= 4)
        return (1 << lz) - 1 + (int)((idx >> (8 - 2 * lz)) & ((1u << lz) - 1));
    return idx == 8 ? 31 : 32;
}

/** Exp-Golomb se(v) (9.1.1): 0, 1, -1, 2, -2, ... */
static inline int32_t dr_bits_se(DrBits *b)
{
    uint32_t v = dr_bits_ue(b);

    return (v & 1) ? (int32_t)((v >> 1) + 1) : -(int32_t)(v >> 1);
}

#endif /* DISC_PARSE_BITS_H */
