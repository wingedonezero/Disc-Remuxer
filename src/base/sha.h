/*
 * SHA-1 and SHA-256 (FIPS 180-4), for the disc formats' content hashes.
 * One context for both: dr_sha_init() picks the function by digest bits
 * (160 or 256).
 */

#ifndef DISC_BASE_SHA_H
#define DISC_BASE_SHA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DrSha {
    uint64_t len;           /**< message bytes so far */
    uint32_t h[8];
    uint8_t  block[64];
    int      bits;          /**< 160 or 256 */
} DrSha;

/** bits: 160 (SHA-1) or 256 (SHA-256); anything else is read as 256. */
void dr_sha_init(DrSha *s, int bits);
void dr_sha_update(DrSha *s, const uint8_t *data, size_t len);
/** The digest (bits / 8 bytes) into out. */
void dr_sha_final(DrSha *s, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DISC_BASE_SHA_H */
