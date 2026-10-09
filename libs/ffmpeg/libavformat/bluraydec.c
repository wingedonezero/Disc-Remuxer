/*
 * Blu-ray (BDMV, BDAV, AVCHD): the format part of the disc layer (disc.h).
 * It reads the disc's structure (bluray_disc.c), its AACS keys
 * (bluray_aacs.c), title list (bluray_titles.c) and tracks
 * (bluray_tracks.c) and describes the disc. One title's tracks become the
 * streams; the disc layer's title reader reads its segments
 * (DiscTitleSource: a segment's stream data in 6144-byte units, AACS
 * decrypted here; each track's PID and PES stream keys; the 1 ms audio
 * lead-in tolerance; the title's chapter marks). Tracks whose codec the
 * title timeline does not read yet are left out (logged).
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
#include "disctl.h"
#include "internal.h"

#define LEAD_IN_TOLERANCE 1080000LL       /* Blu-ray audio lead-in tolerance: 1 ms (ticks) */

typedef struct BlurayDemuxContext {
    BlurayDisc disc;
    BlurayAACS *aacs;
    char     **files;          /* the titles' file names (disc info) */
    int        nb_files;

    /* the opened title, as the title reader gets it (DiscContext.src) */
    const BlurayTitle *title;
    int                seg0;         /* its first segment read (a part of the title: not 0) */
    int                both;         /* 3-D: both views read (the second view is a track) */
    BluraySegReader    seg;          /* the segment being read (seg.s NULL: none) */
    int64_t           *chapter_times;
} BlurayDemuxContext;

static const char *clip_name(AVFormatContext *s, int k)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    return c->title->segs[c->seg0 + k].path;
}

static uint32_t clip_blocks(AVFormatContext *s, int k)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    return ff_bluray_seg_size_views(&c->title->segs[c->seg0 + k], c->both) / BLURAY_UNIT;
}

static int read_block(AVFormatContext *s, int k, uint32_t block, uint8_t *buf)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    const BluraySegment *seg = &c->title->segs[c->seg0 + k];
    int ret;

    if (c->seg.s != seg) {
        ff_bluray_seg_reader_close(&c->seg);
        ff_bluray_seg_reader_init(&c->seg, s, ((DiscContext *)s->priv_data)->fs, c->aacs, seg);
        if (c->both)
            ff_bluray_seg_reader_both(&c->seg);
    }
    ret = ff_bluray_seg_read(&c->seg, (int64_t)block * BLURAY_UNIT, buf);
    return ret == AVERROR_EOF ? AVERROR(EIO) : ret < 0 ? ret : 1;
}

static int has_dir(DiscIOFS *fs, const char *path)
{
    return !fs->ops->find_dir(fs, path);
}

static void bluray_close(AVFormatContext *s)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    ff_bluray_seg_reader_close(&c->seg);
    av_freep(&c->chapter_times);
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

/* The files of segment k in the disc info's file list (one per item, the
 * dependent view's file after its item's when read interleaved). */
static int seg_files(const BlurayTitle *t, int k)
{
    int n = 0;
    for (int j = 0; j < t->segs[k].nb_items; j++)
        n += 1 + (t->segs[k].items[j].dep && t->segs[k].interleaved);
    return n;
}

/* The title's parts, when it plays whole stream files back to back. */
static int describe_parts(const BlurayTitle *t, DiscTitleInfo *dt)
{
    int *first, n, file = 0, ret = 0;
    uint64_t *marks;

    if (ff_bluray_title_parts(t, NULL) < 2)
        return 0;
    first = av_calloc(t->nb_segs + 1, sizeof(*first));
    marks = av_calloc(FFMAX(t->nb_marks, 1), sizeof(*marks));
    if (!first || !marks) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    n = ff_bluray_title_parts(t, first);
    for (int p = 0; p < n; p++) {
        DiscPartInfo *dp = ff_disc_title_add_part(dt);
        if (!dp) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        dp->start       = av_rescale(ff_bluray_segs_duration(t, 0, first[p]), AV_TIME_BASE, 45000);
        dp->duration    = av_rescale(ff_bluray_segs_duration(t, first[p], first[p + 1]), AV_TIME_BASE, 45000);
        dp->first_file  = file;
        for (int k = first[p]; k < first[p + 1]; k++)
            dp->nb_files += seg_files(t, k);
        file           += dp->nb_files;
        dp->nb_chapters = ff_bluray_segs_marks(t, first[p], first[p + 1], marks);
    }
end:
    av_free(first);
    av_free(marks);
    return ret;
}

