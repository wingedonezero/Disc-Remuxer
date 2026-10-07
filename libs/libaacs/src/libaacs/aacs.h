/*
 * This file is part of libaacs
 * Copyright (C) 2009-2010  Obliter0n
 * Copyright (C) 2009-2013  npzacz
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

#ifndef AACS_H_
#define AACS_H_

#include <stddef.h>
#include <stdint.h>

#ifndef AACS_PUBLIC
#  define AACS_PUBLIC
#endif

/**
 * aacs_open_device() error codes
 */
#define AACS_SUCCESS               0 /* no errors */
#define AACS_ERROR_CORRUPTED_DISC -1 /* opening or reading of AACS files failed */
#define AACS_ERROR_NO_CONFIG      -2 /* missing config file */
#define AACS_ERROR_NO_PK          -3 /* no matching processing key */
#define AACS_ERROR_NO_CERT        -4 /* no valid certificate */
#define AACS_ERROR_CERT_REVOKED   -5 /* certificate has been revoked */
#define AACS_ERROR_MMC_OPEN       -6 /* MMC open failed (no MMC drive ?) */
#define AACS_ERROR_MMC_FAILURE    -7 /* MMC failed */
#define AACS_ERROR_NO_DK          -8 /* no matching device key */
#define AACS_ERROR_UNKNOWN        -9 /* some other failure, see logs */
#define AACS_ERROR_UNSUPPORTED_DISC -10  /* unsupported AACS version */

AACS_PUBLIC const char *aacs_error_str(int error);

/**
 * Opaque type for AACS object
 */
typedef struct aacs AACS;

/**
 * Get version of AACS library.
 *
 * @param major  where to store libaacs major version
 * @param minor  where to store libaacs minor version
 * @param micro  where to store libaacs micro version
 */
AACS_PUBLIC void aacs_get_version(int *major, int *minor, int *micro);

/*
 * open / close a disc
 */

/**
 * Initialize an AACS object.
 *
 * Disc must be opened with aacs_open_device().
 *
 * @return  AACS object, NULL on error
 */
AACS_PUBLIC AACS *aacs_init(void);

/**
 * Disable / enable key caching.
 *
 * Controls if keys and revocation lists are cached locally.
 * Disabling caching disables updating the cache and using data from cache.
 * Enabled by default.
 */
AACS_PUBLIC void aacs_set_key_caching(AACS *, int enable);

/**
 * Open AACS disc / device.
 *
 * If device is not accessible (reading from .iso file or network stream),
 * path can be NULL.
 *
 * If files stored in AACS/ are not accessible from the file system (ex. unmounted disc),
 * application should provide file system access for libaacs
 * (see filesystem.h:aacs_set_fopen()).
 *
 * @param  aacs  AACS object
 * @param  path  path to device or disc root or NULL
 * @param  keyfile_path  optional path to AACS key file
 * @return  AACS_SUCCESS or ACS_ERROR_* error code
 */
AACS_PUBLIC int aacs_open_device(AACS *, const char *path, const char *keyfile_path);

/* deprecated */
AACS_PUBLIC AACS *aacs_open(const char *path, const char *keyfile_path);

/* deprecated */
AACS_PUBLIC AACS *aacs_open2(const char *path, const char *keyfile_path, int *error_code);

/**
 * Closes and cleans up the AACS object.
 *
 * @param aacs  AACS object
 */
AACS_PUBLIC void aacs_close(AACS *aacs);

/*
 * decryption
 */

#define AACS_TITLE_FIRST_PLAY  0
#define AACS_TITLE_TOP_MENU    0xffff

/**
 * Select title (optional).
 *
 * Titles can be stored in different CPS units and use different encryption key.
 *
 * If current title is not set, CPS unit is autodetected (with minor performance cost).
 * If application does not provide current title to libaacs, current CPS unit
 * should be reset by selecting First Play title (0xffff) when a new playlist is
 * started.
 *
 * @param aacs  AACS object
 * @param title  Current title from disc index
 */
AACS_PUBLIC void aacs_select_title(AACS *aacs, uint32_t title_number);

/**
 * Decrypt data unit
 *
 * Remove possible bus encryption and AACS encryption.
 *
 * NOTE: only units from the same CPS unit may be decrypted in paraller.
 *
 * @param aacs  AACS object
 * @param buf  unit to decrypt
 * @return  1 on success, 0 on error
 */
