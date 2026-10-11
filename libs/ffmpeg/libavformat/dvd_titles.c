/*
 * DVD-Video: the title list. Built from the disc's structure (dvd_disc.c)
 * and the navigation scan's results (dvd_scan.c).
 *
 * Order. With the scan's cell lists in use, the titles the navigation
 * reached come first, then the others, each group in the order of the title
 * table (TT_SRPT); otherwise the title table's order.
 *
 * A title table entry. Left out: an entry without a title set, a VTS title
 * number or chapters; one whose title set could not be loaded; one whose
 * title set is suspicious (its title VOBs share sectors with another title
 * set's, it has more than 80 program chains, and every chain of two or more
 * cells either starts a new timeline at each cell after the first or fails
 * the program chain check), and, with the scan's cell lists in use, one the
 * navigation did not reach. Its chapters are the title's PTT entries
 * (VTS_PTT_SRPT), each resolved to its program's entry cell; every program
 * chain the chapters use gives a title of its own ("4", then "4/1", ...).
 *
 * A program chain's title. Its cells: the cells the navigation played in
 * it, as one run of consecutive cells (runs at the start and end without an
 * accepted cell or shorter than 5 seconds are dropped, then the shorter end
 * run while two remain), one cell per angle block (the title's angle). Its
 * length: declared (the cells' playback times) and real (the NAV packs'
 * presentation times); a title whose real length is far from its declared
 * one is a fake. Its sectors: the cells' VOBUs walked into extents of the
 * title VOBs, a new segment at a cell that starts a new timeline
 * (stc_discontinuity, also on a cell skipped between two of the list). Its
 * chapters: their places in the segments, out of order ones dropped. An
 * angle block gives one more title per further angle.
 *
 * Cell checks. A cell is rejected when its VOBUs are known to be broken, it
 * has no playback time, lies outside the title VOBs, or is a short cell
 * with a cell command; the full check also reads its VOBUs' NAV packs (all
 * of them, 50 evenly spread or 50 at random) and their presentation times.
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

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/random_seed.h"

#include "dvd_internal.h"

#define SUSPICIOUS_PGCS     80          /* program chains of a suspicious title set (more than) */
#define MAX_ANGLES          8
#define MAX_AUDIO           8           /* VTSI_MAT audio streams */
#define MAX_SUBP            32          /* VTSI_MAT subpicture streams */
#define VTS_AUDIO_COUNT     0x202       /* VTSI_MAT nr_of_vts_audio_streams */
#define VTS_SUBP_COUNT      0x254       /* VTSI_MAT nr_of_vts_subp_streams */
#define FAKE_SECONDS        300         /* a fake's real length differs by more than this ... */
#define FAKE_PERCENT        30          /* ... and more than this much of its declared length */
#define OVERLAP_GAP         0x400       /* sectors between two cells of a title on shared title VOBs (at most) */
#define COMPLEX_VOBUS       100         /* VOBUs to scan one by one before that is told (more than) */
#define NAV_INVALID_TOLD    14          /* "navigation invalid" warnings per title, then the debug log */

#define FLAGS(c)  ((c)->category[0])    /* block_mode 0xc0, block_type 0x30, seamless_play 8,
                                           interleaved 4, stc_discontinuity 2, seamless_angle 1 */
#define CMD_NR(c) ((c)->category[3])    /* cell_cmd_nr */
#define BLOCK_TYPE(f) ((f) >> 4 & 3)
#define BLOCK_MODE(f) ((f) >> 6)

typedef struct Titles {
    DVDDisc                *d;
    const DVDScan          *scan;
    const DVDTitleOptions  *o;
    DVDTitleList           *out;
} Titles;

/* hh:mm:ss of a BCD playback time in seconds (frames left out) */
/* ---- title sets sharing title VOBs ---- */

/* the title VOB sectors of title set vts on the disc ([start, end)) */
static int title_vob_range(const DVDDisc *d, int vts, uint32_t *start, uint32_t *end)
{
    const DVDIfo *f = vts < d->nb_ifos ? d->ifos[vts] : NULL;
    uint32_t tv, last, ifol, b;

    if (!f || f->failed)
        return 0;
    tv   = ff_dvd_header32(f, DVD_TT_SRPT);
    last = ff_dvd_header32(f, DVD_LAST_SECTOR);
    ifol = ff_dvd_header32(f, DVD_I_LAST_SECTOR) & 0x1ffff;
    if (last <= ifol || (b = last - ifol) <= tv)
        return 0;
    *start = d->title_set_start[vts - 1] + tv;
    *end   = d->title_set_start[vts - 1] + b;
    return 1;
}

int ff_dvd_title_vobs_overlap(const DVDDisc *d, int vts)
{
    uint32_t s, e, s2, e2;

    if (!title_vob_range(d, vts, &s, &e) || d->nb_ifos < 2)
        return 0;
    for (int k = 1; k < d->nb_ifos; k++)
        if (k != vts && title_vob_range(d, k, &s2, &e2) && ((s <= s2 && s2 < e) || (s2 <= s && s < e2)))
            return 1;
    return 0;
}

static uint32_t bcd_seconds(uint32_t t)
{
    return ((t >> 24 & 0xf) + (t >> 28) * 10) * 3600 + ((t >> 16 & 0xf) + (t >> 20 & 0xf) * 10) * 60 +
           (t >> 8 & 0xf) + (t >> 12 & 0xf) * 10;
}

static void hms(char *buf, size_t size, uint32_t secs)
{
    snprintf(buf, size, "%"PRIu32":%02"PRIu32":%02"PRIu32, secs / 3600, secs / 60 % 60, secs % 60);
}

static int grow(void **p, int *alloc, int n, size_t elem)
{
    if (n < *alloc)
        return 0;
    return av_reallocp_array(p, *alloc = FFMAX(16, *alloc * 2), elem);
}