/* The 3-D second view of the video (MVC, on its own PID: sel1). */
static int second_view(const BlurayTrack *tk)
{
    return tk->type == AVMEDIA_TYPE_VIDEO && tk->from_track >= 0 && tk->codec == AV_CODEC_ID_H264;
}

/* Whether the title timeline reads a track's codec in this step: AVC,
 * MPEG-2, VC-1 video (the 3-D second view: as a second layer); AC-3, DTS,
 * DTS-HD, TrueHD, LPCM audio; PGS subtitles. */
static int readable(const BlurayTrack *tk)
{
    switch (tk->codec) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_MPEG2VIDEO:
    case AV_CODEC_ID_VC1:  return tk->type == AVMEDIA_TYPE_VIDEO && (tk->from_track < 0 || second_view(tk));
    case AV_CODEC_ID_AC3:
    case AV_CODEC_ID_DTS:
    case AV_CODEC_ID_TRUEHD:
    case AV_CODEC_ID_PCM_BLURAY:
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE: return !(tk->flags & BLURAY_TRACK_SIDE);
    default:               return 0;
    }
}

/* A title's tracks as the disc layer takes them (out: t->nb_tracks). */
static void title_tracks(const BlurayTitle *t, DiscTrack *out)
{
    const BlurayClip *c0 = t->segs[t->track_seg].items[0].clip;
    int bdav = c0->hdr.type == MKBETAG('M', '2', 'T', 'S');
    int base_right = t->pl && (t->pl->app_flags & 0x1000);

    for (int i = 0; i < t->nb_tracks; i++) {
        const BlurayTrack *tk = &t->tracks[i];
        DiscTrack *o = &out[i];

        memset(o, 0, sizeof(*o));
        ff_disc_stream_init(&o->info);
        o->info.type            = tk->type;
        o->info.codec           = tk->codec;
        o->info.inner_track     = tk->inner_track;
        o->info.from_track      = tk->from_track;
        o->info.forced          = !!(tk->flags & BLURAY_TRACK_FORCED);
        o->info.second_view     = tk->type == AVMEDIA_TYPE_VIDEO && tk->from_track >= 0;
        o->info.offset_sequence = tk->offset_seq;
        o->info.second_layer    = !!(tk->flags & BLURAY_TRACK_LAYER2);
        memcpy(o->info.language, tk->lang, sizeof(o->info.language));
        o->id   = -1;
        o->kind   = tk->type == AVMEDIA_TYPE_VIDEO ? DR_KIND_VIDEO :
                    tk->type == AVMEDIA_TYPE_AUDIO ? DR_KIND_AUDIO : DR_KIND_SUBTITLE;
        o->stream = tk->stream;
        o->number = tk->stream;
        if (tk->flags & BLURAY_TRACK_SIDE)
            o->flags |= DISC_TRACK_SIDE;
        o->feed.pid     = second_view(tk) ? (int)tk->sel1 : tk->pid;
        o->feed.nb_keys = ff_bluray_track_keys(tk, bdav, o->feed.key);
        if (second_view(tk))
            o->flags |= DISC_TRACK_SECOND_VIEW;
        /* Dolby Vision: the enhancement layer (with its metadata) on its
         * own PID, sel1 */
        if (tk->flags & BLURAY_TRACK_LAYER2) {
            o->flags |= DISC_TRACK_LAYER2;
            o->layer_feed.pid     = tk->sel1;
            o->layer_feed.nb_keys = ff_bluray_track_keys(tk, bdav, o->layer_feed.key);
        }
        /* when both views are read: which eye each one shows (the
         * playlist's MVC_Base_view_R_flag: the base view is the right eye) */
        if (tk->type == AVMEDIA_TYPE_VIDEO)
            o->eye = second_view(tk) == base_right ? "left" : "right";
        if (tk->codec == AV_CODEC_ID_HDMV_PGS_SUBTITLE)
            o->picture = (tk->flags & BLURAY_TRACK_FORCED) ?
                         "Blu-ray PGS display sets, forced subtitles only (each set shows only its forced objects, "
                         "as a player does with subtitles off)" : "Blu-ray PGS display sets";
        if (!readable(tk))
            o->not_read = "its codec is not read yet";
    }
}

