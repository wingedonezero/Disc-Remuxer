/*
 * Disc I/O: ISO 9660 / Joliet file systems of disc images (ECMA-119)
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * The reader takes the first primary volume descriptor (and, for Joliet, the
 * first supplementary one), reads directories whole, looks names up exactly
 * and gives every file one extent.
 *
 * Known limits, kept as they are for now (to be decided with tests):
 * - only the first extent of a multi-extent file (flag 0x80) is used;
 * - an extended attribute record is not skipped;
 * - any supplementary volume descriptor is taken as Joliet (its escape
 *   sequence is not checked);
 * - interleaved files are read as contiguous;
 * - directories over 1 MiB are refused; a zero record length at a sector start
 *   ends a directory as corrupt.
 */

#include <inttypes.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discio.h"

#define ISO_MAX_DIR_LEN 0x100000
#define ISO_LABEL_ROOM  161     /* a label longer than this (with its NUL) is empty */
#define ISO_MIN_RECORD  0x21

typedef struct ISOVolume {
    int     joliet;
    uint8_t pvd[DISCIO_BLOCK_SIZE];
    uint8_t svd[DISCIO_BLOCK_SIZE];
} ISOVolume;

typedef struct ISODir {
    uint32_t loc, len;
    uint8_t *buf;
} ISODir;

static const DiscIOFSOps iso9660_ops;
static const DiscIOFSOps joliet_ops;

/* ---- names and labels ---- */

/* Windows-1252 code points of bytes 0x80-0x9F (undefined positions map to
 * themselves); other bytes are Latin-1. */
