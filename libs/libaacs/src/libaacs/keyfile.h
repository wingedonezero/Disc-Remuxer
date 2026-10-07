/*
 * This file is part of libaacs
 *
 * Key files (KEYDB.cfg, with or without "0x" before the hex values) read in
 * the order the application gives them, and the volume unique key they lead
 * to. Shared by the HD DVD (hddvd.c) and Blu-ray (bd.c) opens.
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

#ifndef AACS_KEYFILE_H_
#define AACS_KEYFILE_H_

#include "util/attributes.h"
#include "aacs.h"
#include "file/keydbcfg.h"

#include <stddef.h>
#include <stdint.h>

#define AACS_KF_MAX_UK 2000     /* CPS units of a disc (Unit_Key_RO.inf allows 1..2000) */

typedef struct {
    AACS_HDDVD_LOG log;
    void          *log_opaque;
} aacs_kf_logger;

/* What the key files hold for one disc: device and processing keys (every
 * file's, in file order), and of the entries for the disc ID the first media
 * key, volume ID, volume unique key and unit-key list (U) found. */
typedef struct {
    dk_list *dkl;
    pk_list *pkl;
    int      have_mk, have_vid, have_vuk;
    uint8_t  mk[16], vid[16], vuk[16];
    char     mk_from[256], vid_from[256], vuk_from[256];
    unsigned nb_uk;              /* highest unit number of the U list (0: none) */
    uint8_t (*uk)[16];           /* uk[n - 1] = unit n's key */
    uint8_t *have_uk;            /* have_uk[n - 1]: unit n is in the list */
    char     uk_from[256];
} aacs_kf_keys;

BD_PRIVATE void aacs_kf_log(const aacs_kf_logger *l, int level, const char *fmt, ...) BD_ATTR_FORMAT_PRINTF(3, 4);
BD_PRIVATE void aacs_kf_hex(char *dst, const uint8_t *src, unsigned len);

/* One key file into k (disc_id_hex: 40 upper-case hex digits); -1 when it
 * cannot be opened (logged), else 0. */
BD_PRIVATE int  aacs_kf_read(aacs_kf_keys *k, const char *path, const char *disc_id_hex, const aacs_kf_logger *l);
BD_PRIVATE void aacs_kf_free(aacs_kf_keys *k);

/* The volume unique key: from the key files, or computed from the media key
 * (given, or from the media key block mkb with a device / processing key) and
 * the volume ID. mkb_name names the MKB file for the log.
 * @return AACS_SUCCESS, or an AACS_ERROR_* code */
BD_PRIVATE int  aacs_kf_find_vuk(aacs_kf_keys *k, const char *mkb_name, const uint8_t *mkb, size_t mkb_size,
                                 uint8_t vuk[16], const aacs_kf_logger *l);

#endif /* AACS_KEYFILE_H_ */
