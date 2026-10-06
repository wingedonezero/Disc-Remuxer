/*
 * HD DVD (Advanced Content): AACS. The keys come from libaacs (title key
 * files, key files); this file finds each EVOB extent's key information in
 * its navigation pack and makes sectors usable.
 *
 * Navigation pack (a pack of fixed layout): pack header, system header
 * (00 00 01 BB, ending at byte 0x29), private stream 2 (00 00 01 BF, length
 * 0x0101, first byte 0x04) whose data from byte 0x30 is kept, a second
 * packet whose data from 0x137 is kept, and private stream 2 at 0x507
 * (length 0x02f3, first byte 0x01). Of the data kept (nav[]): nav[0x0c] top
 * two bits = the mode (0 -> 0, 1 -> 2, 2 -> 1, 3 -> invalid; 1 = decrypt),
 * nav[0x0e] = title key id offset, nav[0x0f] kept, nav[0x10..0x1b] = the 12
 * seed bytes.
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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libaacs/aacs.h>

#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "hddvd_internal.h"

#define NAV_SIZE      0x7c2
#define NAV_SEARCH    10000          /* blocks: the farthest back a navigation pack is looked for */

struct HDDVDAACS {
    AACS_HDDVD *h;
};

/* ---- files of the AACS directory ---- */

typedef struct Reader {
    void     *log;
    DiscIOFS *fs;
} Reader;

/* name from /AACS, else /AACS_BAK: 0 found, 1 neither exists, < 0 unreadable */
static int read_aacs_file(void *opaque, const char *name, uint8_t **data, size_t *size)
{
    static const char *const dirs[] = { "/AACS", "/AACS_BAK" };
    Reader *r = opaque;
    int found = 0, ret = 1;

    for (int i = 0; i < 2; i++) {
        DiscIOFile *f = NULL;
        char path[64];
        uint8_t *buf;

        snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
        if ((ret = r->fs->ops->open_file(r->fs, path, &f)) == AVERROR(ENOENT))
            continue;
        found = 1;
        if (ret < 0) {
            av_log(r->log, AV_LOG_WARNING, "AACS: cannot open %s: %s\n", path, av_err2str(ret));
            continue;
        }
        if (f->size > INT_MAX || !(buf = malloc(f->size ? f->size : 1))) {
            ff_discio_file_free(&f);
            return AVERROR(ENOMEM);
        }
        if ((ret = ff_discio_file_read(r->fs, f, 0, buf, f->size)) < 0) {
            av_log(r->log, AV_LOG_WARNING, "AACS: cannot read %s: %s\n", path, av_err2str(ret));
            free(buf);
            ff_discio_file_free(&f);
            continue;
        }
        av_log(r->log, AV_LOG_VERBOSE, "AACS: %s, %"PRId64" bytes\n", path, f->size);
        *data = buf;
        *size = f->size;
        ff_discio_file_free(&f);
        return 0;
    }
    return found ? AVERROR(EIO) : 1;
}

static void aacs_log(void *opaque, int level, const char *message)
{
    static const int levels[] = { AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_VERBOSE };
    av_log(opaque, levels[level >= 0 && level <= 2 ? level : 2], "AACS: %s\n", message);
}

static void aacs_debug(const char *message)
{
    av_log(NULL, AV_LOG_DEBUG, "libaacs: %s", message);
}