/* The pass's reading: the segment's units as the title reader reads them
 * (a 3-D segment with both views when a track is the second view). */
static int pass_read(void *opaque, uint32_t block, uint8_t *buf)
{
    int ret = ff_bluray_seg_read(opaque, (int64_t)block * BLURAY_UNIT, buf);
    return ret < 0 ? ret : 1;
}

/* Every title's tracks through the disc layer's stream pass over the
 * segment they are described from (the largest): duplicates, details (for
 * the title list), data. A title whose video has no data is left out; one
 * whose segment cannot be read keeps its tracks as described. */
static int stream_pass(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    BlurayDemuxContext *c = ff_disc_part(s);
    BlurayDisc *b = &c->disc;

    for (int i = 0; i < b->nb_titles; i++) {
        BlurayTitle *t = &b->titles[i];
        const BluraySegment *seg;
        BluraySegReader u;
        DiscPassSource src = { .ts = 1, .read = pass_read, .opaque = &u };
        int n = t->nb_tracks, ret, both = 0;

        if (!n)
            continue;
        if (!(t->dtracks = av_calloc(n, sizeof(*t->dtracks))))
            return AVERROR(ENOMEM);
        title_tracks(t, t->dtracks);
        seg = &t->segs[t->track_seg];
        for (int k = 0; k < n; k++)
            both |= !!(t->dtracks[k].flags & DISC_TRACK_SECOND_VIEW);
        src.index = t->track_seg;
        src.key   = seg->items[0].clip->number + 1ULL;
        ff_bluray_seg_reader_init(&u, s, d->fs, c->aacs, seg);
        if (both && seg->interleaved)
            ff_bluray_seg_reader_both(&u);
        ret = ff_disc_stream_pass(s, t->name, &src, t->dtracks, &n, d->opt_details && d->opt_title < 0);
        ff_bluray_seg_reader_close(&u);
        if (ret == AVERROR(ENOMEM))
            return ret;
        if (ret == AVERROR_INVALIDDATA) {
            av_log(s, AV_LOG_ERROR, "Blu-ray: title %s left out: its video has no data\n", t->name);
            ff_bluray_title_remove(b, i--);
            continue;
        }
        if (ret < 0) {
            av_log(s, AV_LOG_WARNING, "Blu-ray: title %s: its stream details cannot be read: %s\n", t->name,
                   av_err2str(ret));
            title_tracks(t, t->dtracks);
            n = t->nb_tracks;
        }
        t->nb_dtracks = n;
    }
    return 0;
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
        int ret;

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
        if ((ret = ff_disc_title_add_tracks(dt, t->dtracks, t->nb_dtracks)) < 0)
            return ret;
        for (int k = 0; k < t->nb_marks; k++) {
            DiscChapterInfo *ch = ff_disc_title_add_chapter(dt);
            if (!ch)
                return AVERROR(ENOMEM);
            ch->start = av_rescale(t->marks[k], AV_TIME_BASE, 1080000000);
        }
        if ((ret = describe_parts(t, dt)) < 0)
            return ret;
    }
    return 0;
}

/* The opened title: its streams (the tracks the timeline reads, in the
 * title's track order) and what the title reader needs. */
