/*
 * The disc layer (its public face is disc/disc.h): it opens the source
 * once through the disc readers (an image file or a folder), tells which
 * kind of disc it is from its file system and hands it to that kind's
 * format part (disc/model.h). The disc's own details (kind, label, file
 * system) go into the disc info (disc/info.h) here; its titles come from
 * the format part. A title's tracks come from the format part in one shape
 * for every format (DiscTrack): the disc info's streams and, for the
 * opened title, its public tracks and the title reader's tracks are made
 * from them here.
 */

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/str.h"
#include "base/vlog.h"
#include "disc/disc.h"
#include "disc/info.h"
#include "disc/lang.h"
#include "disc/model.h"
#include "io/discio.h"
#include "timeline/disctl.h"

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
    for (int i = 0; i < (int)DR_ARRAY_ELEMS(ends); i++)
        named |= n >= strlen(ends[i]) && !strncasecmp(path + n - strlen(ends[i]), ends[i], strlen(ends[i]));
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

DiscTitleInfo *ff_disc_add_title(DrDisc *d)
{
    DiscTitleInfo *t = dr_array_add(&d->info.titles, &d->info.nb_titles, sizeof(*t), NULL);

    if (t) {
        memset(t, 0, sizeof(*t));
        t->index = d->info.nb_titles - 1;
    }
    return t;
}

DiscStreamInfo *ff_disc_title_add_stream(DiscTitleInfo *t)
{
    DiscStreamInfo *k = dr_array_add(&t->streams, &t->nb_streams, sizeof(*k), NULL);
    if (k)
        ff_disc_stream_init(k);
    return k;
}

DiscChapterInfo *ff_disc_title_add_chapter(DiscTitleInfo *t)
{
    DiscChapterInfo *c = dr_array_add(&t->chapters, &t->nb_chapters, sizeof(*c), NULL);
    if (c)
        memset(c, 0, sizeof(*c));
    return c;
}

DiscPartInfo *ff_disc_title_add_part(DiscTitleInfo *t)
{
    DiscPartInfo *p = dr_array_add(&t->parts, &t->nb_parts, sizeof(*p), NULL);
    if (p)
        memset(p, 0, sizeof(*p));
    return p;
}

int ff_disc_title_add_file(DiscTitleInfo *t, const char *name)
{
    return dr_array_add_ptr(&t->files, &t->nb_files, (void *)name);
}

static int add_prop(DiscPropertyInfo **props, int *nb, DiscPropKey key, const char *fmt, va_list vl)
{
    DiscPropertyInfo *p = dr_array_add(props, nb, sizeof(*p), NULL);
    DrStr bp;

    if (!p)
        return DR_ERR(ENOMEM);

    p->key  = key;
    p->name = disc_prop_name(key);
    dr_str_init(&bp);
    dr_str_vcatf(&bp, fmt, vl);
    p->value = dr_str_take(&bp, 1);
    return p->value ? 0 : DR_ERR(ENOMEM);
}

int ff_disc_title_add_prop(DiscTitleInfo *t, DiscPropKey key, const char *fmt, ...)
{
    va_list vl;
    int ret;

    va_start(vl, fmt);
    ret = add_prop(&t->props, &t->nb_props, key, fmt, vl);
    va_end(vl);
    return ret;
}

int ff_disc_add_prop(DrDisc *d, DiscPropKey key, const char *fmt, ...)
{
    va_list vl;
    int ret;

    va_start(vl, fmt);
    ret = add_prop(&d->info.props, &d->info.nb_props, key, fmt, vl);
    va_end(vl);
    return ret;
}

void ff_disc_stream_init(DiscStreamInfo *k)
{
    memset(k, 0, sizeof(*k));
    k->inner_track = k->from_track = k->offset_sequence = -1;
    k->profile = DISC_PROFILE_UNKNOWN;
    k->level   = DISC_LEVEL_UNKNOWN;
}

