/*
 * Disc I/O: what all file systems share (paths, files, reads over extents),
 * and the choice of the file system a disc image is read through
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

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "discio.h"

void ff_discio_fs_close(DiscIOFS **fs)
{
    if (!*fs)
        return;
    if ((*fs)->ops->close)
        (*fs)->ops->close(*fs);
    av_freep(fs);
}

void ff_discio_file_free(DiscIOFile **file)
{
    if (!*file)
        return;
    av_freep(&(*file)->extents);
    av_freep(&(*file)->data);
    av_freep(file);
}

int ff_discio_file_read(DiscIOFS *fs, const DiscIOFile *file, int64_t pos,
                        uint8_t *buf, int len)
{
    return ff_discio_file_read_attempts(fs, file, pos, buf, len, fs->src->attempts, 0);
}

int ff_discio_file_read_attempts(DiscIOFS *fs, const DiscIOFile *file, int64_t pos,
                                 uint8_t *buf, int len, int attempts, int quiet)
{
    DiscIOSource *src = fs->src;
    int64_t ext_start = 0;   /* file offset where the current extent begins */
    int done = 0;

    if (pos < 0 || len < 0 || pos + len > file->size) {
        av_log(src->logctx, AV_LOG_ERROR,
               "Read of %d bytes at %"PRId64" runs past the end of a %"PRId64"-byte file on '%s'\n",
               len, pos, file->size, src->name);
        return AVERROR(EINVAL);
    }
    if (file->data) {
        memcpy(buf, file->data + pos, len);
        return 0;
    }
    for (int i = 0; i < file->nb_extents && done < len; i++) {
        const DiscIOExtent *e = &file->extents[i];
        int64_t ext_bytes = e->count * DISCIO_BLOCK_SIZE;
        int64_t at = pos + done;

        if (at < ext_start + ext_bytes) {
            int64_t in_ext = at - ext_start;
            int n = FFMIN(len - done, ext_bytes - in_ext);

            if (e->sector == DISCIO_SECTOR_NOT_RECORDED) {
                memset(buf + done, 0, n);   /* not recorded: zeros */
            } else {
                int ret = ff_discio_read_bytes(src, e->sector * DISCIO_BLOCK_SIZE + in_ext,
                                               buf + done, n, attempts, quiet);
                if (ret < 0)
                    return ret;
            }
            done += n;
        }
        ext_start += ext_bytes;
    }
    if (done < len) {
        av_log(src->logctx, AV_LOG_ERROR,
               "The extents of a %"PRId64"-byte file on '%s' end before byte %"PRId64"\n",
               file->size, src->name, pos + done);
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

int ff_discio_walk_path(const char *path, void **dir,
                        int (*subdir)(void *ctx, void **dir, const char *name, int name_len),
                        void *ctx, const char **last)
{
    const char *rest = path;

    for (;;) {
        const char *sep = strchr(rest, '/');
        const char *q;
        int ret;

        if (!sep)
            sep = strchr(rest, '\\');
        if (sep != rest)
            return AVERROR(ENOENT);
        rest++;
        q = strchr(rest, '/');
        if (!q)
            q = strchr(rest, '\\');
        if (!q) {
            *last = rest;
            return 0;
        }
        if (q == rest)
            continue;
        if ((ret = subdir(ctx, dir, rest, q - rest)) < 0)
            return ret;
        rest = q;
    }
}

/* ---- the file system of a disc image ---- */

#define UDF_REVISION_102    0x0102
/* UDF 1.02 volumes recorded before this year prefer ISO 9660 (the option) */
#define UDF102_ISO_BEFORE   2006
/* read attempts of the DVD-Video check (fixed, not the source's setting) */
#define DVD_CHECK_ATTEMPTS  5

/* Continuation bytes a UTF-8 lead byte announces (the 5- and 6-byte forms of
 * the original UTF-8 included). */
static int utf8_trail_bytes(uint8_t b)
{
    return b >= 0xfc ? 5 : b >= 0xf8 ? 4 : b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : b >= 0xc0 ? 1 : 0;
}

/* Whether the length bytes at s are one well-formed UTF-8 character (RFC
 * 3629: no overlong forms, no surrogates, nothing above U+10FFFF). As in the
 * classic Unicode conversion code, after ED and F4 only the upper limit of
 * the second byte is checked. */
static int utf8_legal(const uint8_t *s, int length)
{
    uint8_t a;

    if (length < 1 || length > 4)
        return 0;
    for (int k = length - 1; k >= 2; k--)
        if (s[k] < 0x80 || s[k] > 0xbf)
            return 0;
    if (length >= 2) {
        a = s[1];
        if (a > 0xbf)
            return 0;
        switch (s[0]) {
        case 0xe0: if (a < 0xa0) return 0; break;
        case 0xed: if (a > 0x9f) return 0; break;
        case 0xf0: if (a < 0x90) return 0; break;
        case 0xf4: if (a > 0x8f) return 0; break;
        default:   if (a < 0x80) return 0;
        }
    }
    return s[0] < 0x80 || (s[0] >= 0xc2 && s[0] <= 0xf4);
}

void ff_discio_label_copy(char *dst, const char *src)
{
    uint8_t *d = (uint8_t *)dst;
    int i = 0, p;

    if (strlen(src) < strlen(dst) >> 1)
        return;
    memset(dst, 0, DISCIO_LABEL_SIZE);   /* bytes after the label read as NUL */
    av_strlcpy(dst, src, DISCIO_LABEL_SIZE);
    for (;;) {
        int n;

        p = i;
        if (i > DISCIO_LABEL_SIZE - 2 || !d[p])
            break;
        n = utf8_trail_bytes(d[p]) + 1;
        i += n;
        if (i > DISCIO_LABEL_SIZE - 1 || !utf8_legal(d + p, n))
            break;
    }
    d[p] = 0;
}

/* Whether the file at path is one extent (*ext): 1, 0 when it is missing or
 * not one extent (logged), or AVERROR(ENOMEM). */
static int single_extent(DiscIOFS *fs, const char *path, DiscIOExtent *ext)
{
    DiscIOFile *f = NULL;
    int ret = fs->ops->open_file(fs, path, &f);

    if (ret == AVERROR(ENOMEM))
        return ret;
    if (ret < 0) {
        av_log(fs->src->logctx, AV_LOG_DEBUG, "DVD-Video check, %s: %s cannot be opened (%s)\n",
               fs->ops->name, path, av_err2str(ret));
        return 0;
    }
    ret = f->nb_extents == 1;
    if (ret)
        *ext = f->extents[0];
    else
        av_log(fs->src->logctx, AV_LOG_DEBUG, "DVD-Video check, %s: %s is %d extents, not one\n",
               fs->ops->name, path, f->nb_extents);
    ff_discio_file_free(&f);
    return ret;
}

/* The first sector of an IFO file, read from the source. */
static int read_vmg_sector(DiscIOFS *fs, int64_t sector, uint8_t *buf)
{
    if (sector == DISCIO_SECTOR_NOT_RECORDED) {
        av_log(fs->src->logctx, AV_LOG_DEBUG, "DVD-Video check, %s: the IFO's first sector is not recorded\n",
               fs->ops->name);
        return AVERROR_INVALIDDATA;
    }
    return ff_discio_read_blocks(fs->src, sector * DISCIO_BLOCK_SIZE, buf, DISCIO_BLOCK_SIZE,
                                 DVD_CHECK_ATTEMPTS, 1);
}

int ff_discio_dvd_video_check(DiscIOFS *fs)
{
    void *log = fs->src->logctx;
    const char *what = fs->ops->name;
    uint8_t sec[DISCIO_BLOCK_SIZE];
    DiscIOExtent ifo, bup;
    int ret, nts;

    if ((ret = fs->ops->find_dir(fs, "/")) < 0) {
        if (ret == AVERROR(ENOMEM))
            return ret;
        av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: no root directory (%s): fails\n", what, av_err2str(ret));
        return 0;
    }
    if ((ret = fs->ops->find_dir(fs, "/VIDEO_TS")) < 0) {
        if (ret == AVERROR(ENOMEM))
            return ret;
        av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: no VIDEO_TS directory (%s): passes, not a DVD-Video "
               "volume\n", what, av_err2str(ret));
        return 1;
    }
    if ((ret = single_extent(fs, "/VIDEO_TS/VIDEO_TS.IFO", &ifo)) < 0)
        return ret;
    if (!ret || !ifo.count) {
        av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: VIDEO_TS.IFO is missing or not one non-empty extent: "
               "fails\n", what);
        return 0;
    }
    if ((ret = single_extent(fs, "/VIDEO_TS/VIDEO_TS.BUP", &bup)) < 0)
        return ret;
    if (!ret)
        bup = ifo;   /* the BUP is only a second place to read the same sector */
    if (read_vmg_sector(fs, ifo.sector, sec) < 0) {
        if (bup.sector == ifo.sector) {
            av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: VIDEO_TS.IFO sector %"PRId64" unreadable and no "
                   "separate VIDEO_TS.BUP: fails\n", what, ifo.sector);
            return 0;
        }
        if (read_vmg_sector(fs, bup.sector, sec) < 0) {
            av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: VIDEO_TS.IFO sector %"PRId64" and VIDEO_TS.BUP "
                   "sector %"PRId64" unreadable: fails\n", what, ifo.sector, bup.sector);
            return 0;
        }
        av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: VIDEO_TS.IFO sector %"PRId64" unreadable, VIDEO_TS.BUP "
               "sector %"PRId64" checked instead\n", what, ifo.sector, bup.sector);
    }
    if (memcmp(sec, "DVDVIDEO-VMG", 12)) {
        av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: the VMG sector does not start with DVDVIDEO-VMG: fails\n",
               what);
        return 0;
    }
    nts = AV_RB16(sec + 0x3e);   /* vmg_nr_of_title_sets */
    if (nts < 1 || nts > 99) {
        av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: %d title sets (must be 1-99): fails\n", what, nts);
        return 0;
    }
    for (int i = 1; i <= nts; i++) {
        char path[32];
        DiscIOExtent vts;

        snprintf(path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.IFO", i);
        if ((ret = single_extent(fs, path, &vts)) < 0)
            return ret;
        if (!ret || !vts.count) {
            av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: %s is missing or not one non-empty extent: fails\n",
                   what, path + 10);
            return 0;
        }
    }
    av_log(log, AV_LOG_VERBOSE, "DVD-Video check, %s: VIDEO_TS.IFO and %d title set IFO files found: passes\n",
           what, nts);
    return 1;
}

