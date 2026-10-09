/*
 * Blu-ray: AACS. The keys come from libaacs (key files); this file follows
 * the reference program's rules for the disc's AACS files, which clip uses
 * which unit key and which aligned units are decrypted.
 *
 * Disc open (after the playlists, index and disc id):
 * - /AACS (else /AACS_bd): Unit_Key_RO.inf and MKB_RO.inf, each from that
 *   directory or its DUPLICATE copy. The MKB's type (Type and Version
 *   record) gives the size of a unit key record (64 bytes for the types
 *   0x141003 and 0x151003, else 48). Unit_Key_RO.inf: key block at BE32 0,
 *   header bytes 0x10.. (byte 0x10 = 1 and 0x11 = 1 for a pressed disc;
 *   byte 0x12 non-zero = SKB, not supported), 1..2000 CPS units, unit n's
 *   encrypted key at key block + 16 + (n - 1) * record + 0x20. All keys zero,
 *   or an MKB whose first 2048 bytes are missing or zero: the disc is taken
 *   as not encrypted. Else its disc ID is the SHA-1 of Unit_Key_RO.inf, the
 *   key files must give the unit keys (else the disc cannot be opened), a
 *   disc with one CPS unit uses it for every clip, and with more the three
 *   clips with the most source packets each get the unit whose key decrypts
 *   one of their encrypted units; the other clips find theirs on their first
 *   encrypted unit.
 * - No /AACS, /AACS_bd, /AACS2_av or /AACS2_mv: not encrypted. AVCHD maker
 *   data ("PLEX") is accepted silently; /SLY! or /ANY!, a directory holding MKB_RO.inf or Unit_Key_RO.inf, or a BACKUP
 *   playlist 1..16 bytes longer than its main copy are signs of a disc
 *   decrypted by other software (logged as a warning; reading goes on). The
 *   disc ID is the SHA-1 of the first Unit_Key file found where such
 *   software leaves it.
 * Recordable discs' AACS (Unit_Key_RW.inf, /AACS2_av, /AACS2_mv), BD+ and
 * the per-segment keys of .fmts clips are not supported yet.
 *
 * Aligned unit (6144 bytes = 32 source packets of 192 bytes): byte 0's top
 * two bits (copy permission indicator of the first TP_extra_header) mark it
 * encrypted; bytes 16..6143 are encrypted with AES-128-CBC; after
 * decryption every packet's TS sync byte (byte 4 of each 192) is 0x47.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <libaacs/aacs.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/macros.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/sha.h"

#include "bluray_internal.h"

#define UNIT        6144
#define PACKET      192
#define MAX_UNITS   2000
#define SAMPLES     3          /* clips whose key is found at the open */
#define PROBES      128        /* units looked at per sample clip, twice */

struct BlurayAACS {
    AACS_BD *keys;
    int      nb_units;
};

static int has_dir(DiscIOFS *fs, const char *path)
{
    return !fs->ops->find_dir(fs, path);
}

/* path read whole: 1, 0 missing, < 0 cannot be read */
static int load_path(DiscIOFS *fs, const char *path, int64_t size_wanted, BlurayFile *out)
{
    DiscIOFile *f;
    int64_t size;
    int ret;

    memset(out, 0, sizeof(*out));
    if ((ret = fs->ops->open_file(fs, path, &f)) < 0)
        return ret == AVERROR(ENOENT) ? 0 : ret;
    size = size_wanted >= 0 ? size_wanted : f->size;
    if (size > f->size || size > INT32_MAX) {
        ff_discio_file_free(&f);
        return AVERROR_INVALIDDATA;
    }
    if (!(out->data = av_malloc(size + 1))) {
        ff_discio_file_free(&f);
        return AVERROR(ENOMEM);
    }
    out->size = size;
    ret = ff_discio_file_read(fs, f, 0, out->data, size);
    ff_discio_file_free(&f);
    if (ret < 0) {
        ff_bluray_file_free(out);
        return ret;
    }
    snprintf(out->path, sizeof(out->path), "%s", path);
    return 1;
}