int ff_disc_title_add_tracks(DiscTitleInfo *t, const DiscTrack *tracks, int nb_tracks)
{
    for (int i = 0; i < nb_tracks; i++) {
        DiscStreamInfo *k = ff_disc_title_add_stream(t);
        if (!k)
            return DR_ERR(ENOMEM);
        *k = tracks[i].info;
    }
    return 0;
}

/* ---- the opened title's tracks ---- */

static int add_pid(DrDisc *d, int pid)
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
        snprintf(buf, size, "PID 0x%04x, key 0x%04x%s%s", f->pid, f->key[0], f->key[0] ? "" : " (any)",
                 f->side ? ", from a clip of its own" : "");
}

/* The public track of a track read: its codec, what the caller shows of
 * it, a sub-picture canvas and VobSub header (the extradata the writer
 * copies into the .idx). */
static int track_out(DrDisc *d, const DiscTrack *k, int index, DRCodecID codec, int second_view)
{
    DrDiscTrack *t = &d->out_tracks[d->nb_out_tracks];

    memset(t, 0, sizeof(*t));
    t->number  = index;
    t->type    = k->info.type;
    t->codec   = codec;
    t->profile = DISC_PROFILE_UNKNOWN;
    dr_strlcpy(t->language, k->info.language, sizeof(t->language));
    t->second_view       = k->info.from_track >= 0 && k->info.second_view;
    t->forced            = k->info.forced || (k->info.roles & DISC_ROLE_FORCED);
    t->commentary        = !!(k->info.roles & (DISC_ROLE_COMMENTARY | DISC_ROLE_ALT_COMMENTARY));
    t->visually_impaired = !!(k->info.roles & DISC_ROLE_VISUALLY_IMPAIRED);
    t->line21  = k->line21;
    t->eye     = second_view ? k->eye : NULL;
    if (k->flags & DISC_TRACK_LAYER2)
        t->dolby_vision = d->opt_dolby_vision ? "BL" : "BL+EL+RPU";
    t->picture = k->picture;
    t->caption = k->caption;
    if (k->kind == DR_KIND_SUBTITLE && k->info.width > 0 && k->info.height > 0) {
        t->width  = k->info.width;
        t->height = k->info.height;
    }
    if (codec == DR_CODEC_DVD_SUBTITLE) {
        /* the VobSub index header (readers take size and palette from it) */
        char idx[1024];
        int len = ff_disctl_vobsub_header(idx, sizeof(idx), k->info.width, k->info.height, k->palette);
        if (len < 0 || len >= (int)sizeof(idx))
            return DR_ERR_BUG;
        if (!(d->track_extradata[d->nb_out_tracks] = dr_memdup(idx, len + 1)))
            return DR_ERR_NOMEM;
        t->extradata      = d->track_extradata[d->nb_out_tracks];
        t->extradata_size = len;
    }
    d->nb_out_tracks++;
    return 0;
}

