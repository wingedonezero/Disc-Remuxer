/*
 * HD DVD (Advanced Content): the Advanced VTS information file
 * (HVDVD_TS/HVA00001.VTI). It names the title set's enhanced video objects
 * (EVOB table) and the attributes of their streams (attribute table).
 *
 * Layout (big-endian):
 *   header      0x00 "ADVANCED-VTS"; 0xb8 attribute table, 0xbc EVOB table,
 *               each a sector number (x 2048 = byte position in the file)
 *   table       0x00 record count (attribute table: u16, EVOB table: u32),
 *               0x08 one u32 per record: its byte offset from the table start
 *   attribute   0x206 bytes (see HDDVDEvobAttr)
 *   EVOB        0x140 bytes: 0x002 file name (255 bytes), 0x106 attribute
 *               record (1-based, u32), 0x10a start / 0x10e end presentation
 *               time (90 kHz), 0x112 size in sectors, 0x116 slot (u16)
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "hddvd_internal.h"

#define CHECK_PASS(...) av_log(logctx, AV_LOG_DEBUG, "VTI check " __VA_ARGS__)

static int fail(void *logctx, const char *fmt, ...) av_printf_format(2, 3);

static int fail(void *logctx, const char *fmt, ...)
{
    char msg[256];
    va_list vl;

    va_start(vl, fmt);
    vsnprintf(msg, sizeof(msg), fmt, vl);
    va_end(vl);
    av_log(logctx, AV_LOG_ERROR, "VTI check %s: FAIL; the title set information is not usable\n", msg);
    return AVERROR_INVALIDDATA;
}

static int read_at(void *logctx, HDDVDReadFn read, void *opaque, int64_t pos,
                   uint8_t *buf, int len, const char *what)
{
    int ret = read(opaque, pos, buf, len);
    if (ret < 0)
        av_log(logctx, AV_LOG_ERROR, "VTI: cannot read %s (%d bytes at %"PRId64"): %s\n",
               what, len, pos, av_err2str(ret));
    return ret;
}

static int parse_attrs(void *logctx, HDDVDReadFn read, void *opaque, int64_t table, HDDVDVTI *vti)
{
    uint8_t head[8], rec[HDDVD_VTI_ATTR_SIZE];
    int n, ret;

    if ((ret = read_at(logctx, read, opaque, table, head, sizeof(head), "the attribute table")) < 0)
        return ret;
    n = AV_RB16(head);
    if (n == 0 || n > HDDVD_VTI_MAX_ATTRS)
        return fail(logctx, "attribute record count %d (1..%d)", n, HDDVD_VTI_MAX_ATTRS);
    CHECK_PASS("attribute record count %d: PASS\n", n);

    vti->attrs = av_calloc(n, sizeof(*vti->attrs));
    if (!vti->attrs)
        return AVERROR(ENOMEM);

    for (int i = 0; i < n; i++) {
        HDDVDEvobAttr *a = &vti->attrs[i];
        uint8_t ofs[4];

        if ((ret = read_at(logctx, read, opaque, table + 8 + 4 * i, ofs, 4, "an attribute record offset")) < 0 ||
            (ret = read_at(logctx, read, opaque, table + AV_RB32(ofs), rec, sizeof(rec), "an attribute record")) < 0)
            return ret;
        memcpy(a->raw, rec, sizeof(a->raw));
        a->nb_audio  = AV_RB16(rec + 0x0e);
        a->nb_subpic = AV_RB16(rec + 0xe4);
        if (a->nb_audio > HDDVD_VTI_MAX_AUDIO)
            return fail(logctx, "attribute record %d: audio stream count %d (0..%d)",
                        i + 1, a->nb_audio, HDDVD_VTI_MAX_AUDIO);
        if (a->nb_subpic > HDDVD_VTI_MAX_SUBPIC)
            return fail(logctx, "attribute record %d: sub-picture stream count %d (0..%d)",
                        i + 1, a->nb_subpic, HDDVD_VTI_MAX_SUBPIC);
        for (int j = 0; j < 32; j++)
            a->words[j] = AV_RB32(rec + HDDVD_VTI_ATTR_KEPT + 4 * j);
        CHECK_PASS("attribute record %d at +0x%"PRIx32": %d audio, %d sub-picture streams: PASS\n",
                   i + 1, AV_RB32(ofs), a->nb_audio, a->nb_subpic);
        vti->nb_attrs = i + 1;
    }
    return 0;
}

static int parse_evobs(void *logctx, HDDVDReadFn read, void *opaque, int64_t table, HDDVDVTI *vti)
{
    uint8_t head[8], r[HDDVD_VTI_EVOB_SIZE];
    uint32_t m;
    int ret;

    if ((ret = read_at(logctx, read, opaque, table, head, sizeof(head), "the EVOB table")) < 0)
        return ret;
    m = AV_RB32(head);
    if (m > HDDVD_VTI_MAX_EVOBS)
        return fail(logctx, "EVOB record count %"PRIu32" (0..%d)", m, HDDVD_VTI_MAX_EVOBS);
    CHECK_PASS("EVOB record count %"PRIu32": PASS\n", m);

    for (uint32_t i = 0; i < m; i++) {
        HDDVDEvob *e;
        uint8_t ofs[4];
        uint32_t attr;
        char *dot;

        if ((ret = read_at(logctx, read, opaque, table + 8 + 4 * (int64_t)i, ofs, 4, "an EVOB record offset")) < 0 ||
            (ret = read_at(logctx, read, opaque, table + AV_RB32(ofs), r, sizeof(r), "an EVOB record")) < 0)
            return ret;
        e = av_mallocz(sizeof(*e));
        if (!e)
            return AVERROR(ENOMEM);
        memcpy(e->raw, r, sizeof(r));
        memcpy(e->name, r + 2, 255);           /* 255 bytes, cut there without a NUL */
        e->name[255] = 0;
        memcpy(e->base, e->name, sizeof(e->base));
        if ((dot = strrchr(e->base, '.')))
            *dot = 0;
        attr         = AV_RB32(r + 0x106);
        e->start_ptm = AV_RB32(r + 0x10a);
        e->end_ptm   = AV_RB32(r + 0x10e);
        e->sectors   = AV_RB32(r + 0x112);
        e->slot      = AV_RB16(r + 0x116);
        if (attr == 0 || attr > vti->nb_attrs) {
            ret = fail(logctx, "EVOB record %"PRIu32" (%s): attribute record %"PRIu32" (1..%d)",
                       i + 1, e->name, attr, vti->nb_attrs);
            av_free(e);
            return ret;
        }
        e->attr = attr;
        if (e->slot == 0 || e->slot > HDDVD_VTI_MAX_EVOBS) {
            ret = fail(logctx, "EVOB record %"PRIu32" (%s): slot %d (1..%d)",
                       i + 1, e->name, e->slot, HDDVD_VTI_MAX_EVOBS);
            av_free(e);
            return ret;
        }
        if (vti->evobs[e->slot - 1]) {
            ret = fail(logctx, "EVOB record %"PRIu32" (%s): slot %d already taken by %s",
                       i + 1, e->name, e->slot, vti->evobs[e->slot - 1]->name);
            av_free(e);
            return ret;
        }
        vti->evobs[e->slot - 1] = e;
        vti->nb_evobs = i + 1;
        CHECK_PASS("EVOB record %"PRIu32" at +0x%"PRIx32": %s, attribute record %d, "
                   "time %"PRIu32"..%"PRIu32" (90 kHz), %"PRIu32" sectors, slot %d: PASS\n",
                   i + 1, AV_RB32(ofs), e->name, e->attr, e->start_ptm, e->end_ptm, e->sectors, e->slot);
    }
    return 0;
}

