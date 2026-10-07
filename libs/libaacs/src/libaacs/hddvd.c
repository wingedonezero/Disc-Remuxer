/*
 * This file is part of libaacs
 *
 * AACS for HD DVD (Advanced Content): disc identifier, volume unique key,
 * title keys and the decryption of a 2048-byte EVOB pack.
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
#include "mkb.h"

#include "file/keydbcfg.h"
#include "util/macro.h"

#include <gcrypt.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TKF_SIZE      0x9b0
#define TKF_KEYS      64
#define TKF_KEY_AT(j) (0x84 + 0x24 * (j))

typedef struct {
    uint32_t id;
    uint8_t  key[16];
} hddvd_title_key;

struct aacs_hddvd {
    uint8_t          disc_id[20];
    uint8_t          vuk[16];
    hddvd_title_key *keys;
    unsigned         nb_keys;
};

/* ---- the disc ---- */

static void _close(AACS_HDDVD *h)
{
    if (h) {
        free(h->keys);
        free(h);
    }
}

void aacs_hddvd_close(AACS_HDDVD *h)
{
    _close(h);
}

/* The 64 encrypted title keys of VTKF<n>.AACS (ids n * 0x100 + slot + 1). */
static int _read_tkf(AACS_HDDVD *h, const uint8_t *d, size_t size, unsigned n, const char *name,
                     const aacs_kf_logger *l)
{
    hddvd_title_key *keys;

    if (size < 16 || memcmp(d, "DVD_HD_V_TKF", 12)) {
        aacs_kf_log(l, AACS_HDDVD_LOG_ERROR, "%s: no title key file identifier (DVD_HD_V_TKF)", name);
        return 0;
    }
    if (MKINT_BE32(d + 12) != TKF_SIZE) {
        aacs_kf_log(l, AACS_HDDVD_LOG_ERROR, "%s: recorded length %u (0x%x expected)", name,
             (unsigned)MKINT_BE32(d + 12), TKF_SIZE);
        return 0;
    }
    if (size >= 0x24 && MKINT_BE32(d + 0x20) != 0)
        aacs_kf_log(l, AACS_HDDVD_LOG_WARNING, "%s: bytes 0x20..0x23 are 0x%08x, not 0", name, (unsigned)MKINT_BE32(d + 0x20));
    if (size < TKF_SIZE) {
        aacs_kf_log(l, AACS_HDDVD_LOG_ERROR, "%s: %zu bytes, shorter than its recorded length 0x%x", name, size, TKF_SIZE);
        return 0;
    }
    keys = realloc(h->keys, (h->nb_keys + TKF_KEYS) * sizeof(*keys));
    if (!keys)
        return 0;
    h->keys = keys;
    for (unsigned j = 0; j < TKF_KEYS; j++) {
        uint32_t id = n * 0x100 + j + 1;
        for (unsigned i = 0; i < h->nb_keys; i++)
            if (h->keys[i].id == id) {
                aacs_kf_log(l, AACS_HDDVD_LOG_ERROR, "%s: title key %u is given twice", name, (unsigned)id);
                return 0;
            }
        keys[h->nb_keys].id = id;
        memcpy(keys[h->nb_keys].key, d + TKF_KEY_AT(j), 16);   /* encrypted until the VUK is known */
        h->nb_keys++;
    }
    return 1;
}

static int _read_file(AACS_HDDVD_READ read, void *opaque, const char *name, uint8_t **data, size_t *size,
                      const aacs_kf_logger *l)
{
    int r = read(opaque, name, data, size);
    if (r < 0)
        aacs_kf_log(l, AACS_HDDVD_LOG_ERROR, "%s cannot be read", name);
    return r;
}