int ff_disc_open_tracks(DrDisc *d, const char *title, const DiscTrack *tracks, int nb_tracks, int ts,
                        int second_view, const uint8_t *keep)
{
    const char *fmt = d->fmt->log_name;
    int n = 0, layer = -1, caption_left = 0, ret;
    char place[64];

    /* one more for a Dolby Vision enhancement layer (read from its own PID) */
    if (!(d->tracks = dr_calloc(nb_tracks + 1, sizeof(*d->tracks))) ||
        !(d->track_ids = dr_calloc(nb_tracks + 1, sizeof(*d->track_ids))) ||
        !(d->out_tracks = dr_calloc(nb_tracks + 1, sizeof(*d->out_tracks))) ||
        !(d->track_extradata = dr_calloc(nb_tracks + 1, sizeof(*d->track_extradata))) ||
        (ts && (!(d->feeds = dr_calloc(nb_tracks + 1, sizeof(*d->feeds))) ||
                !(d->pids  = dr_calloc(nb_tracks + 1, sizeof(*d->pids))))))
        return DR_ERR(ENOMEM);
    d->nb_out_tracks = 0;
    for (int i = 0; i < nb_tracks; i++) {
        const DiscTrack *k = &tracks[i];
        int view = !!(k->flags & DISC_TRACK_SECOND_VIEW);
        /* a caption track: its cues are text */
        DRCodecID codec = k->kind == DR_KIND_CAPTION ? DR_CODEC_SUBRIP : k->info.codec;
        DRTitleTrack *t = &d->tracks[n];

        /* the video and a video with its Dolby Vision layer always stay
         * (the layer is ripped as option dolby_vision says, never left out) */
        if (i && keep && !keep[i] && !(k->flags & DISC_TRACK_LAYER2)) {
            dr_vlog(d->log, DR_VLOG_INFO, "%s: title %s: track %d (%s %s%s%s) is not selected: left out\n", fmt, title, i,
                   disc_media_name(k->info.type), ff_disctl_codec_id_name(k->info.codec),
                   k->info.language[0] ? ", " : "", k->info.language);
            caption_left |= k->kind == DR_KIND_CAPTION;
            continue;
        }
        if (view && !second_view) {
            dr_vlog(d->log, DR_VLOG_INFO, "%s: title %s: track %d, the 3-D second view (PID 0x%04x), is left out (not "
                   "asked for)\n", fmt, title, i, k->feed.pid);
            continue;
        }
        if (k->not_read) {
            track_place(place, sizeof(place), k, &k->feed, ts);
            dr_vlog(d->log, DR_VLOG_WARNING, "%s: title %s: track %d (%s %s, %s) is not read yet: left out\n", fmt, title,
                   i, disc_media_name(k->info.type), ff_disctl_codec_id_name(k->info.codec), place);
            continue;
        }
        if (ts) {
            d->feeds[n] = k->feed;
            if (!k->feed.side)          /* a clip of its own: not read with the title's clips */
                add_pid(d, k->feed.pid);
        }
        d->track_ids[n] = ts ? k->feed.pid | k->feed.key[0] << 16 : k->id;
        if ((ret = track_out(d, k, i, codec, second_view)) < 0)
            return ret;
        if (k->untested) {
            dr_vlog(d->log, DR_VLOG_WARNING, "%s: title %s: track %d (%s): %s [untested on real discs]\n", fmt, title, i,
                   ff_disctl_codec_id_name(k->info.codec), k->untested);
            d->untested++;
        }
        if (k->flags & DISC_TRACK_LAYER2)
            layer = i;
        t->codec       = codec;
        t->kind        = view ? DR_KIND_LAYER : k->kind;
        t->sub_flags   = (k->info.forced ? DR_SUB_FORCED_ONLY : 0) | (k->feed.side ? DR_SUB_SIDE : 0);
        t->audio_flags = (k->info.from_track >= 0 ? DR_AUDIO_EXTRACTED : 0) |
                         (k->info.from_track >= 0 || k->info.inner_track >= 0 ? DR_AUDIO_EXTENDED : 0) |
                         (k->flags & DISC_TRACK_EXT_ONLY ? DR_AUDIO_EXT_ONLY : 0) |
                         (d->fmt->kind == DISCIO_DISC_DVD ? DR_AUDIO_DVD : 0);
        if (k->kind == DR_KIND_SUBTITLE) {
            t->width  = k->info.width;
            t->height = k->info.height;
        }
        track_place(place, sizeof(place), k, &k->feed, ts);
        dr_vlog(d->log, DR_VLOG_VERBOSE, "%s: track %d -> stream %d: %s, %s\n", fmt, i, n, ff_disctl_codec_id_name(codec), place);
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
        d->track_ids[n] = ts ? k->layer_feed.pid | k->layer_feed.key[0] << 16 : k->id;
        t->codec       = DR_CODEC_HEVC;
        t->kind        = DR_KIND_LAYER;
        t->layer_flags = d->opt_dolby_vision ? 0 : DR_LAYER_MERGED;
        if (d->opt_dolby_vision) {
            DrDiscTrack *o = &d->out_tracks[d->nb_out_tracks++];
            memset(o, 0, sizeof(*o));
            o->number  = layer;
            o->type    = DISC_MEDIA_VIDEO;
            o->codec   = DR_CODEC_HEVC;
            o->profile = DISC_PROFILE_UNKNOWN;
            dr_strlcpy(o->language, k->info.language, sizeof(o->language));
            o->dolby_vision = "EL";
        }
        dr_vlog(d->log, DR_VLOG_INFO, "%s: track %d: its Dolby Vision enhancement layer, PID 0x%04x, key 0x%04x: %s\n",
               fmt, layer, k->layer_feed.pid, k->layer_feed.key[0],
               d->opt_dolby_vision ? "a stream of its own" : "merged into the video's frames");
        n++;
    }
    /* line-21 caption data the video carries is left out as asked, not lost */
    if (caption_left && d->nb_out_tracks)
        d->out_tracks[0].caption_left = 1;
    d->src.ts        = ts;
    d->src.tracks    = d->tracks;
    d->src.nb_tracks = n;
    d->src.feeds     = d->feeds;
    d->src.pids      = d->pids;
    return 0;
}

