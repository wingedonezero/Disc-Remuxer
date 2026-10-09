/*
 * The disc layer: the one demuxer for discs. It opens the source once
 * through the disc readers (an image file or a folder), tells which kind of
 * disc it is from its file system and hands it to that kind's format part
 * (disc.h). The disc's own details (kind, label, file system) go into the
 * disc info (discinfo.h) here; its titles come from the format part. A
 * title's tracks come from the format part in one shape for every format
 * (DiscTrack): the disc info's streams and, for the opened title, its
 * AVStreams and the title reader's tracks are made from them here.
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

#include <limits.h>
#include <stdarg.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/defs.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"

#include "demux.h"
#include "disc.h"
#include "discinfo.h"
#include "discio.h"
#include "disclang.h"
#include "disctl.h"
#include "internal.h"
#include "matroska.h"

/* The format parts, by the disc kind the readers tell. */
static const DiscFormat *const parts[] = {
    &ff_disc_dvd,
    &ff_disc_hddvd,
    &ff_disc_bluray,
    NULL
};

/* A DVD folder opened as the VIDEO_TS folder itself, or a ".dvdmedia"
 * folder, holding VIDEO_TS.IFO at its top. */
static int dvd_flat_folder(DiscIOFS *fs, const char *path)
{
    static const char *const ends[] = { "/VIDEO_TS", ".dvdmedia" };
    size_t n = strlen(path);
    DiscIOFile *f = NULL;
    int named = 0;

    while (n > 1 && path[n - 1] == '/')
        n--;
    for (int i = 0; i < FF_ARRAY_ELEMS(ends); i++)
        named |= n >= strlen(ends[i]) && !av_strncasecmp(path + n - strlen(ends[i]), ends[i], strlen(ends[i]));
    if (!named || fs->ops->open_file(fs, "/VIDEO_TS.IFO", &f) < 0)
        return 0;
    ff_discio_file_free(&f);
    return 1;
}

static const char *kind_name(enum DiscIODiscFormat k)
{
    switch (k) {
    case DISCIO_DISC_DVD:    return "DVD-Video";
    case DISCIO_DISC_BLURAY: return "Blu-ray";
    case DISCIO_DISC_HDDVD:  return "HD DVD";
    default:                 return "no disc";
    }
}

DiscTitleInfo *ff_disc_add_title(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    DiscTitleInfo *t = av_dynarray2_add((void **)&d->info.titles, &d->info.nb_titles, sizeof(*t), NULL);

    if (t) {
        memset(t, 0, sizeof(*t));
        t->index = d->info.nb_titles - 1;
    }
    return t;
}

DiscStreamInfo *ff_disc_title_add_stream(DiscTitleInfo *t)
{
    DiscStreamInfo *k = av_dynarray2_add((void **)&t->streams, &t->nb_streams, sizeof(*k), NULL);
    if (k)
        ff_disc_stream_init(k);
    return k;
}

DiscChapterInfo *ff_disc_title_add_chapter(DiscTitleInfo *t)
{
    DiscChapterInfo *c = av_dynarray2_add((void **)&t->chapters, &t->nb_chapters, sizeof(*c), NULL);
    if (c)
        memset(c, 0, sizeof(*c));
    return c;
}

DiscPartInfo *ff_disc_title_add_part(DiscTitleInfo *t)
{
    DiscPartInfo *p = av_dynarray2_add((void **)&t->parts, &t->nb_parts, sizeof(*p), NULL);
    if (p)
        memset(p, 0, sizeof(*p));
    return p;
}

int ff_disc_title_add_file(DiscTitleInfo *t, const char *name)
{
    return av_dynarray_add_nofree(&t->files, &t->nb_files, (void *)name);
}

static int add_prop(DiscPropertyInfo **props, int *nb, const char *name, const char *fmt, va_list vl)
{
    DiscPropertyInfo *p = av_dynarray2_add((void **)props, nb, sizeof(*p), NULL);
    AVBPrint bp;

    if (!p)
        return AVERROR(ENOMEM);

    p->name = name;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_vbprintf(&bp, fmt, vl);
    return av_bprint_finalize(&bp, &p->value) < 0 ? AVERROR(ENOMEM) : 0;
}