AACS_PUBLIC int  aacs_decrypt_unit(AACS *aacs, uint8_t *buf);

/**
 * Remove bus encryption.
 *
 * If bus encryption is used, remove bus encryption.
 * Like aacs_decrypt_unit(), but does not perform AACS decryption.
 *
 * @param aacs  AACS object
 * @param buf  unit to decrypt
 * @return  1 on success, 0 on error
 */
AACS_PUBLIC int  aacs_decrypt_bus(AACS *aacs, uint8_t *buf);

/*
 * Disc information
 */
AACS_PUBLIC int aacs_get_mkb_version(AACS *aacs);
AACS_PUBLIC const uint8_t *aacs_get_disc_id(AACS *aacs);
AACS_PUBLIC const uint8_t *aacs_get_vid(AACS *aacs);  /* may fail even if disc can be decrypted */
AACS_PUBLIC const uint8_t *aacs_get_pmsn(AACS *aacs); /* may fail even if disc can be decrypted */
AACS_PUBLIC const uint8_t *aacs_get_mk(AACS *aacs);   /* may fail even if disc can be decrypted */
AACS_PUBLIC const uint8_t *aacs_get_content_cert_id(AACS *aacs);
AACS_PUBLIC const uint8_t *aacs_get_bdj_root_cert_hash(AACS *aacs);
AACS_PUBLIC const uint8_t *aacs2_get_bdj_root_cert_hash(AACS *aacs);

/*
 * AACS Online
 */
AACS_PUBLIC const uint8_t *aacs_get_device_binding_id(AACS *aacs);
AACS_PUBLIC const uint8_t *aacs_get_device_nonce(AACS *aacs);

/*
 * Revocation lists
 */
typedef struct {
    uint16_t  range;
    uint8_t   id[6];
} AACS_RL_ENTRY;

AACS_PUBLIC AACS_RL_ENTRY *aacs_get_hrl(int *num_entries, int *mkb_version);
AACS_PUBLIC AACS_RL_ENTRY *aacs_get_drl(int *num_entries, int *mkb_version);
AACS_PUBLIC void           aacs_free_rl(AACS_RL_ENTRY **rl);

/*
 * Bus encryption information
 */

#define AACS_BUS_ENCRYPTION_ENABLED  0x01  /* Bus encryption enabled in the media */
#define AACS_BUS_ENCRYPTION_CAPABLE  0x02  /* Bus encryption capable drive */

AACS_PUBLIC uint32_t aacs_get_bus_encryption(AACS *);

/*
 * Copy Control Information
 */

struct aacs_basic_cci;

AACS_PUBLIC struct aacs_basic_cci *aacs_get_basic_cci(AACS *, uint32_t title);

/*
 * HD DVD (Advanced Content)
 */

typedef struct aacs_hddvd AACS_HDDVD;

/* Reads a file of the disc's AACS directory (name: "MKBROM.AACS",
 * "VTKF000.AACS", ...): 0 and *data (allocated with malloc, freed by
 * libaacs) / *size, 1 when the file does not exist, < 0 on a read error. */
typedef int (*AACS_HDDVD_READ)(void *opaque, const char *name, uint8_t **data, size_t *size);

#define AACS_HDDVD_LOG_ERROR   0
#define AACS_HDDVD_LOG_WARNING 1
#define AACS_HDDVD_LOG_INFO    2

/* One message of the HD DVD AACS open (level AACS_HDDVD_LOG_*). */
typedef void (*AACS_HDDVD_LOG)(void *opaque, int level, const char *message);

/**
 * Open the AACS layer of an HD DVD: the disc ID (SHA-1 of VTKF000.AACS), the
 * title keys of VTKF000.AACS .. VTKF<nb_title_key_files - 1>.AACS (64 per
 * file, ids file * 0x100 + slot + 1; a missing file is skipped, a bad one
 * fails the open) decrypted with the volume unique key. The volume unique
 * key comes from the key files (KEYDB.cfg, with or without "0x" before the
 * hex values), read in order: the first entry for the disc ID with a volume
 * unique key; else media key (the first entry's, or from MKBROM.AACS with the
 * files' device keys, then processing keys) and the first entry's volume ID.
 * @return the handle, or NULL with *error_code an AACS_ERROR_* code
 */
