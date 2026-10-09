/*
 * DVD-Video: the format part of the disc layer (disc.h). It reads the
 * disc's structure (dvd_disc.c: the VMG, the title table, the text data
 * and every title set's IFO with its checks) and describes the disc: its
 * label is the volume label (a folder: the VMG's provider identifier), its
 * name the text data's when it has one; its title list (dvd_titles.c) with
 * each title's tracks after its stream pass (dvd_streams.c: captions, audio
 * streams without data). A title's sectors are read through dvd_read.c
 * (the sector check, CSS); the title reader comes next.
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
#include "libavutil/avstring.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "disc.h"
#include "disclang.h"
#include "dvd_internal.h"

typedef struct DVDDemuxContext {
    DVDDisc      disc;
    DVDScan      scan;
    DVDTitleList titles;
} DVDDemuxContext;

static void dvd_close(AVFormatContext *s)
{
    DVDDemuxContext *c = ff_disc_part(s);
    ff_dvd_titles_free(&c->titles);
    ff_dvd_scan_free(&c->scan);
    ff_dvd_disc_free(&c->disc);
}

/* text to the debug log line by line (a log message has a size limit) */
static void debug_lines(AVFormatContext *s, const char *head, const char *text)
{
    const char *line = text, *nl;

    av_log(s, AV_LOG_DEBUG, "%s\n", head);
    for (; line && *line; line = nl + 1) {
        if (!(nl = strchr(line, '\n')))
            break;
        av_log(s, AV_LOG_DEBUG, "%.*s\n", (int)(nl - line), line);
    }
}

/* A DVD-Video code_extension as roles (audio: 2 visually impaired, 3
 * director's comments, 4 alternate director's comments; sub-picture: 2
 * large, 3 children's, 6 / 7 large / children's captions, 9 forced, 13-15
 * director's comments (normal, large, children's)). */
static int audio_roles(unsigned ext)
{
    static const int r[] = { DISC_ROLE_VISUALLY_IMPAIRED, DISC_ROLE_COMMENTARY, DISC_ROLE_ALT_COMMENTARY };
    return ext >= 2 && ext <= 4 ? r[ext - 2] : 0;
}

static int subp_roles(unsigned ext)
{
    static const int r[] = { DISC_ROLE_LARGE, DISC_ROLE_CHILDREN, 0, 0, DISC_ROLE_LARGE, DISC_ROLE_CHILDREN, 0,
                             DISC_ROLE_FORCED, 0, 0, 0, DISC_ROLE_COMMENTARY,
                             DISC_ROLE_COMMENTARY | DISC_ROLE_LARGE, DISC_ROLE_COMMENTARY | DISC_ROLE_CHILDREN };
    return ext >= 2 && ext <= 15 ? r[ext - 2] : 0;
}

/* A language code of the attributes (bytes 2-3) as ISO 639-2; none when it
 * is not a known code (logged). */
static void attr_language(AVFormatContext *s, char lang[4], const uint8_t *b)
{
    char code[3] = { b[0], b[1], 0 };
    const char *iso = code[0] ? ff_disc_lang_code(code) : NULL;
    if (iso)
        av_strlcpy(lang, iso, 4);
    else
        av_log(s, AV_LOG_DEBUG, "DVD: language code bytes %02x %02x are no known language\n", b[0], b[1]);
}

/* The stream details read for a track. */
static void video_details(DiscStreamInfo *k, const DiscStreamInfo *v)
{
    k->width      = v->width;
    k->height     = v->height;
    k->aspect     = v->aspect;
    k->frame_rate = v->frame_rate;
    k->profile    = v->profile;
    k->level      = v->level;
    k->bit_rate   = v->bit_rate;
}

static void audio_details(DiscStreamInfo *k, const DiscStreamInfo *a)
{
    k->channels    = a->channels;
    k->layout      = a->layout;
    k->sample_rate = a->sample_rate;
    k->bits        = a->bits;
    k->bit_rate    = a->bit_rate;
    k->profile     = a->profile;
}

/* The title's tracks: the video, the audio streams and the sub-pictures
 * as its title set's attributes describe them, without the audio streams
 * found without data and without a stream
 * listed again (the same coding and stream number: the first one is kept),
 * then the caption track when the stream pass found captions; each with
 * the details read from it. */