/* No UDF volume: ISO 9660 when it passes the DVD-Video check, else Joliet
 * when it mounts and passes, else ISO 9660 unchecked. */
static int mount_without_udf(DiscIOSource *src, DiscIOFS **out)
{
    DiscIOFS *iso = NULL, *jol = NULL;
    int ret;

    if ((ret = ff_discio_iso9660_mount(src, 0, &iso)) < 0) {
        if (ret == AVERROR(ENOMEM))
            return ret;
        av_log(src->logctx, AV_LOG_ERROR, "'%s' has neither a UDF nor an ISO 9660 file system\n", src->name);
        return AVERROR_INVALIDDATA;
    }
    if ((ret = ff_discio_dvd_video_check(iso)) < 0)
        goto fail;
    if (ret) {
        av_log(src->logctx, AV_LOG_VERBOSE, "Reading '%s' through ISO 9660; label '%s'\n", src->name, iso->label);
        *out = iso;
        return 0;
    }
    if ((ret = ff_discio_iso9660_mount(src, 1, &jol)) == AVERROR(ENOMEM))
        goto fail;
    if (ret >= 0) {
        if ((ret = ff_discio_dvd_video_check(jol)) < 0)
            goto fail;
        if (ret) {
            av_log(src->logctx, AV_LOG_VERBOSE, "Reading '%s' through Joliet (ISO 9660 fails the DVD-Video check); "
                   "label '%s'\n", src->name, jol->label);
            ff_discio_fs_close(&iso);
            *out = jol;
            return 0;
        }
        ff_discio_fs_close(&jol);
    }
    av_log(src->logctx, AV_LOG_VERBOSE, "Reading '%s' through ISO 9660 (no file system passes the DVD-Video check); "
           "label '%s'\n", src->name, iso->label);
    *out = iso;
    return 0;

fail:
    ff_discio_fs_close(&iso);
    ff_discio_fs_close(&jol);
    return ret;
}

