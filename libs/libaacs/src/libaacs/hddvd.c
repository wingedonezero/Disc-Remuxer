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
#include "mk.h"
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

typedef struct {
    AACS_HDDVD_LOG log;
    void          *log_opaque;
} hddvd_logger;

static void _log(const hddvd_logger *l, int level, const char *fmt, ...) BD_ATTR_FORMAT_PRINTF(3, 4);

static void _log(const hddvd_logger *l, int level, const char *fmt, ...)
{
    char msg[512];
    va_list ap;

    if (!l->log)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    l->log(l->log_opaque, level, msg);
}

static void _hex(char *dst, const uint8_t *src, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
        sprintf(dst + 2 * i, "%02X", src[i]);
}

/* ---- key files (KEYDB.cfg, with or without "0x" before the hex values) ---- */

typedef struct {
    dk_list *dkl;
    pk_list *pkl;
    int      have_mk, have_vid, have_vuk;
    uint8_t  mk[16], vid[16], vuk[16];
    char     mk_from[256], vid_from[256], vuk_from[256];
} hddvd_keys;

/* n hex digits at s (after an optional "0x") into dst; the rest of the
 * field must be blank. */
static int _hex_field(const char *s, uint8_t *dst, unsigned n_bytes)
{
    while (*s == ' ' || *s == '\t')
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (unsigned i = 0; i < n_bytes; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) ||
            sscanf(s + 2 * i, "%2x", &v) != 1)
            return 0;
        dst[i] = (uint8_t)v;
    }
    s += 2 * n_bytes;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return *s == 0 || *s == '|' || *s == ';';
}

static int _hex_value(const char *s, unsigned long *v)
{
    char *end;
    while (*s == ' ' || *s == '\t')
        s++;
    *v = strtoul(s, &end, 16);   /* "0x" accepted by strtoul */
    return end != s;
}

static void _read_dk(hddvd_keys *k, const char *line)
{
    const char *p;
    dk_list *dk = calloc(1, sizeof(*dk));
    unsigned long v;

    if (!dk)
        return;
    if (!(p = strstr(line, "DEVICE_KEY")) || !_hex_field(p + 10, dk->key, 16) ||
        !(p = strstr(line, "DEVICE_NODE")) || !_hex_value(p + 11, &dk->node)) {
        free(dk);
        return;
    }
    if ((p = strstr(line, "KEY_UV")) && _hex_value(p + 6, &v))
        dk->uv = (uint32_t)v;
    if ((p = strstr(line, "KEY_U_MASK_SHIFT")) && _hex_value(p + 16, &v))
        dk->u_mask_shift = (uint8_t)v;
    /* keep the file order */
    dk_list **t = &k->dkl;
    while (*t)
        t = &(*t)->next;
    *t = dk;
}

static void _read_pk(hddvd_keys *k, const char *line)
{
    pk_list *pk = calloc(1, sizeof(*pk));
    const char *p = strchr(line + 1, '|');

    if (!pk)
        return;
    if (!p || !_hex_field(p + 1, pk->key, 16)) {
        free(pk);
        return;
    }
    pk_list **t = &k->pkl;
    while (*t)
        t = &(*t)->next;
    *t = pk;
}

/* "<disc id> = <title> | <tag> | <value> | ..." for disc_id */
static void _read_entry(hddvd_keys *k, const char *line, const char *file, unsigned lineno)
{
    const char *p = strchr(line, '|');
    char from[256];

    snprintf(from, sizeof(from), "%s line %u", file, lineno);
    while (p) {
        const char *tag = p + 1, *val;
        while (*tag == ' ' || *tag == '\t')
            tag++;
        if (!(val = strchr(tag, '|')))
            break;
        if (tag[1] == ' ' || tag[1] == '\t' || tag[1] == '|') {
            if ((*tag == 'M' || *tag == 'm') && !k->have_mk && _hex_field(val + 1, k->mk, 16)) {
                k->have_mk = 1;
                snprintf(k->mk_from, sizeof(k->mk_from), "%s", from);
            } else if ((*tag == 'I' || *tag == 'i') && !k->have_vid && _hex_field(val + 1, k->vid, 16)) {
                k->have_vid = 1;
                snprintf(k->vid_from, sizeof(k->vid_from), "%s", from);
            } else if ((*tag == 'V' || *tag == 'v') && !k->have_vuk && _hex_field(val + 1, k->vuk, 16)) {
                k->have_vuk = 1;
                snprintf(k->vuk_from, sizeof(k->vuk_from), "%s", from);
            }
        }
        p = strchr(val + 1, '|');
    }
}