int ff_disc_title_add_prop(DiscTitleInfo *t, const char *name, const char *fmt, ...)
{
    va_list vl;
    int ret;

    va_start(vl, fmt);
    ret = add_prop(&t->props, &t->nb_props, name, fmt, vl);
    va_end(vl);
    return ret;
}

int ff_disc_add_prop(AVFormatContext *s, const char *name, const char *fmt, ...)
{
    DiscContext *d = s->priv_data;
    va_list vl;
    int ret;

    va_start(vl, fmt);
    ret = add_prop(&d->info.props, &d->info.nb_props, name, fmt, vl);
    va_end(vl);
    return ret;
}

void ff_disc_stream_init(DiscStreamInfo *k)
{
    memset(k, 0, sizeof(*k));
    k->inner_track = k->from_track = k->offset_sequence = -1;
    k->profile = AV_PROFILE_UNKNOWN;
    k->level   = AV_LEVEL_UNKNOWN;
}

int ff_disc_title_add_tracks(DiscTitleInfo *t, const DiscTrack *tracks, int nb_tracks)
{
    for (int i = 0; i < nb_tracks; i++) {
        DiscStreamInfo *k = ff_disc_title_add_stream(t);
        if (!k)
            return AVERROR(ENOMEM);
        *k = tracks[i].info;
    }
    return 0;
}

/* ---- the opened title's tracks ---- */

static int add_pid(DiscContext *d, int pid)
{
    for (int i = 0; i < d->src.nb_pids; i++)
        if (d->pids[i] == pid)
            return 0;
    d->pids[d->src.nb_pids++] = pid;
    return 1;
}

/* Where a track's data is, for the log. */
static void track_place(char *buf, size_t size, const DiscTrack *k, const DiscTsFeed *f, int ts)
{
    if (!ts)
        snprintf(buf, size, k->id < 0 ? "carried in the video" : "id 0x%x", k->id);
    else if (f->nb_keys > 1)
        snprintf(buf, size, "PID 0x%04x, keys 0x%04x + 0x%04x", f->pid, f->key[0], f->key[1]);
    else
        snprintf(buf, size, "PID 0x%04x, key 0x%04x%s", f->pid, f->key[0], f->key[0] ? "" : " (any)");
}

/* The stream of a track read: its id, codec, time base, dispositions, what
 * the caller shows of it (metadata), a sub-picture canvas and VobSub
 * header. */
static int track_stream(AVFormatContext *s, const DiscTrack *k, int index, const DiscTsFeed *f, int ts,
                        enum AVCodecID codec, int second_view)
{
    DiscContext *d = s->priv_data;
    AVStream *st = avformat_new_stream(s, NULL);
    int ret;

    if (!st)
        return AVERROR(ENOMEM);
    st->id                   = ts ? f->pid | f->key[0] << 16 : k->id;
    st->codecpar->codec_type = k->info.type;
    st->codecpar->codec_id   = codec;
    avpriv_set_pts_info(st, 64, 1, DR_TICKS_PER_SECOND);
    if (k->info.from_track >= 0)
        st->disposition |= AV_DISPOSITION_DEPENDENT;
    if (k->info.forced)
        st->disposition |= AV_DISPOSITION_FORCED;
    if ((k->line21 && (ret = av_dict_set(&st->metadata, "line21", k->line21, 0)) < 0) ||
        (k->info.language[0] && (ret = av_dict_set(&st->metadata, "language", k->info.language, 0)) < 0) ||
        (ret = av_dict_set_int(&st->metadata, "disc_track", index, 0)) < 0 ||
        (second_view && k->eye && (ret = av_dict_set(&st->metadata, "eye", k->eye, 0)) < 0) ||
        ((k->flags & DISC_TRACK_LAYER2) &&
         (ret = av_dict_set(&st->metadata, "dolby_vision", d->opt_dolby_vision ? "BL" : "BL+EL+RPU", 0)) < 0) ||
        (k->picture && (ret = av_dict_set(&st->metadata, "picture", k->picture, 0)) < 0) ||
        (k->caption && (ret = av_dict_set(&st->metadata, "caption", k->caption, 0)) < 0))
        return ret;
    if (k->kind == DR_KIND_SUBTITLE && k->info.width > 0 && k->info.height > 0) {
        st->codecpar->width  = k->info.width;
        st->codecpar->height = k->info.height;
    }
    if (codec == AV_CODEC_ID_DVD_SUBTITLE) {
        /* the VobSub index header (FFmpeg's DVD subtitle decoder reads size
         * and palette from it) */
        char idx[1024];
        int len = ff_disctl_vobsub_header(idx, sizeof(idx), k->info.width, k->info.height, k->palette);
        if (len < 0 || len >= sizeof(idx))
            return AVERROR_BUG;
        if ((ret = ff_alloc_extradata(st->codecpar, len)) < 0)
            return ret;
        memcpy(st->codecpar->extradata, idx, len);
    }
    return 0;
}

