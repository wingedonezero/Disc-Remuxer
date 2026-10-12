/*
 * Blu-ray (BDMV, BDAV, AVCHD): the format part of the disc layer (disc.h).
 * It reads the disc's structure (bluray_disc.c), its AACS keys
 * (bluray_aacs.c), title list (bluray_titles.c) and tracks
 * (bluray_tracks.c) and describes the disc. One title's tracks become the
 * streams; the disc layer's title reader reads its segments
 * (DiscTitleSource: a segment's stream data in 6144-byte units, AACS
 * decrypted here; each track's PID and PES stream keys; the title's
 * chapter marks). Tracks whose codec the
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


typedef struct BlurayDemuxContext {
    BlurayDisc disc;
    BlurayAACS *aacs;
    BlurayHash *hash;          /* the disc's content hash tables: the stream files read are checked */
    char     **files;          /* the titles' file names (disc info) */
    int        nb_files;

    /* the opened title, as the title reader gets it (DiscContext.src) */
    const BlurayTitle *title;
    int                seg0;         /* its first segment read (a part of the title: not 0) */
    int                both;         /* 3-D: both views read (the second view is a track) */
    BluraySegReader    seg;          /* the segment being read (seg.s NULL: none) */
    BluraySegReader    side;         /* the text subtitle clip being read */
    char               side_name[224];
    BluraySegReader    pass;         /* the stream pass's segment */
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

/* Where the playlist starts segment k: its first PlayItem's IN time (a
 * clip title: its STC sequence's presentation start), 45 kHz -> 90 kHz. */
static int64_t clip_start(AVFormatContext *s, int k)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    const BluraySegItem *it = &c->title->segs[c->seg0 + k].items[0];

    return 2 * (int64_t)(it->item ? it->item->in_time : it->stc->start);
}

/* Where unit block of segment k lies: its clip's stream file and the byte
 * offset there (items one after another); not known (< 0) in an item read
 * with both views interleaved. */
static int block_place(AVFormatContext *s, int k, uint32_t block, char *file, int size, int64_t *offset)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    const BluraySegment *seg = &c->title->segs[c->seg0 + k];
    int64_t pos = (int64_t)block * BLURAY_UNIT;

    if (c->both && seg->interleaved)
        return AVERROR(ENOSYS);
    for (int i = 0; i < seg->nb_items; i++) {
        const BluraySegItem *it = &seg->items[i];
        if (pos < it->length) {
            snprintf(file, size, "%05u.%s", it->clip->number, it->clip->stream_type == 2 ? "fmts" : "m2ts");
            *offset = it->start + pos;
            return 0;
        }
        pos -= it->length;
    }
    return AVERROR(EINVAL);
}

static int read_block(AVFormatContext *s, int k, uint32_t block, uint8_t *buf)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    const BluraySegment *seg = &c->title->segs[c->seg0 + k];
    int ret;

    if (c->seg.s != seg) {
        ff_bluray_seg_reader_close(&c->seg);
        ff_bluray_seg_reader_init(&c->seg, s, ((DiscContext *)s->priv_data)->fs, c->aacs, seg);
        c->seg.hash = c->hash;
        if (c->both)
            ff_bluray_seg_reader_both(&c->seg);
    }
    ret = ff_bluray_seg_read(&c->seg, (int64_t)block * BLURAY_UNIT, buf);
    return ret == AVERROR_EOF ? AVERROR(EIO) : ret < 0 ? ret : 1;
}

/* ---- text subtitle clips (a clip of their own per stream) ---- */

static const char *side_name(AVFormatContext *s, int k)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    snprintf(c->side_name, sizeof(c->side_name), "text subtitle clip %s", c->title->side[k].seg.path);
    return c->side_name;
}

static uint32_t side_blocks(AVFormatContext *s, int k)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    return ff_bluray_seg_size(&c->title->side[k].seg) / BLURAY_UNIT;
}