/* ---- cells ---- */

/* ---- VOBUs of a cell, cell checks: libdvdread (vob_read.h) ---- */

/* The cell's title set's own reading (NULL: its title VOBs cannot be read). */
static dvd_vob_reader_t *reading(Titles *T, DVDIfo *f)
{
    return ff_dvd_own_reading(T->d, f);
}

/* The cell of the title's angle in the angle block starting at cell i
 * (a cell in no block: itself); -1 when the block is broken. */
static int angle_cell(Titles *T, DVDIfo *f, const DVDPgc *p, int i, int angle, int angles)
{
    dvd_vob_reader_t *vr = reading(T, f);
    return vr ? DVDAngleCell_rec(vr, p->number, i, angle, angles) : -1;
}

/* The cell's real length in 90 kHz ticks: its last VOBU's end against its
 * first VOBU's start. */
static int cell_duration(Titles *T, DVDIfo *f, DVDPgc *p, int i, uint64_t *dur, int read_anyway)
{
    dvd_vob_reader_t *vr = reading(T, f);
    return vr && DVDCellDuration_rec(vr, p->number, i, dur, read_anyway);
}

/* The full cell check: 1 when the cell fails. */
static int cell_check_failed(Titles *T, DVDIfo *f, DVDPgc *p, int i)
{
    dvd_vob_reader_t *vr = reading(T, f);
    return !vr || DVDCellCheckFailed_rec(vr, p->number, i);
}

/* ---- the program chain check: libdvdread (vob_read.h) ---- */

/* The program chain is broken or a decoy: the reason (0-9), -1 when it
 * passes (whether the title set's title VOBs share sectors with another's
 * is the title table's, handed in). */
static int pgc_rejected(Titles *T, DVDIfo *f, DVDPgc *p)
{
    dvd_vob_reader_t *vr = reading(T, f);
    return vr ? DVDPgcRejected_rec(vr, p->number, ff_dvd_title_vobs_overlap(T->d, f->index)) : 0;
}

/* ---- a title's cell list ---- */

/* The cells left out at the start and end of the title's program chain. */
static void say_left_out(Titles *T, DVDTitle *t, int first, int last)
{
    const DVDPgc *p = t->pgc;

    if (first)
        av_log(T->d->log, AV_LOG_VERBOSE, "DVD: title %s: cells 1-%d left out at the start\n", t->name, first);
    if (last != p->nr_of_cells - 1)
        av_log(T->d->log, AV_LOG_VERBOSE, "DVD: title %s: cells %d-%d left out at the end\n", t->name, last + 2,
               p->nr_of_cells);
}

/* The title's cells from libdvdread: a buffer for them. */
static int cells_room(DVDTitle *t)
{
    int ret;
    t->nb_cells = 0;
    if (t->alloc_cells < t->pgc->nr_of_cells &&
        (ret = av_reallocp_array(&t->cells, t->alloc_cells = FFMAX(t->pgc->nr_of_cells, 1), sizeof(*t->cells))) < 0)
        return ret;
    return 0;
}

/* The cells the navigation played in the title's program chain: one run of
 * consecutive cells (CellWalk, libdvdread). 1 done, 0 not possible, < 0 an
 * error. */
static int cell_walk(Titles *T, DVDTitle *t)
{
    const DVDScanResult *e = t->scan;
    const DVDPgc *p = t->pgc;
    dvd_vob_reader_t *vr = reading(T, t->vts);
    int first = -1, last = -1, ret;

    if (!e || !vr)
        return 0;
    if (e->nb_cells && ((p->number + 1) & 0xffff) != e->pgc)
        return 0;
    if ((ret = cells_room(t)) < 0)
        return ret;
    ret = DVDCellWalk_rec(vr, p->number, e->cells, e->nb_cells, t->angle, t->nr_of_angles, t->cells, &t->nb_cells,
                          &first, &last);
    if (ret < 0)
        return AVERROR(ENOMEM);
    if (first >= 0)                     /* a run was chosen */
        say_left_out(T, t, first, last);
    return ret;
}

/* ---- the cell list without the navigation (trim): libdvdread ---- */

/* The cell list without the navigation: the program chain's cells, decoys
 * at the start and end trimmed (CellTrim). 1 done (no cells when none are
 * left), 0 failed, < 0 an error. */
static int cell_trim(Titles *T, DVDTitle *t)
{
    dvd_vob_reader_t *vr = reading(T, t->vts);
    int first = t->nb_chapters ? t->chapters[0].cell : -1;
    int mid   = t->nb_chapters ? t->chapters[t->nb_chapters / 2].cell : -1, ret;
    dvd_cell_trim_t res;

    if (!vr)
        return 0;
    if ((ret = cells_room(t)) < 0)
        return ret;
    ret = DVDCellTrim_rec(vr, t->pgc->number, t->nb_chapters, first, mid, t->angle, t->nr_of_angles, t->cells,
                          &t->nb_cells, &res);
    if (res.fake_percent >= 0)
        av_log(T->d->log, AV_LOG_INFO, "DVD: title %s: fake cells make up %d%% of it: its cells are left out\n",
               t->name, res.fake_percent);
    if (res.first >= 0)
        say_left_out(T, t, res.first, res.last);
    return ret;
}

/* ---- segments and chapter places: libdvdread ---- */

