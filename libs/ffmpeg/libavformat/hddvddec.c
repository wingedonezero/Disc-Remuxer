/*
 * HD DVD (Advanced Content): the format part of the disc layer (disc.h).
 * On the disc the disc layer opened it reads the title set's structure
 * (VTI), the playlists, the AACS keys, the title plan and the titles'
 * tracks; one title's tracks become the streams. The disc layer's title
 * reader reads its EVOB clips (DiscTitleSource: the blocks of a clip, AACS
 * decrypted here; LPCM's 5-byte audio frame header; the title's chapter
 * marks).
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
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/dict.h"

#include "avformat.h"
#include "disc.h"
#include "discinfo.h"
#include "discio.h"
#include "disctl.h"
#include "hddvd_internal.h"
#include "internal.h"

#define HDDVD_BLOCK 2048

/* The stream pass's reading: an EVOB's blocks as the title reader reads them. */
typedef struct PassRead {
    void      *log;
    HDDVDAACS *aacs;
    DiscIOFS  *fs;
    HDDVDClip *clip;
} PassRead;

typedef struct HDDVDDemuxContext {
    DiscIOFS      *fs;              /**< the disc layer's */
    HDDVDVTI      *vti;
    HDDVDXpl     **xpls;
    int            nb_xpls;
    HDDVDAACS     *aacs;
    HDDVDTitlePlan *plan;
    PassRead       pass;            /**< the stream pass's EVOB */

    /* the opened title, as the title reader gets it (DiscContext.src) */
    const HDDVDTitle *title;
    int64_t       *chapter_times;
    const char   **chapter_names;
} HDDVDDemuxContext;


static const char *clip_name(AVFormatContext *s, int k)
{
    HDDVDDemuxContext *c = ff_disc_part(s);
    return c->title->clips[k]->evob->name;
}

static uint32_t clip_blocks(AVFormatContext *s, int k)
{
    HDDVDDemuxContext *c = ff_disc_part(s);
    return c->title->clips[k]->size / HDDVD_BLOCK;
}

static int read_block(AVFormatContext *s, int k, uint32_t block, uint8_t *buf)
{
    HDDVDDemuxContext *c = ff_disc_part(s);
    return ff_hddvd_clip_block(s, c->aacs, c->fs, c->title->clips[k], block, buf);
}

/* Where the disc starts clip k: its EVOB's presentation start (VTI), 90 kHz. */
static int64_t clip_start(AVFormatContext *s, int k)
{
    HDDVDDemuxContext *c = ff_disc_part(s);
    return c->title->clips[k]->evob->start_ptm;
}

/* The opened title's chapter marks given back. */
static void close_title(AVFormatContext *s)
{
    HDDVDDemuxContext *c = ff_disc_part(s);

    av_freep(&c->chapter_times);
    av_freep(&c->chapter_names);
    c->title = NULL;
}

static void hddvd_close(AVFormatContext *s)
{
    HDDVDDemuxContext *c = ff_disc_part(s);

    close_title(s);
    ff_hddvd_titles_free(&c->plan);
    ff_hddvd_aacs_close(&c->aacs);
    ff_hddvd_xpl_free_all(&c->xpls, c->nb_xpls);
    ff_hddvd_vti_free(&c->vti);
}

/* The id the disc sector reader (mpeg.c) gives a track's packets: video stream_id
 * 0xE0 (MPEG-2) / 0xE2 (AVC) / 0xFD with extension 0x55 (VC-1); private
 * stream 1 sub-streams AC-3 0x80+n, DTS 0x88+n, LPCM 0xA0+n, MLP 0xB0+n,
 * Dolby Digital Plus 0xC0+n, sub-pictures 0x20+n; MPEG audio stream_id
 * 0xC0+n. */
static int pes_id(const HDDVDTrack *k)
{
    static const int audio_base[8] = { 0x80, 0xb0, 0x1c0, -1, 0xa0, 0xa0, 0x88, 0xc0 };

    switch (k->type) {
    case AVMEDIA_TYPE_VIDEO:
        return k->codec == AV_CODEC_ID_VC1 ? 0xfd55 : k->codec == AV_CODEC_ID_H264 ? 0x1e2 : 0x1e0;
    case AVMEDIA_TYPE_AUDIO:
        return k->coding >= 0 && k->coding < 8 && audio_base[k->coding] >= 0 ? audio_base[k->coding] + k->number : -1;
    case AVMEDIA_TYPE_SUBTITLE:
        return 0x20 + k->number;
    default:
        return -1;
    }
}

/* What a sub-picture track's units are, for the caller's log (coding mode
 * p[0] >> 5 of its attribute record: 0..7). */