static int side_read(AVFormatContext *s, int k, uint32_t block, uint8_t *buf)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    const BluraySegment *seg = &c->title->side[k].seg;
    int ret;

    if (c->side.s != seg) {
        ff_bluray_seg_reader_close(&c->side);
        ff_bluray_seg_reader_init(&c->side, s, ((DiscContext *)s->priv_data)->fs, c->aacs, seg);
        c->side.hash = c->hash;
    }
    ret = ff_bluray_seg_read(&c->side, (int64_t)block * BLURAY_UNIT, buf);
    return ret == AVERROR_EOF ? AVERROR(EIO) : ret < 0 ? ret : 1;
}

/* Where text subtitle clip k lies on the title (the reference program's
 * rule): its SubPlayItem's IN time is where the PlayItem it is synchronised
 * with is at its sync_start_PTS_of_PlayItem, so its clock lies IN time -
 * sync start ahead of that PlayItem's clip's; the first segment of the
 * title (in its order) that plays that clip must be the first one read,
 * with that clip as its first item; else the clip's times are taken as the
 * first segment's (offset 0: what the reference program does, said; LOOK
 * INTO AGAIN LATER: the sync item's own segment would place them). The
 * sync start before its clip's STC sequence, or the IN time before the
 * text subtitle clip's, is said. */
static int side_offset(AVFormatContext *s, int k, int first, int64_t *offset)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    const BlurayTitle *t = c->title;
    const BluraySide *x = &t->side[k];
    const char *name = side_name(s, k);
    int r0 = c->seg0 + first, seg, item = -1;

    *offset = 0;
    for (seg = 0; seg < t->nb_segs && item < 0; seg++)
        for (int i = 0; i < t->segs[seg].nb_items && item < 0; i++)
            if (t->segs[seg].items[i].clip == x->sync_clip)
                item = i;
    seg--;
    if (item < 0 || seg != r0 || item) {
        if (item < 0)
            av_log(s, AV_LOG_WARNING, "Blu-ray: title %s: %s is synchronised with clip %05u, which no segment of "
                   "the title plays: its times are taken as the first segment's (the reference program's way) "
                   "[untested on real discs]\n", t->name, name, x->sync_clip->number);
        else
            av_log(s, AV_LOG_WARNING, "Blu-ray: title %s: %s is synchronised with clip %05u, which is %s: its "
                   "times are taken as the first segment's (the reference program's way; LOOK INTO AGAIN LATER) "
                   "[untested on real discs]\n", t->name, name, x->sync_clip->number,
                   seg != r0 ? "on another segment than the first one read" : "not the first item of its segment");
        ((DiscContext *)s->priv_data)->untested++;
        return 0;
    }
    if (x->sync_pts < x->sync_stc->start)
        av_log(s, AV_LOG_WARNING, "Blu-ray: title %s: %s: its sync start %"PRIu32" (45 kHz) lies before the STC "
               "sequence of clip %05u (%"PRIu32")\n", t->name, name, x->sync_pts, x->sync_clip->number,
               x->sync_stc->start);
    if (x->in_time < x->stc->start)
        av_log(s, AV_LOG_WARNING, "Blu-ray: title %s: %s: its IN time %"PRIu32" (45 kHz) lies before its STC "
               "sequence (%"PRIu32")\n", t->name, name, x->in_time, x->stc->start);
    *offset = ((int64_t)x->in_time - x->sync_pts) * 24000;
    av_log(s, AV_LOG_VERBOSE, "Blu-ray: title %s: %s: IN time %"PRIu32", synchronised with clip %05u at %"PRIu32
           " (45 kHz): its clock lies %+.3f s ahead\n", t->name, name, x->in_time, x->sync_clip->number,
           x->sync_pts, (double)*offset / DR_TICKS_PER_SECOND);
    return 1;
}

static int has_dir(DiscIOFS *fs, const char *path)
{
    return !fs->ops->find_dir(fs, path);
}

/* The opened title's segment reader and chapter marks given back; the
 * next title starts without both views. */
static void close_title(AVFormatContext *s)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    ff_bluray_seg_reader_close(&c->seg);
    ff_bluray_seg_reader_close(&c->side);
    av_freep(&c->chapter_times);
    c->title = NULL;
    c->seg0  = 0;
    c->both  = 0;
}

