/*
 * CRC-32/MPEG-2 (ISO 13818-1 Annex A: polynomial 0x04C11DB7, no
 * reflection), the checksum MPEG-TS sections carry — a valid section
 * hashed whole (its CRC bytes included) gives 0.
 */

#ifndef DISC_BASE_CRC32_H
#define DISC_BASE_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The running CRC over len bytes; start with crc 0xffffffff. */
uint32_t dr_crc32_mpeg(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DISC_BASE_CRC32_H */