int ff_hddvd_aacs_open(void *logctx, DiscIOFS *fs, const char *const *key_files, int nb_key_files,
                       int nb_playlists, HDDVDAACS **out)
{
    Reader r = { logctx, fs };
    HDDVDAACS *a;
    int err;

    *out = NULL;
    if (fs->ops->find_dir(fs, "/AACS") < 0) {
        av_log(logctx, AV_LOG_VERBOSE, "No /AACS directory: the disc is not encrypted\n");
        return 0;
    }
    if (!nb_key_files)
        av_log(logctx, AV_LOG_WARNING, "AACS: no key file is set (option keydb)\n");
    if (!(a = av_mallocz(sizeof(*a))))
        return AVERROR(ENOMEM);
    aacs_set_debug_handler(aacs_debug);
    a->h = aacs_hddvd_open(read_aacs_file, &r, key_files, nb_key_files, nb_playlists,
                           aacs_log, logctx, &err);
    if (!a->h) {
        av_log(logctx, AV_LOG_ERROR, "AACS: the disc cannot be decrypted (%s)\n",
               err == AACS_ERROR_NO_CONFIG ? "no key for it in the key files" :
               err == AACS_ERROR_NO_PK     ? "no key of the key files gives its media key" :
               err == AACS_ERROR_CORRUPTED_DISC ? "its AACS files are not usable" : aacs_error_str(err));
        av_free(a);
        return err == AACS_ERROR_CORRUPTED_DISC ? AVERROR_INVALIDDATA : AVERROR(EACCES);
    }
    *out = a;
    return 0;
}

void ff_hddvd_aacs_close(HDDVDAACS **pa)
{
    HDDVDAACS *a = *pa;

    if (!a)
        return;
    aacs_hddvd_close(a->h);
    av_freep(pa);
}

/* ---- sectors ---- */

static int pack_header_len(const uint8_t *s)
{
    int stuffing = s[0xd] & 7;
    return 14 + (stuffing && s[0xe] ? stuffing : 0);
}

static int is_pack(const uint8_t *s)
{
    return AV_RB32(s) == 0x000001ba && (s[4] & 0xc0) == 0x40;
}

/* The navigation pack's data (NAV_SIZE bytes) when sec is one. */
static int nav_of(uint8_t *nav, const uint8_t *s)
{
    int h;

    if (!is_pack(s))
        return 0;
    h = pack_header_len(s);
    if (AV_RB32(s + h) != 0x000001bb || h + AV_RB16(s + h + 4) != 0x23 ||
        AV_RB32(s + 0x29) != 0x000001bf || AV_RB32(s + 0x2c) != 0xbf010104)
        return 0;
    memcpy(nav, s + 0x30, 0x100);
    memcpy(nav + 0x100, s + 0x137, 0x3d0);
    if (AV_RB32(s + 0x507) != 0x000001bf || AV_RB32(s + 0x50a) != 0xbf02f301)
        return 0;
    memcpy(nav + 0x4d0, s + 0x50e, 0x2f2);
    return 1;
}

static int key_info(HDDVDExtent *x, const uint8_t *nav)
{
    static const int modes[] = { 0, 2, 1 };
    int m = nav[0x0c] >> 6;

    if (m == 3)
        return 0;
    x->key_mode   = modes[m];
    x->key_offset = nav[0x0e];
    x->key_byte   = nav[0x0f];
    memcpy(x->seed, nav + 0x10, 12);
    x->key_resolved = 1;
    return 1;
}

/* The extent's first EVO block, else up to 9999 blocks back from pos. pos is
 * a position in the EVOB's stream, used as a position in the EVO file (the
 * same for an EVOB whose blocks are in order). */
static int find_nav(void *logctx, DiscIOFS *fs, HDDVDClip *c, const HDDVDExtent *x, int64_t pos, uint8_t *nav)
{
    uint8_t buf[2048];
    int ret;

    if ((ret = ff_discio_file_read(fs, c->evo, (int64_t)x->file * 2048, buf, 2048)) < 0) {
        av_log(logctx, AV_LOG_ERROR, "EVOB %s: cannot read block %"PRIu32" for its navigation pack: %s\n",
               c->evob->name, x->file, av_err2str(ret));
        return 0;
    }
    if (nav_of(nav, buf))
        return 1;
    for (int i = 1; i < NAV_SEARCH; i++) {
        int64_t off = (int64_t)i * 2048;
        if (off >= pos)
            break;
        if ((ret = ff_discio_file_read(fs, c->evo, pos - off, buf, 2048)) < 0) {
            av_log(logctx, AV_LOG_ERROR, "EVOB %s: cannot read byte %"PRId64" looking for a navigation pack: %s\n",
                   c->evob->name, pos - off, av_err2str(ret));
            return 0;
        }
        if (nav_of(nav, buf)) {
            av_log(logctx, AV_LOG_DEBUG, "EVOB %s: navigation pack %d blocks before byte %"PRId64"\n",
                   c->evob->name, i, pos);
            return 1;
        }
    }
    av_log(logctx, AV_LOG_ERROR, "EVOB %s: no navigation pack before byte %"PRId64"\n", c->evob->name, pos);
    return 0;
}