static int _read_key_file(hddvd_keys *k, const char *path, const char *disc_id_hex, const hddvd_logger *l)
{
    FILE *fp = fopen(path, "r");
    char line[8192];
    unsigned lineno = 0, entries = 0, dks = 0, pks = 0;

    if (!fp) {
        _log(l, AACS_HDDVD_LOG_WARNING, "key file %s cannot be opened", path);
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        const char *s = line;
        lineno++;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '|') {
            const char *t = s + 1;
            while (*t == ' ' || *t == '\t')
                t++;
            if (!strncasecmp(t, "DK", 2) && (t[2] == ' ' || t[2] == '|')) {
                _read_dk(k, s);
                dks++;
            } else if (!strncasecmp(t, "PK", 2) && (t[2] == ' ' || t[2] == '|')) {
                _read_pk(k, s);
                pks++;
            }
            continue;
        }
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
        if (!strncasecmp(s, disc_id_hex, 40) && !isxdigit((unsigned char)s[40])) {
            _read_entry(k, s + 40, path, lineno);
            entries++;
        }
    }
    fclose(fp);
    _log(l, AACS_HDDVD_LOG_INFO, "key file %s: %u device key(s), %u processing key(s), %u entr%s for this disc",
         path, dks, pks, entries, entries == 1 ? "y" : "ies");
    return 0;
}

static void _free_keys(hddvd_keys *k)
{
    while (k->dkl) {
        dk_list *n = k->dkl->next;
        free(k->dkl);
        k->dkl = n;
    }
    while (k->pkl) {
        pk_list *n = k->pkl->next;
        free(k->pkl);
        k->pkl = n;
    }
}

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
                     const hddvd_logger *l)
{
    hddvd_title_key *keys;

    if (size < 16 || memcmp(d, "DVD_HD_V_TKF", 12)) {
        _log(l, AACS_HDDVD_LOG_ERROR, "%s: no title key file identifier (DVD_HD_V_TKF)", name);
        return 0;
    }
    if (MKINT_BE32(d + 12) != TKF_SIZE) {
        _log(l, AACS_HDDVD_LOG_ERROR, "%s: recorded length %u (0x%x expected)", name,
             (unsigned)MKINT_BE32(d + 12), TKF_SIZE);
        return 0;
    }
    if (size >= 0x24 && MKINT_BE32(d + 0x20) != 0)
        _log(l, AACS_HDDVD_LOG_WARNING, "%s: bytes 0x20..0x23 are 0x%08x, not 0", name, (unsigned)MKINT_BE32(d + 0x20));
    if (size < TKF_KEY_AT(TKF_KEYS - 1) + 16) {
        _log(l, AACS_HDDVD_LOG_ERROR, "%s: %zu bytes, too short for %d title keys", name, size, TKF_KEYS);
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
                _log(l, AACS_HDDVD_LOG_ERROR, "%s: title key %u is given twice", name, (unsigned)id);
                return 0;
            }
        keys[h->nb_keys].id = id;
        memcpy(keys[h->nb_keys].key, d + TKF_KEY_AT(j), 16);   /* encrypted until the VUK is known */
        h->nb_keys++;
    }
    return 1;
}

static int _read_file(AACS_HDDVD_READ read, void *opaque, const char *name, uint8_t **data, size_t *size,
                      const hddvd_logger *l)
{
    int r = read(opaque, name, data, size);
    if (r < 0)
        _log(l, AACS_HDDVD_LOG_ERROR, "%s cannot be read", name);
    return r;
}

/* The volume unique key: from the key files, or computed from the media key
 * (given, or from the MKB with a device / processing key) and the volume ID. */