static int describe_tracks(AVFormatContext *s, const DVDTitle *t, DiscTitleInfo *dt)
{
    static const enum AVCodecID codecs[8] = { AV_CODEC_ID_AC3, AV_CODEC_ID_NONE, AV_CODEC_ID_MP2, AV_CODEC_ID_MP2,
                                              AV_CODEC_ID_PCM_DVD, AV_CODEC_ID_NONE, AV_CODEC_ID_DTS,
                                              AV_CODEC_ID_NONE };
    const uint8_t *h = t->vts->header;
    DiscStreamInfo *k;
    int kept[8], nb_kept = 0, sids = 0;

    if (!(k = ff_disc_title_add_stream(dt)))
        return AVERROR(ENOMEM);
    video_details(k, &t->details[0]);
    k->type  = AVMEDIA_TYPE_VIDEO;
    k->codec = (h[DVD_VTS_VIDEO_ATTR] >> 6) == 0 ? AV_CODEC_ID_MPEG1VIDEO :
               (h[DVD_VTS_VIDEO_ATTR] >> 6) == 1 ? AV_CODEC_ID_MPEG2VIDEO : AV_CODEC_ID_NONE;
    for (int i = 0; i < t->nb_audio; i++) {
        const uint8_t *a = h + DVD_VTS_AUDIO_ATTR + 8 * t->audio[i].attr;
        enum AVCodecID codec = codecs[a[0] >> 5];
        int dup = -1;

        if (t->audio_dropped >> i & 1)
            continue;
        for (int j = 0; j < nb_kept && dup < 0; j++) {
            const uint8_t *b = h + DVD_VTS_AUDIO_ATTR + 8 * t->audio[kept[j]].attr;
            if (t->audio[kept[j]].stream == t->audio[i].stream && codecs[b[0] >> 5] == codec)
                dup = kept[j];
        }
        if (dup >= 0) {
            av_log(s, AV_LOG_INFO, "DVD: title %s: audio track %d is stream %d again (as track %d): left out\n",
                   t->name, i, t->audio[i].stream, dup);
            continue;
        }
        kept[nb_kept++] = i;
        if (!(k = ff_disc_title_add_stream(dt)))
            return AVERROR(ENOMEM);
        audio_details(k, &t->details[1 + i]);
        k->type  = AVMEDIA_TYPE_AUDIO;
        k->codec = codec;
        if ((a[0] & 0x0c) == 4)
            attr_language(s, k->language, a + 2);
        k->roles = audio_roles(a[5]);
    }
    for (int i = 0; i < t->nb_subp; i++) {
        const uint8_t *p = h + DVD_VTS_SUBP_ATTR + 6 * t->subp[i].attr;
        if (sids >> t->subp[i].stream & 1) {
            av_log(s, AV_LOG_INFO, "DVD: title %s: sub-picture track %d is stream %d again: left out\n",
                   t->name, i, t->subp[i].stream);
            continue;
        }
        sids |= 1 << t->subp[i].stream;
        if (!(k = ff_disc_title_add_stream(dt)))
            return AVERROR(ENOMEM);
        k->type   = AVMEDIA_TYPE_SUBTITLE;
        k->codec  = AV_CODEC_ID_DVD_SUBTITLE;
        k->width  = t->subp_width;
        k->height = t->subp_height;
        if ((p[0] & 3) == 1)
            attr_language(s, k->language, p + 2);
        k->roles = subp_roles(p[5]);
    }
    if (t->captions) {
        if (!(k = ff_disc_title_add_stream(dt)))
            return AVERROR(ENOMEM);
        k->type  = AVMEDIA_TYPE_SUBTITLE;
        k->codec = AV_CODEC_ID_EIA_608;
        av_strlcpy(k->language, "eng", sizeof(k->language));
        k->bit_rate = t->details[0].bit_rate;       /* carried in the video: its rate */
    }
    return 0;
}

/* The disc info's titles: length (the cells' playback times), size (the
 * title VOB sectors it plays), angle, whether selected, the program chain
 * and the navigation's recording it comes from, its segments' cells (as its
 * "files"), its chapters. */