/* The last extent starting at or before block pos >> 11. */
static HDDVDExtent *extent_at(HDDVDClip *c, int64_t pos)
{
    uint32_t blk = (uint32_t)(pos >> 11);
    int lo = 0, hi = c->nb_extents;

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (blk < c->extents[mid].stream)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo ? &c->extents[lo - 1] : NULL;
}

int ff_hddvd_aacs_sector(void *logctx, HDDVDAACS *a, DiscIOFS *fs, HDDVDClip *c,
                         int64_t pos, uint8_t *sec, int search)
{
    uint8_t nav[NAV_SIZE], key[16];
    HDDVDExtent *x;
    uint32_t id;
    int h, sid;

    if (!is_pack(sec))
        return 0;
    h = pack_header_len(sec);
    if (AV_RB24(sec + h) != 1)
        return 0;
    sid = sec[h + 3];
    if (sid != 0xbb && (sid == 0xbf || !(sec[h + 6] & 0x30)))
        return 1;                                   /* nothing to decrypt */
    if (!(x = extent_at(c, pos))) {
        av_log(logctx, AV_LOG_ERROR, "EVOB %s: byte %"PRId64" lies in no extent\n", c->evob->name, pos);
        return 0;
    }
    if (!x->key_resolved) {
        if (nav_of(nav, sec)) {
            if (!key_info(x, nav)) {
                if (search)
                    av_log(logctx, AV_LOG_ERROR, "EVOB %s: navigation pack at byte %"PRId64" has key mode 3\n",
                           c->evob->name, pos);
                return 0;
            }
        } else {
            if (!search)
                return 0;
            if (!find_nav(logctx, fs, c, x, pos, nav))
                return 0;
            if (!key_info(x, nav)) {
                av_log(logctx, AV_LOG_ERROR, "EVOB %s: navigation pack has key mode 3\n", c->evob->name);
                return 0;
            }
        }
    }
    if (sid == 0xbb || sid == 0xbf || !(sec[h + 6] & 0x30))
        return 1;
    if (x->key_mode != 1) {
        av_log(logctx, AV_LOG_ERROR, "EVOB %s: scrambled pack at byte %"PRId64" with key mode %d\n",
               c->evob->name, pos, x->key_mode);
        return 0;
    }
    id = x->key_offset + c->keybase;
    if (!a || !aacs_hddvd_title_key(a->h, id, key)) {
        av_log(logctx, AV_LOG_ERROR, "EVOB %s: scrambled pack at byte %"PRId64" needs title key %"PRIu32", %s\n",
               c->evob->name, pos, id, a ? "which the disc does not have" : "but the disc has no AACS keys");
        return 0;
    }
    if (aacs_hddvd_decrypt_pack(key, x->seed, sec) < 0) {
        av_log(logctx, AV_LOG_ERROR, "EVOB %s: decryption failed at byte %"PRId64"\n", c->evob->name, pos);
        return 0;
    }
    sec[h + 6] &= 0xcf;
    return 1;
}

int ff_hddvd_clip_block(void *logctx, HDDVDAACS *a, DiscIOFS *fs, HDDVDClip *c, uint32_t block, uint8_t *buf)
{
    const HDDVDExtent *x = extent_at(c, (int64_t)block * 2048);
    int ret;

    if (!x || !c->evo || block - x->stream >= x->count) {
        av_log(logctx, AV_LOG_ERROR, "EVOB %s: block %"PRIu32" lies in no extent\n", c->evob->name, block);
        return AVERROR(EINVAL);
    }
    if ((ret = ff_discio_file_read(fs, c->evo, ((int64_t)x->file + block - x->stream) * 2048, buf, 2048)) < 0)
        return ret;
    return ff_hddvd_aacs_sector(logctx, a, fs, c, (int64_t)block * 2048, buf, 1);
}