int ff_discio_mount_image(DiscIOSource *src, const DiscIOImageOptions *opts, DiscIOFS **out)
{
    static const DiscIOImageOptions defaults = DISCIO_IMAGE_OPTIONS_DEFAULT;
    char label[DISCIO_LABEL_SIZE] = "";
    DiscIOFS *udf = NULL, *iso = NULL;
    int ret, year;

    *out = NULL;
    if (!opts)
        opts = &defaults;
    if (opts->udf_reader != DISCIO_UDF_NETBSD && opts->udf_reader != DISCIO_UDF_LINUX)
        return AVERROR(EINVAL);
    ret = opts->udf_reader == DISCIO_UDF_LINUX ? ff_discio_udf_linux_mount(src, &udf)
                                               : ff_discio_udf_netbsd_mount(src, &udf);
    if (ret == AVERROR(ENOMEM))
        return ret;
    if (ret < 0) {
        av_log(src->logctx, AV_LOG_VERBOSE, "'%s' has no readable UDF file system (%s); trying ISO 9660\n",
               src->name, av_err2str(ret));
        return mount_without_udf(src, out);
    }
    ff_discio_label_copy(label, udf->label);
    if (strcmp(label, udf->label))
        av_log(src->logctx, AV_LOG_WARNING, "UDF label '%s' of '%s' cut to '%s' (not valid UTF-8 after that)\n",
               udf->label, src->name, label);
    memcpy(udf->label, label, sizeof(label));

    if (udf->udf_revision != UDF_REVISION_102) {
        av_log(src->logctx, AV_LOG_VERBOSE, "Reading '%s' through %s (UDF revision 0x%04x); label '%s'\n",
               src->name, udf->ops->name, udf->udf_revision, udf->label);
        *out = udf;
        return 0;
    }
    year = AV_RL16(udf->udf_recording_time + 2);
    if (opts->prefer_iso_for_old_udf102 && year < UDF102_ISO_BEFORE) {
        av_log(src->logctx, AV_LOG_VERBOSE, "'%s' has a UDF 1.02 file system recorded in %d (before %d): ISO 9660 "
               "is preferred when it holds a valid DVD-Video structure\n", src->name, year, UDF102_ISO_BEFORE);
    } else if ((ret = ff_discio_dvd_video_check(udf)) < 0) {
        goto fail;
    } else if (ret) {
        av_log(src->logctx, AV_LOG_VERBOSE, "Reading '%s' through %s (UDF 1.02 recorded in %d, DVD-Video check "
               "passed); label '%s'\n", src->name, udf->ops->name, year, udf->label);
        *out = udf;
        return 0;
    } else {
        av_log(src->logctx, AV_LOG_VERBOSE, "The UDF 1.02 file system of '%s' fails the DVD-Video check; trying "
               "ISO 9660 and Joliet\n", src->name);
    }

    for (int joliet = 0; joliet < 2; joliet++) {
        if ((ret = ff_discio_iso9660_mount(src, joliet, &iso)) == AVERROR(ENOMEM))
            goto fail;
        if (ret < 0)
            continue;
        if ((ret = ff_discio_dvd_video_check(iso)) < 0)
            goto fail;
        if (ret) {
            char own[DISCIO_LABEL_SIZE];

            memcpy(own, iso->label, sizeof(own));
            ff_discio_label_copy(iso->label, udf->label);
            av_log(src->logctx, AV_LOG_VERBOSE, "Reading '%s' through %s instead of UDF 1.02; label '%s' (%s label "
                   "'%s', UDF label '%s')\n", src->name, iso->ops->name, iso->label, iso->ops->name, own,
                   udf->label);
            ff_discio_fs_close(&udf);
            *out = iso;
            return 0;
        }
        ff_discio_fs_close(&iso);
    }
    av_log(src->logctx, AV_LOG_VERBOSE, "Neither ISO 9660 nor Joliet passes the DVD-Video check; reading '%s' through "
           "%s without the check; label '%s'\n", src->name, udf->ops->name, udf->label);
    *out = udf;
    return 0;

fail:
    ff_discio_fs_close(&udf);
    ff_discio_fs_close(&iso);
    return ret;
}