int ff_hddvd_vti_parse(void *logctx, HDDVDReadFn read, void *opaque, HDDVDVTI **out)
{
    uint8_t hdr[HDDVD_VTI_HEADER_SIZE];
    HDDVDVTI *vti;
    int64_t attrs, evobs;
    int ret;

    *out = NULL;
    if ((ret = read_at(logctx, read, opaque, 0, hdr, sizeof(hdr), "the header")) < 0)
        return ret;
    if (memcmp(hdr, "ADVANCED-VTS", 12))
        return fail(logctx, "identifier \"%.12s\" (\"ADVANCED-VTS\")", (const char *)hdr);
    CHECK_PASS("identifier ADVANCED-VTS: PASS\n");

    vti = av_mallocz(sizeof(*vti));
    if (!vti)
        return AVERROR(ENOMEM);
    attrs = (int64_t)AV_RB32(hdr + 0xb8) * 2048;
    evobs = (int64_t)AV_RB32(hdr + 0xbc) * 2048;
    if ((ret = parse_attrs(logctx, read, opaque, attrs, vti)) < 0 ||
        (ret = parse_evobs(logctx, read, opaque, evobs, vti)) < 0) {
        ff_hddvd_vti_free(&vti);
        return ret;
    }
    av_log(logctx, AV_LOG_VERBOSE, "VTI: %d attribute records, %d EVOB records\n",
           vti->nb_attrs, vti->nb_evobs);
    *out = vti;
    return 0;
}

typedef struct FileReader {
    DiscIOFS   *fs;
    DiscIOFile *file;
} FileReader;

static int file_read(void *opaque, int64_t pos, uint8_t *buf, int len)
{
    FileReader *r = opaque;
    return ff_discio_file_read(r->fs, r->file, pos, buf, len);
}

int ff_hddvd_vti_open(void *logctx, DiscIOFS *fs, HDDVDVTI **out)
{
    static const char *const folders[] = { "HVDVD_TS", "HDDVD_TS" };
    FileReader r = { fs };
    int ret = AVERROR(ENOENT);

    *out = NULL;
    for (int i = 0; i < 2; i++) {
        char path[64];

        snprintf(path, sizeof(path), "/%s/HVA00001.VTI", folders[i]);
        ret = fs->ops->open_file(fs, path, &r.file);
        if (ret == AVERROR(ENOENT)) {
            av_log(logctx, AV_LOG_DEBUG, "VTI: no %s\n", path);
            continue;
        }
        if (ret < 0) {
            av_log(logctx, AV_LOG_ERROR, "VTI: cannot open %s: %s\n", path, av_err2str(ret));
            return ret;
        }
        av_log(logctx, AV_LOG_VERBOSE, "VTI: %s, %"PRId64" bytes\n", path, r.file->size);
        ret = ff_hddvd_vti_parse(logctx, file_read, &r, out);
        ff_discio_file_free(&r.file);
        if (ret >= 0)
            memcpy((*out)->folder, folders[i], sizeof((*out)->folder));
        return ret;
    }
    av_log(logctx, AV_LOG_ERROR, "VTI: neither /HVDVD_TS/HVA00001.VTI nor /HDDVD_TS/HVA00001.VTI exists\n");
    return ret;
}

void ff_hddvd_vti_free(HDDVDVTI **pvti)
{
    HDDVDVTI *vti = *pvti;

    if (!vti)
        return;
    for (int i = 0; i < HDDVD_VTI_MAX_EVOBS; i++)
        av_free(vti->evobs[i]);
    av_free(vti->attrs);
    av_freep(pvti);
}
