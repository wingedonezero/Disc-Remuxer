/*
 * This file is part of libaacs
 *
 * AACS for Blu-ray with the application's own file reading: the CPS unit
 * keys of a disc (from its encrypted unit keys and the volume unique key the
 * key files lead to, or from the key files' unit-key list) and the
 * decryption of a 6144-byte aligned unit with one of them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include "util/attributes.h"

#include "aacs.h"
#include "crypto.h"
#include "keyfile.h"

#include <stdlib.h>
#include <string.h>

#define ALIGNED_UNIT 6144

struct aacs_bd {
    unsigned  nb_uk;
    uint8_t (*uk)[16];
};

void aacs_bd_close(AACS_BD *b)
{
    if (b) {
        if (b->uk)
            memset(b->uk, 0, b->nb_uk * sizeof(*b->uk));
        free(b->uk);
        free(b);
    }
}

AACS_BD *aacs_bd_open(const uint8_t disc_id[20], const uint8_t (*enc_uk)[16], unsigned nb_uk,
                      const uint8_t *mkb, size_t mkb_size, const char *mkb_name,
                      const char *const *key_files, unsigned nb_key_files,
                      AACS_BD_LOG log, void *log_opaque, int *error_code)
{
    aacs_kf_logger l = { log, log_opaque };
    aacs_kf_keys k;
    AACS_BD *b = NULL;
    uint8_t vuk[16];
    char hex[41];
    int err = AACS_ERROR_UNKNOWN;

    memset(&k, 0, sizeof(k));
    if (!nb_uk || !crypto_init() || !(b = calloc(1, sizeof(*b))) || !(b->uk = calloc(nb_uk, sizeof(*b->uk))))
        goto fail;
    b->nb_uk = nb_uk;

    aacs_kf_hex(hex, disc_id, 20);
    for (unsigned i = 0; i < nb_key_files; i++)
        aacs_kf_read(&k, key_files[i], hex, &l);

    /* the volume unique key if an entry gives it; else the entry's unit
     * keys; else the media key and volume ID */
    if (!k.have_vuk && k.nb_uk) {
        for (unsigned n = 1; n <= nb_uk; n++)
            if (n > k.nb_uk || !k.have_uk[n - 1]) {
                aacs_kf_log(&l, AACS_BD_LOG_ERROR, "the unit keys of %s lack unit %u of %u", k.uk_from, n, nb_uk);
                err = AACS_ERROR_NO_CONFIG;
                goto fail;
            }
        memcpy(b->uk, k.uk, nb_uk * sizeof(*b->uk));
        aacs_kf_log(&l, AACS_BD_LOG_INFO, "%u unit key(s) from %s", nb_uk, k.uk_from);
        err = AACS_SUCCESS;
        goto out;
    }
    if ((err = aacs_kf_find_vuk(&k, mkb_name, mkb, mkb_size, vuk, &l)) != AACS_SUCCESS)
        goto fail;
    for (unsigned n = 0; n < nb_uk; n++)
        if (crypto_aes128d(vuk, enc_uk[n], b->uk[n])) {
            err = AACS_ERROR_UNKNOWN;
            goto fail;
        }
    memset(vuk, 0, sizeof(vuk));
    aacs_kf_log(&l, AACS_BD_LOG_INFO, "%u unit key(s) decrypted with the volume unique key", nb_uk);
    err = AACS_SUCCESS;
    goto out;

fail:
    aacs_bd_close(b);
    b = NULL;
out:
    aacs_kf_free(&k);
    memset(&k, 0, sizeof(k));
    if (error_code)
        *error_code = err;
    return b;
}

unsigned aacs_bd_units(const AACS_BD *b)
{
    return b->nb_uk;
}

int aacs_bd_unit_key(const AACS_BD *b, unsigned unit, uint8_t key[16])
{
    if (unit < 1 || unit > b->nb_uk)
        return 0;
    memcpy(key, b->uk[unit - 1], 16);
    return 1;
}

int aacs_bd_decrypt_unit(const uint8_t unit_key[16], uint8_t *unit)
{
    uint8_t bk[16];

    if (crypto_aes128e(unit_key, unit, bk))
        return -1;
    for (unsigned i = 0; i < 16; i++)
        bk[i] ^= unit[i];
    return crypto_aacs_decrypt(bk, unit + 16, ALIGNED_UNIT - 16, NULL, 0) ? -1 : 0;
}