static int has_file(DiscIOFS *fs, const char *path)
{
    DiscIOFile *f = NULL;
    int ret = fs->ops->open_file(fs, path, &f);

    ff_discio_file_free(&f);
    av_log(fs->src->logctx, AV_LOG_DEBUG, "%s: %s\n", path, ret >= 0 ? "found" : av_err2str(ret));
    return ret >= 0;
}

enum DiscIODiscFormat ff_discio_disc_format(DiscIOFS *fs)
{
    if ((has_file(fs, "/BDMV/index.bdmv") || has_file(fs, "/BDMV/INDEX.BDM")) &&
        (has_file(fs, "/BDMV/MovieObject.bdmv") || has_file(fs, "/BDMV/MOVIEOBJ.BDM")))
        return DISCIO_DISC_BLURAY;
    if (has_file(fs, "/BDAV/info.bdav"))
        return DISCIO_DISC_BLURAY;
    if (has_file(fs, "/HVDVD_TS/HVA00001.VTI") || has_file(fs, "/HDDVD_TS/HVA00001.VTI")) {
        if (has_file(fs, "/ADV_OBJ/DISCID.DAT"))
            return DISCIO_DISC_HDDVD;
        av_log(fs->src->logctx, AV_LOG_DEBUG, "HD DVD title set found but no /ADV_OBJ/DISCID.DAT: not an HD DVD\n");
    }
    if (has_file(fs, "/VIDEO_TS/VIDEO_TS.IFO"))
        return DISCIO_DISC_DVD;
    return DISCIO_DISC_NONE;
}