/* An AACS file: /AACS/<name>, else /AACS_bd/<name>; when that cannot be
 * read, the DUPLICATE copy of the directory it was found in (/AACS when it
 * was found in neither), read with the first copy's size.
 * @return 1, 0 no copy exists, < 0 no copy can be read */
static int load_aacs_file(void *log, DiscIOFS *fs, const char *name, int quiet, BlurayFile *out)
{
    const char *dir = "AACS";
    int64_t size = -1;
    char path[96];
    DiscIOFile *f;
    int ret, err = 0;

    snprintf(path, sizeof(path), "/AACS/%s", name);
    if ((ret = fs->ops->open_file(fs, path, &f)) == AVERROR(ENOENT)) {
        snprintf(path, sizeof(path), "/AACS_bd/%s", name);
        if ((ret = fs->ops->open_file(fs, path, &f)) == AVERROR(ENOENT))
            ret = 1;
        else
            dir = "AACS_bd";
    }
    if (ret <= 0) {
        if (!ret) {
            size = f->size;
            ff_discio_file_free(&f);
            if ((ret = load_path(fs, path, size, out)) > 0)
                return 1;
        }
        err = ret;
        av_log(log, AV_LOG_WARNING, "Blu-ray: AACS: %s cannot be read (%s)\n", path, av_err2str(ret));
    }
    snprintf(path, sizeof(path), "/%s/DUPLICATE/%s", dir, name);
    if ((ret = load_path(fs, path, size, out)) > 0) {
        av_log(log, AV_LOG_VERBOSE, "Blu-ray: AACS: %s read from %s\n", name, path);
        return 1;
    }
    if (ret < 0) {
        av_log(log, AV_LOG_WARNING, "Blu-ray: AACS: %s cannot be read (%s)\n", path, av_err2str(ret));
        return ret;
    }
    if (err < 0)
        return err;
    if (!quiet)
        av_log(log, AV_LOG_VERBOSE, "Blu-ray: AACS: no %s\n", name);
    return 0;
}

static void sha1(const uint8_t *data, uint32_t size, uint8_t digest[20])
{
    struct AVSHA *s = av_sha_alloc();

    memset(digest, 0, 20);
    if (!s)
        return;
    av_sha_init(s, 160);
    av_sha_update(s, data, size);
    av_sha_final(s, digest);
    av_free(s);
}

static void set_disc_id(void *log, BlurayDisc *d, const BlurayFile *f)
{
    char hex[41];

    sha1(f->data, f->size, d->disc_id);
    d->has_disc_id = 1;
    snprintf(d->disc_id_from, sizeof(d->disc_id_from), "%s", f->path);
    for (int i = 0; i < 20; i++)
        snprintf(hex + 2 * i, 3, "%02X", d->disc_id[i]);
    av_log(log, AV_LOG_VERBOSE, "Blu-ray: disc ID %s (SHA-1 of %s)\n", hex, f->path);
}

static void set_keys(BlurayDisc *d, int unit)
{
    for (int i = 0; i < d->nb_clips; i++)
        d->clips[i].key_unit = unit;
}

/* ---- a disc without AACS directories ---- */

static int avchd_maker_data(const BlurayDisc *d)
{
    const BlurayPlaylist *pl;

    if (d->bdav || !d->nb_playlists)
        return 0;
    pl = &d->playlists[0];
    for (int i = 0; i < pl->nb_ext; i++) {
        const BlurayExtEntry *e = &pl->ext[i];
        if (e->id1 == 0x1000 && (e->id2 | 0x10) == 0x110 && e->copied >= 4 && !memcmp(e->data, "PLEX", 4))
            return 1;
    }
    return 0;
}

typedef struct DirList {
    char **names;
    int    nb;
    int    found;
    int    err;
} DirList;