static void bluray_close(AVFormatContext *s)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    close_title(s);
    for (int i = 0; i < c->nb_files; i++)
        av_free(c->files[i]);
    av_freep(&c->files);
    ff_bluray_aacs_close(&c->aacs);
    ff_bluray_hash_free(&c->hash);
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

/* Why the title reader does not read a track (NULL: it reads it): a codec
 * the codec table does not have, a video taken out of another one but the
 * 3-D second view. */
static const char *not_read(const BlurayTrack *tk)
{
    if (!ff_disctl_codec(tk->codec))
        return "its codec";
    if (tk->type == AVMEDIA_TYPE_VIDEO && tk->from_track >= 0 && !second_view(tk))
        return "a video taken out of another one";
    return NULL;
}

/* A text subtitle's character code by name (the Blu-ray stream coding
 * information), NULL for one the format does not define. */
static const char *char_code_name(int code)
{
    static const char *const names[] = { NULL, "UTF-8", "UTF-16BE", "Shift-JIS", "EUC-KR", "GB18030", "GB2312",
                                         "Big5" };
    return code > 0 && code < FF_ARRAY_ELEMS(names) ? names[code] : NULL;
}

/* Codecs the title reader reads that no real Blu-ray has had yet: read,
 * and said in the log. */
static const char *untested(const BlurayTrack *tk)
{
    switch (tk->codec) {
    case AV_CODEC_ID_EAC3:     return "E-AC-3 on a Blu-ray";
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_AAC_LATM: return "AAC on a Blu-ray";
    case AV_CODEC_ID_MP1:
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:      return "MPEG audio on a Blu-ray";
    case AV_CODEC_ID_HDMV_TEXT_SUBTITLE: return "text subtitles (from a clip of their own) on a Blu-ray";
    default:                   return NULL;
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
        o->feed.pid     = second_view(tk) ? (int)tk->sel1 : tk->pid;
        /* a text subtitle: its stream comes after the clip's streams, its
         * clip of its own after the title's side clips before it */
        if (tk->flags & BLURAY_TRACK_SIDE) {
            o->flags    |= DISC_TRACK_SIDE;
            o->feed.side = tk->stream - c0->program.nb_streams + 1;
        }
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
        if (tk->codec == AV_CODEC_ID_HDMV_TEXT_SUBTITLE)
            o->picture = (tk->flags & BLURAY_TRACK_FORCED) ?
                         "Blu-ray text subtitle dialogs, forced subtitles only (each dialog keeps only its forced "
                         "regions, as a player does with subtitles off)" : "Blu-ray text subtitle dialogs";
        o->not_read = not_read(tk);
        o->untested = untested(tk);
    }
}

/* The pass's reading: the segment's units as the title reader reads them
 * (a 3-D segment with both views when a track is the second view). */
static int pass_read(void *opaque, uint32_t block, uint8_t *buf)
{
    int ret = ff_bluray_seg_read(opaque, (int64_t)block * BLURAY_UNIT, buf);
    return ret < 0 ? ret : 1;
}

/* ---- the title entries (disc.h) ---- */

static int nb_titles(AVFormatContext *s)
{
    return ((BlurayDemuxContext *)ff_disc_part(s))->disc.nb_titles;
}

static const char *title_name(AVFormatContext *s, int i)
{
    return ((BlurayDemuxContext *)ff_disc_part(s))->disc.titles[i].name;
}

static int title_tracks_of(AVFormatContext *s, int i, DiscTrack *out, int out_size)
{
    const BlurayTitle *t = &((BlurayDemuxContext *)ff_disc_part(s))->disc.titles[i];
    if (t->nb_tracks > out_size)
        return AVERROR_BUG;
    title_tracks(t, out);
    return t->nb_tracks;
}

/* The segment the tracks are described from (the largest), read as the
 * title reader reads it; with both views when a track is the 3-D second
 * view; results shared by the titles that start with the same clip. */