static int open_title(AVFormatContext *s, const BlurayTitle *t, int part)
{
    DiscContext *d = s->priv_data;
    BlurayDemuxContext *c = ff_disc_part(s);
    int s0 = 0, s1 = t->nb_segs, nb_marks;
    uint64_t *marks;

    c->title = t;
    if (part >= 0) {
        int *first = av_calloc(t->nb_segs + 1, sizeof(*first)), nb_parts;
        if (!first)
            return AVERROR(ENOMEM);
        nb_parts = ff_bluray_title_parts(t, first);
        if (nb_parts < 2 || part >= nb_parts) {
            av_log(s, AV_LOG_ERROR, "Blu-ray: title %s has no part %d (%s)\n", t->name, part,
                   nb_parts < 2 ? "it plays no whole stream files back to back" : "too few parts");
            av_free(first);
            return AVERROR(EINVAL);
        }
        s0 = first[part];
        s1 = first[part + 1];
        av_free(first);
        av_log(s, AV_LOG_VERBOSE, "Blu-ray: title %s: part %d of %d alone: %s .. %s\n", t->name, part, nb_parts,
               t->segs[s0].path, t->segs[s1 - 1].path);
    }
    c->seg0 = s0;
    /* the second view of a 3-D title: asked for (option second_view) and its
     * segments read with both views */
    if (d->opt_second_view) {
        int has = 0, all = 1;
        for (int i = 0; i < t->nb_tracks; i++)
            has |= second_view(&t->tracks[i]);
        for (int k = s0; k < s1; k++)
            all &= t->segs[k].interleaved;
        c->both = has && all;
        if (has && !all)
            av_log(s, AV_LOG_WARNING, "Blu-ray: title %s: not every segment holds both views: the second view is "
                   "not read [untested on real discs]\n", t->name);
    }
    for (int k = s0; k < s1; k++) {
        int64_t size = ff_bluray_seg_size(&t->segs[k]);
        /* 3-D: the base view's stream files alone (the second view is not
         * read in this step) */
        if (t->segs[k].interleaved)
            av_log(s, AV_LOG_VERBOSE, "Blu-ray: title %s: segment %s is 3-D: its base view is read\n", t->name,
                   t->segs[k].desc);
        if (size % BLURAY_UNIT)
            av_log(s, AV_LOG_WARNING, "Blu-ray: segment %s: its last %"PRId64" bytes are no whole unit and are not "
                   "read [untested on real discs]\n", t->segs[k].path, size % BLURAY_UNIT);
    }
    if (!t->nb_tracks || !readable(&t->tracks[0])) {
        av_log(s, AV_LOG_ERROR, "Blu-ray: title %s: its video (%s) is not read yet\n", t->name,
               t->nb_tracks ? avcodec_get_name(t->tracks[0].codec) : "none");
        return AVERROR_PATCHWELCOME;
    }
    if (!(c->chapter_times = av_calloc(FFMAX(t->nb_marks, 1), sizeof(*c->chapter_times))))
        return AVERROR(ENOMEM);
    if (!(marks = av_calloc(FFMAX(t->nb_marks, 1), sizeof(*marks))))
        return AVERROR(ENOMEM);
    /* the whole title: its marks as they are; a part: its own, from its start */
    if (part < 0) {
        for (int i = 0; i < t->nb_marks; i++)
            marks[i] = t->marks[i];
        nb_marks = t->nb_marks;
    } else
        nb_marks = ff_bluray_segs_marks(t, s0, s1, marks);
    for (int i = 0; i < nb_marks; i++)
        c->chapter_times[i] = marks[i];
    av_free(marks);
    s->duration = av_rescale(ff_bluray_segs_duration(t, s0, s1), AV_TIME_BASE, 45000);
    d->src = (DiscTitleSource){
        .nb_clips      = s1 - s0,
        .clip_kind     = "segment",
        .clip_name     = clip_name,
        .clip_blocks   = clip_blocks,
        .read_block    = read_block,
        .tolerance     = LEAD_IN_TOLERANCE,
        .chapter_times = c->chapter_times,
        .nb_chapters   = nb_marks,
    };
    return ff_disc_open_tracks(s, t->name, t->dtracks, t->nb_dtracks, 1, c->both);
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
    if ((ret = ff_bluray_titles_build(s, b, d->opt_min_length)) < 0 || (ret = stream_pass(s)) < 0 ||
        (ret = describe_titles(s)) < 0)
        return ret;
    d->info.name      = b->name;
    d->info.encrypted = has_dir(d->fs, "/AACS") || has_dir(d->fs, "/AACS_bd") ||
                        has_dir(d->fs, "/AACS2_av") || has_dir(d->fs, "/AACS2_mv");
    if (d->opt_title < 0)               /* only the disc info, no title opened */
        return 0;
    if (d->opt_title >= b->nb_titles) {
        av_log(s, AV_LOG_ERROR, "Blu-ray: title %d does not exist (the disc has %d)\n", d->opt_title, b->nb_titles);
        return AVERROR(EINVAL);
    }
    return open_title(s, &b->titles[d->opt_title], d->opt_part);
}

const DiscFormat ff_disc_bluray = {
    .name        = "Blu-ray",
    .log_name    = "Blu-ray",
    .kind        = DISCIO_DISC_BLURAY,
    .priv_size   = sizeof(BlurayDemuxContext),
    .open        = bluray_open,
    .close       = bluray_close,
};
