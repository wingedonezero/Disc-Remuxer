/*
 * Blu-ray: the integrity of an AACS disc's stream files by its content hash
 * tables (AACS, Blu-ray Disc pre-recorded: the Content Certificate and the
 * Content Hash Table of each layer, /AACS/Content<NNN>.cer and
 * /AACS/ContentHash<NNN>.tbl).
 *
 * The certificate's first bytes: its type (0x00: the hash values are SHA-1,
 * 0x10: SHA-256), flags, at byte 6 the number of layers, at bytes 8..11 the
 * number of hash values of its layer, at bytes 12..13 the number of the
 * table's index entries. The table: the index entries, 12 bytes each (the
 * first hash value's number, the clip, the clip's first hash unit it covers;
 * big-endian, in clip order), then the hash values, 8 bytes each. A hash
 * unit is 96 sectors (196,608 bytes, 32 aligned units) of a clip's stream
 * file from its start; its value is the last 8 bytes of the digest of those
 * bytes as they are on the disc (still encrypted with the content's keys).
 *
 * The check sees each aligned unit as it is read, before it is decrypted: a
 * hash unit read from its first to its last aligned unit in order is
 * digested and compared; one read in part (a title that starts inside it,
 * a jump) is not checked; the shorter last hash unit of a file is not
 * checked either.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/vlog.h"
#include "base/mem.h"
#include "base/sha.h"

#include "bluray_internal.h"

#define HASH_UNIT   0x30000     /* bytes of a hash unit: 96 sectors */
#define MAX_LAYERS  8
#define LOGGED      5           /* damaged hash units logged per file */

typedef struct HashRun {
    uint32_t       clip, first, count; /**< the clip's hash units first .. first + count - 1 */
    const uint8_t *values;             /**< their values, 8 bytes each */
} HashRun;

typedef struct ClipCheck {
    uint32_t       clip;
    int64_t        expect;             /**< the offset of the next aligned unit of the hash unit, -1 none */
    const uint8_t *want;               /**< the value of the hash unit being read, NULL: not checked */
    DrSha          sha;
    int64_t        failed;
} ClipCheck;

struct BlurayHash {
    int        sha256;
    BlurayFile tables[MAX_LAYERS];
    int        nb_tables;
    HashRun   *runs;
    int        nb_runs;
    ClipCheck *clips;
    int        nb_clips;
    int64_t    checked, failed;        /**< since the last reset */
    int        files_failed;
};

static int run_cmp(const void *a, const void *b)
{
    const HashRun *x = a, *y = b;
    if (x->clip != y->clip)
        return x->clip < y->clip ? -1 : 1;
    return x->first < y->first ? -1 : x->first > y->first;
}

void ff_bluray_hash_free(BlurayHash **ph)
{
    BlurayHash *h = *ph;

    if (!h)
        return;
    for (int i = 0; i < h->nb_tables; i++)
        ff_bluray_file_free(&h->tables[i]);
    dr_free(h->runs);
    dr_free(h->clips);
    dr_freep(ph);
}

/* One layer's index entries into runs. 0, or DR_ERR_INVALID. */
static int add_layer(void *log, BlurayHash *h, const BlurayFile *cer, const BlurayFile *tbl, int layer)
{
    uint32_t nb_values = dr_rb32(cer->data + 8), nb_entries = dr_rb16(cer->data + 12), first;
    const uint8_t *p = tbl->data, *values;
    HashRun *runs;

    if ((uint64_t)nb_entries * 12 + (uint64_t)nb_values * 8 > (uint64_t)tbl->size || !nb_entries) {
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: AACS: %s: %"PRIu32" entries and %"PRIu32" hash values do not fit "
               "its %"PRId64" bytes\n", tbl->path, nb_entries, nb_values, (int64_t)tbl->size);
        return DR_ERR_INVALID;
    }
    if (!(runs = dr_realloc_array(h->runs, h->nb_runs + nb_entries, sizeof(*runs))))
        return DR_ERR(ENOMEM);
    h->runs = runs;
    values  = p + nb_entries * 12;
    first   = dr_rb32(p);
    for (uint32_t i = 0; i < nb_entries; i++) {
        uint32_t idx = dr_rb32(p + 12 * i), next = i + 1 < nb_entries ? dr_rb32(p + 12 * (i + 1)) : first + nb_values;
        if (idx < first || next < idx || next - first > nb_values) {
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: AACS: %s: entry %"PRIu32" is out of order\n", tbl->path, i);
            return DR_ERR_INVALID;
        }
        h->runs[h->nb_runs++] = (HashRun){ dr_rb32(p + 12 * i + 4), dr_rb32(p + 12 * i + 8), next - idx,
                                           values + (uint64_t)(idx - first) * 8 };
    }
    dr_vlog(log, DR_VLOG_VERBOSE, "Blu-ray: AACS: layer %d: %"PRIu32" hash values for %"PRIu32" clip runs\n", layer,
           nb_values, nb_entries);
    return 0;
}