static int root_entry(void *opaque, const char *name, int is_dir)
{
    DirList *l = opaque;
    char *n;

    if (!is_dir || strlen(name) >= 30 || !strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    if (!(n = av_asprintf("/%s", name)) || av_dynarray_add_nofree(&l->names, &l->nb, n) < 0) {
        av_free(n);
        return l->err = AVERROR(ENOMEM);
    }
    return 0;
}

static int key_file_entry(void *opaque, const char *name, int is_dir)
{
    DirList *l = opaque;

    if (!is_dir && (!strcmp(name, "MKB_RO.inf") || !strcmp(name, "Unit_Key_RO.inf")))
        l->found = 1;
    return l->found;
}

static void decrypted_warning(void *log, const char *software, const char *why)
{
    av_log(log, AV_LOG_WARNING, "Blu-ray: the disc looks decrypted by %s (%s); the disc is read as it is "
           "(errors may follow)\n", software, why);
}

/* Signs of a disc decrypted by other software: 0, < 0 error */
static int decrypted_checks(void *log, BlurayDisc *d)
{
    DiscIOFS *fs = d->fs;
    DirList root = { 0 };
    int ret;

    if ((ret = fs->ops->list_dir(fs, "/", root_entry, &root)) < 0 || root.err < 0) {
        ret = ret < 0 ? ret : root.err;
        av_log(log, AV_LOG_ERROR, "Blu-ray: the root directory cannot be listed (%s)\n", av_err2str(ret));
        goto out;
    }
    for (int i = 0; i < root.nb; i++) {
        DirList l = { 0 };
        fs->ops->list_dir(fs, root.names[i], key_file_entry, &l);
        if (l.found) {
            char why[80];
            snprintf(why, sizeof(why), "AACS key files in %s", root.names[i]);
            decrypted_warning(log, "on-the-fly decryption software", why);
            goto out;
        }
    }
    for (int i = 0; i < d->nb_playlist_ids; i++) {
        char main_path[48], backup_path[48];
        BlurayFile b;
        DiscIOFile *f;
        int64_t main_size;

        snprintf(main_path, sizeof(main_path), "/BDMV/PLAYLIST/%05u.mpls", d->playlist_ids[i]);
        snprintf(backup_path, sizeof(backup_path), "/BDMV/BACKUP/PLAYLIST/%05u.mpls", d->playlist_ids[i]);
        if (fs->ops->open_file(fs, main_path, &f) < 0)
            continue;
        main_size = f->size;
        ff_discio_file_free(&f);
        if (load_path(fs, backup_path, -1, &b) <= 0)
            continue;
        if (b.size > main_size && b.size - main_size <= 16) {
            int zero = 1;
            char why[160];
            for (uint32_t k = main_size; k < b.size; k++)
                zero &= !b.data[k];
            snprintf(why, sizeof(why), "%s is %"PRId64" bytes longer than %s", backup_path,
                     b.size - main_size, main_path);
            decrypted_warning(log, zero ? "AnyDVD" : "on-the-fly decryption software", why);
            ff_bluray_file_free(&b);
            goto out;
        }
        ff_bluray_file_free(&b);
    }
out:
    for (int i = 0; i < root.nb; i++)
        av_free(root.names[i]);
    av_free(root.names);
    return ret < 0 ? ret : 0;
}

/* The disc ID of a disc without AACS directories (other software leaves
 * the Unit_Key file in one of these places). */
static void plain_disc_id(void *log, BlurayDisc *d)
{
    static const char *const paths[] = {
        "/AACS/Unit_Key_RO.inf", "/AACS/AACS_av/Unit_Key_RW.inf", "/AACS/AACS_mv/Unit_Key_RW.inf",
        "/AACS_bd/AACS_av/Unit_Key_RW.inf", "/AACS_bd/AACS_mv/Unit_Key_RW.inf",
        "/SLY!/Unit_Key_RO.inf", "/ANY!/Unit_Key_RO.inf",
    };

    for (int i = 0; i < FF_ARRAY_ELEMS(paths); i++) {
        BlurayFile f;
        int ret = load_path(d->fs, paths[i], -1, &f);
        if (!ret)
            continue;
        if (ret > 0)
            set_disc_id(log, d, &f);
        else
            av_log(log, AV_LOG_WARNING, "Blu-ray: %s cannot be read (%s): no disc ID\n", paths[i], av_err2str(ret));
        ff_bluray_file_free(&f);
        return;
    }
}

static int not_encrypted(void *log, BlurayDisc *d)
{
    DiscIOFS *fs = d->fs;
    int ret;

    if (avchd_maker_data(d)) {
        av_log(log, AV_LOG_VERBOSE, "Blu-ray: AVCHD maker data found: the disc is not encrypted "
               "[untested on real discs]\n");
    } else {
        if (has_dir(fs, "/SLY!") || has_dir(fs, "/ANY!"))
            decrypted_warning(log, "AnyDVD", has_dir(fs, "/SLY!") ? "/SLY! directory" : "/ANY! directory");
        else if ((ret = decrypted_checks(log, d)) < 0)
            return ret;
        av_log(log, AV_LOG_VERBOSE, "Blu-ray: no AACS directory: the disc is not encrypted\n");
    }
    set_keys(d, BLURAY_KEY_NONE);
    plain_disc_id(log, d);
    return 0;
}

/* ---- the AACS files ---- */

typedef struct UnitKeys {
    int       nb;                  /* CPS units */
    uint8_t (*enc)[16];            /* their encrypted keys */
    int       skb;
} UnitKeys;

/* Size of a unit key record: 64 bytes for the MKB types 0x141003 and
 * 0x151003, else 48. */
static int record_size(void *log, const BlurayFile *mkb)
{
    uint32_t type;

    if (mkb->size < 12 || mkb->data[0] != 0x10) {
        av_log(log, AV_LOG_DEBUG, "Blu-ray: AACS: %s has no Type and Version record first\n", mkb->path);
        return 48;
    }
    type = AV_RB24(mkb->data + 5);
    av_log(log, AV_LOG_VERBOSE, "Blu-ray: AACS: MKB type 0x%06"PRIX32", version %"PRIu32"\n",
           type, AV_RB32(mkb->data + 8));
    return type == 0x141003 || type == 0x151003 ? 64 : 48;
}

/* Unit_Key_RO.inf: 1, 0 not a pressed disc's file, < 0 not readable as one */
static int parse_unit_keys(void *log, const BlurayFile *f, int record, UnitKeys *u)
{
    const uint8_t *p = f->data;
    uint32_t pos, need;
    int type;

    memset(u, 0, sizeof(*u));
    if (f->size < 0x1a) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: %s is too short (%"PRIu32" bytes)\n", f->path, f->size);
        return AVERROR_INVALIDDATA;
    }
    type = p[0x10];
    if (type == 2 || type == 4) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: %s is a recordable disc's unit key file (type %d)\n",
               f->path, type);
        return 0;
    }
    if (type != 1 || p[0x11] != 1) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: %s has unit key file type %d.%d (1.1 expected)\n",
               f->path, type, p[0x11]);
        return AVERROR_INVALIDDATA;
    }
    u->skb = p[0x12] != 0;
    pos = AV_RB32(p);
    if (pos > f->size - 2) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: %s: its key block (at %"PRIu32") lies outside the file\n",
               f->path, pos);
        return AVERROR_INVALIDDATA;
    }
    u->nb = AV_RB16(p + pos);
    if (u->nb < 1 || u->nb > MAX_UNITS) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: %s gives %d CPS units (1..%d)\n", f->path, u->nb, MAX_UNITS);
        return AVERROR_INVALIDDATA;
    }
    need = (uint32_t)u->nb * record;
    if (pos + 16 > f->size || f->size - pos - 16 < need) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: %s: the %d unit key records do not fit in the file\n",
               f->path, u->nb);
        return AVERROR_INVALIDDATA;
    }
    if (!(u->enc = av_malloc_array(u->nb, sizeof(*u->enc))))
        return AVERROR(ENOMEM);
    for (int n = 0; n < u->nb; n++)
        memcpy(u->enc[n], p + pos + 16 + n * record + 0x20, 16);
    av_log(log, AV_LOG_VERBOSE, "Blu-ray: AACS: %s: %d CPS unit(s), %d-byte records%s\n", f->path, u->nb,
           record, u->skb ? ", SKB" : "");
    return 1;
}

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