/* libdvdread's segments as the title's (extents, cell runs and their text). */
static int take_segments(DVDTitle *t, const dvd_title_segments_t *o)
{
    if (o->nb_segments && !(t->segments = av_calloc(o->nb_segments, sizeof(*t->segments))))
        return AVERROR(ENOMEM);
    t->alloc_segments = t->nb_segments = o->nb_segments;
    for (int k = 0; k < o->nb_segments; k++) {
        const dvd_segment_t *s = &o->segments[k];
        DVDSegment *g = &t->segments[k];
        AVBPrint bp;
        int ret;

        if ((s->nb_extents && !(g->extents = av_malloc_array(s->nb_extents, sizeof(*g->extents)))) ||
            (s->nb_runs && !(g->runs = av_malloc_array(s->nb_runs, sizeof(*g->runs)))))
            return AVERROR(ENOMEM);
        for (int e = 0; e < s->nb_extents; e++)
            g->extents[e] = (DVDExtent){ s->extents[e].logical, s->extents[e].sector, s->extents[e].count };
        if (s->nb_runs)
            memcpy(g->runs, s->runs, s->nb_runs * sizeof(*g->runs));
        g->alloc_extents = g->nb_extents = s->nb_extents;
        g->alloc_runs    = g->nb_runs    = s->nb_runs;
        g->last_cell     = s->last_cell;
        g->size          = s->size;
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        if (g->nb_runs > 1)
            av_bprint_chars(&bp, '(', 1);
        for (int r = 0; r < g->nb_runs; r++) {
            if (g->runs[r][0] == g->runs[r][1])
                av_bprintf(&bp, "%s%u", r ? "," : "", g->runs[r][0]);
            else
                av_bprintf(&bp, "%s%u-%u", r ? "," : "", g->runs[r][0], g->runs[r][1]);
        }
        if (g->nb_runs > 1)
            av_bprint_chars(&bp, ')', 1);
        if ((ret = av_bprint_finalize(&bp, &g->cell_map)) < 0)
            return ret;
    }
    return 0;
}

/* The chapters' places in the segments (libdvdread); unplaced ones dropped,
 * a leading one that cannot be placed dropped or (the next one not at the
 * start) put at the start. */
static int place_chapters(Titles *T, DVDTitle *t, dvd_vob_reader_t *vr, const dvd_title_segments_t *o)
{
    dvd_chapter_place_t *pl;
    int n;

    if (t->angle)
        for (int k = 0; k < t->nb_chapters; k++)
            if (t->chapters[k].cell >= 0)
                t->chapters[k].cell = angle_cell(T, t->vts, t->pgc, t->chapters[k].cell, t->angle, t->nr_of_angles);
    if (!t->nb_chapters)
        return 0;
    if (!(pl = av_malloc_array(t->nb_chapters, sizeof(*pl))))
        return AVERROR(ENOMEM);
    for (int k = 0; k < t->nb_chapters; k++)
        pl[k].cell = t->chapters[k].cell;
    DVDChapterPlaces_rec(vr, t->pgc->number, o, pl, t->nb_chapters);
    for (int k = 0; k < t->nb_chapters; k++) {
        DVDChapter *ch = &t->chapters[k];
        ch->segment = pl[k].segment < 0 ? DVD_CHAPTER_NONE : pl[k].segment;
        if (pl[k].segment >= 0) {
            ch->offset = pl[k].offset;
            ch->time   = pl[k].time;
        }
    }
    av_free(pl);
    n = t->nb_chapters;
    if (n > 2) {
        while (t->chapters[0].segment == DVD_CHAPTER_NONE) {
            if (t->chapters[1].segment != DVD_CHAPTER_NONE) {
                if (t->chapters[1].segment || t->chapters[1].offset) {
                    t->chapters[0].segment = 0;
                    t->chapters[0].offset  = 0;
                    t->chapters[0].time    = 0;
                    t->chapters[0].cell    = t->cells[0];
                }
                break;
            }
            memmove(t->chapters, t->chapters + 1, --t->nb_chapters * sizeof(*t->chapters));
            if (t->nb_chapters <= 2)
                break;
        }
    }
    n = 0;
    for (int k = 0; k < t->nb_chapters; k++)
        if (t->chapters[k].segment != DVD_CHAPTER_NONE)
            t->chapters[n++] = t->chapters[k];
    t->nb_chapters = n;
    return 0;
}

/* The cells' VOBUs as extents in segments, then the chapters' places
 * (libdvdread). 1 done, 0 failed, < 0 an error. */
static int build_segments(Titles *T, DVDTitle *t)
{
    dvd_vob_reader_t *vr = reading(T, t->vts);
    dvd_title_segments_t o;
    int ret, err;

    if (!vr)
        return 0;
    ret = DVDTitleSegments_rec(vr, t->pgc->number, t->cells, t->nb_cells, &o);
    if (o.counted && o.vobus_to_scan > COMPLEX_VOBUS)
        av_log(T->d->log, AV_LOG_INFO, "DVD: title %s: %u cells and %u VOBUs are read one by one (a complex "
               "multiplex); this takes a while\n", t->name, o.cells_to_scan, o.vobus_to_scan);
    for (int k = 0; k < o.nb_nav_invalid; k++) {
        uint32_t secs = o.nav_invalid[k];
        if (++t->nav_invalid <= NAV_INVALID_TOLD)
            av_log(T->d->log, AV_LOG_WARNING, "DVD: title %s: the navigation information is invalid around "
                   "%"PRIu32":%02"PRIu32":%02"PRIu32" (check the output for audio / video sync)\n",
                   t->name, secs / 3600, secs / 60 % 60, secs % 60);
        else
            av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: the navigation information is invalid around "
                   "%"PRIu32":%02"PRIu32":%02"PRIu32"\n", t->name, secs / 3600, secs / 60 % 60, secs % 60);
    }
    if (ret < 0) {
        DVDFreeTitleSegments_rec(&o);
        return AVERROR(ENOMEM);
    }
    if (ret > 0) {
        if ((err = take_segments(t, &o)) < 0 || (err = place_chapters(T, t, vr, &o)) < 0)
            ret = err;
        else
            t->built = 1;
    }
    DVDFreeTitleSegments_rec(&o);
    return ret;
}