AACS_PUBLIC AACS_HDDVD *aacs_hddvd_open(AACS_HDDVD_READ read, void *opaque,
                                        const char *const *key_files, unsigned nb_key_files,
                                        unsigned nb_title_key_files,
                                        AACS_HDDVD_LOG log, void *log_opaque, int *error_code);
AACS_PUBLIC void aacs_hddvd_close(AACS_HDDVD *);

/* The 20-byte disc ID. */
AACS_PUBLIC const uint8_t *aacs_hddvd_disc_id(const AACS_HDDVD *);

/* Title key id: 1 and key, or 0 when the disc has no such key. */
AACS_PUBLIC int aacs_hddvd_title_key(const AACS_HDDVD *, uint32_t id, uint8_t key[16]);

/**
 * Decrypt a 2048-byte EVOB pack in place: bytes 0x80..0x7ff with AES-128-CBC
 * (AACS IV), key AES-G(title key, pack bytes 0x54..0x57 + seed12), seed12
 * coming from the EVOB's navigation pack. The pack's scrambling bits are
 * left to the caller.
 * @return 0, < 0 on a cryptography error
 */
AACS_PUBLIC int aacs_hddvd_decrypt_pack(const uint8_t title_key[16], const uint8_t seed12[12], uint8_t *pack);

/*
 * Blu-ray with the application's own file reading
 */

typedef struct aacs_bd AACS_BD;

/* The same messages and levels as the HD DVD open. */
typedef AACS_HDDVD_LOG AACS_BD_LOG;
#define AACS_BD_LOG_ERROR   AACS_HDDVD_LOG_ERROR
#define AACS_BD_LOG_WARNING AACS_HDDVD_LOG_WARNING
#define AACS_BD_LOG_INFO    AACS_HDDVD_LOG_INFO

/**
 * The CPS unit keys of a Blu-ray disc. disc_id: the disc ID the key files use
 * (SHA-1 of its Unit_Key_RO.inf); enc_uk: the nb_uk (>= 1) encrypted unit
 * keys of that file; mkb: its media key block (mkb_name for the log), or
 * NULL. The key files (KEYDB.cfg, with or without "0x" before the hex
 * values) are read in order; of their entries for the disc ID the first
 * volume unique key decrypts the unit keys; without one, the first unit-key
 * list (U, every unit 1..nb_uk) is used; else the media key (the first
 * entry's, or from the MKB with the files' device keys, then processing keys)
 * and the first entry's volume ID give the volume unique key.
 * @return the handle, or NULL with *error_code an AACS_ERROR_* code
 */
AACS_PUBLIC AACS_BD *aacs_bd_open(const uint8_t disc_id[20], const uint8_t (*enc_uk)[16], unsigned nb_uk,
                                  const uint8_t *mkb, size_t mkb_size, const char *mkb_name,
                                  const char *const *key_files, unsigned nb_key_files,
                                  AACS_BD_LOG log, void *log_opaque, int *error_code);
AACS_PUBLIC void aacs_bd_close(AACS_BD *);

/* Number of CPS units; unit n's key (1..n): 1, else 0. */
AACS_PUBLIC unsigned aacs_bd_units(const AACS_BD *);
AACS_PUBLIC int aacs_bd_unit_key(const AACS_BD *, unsigned unit, uint8_t key[16]);

/**
 * Decrypt a 6144-byte aligned unit in place: bytes 16..6143 with
 * AES-128-CBC (AACS IV), key AES-G(unit key, the unit's first 16 bytes).
 * Neither the result nor the copy permission bits are checked or changed.
 * @return 0, < 0 on a cryptography error
 */
AACS_PUBLIC int aacs_bd_decrypt_unit(const uint8_t unit_key[16], uint8_t *unit);

/*
 * Debug log: every message libaacs would write (to stderr or AACS_DEBUG_FILE,
 * as AACS_DEBUG_MASK selects) goes to handler instead; NULL restores that.
 */
typedef void (*AACS_DEBUG_HANDLER)(const char *message);
AACS_PUBLIC void aacs_set_debug_handler(AACS_DEBUG_HANDLER handler);

#endif /* AACS_H_ */