AACS_HDDVD *aacs_hddvd_open(AACS_HDDVD_READ read, void *opaque,
                            const char *const *key_files, unsigned nb_key_files,
                            unsigned nb_title_key_files,
                            AACS_HDDVD_LOG log, void *log_opaque, int *error_code)
{
    aacs_kf_logger l = { log, log_opaque };
    aacs_kf_keys k;
    AACS_HDDVD *h;
    uint8_t *d = NULL, *mkb = NULL;
    size_t size = 0, mkb_size = 0;
    char hex[41], name[32];
    int err = AACS_ERROR_CORRUPTED_DISC, r;

    memset(&k, 0, sizeof(k));
    if (!crypto_init() || !(h = calloc(1, sizeof(*h)))) {
        err = AACS_ERROR_UNKNOWN;
        goto out;
    }

    /* the disc identifier the key files use: SHA-1 of VTKF000.AACS */
    if ((r = _read_file(read, opaque, "VTKF000.AACS", &d, &size, &l)) != 0) {
        if (r > 0)
            aacs_kf_log(&l, AACS_HDDVD_LOG_ERROR, "no VTKF000.AACS: the disc has no identifier");
        goto fail;
    }
    gcry_md_hash_buffer(GCRY_MD_SHA1, h->disc_id, d, size);
    free(d);
    d = NULL;
    aacs_kf_hex(hex, h->disc_id, 20);
    aacs_kf_log(&l, AACS_HDDVD_LOG_INFO, "disc ID %s (SHA-1 of VTKF000.AACS)", hex);

    /* the title key files, one per playlist */
    for (unsigned n = 0; n < nb_title_key_files; n++) {
        snprintf(name, sizeof(name), "VTKF%03u.AACS", n);
        if ((r = _read_file(read, opaque, name, &d, &size, &l)) > 0)
            continue;
        if (r < 0 || !_read_tkf(h, d, size, n, name, &l))
            goto fail;
        free(d);
        d = NULL;
    }

    /* the keys */
    for (unsigned i = 0; i < nb_key_files; i++)
        aacs_kf_read(&k, key_files[i], hex, &l);
    if ((r = _read_file(read, opaque, "MKBROM.AACS", &mkb, &mkb_size, &l)) < 0)
        goto fail;
    if (r > 0)
        aacs_kf_log(&l, AACS_HDDVD_LOG_WARNING, "no MKBROM.AACS");
    if ((err = aacs_kf_find_vuk(&k, "MKBROM.AACS", r == 0 ? mkb : NULL, mkb_size, h->vuk, &l)) != AACS_SUCCESS)
        goto fail;
    for (unsigned i = 0; i < h->nb_keys; i++) {
        uint8_t enc[16];
        memcpy(enc, h->keys[i].key, 16);
        if (crypto_aes128d(h->vuk, enc, h->keys[i].key)) {
            err = AACS_ERROR_UNKNOWN;
            goto fail;
        }
    }
    aacs_kf_log(&l, AACS_HDDVD_LOG_INFO, "%u title keys", h->nb_keys);
    err = AACS_SUCCESS;
    goto out;

fail:
    if (err == AACS_SUCCESS)
        err = AACS_ERROR_CORRUPTED_DISC;
    _close(h);
    h = NULL;
out:
    free(d);
    free(mkb);
    aacs_kf_free(&k);
    if (error_code)
        *error_code = err;
    return h;
}

const uint8_t *aacs_hddvd_disc_id(const AACS_HDDVD *h)
{
    return h->disc_id;
}

int aacs_hddvd_title_key(const AACS_HDDVD *h, uint32_t id, uint8_t key[16])
{
    for (unsigned i = 0; i < h->nb_keys; i++)
        if (h->keys[i].id == id) {
            memcpy(key, h->keys[i].key, 16);
            return 1;
        }
    return 0;
}

int aacs_hddvd_decrypt_pack(const uint8_t title_key[16], const uint8_t seed12[12], uint8_t *pack)
{
    uint8_t seed[16], bk[16];

    memcpy(seed, pack + 0x54, 4);
    memcpy(seed + 4, seed12, 12);
    if (crypto_aes128d(title_key, seed, bk))
        return -1;
    for (unsigned i = 0; i < 16; i++)
        bk[i] ^= seed[i];
    return crypto_aacs_decrypt(bk, pack + 0x80, 0x780, NULL, 0) ? -1 : 0;
}
