/*
 * Reducing a rational (a frame rate) to numerator / denominator within a
 * bound: exact when the reduced pair fits, else the best approximation by
 * continued fractions — the same procedure the processing library uses, so
 * the rates the rules report stay value-identical across the migration.
 *
 * The procedure follows FFmpeg's libavutil/rational.c (av_reduce),
 * Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
 */

#ifndef DISC_PARSE_RATIONAL_H
#define DISC_PARSE_RATIONAL_H

#include <stdint.h>

static inline int64_t dr_gcd64(int64_t a, int64_t b)
{
    while (b) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/** num / den reduced into *dst_num / *dst_den, both <= max; returns 1 when
 *  exact. */
static inline int dr_reduce(int *dst_num, int *dst_den, int64_t num, int64_t den, int64_t max)
{
    int64_t a0n = 0, a0d = 1, a1n = 1, a1d = 0;
    int sign = (num < 0) ^ (den < 0);
    int64_t gcd = dr_gcd64(num < 0 ? -num : num, den < 0 ? -den : den);

    if (gcd) {
        num = (num < 0 ? -num : num) / gcd;
        den = (den < 0 ? -den : den) / gcd;
    }
    if (num <= max && den <= max) {
        a1n = num;
        a1d = den;
        den = 0;
    }

    while (den) {
        uint64_t x       = num / den;
        int64_t next_den = num - den * x;
        int64_t a2n      = x * a1n + a0n;
        int64_t a2d      = x * a1d + a0d;

        if (a2n > max || a2d > max) {
            if (a1n)
                x = (max - a0n) / a1n;
            if (a1d && (max - a0d) / a1d < (int64_t)x)
                x = (max - a0d) / a1d;
            if (den * (2 * (int64_t)x * a1d + a0d) > num * a1d) {
                a1n = x * a1n + a0n;
                a1d = x * a1d + a0d;
            }
            break;
        }

        a0n = a1n;
        a0d = a1d;
        a1n = a2n;
        a1d = a2d;
        num = den;
        den = next_den;
    }

    *dst_num = (int)(sign ? -a1n : a1n);
    *dst_den = (int)a1d;
    return den == 0;
}

#endif /* DISC_PARSE_RATIONAL_H */
