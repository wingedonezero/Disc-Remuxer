/*
 * HD DVD (Advanced Content): the format part of the disc layer (disc.h).
 * On the disc the disc layer opened it reads the title set's structure
 * (VTI), the playlists, the AACS keys, the title plan and the titles'
 * tracks; one title's tracks become the streams. The disc layer's title
 * reader reads its EVOB clips (DiscTitleSource: the blocks of a clip, AACS
 * decrypted here; LPCM's 5-byte audio frame header; the 102 ms audio
 * lead-in tolerance; the title's chapter marks).
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
#include "discrip.h"
#include "hddvd_internal.h"
#include "internal.h"

#define HDDVD_BLOCK 2048
#define LEAD_IN_TOLERANCE 110160000LL     /* HD DVD audio lead-in tolerance: 102 ms (ticks) */

typedef struct HDDVDDemuxContext {
    DiscIOFS      *fs;              /**< the disc layer's */
    HDDVDVTI      *vti;
    HDDVDXpl     **xpls;
    int            nb_xpls;
    HDDVDAACS     *aacs;
    HDDVDTitlePlan *plan;

    /* the opened title, as the title reader gets it (DiscContext.src) */
    const HDDVDTitle *title;
    DRTitleTrack  *tracks;
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

static void hddvd_close(AVFormatContext *s)
{
    HDDVDDemuxContext *c = ff_disc_part(s);

    av_freep(&c->tracks);
    av_freep(&c->chapter_times);
    av_freep(&c->chapter_names);
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

static int add_streams(AVFormatContext *s, const HDDVDTitle *t)
{
    HDDVDDemuxContext *c = ff_disc_part(s);

    for (int i = 0; i < t->nb_tracks; i++) {
        const HDDVDTrack *k = &t->tracks[i];
        AVStream *st = avformat_new_stream(s, NULL);

        if (!st)
            return AVERROR(ENOMEM);
        if (k->type == AVMEDIA_TYPE_VIDEO) {
            /* the video attribute's line-21 caption marks (byte 0x03 bit 7:
             * field 1, bit 6: field 2, laid out as in DVD-Video's video
             * attributes; on the corpus they go with caption blocks in the
             * video, which HD DVD titles do not keep) */
            uint8_t v = c->vti->attrs[t->clips[t->segment]->evob->attr - 1].raw[3];
            const char *l21 = (v & 0xC0) == 0xC0 ? "fields 1 and 2" : v & 0x80 ? "field 1" : v & 0x40 ? "field 2" : NULL;
            if (l21 && av_dict_set(&st->metadata, "line21", l21, 0) < 0)
                return AVERROR(ENOMEM);
        }
        st->id                    = pes_id(k);
        st->codecpar->codec_type  = k->type;
        st->codecpar->codec_id    = k->codec;
        avpriv_set_pts_info(st, 64, 1, DR_TICKS_PER_SECOND);
        if (k->from_track >= 0)
            st->disposition |= AV_DISPOSITION_DEPENDENT;
        if (k->type == AVMEDIA_TYPE_SUBTITLE) {
            char idx[1024];
            int n = ff_discrip_vobsub_header(idx, sizeof(idx), k->width, k->height, k->palette);
            int ret;

            st->codecpar->width   = k->width;
            st->codecpar->height  = k->height;
            char kind[96];
            /* what the units are, for the caller's log */
            if (k->coding == HDDVD_SUBPIC_CODING_HD)
                snprintf(kind, sizeof(kind), "HD DVD sub-pictures: 256 colours, each with its own palette");
            else
                snprintf(kind, sizeof(kind), "HD DVD sub-pictures of coding %d (not met on a real disc)", k->coding);
            if ((ret = av_dict_set(&st->metadata, "picture", kind, 0)) < 0)
                return ret;
            /* the VobSub index header (FFmpeg's DVD subtitle decoder reads
             * size and palette from it) */
            if (n < 0 || n >= sizeof(idx))
                return AVERROR_BUG;
            if ((ret = ff_alloc_extradata(st->codecpar, n)) < 0)
                return ret;
            memcpy(st->codecpar->extradata, idx, n);
        }
        if (k->lang[0] && av_dict_set(&st->metadata, "language", k->lang, 0) < 0)
            return AVERROR(ENOMEM);
    }
    return 0;
}

/* The disc info's titles: name, language, duration, size, whether
 * selected, the EVOB files, chapter records, streams (their details are not
 * known before the title is read). */
static int describe_titles(AVFormatContext *s)
{
    HDDVDDemuxContext *c = ff_disc_part(s);

    ((DiscContext *)s->priv_data)->info.encrypted = c->aacs != NULL;
    for (int i = 0; i < c->plan->nb_titles; i++) {
        const HDDVDTitle *t = &c->plan->titles[i];
        DiscTitleInfo *dt = ff_disc_add_title(s);

        if (!dt)
            return AVERROR(ENOMEM);
        dt->name     = t->name && *t->name ? t->name : NULL;
        dt->duration = av_rescale(t->duration, AV_TIME_BASE, 90000);
        dt->size     = t->size;
        dt->selected = !t->not_selected;
        if (t->lang)
            av_strlcpy(dt->language, t->lang, sizeof(dt->language));
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
        for (int k = 0; k < t->nb_tracks; k++) {
            const HDDVDTrack *tk = &t->tracks[k];
            DiscStreamInfo *ds = ff_disc_title_add_stream(dt);
            if (!ds)
                return AVERROR(ENOMEM);
            ds->type   = tk->type;
            ds->codec  = tk->codec;
            ds->inner_track = tk->inner_track;
            ds->from_track  = tk->from_track;
            if (tk->type == AVMEDIA_TYPE_SUBTITLE) {
                ds->width  = tk->width;
                ds->height = tk->height;
            }
            memcpy(ds->language, tk->lang, sizeof(ds->language));
        }
    }
    return 0;
}

/* The opened title for the title reader: its clips, tracks (track 0 is
 * the title's video) and chapter marks, HD DVD's LPCM header and lead-in
 * tolerance. */
static int title_source(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    HDDVDDemuxContext *c = ff_disc_part(s);
    const HDDVDTitle *t = c->title;

    if (!(c->chapter_times = av_calloc(FFMAX(t->nb_marks, 1), sizeof(*c->chapter_times))) ||
        !(c->chapter_names = av_calloc(FFMAX(t->nb_marks, 1), sizeof(*c->chapter_names))) ||
        !(c->tracks = av_calloc(t->nb_tracks, sizeof(*c->tracks))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < t->nb_marks; i++) {
        c->chapter_times[i] = (int64_t)t->marks[i].ms * (DR_TICKS_PER_SECOND / 1000);
        c->chapter_names[i] = t->marks[i].name;
    }
    for (int i = 0; i < t->nb_tracks; i++) {
        const HDDVDTrack *k = &t->tracks[i];
        c->tracks[i].codec       = k->codec;
        c->tracks[i].kind        = k->type == AVMEDIA_TYPE_VIDEO ? DR_KIND_VIDEO :
                                   k->type == AVMEDIA_TYPE_AUDIO ? DR_KIND_AUDIO : DR_KIND_SUBTITLE;
        c->tracks[i].audio_flags = (k->from_track >= 0 ? DR_AUDIO_EXTRACTED : 0) |
                                   (k->from_track >= 0 || k->inner_track >= 0 ? DR_AUDIO_EXTENDED : 0);
        c->tracks[i].width       = k->width;
        c->tracks[i].height      = k->height;
    }
    d->src = (DiscTitleSource){
        .nb_clips      = t->nb_clips,
        .clip_kind     = "EVOB",
        .clip_name     = clip_name,
        .clip_blocks   = clip_blocks,
        .read_block    = read_block,
        .lpcm_header   = 5,
        .tolerance     = LEAD_IN_TOLERANCE,
        .tracks        = c->tracks,
        .nb_tracks     = t->nb_tracks,
        .chapter_times = c->chapter_times,
        .chapter_names = (const char *const *)c->chapter_names,
        .nb_chapters   = t->nb_marks,
    };
    return 0;
}

static int hddvd_open(AVFormatContext *s)
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
    if ((ret = ff_hddvd_titles_plan(s, c->fs, c->vti, c->xpls, c->nb_xpls, d->opt_min_length, &c->plan)) < 0)
        return ret;
    if ((ret = ff_hddvd_tracks_build(s, c->fs, c->aacs, c->vti, c->xpls, c->nb_xpls, c->plan)) < 0)
        return ret;
    if ((ret = describe_titles(s)) < 0)
        return ret;
    if (d->opt_title < 0)               /* only the disc info, no title opened */
        return 0;
    if (d->opt_title >= c->plan->nb_titles) {
        av_log(s, AV_LOG_ERROR, "HD DVD: title %d does not exist (the disc has %d)\n", d->opt_title,
               c->plan->nb_titles);
        return AVERROR(EINVAL);
    }
    c->title = &c->plan->titles[d->opt_title];
    if ((ret = add_streams(s, c->title)) < 0)
        return ret;
    s->duration = av_rescale(c->title->duration, AV_TIME_BASE, 90000);
    return title_source(s);
}

const DiscFormat ff_disc_hddvd = {
    .name        = "HD DVD",
    .kind        = DISCIO_DISC_HDDVD,
    .priv_size   = sizeof(HDDVDDemuxContext),
    .open        = hddvd_open,
    .close       = hddvd_close,
};
