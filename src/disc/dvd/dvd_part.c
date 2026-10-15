/*
 * DVD-Video: the format part of the disc layer (disc.h). The DVD engine
 * (libdvdread, libdvdnav) reads and checks the disc, plays its navigation,
 * builds its title list and probes each title's streams, and says what it
 * finds in the program's log; this part asks for it (dvd_disc.c,
 * dvd_scan.c, dvd_title_list.c) and describes the disc from the engine's own
 * structures: its label is the volume label (a folder: the VMG's provider
 * identifier), its name the text data's when it has one; its titles with
 * their tracks (from the title sets' attributes, after the probe) and the
 * disc layer's stream pass (pass.c: duplicates, details, data). An
 * opened title is read by the disc layer's title reader: its segments one
 * after another, each sector through libdvdread (dvd_read.c: the sector
 * check, CSS).
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

#include <stdio.h>
#include <string.h>

#include "base/str.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/vlog.h"
#include "base/bytes.h"
#include "base/mem.h"
#include "base/mem.h"

#include "disc/model.h"
#include "disc/lang.h"
#include "timeline/disctl.h"
#include "base/str.h"
#include "disc/dvd/dvd_internal.h"


/* The stream pass's reading: a segment's sectors as the title reader reads them. */
typedef struct PassRead {
    DVDDisc        *disc;
    const dvd_title_t *t;
    int             seg;
    uint32_t        nb_blocks;
} PassRead;

typedef struct DVDDemuxContext {
    DVDDisc      disc;
    dvdnav_scan_results_t scan;
    dvd_title_list_t titles;    /* dvd_title_list.c's */
    char        (*sources)[96]; /* per title what it is made from, for the disc info */
    PassRead     pass;          /* the stream pass's segment */

    /* the opened title, as the title reader gets it (DiscContext.src) */
    const dvd_title_t *title;
    dvd_vob_reader_t *vobs;             /* a reading of its title VOBs (dvd_vob.c), for the title reader */
    dvd_vob_damage_t ended;             /* the damaged range its latest read ended (nb_sectors 0: none) */
    int             view_seg;           /* the segment its file view was set for (-1: none yet) */
    int64_t        *chapter_times;
    int            *chapter_clips;
    int64_t        *chapter_offsets;
} DVDDemuxContext;

static const char *clip_name(DrDisc *d, int k)
{
    DVDDemuxContext *c = ff_disc_part(d);
    return c->title->cell_maps[k];
}

static uint32_t clip_blocks(DrDisc *d, int k)
{
    DVDDemuxContext *c = ff_disc_part(d);
    return ff_dvd_segment_blocks(&c->title->segs.segments[k]);
}

static int read_block(DrDisc *d, int k, uint32_t block, uint8_t *buf)
{
    DVDDemuxContext *c = ff_disc_part(d);
    int ret;

    /* a new segment: libdvdread checks its file view (an image's VOB files) */
    if (k != c->view_seg) {
        if ((ret = ff_dvd_segment_view(c->title, c->vobs, k)) < 0)
            return ret;
        dr_vlog(d->log, DR_VLOG_DEBUG, "segment %s: %s\n", c->title->cell_maps[k],
               ret ? "a read that fails is tried through the VOB files" : "no second source");
        c->view_seg = k;
    }
    return ff_dvd_segment_read_vobs(&c->disc, c->title, c->vobs, k, block, buf, &c->ended);
}

/* Where block of segment k lies: the VOB file and the byte offset there. */
static int block_place(DrDisc *d, int k, uint32_t block, char *file, int size, int64_t *offset)
{
    DVDDemuxContext *c = ff_disc_part(d);
    return ff_dvd_segment_place(c->title, c->vobs, k, block, file, size, offset);
}

/* The damaged ranges as libdvdread names them: the one the latest read
 * ended; flush: the one being read ends too. */