int ff_disc_open_tracks(AVFormatContext *s, const char *title, const DiscTrack *tracks, int nb_tracks, int ts,
                        int second_view, const uint8_t *keep)
{
    DiscContext *d = s->priv_data;
    const char *fmt = d->fmt->log_name;
    int n = 0, layer = -1, caption_left = 0, ret;
    char place[64];

    /* one more for a Dolby Vision enhancement layer (read from its own PID) */
    if (!(d->tracks = av_calloc(nb_tracks + 1, sizeof(*d->tracks))) ||
        (ts && (!(d->feeds = av_calloc(nb_tracks + 1, sizeof(*d->feeds))) ||
                !(d->pids  = av_calloc(nb_tracks + 1, sizeof(*d->pids))))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < nb_tracks; i++) {
        const DiscTrack *k = &tracks[i];
        int view = !!(k->flags & DISC_TRACK_SECOND_VIEW);
        /* a caption track: its cues are text */
        enum AVCodecID codec = k->kind == DR_KIND_CAPTION ? AV_CODEC_ID_SUBRIP : k->info.codec;
        DRTitleTrack *t = &d->tracks[n];

        /* the video and a video with its Dolby Vision layer always stay
         * (the layer is ripped as option dolby_vision says, never left out) */
        if (i && keep && !keep[i] && !(k->flags & DISC_TRACK_LAYER2)) {
            av_log(s, AV_LOG_INFO, "%s: title %s: track %d (%s %s%s%s) is not selected: left out\n", fmt, title, i,
                   av_get_media_type_string(k->info.type), avcodec_get_name(k->info.codec),
                   k->info.language[0] ? ", " : "", k->info.language);
            caption_left |= k->kind == DR_KIND_CAPTION;
            continue;
        }
        if (view && !second_view) {
            av_log(s, AV_LOG_INFO, "%s: title %s: track %d, the 3-D second view (PID 0x%04x), is left out (not "
                   "asked for)\n", fmt, title, i, k->feed.pid);
            continue;
        }
        if (k->not_read) {
            track_place(place, sizeof(place), k, &k->feed, ts);
            av_log(s, AV_LOG_WARNING, "%s: title %s: track %d (%s %s, %s) is not read yet: left out\n", fmt, title,
                   i, av_get_media_type_string(k->info.type), avcodec_get_name(k->info.codec), place);
            continue;
        }
        if (ts) {
            d->feeds[n] = k->feed;
            add_pid(d, k->feed.pid);
        }
        if ((ret = track_stream(s, k, i, &k->feed, ts, codec, second_view)) < 0)
            return ret;
        if (k->untested) {
            av_log(s, AV_LOG_WARNING, "%s: title %s: track %d (%s): %s [untested on real discs]\n", fmt, title, i,
                   avcodec_get_name(k->info.codec), k->untested);
            d->untested++;
        }
        if (k->flags & DISC_TRACK_LAYER2)
            layer = i;
        t->codec       = codec;
        t->kind        = view ? DR_KIND_LAYER : k->kind;
        t->sub_flags   = k->info.forced ? DR_SUB_FORCED_ONLY : 0;
        t->audio_flags = (k->info.from_track >= 0 ? DR_AUDIO_EXTRACTED : 0) |
                         (k->info.from_track >= 0 || k->info.inner_track >= 0 ? DR_AUDIO_EXTENDED : 0);
        if (k->kind == DR_KIND_SUBTITLE) {
            t->width  = k->info.width;
            t->height = k->info.height;
        }
        track_place(place, sizeof(place), k, &k->feed, ts);
        av_log(s, AV_LOG_VERBOSE, "%s: track %d -> stream %d: %s, %s\n", fmt, i, n, avcodec_get_name(codec), place);
        n++;
    }
    /* the Dolby Vision enhancement layer (with its metadata, the RPU): its
     * own PID; merged: a second layer whose pictures go into the video's
     * frames (no stream of its own), separate: a stream of its own */
    if (layer >= 0) {
        const DiscTrack *k = &tracks[layer];
        DRTitleTrack *t = &d->tracks[n];

        if (ts) {
            d->feeds[n] = k->layer_feed;
            add_pid(d, k->layer_feed.pid);
        }
        t->codec       = AV_CODEC_ID_HEVC;
        t->kind        = DR_KIND_LAYER;
        t->layer_flags = d->opt_dolby_vision ? 0 : DR_LAYER_MERGED;
        if (d->opt_dolby_vision) {
            AVStream *st = avformat_new_stream(s, NULL);
            if (!st)
                return AVERROR(ENOMEM);
            st->id                   = ts ? k->layer_feed.pid | k->layer_feed.key[0] << 16 : k->id;
            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_id   = AV_CODEC_ID_HEVC;
            avpriv_set_pts_info(st, 64, 1, DR_TICKS_PER_SECOND);
            if ((k->info.language[0] && (ret = av_dict_set(&st->metadata, "language", k->info.language, 0)) < 0) ||
                (ret = av_dict_set_int(&st->metadata, "disc_track", layer, 0)) < 0 ||
                (ret = av_dict_set(&st->metadata, "dolby_vision", "EL", 0)) < 0)
                return ret;
        }
        av_log(s, AV_LOG_VERBOSE, "%s: track %d: its Dolby Vision enhancement layer, PID 0x%04x, key 0x%04x: %s\n",
               fmt, layer, k->layer_feed.pid, k->layer_feed.key[0],
               d->opt_dolby_vision ? "a stream of its own" : "merged into the video's frames");
        n++;
    }
    /* line-21 caption data the video carries is left out as asked, not lost */
    if (caption_left && s->nb_streams &&
        (ret = av_dict_set(&s->streams[0]->metadata, "caption_track", "not selected", 0)) < 0)
        return ret;
    d->src.ts        = ts;
    d->src.tracks    = d->tracks;
    d->src.nb_tracks = n;
    d->src.feeds     = d->feeds;
    d->src.pids      = d->pids;
    return 0;
}

extern const FFInputFormat ff_disc_demuxer;

const DiscInfo *disc_info(const AVFormatContext *s)
{
    return s && s->iformat == &ff_disc_demuxer.p ? &((const DiscContext *)s->priv_data)->info : NULL;
}

/* The opened title's reader, tracks and the format part's state of it
 * given back (the disc stays loaded). */
static void title_close(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;

    ff_disc_title_reader_close(&d->reader);
    if (d->fmt && d->part && d->fmt->close_title)
        d->fmt->close_title(s);
    av_freep(&d->tracks);
    av_freep(&d->feeds);
    av_freep(&d->pids);
    memset(&d->src, 0, sizeof(d->src));
    d->untested = 0;
    d->progress = 0;
}

static int disc_close(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;

    title_close(s);
    if (d->fmt && d->part)
        d->fmt->close(s);
    av_freep(&d->part);
    for (int i = 0; d->title_tracks && i < d->nb_title_slots; i++)
        av_freep(&d->title_tracks[i]);
    av_freep(&d->title_tracks);
    av_freep(&d->nb_title_tracks);
    ff_disc_pass_cache_free(&d->pass_cache);
    for (int i = 0; i < d->info.nb_titles; i++) {
        av_freep(&d->info.titles[i].streams);
        av_freep(&d->info.titles[i].chapters);
        av_freep(&d->info.titles[i].files);
        av_freep(&d->info.titles[i].parts);
        for (int k = 0; k < d->info.titles[i].nb_props; k++)
            av_freep(&d->info.titles[i].props[k].value);
        av_freep(&d->info.titles[i].props);
    }
    av_freep(&d->info.titles);
    d->info.nb_titles = 0;
    for (int k = 0; k < d->info.nb_props; k++)
        av_freep(&d->info.props[k].value);
    av_freep(&d->info.props);
    d->info.nb_props = 0;
    ff_discio_fs_close(&d->fs);
    ff_discio_source_free(&d->image);
    return 0;
}

/* ---- the flow, the same for every format ---- */

#define MAX_TITLE_TRACKS 256

/* Every title's probe (the format's own), its tracks and the stream pass
 * over its largest segment (discpass.c: duplicates, details for the title
 * list, data). A title whose pass fails is left out, said in the log (the
 * title list and a title's open alike: the titles' numbers must agree). */
static int titles_pass(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    const DiscFormat *f = d->fmt;
    const char *fmt = f->log_name;
    int n = f->nb_titles(s), details = d->opt_details && d->opt_title < 0;
    DiscTrack *tr;

    if (!(d->title_tracks = av_calloc(FFMAX(n, 1), sizeof(*d->title_tracks))) ||
        !(d->nb_title_tracks = av_calloc(FFMAX(n, 1), sizeof(*d->nb_title_tracks))) ||
        !(tr = av_calloc(MAX_TITLE_TRACKS, sizeof(*tr))))
        return AVERROR(ENOMEM);
    d->nb_title_slots = n;
    for (int i = 0; i < n; i++) {
        const char *name = f->title_name(s, i);
        DiscPassSource src = { 0 };
        int ret = f->title_probe ? f->title_probe(s, i) : 0, nb = 0, seg = 0;

        if (ret >= 0)
            ret = nb = f->title_tracks(s, i, tr, MAX_TITLE_TRACKS);
        if (ret >= 0 && nb && (ret = seg = f->title_segment(s, i, &src)) > 0) {
            ret = ff_disc_stream_pass(s, name, &src, tr, &nb, details);
            if (f->segment_close)
                f->segment_close(s, &src);
        }
        if (ret == AVERROR(ENOMEM)) {
            av_free(tr);
            return ret;
        }
        if (ret < 0) {
            if (ret == AVERROR_INVALIDDATA)
                av_log(s, AV_LOG_ERROR, "%s: title %s: no audio / video data can be decoded from it: title "
                       "skipped\n", fmt, name ? name : "?");
            else
                av_log(s, AV_LOG_ERROR, "%s: title %s: its streams cannot be read (%s): title skipped\n", fmt,
                       name ? name : "?", av_err2str(ret));
            f->remove_title(s, i);
            n--;
            i--;
            continue;
        }
        if (nb && !(d->title_tracks[i] = av_memdup(tr, nb * sizeof(*tr)))) {
            av_free(tr);
            return AVERROR(ENOMEM);
        }
        d->nb_title_tracks[i] = nb;
    }
    av_free(tr);
    return 0;
}

/* The disc info's titles: each title's own (the format part's) and its
 * tracks. */
static int titles_info(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    int n = d->fmt->nb_titles(s), ret;

    for (int i = 0; i < n; i++) {
        DiscTitleInfo *dt = ff_disc_add_title(s);
        if (!dt)
            return AVERROR(ENOMEM);
        if ((ret = d->fmt->title_info(s, i, dt)) < 0 ||
            (ret = ff_disc_title_add_tracks(dt, d->title_tracks[i], d->nb_title_tracks[i])) < 0)
            return ret;
    }
    return 0;
}

/* The title option's title (its part): the format part's source of it and
 * its streams and title-reader tracks from its tracks. */
static int open_title(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    const DiscFormat *f = d->fmt;
    int i = d->opt_title, both = 0, ret;
    const char *name;

    if (i >= d->info.nb_titles) {
        av_log(s, AV_LOG_ERROR, "%s: title %d does not exist (the disc has %d)\n", f->log_name, i,
               d->info.nb_titles);
        return AVERROR(EINVAL);
    }
    if (d->opt_part >= 0 && d->opt_part >= d->info.titles[i].nb_parts) {
        av_log(s, AV_LOG_ERROR, "Title %d has no part %d (it has %d)\n", i, d->opt_part, d->info.titles[i].nb_parts);
        return AVERROR(EINVAL);
    }
    name = f->title_name(s, i);
    if (!d->nb_title_tracks[i] || d->title_tracks[i][0].not_read) {
        av_log(s, AV_LOG_ERROR, "%s: title %s: its video (%s) is not read yet\n", f->log_name, name ? name : "?",
               d->nb_title_tracks[i] ? avcodec_get_name(d->title_tracks[i][0].info.codec) : "none");
        return AVERROR_PATCHWELCOME;
    }
    if ((ret = f->open_title(s, i, d->opt_part, &both)) < 0)
        return ret;
    return ff_disc_open_tracks(s, name ? name : "?", d->title_tracks[i], d->nb_title_tracks[i], d->src.ts, both,
                               d->keep);
}

static int disc_read_header(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    DiscIOImageOptions opts = DISCIO_IMAGE_OPTIONS_DEFAULT;
    enum DiscIODiscFormat kind;
    int ret;

    opts.udf_reader = d->opt_udf_reader;
    if (ff_discio_folder_is_dir(s->url)) {
        /* a disc folder: its files are read as host files */
        if ((ret = ff_discio_mount_folder(s, s->url, d->opt_read_attempts, &d->fs)) < 0)
            return ret;
    } else {
        if ((ret = ff_discio_source_open_file(s, s->url, &d->image)) < 0)
            return ret;
        d->image->attempts = d->opt_read_attempts;
        if ((ret = ff_discio_mount_image(d->image, &opts, &d->fs)) < 0)
            return ret;
    }
    kind = ff_discio_disc_format(d->fs);
    if (kind == DISCIO_DISC_NONE && !d->image && dvd_flat_folder(d->fs, s->url)) {
        kind = DISCIO_DISC_DVD;
        d->dvd_flat = 1;
    }
    for (int i = 0; parts[i] && !d->fmt; i++)
        if (parts[i]->kind == kind)
            d->fmt = parts[i];
    if (kind == DISCIO_DISC_NONE) {
        av_log(s, AV_LOG_ERROR, "'%s' holds no disc this program knows (no VIDEO_TS, HVDVD_TS / HDDVD_TS "
               "or BDMV structure)\n", s->url);
        return AVERROR_INVALIDDATA;
    }
    if (!d->fmt) {
        av_log(s, AV_LOG_ERROR, "'%s' is a %s disc, which is not read through the disc layer yet\n",
               s->url, kind_name(kind));
        return AVERROR_PATCHWELCOME;
    }
    av_log(s, AV_LOG_VERBOSE, "'%s': %s disc\n", s->url, d->fmt->name);
    d->info.kind         = d->fmt->name;
    d->info.label        = d->fs->label;
    d->info.filesystem   = d->fs->ops->name;
    d->info.udf_revision = d->fs->udf_revision;
    if (!(d->part = av_mallocz(d->fmt->priv_size)))
        return AVERROR(ENOMEM);
    if ((ret = d->fmt->load(s)) < 0 || (ret = titles_pass(s)) < 0 ||
        (d->fmt->disc_done && (ret = d->fmt->disc_done(s)) < 0) || (ret = titles_info(s)) < 0)
        return ret;
    if (d->opt_title < 0)               /* only the disc info, no title opened */
        return 0;
    if ((ret = open_title(s)) < 0)
        return ret;
    return ff_disc_title_reader_open(s, &d->src);
}

void disc_title_close(AVFormatContext *s)
{
    if (!disc_info(s))
        return;
    /* FFmpeg's read state of the title's streams, then the streams, the
     * chapters and the title's results themselves */
    ff_read_frame_flush(s);
    title_close(s);
    while (s->nb_streams)
        ff_remove_stream(s, s->streams[s->nb_streams - 1]);
    while (s->nb_chapters) {
        s->nb_chapters--;
        av_dict_free(&s->chapters[s->nb_chapters]->metadata);
        av_freep(&s->chapters[s->nb_chapters]);
    }
    av_freep(&s->chapters);
    av_dict_set(&s->metadata, "untested", NULL, 0);
    s->duration = AV_NOPTS_VALUE;
}

int disc_title_open(AVFormatContext *s, int title, int part, int second_view, const uint8_t *keep)
{
    DiscContext *d;
    int ret;

    if (!disc_info(s))
        return AVERROR(EINVAL);
    d = s->priv_data;
    disc_title_close(s);
    if (title < 0)
        return AVERROR(EINVAL);
    /* a 3-D title's second view is read only when one of its tracks is
     * selected */
    if (second_view && keep && title < d->info.nb_titles) {
        int any = 0;
        for (int i = 0; i < d->nb_title_tracks[title]; i++)
            any |= (d->title_tracks[title][i].flags & DISC_TRACK_SECOND_VIEW) && keep[i];
        second_view = any;
    }
    d->opt_title       = title;
    d->opt_part        = part;
    d->opt_second_view = second_view;
    d->keep            = keep;
    ret = open_title(s);
    d->keep = NULL;
    if (ret < 0 || (ret = ff_disc_title_reader_open(s, &d->src)) < 0)
        disc_title_close(s);
    return ret;
}

const char *disc_language(const char *code)
{
    return ff_disc_lang_code(code);
}

const char *disc_language_name(const char *code)
{
    return ff_disc_lang_name(code);
}

const char *disc_mkv_codec_id(const DiscStreamInfo *k)
{
    if (k->second_view)
        return "V_MPEG4/ISO/MVC";
    switch (k->codec) {
    case AV_CODEC_ID_PCM_DVD:
    case AV_CODEC_ID_PCM_BLURAY: return "A_PCM/INT/LIT";    /* written as PCM */
    case AV_CODEC_ID_EIA_608:    return "S_TEXT/UTF8";      /* made into text */
    default:
        for (int i = 0; ff_mkv_codec_tags[i].id != AV_CODEC_ID_NONE; i++)
            if (ff_mkv_codec_tags[i].id == k->codec)
                return ff_mkv_codec_tags[i].str;
        return NULL;
    }
}

static int disc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    DiscContext *d = s->priv_data;

    if (!d->reader)                     /* only the disc info: nothing to read */
        return AVERROR_EOF;
    return ff_disc_title_reader_packet(s, pkt);
}