/* Cells, length, segments and chapters of a title. 1 done (a title left
 * without segments is a fake or not the program chain's), 0 failed, < 0
 * an error. */
static int build_title(Titles *T, DVDTitle *t)
{
    DVDIfo *f = t->vts;
    DVDPgc *p = t->pgc;
    uint64_t real = 0;
    int mode = T->o->cell_mode, ret, suspect = 0;

    if (!t->scan && pgc_rejected(T, f, p) >= 0) {
        t->nb_cells = 0;
    } else {
        if (!mode)
            mode = T->scan->nb_results ? DVD_CELL_MODE_WALK : DVD_CELL_MODE_TRIM;
        switch (mode) {
        case DVD_CELL_MODE_WALK:
            if (!t->scan) {
                t->nb_cells = 0;
                ret = 1;
            } else {
                ret = cell_walk(T, t);
            }
            break;
        case DVD_CELL_MODE_TRIM:
            ret = cell_trim(T, t);
            break;
        default:
            if (t->scan && (ret = cell_walk(T, t)) != 0)
                break;
            if (t->scan) {
                av_log(T->d->log, AV_LOG_INFO, "DVD: title %s: the navigation's cells do not work (structure "
                       "protection?): the cells are trimmed instead\n", t->name);
                if (pgc_rejected(T, f, p) >= 0) {
                    t->nb_cells = 0;
                    ret = 1;
                    break;
                }
            }
            ret = cell_trim(T, t);
        }
        if (ret <= 0) {
            if (!ret)
                av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: no cell list\n", t->name);
            return ret;
        }
    }

    /* length */
    t->declared = 0;
    for (int i = 0; i < t->nb_cells; i++) {
        uint64_t dur;
        t->declared += bcd_seconds(p->cells[t->cells[i]].playback_time);
        if (cell_duration(T, f, p, t->cells[i], &dur, 0))
            real += dur;
    }
    t->measured = t->nb_cells ? real / 90000 : 0;
    if (t->nb_cells) {
        uint32_t diff = t->declared > t->measured ? t->declared - t->measured : t->measured - t->declared;
        if (!t->declared || (diff > FAKE_SECONDS && diff * 100 / t->declared > FAKE_PERCENT))
            suspect = 1;
    }
    if (!suspect && t->nb_cells >= 2 && !t->scan && ff_dvd_title_vobs_overlap(T->d, f->index)) {
        /* on title VOBs shared with another title set: cells must follow each other */
        int over = 0, last = 0;
        for (int i = 1; i < t->nb_cells && !suspect; i++) {
            const DVDCell *c = &p->cells[t->cells[i]], *b = &p->cells[t->cells[i - 1]];
            if (FLAGS(c) & 8)
                continue;
            if (c->first_sector <= b->last_sector) {
                over++;
                last = i;
            } else if (c->first_sector - b->last_sector > OVERLAP_GAP) {
                suspect = 1;
            }
        }
        if (!suspect && over >= 2)
            suspect = 1;
        if (!suspect && over == 1) {
            int j = last, k;
            do {
                if (j == 1) {
                    j = 0;
                    break;
                }
                j--;
            } while (FLAGS(&p->cells[t->cells[j]]) & 8);
            for (k = last; k < t->nb_cells - 1 && (FLAGS(&p->cells[t->cells[k + 1]]) & 8); k++)
                ;
            if (k > t->nb_cells - 1)
                k = t->nb_cells - 1;
            if (p->cells[t->cells[k]].last_sector >= p->cells[t->cells[j]].first_sector)
                suspect = 1;
        }
    }
    if (suspect) {
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: declared %"PRIu32" s, real %"PRIu32" s: no segments\n",
               t->name, t->declared, t->measured);
        return 1;
    }
    if (!t->built && (ret = build_segments(T, t)) <= 0)
        return ret;
    return 1;
}

/* ---- titles ---- */

void ff_dvd_title_free(DVDTitle **pt)
{
    DVDTitle *t = *pt;
    if (!t)
        return;
    for (int k = 0; k < t->nb_segments; k++) {
        av_free(t->segments[k].extents);
        av_free(t->segments[k].runs);
        av_free(t->segments[k].cell_map);
    }
    av_free(t->segments);
    av_free(t->chapters);
    av_free(t->cells);
    av_freep(pt);
}

void ff_dvd_titles_free(DVDTitleList *l)
{
    for (int i = 0; i < l->nb_titles; i++)
        ff_dvd_title_free(&l->titles[i]);
    av_freep(&l->titles);
    l->nb_titles = 0;
}

static const DVDScanResult *scan_find(const DVDScan *s, unsigned title, unsigned pgc)
{
    for (int i = 0; i < s->nb_results; i++)
        if (s->results[i].title == (title & 0xff) && s->results[i].pgc == (pgc & 0xff))
            return &s->results[i];
    return NULL;
}

/* the navigation reached title (1-based) in a program chain */
static int scan_reached(const DVDScan *s, unsigned title)
{
    for (int i = 0; i < s->nb_results; i++)
        if (s->results[i].title == (title & 0xff) && s->results[i].pgc)
            return 1;
    return 0;
}

static unsigned tt_byte(const DVDDisc *d, uint32_t o)
{
    if (o + 1 > d->tt_srpt.size) {
        av_log(d->log, AV_LOG_DEBUG, "DVD: the title table ends before byte %"PRIu32"\n", o);
        return 0;
    }
    return d->tt_srpt.data[o];
}

/* The title's audio streams: those the program chain's audio_control
 * lists (present), in its order, then the title set's other ones. */