static int damage(DrDisc *d, int k, int flush, DiscDamage *out)
{
    DVDDemuxContext *c = ff_disc_part(d);
    const dvd_vob_damage_t *g = &c->ended;

    if (!g->nb_sectors && flush)
        DVDEndTitleVOBDamage_rec(c->vobs, &c->ended);
    if (!g->nb_sectors)
        return 0;
    *out = (DiscDamage){ .kind = g->kind == DVD_DAMAGE_ZERO ? DISC_DAMAGE_ZERO : DISC_DAMAGE_NO_MPEG,
                         .blocks = g->nb_sectors };
    snprintf(out->file, sizeof(out->file), "%s", g->file);
    snprintf(out->last_file, sizeof(out->last_file), "%s", g->last_file);
    out->offset      = g->offset;
    out->last_offset = g->last_offset;
    c->ended.nb_sectors = 0;
    return 1;
}

/* Where the disc starts segment k: the presentation start of its first
 * VOBU (VOBU_S_PTM of the PCI in the navigation pack its first sector is:
 * the pack header, the system header, then private stream 2 with sub-stream
 * 0x00, DVD-Video Book), 90 kHz; none when that sector is no navigation
 * pack or cannot be read. */
static int64_t clip_start(DrDisc *d, int k)
{
    DVDDemuxContext *c = ff_disc_part(d);
    uint8_t b[DVD_SECTOR];

    /* (on the title set's own reading: the title reader's ranges stay its own) */
    if (ff_dvd_segment_read(&c->disc, c->title, k, 0, b) <= 0 || dr_rb32(b) != 0x1ba ||
        dr_rb32(b + 0x26) != 0x1bf || b[0x2c])
        return DR_NOPTS_VALUE;
    return dr_rb32(b + 0x2d + 0x0c);
}

/* The opened title's chapter records given back. */
static void close_title(DrDisc *d)
{
    DVDDemuxContext *c = ff_disc_part(d);
    dr_freep(&c->chapter_times);
    dr_freep(&c->chapter_clips);
    dr_freep(&c->chapter_offsets);
    DVDCloseTitleVOBs_rec(c->vobs);
    c->vobs  = NULL;
    c->title = NULL;
}

static void dvd_close(DrDisc *d)
{
    DVDDemuxContext *c = ff_disc_part(d);
    close_title(d);
    DVDFreeTitleList_rec(&c->titles);
    dr_freep(&c->sources);
    dvdnav_scan_free_results(&c->scan);
    ff_dvd_disc_free(&c->disc);
}