/* ---- units ---- */

static void clear_marks(uint8_t *unit)
{
    for (int k = 0; k < UNIT / PACKET; k++)
        unit[k * PACKET] &= 0x3f;
}

/* Unit n decrypts a copy of the unit into TS packets (sync byte 0x47 in
 * every packet after the first, whose header is not encrypted). */
static int key_fits(BlurayAACS *a, int n, const uint8_t *unit)
{
    uint8_t copy[UNIT], key[16];

    if (!aacs_bd_unit_key(a->keys, n, key))
        return 0;
    memcpy(copy, unit, UNIT);
    if (aacs_bd_decrypt_unit(key, copy) < 0)
        return 0;
    for (int k = 1; k < UNIT / PACKET; k++)
        if (copy[k * PACKET + 4] != 0x47)
            return 0;
    return 1;
}

/* The unit whose key fits an encrypted unit, or 0. */
static int find_key(BlurayAACS *a, const uint8_t *unit)
{
    for (int n = 1; n <= a->nb_units; n++)
        if (key_fits(a, n, unit))
            return n;
    return 0;
}

static int encrypted(const uint8_t *unit)
{
    return unit[4] == 0x47 && unit[0] >= 0xc0;
}

/* The clips with the most source packets (then the highest numbers) each
 * get the unit whose key decrypts one of their encrypted units: unit indexes
 * evenly spread over the stream file, then its first units. */