#define OFFSET(x) offsetof(DiscContext, x)
static const AVOption disc_options[] = {
    {"title",           "the title to open (0 = the first of the disc's title list; -1 = none: only the disc info)", OFFSET(opt_title), AV_OPT_TYPE_INT, { .i64=0 }, -1, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    {"part",            "with title: only that part of it (a title playing whole stream files back to back), from 0; -1 = the whole title", OFFSET(opt_part), AV_OPT_TYPE_INT, { .i64=-1 }, -1, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    {"details",         "the title list: read each track's details (size, frame rate, channels, ...) from the stream", OFFSET(opt_details), AV_OPT_TYPE_BOOL, { .i64=1 }, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    {"dolby_vision",    "Dolby Vision titles: the enhancement layer (with its metadata) merged into the video's frames, as one track, or a track of its own", OFFSET(opt_dolby_vision), AV_OPT_TYPE_INT, { .i64=0 }, 0, 1, AV_OPT_FLAG_DECODING_PARAM, .unit = "dolby_vision" },
    {"merged",          "one track: each frame the base picture, then the enhancement picture's units", 0, AV_OPT_TYPE_CONST, { .i64=0 }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "dolby_vision" },
    {"separate",        "the enhancement layer as a track of its own, as on the disc", 0, AV_OPT_TYPE_CONST, { .i64=1 }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "dolby_vision" },
    {"second_view",     "3-D titles: read the second view as a track of its own (a second layer of the video)", OFFSET(opt_second_view), AV_OPT_TYPE_BOOL, { .i64=0 }, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    {"min_length",      "titles shorter than this (seconds) are listed, not selected", OFFSET(opt_min_length), AV_OPT_TYPE_INT, { .i64=0 }, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    {"dvd_cell_mode",   "DVD: a title's cells: auto (the navigation's when the scan worked, else trimmed), walk (the cells the navigation played), trim (the program chain's, start and end trimmed), walk_trim (walk, trim for titles the navigation did not reach)", OFFSET(opt_dvd_cell_mode), AV_OPT_TYPE_INT, { .i64=0 }, 0, 3, AV_OPT_FLAG_DECODING_PARAM, .unit = "dvd_cell_mode" },
        {"auto",        "the navigation's cells when the scan worked, else trimmed", 0, AV_OPT_TYPE_CONST, { .i64=0 }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "dvd_cell_mode" },
        {"walk",        "the cells the navigation played",                 0, AV_OPT_TYPE_CONST, { .i64=1 }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "dvd_cell_mode" },
        {"trim",        "the program chain's cells, start and end trimmed", 0, AV_OPT_TYPE_CONST, { .i64=2 }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "dvd_cell_mode" },
        {"walk_trim",   "walk; trim for the titles the navigation did not reach", 0, AV_OPT_TYPE_CONST, { .i64=3 }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "dvd_cell_mode" },
    {"dvd_cell_check",  "DVD: the VOBUs a full cell check reads: all, 50 evenly spread, 50 at random", OFFSET(opt_dvd_cell_check), AV_OPT_TYPE_INT, { .i64=0 }, 0, 2, AV_OPT_FLAG_DECODING_PARAM, .unit = "dvd_cell_check" },
        {"all",         "every VOBU",                                      0, AV_OPT_TYPE_CONST, { .i64=0 }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "dvd_cell_check" },
        {"spread",      "50 VOBUs evenly spread",                          0, AV_OPT_TYPE_CONST, { .i64=1 }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "dvd_cell_check" },
        {"random",      "50 VOBUs at random (a different choice each run)", 0, AV_OPT_TYPE_CONST, { .i64=2 }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "dvd_cell_check" },
    {"cc_helper",       "the caption converter program (CCExtractor) for DVD caption tracks; default: the one next to the running program", OFFSET(opt_cc_helper), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    {"keydb",           "AACS key files (KEYDB.cfg), read in this order",          OFFSET(opt_keydb),          AV_OPT_TYPE_STRING | AV_OPT_TYPE_FLAG_ARRAY, { .arr = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    {"read_attempts",   "read attempts per request on the disc",                    OFFSET(opt_read_attempts),  AV_OPT_TYPE_INT,    { .i64=DISCIO_DEFAULT_ATTEMPTS }, 1, 100, AV_OPT_FLAG_DECODING_PARAM },
    {"udf_reader",      "UDF reader for disc images",                               OFFSET(opt_udf_reader),     AV_OPT_TYPE_INT,    { .i64=DISCIO_UDF_NETBSD }, DISCIO_UDF_NETBSD, DISCIO_UDF_LINUX, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
        {"netbsd",      "based on NetBSD (default)",                                0,                          AV_OPT_TYPE_CONST,  { .i64=DISCIO_UDF_NETBSD }, 0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
        {"linux",       "based on Linux",                                           0,                          AV_OPT_TYPE_CONST,  { .i64=DISCIO_UDF_LINUX },  0, 0, AV_OPT_FLAG_DECODING_PARAM, .unit = "udf_reader" },
    {"progress",        "how far the title is read (1/10000), exported", OFFSET(progress), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, 10000, AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY },
    {NULL}
};

static const AVClass disc_class = {
    .class_name = "disc demuxer",
    .item_name  = av_default_item_name,
    .option     = disc_options,
    .version    = LIBAVUTIL_VERSION_INT
};

const FFInputFormat ff_disc_demuxer = {
    .p.name         = "disc",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Disc (DVD-Video, HD DVD, Blu-ray)"),
    .p.priv_class   = &disc_class,
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_NOFILE | AVFMT_NO_BYTE_SEEK |
                      AVFMT_NOGENSEARCH | AVFMT_NOBINSEARCH,
    .priv_data_size = sizeof(DiscContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_close     = disc_close,
    .read_header    = disc_read_header,
    .read_packet    = disc_read_packet,
};