const DiscInfo *dr_disc_info(const DrDisc *d)
{
    return d ? &d->info : NULL;
}

/* The opened title's reader, tracks and the format part's state of it
 * given back (the disc stays loaded). */
static void title_close(DrDisc *d)
{

    ff_disc_title_reader_close(&d->reader);
    if (d->fmt && d->part && d->fmt->close_title)
        d->fmt->close_title(d);
    dr_freep(&d->tracks);
    dr_freep(&d->track_ids);
    dr_freep(&d->feeds);
    dr_freep(&d->pids);
    for (int i = 0; d->track_extradata && i < d->nb_out_tracks; i++)
        dr_freep(&d->track_extradata[i]);
    dr_freep(&d->track_extradata);
    dr_freep(&d->out_tracks);
    d->nb_out_tracks = 0;
    dr_freep(&d->chapters);
    d->nb_chapters = 0;
    for (int i = 0; i < d->nb_damage; i++)
        dr_freep(&d->damage[i]);
    dr_freep(&d->damage);
    d->nb_damage = 0;
    d->damaged_ranges = d->damaged_bytes = 0;
    d->untested_total = 0;
    d->duration = 0;
    memset(&d->src, 0, sizeof(d->src));
    d->untested = 0;
    d->progress = 0;
}

static int disc_close(DrDisc *d)
{

    title_close(d);
    if (d->fmt && d->part)
        d->fmt->close(d);
    dr_freep(&d->part);
    for (int i = 0; d->title_tracks && i < d->nb_title_slots; i++)
        dr_freep(&d->title_tracks[i]);
    dr_freep(&d->title_tracks);
    dr_freep(&d->nb_title_tracks);
    ff_disc_pass_cache_free(&d->pass_cache);
    for (int i = 0; i < d->info.nb_titles; i++) {
        dr_freep(&d->info.titles[i].streams);
        dr_freep(&d->info.titles[i].chapters);
        dr_freep(&d->info.titles[i].files);
        dr_freep(&d->info.titles[i].parts);
        for (int k = 0; k < d->info.titles[i].nb_props; k++)
            dr_freep(&d->info.titles[i].props[k].value);
        dr_freep(&d->info.titles[i].props);
    }
    dr_freep(&d->info.titles);
    d->info.nb_titles = 0;
    for (int k = 0; k < d->info.nb_props; k++)
        dr_freep(&d->info.props[k].value);
    dr_freep(&d->info.props);
    d->info.nb_props = 0;
    ff_discio_fs_close(&d->fs);
    ff_discio_source_free(&d->image);
    return 0;
}