static int title_segment(AVFormatContext *s, int i, DiscPassSource *src)
{
    DiscContext *d = s->priv_data;
    BlurayDemuxContext *c = ff_disc_part(s);
    const BlurayTitle *t = &c->disc.titles[i];
    const BluraySegment *seg = &t->segs[t->track_seg];
    int both = 0;

    for (int k = 0; k < t->nb_tracks; k++)
        both |= second_view(&t->tracks[k]);
    ff_bluray_seg_reader_init(&c->pass, s, d->fs, c->aacs, seg);
    if (both && seg->interleaved)
        ff_bluray_seg_reader_both(&c->pass);
    *src = (DiscPassSource){ .index = t->track_seg, .ts = 1, .key = seg->items[0].clip->number + 1ULL,
                             .read = pass_read, .opaque = &c->pass };
    return 1;
}

static void segment_close(AVFormatContext *s, DiscPassSource *src)
{
    ff_bluray_seg_reader_close(&((BlurayDemuxContext *)ff_disc_part(s))->pass);
}

static void remove_title(AVFormatContext *s, int i)
{
    ff_bluray_title_remove(&((BlurayDemuxContext *)ff_disc_part(s))->disc, i);
}

/* The disc info's title: name (the title's own, else the disc's), what it
 * is made from, its angle, duration and size (its STC sequences' and their
 * bytes), the clip files it plays (one per segment item), chapter marks,
 * parts. */
static int title_info(AVFormatContext *s, int i, DiscTitleInfo *dt)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    BlurayDisc *b = &c->disc;
    const BlurayTitle *t = &b->titles[i];

    dt->name     = t->title_name ? t->title_name : b->name;
    dt->source   = t->name;
    dt->angle    = t->angle;
    if (dt->angle) {                        /* angle 1 the main one; the others not ripped with all */
        dt->main_angle = 1;
        dt->main_title = -1;
        for (int j = 0; j < b->nb_titles; j++)
            if (!strcmp(b->titles[j].name, t->name) && b->titles[j].angle == 1)
                dt->main_title = j;
    }
    dt->duration = av_rescale(ff_bluray_title_duration(t), AV_TIME_BASE, 45000);
    dt->size     = ff_bluray_title_size(t);
    dt->selected = dt->angle <= 1;
    av_strlcpy(dt->language, b->name_lang, sizeof(dt->language));
    if ((t->alt_name && ff_disc_title_add_prop(dt, "Other name", "%s", t->alt_name) < 0) ||
        (t->year && ff_disc_title_add_prop(dt, "Recorded", "%04d-%02d-%02d %02d:%02d:%02d", t->year, t->month,
                                           t->day, t->hour, t->minute, t->second) < 0) ||
        (t->nb_side && ff_disc_title_add_prop(dt, "Side clips (text subtitles)", "%d", t->nb_side) < 0))
        return AVERROR(ENOMEM);
    if (b->pl_menu_name && t->pl)
        dt->menu_name = b->pl_menu_name[t->pl - b->playlists];
    if (b->pl_check && t->pl) {
        int p = t->pl - b->playlists;
        if (b->pl_check[p] == BLURAY_PL_FAKE) {
            dt->playlist_check = DISC_PLAYLIST_LIKELY_FAKE;
            if (ff_disc_title_add_prop(dt, "Fake playlist check", "likely fake: %d item(s) play a copy of a clip "
                                       "it played before", b->pl_repeats[p]) < 0)
                return AVERROR(ENOMEM);
        } else if (b->pl_check[p] == BLURAY_PL_REAL) {
            dt->playlist_check = DISC_PLAYLIST_LIKELY_REAL;
            if (ff_disc_title_add_prop(dt, "Fake playlist check", "likely real: no clip played twice, as long as "
                                       "the fakes") < 0)
                return AVERROR(ENOMEM);
        }
    }
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
    for (int k = 0; k < t->nb_marks; k++) {
        DiscChapterInfo *ch = ff_disc_title_add_chapter(dt);
        if (!ch)
            return AVERROR(ENOMEM);
        ch->start = av_rescale(t->marks[k], AV_TIME_BASE, 1080000000);
    }
    return describe_parts(t, dt);
}