int ff_bluray_hash_open(void *log, DiscIOFS *fs, BlurayHash **out)
{
    BlurayFile cer = { 0 }, more = { 0 };
    BlurayHash *h;
    int layers, ret;
    char name[32];

    *out = NULL;
    if ((ret = ff_bluray_aacs_file(log, fs, "Content000.cer", 1, &cer)) <= 0) {
        dr_vlog(log, ret ? DR_VLOG_WARNING : DR_VLOG_INFO, "Blu-ray: AACS: %s: the stream files are not checked against "
               "the disc's content hashes\n", ret ? "its content certificate cannot be read" :
               "no content hash table");
        return ret;
    }
    if (cer.size < 0x14 || (cer.data[0] != 0x00 && cer.data[0] != 0x10)) {
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: AACS: %s is no content certificate this program knows (type 0x%02x): "
               "the stream files are not checked against the disc's content hashes\n", cer.path,
               cer.size ? cer.data[0] : 0);
        ff_bluray_file_free(&cer);
        return 0;
    }
    if (!(h = dr_mallocz(sizeof(*h)))) {
        ff_bluray_file_free(&cer);
        return DR_ERR(ENOMEM);
    }
    h->sha256 = cer.data[0] != 0;
    layers = DR_MIN(DR_MAX(cer.data[6], 1), MAX_LAYERS);
    for (int l = 0; l < layers; l++) {
        const BlurayFile *c = &cer;
        if (l) {
            snprintf(name, sizeof(name), "Content%03d.cer", l);
            ff_bluray_file_free(&more);
            if ((ret = ff_bluray_aacs_file(log, fs, name, 0, &more)) <= 0 || more.size < 0x14) {
                ret = ret < 0 ? ret : DR_ERR_INVALID;
                break;
            }
            c = &more;
        }
        snprintf(name, sizeof(name), "ContentHash%03d.tbl", l);
        if ((ret = ff_bluray_aacs_file(log, fs, name, 0, &h->tables[h->nb_tables])) <= 0) {
            ret = ret < 0 ? ret : DR_ERR_INVALID;
            break;
        }
        h->nb_tables++;
        if ((ret = add_layer(log, h, c, &h->tables[h->nb_tables - 1], l)) < 0)
            break;
    }
    ff_bluray_file_free(&cer);
    ff_bluray_file_free(&more);
    if (ret < 0) {
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: AACS: the content hash table cannot be loaded (%s): the stream files "
               "are not checked against the disc's content hashes\n", dr_err_text(ret));
        ff_bluray_hash_free(&h);
        return ret == DR_ERR(ENOMEM) ? ret : 0;
    }
    qsort(h->runs, h->nb_runs, sizeof(*h->runs), run_cmp);
    dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: AACS: content hash table loaded (%d layer%s, %s): the stream files read "
           "are checked against it [untested on real discs]\n", layers, layers == 1 ? "" : "s",
           h->sha256 ? "SHA-256" : "SHA-1");
    *out = h;
    return 1;
}

void ff_bluray_hash_reset(BlurayHash *h)
{
    if (!h)
        return;
    h->checked = h->failed = 0;
    h->files_failed = 0;
    for (int i = 0; i < h->nb_clips; i++) {
        h->clips[i].expect = -1;
        h->clips[i].want   = NULL;
        h->clips[i].failed = 0;
    }
}

static ClipCheck *clip_check(BlurayHash *h, uint32_t clip)
{
    ClipCheck *c;

    for (int i = 0; i < h->nb_clips; i++)
        if (h->clips[i].clip == clip)
            return &h->clips[i];
    if (!(c = dr_realloc_array(h->clips, h->nb_clips + 1, sizeof(*c))))
        return NULL;
    h->clips = c;
    c = &h->clips[h->nb_clips++];
    *c = (ClipCheck){ .clip = clip, .expect = -1 };
    return c;
}

/* The value of the clip's hash unit k, NULL when the table has none. */
static const uint8_t *value_of(const BlurayHash *h, uint32_t clip, uint32_t k)
{
    int lo = 0, hi = h->nb_runs - 1;

    while (lo <= hi) {
        int m = (lo + hi) / 2;
        const HashRun *r = &h->runs[m];
        if (r->clip < clip || (r->clip == clip && r->first + r->count <= k))
            lo = m + 1;
        else if (r->clip > clip || r->first > k)
            hi = m - 1;
        else
            return r->values + (uint64_t)(k - r->first) * 8;
    }
    return NULL;
}

int ff_bluray_hash_unit(void *log, BlurayHash *h, uint32_t clip, int64_t off, const uint8_t *unit)
{
    ClipCheck *c;
    uint8_t digest[32];
    int bits = h->sha256 ? 256 : 160;

    if (!(c = clip_check(h, clip)))
        return DR_ERR(ENOMEM);
    if (off % HASH_UNIT == 0) {
        c->want = value_of(h, clip, off / HASH_UNIT);
        if (c->want)
            dr_sha_init(&c->sha, bits);
    } else if (off != c->expect) {
        c->want = NULL;                /* read in part: not checked */
    }
    c->expect = off + BLURAY_UNIT;
    if (!c->want)
        return 0;
    dr_sha_update(&c->sha, unit, BLURAY_UNIT);
    if (off % HASH_UNIT != HASH_UNIT - BLURAY_UNIT)
        return 0;
    dr_sha_final(&c->sha, digest);
    h->checked++;
    if (memcmp(digest + bits / 8 - 8, c->want, 8)) {
        h->failed++;
        h->files_failed += !c->failed;
        if (++c->failed <= LOGGED)
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: %05"PRIu32".m2ts: the %d bytes at offset %"PRId64" do not match "
                   "the disc's content hash: damaged\n", clip, HASH_UNIT, off + BLURAY_UNIT - HASH_UNIT);
        if (c->failed == LOGGED)
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: %05"PRIu32".m2ts: more damaged blocks: not each one is logged\n",
                   clip);
    }
    c->want = NULL;
    return 0;
}

void ff_bluray_hash_stats(const BlurayHash *h, int64_t *checked, int64_t *failed, int *files_failed)
{
    *checked      = h ? h->checked : 0;
    *failed       = h ? h->failed : 0;
    *files_failed = h ? h->files_failed : 0;
}