/* ---- the flow, the same for every format ---- */

#define MAX_TITLE_TRACKS 256

/* A title's video tracks the disc names no language for (no disc format
 * does for video) get the video_language option's: a code, or "market"
 * (the disc's market language, else English). */
static int video_language(DrDisc *d, const char *name, DiscTrack *tr, int nb)
{
    int given = 0;
    const char *lang = d->opt_video_language;

    if (!lang || !*lang)
        return 0;
    if (!strcmp(lang, "market"))
        lang = d->market[0] ? d->market : "eng";
    for (int j = 0; j < nb; j++)
        if (tr[j].info.type == DISC_MEDIA_VIDEO && !tr[j].info.language[0]) {
            dr_strlcpy(tr[j].info.language, lang, sizeof(tr[j].info.language));
            dr_vlog(d->log, DR_VLOG_DEBUG, "Title %s: video track %d: the disc names no language: %s (the "
                   "video_language option)\n", name ? name : "?", j, tr[j].info.language);
            given++;
        }
    return given;
}

/* Every title's probe (the format's own), its tracks and the stream pass
 * over its largest segment (pass.c: duplicates, details for the title
 * list, data). A title whose pass fails is left out, said in the log (the
 * title list and a title's open alike: the titles' numbers must agree). */
static int titles_pass(DrDisc *d)
{
    const DiscFormat *f = d->fmt;
    const char *fmt = f->log_name;
    int n = f->nb_titles(d), details = d->opt_details && d->opt_title < 0, lang_given = 0;
    DiscTrack *tr;

    if (!(d->title_tracks = dr_calloc(DR_MAX(n, 1), sizeof(*d->title_tracks))) ||
        !(d->nb_title_tracks = dr_calloc(DR_MAX(n, 1), sizeof(*d->nb_title_tracks))) ||
        !(tr = dr_calloc(MAX_TITLE_TRACKS, sizeof(*tr))))
        return DR_ERR(ENOMEM);
    d->nb_title_slots = n;
    for (int i = 0; i < n; i++) {
        const char *name = f->title_name(d, i);
        DiscPassSource src = { 0 };
        int ret = f->title_probe ? (f->title_probe(d, i)) : 0, nb = 0, seg = 0;

        if (ret >= 0)
            ret = nb = (f->title_tracks(d, i, tr, MAX_TITLE_TRACKS));
        if (ret >= 0 && nb && (ret = seg = (f->title_segment(d, i, &src))) > 0) {
            ret = ff_disc_stream_pass(d, name, &src, tr, &nb, details);
            if (f->segment_close)
                f->segment_close(d, &src);
        }
        if (ret == DR_ERR(ENOMEM)) {
            dr_free(tr);
            return ret;
        }
        if (ret < 0) {
            if (ret == DR_ERR_INVALID)
                dr_vlog(d->log, DR_VLOG_ERROR, "%s: title %s: no audio / video data can be decoded from it: title "
                       "skipped\n", fmt, name ? name : "?");
            else
                dr_vlog(d->log, DR_VLOG_ERROR, "%s: title %s: its streams cannot be read (%s): title skipped\n", fmt,
                       name ? name : "?", dr_err_text(ret));
            f->remove_title(d, i);
            n--;
            i--;
            continue;
        }
        lang_given += video_language(d, name, tr, nb);
        if (nb && !(d->title_tracks[i] = dr_memdup(tr, nb * sizeof(*tr)))) {
            dr_free(tr);
            return DR_ERR(ENOMEM);
        }
        d->nb_title_tracks[i] = nb;
    }
    dr_free(tr);
    /* said once for the disc (each title's in the detail log) */
    if (lang_given)
        dr_vlog(d->log, DR_VLOG_INFO, "%s: the disc names no language for the video of %d title(s): %s given (the "
               "video_language setting)\n", fmt, lang_given, !strcmp(d->opt_video_language, "market") ?
               (d->market[0] ? d->market : "eng") : d->opt_video_language);
    return 0;
}

