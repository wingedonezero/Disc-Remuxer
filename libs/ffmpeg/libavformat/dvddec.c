/*
 * DVD-Video: the format part of the disc layer (disc.h). It reads the
 * disc's structure (dvd_disc.c: the VMG, the title table, the text data
 * and every title set's IFO with its checks) and describes the disc: its
 * label is the volume label (a folder: the VMG's provider identifier), its
 * name the text data's when it has one; its title list (dvd_titles.c) with
 * each title's tracks after its probe (dvd_streams.c: captions, audio
 * streams without data) and the disc layer's stream pass (discpass.c:
 * duplicates, details, data). An opened
 * title is read by the disc layer's title reader: its segments one after
 * another, each sector through dvd_read.c (the sector check, CSS).
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
#include "libavutil/intreadwrite.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "disc.h"
#include "disclang.h"
#include "disctl.h"
#include "dvd_internal.h"
#include "internal.h"


/* The stream pass's reading: a segment's sectors as the title reader reads them. */
typedef struct PassRead {
    DVDDisc        *disc;
    const DVDTitle *t;
    int             seg;
    uint32_t        nb_blocks;
} PassRead;

typedef struct DVDDemuxContext {
    DVDDisc      disc;
    DVDScan      scan;
    DVDTitleList titles;
    PassRead     pass;          /* the stream pass's segment */

    /* the opened title, as the title reader gets it (DiscContext.src) */
    const DVDTitle *title;
    int64_t        *chapter_times;
    int            *chapter_clips;
    int64_t        *chapter_offsets;
} DVDDemuxContext;

static const char *clip_name(AVFormatContext *s, int k)
{
    DVDDemuxContext *c = ff_disc_part(s);
    return c->title->segments[k].cell_map;
}

static uint32_t clip_blocks(AVFormatContext *s, int k)
{
    DVDDemuxContext *c = ff_disc_part(s);
    return ff_dvd_segment_blocks(&c->title->segments[k]);
}

static int read_block(AVFormatContext *s, int k, uint32_t block, uint8_t *buf)
{
    DVDDemuxContext *c = ff_disc_part(s);
    return ff_dvd_segment_read(&c->disc, c->title, k, block, buf);
}

static int block_place(AVFormatContext *s, int k, uint32_t block, char *file, int size, int64_t *offset)
{
    DVDDemuxContext *c = ff_disc_part(s);
    return ff_dvd_segment_place(&c->disc, c->title, k, block, file, size, offset);
}

/* Where the disc starts segment k: the presentation start of its first
 * VOBU (VOBU_S_PTM of the PCI in the navigation pack its first sector is:
 * the pack header, the system header, then private stream 2 with sub-stream
 * 0x00, DVD-Video Book), 90 kHz; none when that sector is no navigation
 * pack or cannot be read. */
static int64_t clip_start(AVFormatContext *s, int k)
{
    uint8_t b[DVD_SECTOR];

    if (read_block(s, k, 0, b) <= 0 || AV_RB32(b) != 0x1ba || AV_RB32(b + 0x26) != 0x1bf || b[0x2c])
        return AV_NOPTS_VALUE;
    return AV_RB32(b + 0x2d + 0x0c);
}

/* The opened title's chapter records given back. */
static void close_title(AVFormatContext *s)
{
    DVDDemuxContext *c = ff_disc_part(s);
    av_freep(&c->chapter_times);
    av_freep(&c->chapter_clips);
    av_freep(&c->chapter_offsets);
    c->title = NULL;
}

