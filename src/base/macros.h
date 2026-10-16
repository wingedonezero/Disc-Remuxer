/*
 * Small macros every module uses.
 */

#ifndef DISC_BASE_MACROS_H
#define DISC_BASE_MACROS_H

#define DR_MIN(a, b) ((a) < (b) ? (a) : (b))
#define DR_MAX(a, b) ((a) > (b) ? (a) : (b))
#define DR_ABS(a)    ((a) >= 0 ? (a) : -(a))
#define DR_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

#include <stdint.h>

/* a * b / c with the result rounded to the nearest (halves away from 0),
 * for values whose product fits 64 bits comfortably (times and rates). */
static inline int64_t dr_rescale(int64_t a, int64_t b, int64_t c)
{
    int64_t p = a * b;
    return p >= 0 ? (p + c / 2) / c : -((-p + c / 2) / c);
}

#endif
