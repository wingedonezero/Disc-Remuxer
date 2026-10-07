/*
 * Blu-ray (BDMV, BDAV, AVCHD): the format part of the disc layer (disc.h).
 * It reads the disc's structure (bluray_disc.c), its AACS keys
 * (bluray_aacs.c) and title list (bluray_titles.c) and describes the disc;
 * tracks and reading follow in their steps.
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

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/error.h"
#include "libavutil/log.h"

#include "avformat.h"
#include "bluray_internal.h"
#include "disc.h"

typedef struct BlurayDemuxContext {
    BlurayDisc disc;
    BlurayAACS *aacs;
    char     **files;          /* the titles' file names (disc info) */
    int        nb_files;
} BlurayDemuxContext;

static int has_dir(DiscIOFS *fs, const char *path)
{
    return !fs->ops->find_dir(fs, path);
}

static void bluray_close(AVFormatContext *s)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    for (int i = 0; i < c->nb_files; i++)
        av_free(c->files[i]);
    av_freep(&c->files);
    ff_bluray_aacs_close(&c->aacs);
    ff_bluray_disc_free(&c->disc);
}

static const char *keep(BlurayDemuxContext *c, char *str)
{
    if (!str || av_dynarray_add_nofree(&c->files, &c->nb_files, str) < 0) {
        av_free(str);
        return NULL;
    }
    return str;
}

/* The disc info's titles: name (the title's own, else the disc's), what it
 * is made from, its angle, duration and size (its STC sequences' and their
 * bytes), the clip files it plays (one per segment item), its tracks,
 * chapter marks. */
static int describe_titles(AVFormatContext *s)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    BlurayDisc *b = &c->disc;

    for (int i = 0; i < b->nb_titles; i++) {
        const BlurayTitle *t = &b->titles[i];
        DiscTitleInfo *dt;

        if (!(dt = ff_disc_add_title(s)))
            return AVERROR(ENOMEM);
        dt->name     = t->title_name ? t->title_name : b->name;
        dt->source   = t->name;
        dt->angle    = t->angle;
        dt->duration = av_rescale(ff_bluray_title_duration(t), AV_TIME_BASE, 45000);
        dt->size     = ff_bluray_title_size(t);
        dt->selected = 1;
        av_strlcpy(dt->language, b->name_lang, sizeof(dt->language));
        for (int k = 0; k < t->nb_segs; k++)
            for (int j = 0; j < t->segs[k].nb_items; j++) {
                const BluraySegItem *it = &t->segs[k].items[j];
                const BlurayClip *cl = it->clip;
                const char *f = keep(c, av_asprintf("%05u.%s", cl->number, cl->stream_type == 2 ? "fmts" : "m2ts"));
                if (!f || ff_disc_title_add_file(dt, f) < 0)
                    return AVERROR(ENOMEM);
                /* a stereoscopic item reads its dependent view's file too */
                if (it->dep && t->segs[k].interleaved) {
                    cl = it->dep;
                    f  = keep(c, av_asprintf("%05u.%s", cl->number, cl->stream_type == 2 ? "fmts" : "m2ts"));
                    if (!f || ff_disc_title_add_file(dt, f) < 0)
                        return AVERROR(ENOMEM);
                }
            }
        for (int k = 0; k < t->nb_tracks; k++) {
            const BlurayTrack *tk = &t->tracks[k];
            DiscStreamInfo *ds = ff_disc_title_add_stream(dt);
            if (!ds)
                return AVERROR(ENOMEM);
            ds->type        = tk->type;
            ds->codec       = tk->codec;
            ds->inner_track = tk->inner_track;
            ds->from_track  = tk->from_track;
            ds->forced      = !!(tk->flags & BLURAY_TRACK_FORCED);
            ds->second_view = tk->type == AVMEDIA_TYPE_VIDEO && tk->from_track >= 0;
            ds->offset_sequence = tk->offset_seq;
            ds->second_layer    = !!(tk->flags & BLURAY_TRACK_LAYER2);
            memcpy(ds->language, tk->lang, sizeof(ds->language));
        }
        for (int k = 0; k < t->nb_marks; k++) {
            DiscChapterInfo *ch = ff_disc_title_add_chapter(dt);
            if (!ch)
                return AVERROR(ENOMEM);
            ch->start = av_rescale(t->marks[k], AV_TIME_BASE, 1080000000);
        }
    }
    return 0;
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
    if ((ret = ff_bluray_aacs_open(s, b, (const char *const *)d->opt_keydb, d->nb_opt_keydb, &c->aacs)) < 0)
        return ret;
    b->keys = c->aacs;
    if ((ret = ff_bluray_titles_build(s, b, d->opt_min_length)) < 0 || (ret = describe_titles(s)) < 0)
        return ret;
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