static const char *const subpic_kind[8] = {
    "HD DVD sub-pictures of coding 0 (not met on a real disc)",
    "HD DVD sub-pictures of coding 1 (not met on a real disc)",
    "HD DVD sub-pictures of coding 2 (not met on a real disc)",
    "HD DVD sub-pictures of coding 3 (not met on a real disc)",
    "HD DVD sub-pictures: 256 colours, each with its own palette",      /* HDDVD_SUBPIC_CODING_HD */
    "HD DVD sub-pictures of coding 5 (not met on a real disc)",
    "HD DVD sub-pictures of coding 6 (not met on a real disc)",
    "HD DVD sub-pictures of coding 7 (not met on a real disc)",
};

/* A title's tracks as the disc layer takes them (out: t->nb_tracks). */
static void title_tracks(const HDDVDDemuxContext *c, const HDDVDTitle *t, DiscTrack *out)
{
    for (int i = 0; i < t->nb_tracks; i++) {
        const HDDVDTrack *k = &t->tracks[i];
        DiscTrack *o = &out[i];

        memset(o, 0, sizeof(*o));
        ff_disc_stream_init(&o->info);
        o->info.type        = k->type;
        o->info.codec       = k->codec;
        o->info.inner_track = k->inner_track;
        o->info.from_track  = k->from_track;
        memcpy(o->info.language, k->lang, sizeof(o->info.language));
        o->id     = pes_id(k);
        o->kind   = k->type == AVMEDIA_TYPE_VIDEO ? DR_KIND_VIDEO :
                    k->type == AVMEDIA_TYPE_AUDIO ? DR_KIND_AUDIO : DR_KIND_SUBTITLE;
        o->stream = k->number;
        o->number = k->index;
        if (k->type == AVMEDIA_TYPE_VIDEO) {
            /* the video attribute's line-21 caption marks (byte 0x03 bit 7:
             * field 1, bit 6: field 2, laid out as in DVD-Video's video
             * attributes; on the corpus they go with caption blocks in the
             * video, which HD DVD titles do not keep) */
            uint8_t v = c->vti->attrs[t->clips[t->segment]->evob->attr - 1].raw[3];
            o->line21 = (v & 0xC0) == 0xC0 ? "fields 1 and 2" : v & 0x80 ? "field 1" : v & 0x40 ? "field 2" : NULL;
        } else if (k->type == AVMEDIA_TYPE_SUBTITLE) {
            o->info.width  = k->width;
            o->info.height = k->height;
            o->palette     = k->palette;
            o->picture     = subpic_kind[k->coding & 7];
        }
    }
}

/* The pass's reading: the clip's blocks as the title reader reads them. */
static int pass_read(void *opaque, uint32_t block, uint8_t *buf)
{
    PassRead *r = opaque;
    if (block >= r->clip->size / HDDVD_BLOCK)
        return AVERROR_EOF;
    return ff_hddvd_clip_block(r->log, r->aacs, r->fs, r->clip, block, buf);
}

/* ---- the title entries (disc.h) ---- */

static int nb_titles(AVFormatContext *s)
{
    return ((HDDVDDemuxContext *)ff_disc_part(s))->plan->nb_titles;
}

static const char *title_name(AVFormatContext *s, int i)
{
    return ((HDDVDDemuxContext *)ff_disc_part(s))->plan->titles[i].name;
}

static int title_tracks_of(AVFormatContext *s, int i, DiscTrack *out, int out_size)
{
    HDDVDDemuxContext *c = ff_disc_part(s);
    const HDDVDTitle *t = &c->plan->titles[i];
    if (t->nb_tracks > out_size)
        return AVERROR_BUG;
    title_tracks(c, t, out);
    return t->nb_tracks;
}

/* The EVOB the tracks come from (the largest), read as the title reader
 * reads it; HD DVD's 5-byte LPCM header. */
static int title_segment(AVFormatContext *s, int i, DiscPassSource *src)
{
    HDDVDDemuxContext *c = ff_disc_part(s);
    const HDDVDTitle *t = &c->plan->titles[i];

    c->pass = (PassRead){ s, c->aacs, c->fs, t->clips[t->segment] };
    *src = (DiscPassSource){ .index = t->segment, .lpcm_header = 5, .key = (uintptr_t)c->pass.clip,
                             .read = pass_read, .opaque = &c->pass };
    return 1;
}

static void remove_title(AVFormatContext *s, int i)
{
    HDDVDTitlePlan *plan = ((HDDVDDemuxContext *)ff_disc_part(s))->plan;
    HDDVDTitle *t = &plan->titles[i];

    av_free(t->clips);
    av_free(t->marks);
    av_free(t->tracks);
    memmove(t, t + 1, (plan->nb_titles - i - 1) * sizeof(*t));
    plan->nb_titles--;
}

/* The disc info's title: name, language, duration, size, whether selected,
 * the EVOB files, chapter records. */
