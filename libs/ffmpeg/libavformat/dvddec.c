/*
 * DVD-Video: the format part of the disc layer (disc.h). It reads the
 * disc's structure (dvd_disc.c: the VMG, the title table, the text data
 * and every title set's IFO with its checks) and describes the disc: its
 * label is the volume label (a folder: the VMG's provider identifier), its
 * name the text data's when it has one. The title list, CSS, the tracks and
 * the reading of a title come in later steps.
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
#include "disc.h"
#include "dvd_internal.h"

typedef struct DVDDemuxContext {
    DVDDisc disc;
} DVDDemuxContext;

static void dvd_close(AVFormatContext *s)
{
    DVDDemuxContext *c = ff_disc_part(s);
    ff_dvd_disc_free(&c->disc);
}

static int dvd_open(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    DVDDemuxContext *c = ff_disc_part(s);
    int folder = !d->image, ret;

    if ((ret = ff_dvd_disc_open(s, d->fs, d->dvd_flat ? "" : "/VIDEO_TS", folder, folder ? NULL : d->fs->label,
                                &c->disc)) < 0)
        return ret;
    if (av_log_get_level() >= AV_LOG_DEBUG) {
        AVBPrint bp;
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        ff_dvd_disc_dump(&c->disc, &bp);
        if (av_bprint_is_complete(&bp)) {
            /* line by line: a log message has a size limit */
            char *line = bp.str, *nl;
            av_log(s, AV_LOG_DEBUG, "DVD structure:\n");
            for (; *line; line = nl + 1) {
                if (!(nl = strchr(line, '\n')))
                    break;
                av_log(s, AV_LOG_DEBUG, "%.*s\n", (int)(nl - line), line);
            }
        }
        av_bprint_finalize(&bp, NULL);
    }
    d->info.label = c->disc.volume_name;
    d->info.name  = c->disc.text_name;
    if (d->opt_title < 0)               /* only the disc info */
        return 0;
    av_log(s, AV_LOG_ERROR, "DVD: the title list is not built yet: no title can be opened\n");
    return AVERROR_PATCHWELCOME;
}

const DiscFormat ff_disc_dvd = {
    .name        = "DVD-Video",
    .kind        = DISCIO_DISC_DVD,
    .priv_size   = sizeof(DVDDemuxContext),
    .open        = dvd_open,
    .close       = dvd_close,
};