static int _find_vuk(AACS_HDDVD *h, hddvd_keys *k, const uint8_t *mkb_data, size_t mkb_size, const hddvd_logger *l)
{
    char hex[41];
    MKB *mkb = NULL;
    int err;

    if (k->have_vuk) {
        memcpy(h->vuk, k->vuk, 16);
        _log(l, AACS_HDDVD_LOG_INFO, "volume unique key from %s", k->vuk_from);
        return AACS_SUCCESS;
    }
    if (!k->have_mk && mkb_data) {
        uint8_t *copy = malloc(mkb_size);
        if (copy) {
            memcpy(copy, mkb_data, mkb_size);
            mkb = mkb_init(copy, mkb_size);     /* owns copy */
        }
        if (!mkb) {
            _log(l, AACS_HDDVD_LOG_ERROR, "MKBROM.AACS is not a media key block");
            return AACS_ERROR_CORRUPTED_DISC;
        }
        err = AACS_ERROR_NO_DK;
        if (k->dkl)
            err = aacs_calc_mk_dks(mkb, k->dkl, k->mk);
        if (err != AACS_SUCCESS)
            err = aacs_calc_mk_pks(mkb, k->pkl, k->mk);
        if (err == AACS_SUCCESS) {
            k->have_mk = 1;
            snprintf(k->mk_from, sizeof(k->mk_from), "MKB version %u with a device or processing key",
                     (unsigned)mkb_version(mkb));
        } else {
            _log(l, AACS_HDDVD_LOG_WARNING, "MKB version %u: no device or processing key of the key files fits",
                 (unsigned)mkb_version(mkb));
        }
        mkb_close(mkb);
    }
    if (k->have_mk) {
        _hex(hex, k->mk, 16);
        _log(l, AACS_HDDVD_LOG_INFO, "media key %s (%s)", hex, k->mk_from);
    }
    if (!k->have_vid) {
        _log(l, AACS_HDDVD_LOG_ERROR, "no volume unique key and no volume ID for this disc in the key files: "
             "the disc cannot be decrypted (the volume ID comes from a drive)");
        return AACS_ERROR_NO_CONFIG;
    }
    if (!k->have_mk)
        return AACS_ERROR_NO_PK;
    if (crypto_aes128d(k->mk, k->vid, h->vuk))
        return AACS_ERROR_UNKNOWN;
    for (unsigned i = 0; i < 16; i++)
        h->vuk[i] ^= k->vid[i];
    _log(l, AACS_HDDVD_LOG_INFO, "volume unique key from the media key and the volume ID of %s", k->vid_from);
    return AACS_SUCCESS;
}

AACS_HDDVD *aacs_hddvd_open(AACS_HDDVD_READ read, void *opaque,
                            const char *const *key_files, unsigned nb_key_files,
                            unsigned nb_title_key_files,
                            AACS_HDDVD_LOG log, void *log_opaque, int *error_code)
{
    hddvd_logger l = { log, log_opaque };
    hddvd_keys k;
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
            _log(&l, AACS_HDDVD_LOG_ERROR, "no VTKF000.AACS: the disc has no identifier");
        goto fail;
    }
    gcry_md_hash_buffer(GCRY_MD_SHA1, h->disc_id, d, size);
    free(d);
    d = NULL;
    _hex(hex, h->disc_id, 20);
    _log(&l, AACS_HDDVD_LOG_INFO, "disc ID %s (SHA-1 of VTKF000.AACS)", hex);

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
        _read_key_file(&k, key_files[i], hex, &l);
    if ((r = _read_file(read, opaque, "MKBROM.AACS", &mkb, &mkb_size, &l)) < 0)
        goto fail;
    if (r > 0)
        _log(&l, AACS_HDDVD_LOG_WARNING, "no MKBROM.AACS");
    if ((err = _find_vuk(h, &k, r == 0 ? mkb : NULL, mkb_size, &l)) != AACS_SUCCESS)
        goto fail;
    for (unsigned i = 0; i < h->nb_keys; i++) {
        uint8_t enc[16];
        memcpy(enc, h->keys[i].key, 16);
        if (crypto_aes128d(h->vuk, enc, h->keys[i].key)) {
            err = AACS_ERROR_UNKNOWN;
            goto fail;
        }
    }
    _log(&l, AACS_HDDVD_LOG_INFO, "%u title keys", h->nb_keys);
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
    _free_keys(&k);
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