static void sample_keys(void *log, BlurayDisc *d, BlurayAACS *a)
{
    uint64_t prev = UINT64_MAX;
    int samples = 0;
    uint8_t unit[UNIT];

    while (samples < SAMPLES) {
        BlurayClip *c = NULL;
        uint64_t best = 0;
        DiscIOFile *f;
        int64_t units;
        int found = 0;

        for (int i = 0; i < d->nb_clips; i++) {
            uint64_t k = (uint64_t)d->clips[i].packets << 32 | d->clips[i].number;
            if (k < prev && (!c || k > best)) {
                c    = &d->clips[i];
                best = k;
            }
        }
        if (!c)
            break;
        prev = best;
        if (!c->stream_path[0] || d->fs->ops->open_file(d->fs, c->stream_path, &f) < 0)
            continue;
        units = f->size / UNIT;
        for (int pass = 0; pass < 2 && !found; pass++) {
            int64_t probes = units > PROBES ? PROBES : pass ? units : 0;
            for (int64_t i = 0; i < probes && !found; i++) {
                int64_t idx = pass ? i : i * units / PROBES;
                if (ff_discio_file_read(d->fs, f, idx * UNIT, unit, UNIT) < 0)
                    continue;
                if (encrypted(unit)) {
                    int n = find_key(a, unit);
                    found = 1;
                    if (n) {
                        c->key_unit = n;
                        av_log(log, AV_LOG_VERBOSE, "Blu-ray: AACS: clip %05u uses CPS unit %d (unit %"PRId64
                               " of %s)\n", c->number, n, idx, c->stream_path);
                    } else {
                        av_log(log, AV_LOG_WARNING, "Blu-ray: AACS: no unit key decrypts unit %"PRId64" of %s\n",
                               idx, c->stream_path);
                    }
                }
            }
        }
        ff_discio_file_free(&f);
        if (found)
            samples++;
    }
}