static void title_audio(Titles *T, DVDTitle *t)
{
    const DVDIfo *f = t->vts;
    unsigned n = AV_RB16(f->header + VTS_AUDIO_COUNT), mask = 0;

    t->nb_audio = 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t b = t->pgc->audio_control[2 * i];
        if (!(b & 0x80))
            continue;
        mask |= 1 << (b & 7);
        t->audio[t->nb_audio++] = (DVDStreamRef){ b & 7, i };
        if ((b & 7) != i)
            av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: audio_control %u names stream %u\n", t->name, i, b & 7);
    }
    for (unsigned k = 0; k < n; k++)
        if (!(mask >> k & 1))
            t->audio[t->nb_audio++] = (DVDStreamRef){ k, k };
    t->audio_listed = mask;
}

/* The title's sub-picture streams: those the program chain's subp_control
 * lists (present), each by its 4:3 stream number on a 4:3 title set, its
 * wide one on a 16:9 title set; a single stream whose wide number is 1 and
 * letterbox number 0 also as stream 0. Their canvas: the title set's video
 * frame. */
static void title_subp(Titles *T, DVDTitle *t)
{
    const DVDIfo *f = t->vts;
    const uint8_t *v = f->header + DVD_VTS_VIDEO_ATTR;
    unsigned n = AV_RB16(f->header + VTS_SUBP_COUNT), ar = v[0] >> 2 & 3, standard = v[0] >> 4 & 3;

    if (standard == 0) {
        t->subp_width  = 720;
        t->subp_height = 480;
    } else if (standard == 1) {
        t->subp_width  = 720;
        t->subp_height = 576;
    } else {
        t->subp_width  = 352;
        t->subp_height = 480;
        av_log(T->d->log, AV_LOG_WARNING, "DVD: VTS_%02d: video standard %u: the sub-pictures' canvas is taken as "
               "352x480 [untested on real discs]\n", f->index, standard);
    }
    t->nb_subp = 0;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *c = t->pgc->subp_control + 4 * i;
        uint8_t sid;
        if (!(c[0] & 0x80))
            continue;
        if (ar == 0) {
            sid = c[0];
        } else if (ar == 3) {
            sid = c[1];
        } else {
            sid = c[0];
            av_log(T->d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: aspect ratio code %u: the 4:3 sub-picture streams are "
                   "used\n", f->index, ar);
        }
        t->subp[t->nb_subp++] = (DVDStreamRef){ sid & 0x1f, i };
        if (n == 1 && ar == 3 && (c[1] & 0x1f) == 1 && (c[2] & 0x1f) == 0)
            t->subp[t->nb_subp++] = (DVDStreamRef){ 0, i };
    }
}

/* A program chain's title of title table entry idx (0-based), angle a. */
static int title_init(Titles *T, DVDTitle *t, int idx, DVDIfo *f, DVDPgc *p, const DVDChapter *ch, int nb_ch,
                      int angle)
{
    uint32_t o = 8 + 12 * idx;
    unsigned angles;

    t->title   = idx;
    t->pb_ty   = tt_byte(T->d, o);
    t->vtsn    = tt_byte(T->d, o + 6);
    t->vts_ttn = tt_byte(T->d, o + 7);
    angles     = o + 2 <= T->d->tt_srpt.size ? T->d->tt_srpt.data[o + 1] : 1;
    if (!angles)
        angles = 1;
    if (angles > MAX_ANGLES) {
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %d: %u angles: %d are read\n", idx + 1, angles, MAX_ANGLES);
        angles = MAX_ANGLES;
    }
    t->nr_of_angles = angles;
    if (angle >= (int)angles)
        return 0;
    t->angle = angle;
    t->vts   = f;
    t->pgc   = p;
    if (!(t->chapters = av_malloc_array(FFMAX(nb_ch, 1), sizeof(*t->chapters))))
        return AVERROR(ENOMEM);
    memcpy(t->chapters, ch, nb_ch * sizeof(*ch));
    t->nb_chapters = nb_ch;
    t->scan = scan_find(T->scan, idx + 1, p->number + 1);
    if (AV_RB16(f->header + VTS_AUDIO_COUNT) > MAX_AUDIO) {
        av_log(T->d->log, AV_LOG_WARNING, "DVD: VTS_%02d has %u audio streams\n", f->index,
               AV_RB16(f->header + VTS_AUDIO_COUNT));
        return 0;
    }
    title_audio(T, t);
    if (AV_RB16(f->header + VTS_SUBP_COUNT) > MAX_SUBP) {
        av_log(T->d->log, AV_LOG_WARNING, "DVD: VTS_%02d has %u subpicture streams\n", f->index,
               AV_RB16(f->header + VTS_SUBP_COUNT));
        return 0;
    }
    title_subp(T, t);
    return 1;
}

static int list_add(DVDTitleList *l, DVDTitle *t)
{
    return av_dynarray_add_nofree(&l->titles, &l->nb_titles, t);
}

static void title_skip(Titles *T, DVDIfo *f, const DVDPgc *p)
{
    av_log(T->d->log, AV_LOG_DEBUG, "DVD: title skipped: VTS %d PGC %d\n", f->index, p->number + 1);
}

/* the cells' declared length differs from the real one: a fake */
static int fake_length(const DVDTitle *t)
{
    uint32_t diff;
    if (!t->nb_cells)
        return 0;
    diff = t->declared > t->measured ? t->declared - t->measured : t->measured - t->declared;
    return !t->declared || (diff > FAKE_SECONDS && diff * 100 / t->declared > FAKE_PERCENT);
}

/* The titles of one program chain of title table entry idx: the first
 * angle, then one per further angle. */
