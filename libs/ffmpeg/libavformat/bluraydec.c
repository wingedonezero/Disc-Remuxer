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
    DRTitleTrack      *tracks;
    DiscTsFeed        *feeds;
    int               *pids;
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
    av_freep(&c->tracks);
    av_freep(&c->feeds);
    av_freep(&c->pids);
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
        if ((ret = describe_parts(t, dt)) < 0)
            return ret;
    }
    return 0;
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

static int add_pid(BlurayDemuxContext *c, int *nb, int pid)
{
    for (int i = 0; i < *nb; i++)
        if (c->pids[i] == pid)
            return 0;
    c->pids[(*nb)++] = pid;
    return 1;
}

/* The opened title: its streams (the tracks the timeline reads, in the
 * title's track order) and what the title reader needs. */
static int open_title(AVFormatContext *s, const BlurayTitle *t, int part)
{
    DiscContext *d = s->priv_data;
    BlurayDemuxContext *c = ff_disc_part(s);
    const BlurayClip *c0 = t->segs[t->track_seg].items[0].clip;
    int bdav = c0->hdr.type == MKBETAG('M', '2', 'T', 'S'), n = 0, nb_pids = 0, nb_marks;
    int s0 = 0, s1 = t->nb_segs;
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
    if (!(c->tracks = av_calloc(t->nb_tracks, sizeof(*c->tracks))) ||
        !(c->feeds  = av_calloc(t->nb_tracks, sizeof(*c->feeds))) ||
        !(c->pids   = av_calloc(t->nb_tracks, sizeof(*c->pids))) ||
        !(c->chapter_times = av_calloc(FFMAX(t->nb_marks, 1), sizeof(*c->chapter_times))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < t->nb_tracks; i++) {
        const BlurayTrack *tk = &t->tracks[i];
        DiscTsFeed *f = &c->feeds[n];
        AVStream *st;

        if (second_view(tk) && !c->both) {
            av_log(s, AV_LOG_INFO, "Blu-ray: title %s: track %d, the 3-D second view (PID 0x%04x), is left out (not "
                   "asked for)\n", t->name, i, tk->sel1);
            continue;
        }
        if (!readable(tk)) {
            av_log(s, AV_LOG_WARNING, "Blu-ray: title %s: track %d (%s %s, PID 0x%04x) is not read yet: left out\n",
                   t->name, i, av_get_media_type_string(tk->type), avcodec_get_name(tk->codec), tk->pid);
            continue;
        }
        f->pid     = second_view(tk) ? (int)tk->sel1 : tk->pid;
        f->nb_keys = ff_bluray_track_keys(tk, bdav, f->key);
        if (!(st = avformat_new_stream(s, NULL)))
            return AVERROR(ENOMEM);
        st->id                   = f->pid | f->key[0] << 16;
        st->codecpar->codec_type = tk->type;
        st->codecpar->codec_id   = tk->codec;
        avpriv_set_pts_info(st, 64, 1, DR_TICKS_PER_SECOND);
        if (tk->from_track >= 0)
            st->disposition |= AV_DISPOSITION_DEPENDENT;
        if (tk->flags & BLURAY_TRACK_FORCED)
            st->disposition |= AV_DISPOSITION_FORCED;
        if (tk->lang[0] && av_dict_set(&st->metadata, "language", tk->lang, 0) < 0)
            return AVERROR(ENOMEM);
        if (av_dict_set_int(&st->metadata, "disc_track", i, 0) < 0)
            return AVERROR(ENOMEM);
        /* both views read: which eye each one shows (the playlist's
         * MVC_Base_view_R_flag: the base view is the right eye) */
        if (c->both && tk->type == AVMEDIA_TYPE_VIDEO) {
            int base_right = t->pl && (t->pl->app_flags & 0x1000);
            if (av_dict_set(&st->metadata, "eye", second_view(tk) == base_right ? "left" : "right", 0) < 0)
                return AVERROR(ENOMEM);
        }
        /* what the units are, for the caller's log */
        if (tk->codec == AV_CODEC_ID_HDMV_PGS_SUBTITLE &&
            av_dict_set(&st->metadata, "picture", (tk->flags & BLURAY_TRACK_FORCED) ?
                        "Blu-ray PGS display sets, forced subtitles only (each set shows only its forced objects, "
                        "as a player does with subtitles off)" : "Blu-ray PGS display sets", 0) < 0)
            return AVERROR(ENOMEM);
        c->tracks[n].codec       = tk->codec;
        c->tracks[n].kind        = second_view(tk) ? DR_KIND_LAYER :
                                   tk->type == AVMEDIA_TYPE_VIDEO ? DR_KIND_VIDEO :
                                   tk->type == AVMEDIA_TYPE_AUDIO ? DR_KIND_AUDIO : DR_KIND_SUBTITLE;
        c->tracks[n].sub_flags   = (tk->flags & BLURAY_TRACK_FORCED) ? DR_SUB_FORCED_ONLY : 0;
        c->tracks[n].audio_flags = (tk->from_track >= 0 ? DR_AUDIO_EXTRACTED : 0) |
                                   (tk->from_track >= 0 || tk->inner_track >= 0 ? DR_AUDIO_EXTENDED : 0);
        add_pid(c, &nb_pids, f->pid);
        if (f->nb_keys > 1)
            av_log(s, AV_LOG_VERBOSE, "Blu-ray: track %d -> stream %d: %s, PID 0x%04x, keys 0x%04x + 0x%04x\n", i, n,
                   avcodec_get_name(tk->codec), tk->pid, f->key[0], f->key[1]);
        else
            av_log(s, AV_LOG_VERBOSE, "Blu-ray: track %d -> stream %d: %s, PID 0x%04x, key 0x%04x%s\n", i, n,
                   avcodec_get_name(tk->codec), tk->pid, f->key[0], f->key[0] ? "" : " (any)");
        n++;
    }
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
        .ts            = 1,
        .pids          = c->pids,
        .nb_pids       = nb_pids,
        .feeds         = c->feeds,
        .tolerance     = LEAD_IN_TOLERANCE,
        .tracks        = c->tracks,
        .nb_tracks     = n,
        .chapter_times = c->chapter_times,
        .nb_chapters   = nb_marks,
    };
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
    .kind        = DISCIO_DISC_BLURAY,
    .priv_size   = sizeof(BlurayDemuxContext),
    .open        = bluray_open,
    .close       = bluray_close,
};