static void aacs_log(void *opaque, int level, const char *message)
{
    static const int levels[] = { AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_VERBOSE };
    av_log(opaque, levels[level >= 0 && level <= 2 ? level : 2], "Blu-ray: AACS: %s\n", message);
}

static void aacs_debug(const char *message)
{
    av_log(NULL, AV_LOG_DEBUG, "libaacs: %s", message);
}

/* ---- the step ---- */

int ff_bluray_aacs_open(void *log, BlurayDisc *d, const char *const *key_files, int nb_key_files,
                        BlurayAACS **out)
{
    DiscIOFS *fs = d->fs;
    BlurayFile uk = { 0 }, mkb = { 0 };
    UnitKeys u = { 0 };
    BlurayAACS *a = NULL;
    int aacs, av2, mv2, ret, err;

    *out = NULL;
    set_keys(d, BLURAY_KEY_UNKNOWN);
    aacs = has_dir(fs, "/AACS") || has_dir(fs, "/AACS_bd");
    av2  = has_dir(fs, "/AACS2_av");
    mv2  = has_dir(fs, "/AACS2_mv");
    d->aacs = aacs;
    if (!aacs && !av2 && !mv2)
        return not_encrypted(log, d);
    if (av2 || mv2) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS 2 of a recordable disc (%s) is not supported yet "
               "[untested on real discs]\n", av2 ? "/AACS2_av" : "/AACS2_mv");
        return AVERROR_PATCHWELCOME;
    }

    /* the unit keys and the media key block */
    if ((ret = load_aacs_file(log, fs, "Unit_Key_RO.inf", 1, &uk)) < 0)
        goto end;
    if (!ret) {
        if (has_dir(fs, "/AACS/AACS_av") || has_dir(fs, "/AACS/AACS_mv") ||
            has_dir(fs, "/AACS_bd/AACS_av") || has_dir(fs, "/AACS_bd/AACS_mv")) {
            av_log(log, AV_LOG_ERROR, "Blu-ray: AACS of a recordable disc (Unit_Key_RW.inf) is not supported yet "
                   "[untested on real discs]\n");
            ret = AVERROR_PATCHWELCOME;
        } else {
            av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: the disc has no Unit_Key_RO.inf\n");
            ret = AVERROR_INVALIDDATA;
        }
        goto end;
    }
    if ((ret = load_aacs_file(log, fs, "MKB_RO.inf", 0, &mkb)) <= 0) {
        if (!ret) {
            av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: the disc has no MKB_RO.inf\n");
            ret = AVERROR_INVALIDDATA;
        }
        goto end;
    }
    if ((ret = parse_unit_keys(log, &uk, record_size(log, &mkb), &u)) <= 0) {
        if (!ret)
            ret = AVERROR_PATCHWELCOME;
        goto end;
    }

    /* nothing to decrypt after all */
    if (all_zero((const uint8_t *)u.enc, u.nb * sizeof(*u.enc)) || mkb.size < 2048 || all_zero(mkb.data, 2048)) {
        av_log(log, AV_LOG_VERBOSE, "Blu-ray: AACS: %s: the disc is not encrypted\n",
               mkb.size < 2048 || all_zero(mkb.data, 2048) ? "the MKB's first 2048 bytes are missing or zero"
                                                            : "every unit key is zero");
        set_keys(d, BLURAY_KEY_NONE);
        ret = 0;
        goto end;
    }
    set_disc_id(log, d, &uk);
    if (u.nb == 1)
        set_keys(d, 1);
    if (u.skb) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: the disc uses SKB, which is not supported "
               "[untested on real discs]\n");
        ret = AVERROR_PATCHWELCOME;
        goto end;
    }

    /* the keys */
    if (!nb_key_files)
        av_log(log, AV_LOG_WARNING, "Blu-ray: AACS: no key file is set (option keydb)\n");
    if (!(a = av_mallocz(sizeof(*a)))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    aacs_set_debug_handler(aacs_debug);
    a->keys = aacs_bd_open(d->disc_id, (const uint8_t (*)[16])u.enc, u.nb, mkb.data, mkb.size, mkb.path,
                           key_files, nb_key_files, aacs_log, log, &err);
    if (!a->keys) {
        char id[41] = "unknown";
        for (int i = 0; d->has_disc_id && i < 20; i++)
            snprintf(id + 2 * i, 3, "%02X", d->disc_id[i]);
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: the volume key is unknown for this disc (disc ID %s): it "
               "cannot be decrypted (%s)\n", id,
               err == AACS_ERROR_NO_CONFIG ? "no key for it in the key files" :
               err == AACS_ERROR_NO_PK     ? "no key of the key files gives its media key" :
               err == AACS_ERROR_CORRUPTED_DISC ? "its AACS files are not usable" : aacs_error_str(err));
        /* a key is missing (not a file access problem) */
        ret = err == AACS_ERROR_CORRUPTED_DISC ? AVERROR_INVALIDDATA : AVERROR(ENOKEY);
        goto end;
    }
    a->nb_units = u.nb;
    av_log(log, AV_LOG_WARNING, "Blu-ray: AACS: the disc is decrypted with %d unit key(s) "
           "[untested on real discs]\n", u.nb);
    if (u.nb > 1)
        sample_keys(log, d, a);
    *out = a;
    a = NULL;
    ret = 0;
