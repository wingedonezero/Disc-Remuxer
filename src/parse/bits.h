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

#endif /* DISC_PARSE_BITS_H */