static int title_info(AVFormatContext *s, int i, DiscTitleInfo *dt)
{
    const HDDVDTitle *t = &((HDDVDDemuxContext *)ff_disc_part(s))->plan->titles[i];

    dt->name     = t->name && *t->name ? t->name : NULL;
    dt->duration = av_rescale(t->duration, AV_TIME_BASE, 90000);
    dt->size     = t->size;
    dt->selected = !t->not_selected;
    if (t->lang)
        av_strlcpy(dt->language, t->lang, sizeof(dt->language));
    if (ff_disc_title_add_prop(dt, "From", "%s", t->from_playlist ? "a title of the playlist" :
                               "an EVOB no playlist title plays") < 0 ||
        (t->segment >= 0 && t->segment < t->nb_clips &&
         ff_disc_title_add_prop(dt, "Tracks described from", "%s", t->clips[t->segment]->evob->name) < 0))
        return AVERROR(ENOMEM);
    for (int k = 0; k < t->nb_clips; k++)
        if (ff_disc_title_add_file(dt, t->clips[k]->evob->name) < 0)
            return AVERROR(ENOMEM);
    for (int k = 0; k < t->nb_marks; k++) {
        DiscChapterInfo *ch = ff_disc_title_add_chapter(dt);
        if (!ch)
            return AVERROR(ENOMEM);
        ch->start = (int64_t)t->marks[k].ms * (AV_TIME_BASE / 1000);
        ch->name  = t->marks[k].name && *t->marks[k].name ? t->marks[k].name : NULL;
    }
    return 0;
}

/* The opened title's source (its streams and tracks: the disc layer's):
 * its clips and chapter marks, HD DVD's LPCM header. */
static int open_title(AVFormatContext *s, int i, int part, int *second_view)
{
    DiscContext *d = s->priv_data;
    HDDVDDemuxContext *c = ff_disc_part(s);
    const HDDVDTitle *t = c->title = &c->plan->titles[i];

    if (!(c->chapter_times = av_calloc(FFMAX(t->nb_marks, 1), sizeof(*c->chapter_times))) ||
        !(c->chapter_names = av_calloc(FFMAX(t->nb_marks, 1), sizeof(*c->chapter_names))))
        return AVERROR(ENOMEM);
    for (int k = 0; k < t->nb_marks; k++) {
        c->chapter_times[k] = (int64_t)t->marks[k].ms * (DR_TICKS_PER_SECOND / 1000);
        c->chapter_names[k] = t->marks[k].name;
    }
    s->duration = av_rescale(t->duration, AV_TIME_BASE, 90000);
    d->src = (DiscTitleSource){
        .nb_clips      = t->nb_clips,
        .clip_kind     = "EVOB",
        .clip_name     = clip_name,
        .clip_blocks   = clip_blocks,
        .read_block    = read_block,
        .clip_start    = clip_start,
        .clip_start_name = "its EVOB's start time (VTI)",
        .lpcm_header   = 5,
        .chapter_times = c->chapter_times,
        .chapter_names = (const char *const *)c->chapter_names,
        .nb_chapters   = t->nb_marks,
    };
    return 0;
}

static int hddvd_load(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    HDDVDDemuxContext *c = ff_disc_part(s);
    int ret;

    c->fs = d->fs;
    if ((ret = ff_hddvd_vti_open(s, c->fs, &c->vti)) < 0)
        return ret;
    if ((ret = ff_hddvd_xpl_load(s, c->fs, &c->xpls, &c->nb_xpls)) < 0)
        return ret;
    ff_hddvd_evob_marks(s, c->vti, c->xpls, c->nb_xpls);
    if ((ret = ff_hddvd_aacs_open(s, c->fs, (const char *const *)d->opt_keydb, d->nb_opt_keydb,
                                  c->nb_xpls, &c->aacs)) < 0)
        return ret;
    d->info.encrypted = c->aacs != NULL;
    if (ff_disc_add_prop(s, "Protection: AACS", "%s", c->aacs ? "yes" : "no") < 0)
        return AVERROR(ENOMEM);
    if ((ret = ff_hddvd_titles_plan(s, c->fs, c->vti, c->xpls, c->nb_xpls, d->opt_min_length, &c->plan)) < 0)
        return ret;
    if (c->plan->nb_titles && c->plan->titles[0].lang) {
        char lang[1][4];
        av_strlcpy(lang[0], c->plan->titles[0].lang, sizeof(lang[0]));
        ff_disc_set_market(s, (const char (*)[4])lang, 1, 1, "its title set's default language");
    }
    return ff_hddvd_tracks_build(s, c->fs, c->aacs, c->vti, c->xpls, c->nb_xpls, c->plan);
}

const DiscFormat ff_disc_hddvd = {
    .name        = "HD DVD",
    .log_name    = "HD DVD",
    .kind        = DISCIO_DISC_HDDVD,
    .priv_size     = sizeof(HDDVDDemuxContext),
    .load          = hddvd_load,
    .nb_titles     = nb_titles,
    .title_name    = title_name,
    .title_tracks  = title_tracks_of,
    .title_segment = title_segment,
    .remove_title  = remove_title,
    .title_info    = title_info,
    .open_title    = open_title,
    .close_title   = close_title,
    .close         = hddvd_close,
};