static void dvd_close(AVFormatContext *s)
{
    DVDDemuxContext *c = ff_disc_part(s);
    close_title(s);
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

#define MAX_TRACKS (1 + 8 + 33 + 1)

/* The title's tracks as its title set's attributes describe them: the
 * video, the audio streams (without those the probe found without data),
 * the sub-pictures, then the caption track when the probe found captions
 * (the stream pass then gives their details and data). Packet ids: video
 * 0x1E0, MPEG audio 0x1C0 + n, private stream 1 sub-streams AC-3 0x80 + n,
 * DTS 0x88 + n, LPCM 0xA0 + n, sub-pictures 0x20 + n. */
static int title_tracks(AVFormatContext *s, const DVDTitle *t, DiscTrack *out)
{
    static const enum AVCodecID codecs[8] = { AV_CODEC_ID_AC3, AV_CODEC_ID_NONE, AV_CODEC_ID_MP2, AV_CODEC_ID_MP2,
                                              AV_CODEC_ID_PCM_DVD, AV_CODEC_ID_NONE, AV_CODEC_ID_DTS,
                                              AV_CODEC_ID_NONE };
    static const int id_base[8] = { 0x80, 0, 0x1c0, 0x1c0, 0xa0, 0, 0x88, 0 };
    const uint8_t *h = t->vts->header;
    int n = 0;
    DiscStreamInfo *k;

    memset(out, 0, MAX_TRACKS * sizeof(*out));
    for (int i = 0; i < MAX_TRACKS; i++)
        ff_disc_stream_init(&out[i].info);
    k = &out[n].info;
    k->type  = AVMEDIA_TYPE_VIDEO;
    k->codec = (h[DVD_VTS_VIDEO_ATTR] >> 6) == 0 ? AV_CODEC_ID_MPEG1VIDEO :
               (h[DVD_VTS_VIDEO_ATTR] >> 6) == 1 ? AV_CODEC_ID_MPEG2VIDEO : AV_CODEC_ID_NONE;
    out[n].id   = 0x1e0;
    out[n].kind = DR_KIND_VIDEO;
    n++;
    for (int i = 0; i < t->nb_audio; i++) {
        const uint8_t *a = h + DVD_VTS_AUDIO_ATTR + 8 * t->audio[i].attr;

        if (t->audio_dropped >> i & 1)
            continue;
        k = &out[n].info;
        k->type  = AVMEDIA_TYPE_AUDIO;
        k->codec = codecs[a[0] >> 5];
        if ((a[0] & 0x0c) == 4)
            attr_language(s, k->language, a + 2);
        k->roles = audio_roles(a[5]);
        out[n].id     = id_base[a[0] >> 5] + t->audio[i].stream;
        out[n].kind   = DR_KIND_AUDIO;
        out[n].stream = t->audio[i].stream;
        out[n].number = 1 + i;
        n++;
    }
    for (int i = 0; i < t->nb_subp; i++) {
        const uint8_t *p = h + DVD_VTS_SUBP_ATTR + 6 * t->subp[i].attr;
        k = &out[n].info;
        k->type   = AVMEDIA_TYPE_SUBTITLE;
        k->codec  = AV_CODEC_ID_DVD_SUBTITLE;
        k->width  = t->subp_width;
        k->height = t->subp_height;
        if ((p[0] & 3) == 1)
            attr_language(s, k->language, p + 2);
        k->roles = subp_roles(p[5]);
        out[n].id      = 0x20 + t->subp[i].stream;
        out[n].kind    = DR_KIND_SUBTITLE;
        out[n].stream  = t->subp[i].stream;
        out[n].number  = 1 + t->nb_audio + i;
        out[n].palette = t->pgc->palette;
        out[n].picture = "DVD sub-pictures: 4 colours of the program chain's 16-entry palette";
        n++;
    }
    if (t->captions) {
        k = &out[n].info;
        k->type  = AVMEDIA_TYPE_SUBTITLE;
        k->codec = AV_CODEC_ID_EIA_608;
        av_strlcpy(k->language, "eng", sizeof(k->language));
        out[n].id      = -1;
        out[n].kind    = DR_KIND_CAPTION;
        out[n].caption = "line-21 closed captions (CEA-608) in the video, made into text";
        n++;
    }
    return n;
}

/* ---- the title entries (disc.h) ---- */

static int nb_titles(AVFormatContext *s)
{
    return ((DVDDemuxContext *)ff_disc_part(s))->titles.nb_titles;
}

static const char *title_name(AVFormatContext *s, int i)
{
    return ((DVDDemuxContext *)ff_disc_part(s))->titles.titles[i]->name;
}

/* The probe of the title's streams (dvd_streams.c: captions, audio streams
 * without data). */
static int title_probe(AVFormatContext *s, int i)
{
    DVDDemuxContext *c = ff_disc_part(s);
    return ff_dvd_title_streams(&c->disc, c->titles.titles[i]);
}

static int title_tracks_of(AVFormatContext *s, int i, DiscTrack *out, int out_size)
{
    DVDDemuxContext *c = ff_disc_part(s);
    if (out_size < MAX_TRACKS)
        return AVERROR_BUG;
    return title_tracks(s, c->titles.titles[i], out);
}


static int pass_read(void *opaque, uint32_t block, uint8_t *buf)
{
    PassRead *r = opaque;
    if (block >= r->nb_blocks)
        return AVERROR_EOF;
    return ff_dvd_segment_read(r->disc, r->t, r->seg, block, buf);
}

/* A segment's identity: its title set and every extent it plays (FNV-1a). */
static uint64_t segment_key(const DVDTitle *t, const DVDSegment *g)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    uint32_t v[3];

    for (int i = -1; i < g->nb_extents; i++) {
        if (i < 0) {
            v[0] = t->vtsn;
            v[1] = v[2] = 0;
        } else {
            v[0] = g->extents[i].logical;
            v[1] = g->extents[i].sector;
            v[2] = g->extents[i].count;
        }
        for (int j = 0; j < sizeof(v); j++) {
            h ^= ((const uint8_t *)v)[j];
            h *= 0x100000001b3ULL;
        }
    }
    return h ? h : 1;
}

