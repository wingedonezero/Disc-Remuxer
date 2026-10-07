/*
 * Blu-ray (BDMV, BDAV, AVCHD): the format part of the disc layer (disc.h).
 * It reads the disc's structure (bluray_disc.c) and describes the disc;
 * titles, keys and reading follow in their steps.
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

#include <string.h>

#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/log.h"

#include "avformat.h"
#include "bluray_internal.h"
#include "disc.h"

typedef struct BlurayDemuxContext {
    BlurayDisc disc;
} BlurayDemuxContext;

static int has_dir(DiscIOFS *fs, const char *path)
{
    return !fs->ops->find_dir(fs, path);
}

static void bluray_close(AVFormatContext *s)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    ff_bluray_disc_free(&c->disc);
}

static int bluray_open(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    BlurayDemuxContext *c = ff_disc_part(s);
    BlurayDisc *b = &c->disc;
    int ret;

    if ((ret = ff_bluray_disc_open(s, d->fs, s->url, NULL, b)) < 0)
        return ret;
    if (av_log_get_level() >= AV_LOG_DEBUG) {
        AVBPrint bp;
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        ff_bluray_disc_dump(b, &bp);
        if (av_bprint_is_complete(&bp)) {
            /* line by line: a log message has a size limit */
            char *line = bp.str, *nl;
            av_log(s, AV_LOG_DEBUG, "Blu-ray structure:\n");
            for (; *line; line = nl + 1) {
                if (!(nl = strchr(line, '\n')))
                    break;
                av_log(s, AV_LOG_DEBUG, "%.*s\n", (int)(nl - line), line);
            }
        }
        av_bprint_finalize(&bp, NULL);
    }
    d->info.name      = b->name;
    d->info.encrypted = has_dir(d->fs, "/AACS") || has_dir(d->fs, "/AACS_bd") ||
                        has_dir(d->fs, "/AACS2_av") || has_dir(d->fs, "/AACS2_mv");
    if (d->opt_title < 0)
        return 0;
    av_log(s, AV_LOG_ERROR, "Blu-ray: titles are not built yet: only the disc is described\n");
    return AVERROR_PATCHWELCOME;
}

const DiscFormat ff_disc_bluray = {
    .name        = "Blu-ray",
    .kind        = DISCIO_DISC_BLURAY,
    .priv_size   = sizeof(BlurayDemuxContext),
    .open        = bluray_open,
    .close       = bluray_close,
};
