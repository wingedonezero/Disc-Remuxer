/*
 * Blu-ray: reading the disc's files (main copy or BACKUP copy), listing the
 * numbered files of a directory, extension data blocks
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/err.h"
#include "base/bytes.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "bluray_internal.h"

/* files above this are not loaded whole (clip information with a large EP
 * map stays well below it) */
#define MAX_FILE (64 << 20)

int ff_bluray_read(const BlurayFile *f, uint32_t off, void *buf, uint32_t len)
{
    if ((uint64_t)off + len > f->size)
        return 0;
    memcpy(buf, f->data + off, len);
    return 1;
}

void ff_bluray_header(const uint8_t h[8], BlurayHeader *out)
{
    char v[5];

    memcpy(v, h + 4, 4);
    v[4] = 0;
    out->type    = dr_rb32(h);
    out->version = strtoul(v, NULL, 16);
}

void ff_bluray_file_free(BlurayFile *f)
{
    dr_freep(&f->data);
    f->size = 0;
}

/* A file loaded whole: 1, 0 missing, < 0 a read error; *size: its size
 * when it was found. */
static int load_one(void *log, DiscIOFS *fs, const char *path, BlurayFile *out, int64_t *size)
{
    DiscIOFile *file;
    int ret = fs->ops->open_file(fs, path, &file);

    *size = -1;
    if (ret == DR_ERR(ENOENT))
        return 0;
    if (ret < 0)
        return ret;
    *size = file->size;
    if (file->size > MAX_FILE) {
        dr_vlog(log, DR_VLOG_ERROR, "Blu-ray: %s is %"PRId64" bytes: too large to read\n", path, file->size);
        ff_discio_file_free(&file);
        return DR_ERR_INVALID;
    }
    if (!(out->data = dr_malloc(file->size + 1))) {
        ff_discio_file_free(&file);
        return DR_ERR(ENOMEM);
    }
    out->size = file->size;
    if ((ret = ff_discio_file_read(fs, file, 0, out->data, file->size)) < 0) {
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: %s cannot be read (%s)\n", path, dr_err_text(ret));
        ff_bluray_file_free(out);
    }
    ff_discio_file_free(&file);
    if (ret >= 0)
        snprintf(out->path, sizeof(out->path), "%s", path);
    return ret < 0 ? ret : 1;
}

int ff_bluray_load(void *log, DiscIOFS *fs, const char *root, const char *dir,
                   const char *const *names, const char *const *exts, BlurayFile *out)
{
    int n = 0;

    memset(out, 0, sizeof(*out));
    while (names[n])
        n++;
    for (int k = 0; k < 5 && exts[k]; k++) {
        const char *nm = k < n ? names[k] : names[0];
        char main_path[96], backup_path[96];
        BlurayFile b = { 0 };
        int64_t msize, bsize;
        int m, r;

        snprintf(main_path, sizeof(main_path), "/%s%s/%s.%s", root, dir, nm, exts[k]);
        snprintf(backup_path, sizeof(backup_path), "/%s/BACKUP%s/%s.%s", root, dir, nm, exts[k]);
        m = load_one(log, fs, main_path, out, &msize);
        if (m == DR_ERR(ENOMEM))
            return m;
        if (m == 1)
            return 1;
        /* the main copy is missing, or cannot be read: its BACKUP copy (of
         * the same size when the main copy exists) */
        r = load_one(log, fs, backup_path, &b, &bsize);
        if (r == DR_ERR(ENOMEM))
            return r;
        if (r == 1 && msize >= 0 && bsize != msize) {
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: the backup copy %s is not the size of %s: not used\n",
                   backup_path, main_path);
            ff_bluray_file_free(&b);
            r = 0;
        }
        if (r == 1) {
            if (m < 0)
                dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: %s read from its backup copy %s\n", main_path, backup_path);
            *out = b;
            return 1;
        }
        if (m < 0 || r < 0)
            return m < 0 ? m : r;
    }
    return 0;
}

typedef struct List {
    void     *log;
    const char *dir, *suffix;
    size_t    slen;
    uint32_t *v;
    int       n, cap;
    int       err;
} List;