/* The disc info's titles: each title's own (the format part's) and its
 * tracks. */
static int titles_info(DrDisc *d)
{
    int n = d->fmt->nb_titles(d), ret;

    for (int i = 0; i < n; i++) {
        DiscTitleInfo *dt = ff_disc_add_title(d);
        if (!dt)
            return DR_ERR(ENOMEM);
        if ((ret = (d->fmt->title_info(d, i, dt))) < 0 ||
            (ret = ff_disc_title_add_tracks(dt, d->title_tracks[i], d->nb_title_tracks[i])) < 0)
            return ret;
    }
    return 0;
}

/* The title option's title (its part): the format part's source of it and
 * its streams and title-reader tracks from its tracks. */
static int open_title(DrDisc *d)
{
    const DiscFormat *f = d->fmt;
    int i = d->opt_title, both = 0, ret;
    const char *name;

    if (i >= d->info.nb_titles) {
        dr_vlog(d->log, DR_VLOG_ERROR, "%s: title %d does not exist (the disc has %d)\n", f->log_name, i,
               d->info.nb_titles);
        return DR_ERR(EINVAL);
    }
    if (d->opt_part >= 0 && d->opt_part >= d->info.titles[i].nb_parts) {
        dr_vlog(d->log, DR_VLOG_ERROR, "Title %d has no part %d (it has %d)\n", i, d->opt_part, d->info.titles[i].nb_parts);
        return DR_ERR(EINVAL);
    }
    name = f->title_name(d, i);
    if (!d->nb_title_tracks[i] || d->title_tracks[i][0].not_read) {
        dr_vlog(d->log, DR_VLOG_ERROR, "%s: title %s: its video (%s) is not read yet\n", f->log_name, name ? name : "?",
               d->nb_title_tracks[i] ? ff_disctl_codec_id_name(d->title_tracks[i][0].info.codec) : "none");
        return DR_ERR_UNSUPPORTED;
    }
    if ((ret = (f->open_title(d, i, d->opt_part, &both))) < 0)
        return ret;
    return ff_disc_open_tracks(d, name ? name : "?", d->title_tracks[i], d->nb_title_tracks[i], d->src.ts, both,
                               d->keep);
}

