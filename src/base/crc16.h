/*
 * CRC-16 "ANSI" (polynomial 0x8005), bit-identical to FFmpeg's
 * av_crc(AV_CRC_16_ANSI): the table is built the same way (big-endian
 * rounds, byte-swapped) and the update runs low byte first. A/52 frames
 * carry this CRC (crc1 / crc2); a whole protected span gives 0.
 */

#ifndef DISC_BASE_CRC16_H
#define DISC_BASE_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t dr_crc16_ansi(uint16_t crc, const uint8_t *buf, size_t len);

/** CRC-16 CCITT (polynomial 0x1021, most significant bit first), as
 *  av_crc(AV_CRC_16_CCITT) runs it: DTS (ETSI TS 102 114) protects its
 *  extension substream and lossless headers with it, from 0xFFFF; a whole
 *  protected span (the CRC field included) gives 0. */
uint16_t dr_crc16_ccitt(uint16_t crc, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
