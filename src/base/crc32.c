/*
 * CRC-32/MPEG-2 (see crc32.h): byte-wise over a table built on first use.
 */

#include "base/crc32.h"

uint32_t dr_crc32_mpeg(uint32_t crc, const uint8_t *data, size_t len)
{
    static uint32_t table[256];

    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int b = 0; b < 8; b++)
                c = c & 0x80000000 ? (c << 1) ^ 0x04c11db7 : c << 1;
            table[i] = c;
        }
    }
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ table[(crc >> 24) ^ data[i]];
    return crc;
}