static int disc_read_header(DrDisc *d)
{
    DiscIOImageOptions opts = DISCIO_IMAGE_OPTIONS_DEFAULT;
    enum DiscIODiscFormat kind;
    int ret;

    opts.udf_reader                = d->opt_udf_reader;
    opts.prefer_iso_for_old_udf102 = d->opt_prefer_iso;
    if (ff_discio_folder_is_dir(d->source)) {
        /* a disc folder: its files are read as host files */
        if ((ret = ff_discio_mount_folder((void *)d->log, d->source, d->opt_read_attempts, &d->fs)) < 0)
            return ret;
    } else {
        if ((ret = ff_discio_source_open_file((void *)d->log, d->source, &d->image)) < 0)
            return ret;
        d->image->attempts = d->opt_read_attempts;
        if ((ret = ff_discio_mount_image(d->image, &opts, &d->fs)) < 0)
            return ret;
    }
    if (d->opt_video_language && *d->opt_video_language && strcmp(d->opt_video_language, "market")) {
        const char *code = dr_disc_lang_code(d->opt_video_language);
        if (!code || strlen(code) != 3) {
            dr_vlog(d->log, DR_VLOG_ERROR, "video_language '%s' is no ISO 639 language code\n", d->opt_video_language);
            return DR_ERR(EINVAL);
        }
        dr_free(d->opt_video_language);
        if (!(d->opt_video_language = dr_strdup(code)))
            return DR_ERR(ENOMEM);
    }
    kind = ff_discio_disc_format(d->fs);
    if (kind == DISCIO_DISC_NONE && !d->image && dvd_flat_folder(d->fs, d->source)) {
        kind = DISCIO_DISC_DVD;
        d->dvd_flat = 1;
    }
    for (int i = 0; parts[i] && !d->fmt; i++)
        if (parts[i]->kind == kind)
            d->fmt = parts[i];
    if (kind == DISCIO_DISC_NONE) {
        dr_vlog(d->log, DR_VLOG_ERROR, "'%s' holds no disc this program knows (no VIDEO_TS, HVDVD_TS / HDDVD_TS "
               "or BDMV structure)\n", d->source);
        return DR_ERR_INVALID;
    }
    if (!d->fmt) {
        dr_vlog(d->log, DR_VLOG_ERROR, "'%s' is a %s disc, which is not read through the disc layer yet\n",
               d->source, kind_name(kind));
        return DR_ERR_UNSUPPORTED;
    }
    dr_vlog(d->log, DR_VLOG_VERBOSE, "'%s': %s disc\n", d->source, d->fmt->name);
    d->info.kind         = d->fmt->name;
    d->info.label        = d->fs->label;
    d->info.filesystem   = d->fs->ops->name;
    d->info.udf_revision = d->fs->udf_revision;
    if (!(d->part = dr_mallocz(d->fmt->priv_size)))
        return DR_ERR(ENOMEM);
    if ((ret = (d->fmt->load(d))) < 0 ||
        (ret = d->market[0] ? ff_disc_add_prop(d, DISC_PROP_MARKET_LANGUAGE, "%s (%s)", d->market, d->market_from) :
                              ff_disc_add_prop(d, DISC_PROP_MARKET_LANGUAGE, "none named")) < 0 ||
        (ret = titles_pass(d)) < 0 ||
        (d->fmt->disc_done && (ret = (d->fmt->disc_done(d))) < 0) || (ret = titles_info(d)) < 0)
        return ret;
    if (d->opt_title < 0)               /* only the disc info, no title opened */
        return 0;
    if ((ret = open_title(d)) < 0)
        return ret;
    return ff_disc_title_reader_open(d, &d->src);
}

void dr_disc_title_close(DrDisc *d)
{
    if (!d)
        return;
    /* the reader, the tracks and the title's results (the typed state) */
    title_close(d);
}

int dr_disc_title_open(DrDisc *d, int title, int part, int second_view, const uint8_t *keep)
{
    int ret;

    if (!d)
        return DR_ERR(EINVAL);
    dr_disc_title_close(d);
    if (title < 0)
        return DR_ERR(EINVAL);
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
    ret = open_title(d);
    d->keep = NULL;
    if (ret < 0 || (ret = ff_disc_title_reader_open(d, &d->src)) < 0)
        dr_disc_title_close(d);
    return ret;
}

void ff_disc_set_market(DrDisc *d, const char (*codes)[4], int n, int ordered, const char *from)
{
    const char *first = NULL;
    int several = 0, eng = 0;

    for (int i = 0; i < n; i++) {
        const char *c = dr_disc_lang_code(codes[i]);
        if (!c || !strcmp(c, "und"))
            continue;
        if (!first)
            first = c;
        else if (strcmp(first, c))
            several = 1;
        eng |= !strcmp(c, "eng");
    }
    if (!first)
        return;
    dr_strlcpy(d->market, several && !ordered && eng ? "eng" : first, sizeof(d->market));
    d->market_from = from;
}

const char *disc_language(const char *code)
{
    return dr_disc_lang_code(code);
}

const char *disc_language_name(const char *code)
{
    return dr_disc_lang_name(code);
}


int dr_disc_read(DrDisc *d, DrDiscFrame *f)
{
    if (!d->reader)                     /* only the disc info: nothing to read */
        return DR_ERR_EOF;
    return ff_disc_title_reader_frame(d, f);
}

void dr_disc_frame_unref(DrDiscFrame *f)
{
    dr_buf_unref(&f->buf);
    f->data = NULL;
    f->size = 0;
}