end:
    if (a) {
        aacs_bd_close(a->keys);
        av_free(a);
    }
    av_free(u.enc);
    ff_bluray_file_free(&uk);
    ff_bluray_file_free(&mkb);
    return ret;
}

void ff_bluray_aacs_close(BlurayAACS **pa)
{
    BlurayAACS *a = *pa;

    if (!a)
        return;
    aacs_bd_close(a->keys);
    av_freep(pa);
}

int ff_bluray_aacs_unit(void *log, BlurayAACS *a, BlurayClip *c, uint8_t *unit)
{
    uint8_t key[16];

    if (unit[4] != 0x47 || unit[0] < 0x40)
        return 0;                           /* not a TS unit, or not marked encrypted */
    if (c->key_unit == BLURAY_KEY_UNKNOWN) {
        if (!a) {
            av_log(log, AV_LOG_ERROR, "Blu-ray: clip %05u: an encrypted unit, but the disc has no AACS keys\n",
                   c->number);
            return AVERROR(ENOKEY);
        }
        if (unit[0] < 0xc0) {
            av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: clip %05u: its first marked unit (copy permission %d) "
                   "gives no key\n", c->number, unit[0] >> 6);
            return AVERROR_INVALIDDATA;
        }
        if (!(c->key_unit = find_key(a, unit))) {
            c->key_unit = BLURAY_KEY_UNKNOWN;
            av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: clip %05u: no unit key decrypts its first encrypted unit\n",
                   c->number);
            return AVERROR(ENOKEY);
        }
        av_log(log, AV_LOG_VERBOSE, "Blu-ray: AACS: clip %05u uses CPS unit %d\n", c->number, c->key_unit);
    }
    if (c->key_unit == BLURAY_KEY_NONE)
        return 0;                           /* the disc is not encrypted: used as it is */
    if (c->stream_type == 2 && (unit[0] & 0xc0) != 0xc0) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: clip %05u: segment keys (copy permission %d in a .fmts file) are "
               "not supported yet [untested on real discs]\n", c->number, unit[0] >> 6);
        return AVERROR_PATCHWELCOME;
    }
    if (!a || !aacs_bd_unit_key(a->keys, c->key_unit, key) || aacs_bd_decrypt_unit(key, unit) < 0) {
        av_log(log, AV_LOG_ERROR, "Blu-ray: AACS: clip %05u: CPS unit %d cannot decrypt\n", c->number, c->key_unit);
        return AVERROR(ENOKEY);
    }
    clear_marks(unit);
    return 0;
}