/* text to the debug log line by line (a log message has a size limit) */
static void debug_lines(DrDisc *d, const char *head, const char *text)
{
    const char *line = text, *nl;

    dr_vlog(d->log, DR_VLOG_DEBUG, "%s\n", head);
    for (; line && *line; line = nl + 1) {
        if (!(nl = strchr(line, '\n')))
            break;
        dr_vlog(d->log, DR_VLOG_DEBUG, "%.*s\n", (int)(nl - line), line);
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
static void attr_language(DrDisc *d, char lang[4], const uint8_t *b)
{
    char code[3] = { b[0], b[1], 0 };
    const char *iso = code[0] ? dr_disc_lang_code(code) : NULL;
    if (iso)
        dr_strlcpy(lang, iso, 4);
    else
        dr_vlog(d->log, DR_VLOG_DEBUG, "DVD: language code bytes %02x %02x are no known language\n", b[0], b[1]);
}

#define MAX_TRACKS (1 + 8 + 33 + 1)

/* The title's tracks as its title set's attributes describe them: the
 * video, the audio streams (without those the probe found without data),
 * the sub-pictures, then the caption track when the probe found captions
 * (the stream pass then gives their details and data). Packet ids: video
 * 0x1E0, MPEG audio 0x1C0 + n, private stream 1 sub-streams AC-3 0x80 + n,
 * DTS 0x88 + n, LPCM 0xA0 + n, sub-pictures 0x20 + n. */
static int title_tracks(DrDisc *d, const dvd_title_t *t, DiscTrack *out)
{
    static const DRCodecID codecs[8] = { DR_CODEC_AC3, DR_CODEC_NONE, DR_CODEC_MP2, DR_CODEC_MP2,
                                              DR_CODEC_PCM_DVD, DR_CODEC_NONE, DR_CODEC_DTS,
                                              DR_CODEC_NONE };
    static const int id_base[8] = { 0x80, 0, 0x1c0, 0x1c0, 0xa0, 0, 0x88, 0 };
    DVDDisc *disc = &((DVDDemuxContext *)ff_disc_part(d))->disc;
    const uint8_t *h = ff_dvd_set(disc, t->vtsn)->header;
    int n = 0;
    DiscStreamInfo *k;

    memset(out, 0, MAX_TRACKS * sizeof(*out));
    for (int i = 0; i < MAX_TRACKS; i++)
        ff_disc_stream_init(&out[i].info);
    k = &out[n].info;
    k->type  = DISC_MEDIA_VIDEO;
    k->codec = (h[DVD_VTS_VIDEO_ATTR] >> 6) == 0 ? DR_CODEC_MPEG1VIDEO :
               (h[DVD_VTS_VIDEO_ATTR] >> 6) == 1 ? DR_CODEC_MPEG2VIDEO : DR_CODEC_NONE;
    out[n].id   = 0x1e0;
    out[n].kind = DR_KIND_VIDEO;
    n++;
    for (int i = 0; i < t->nb_audio; i++) {
        const uint8_t *a = h + DVD_VTS_AUDIO_ATTR + 8 * t->audio[i].attr;

        if (t->audio_dropped >> i & 1)
            continue;
        k = &out[n].info;
        k->type  = DISC_MEDIA_AUDIO;
        k->codec = codecs[a[0] >> 5];
        if ((a[0] & 0x0c) == 4)
            attr_language(d, k->language, a + 2);
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
        k->type   = DISC_MEDIA_SUBTITLE;
        k->codec  = DR_CODEC_DVD_SUBTITLE;
        k->width  = t->subp_width;
        k->height = t->subp_height;
        if ((p[0] & 3) == 1)
            attr_language(d, k->language, p + 2);
        k->roles = subp_roles(p[5]);
        out[n].id      = 0x20 + t->subp[i].stream;
        out[n].kind    = DR_KIND_SUBTITLE;
        out[n].stream  = t->subp[i].stream;
        out[n].number  = 1 + t->nb_audio + i;
        out[n].palette = ff_dvd_title_pgc(disc, t)->palette;
        out[n].picture = "DVD sub-pictures: 4 colours of the program chain's 16-entry palette";
        n++;
    }
    if (t->captions) {
        k = &out[n].info;
        k->type  = DISC_MEDIA_SUBTITLE;
        k->codec = DR_CODEC_EIA_608;
        dr_strlcpy(k->language, "eng", sizeof(k->language));
        out[n].id      = -1;
        out[n].kind    = DR_KIND_CAPTION;
        out[n].caption = "line-21 closed captions (CEA-608) in the video, made into text";
        n++;
    }
    return n;
}

/* ---- the title entries (disc.h) ---- */

static int nb_titles(DrDisc *d)
{
    return ((DVDDemuxContext *)ff_disc_part(d))->titles.nb_titles;
}

static const char *title_name(DrDisc *d, int i)
{
    return ((DVDDemuxContext *)ff_disc_part(d))->titles.titles[i]->name;
}

/* The probe of the title's streams (libdvdread: captions, audio streams
 * without data); < 0: a sector cannot be read, the title cannot be used. */
static int title_probe(DrDisc *d, int i)
{
    DVDDemuxContext *c = ff_disc_part(d);
    int ret = DVDTitleStreams_rec(&c->disc, c->titles.titles[i]);
    return ret == -1 ? DR_ERR(ENOMEM) : ret == -3 ? DR_ERR_INVALID : ret < 0 ? DR_ERR(EIO) : 0;
}

static int title_tracks_of(DrDisc *d, int i, DiscTrack *out, int out_size)
{
    DVDDemuxContext *c = ff_disc_part(d);
    if (out_size < MAX_TRACKS)
        return DR_ERR_BUG;
    return title_tracks(d, c->titles.titles[i], out);
}


static int pass_read(void *opaque, uint32_t block, uint8_t *buf)
{
    PassRead *r = opaque;
    if (block >= r->nb_blocks)
        return DR_ERR_EOF;
    return ff_dvd_segment_read(r->disc, r->t, r->seg, block, buf);
}

/* A segment's identity: its title set and every extent it plays (FNV-1a). */
static uint64_t segment_key(const dvd_title_t *t, const dvd_segment_t *g)
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
        for (int j = 0; j < (int)sizeof(v); j++) {
            h ^= ((const uint8_t *)v)[j];
            h *= 0x100000001b3ULL;
        }
    }
    return h ? h : 1;
}