int dr_disc_nb_tracks(const DrDisc *d)
{
    return d->nb_out_tracks;
}

const DrDiscTrack *dr_disc_track(const DrDisc *d, int i)
{
    return i >= 0 && i < d->nb_out_tracks ? &d->out_tracks[i] : NULL;
}

int64_t dr_disc_progress(const DrDisc *d)
{
    return d->progress;
}

int dr_disc_nb_chapters(const DrDisc *d)
{
    return d->nb_chapters;
}

const DrDiscChapter *dr_disc_chapter(const DrDisc *d, int i)
{
    return i >= 0 && i < d->nb_chapters ? &d->chapters[i] : NULL;
}

int64_t dr_disc_title_duration(const DrDisc *d)
{
    return d->duration;
}

int64_t dr_disc_untested(const DrDisc *d)
{
    return d->untested_total;
}

int64_t dr_disc_damaged_ranges(const DrDisc *d)
{
    return d->damaged_ranges;
}

int64_t dr_disc_damaged_bytes(const DrDisc *d)
{
    return d->damaged_bytes;
}

const char *dr_disc_damage(const DrDisc *d, int i)
{
    return i >= 0 && i < d->nb_damage ? d->damage[i] : "";
}

int dr_disc_open(DrDisc **disc, const char *source, const DrDiscOptions *o, const char *log)
{
    DrDisc *d = dr_mallocz(sizeof(*d));
    int ret;

    *disc = NULL;
    if (!d)
        return DR_ERR_NOMEM;
    d->log              = log ? log : "disc";
    d->opt_title        = o->title;
    d->opt_part         = o->part;
    d->opt_details      = o->details;
    d->opt_dolby_vision = o->dolby_vision;
    d->opt_second_view  = o->second_view;
    d->opt_min_length   = o->min_length;
    d->opt_dvd_cell_mode  = o->dvd_cell_mode;
    d->opt_dvd_cell_check = o->dvd_cell_check;
    d->opt_read_attempts  = o->read_attempts > 0 ? o->read_attempts : DISCIO_DEFAULT_ATTEMPTS;
    d->opt_udf_reader     = o->udf_reader;
    d->opt_prefer_iso     = o->prefer_iso;
    if (!(d->source = dr_strdup(source)) ||
        (o->cc_helper && *o->cc_helper && !(d->opt_cc_helper = dr_strdup(o->cc_helper))) ||
        (o->video_language && *o->video_language &&
         !(d->opt_video_language = dr_strdup(o->video_language)))) {
        ret = DR_ERR_NOMEM;
        goto fail;
    }
    if (o->nb_keydb) {
        if (!(d->opt_keydb = dr_calloc(o->nb_keydb, sizeof(*d->opt_keydb)))) {
            ret = DR_ERR_NOMEM;
            goto fail;
        }
        for (int i = 0; i < o->nb_keydb; i++) {
            if (!(d->opt_keydb[i] = dr_strdup(o->keydb[i]))) {
                ret = DR_ERR_NOMEM;
                goto fail;
            }
            d->nb_opt_keydb++;
        }
    }
    if ((ret = disc_read_header(d)) < 0)
        goto fail;
    *disc = d;
    return 0;
fail:
    dr_disc_close(&d);
    return ret;
}

void dr_disc_set_chapter_rule(DrDisc *d, int exact, int before_ms, int after_ms)
{
    d->chapter_rule = (DRChapterRule){ .set = 1, .exact = exact, .before_ms = before_ms, .after_ms = after_ms };
}

void dr_disc_close(DrDisc **disc)
{
    DrDisc *d = *disc;

    if (!d)
        return;
    disc_close(d);
    for (unsigned i = 0; i < d->nb_opt_keydb; i++)
        dr_free(d->opt_keydb[i]);
    dr_free(d->opt_keydb);
    dr_free(d->opt_cc_helper);
    dr_free(d->opt_video_language);
    dr_free((void *)d->source);
    dr_freep(disc);
}