static int add_pgc_titles(Titles *T, int idx, DVDIfo *f, DVDPgc *p, const DVDChapter *ch, int nb_ch, int k)
{
    DVDTitle *t;
    char d1[16], d2[16];
    int ret;

    if (!nb_ch)
        return 0;
    if (!(t = av_mallocz(sizeof(*t))))
        return AVERROR(ENOMEM);
    if (k)
        snprintf(t->name, sizeof(t->name), "%d/%d", idx + 1, k);
    else
        snprintf(t->name, sizeof(t->name), "%d", idx + 1);
    if ((ret = title_init(T, t, idx, f, p, ch, nb_ch, 0)) <= 0) {
        if (!ret)
            av_log(T->d->log, AV_LOG_INFO, "DVD: title %s left out: its program chain cannot be used\n", t->name);
        title_skip(T, f, p);
        goto drop;
    }
    if (bcd_seconds(p->playback_time) < (uint32_t)T->o->min_length)
        t->short_title = 1;
    if ((ret = build_title(T, t)) <= 0) {
        if (!ret)
            av_log(T->d->log, AV_LOG_INFO, "DVD: title %s (VTS %d PGC %d) left out: navigation error\n", t->name,
                   f->index, p->number + 1);
        title_skip(T, f, p);
        goto drop;
    }
    if (!t->nb_segments && !fake_length(t)) {
        title_skip(T, f, p);
        ret = 0;
        goto drop;
    }
    hms(d1, sizeof(d1), t->declared);
    if (fake_length(t)) {
        hms(d2, sizeof(d2), t->measured);
        av_log(T->d->log, AV_LOG_INFO, "DVD: title %s left out: its declared length is %s, its real length %s "
               "(a fake title)\n", t->name, d1, d2);
        title_skip(T, f, p);
        ret = 0;
        goto drop;
    }
    if (t->declared < (uint32_t)T->o->min_length)
        t->short_title = 1;
    av_log(T->d->log, AV_LOG_VERBOSE, "DVD: title %s added (%d cell(s), %s)%s\n", t->name, t->nb_cells, d1,
           t->short_title ? ", shorter than the minimum length" : "");
    if ((ret = list_add(T->out, t)) < 0)
        goto drop;
    for (int a = 1; a < t->nr_of_angles; a++) {
        DVDTitle *ta = av_mallocz(sizeof(*ta));
        if (!ta)
            return AVERROR(ENOMEM);
        memcpy(ta->name, t->name, sizeof(ta->name));
        if ((ret = title_init(T, ta, idx, f, p, ch, nb_ch, a)) < 0 || (ret > 0 && (ret = build_title(T, ta)) < 0)) {
            ff_dvd_title_free(&ta);
            return ret;
        }
        if (!ret || !ta->nb_segments) {
            av_log(T->d->log, AV_LOG_WARNING, "DVD: title %s: angle %d cannot be added\n", t->name, a + 1);
            ff_dvd_title_free(&ta);
            return 0;
        }
        ta->short_title = t->short_title;
        if ((ret = list_add(T->out, ta)) < 0) {
            ff_dvd_title_free(&ta);
            return ret;
        }
        av_log(T->d->log, AV_LOG_VERBOSE, "DVD: title %s: angle %d added\n", t->name, a + 1);
    }
    return 0;
drop:
    ff_dvd_title_free(&t);
    return ret < 0 ? ret : 0;
}

/* The chapters of title table entry idx: its PTT entries, each at its
 * program's entry cell (a program chain the entries skip gets chapter 1).
 * 0 none (logged), 1 done, < 0 error. */
static int title_chapters(Titles *T, int idx, DVDIfo *f, DVDChapter **out, int *nb_out)
{
    const DVDTable *s = &f->ptt_srpt;
    unsigned vtsn = tt_byte(T->d, 8 + 12 * idx + 6), ttn = tt_byte(T->d, 8 + 12 * idx + 7), n;
    uint32_t start, end;
    DVDChapter *ch = NULL;
    int nb = 0, alloc = 0, ret;

    *out = NULL;
    *nb_out = 0;
    n = s->size >= 2 ? AV_RB16(s->data) : 0;
    if (!ttn || ttn > n) {
        av_log(T->d->log, AV_LOG_WARNING, "DVD: VTS_%02u has no title %u\n", vtsn, ttn);
        return 0;
    }
    start = 4 * ttn + 8 <= s->size ? AV_RB32(s->data + 4 * ttn + 4) : 0;
    if (ttn != n) {
        end = 4 * ttn + 0xc <= s->size ? AV_RB32(s->data + 4 * ttn + 8) : 0;
    } else {
        uint32_t lb = s->size >= 8 ? AV_RB32(s->data + 4) : 0;
        end = lb + 1;
        if (s->size < 8 || (end & 3) == 1) {
            av_log(T->d->log, AV_LOG_WARNING, "DVD: VTS_%02u_0.IFO: the chapter table's end is off by one: worked "
                   "around\n", vtsn);
            end = lb;
        }
    }
    if ((start & 3) || end < start || (end & 3) || end - start <= 3) {
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: VTS_%02u title %u: its chapter list is broken\n", vtsn, ttn);
        return 0;
    }
    for (uint32_t k = 0; k < (end - start) >> 2; k++) {
        uint32_t at = start + 4 * k;
        unsigned pgcn = at + 2 <= s->size ? AV_RB16(s->data + at) : 0;
        unsigned pgn  = at + 4 <= s->size ? AV_RB16(s->data + at + 2) : 0;
        unsigned from = nb && ch[nb - 1].pgcn < pgcn ? ch[nb - 1].pgcn + 1u : pgcn;
        for (unsigned q = from; q <= pgcn; q++) {
            unsigned g = q == pgcn ? pgn : 1;
            const DVDPgc *p = q && q <= (unsigned)f->nb_pgcs ? f->pgcs[q - 1] : NULL;
            unsigned cell;
            if (!p || !g || g > p->nr_of_programs || !(cell = p->program_map[g - 1]) || cell > p->nr_of_cells) {
                av_log(T->d->log, AV_LOG_WARNING, "DVD: VTS_%02u title %u: no cell for program chain %u program "
                       "%u\n", vtsn, ttn, q, p ? g : 0);
                continue;
            }
            if ((ret = grow((void **)&ch, &alloc, nb, sizeof(*ch))) < 0) {
                av_free(ch);
                return ret;
            }
            ch[nb++] = (DVDChapter){ .pgcn = q, .pgn = g, .cell = cell - 1, .segment = DVD_CHAPTER_NONE };
        }
    }
    *out = ch;
    *nb_out = nb;
    return 1;
}

