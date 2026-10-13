/*
 * CRC-16 ANSI (see crc16.h).
 */

#include "base/crc16.h"

static uint32_t tab[256];
static int built;

static uint32_t bswap32(uint32_t x)
{
    return x << 24 | (x & 0xff00) << 8 | (x >> 8 & 0xff00) | x >> 24;
}

static void build(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int j = 0; j < 8; j++)
            c = (c << 1) ^ (((uint32_t)0x8005 << 16) & (uint32_t)((int32_t)c >> 31));
        tab[i] = bswap32(c);
    }
    built = 1;
}

uint16_t dr_crc16_ansi(uint16_t crc, const uint8_t *buf, size_t len)
{
    uint32_t c = crc;

    if (!built)
        build();            /* concurrent first calls write the same values */
    for (size_t i = 0; i < len; i++)
        c = tab[(uint8_t)c ^ buf[i]] ^ (c >> 8);
    return (uint16_t)c;
}
