/*
 * This file is part of libaacs
 *
 * Key files (KEYDB.cfg, with or without "0x" before the hex values) and the
 * volume unique key, for the HD DVD and Blu-ray opens (keyfile.h).
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

#include "keyfile.h"

#include "crypto.h"
#include "mk.h"
#include "mkb.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void aacs_kf_log(const aacs_kf_logger *l, int level, const char *fmt, ...)
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

void aacs_kf_hex(char *dst, const uint8_t *src, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
        sprintf(dst + 2 * i, "%02X", src[i]);
}

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

static void _read_dk(aacs_kf_keys *k, const char *line)
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

static void _read_pk(aacs_kf_keys *k, const char *line)
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

/* "<n>-<key>" at s: 1 and unit n (1..AACS_KF_MAX_UK) with its key, else 0. */
static int _uk_field(const char *s, unsigned *n, uint8_t key[16])
{
    char *end;
    unsigned long v;

    while (*s == ' ' || *s == '\t')
        s++;
    if (!isdigit((unsigned char)*s))
        return 0;
    v = strtoul(s, &end, 10);
    if (*end != '-' || v < 1 || v > AACS_KF_MAX_UK || !_hex_field(end + 1, key, 16))
        return 0;
    *n = (unsigned)v;
    return 1;
}

/* The unit-key list that follows a U tag ("| U | 1-0x<key> | 2-0x<key> ..."),
 * from the field at s; returns where the next tag starts (or NULL). */
static const char *_read_uks(aacs_kf_keys *k, const char *s, const char *from)
{
    int take = !k->nb_uk;          /* only the first list found counts */

    for (;;) {
        uint8_t key[16];
        unsigned n;

        if (!_uk_field(s, &n, key))
            return s - 1;          /* the '|' before this field: a tag follows */
        if (take) {
            if (n > k->nb_uk) {
                uint8_t (*uk)[16] = realloc(k->uk, n * sizeof(*uk));
                uint8_t *have = realloc(k->have_uk, n);
                if (uk)
                    k->uk = uk;
                if (have)
                    k->have_uk = have;
                if (!uk || !have)
                    return NULL;
                memset(k->have_uk + k->nb_uk, 0, n - k->nb_uk);
                k->nb_uk = n;
            }
            memcpy(k->uk[n - 1], key, 16);
            k->have_uk[n - 1] = 1;
            snprintf(k->uk_from, sizeof(k->uk_from), "%s", from);
        }
        if (!(s = strchr(s, '|')))
            return NULL;
        s++;
    }
}

/* "<disc id> = <title> | <tag> | <value> | ..." for disc_id */
static void _read_entry(aacs_kf_keys *k, const char *line, const char *file, unsigned lineno)
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
            } else if (*tag == 'U' || *tag == 'u') {
                if (!(p = _read_uks(k, val + 1, from)))
                    break;
                continue;
            }
        }
        p = strchr(val + 1, '|');
    }
}

int aacs_kf_read(aacs_kf_keys *k, const char *path, const char *disc_id_hex, const aacs_kf_logger *l)
{
    FILE *fp = fopen(path, "r");
    char line[8192];
    unsigned lineno = 0, entries = 0, dks = 0, pks = 0;

    if (!fp) {
        aacs_kf_log(l, AACS_HDDVD_LOG_WARNING, "key file %s cannot be opened", path);
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
    aacs_kf_log(l, AACS_HDDVD_LOG_INFO, "key file %s: %u device key(s), %u processing key(s), %u entr%s for this disc",
                path, dks, pks, entries, entries == 1 ? "y" : "ies");
    return 0;
}

void aacs_kf_free(aacs_kf_keys *k)
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
    free(k->uk);
    free(k->have_uk);
    k->uk      = NULL;
    k->have_uk = NULL;
    k->nb_uk   = 0;
}

int aacs_kf_find_vuk(aacs_kf_keys *k, const char *mkb_name, const uint8_t *mkb_data, size_t mkb_size,
                     uint8_t vuk[16], const aacs_kf_logger *l)
{
    char hex[41];
    MKB *mkb = NULL;
    int err;

    if (k->have_vuk) {
        memcpy(vuk, k->vuk, 16);
        aacs_kf_log(l, AACS_HDDVD_LOG_INFO, "volume unique key from %s", k->vuk_from);
        return AACS_SUCCESS;
    }
    if (!k->have_mk && mkb_data) {
        uint8_t *copy = malloc(mkb_size);
        if (copy) {
            memcpy(copy, mkb_data, mkb_size);
            mkb = mkb_init(copy, mkb_size);     /* owns copy */
        }
        if (!mkb) {
            aacs_kf_log(l, AACS_HDDVD_LOG_ERROR, "%s is not a media key block", mkb_name);
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
            aacs_kf_log(l, AACS_HDDVD_LOG_WARNING, "MKB version %u: no device or processing key of the key files fits",
                        (unsigned)mkb_version(mkb));
        }
        mkb_close(mkb);
    }
    if (k->have_mk) {
        aacs_kf_hex(hex, k->mk, 16);
        aacs_kf_log(l, AACS_HDDVD_LOG_INFO, "media key %s (%s)", hex, k->mk_from);
    }
    if (!k->have_vid) {
        aacs_kf_log(l, AACS_HDDVD_LOG_ERROR, "no volume unique key and no volume ID for this disc in the key files: "
                    "the disc cannot be decrypted (the volume ID comes from a drive)");
        return AACS_ERROR_NO_CONFIG;
    }
    if (!k->have_mk)
        return AACS_ERROR_NO_PK;
    if (crypto_aes128d(k->mk, k->vid, vuk))
        return AACS_ERROR_UNKNOWN;
    for (unsigned i = 0; i < 16; i++)
        vuk[i] ^= k->vid[i];
    aacs_kf_log(l, AACS_HDDVD_LOG_INFO, "volume unique key from the media key and the volume ID of %s", k->vid_from);
    return AACS_SUCCESS;
}