/* the title table entry has no title set, VTS title or chapters */
static int title_entry_empty(const DVDDisc *d, int idx)
{
    uint32_t o = 8 + 12 * idx;
    unsigned ts = tt_byte(d, o + 6), ttn = tt_byte(d, o + 7);
    unsigned ptts = o + 4 <= d->tt_srpt.size ? AV_RB16(d->tt_srpt.data + o + 2) : 0;
    return !ts || !ttn || !ptts;
}

/* the title set's titles are decoys: shared title VOBs, many program chains,
 * each chain of several cells a new timeline at every cell or failing the
 * program chain check */
static int title_set_suspicious(Titles *T, DVDIfo *f)
{
    if (f->suspicious)
        return f->suspicious == 1;
    f->suspicious = 2;
    if (!ff_dvd_title_vobs_overlap(T->d, f->index) || f->nb_pgcs <= SUSPICIOUS_PGCS)
        return 0;
    for (int i = 0; i < f->nb_pgcs; i++) {
        DVDPgc *p = f->pgcs[i];
        if (!p || p->nr_of_cells < 2)
            continue;
        for (int c = 1; c < p->nr_of_cells; c++)
            if (!(FLAGS(&p->cells[c]) & 2)) {
                if (pgc_rejected(T, f, p) < 0)
                    return 0;
                break;
            }
    }
    f->suspicious = 1;
    return 1;
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* Title table entry idx (0-based). */
static int add_title(Titles *T, int idx)
{
    unsigned vtsn = tt_byte(T->d, 8 + 12 * idx + 6);
    DVDIfo *f;
    DVDChapter *ch = NULL, *sub = NULL;
    uint16_t *pgcns = NULL;
    int nb = 0, nb_pgcs = 0, ret = 0;
    int mode = T->o->cell_mode ? T->o->cell_mode : T->scan->nb_results ? DVD_CELL_MODE_WALK : DVD_CELL_MODE_TRIM;

    if (mode == DVD_CELL_MODE_WALK && !scan_reached(T->scan, idx + 1)) {
        av_log(T->d->log, AV_LOG_INFO, "DVD: title %d left out: not reached by the navigation\n", idx + 1);
        return 0;
    }
    if (title_entry_empty(T->d, idx)) {
        av_log(T->d->log, AV_LOG_INFO, "DVD: title %d left out: its title table entry has no title set, VTS title "
               "or chapters\n", idx + 1);
        return 0;
    }
    if (vtsn > (unsigned)T->d->nr_of_title_sets)
        return 0;
    f = vtsn < (unsigned)T->d->nb_ifos ? T->d->ifos[vtsn] : NULL;
    if (!f || f->failed) {
        av_log(T->d->log, AV_LOG_INFO, "DVD: title %d left out: its title set (VTS_%02u) could not be read\n",
               idx + 1, vtsn);
        return 0;
    }
    if (title_set_suspicious(T, f)) {
        av_log(T->d->log, AV_LOG_INFO, "DVD: title %d left out: its title set (VTS_%02u) looks like decoys\n",
               idx + 1, vtsn);
        return 0;
    }
    if ((ret = title_chapters(T, idx, f, &ch, &nb)) <= 0)
        return ret;
    /* the program chains the chapters use, ascending */
    if (!(pgcns = av_malloc_array(FFMAX(nb, 1), sizeof(*pgcns))) ||
        !(sub = av_malloc_array(FFMAX(nb, 1), sizeof(*sub)))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < nb; i++) {
        int seen = 0;
        for (int j = 0; j < nb_pgcs && !seen; j++)
            seen = pgcns[j] == ch[i].pgcn;
        if (!seen)
            pgcns[nb_pgcs++] = ch[i].pgcn;
    }
    qsort(pgcns, nb_pgcs, sizeof(*pgcns), cmp_u16);
    for (int k = 0; k < nb_pgcs; k++) {
        unsigned pgcn = pgcns[k];
        DVDPgc *p = pgcn && pgcn <= (unsigned)f->nb_pgcs ? f->pgcs[pgcn - 1] : NULL;
        int ns = 0, why;
        if (!p)
            continue;
        why = pgc_rejected(T, f, p);
        if (why >= 0 && !scan_find(T->scan, idx + 1, pgcn)) {
            av_log(T->d->log, AV_LOG_INFO, "DVD: title %d: program chain %u left out: it fails the program chain "
                   "check (reason %d)\n", idx + 1, pgcn, why);
            title_skip(T, f, p);
            continue;
        }
        for (int i = 0; i < nb; i++)
            if (ch[i].pgcn == pgcn)
                sub[ns++] = ch[i];
        if ((ret = add_pgc_titles(T, idx, f, p, sub, ns, k)) < 0)
            goto end;
    }
end:
    av_free(pgcns);
    av_free(sub);
    av_free(ch);
    return ret;
}

/* b plays a's sectors with a subset of its streams and no more chapters */
static int same_title(const DVDTitle *a, const DVDTitle *b)
{
    if (a->nb_segments != b->nb_segments)
        return 0;
    for (int k = 0; k < a->nb_segments; k++) {
        const DVDSegment *x = &a->segments[k], *y = &b->segments[k];
        if (x->nb_extents != y->nb_extents)
            return 0;
        for (int e = 0; e < x->nb_extents; e++) {
            const DVDExtent *p = &x->extents[e], *q = &y->extents[e];
            if (p->count != q->count)
                return 0;
            if (a->vts->title_vob_base && b->vts->title_vob_base) {
                if (p->sector + a->vts->title_vob_base != q->sector + b->vts->title_vob_base)
                    return 0;
            } else if (a->vts != b->vts || p->sector != q->sector) {
                return 0;
            }
        }
    }
    for (int i = 0; i < b->nb_audio; i++) {
        int found = 0;
        for (int j = 0; j < a->nb_audio && !found; j++)
            found = a->audio[j].stream == b->audio[i].stream;
        if (!found)
            return 0;
    }
    for (int i = 0; i < b->nb_subp; i++) {
        int found = 0;
        for (int j = 0; j < a->nb_subp && !found; j++)
            found = a->subp[j].stream == b->subp[i].stream;
        if (!found)
            return 0;
    }
    return b->nb_chapters <= a->nb_chapters && (a->scan || !b->scan);
}

/* A title playing an earlier one's sectors with no more streams and
 * chapters is left out (logged); the search starts over after each. */
static void remove_duplicates(Titles *T)
{
    DVDTitleList *l = T->out;

    for (int i = 0; i < l->nb_titles; i++) {
        for (int j = 0; j < l->nb_titles; j++) {
            DVDTitle *a = l->titles[i], *b = l->titles[j];
            if (a == b || !same_title(a, b))
                continue;
            av_log(T->d->log, AV_LOG_INFO, "DVD: title %d left out: it is the same as title %d (VTS_%02d)\n",
                   b->title + 1, a->title + 1, b->vtsn);
            ff_dvd_title_free(&l->titles[j]);
            memmove(l->titles + j, l->titles + j + 1, (l->nb_titles - j - 1) * sizeof(*l->titles));
            l->nb_titles--;
            i = -1;
            break;
        }
    }
}

int ff_dvd_titles_build(DVDDisc *d, const DVDScan *scan, const DVDTitleOptions *o, DVDTitleList *out)
{
    Titles T = { .d = d, .scan = scan, .o = o, .out = out };
    unsigned n = d->tt_srpt.size >= 2 ? AV_RB16(d->tt_srpt.data) : 0;
    int ret = 0;

    memset(out, 0, sizeof(*out));
    /* the full cell check's VOBUs (libdvdread) */
    if (d->reader)
        DVDSetCellCheck_rec(d->reader, (dvd_cell_check_t)o->cell_check, av_get_random_seed());
    if (o->cell_mode < DVD_CELL_MODE_TRIM) {
        /* the titles the navigation reached, then the others */
        for (unsigned i = 0; i < n; i++)
            if (scan_reached(scan, i + 1) && (ret = add_title(&T, i)) < 0)
                goto fail;
        for (unsigned i = 0; i < n; i++)
            if (!scan_reached(scan, i + 1) && (ret = add_title(&T, i)) < 0)
                goto fail;
    } else {
        for (unsigned i = 0; i < n; i++)
            if ((ret = add_title(&T, i)) < 0)
                goto fail;
    }
    remove_duplicates(&T);
    return 0;
fail:
    ff_dvd_titles_free(out);
    return ret;
}

int ff_dvd_cell_check(DVDDisc *d, int vts, int pgcn, int cell, int mode)
{
    DVDTitleOptions o = { .cell_check = mode };
    DVDScan none = { 0 };
    Titles T = { .d = d, .scan = &none, .o = &o };
    DVDIfo *f = vts > 0 && vts < d->nb_ifos ? d->ifos[vts] : NULL;
    DVDPgc *p = f && !f->failed && pgcn > 0 && pgcn <= f->nb_pgcs ? f->pgcs[pgcn - 1] : NULL;

    if (!p || cell < 1 || cell > p->nr_of_cells)
        return AVERROR(EINVAL);
    DVDSetCellCheck_rec(d->reader, (dvd_cell_check_t)mode, av_get_random_seed());
    return cell_check_failed(&T, f, p, cell - 1);
}

char *ff_dvd_titles_dump(const DVDTitleList *l)
{
    AVBPrint bp;
    char *s;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (int i = 0; i < l->nb_titles; i++) {
        const DVDTitle *t = l->titles[i];
        int64_t size = 0;
        av_bprintf(&bp, "title %d name %s ttn %d vts %d pgc %d angle %d/%d cells %d chapters %d length %"PRIu32
                   " real %"PRIu32" segments %d", i, t->name, t->title + 1, t->vtsn, t->pgc->number + 1,
                   t->angle + 1, t->nr_of_angles, t->nb_cells, t->nb_chapters, t->declared, t->measured,
                   t->nb_segments);
        for (int k = 0; k < t->nb_segments; k++)
            size += t->segments[k].size;
        av_bprintf(&bp, " size %"PRId64" map ", size);
        for (int k = 0; k < t->nb_segments; k++)
            av_bprintf(&bp, "%s%s", k ? "," : "", t->segments[k].cell_map);
        if (!t->nb_segments)
            av_bprintf(&bp, "-");
        av_bprintf(&bp, " scan %s tracks V", t->scan ? t->scan->name : "-");
        for (int k = 0; k < t->nb_audio; k++)
            av_bprintf(&bp, ",A%u.%u", t->audio[k].stream, t->audio[k].attr);
        for (int k = 0; k < t->nb_subp; k++)
            av_bprintf(&bp, ",S%u.%u", t->subp[k].stream, t->subp[k].attr);
        av_bprintf(&bp, "%s\n", t->short_title ? " short" : "");
    }
    if (av_bprint_finalize(&bp, &s) < 0)
        return NULL;
    return s;
}