/* The title's largest segment (the first of equal ones), read as the
 * title reader reads it; DVD-Video's 3-byte LPCM header; empty sub-picture
 * tracks reported (read on to the segment's end). */
static int title_segment(DrDisc *d, int i, DiscPassSource *src)
{
    DVDDemuxContext *c = ff_disc_part(d);
    const dvd_title_t *t = c->titles.titles[i];
    int64_t best = -1;
    int seg = 0;

    if (!t->segs.nb_segments)
        return 0;
    for (int k = 0; k < t->segs.nb_segments; k++)
        if (t->segs.segments[k].size > best) {
            best = t->segs.segments[k].size;
            seg = k;
        }
    c->pass = (PassRead){ &c->disc, t, seg, ff_dvd_segment_blocks(&t->segs.segments[seg]) };
    *src = (DiscPassSource){ .index = seg, .lpcm_header = 3, .read_all = 1,
                             .key = segment_key(t, &t->segs.segments[seg]),
                             .read = pass_read, .opaque = &c->pass };
    return 1;
}

static void remove_title(DrDisc *d, int i)
{
    DVDDemuxContext *c = ff_disc_part(d);
    dvd_title_list_t *l = &c->titles;
    DVDFreeTitle_rec(l->titles[i]);
    memmove(l->titles + i, l->titles + i + 1, (l->nb_titles - i - 1) * sizeof(*l->titles));
    memmove(c->sources + i, c->sources + i + 1, (l->nb_titles - i - 1) * sizeof(*c->sources));
    l->nb_titles--;
}

/* The disc info's title: length (the cells' playback times), size (the
 * title VOB sectors it plays), angle (with the main angle and its title, the
 * order the disc plays the angles in, the cells all angles play), whether
 * selected, the program chain
 * and the navigation's recording it comes from, its segments' cells (as its
 * "files"), its chapters. */
static int title_info(DrDisc *d, int i, DiscTitleInfo *dt)
{
    DVDDemuxContext *c = ff_disc_part(d);
    const dvd_title_t *t = c->titles.titles[i];
    char *source = c->sources[i];

    dt->duration = (int64_t)t->declared * INT64_C(1000000);
    dt->angle    = t->nr_of_angles > 1 ? t->angle + 1 : 0;
    dt->selected = dt->angle <= 1;          /* an angle other than the main one: not ripped with all */
    if (dt->angle) {
        dt->main_angle   = t->main_angle;
        dt->main_title   = -1;
        dt->shared_cells = t->shared_cells;
        for (int j = 0; j < c->titles.nb_titles; j++) {
            const dvd_title_t *o = c->titles.titles[j];
            if (o->title == t->title && o->pgcn == t->pgcn && o->angle + 1 == t->main_angle)
                dt->main_title = j;
        }
        if (t->scan)
            memcpy(dt->angle_order, t->scan->angles, sizeof(dt->angle_order));
    }
    if (t->scan)
        snprintf(source, sizeof(c->sources[i]), "VTS_%02d program chain %d (navigation %s), cells", t->vtsn,
                 t->pgcn + 1, t->scan->name);
    else
        snprintf(source, sizeof(c->sources[i]), "VTS_%02d program chain %d, cells", t->vtsn, t->pgcn + 1);
    dt->source = source;
    if (ff_disc_title_add_prop(dt, DISC_PROP_TITLE_ON_DISC, "%s", t->name) < 0 ||
        ff_disc_title_add_prop(dt, DISC_PROP_TITLE_SET, "VTS_%02d, its title %d", t->vtsn, t->vts_ttn) < 0 ||
        ff_disc_title_add_prop(dt, DISC_PROP_PROGRAM_CHAIN, "%d", t->pgcn + 1) < 0 ||
        ff_disc_title_add_prop(dt, DISC_PROP_NAVIGATION, "%s", t->scan ? t->scan->name : "not reached (cells trimmed)") < 0 ||
        (t->nr_of_angles > 1 && ff_disc_title_add_prop(dt, DISC_PROP_ANGLES, "%d", t->nr_of_angles) < 0) ||
        ff_disc_title_add_prop(dt, DISC_PROP_CELLS, "%d", t->nb_cells) < 0 ||
        ff_disc_title_add_prop(dt, DISC_PROP_LENGTH_MEASURED, "%u:%02u:%02u (the program chain says %u:%02u:%02u)",
                               t->measured / 3600, t->measured / 60 % 60, t->measured % 60, t->declared / 3600,
                               t->declared / 60 % 60, t->declared % 60) < 0)
        return DR_ERR(ENOMEM);
    for (int k = 0; k < t->segs.nb_segments; k++) {
        dt->size += t->segs.segments[k].size;
        if (ff_disc_title_add_file(dt, t->cell_maps[k]) < 0)
            return DR_ERR(ENOMEM);
    }
    for (int k = 0; k < t->nb_chapters; k++) {
        DiscChapterInfo *ch = ff_disc_title_add_chapter(dt);
        if (!ch)
            return DR_ERR(ENOMEM);
        ch->start = dr_rescale(t->chapters[k].time, INT64_C(1000000), 90000);
    }
    return 0;
}