/* The title's largest segment (the first of equal ones), read as the
 * title reader reads it; DVD-Video's 3-byte LPCM header; empty sub-picture
 * tracks reported (read on to the segment's end). */
static int title_segment(AVFormatContext *s, int i, DiscPassSource *src)
{
    DVDDemuxContext *c = ff_disc_part(s);
    const DVDTitle *t = c->titles.titles[i];
    int64_t best = -1;
    int seg = 0;

    if (!t->nb_segments)
        return 0;
    for (int k = 0; k < t->nb_segments; k++)
        if (t->segments[k].size > best) {
            best = t->segments[k].size;
            seg = k;
        }
    c->pass = (PassRead){ &c->disc, t, seg, ff_dvd_segment_blocks(&t->segments[seg]) };
    *src = (DiscPassSource){ .index = seg, .lpcm_header = 3, .read_all = 1, .key = segment_key(t, &t->segments[seg]),
                             .read = pass_read, .opaque = &c->pass };
    return 1;
}

static void remove_title(AVFormatContext *s, int i)
{
    DVDTitleList *l = &((DVDDemuxContext *)ff_disc_part(s))->titles;
    ff_dvd_title_free(&l->titles[i]);
    memmove(l->titles + i, l->titles + i + 1, (l->nb_titles - i - 1) * sizeof(*l->titles));
    l->nb_titles--;
}

/* The disc info's title: length (the cells' playback times), size (the
 * title VOB sectors it plays), angle, whether selected, the program chain
 * and the navigation's recording it comes from, its segments' cells (as its
 * "files"), its chapters. */
static int title_info(AVFormatContext *s, int i, DiscTitleInfo *dt)
{
    DVDTitle *t = ((DVDDemuxContext *)ff_disc_part(s))->titles.titles[i];

    dt->duration = (int64_t)t->declared * AV_TIME_BASE;
    dt->angle    = t->nr_of_angles > 1 ? t->angle + 1 : 0;
    dt->selected = !t->short_title;
    if (t->scan)
        snprintf(t->source, sizeof(t->source), "VTS_%02d program chain %d (navigation %s), cells", t->vtsn,
                 t->pgc->number + 1, t->scan->name);
    else
        snprintf(t->source, sizeof(t->source), "VTS_%02d program chain %d, cells", t->vtsn, t->pgc->number + 1);
    dt->source = t->source;
    if (ff_disc_title_add_prop(dt, "Title on the disc", "%s", t->name) < 0 ||
        ff_disc_title_add_prop(dt, "Title set", "VTS_%02d, its title %d", t->vtsn, t->vts_ttn) < 0 ||
        ff_disc_title_add_prop(dt, "Program chain", "%d", t->pgc->number + 1) < 0 ||
        ff_disc_title_add_prop(dt, "Navigation", "%s", t->scan ? t->scan->name : "not reached (cells trimmed)") < 0 ||
        (t->nr_of_angles > 1 && ff_disc_title_add_prop(dt, "Angles", "%d", t->nr_of_angles) < 0) ||
        ff_disc_title_add_prop(dt, "Cells", "%d", t->nb_cells) < 0 ||
        ff_disc_title_add_prop(dt, "Length measured", "%u:%02u:%02u (the program chain says %u:%02u:%02u)",
                               t->measured / 3600, t->measured / 60 % 60, t->measured % 60, t->declared / 3600,
                               t->declared / 60 % 60, t->declared % 60) < 0)
        return AVERROR(ENOMEM);
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
    return 0;
}

/* The opened title's source (its streams and tracks: the disc layer's):
 * its segments
 * one after another, DVD-Video's 3-byte LPCM header,
 * the chapter records placed on its segments. */