static const uint16_t cp1252_80[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

/* UTF-16 units to UTF-8 in out (room bytes): a high surrogate followed by a
 * low one is joined, an unpaired one encoded alone; a high surrogate as the
 * last unit, or output that does not fit, gives length 0. NUL units are kept.
 * Returns the number of bytes written (no terminator added). */
static int units_to_utf8(const uint16_t *w, int n, uint8_t *out, int room)
{
    int len = 0;

    for (int i = 0; i < n; ) {
        uint32_t c = w[i++];
        int k;

        if ((c & 0xfc00) == 0xd800) {
            if (i >= n)
                return 0;
            if ((w[i] & 0xfc00) == 0xdc00)
                c = (c << 10) + w[i++] - 0x035fdc00;
        }
        k = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (len + k > room)
            return 0;
        switch (k) {
        case 1: out[len++] = c; break;
        case 2: out[len++] = 0xc0 | c >> 6;  out[len++] = 0x80 | (c & 0x3f); break;
        case 3: out[len++] = 0xe0 | c >> 12; out[len++] = 0x80 | ((c >> 6) & 0x3f);
                out[len++] = 0x80 | (c & 0x3f); break;
        default: out[len++] = 0xf0 | c >> 18; out[len++] = 0x80 | ((c >> 12) & 0x3f);
                 out[len++] = 0x80 | ((c >> 6) & 0x3f); out[len++] = 0x80 | (c & 0x3f); break;
        }
    }
    return len;
}

/* A label from units w[0..n) plus a terminator unit, within the label room,
 * cut at the first NUL. */
static void units_to_label(const uint16_t *w, int n, char *label)
{
    uint16_t tmp[33];
    int len;

    memcpy(tmp, w, n * sizeof(*w));
    tmp[n] = 0;
    len = units_to_utf8(tmp, n + 1, (uint8_t *)label, ISO_LABEL_ROOM);
    /* the last byte written is the terminator unit's NUL; an earlier NUL
     * unit ends the label sooner, as C strings do */
    if (!len)
        label[0] = 0;
}

/* The PVD volume identifier: trailing spaces trimmed, bytes 0x80-0x9F as
 * Windows-1252, other bytes as Latin-1. */
static void pvd_label(const uint8_t *id, char *label)
{
    uint16_t w[32];
    int l = 32;

    while (l > 0 && id[l - 1] == ' ')
        l--;
    for (int i = 0; i < l; i++)
        w[i] = id[i] >= 0x80 && id[i] <= 0x9f ? cp1252_80[id[i] - 0x80] : id[i];
    units_to_label(w, l, label);
}

/* The SVD volume identifier: 16 big-endian UCS-2 units, trailing U+0020
 * trimmed. */
static void svd_label(const uint8_t *id, char *label)
{
    uint16_t w[16];
    int l = 16;

    for (int i = 0; i < 16; i++)
        w[i] = AV_RB16(id + 2 * i);
    while (l > 0 && w[l - 1] == 0x0020)
        l--;
    units_to_label(w, l, label);
}

/* A Joliet file identifier as the lookup compares it: UCS-2 big-endian to
 * UTF-8 (at most 255 bytes, else ""; NUL units kept), a trailing ";1" dropped
 * when longer than 2 bytes; identifiers under 2 bytes give "".
 * Returns the length; out has room for 255 bytes. */
static int joliet_name(const uint8_t *id, int id_len, uint8_t *out)
{
    uint16_t w[128];
    int n = id_len / 2, len;

    if (id_len < 2)
        return 0;
    for (int i = 0; i < n; i++)
        w[i] = AV_RB16(id + 2 * i);
    len = units_to_utf8(w, n, out, 255);
    if (len > 2 && out[len - 2] == ';' && out[len - 1] == '1')
        len -= 2;
    return len;
}

/* ---- directories ---- */

static void dir_free(ISODir *d)
{
    if (d) {
        av_free(d->buf);
        av_free(d);
    }
}

/* Reads a directory whole; NULL when its length is outside 33 bytes - 1 MiB or
 * the read fails (both logged). */
static ISODir *dir_load(DiscIOFS *fs, uint32_t loc, uint32_t len)
{
    DiscIOSource *src = fs->src;
    ISODir *d;
    int ret;

    if (len < ISO_MIN_RECORD || len > ISO_MAX_DIR_LEN) {
        av_log(src->logctx, AV_LOG_DEBUG,
               "%s on '%s': directory at sector %"PRIu32" has length %"PRIu32", outside 33 bytes - 1 MiB\n",
               fs->ops->name, src->name, loc, len);
        return NULL;
    }
    if (!(d = av_mallocz(sizeof(*d))) || !(d->buf = av_malloc(len))) {
        av_free(d);
        return NULL;
    }
    d->loc = loc;
    d->len = len;
    if ((ret = ff_discio_read_bytes(src, (int64_t)loc * DISCIO_BLOCK_SIZE, d->buf, len,
                                    src->attempts, 0)) < 0) {
        av_log(src->logctx, AV_LOG_DEBUG, "%s on '%s': directory at sector %"PRIu32" unreadable\n",
               fs->ops->name, src->name, loc);
        dir_free(d);
        return NULL;
    }
    return d;
}

static ISODir *root_dir(DiscIOFS *fs)
{
    ISOVolume *v = fs->priv;
    const uint8_t *d = v->joliet ? v->svd : v->pvd;

    return dir_load(fs, AV_RL32(d + 158), AV_RL32(d + 166));
}

/* The record named name in dir (offset into its data), or -1. ISO 9660
 * names match exactly or with ";1"; Joliet names after conversion. The first
 * match wins; a corrupt record ends the search with nothing found. */
static int dir_find(DiscIOFS *fs, const ISODir *dir, const char *name, int n)
{
    ISOVolume *v = fs->priv;
    const uint8_t *buf = dir->buf;
    int len = dir->len, off = 0;

    while (off < len) {
        int rl = buf[off], lfi, id_end, avail;
        const uint8_t *id;

        if (!rl) {
            if (off % DISCIO_BLOCK_SIZE == 0) {
                av_log(fs->src->logctx, AV_LOG_DEBUG,
                       "%s: zero record length at a sector start (offset %d) in the directory at sector %"PRIu32"\n",
                       fs->ops->name, off, dir->loc);
                return -1;
            }
            off = (off + DISCIO_BLOCK_SIZE - 1) / DISCIO_BLOCK_SIZE * DISCIO_BLOCK_SIZE;
            continue;
        }
        if (rl < ISO_MIN_RECORD || off + rl > len) {
            av_log(fs->src->logctx, AV_LOG_DEBUG,
                   "%s: bad record length %d at offset %d in the directory at sector %"PRIu32"\n",
                   fs->ops->name, rl, off, dir->loc);
            return -1;
        }
        lfi    = buf[off + 0x20];
        /* the identifier is taken from the directory data as recorded (a
         * malformed length can reach past the record, not past the data);
         * a match needs the recorded length and the bytes present */
        id     = buf + FFMIN(off + 0x21, len);
        id_end = FFMIN(off + 0x21 + lfi, len);
        avail  = id_end - (int)(id - buf);
        if (!v->joliet) {
            if ((lfi == n && avail == n && !memcmp(id, name, n)) ||
                (lfi == n + 2 && avail == n + 2 && !memcmp(id, name, n) && id[n] == ';' && id[n + 1] == '1'))
                return off;
        } else {
            uint8_t conv[255];
            int cl = joliet_name(id, avail, conv);
            if (cl == n && !memcmp(conv, name, n))
                return off;
        }
        off += rl;
    }
    return -1;
}

typedef struct WalkCtx {
    DiscIOFS *fs;
} WalkCtx;

static int walk_subdir(void *ctx, void **dirp, const char *name, int n)
{
    DiscIOFS *fs = ((WalkCtx *)ctx)->fs;
    ISODir *dir = *dirp, *sub;
    int off = dir_find(fs, dir, name, n);

    if (off < 0 || !(dir->buf[off + 0x19] & 2))
        return AVERROR(ENOENT);
    if (!(sub = dir_load(fs, AV_RL32(dir->buf + off + 2), AV_RL32(dir->buf + off + 10))))
        return AVERROR_INVALIDDATA;
    dir_free(dir);
    *dirp = sub;
    return 0;
}

/* The directory holding the last component of path, and that component. */
static int walk_to_parent(DiscIOFS *fs, const char *path, ISODir **parent, const char **last)
{
    WalkCtx ctx = { fs };
    void *dir = root_dir(fs);
    int ret;

    if (!dir)
        return AVERROR_INVALIDDATA;
    if ((ret = ff_discio_walk_path(path, &dir, walk_subdir, &ctx, last)) < 0) {
        dir_free(dir);
        return ret;
    }
    *parent = dir;
    return 0;
}

static int iso_open_file(DiscIOFS *fs, const char *path, DiscIOFile **out)
{
    ISODir *dir;
    const char *last;
    DiscIOFile *f;
    int off, ret, flags;
    uint32_t start, size;

    *out = NULL;
    if ((ret = walk_to_parent(fs, path, &dir, &last)) < 0)
        return ret;
    off = dir_find(fs, dir, last, strlen(last));
    if (off < 0 || ((flags = dir->buf[off + 0x19]) & 2)) {
        dir_free(dir);
        return AVERROR(ENOENT);
    }
    if (flags != 1 && (flags & 1))
        av_log(fs->src->logctx, AV_LOG_DEBUG, "%s: file '%s' is hidden and has other flags (0x%02x)\n",
               fs->ops->name, path, flags);
    start = AV_RL32(dir->buf + off + 2);
    size  = AV_RL32(dir->buf + off + 10);
    dir_free(dir);

    if (!(f = av_mallocz(sizeof(*f))) || !(f->extents = av_mallocz(sizeof(*f->extents)))) {
        av_free(f);
        return AVERROR(ENOMEM);
    }
    f->size              = size;
    f->nb_extents        = 1;
    f->extents[0].sector = start;
    f->extents[0].count  = (size + 0x7ffULL) >> 11;
    *out = f;
    return 0;
}

static int iso_list_dir(DiscIOFS *fs, const char *path, DiscIODirCallback cb, void *opaque)
{
    ISOVolume *v = fs->priv;
    ISODir *dir;
    int ret = 0, off = 0;

    if (!strcmp(path, "/")) {
        if (!(dir = root_dir(fs)))
            return AVERROR_INVALIDDATA;
    } else {
        ISODir *parent;
        const char *last;
        WalkCtx ctx = { fs };
        void *d;

        if ((ret = walk_to_parent(fs, path, &parent, &last)) < 0)
            return ret;
        d = parent;
        ret = walk_subdir(&ctx, &d, last, strlen(last));
        if (ret < 0) {
            dir_free(parent);
            return ret;
        }
        dir = d;
    }

    while (off < (int)dir->len) {
        int rl = dir->buf[off], lfi, id_end, is_dir;
        const uint8_t *id;
        char name[256];

        if (!rl) {
            if (off % DISCIO_BLOCK_SIZE == 0) {
                ret = AVERROR_INVALIDDATA;
                break;
            }
            off = (off + DISCIO_BLOCK_SIZE - 1) / DISCIO_BLOCK_SIZE * DISCIO_BLOCK_SIZE;
            continue;
        }
        if (rl < ISO_MIN_RECORD || off + rl > (int)dir->len) {
            ret = AVERROR_INVALIDDATA;
            break;
        }
        is_dir = (dir->buf[off + 0x19] >> 1) & 1;
        lfi    = dir->buf[off + 0x20];
        id     = dir->buf + FFMIN(off + 0x21, (int)dir->len);
        id_end = FFMIN(off + 0x21 + lfi, (int)dir->len);
        lfi    = id_end - (int)(id - dir->buf);
        if (lfi == 1 && id[0] == 0) {
            strcpy(name, ".");
        } else if (lfi == 1 && id[0] == 1) {
            strcpy(name, "..");
        } else {
            int n;
            if (!v->joliet) {
                n = lfi;
                if (n >= 3 && id[n - 2] == ';' && id[n - 1] == '1')
                    n -= 2;
                memcpy(name, id, n);
            } else {
                n = joliet_name(id, lfi, (uint8_t *)name);
            }
            name[n] = 0;  /* a name holding a NUL is shown up to it (an empty name then) */
            if (!n)
                strcpy(name, "x");  /* only a name of no characters at all is shown as "x" */
        }
        if ((ret = cb(opaque, name, is_dir)))
            break;
        off += rl;
    }
    if (ret == AVERROR_INVALIDDATA)
        av_log(fs->src->logctx, AV_LOG_DEBUG, "%s: corrupt record in the directory at sector %"PRIu32"\n",
               fs->ops->name, dir->loc);
    dir_free(dir);
    return ret;
}

static int iso_find_dir(DiscIOFS *fs, const char *path)
{
    ISODir *dir, *parent;
    const char *last;
    WalkCtx ctx = { fs };
    void *d;
    int ret;

    if (!strcmp(path, "/")) {
        if (!(dir = root_dir(fs)))
            return AVERROR_INVALIDDATA;
        dir_free(dir);
        return 0;
    }
    if ((ret = walk_to_parent(fs, path, &parent, &last)) < 0)
        return ret;
    d = parent;
    if ((ret = walk_subdir(&ctx, &d, last, strlen(last))) < 0) {
        dir_free(parent);
        return ret;
    }
    dir_free(d);
    return 0;
}

static void iso_close(DiscIOFS *fs)
{
    av_freep(&fs->priv);
}

static const DiscIOFSOps iso9660_ops = {
    .name      = "ISO 9660",
    .open_file = iso_open_file,
    .list_dir  = iso_list_dir,
    .close     = iso_close,
    .find_dir  = iso_find_dir,
    .kind      = DISCIO_FS_ISO9660,
};

static const DiscIOFSOps joliet_ops = {
    .name      = "Joliet",
    .open_file = iso_open_file,
    .list_dir  = iso_list_dir,
    .close     = iso_close,
    .find_dir  = iso_find_dir,
    .kind      = DISCIO_FS_JOLIET,
};

int ff_discio_iso9660_mount(DiscIOSource *src, int joliet, DiscIOFS **out)
{
    const char *what = joliet ? "Joliet" : "ISO 9660";
    uint8_t sec[DISCIO_BLOCK_SIZE];
    int pvd_found = 0, svd_found = 0, ret;
    DiscIOFS *fs;
    ISOVolume *v;

    *out = NULL;
    if (!(fs = av_mallocz(sizeof(*fs))) || !(v = av_mallocz(sizeof(*v)))) {
        av_free(fs);
        return AVERROR(ENOMEM);
    }
    fs->ops  = joliet ? &joliet_ops : &iso9660_ops;
    fs->priv = v;
    fs->src  = src;
    v->joliet = joliet;

    for (int s = 16; s < 128; s++) {
        if ((ret = ff_discio_read_blocks(src, (int64_t)s * DISCIO_BLOCK_SIZE, sec, sizeof(sec),
                                         src->attempts, 0)) < 0) {
            av_log(src->logctx, AV_LOG_DEBUG, "%s on '%s': volume descriptor sector %d unreadable\n",
                   what, src->name, s);
            if (pvd_found)
                break;   /* a read failure after the primary descriptor keeps the volume */
            goto fail;
        }
        if (sec[0] == 0xff)
            break;
        if (sec[0] == 1 && !pvd_found) {
            memcpy(v->pvd, sec, sizeof(sec));
            if (memcmp(sec + 1, "CD001", 5)) {
                av_log(src->logctx, AV_LOG_DEBUG, "%s on '%s': the primary volume descriptor has no CD001\n", what, src->name);
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            if (AV_RL16(sec + 0x80) != DISCIO_BLOCK_SIZE) {
                av_log(src->logctx, AV_LOG_DEBUG, "%s on '%s': logical block size is not 2048\n", what, src->name);
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            pvd_found = 1;
            av_log(src->logctx, AV_LOG_DEBUG, "%s on '%s': primary volume descriptor at sector %d\n", what, src->name, s);
            if (!joliet)
                pvd_label(sec + 0x28, fs->label);
            continue;
        }
        if (sec[0] == 2 && !svd_found) {
            svd_found = 1;
            memcpy(v->svd, sec, sizeof(sec));
            if (memcmp(sec + 1, "CD001", 5) || AV_RL16(sec + 0x80) != DISCIO_BLOCK_SIZE) {
                av_log(src->logctx, AV_LOG_DEBUG,
                       "%s on '%s': the supplementary volume descriptor has no CD001 or a block size other than 2048\n",
                       what, src->name);
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            av_log(src->logctx, AV_LOG_DEBUG, "%s on '%s': supplementary volume descriptor at sector %d\n", what, src->name, s);
            if (joliet)
                svd_label(sec + 0x28, fs->label);
        }
    }
    if (!pvd_found || (joliet && !svd_found)) {
        av_log(src->logctx, AV_LOG_DEBUG, "%s on '%s': no %s volume descriptor in sectors 16-127\n",
               what, src->name, pvd_found ? "supplementary" : "primary");
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }
    av_log(src->logctx, AV_LOG_VERBOSE, "%s file system on '%s': label '%s'\n", what, src->name, fs->label);
    *out = fs;
    return 0;

fail:
    av_free(v);
    av_free(fs);
    return ret;
}

int ff_discio_iso9660_creation_date(const DiscIOFS *fs, char date[15])
{
    const ISOVolume *v;

    if (fs->ops != &iso9660_ops && fs->ops != &joliet_ops)
        return AVERROR(EINVAL);
    v = fs->priv;
    memcpy(date, v->pvd + 813, 14);
    date[14] = 0;
    return 0;
}