/* The opened title's source (its streams and tracks: the disc layer's):
 * its segments
 * one after another, DVD-Video's 3-byte LPCM header,
 * the chapter records placed on its segments. */
static int open_title(DrDisc *d, int i, int part, int *second_view)
{
    
    DVDDemuxContext *c = ff_disc_part(d);
    const dvd_title_t *t = c->titles.titles[i];
    int nb_ch = 0;

    c->title = t;
    memset(&c->ended, 0, sizeof(c->ended));
    c->view_seg = -1;
    if (!(c->vobs = ff_dvd_title_vobs_open(&c->disc, t->vtsn)))
        return DR_ERR(EIO);
    if (!(c->chapter_times = dr_calloc(DR_MAX(t->nb_chapters, 1), sizeof(*c->chapter_times))) ||
        !(c->chapter_clips = dr_calloc(DR_MAX(t->nb_chapters, 1), sizeof(*c->chapter_clips))) ||
        !(c->chapter_offsets = dr_calloc(DR_MAX(t->nb_chapters, 1), sizeof(*c->chapter_offsets))))
        return DR_ERR(ENOMEM);
    /* chapter records: positions (a segment and a byte offset in it; a
     * chapter starts at the first video frame from there), with their times
     * from the title's start */
    for (int k = 0; k < t->nb_chapters; k++, nb_ch++) {
        c->chapter_times[k]   = t->chapters[k].time * DR_TICKS_PER_PTS;
        c->chapter_clips[k]   = t->chapters[k].segment == DVD_TITLE_CHAPTER_NONE ? -1 : t->chapters[k].segment;
        c->chapter_offsets[k] = t->chapters[k].offset;
    }
    d->duration = (int64_t)t->declared * INT64_C(1000000) * 1080; /* microseconds -> ticks */
    d->src = (DiscTitleSource){
        .nb_clips      = t->segs.nb_segments,
        .clip_kind     = "segment",
        .clip_name     = clip_name,
        .clip_blocks   = clip_blocks,
        .read_block    = read_block,
        .block_place   = block_place,
        .damage        = damage,
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
static void menu_languages(DrDisc *d, DVDDisc *disc)
{
    char codes[32][4];
    int n = 0;

    for (int i = 0; i < ff_dvd_nb_sets(disc) && !n; i++) {
        const dvd_ifo_rec_t *f = ff_dvd_set(disc, i);
        uint8_t p[DVD_SECTOR];
        uint32_t sector;

        if (!f || !(sector = ff_dvd_header32(f, i ? 0xD0 : 0xC8)) ||
            ff_dvd_ifo_read(disc, i, sector, p, sizeof(p)) < 0)
            continue;
        for (int k = 0; k < DR_MIN(dr_rb16(p), 32) && 8 + 8 * k + 2 <= DVD_SECTOR; k++) {
            const uint8_t *lu = p + 8 + 8 * k;
            if (lu[0] >= 'a' && lu[0] <= 'z' && lu[1] >= 'a' && lu[1] <= 'z') {
                codes[n][0] = lu[0];
                codes[n][1] = lu[1];
                codes[n][2] = 0;
                n++;
            }
        }
    }
    ff_disc_set_market(d, (const char (*)[4])codes, n, 1, "its menus");
}

static int dvd_load(DrDisc *d)
{
    
    DVDDemuxContext *c = ff_disc_part(d);
    DVDTitleOptions o = { d->opt_dvd_cell_mode, d->opt_dvd_cell_check, d->opt_min_length };
    int folder = !d->image, ret;
    char *dump;

    if ((ret = ff_dvd_disc_open((void *)d->log, d->fs, d->dvd_flat ? "" : "/VIDEO_TS", folder, folder ? NULL : d->fs->label,
                                &c->disc)) < 0)
        return ret;
    if (dr_vlog_wants(DR_VLOG_DEBUG)) {
        DrStr bp;
        dr_str_init(&bp);
        ff_dvd_disc_dump(&c->disc, &bp);
        if (dr_str_ok(&bp))
            debug_lines(d, "DVD structure:", bp.str);
        dr_str_free(&bp);
    }
    d->info.label = c->disc.volume_name;
    d->info.name  = c->disc.rec->text_name;
    menu_languages(d, &c->disc);

    /* the titles: the navigation played (not for trimmed cells), then the
     * title list from it (a failed scan leaves no results) */
    if ((o.cell_mode != DVD_CELL_MODE_TRIM && ff_dvd_scan(&c->disc, NULL, NULL, &c->scan) == DR_ERR(ENOMEM)) ||
        (ret = ff_dvd_titles_build(&c->disc, &c->scan, &o, &c->titles)) < 0 ||
        !(c->sources = dr_calloc(DR_MAX(c->titles.nb_titles, 1), sizeof(*c->sources))))
        return ret < 0 ? ret : DR_ERR(ENOMEM);
    /* what the navigation found */
    ff_dvd_reports_log(&c->disc);
    if (dr_vlog_wants(DR_VLOG_DEBUG) && (dump = ff_dvd_titles_dump(&c->titles))) {
        debug_lines(d, "DVD titles:", dump);
        dr_free(dump);
    }
    return 0;
}

/* The disc's regions: VMGI_MAT vmg_category (0x22) bits 16-23, a set bit =
 * not for that region (1-8). */
static int add_region(DrDisc *d, DVDDisc *disc)
{
    const dvd_ifo_rec_t *vmg = ff_dvd_set(disc, 0);
    unsigned mask;
    char list[32] = "";

    if (!vmg)
        return 0;
    mask = ff_dvd_header32(vmg, DVD_VMG_CATEGORY) >> 16 & 0xff;
    if (!mask)
        return ff_disc_add_prop(d, DISC_PROP_REGION, "all (region free)");
    if (mask == 0xff)
        return ff_disc_add_prop(d, DISC_PROP_REGION, "none (every region locked out)");
    for (int r = 1; r <= 8; r++)
        if (!(mask & 1 << (r - 1)))
            dr_strlcatf(list, sizeof(list), "%s%d", list[0] ? ", " : "", r);
    return ff_disc_add_prop(d, DISC_PROP_REGION, "%s", list);
}

/* Protection: CSS when a title set's scrambled sectors were met while the
 * stream passes read (its title key looked for); the disc's regions. */
static int disc_done(DrDisc *d)
{
    
    DVDDemuxContext *c = ff_disc_part(d);
    int css = 0, ret;

    for (int i = 1; i < ff_dvd_nb_sets(&c->disc); i++)
        css |= ff_dvd_css_state(&c->disc, i, NULL) != 0;
    d->info.encrypted = css;
    {
        int harmless, breaks;
        ff_dvd_reports_count(&c->disc, &harmless, &breaks);
        if ((ret = !harmless && !breaks ? ff_disc_add_prop(d, DISC_PROP_IFO_CHECK, "passed") :
                   ff_disc_add_prop(d, DISC_PROP_IFO_CHECK, "%d finding(s) that break the DVD-Video rules, "
                                    "%d harmless", breaks, harmless)) < 0)
            return ret;
    }
    if ((ret = ff_disc_add_prop(d, DISC_PROP_CSS, "%s", css ? "yes" :
                                "no (no scrambled sector in what was read)")) < 0)
        return ret;
    return add_region(d, &c->disc);
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