/* The opened title's source (its streams and tracks: the disc layer's):
 * its segments (a part's), chapter marks, both views of a 3-D title when
 * asked for. */
/* The title's stream data against the disc's content hash tables: one
 * line; the damaged hash units (each already logged). */
static int64_t integrity(AVFormatContext *s)
{
    BlurayDemuxContext *c = ff_disc_part(s);
    int64_t checked, failed;
    int files;

    if (!c->hash)
        return 0;
    ff_bluray_hash_stats(c->hash, &checked, &failed, &files);
    if (!checked)
        av_log(s, AV_LOG_INFO, "Blu-ray: content hash check: no whole hash unit of this title was read: nothing "
               "checked\n");
    else if (!failed)
        av_log(s, AV_LOG_INFO, "Blu-ray: content hash check: %"PRId64" blocks (%.1f GB) checked against the disc's "
               "content hashes: all correct\n", checked, checked * 196608.0 / (1 << 30));
    else
        av_log(s, AV_LOG_WARNING, "Blu-ray: content hash check: %"PRId64" of %"PRId64" blocks checked are damaged, in "
               "%d stream file%s (see above)\n", failed, checked, files, files == 1 ? "" : "s");
    return failed;
}

static int open_title(AVFormatContext *s, int i, int part, int *second_view_read)
{
    DiscContext *d = s->priv_data;
    BlurayDemuxContext *c = ff_disc_part(s);
    const BlurayTitle *t = &c->disc.titles[i];
    int s0 = 0, s1 = t->nb_segs, nb_marks;
    uint64_t *marks;

    c->title = t;
    /* text subtitles: their bytes stay as on the disc, the encoding of
     * their text is said when it is not UTF-8 (the reference program says
     * it does not support it) */
    for (int k = 0; k < t->nb_tracks; k++) {
        const BlurayTrack *tk = &t->tracks[k];
        const char *name = char_code_name(tk->char_code);
        if (tk->codec != AV_CODEC_ID_HDMV_TEXT_SUBTITLE || (tk->flags & BLURAY_TRACK_FORCED) || tk->char_code == 1)
            continue;
        av_log(s, AV_LOG_WARNING, "Blu-ray: title %s: track %d: text subtitles whose text is %s%s (character code "
               "%d): kept as on the disc; a reader that takes it as UTF-8 shows it wrong [untested on real discs]\n",
               t->name, k, name ? "in " : "in an encoding the format does not define", name ? name : "",
               tk->char_code);
        d->untested++;
    }
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
            av_log(s, AV_LOG_INFO, "Blu-ray: title %s: segment %s is 3-D: %s\n", t->name, t->segs[k].desc,
                   c->both ? "both views are read" : "its base view is read (the second view with option "
                   "second_view)");
        if (size % BLURAY_UNIT)
            av_log(s, AV_LOG_WARNING, "Blu-ray: segment %s: its last %"PRId64" bytes are no whole unit and are not "
                   "read [untested on real discs]\n", t->segs[k].path, size % BLURAY_UNIT);
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
    ff_bluray_hash_reset(c->hash);
    d->src = (DiscTitleSource){
        .nb_clips      = s1 - s0,
        .clip_kind     = "segment",
        .clip_name     = clip_name,
        .clip_blocks   = clip_blocks,
        .read_block    = read_block,
        .block_place   = block_place,
        .clip_start    = clip_start,
        .clip_start_name = "the start time the playlist gives it (IN time)",
        .integrity     = integrity,
        .ts            = 1,
        .chapter_times = c->chapter_times,
        .nb_chapters   = nb_marks,
        .nb_sides      = t->nb_side,
        .side_name     = side_name,
        .side_blocks   = side_blocks,
        .side_read     = side_read,
        .side_offset   = side_offset,
    };
    *second_view_read = c->both;
    return 0;
}