static int describe_titles(AVFormatContext *s)
{
    DVDDemuxContext *c = ff_disc_part(s);
    int ret;

    for (int i = 0; i < c->titles.nb_titles; i++) {
        DVDTitle *t = c->titles.titles[i];
        DiscTitleInfo *dt = ff_disc_add_title(s);

        if (!dt)
            return AVERROR(ENOMEM);
        dt->duration = (int64_t)t->declared * AV_TIME_BASE;
        dt->angle    = t->nr_of_angles > 1 ? t->angle + 1 : 0;
        dt->selected = !t->short_title;
        if (t->scan)
            snprintf(t->source, sizeof(t->source), "VTS_%02d program chain %d (navigation %s), cells", t->vtsn,
                     t->pgc->number + 1, t->scan->name);
        else
            snprintf(t->source, sizeof(t->source), "VTS_%02d program chain %d, cells", t->vtsn, t->pgc->number + 1);
        dt->source = t->source;
        for (int k = 0; k < t->nb_segments; k++) {
            dt->size += t->segments[k].size;
            if (ff_disc_title_add_file(dt, t->segments[k].cell_map) < 0)
                return AVERROR(ENOMEM);
        }
        for (int k = 0; k < t->nb_chapters; k++) {
            DiscChapterInfo *ch = ff_disc_title_add_chapter(dt);
            if (!ch)
                return AVERROR(ENOMEM);
            ch->start = av_rescale(t->chapters[k].time, AV_TIME_BASE, 90000);
        }
        if ((ret = describe_tracks(s, t, dt)) < 0)
            return ret;
    }
    return 0;
}

/* The stream pass and the stream details of every title; a title whose
 * pass fails (a sector cannot be read, no video data) is left out of the
 * list. */
static int stream_pass(AVFormatContext *s)
{
    DVDDemuxContext *c = ff_disc_part(s);
    DVDTitleList *l = &c->titles;

    for (int i = 0; i < l->nb_titles; i++) {
        int ret = ff_dvd_title_streams(&c->disc, l->titles[i]);
        if (ret >= 0)
            ret = ff_dvd_title_details(&c->disc, l->titles[i], ((DiscContext *)s->priv_data)->opt_details);
        if (ret == AVERROR(ENOMEM))
            return ret;
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "DVD: title %s left out: its streams cannot be read (%s)\n",
                   l->titles[i]->name, av_err2str(ret));
            ff_dvd_title_free(&l->titles[i]);
            memmove(l->titles + i, l->titles + i + 1, (l->nb_titles - i - 1) * sizeof(*l->titles));
            l->nb_titles--;
            i--;
        }
    }
    return 0;
}

static int dvd_open(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    DVDDemuxContext *c = ff_disc_part(s);
    DVDTitleOptions o = { d->opt_dvd_cell_mode, d->opt_dvd_cell_check, d->opt_min_length };
    int folder = !d->image, ret;
    char *dump;

    if ((ret = ff_dvd_disc_open(s, d->fs, d->dvd_flat ? "" : "/VIDEO_TS", folder, folder ? NULL : d->fs->label,
                                &c->disc)) < 0)
        return ret;
    if (av_log_get_level() >= AV_LOG_DEBUG) {
        AVBPrint bp;
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        ff_dvd_disc_dump(&c->disc, &bp);
        if (av_bprint_is_complete(&bp))
            debug_lines(s, "DVD structure:", bp.str);
        av_bprint_finalize(&bp, NULL);
    }
    d->info.label = c->disc.volume_name;
    d->info.name  = c->disc.text_name;

    /* the titles: the navigation played (not for trimmed cells), then the
     * title list from it (a failed scan leaves no results) */
    if ((o.cell_mode != DVD_CELL_MODE_TRIM && ff_dvd_scan(&c->disc, NULL, NULL, &c->scan) == AVERROR(ENOMEM)) ||
        (ret = ff_dvd_titles_build(&c->disc, &c->scan, &o, &c->titles)) < 0 ||
        (ret = stream_pass(s)) < 0 ||
        (ret = describe_titles(s)) < 0)
        return ret < 0 ? ret : AVERROR(ENOMEM);
    if (av_log_get_level() >= AV_LOG_DEBUG && (dump = ff_dvd_titles_dump(&c->titles))) {
        debug_lines(s, "DVD titles:", dump);
        av_free(dump);
    }
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