static int list_entry(void *opaque, const char *name, int is_dir)
{
    List *l = opaque;
    char buf[6];

    if (is_dir)
        return 0;
    /* "NNNNN" + suffix exactly */
    if (strlen(name) != 5 + l->slen || memcmp(name + 5, l->suffix, l->slen))
        return 0;
    for (int i = 0; i < 5; i++)
        if (name[i] < '0' || name[i] > '9') {
            dr_vlog(l->log, DR_VLOG_DEBUG, "Blu-ray: %s/%s is not a numbered file: left out\n", l->dir, name);
            return 0;
        }
    if (l->n == l->cap) {
        uint32_t *v = dr_realloc_array(l->v, l->cap + 64, sizeof(*v));
        if (!v)
            return l->err = DR_ERR(ENOMEM);
        l->v    = v;
        l->cap += 64;
    }
    memcpy(buf, name, 5);
    buf[5] = 0;
    l->v[l->n++] = strtoul(buf, NULL, 10);
    return 0;
}

int ff_bluray_list_numbered(void *log, DiscIOFS *fs, const char *dir, const char *suffix,
                            uint32_t **out, int *nb)
{
    List l = { .log = log, .dir = dir, .suffix = suffix, .slen = strlen(suffix) };
    int ret = fs->ops->list_dir(fs, dir, list_entry, &l);

    *out = NULL;
    *nb  = 0;
    if (ret == DR_ERR(ENOENT))           /* a missing directory is no error */
        ret = 0;
    if (ret < 0 || l.err < 0) {
        dr_free(l.v);
        if (ret >= 0)
            ret = l.err;
        dr_vlog(log, DR_VLOG_ERROR, "Blu-ray: %s cannot be listed (%s)\n", dir, dr_err_text(ret));
        return ret;
    }
    *out = l.v;
    *nb  = l.n;
    return 0;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

void ff_bluray_sort_numbers(uint32_t *v, int n)
{
    qsort(v, n, sizeof(*v), cmp_u32);
}

void ff_bluray_ext_free(BlurayExtEntry **entries, int *nb)
{
    for (int i = 0; i < *nb; i++)
        dr_freep(&(*entries)[i].data);
    dr_freep(entries);
    *nb = 0;
}

int ff_bluray_ext_data(void *log, const BlurayFile *f, uint32_t off,
                       int (*parsed_directly)(uint16_t id1, uint16_t id2),
                       BlurayExtEntry **entries, int *nb)
{
    uint8_t h[12], e[12];
    uint32_t pos;
    int k;

    if (!ff_bluray_read(f, off, h, 4))
        return 0;
    if (!dr_rb32(h))                      /* no extension data */
        return 1;
    if (!ff_bluray_read(f, off, h, 12))
        return 0;
    if (dr_rb32(h + 4) >= dr_rb32(h) || !h[11])
        return 1;                         /* data start not inside the block, or no entries */
    k   = h[11];
    pos = off + 12;
    for (int i = 0; i < k; i++, pos += 12) {
        BlurayExtEntry *x;
        uint32_t n = 0;

        if (!ff_bluray_read(f, pos, e, 12)) {
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: extension entry %d cannot be read\n", f->path, i);
            return 0;
        }
        x = dr_array_add(entries, nb, sizeof(**entries), NULL);
        if (!x)
            return DR_ERR(ENOMEM);
        memset(x, 0, sizeof(*x));
        x->id1    = dr_rb16(e);
        x->id2    = dr_rb16(e + 2);
        x->start  = dr_rb32(e + 4) + off;
        x->length = dr_rb32(e + 8);
        if (!parsed_directly || !parsed_directly(x->id1, x->id2)) {
            n = x->start < f->size ? f->size - x->start : 0;
            n = DR_MIN(n, x->length);
        }
        x->copied = n;
        if (n) {
            if (!(x->data = dr_malloc(n)))
                return DR_ERR(ENOMEM);
            memcpy(x->data, f->data + x->start, n);
        }
    }
    return 1;
}