static int bluray_load(AVFormatContext *s)
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
    /* an AACS disc read with its keys: its stream files against its content
     * hash tables (hashes of the bytes as they are on the disc) */
    if (c->aacs && (ret = ff_bluray_hash_open(s, d->fs, &c->hash)) < 0)
        return ret;
    b->keys = c->aacs;
    if ((ret = ff_bluray_menu_read(s, b)) < 0 ||
        (ret = ff_bluray_titles_build(s, b, d->opt_min_length)) < 0)
        return ret;
    d->info.menu_name = b->menu_name;
    if ((ret = ff_bluray_decoys_check(s, b)) < 0)
        return ret;
    if (b->menu_name && ff_disc_add_prop(s, "Menu names", "%d playlists named by the menu's project data "
                                         "(project '%s')", b->nb_menu_named, b->menu_name) < 0)
        return AVERROR(ENOMEM);
    d->info.fake_playlists  = b->nb_fake;
    d->info.fakes_suspected = b->fake_suspected;
    if ((b->nb_fake && ff_disc_add_prop(s, "Fake playlists", "%d likely fake, %d likely real (the titles are marked)",
                                        b->nb_fake, b->nb_real) < 0) ||
        (b->fake_suspected && ff_disc_add_prop(s, "Fake playlists", "possibly (many different long clip orders, "
                                               "no known kind of fake)") < 0))
        return AVERROR(ENOMEM);
    d->info.name      = b->name;
    ff_disc_set_market(s, (const char (*)[4])b->meta_lang, b->nb_meta_lang, 0, "its disc library files");
    if (ff_disc_add_prop(s, "Kind", "%s", b->parent_fs ? "AVCHD" : b->bdav ? "BDAV (recorded)" : "BDMV") < 0 ||
        (b->name && b->name_lang[0] && ff_disc_add_prop(s, "Name's language", "%s", b->name_lang) < 0) ||
        (b->name_eng && (!b->name || strcmp(b->name_eng, b->name)) &&
         ff_disc_add_prop(s, "English name", "%s", b->name_eng) < 0) ||
        ff_disc_add_prop(s, "Protection: AACS", "%s", b->aacs ? "yes" : "no") < 0 ||
        (c->aacs && ff_disc_add_prop(s, "Content hash check", "%s", c->hash ? "yes: the stream files read are "
                                     "checked against the disc's content hashes" : "no table") < 0) ||
        ff_disc_add_prop(s, "Protection: AACS 2.0", "not checked yet") < 0 ||
        ff_disc_add_prop(s, "Protection: BD+", "%s", b->bdplus ? "yes (not supported yet)" : "no") < 0 ||
        ff_disc_add_prop(s, "Protection: bus encryption", "not checked yet") < 0 ||
        ff_disc_add_prop(s, "Playlists", "%d", b->nb_playlists) < 0 ||
        ff_disc_add_prop(s, "Clips", "%d", b->nb_clips) < 0 ||
        (b->nb_jars && ff_disc_add_prop(s, "BD-J archives", "%d", b->nb_jars) < 0))
        return AVERROR(ENOMEM);
    if (b->has_disc_id) {
        char id[41];
        for (int i = 0; i < 20; i++)
            snprintf(id + 2 * i, 3, "%02X", b->disc_id[i]);
        if (ff_disc_add_prop(s, "Protection: AACS disc ID", "%s", id) < 0)
            return AVERROR(ENOMEM);
    }
    d->info.encrypted = has_dir(d->fs, "/AACS") || has_dir(d->fs, "/AACS_bd") ||
                        has_dir(d->fs, "/AACS2_av") || has_dir(d->fs, "/AACS2_mv");
    return 0;
}

const DiscFormat ff_disc_bluray = {
    .name        = "Blu-ray",
    .log_name    = "Blu-ray",
    .kind        = DISCIO_DISC_BLURAY,
    .priv_size     = sizeof(BlurayDemuxContext),
    .load          = bluray_load,
    .nb_titles     = nb_titles,
    .title_name    = title_name,
    .title_tracks  = title_tracks_of,
    .title_segment = title_segment,
    .segment_close = segment_close,
    .remove_title  = remove_title,
    .title_info    = title_info,
    .open_title    = open_title,
    .close_title   = close_title,
    .close         = bluray_close,
};
