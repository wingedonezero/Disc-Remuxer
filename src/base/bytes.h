/*
 * Byte reads: little- and big-endian words out of byte buffers (the disc
 * structures are little-endian on HD DVD's and UDF's tables, big-endian on
 * DVD's and Blu-ray's).
 */

#ifndef DISC_BASE_BYTES_H
#define DISC_BASE_BYTES_H

#include <stdint.h>

static inline uint16_t dr_rl16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t dr_rl32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t dr_rl64(const uint8_t *p)
{
    return dr_rl32(p) | (uint64_t)dr_rl32(p + 4) << 32;
}

static inline uint16_t dr_rb16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t dr_rb24(const uint8_t *p)
{
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | (uint32_t)p[2];
}

static inline uint32_t dr_rb32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline uint64_t dr_rb64(const uint8_t *p)
{
    return (uint64_t)dr_rb32(p) << 32 | dr_rb32(p + 4);
}

static inline void dr_wb16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v;
}

static inline void dr_wb32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static inline void dr_wl16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static inline void dr_wl32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline void dr_wl64(uint8_t *p, uint64_t v)
{
    dr_wl32(p, (uint32_t)v);
    dr_wl32(p + 4, (uint32_t)(v >> 32));
}

/* A four-character big-endian tag ('H','D','M','V' = the bytes in file
 * order read big-endian). */
#define DR_MKBETAG(a, b, c, d) ((uint32_t)(d) | ((uint32_t)(c) << 8) | ((uint32_t)(b) << 16) | \
                                ((uint32_t)(a) << 24))

/* The IEEE-754 single the 32 bits spell. */
static inline float dr_int2float(uint32_t v)
{
    union { uint32_t i; float f; } u = { v };
    return u.f;
}

#endif