static int open_title(AVFormatContext *s, int i, int part, int *second_view)
{
    DiscContext *d = s->priv_data;
    DVDDemuxContext *c = ff_disc_part(s);
    const DVDTitle *t = c->titles.titles[i];
    int nb_ch = 0;

    c->title = t;
    if (!(c->chapter_times = av_calloc(FFMAX(t->nb_chapters, 1), sizeof(*c->chapter_times))) ||
        !(c->chapter_clips = av_calloc(FFMAX(t->nb_chapters, 1), sizeof(*c->chapter_clips))) ||
        !(c->chapter_offsets = av_calloc(FFMAX(t->nb_chapters, 1), sizeof(*c->chapter_offsets))))
        return AVERROR(ENOMEM);
    /* chapter records: positions (a segment and a byte offset in it; a
     * chapter starts at the first video frame from there), with their times
     * from the title's start */
    for (int k = 0; k < t->nb_chapters; k++, nb_ch++) {
        c->chapter_times[k]   = t->chapters[k].time * DR_TICKS_PER_PTS;
        c->chapter_clips[k]   = t->chapters[k].segment == DVD_CHAPTER_NONE ? -1 : t->chapters[k].segment;
        c->chapter_offsets[k] = t->chapters[k].offset;
    }
    s->duration = (int64_t)t->declared * AV_TIME_BASE;
    d->src = (DiscTitleSource){
        .nb_clips      = t->nb_segments,
        .clip_kind     = "segment",
        .clip_name     = clip_name,
        .clip_blocks   = clip_blocks,
        .read_block    = read_block,
        .block_place   = block_place,
        .clip_start    = clip_start,
        .clip_start_name = "its first VOBU's start time (navigation pack)",
        .lpcm_header   = 3,
        .chapter_times = c->chapter_times,
        .nb_chapters   = nb_ch,
        .chapter_clips   = c->chapter_clips,
        .chapter_offsets = c->chapter_offsets,
    };
    return 0;
}

/* The disc's menu languages, in the order its menu language units list
 * them (VMGM_PGCI_UT, else the first title set's VTSM_PGCI_UT): each unit's
 * lang_code (ISO 639-1). */
static void menu_languages(AVFormatContext *s, DVDDisc *disc)
{
    char codes[32][4];
    int n = 0;

    for (int i = 0; i < disc->nb_ifos && !n; i++) {
        DVDIfo *f = disc->ifos[i];
        uint8_t p[DVD_SECTOR];
        uint32_t sector;

        if (!f || f->failed || !(sector = ff_dvd_header32(f, i ? 0xD0 : 0xC8)) ||
            ff_dvd_ifo_read(disc, f, sector, p, sizeof(p)) < 0)
            continue;
        for (int k = 0; k < FFMIN(AV_RB16(p), 32) && 8 + 8 * k + 2 <= DVD_SECTOR; k++) {
            const uint8_t *lu = p + 8 + 8 * k;
            if (lu[0] >= 'a' && lu[0] <= 'z' && lu[1] >= 'a' && lu[1] <= 'z') {
                codes[n][0] = lu[0];
                codes[n][1] = lu[1];
                codes[n][2] = 0;
                n++;
            }
        }
    }
    ff_disc_set_market(s, (const char (*)[4])codes, n, 1, "its menus");
}

static int dvd_load(AVFormatContext *s)
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
    menu_languages(s, &c->disc);

    /* the titles: the navigation played (not for trimmed cells), then the
     * title list from it (a failed scan leaves no results) */
    if ((o.cell_mode != DVD_CELL_MODE_TRIM && ff_dvd_scan(&c->disc, NULL, NULL, &c->scan) == AVERROR(ENOMEM)) ||
        (ret = ff_dvd_titles_build(&c->disc, &c->scan, &o, &c->titles)) < 0)
        return ret < 0 ? ret : AVERROR(ENOMEM);
    if (av_log_get_level() >= AV_LOG_DEBUG && (dump = ff_dvd_titles_dump(&c->titles))) {
        debug_lines(s, "DVD titles:", dump);
        av_free(dump);
    }
    return 0;
}

/* Protection: CSS when a title set's scrambled sectors were met while the
 * stream passes read (its title key looked for). */
static int disc_done(AVFormatContext *s)
{
    DiscContext *d = s->priv_data;
    DVDDemuxContext *c = ff_disc_part(s);
    int css = 0;

    for (int i = 1; i < c->disc.nb_ifos; i++)
        css |= c->disc.ifos[i] && c->disc.ifos[i]->css_state != 0;
    d->info.encrypted = css;
    return ff_disc_add_prop(s, "Protection: CSS", "%s", css ? "yes" :
                            "no (no scrambled sector in what was read)");
}

const DiscFormat ff_disc_dvd = {
    .name          = "DVD-Video",
    .log_name      = "DVD",
    .kind          = DISCIO_DISC_DVD,
    .priv_size     = sizeof(DVDDemuxContext),
    .load          = dvd_load,
    .nb_titles     = nb_titles,
    .title_name    = title_name,
    .title_probe   = title_probe,
    .title_tracks  = title_tracks_of,
    .title_segment = title_segment,
    .remove_title  = remove_title,
    .disc_done     = disc_done,
    .title_info    = title_info,
    .open_title    = open_title,
    .close_title   = close_title,
    .close         = dvd_close,
};
