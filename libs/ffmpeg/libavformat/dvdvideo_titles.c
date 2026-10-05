/*
 * DVD-Video demuxer: the title plan
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * The titles of a disc, as sector extents to read: every entry of the title
 * search table is looked at (in the order the options give); each distinct
 * program chain of its parts of title becomes a title ("3", or "3/1", "3/2"
 * for further chains), followed by one title per further angle. A title's
 * cells are one range of its program chain (one cell per angle block), chosen
 * by the cell mode: from the cells the navigation scan played (cell walk), or
 * by trimming cells that do not look like content off the chain's ends (cell
 * trim). Its segments are the runs of title-VOB blocks played without a
 * discontinuity, walked VOBU by VOBU; chapters are placed in them. Titles
 * that repeat another title are dropped; titles shorter than the minimum
 * length or whose declared and measured lengths are far apart are kept in the
 * list, not selected, with the reason.
 *
 * Cells are named by their index in the program chain; cell numbers on disc
 * are 1-based.
 */

#include <stdarg.h>

#include "dvdvideo_internal.h"

#define TICKS                   90000       /* 90 kHz ticks per second */
#define TIME_SCALE              12000       /* chapter times: 1/1,080,000,000 s = 90 kHz x 12000 */
#define NAV_INVALID_SHOWN       14          /* "navigation information invalid" warnings per title */
#define MANY_VOBUS              100         /* VOBUs to read above which the scan is announced */

static const char *const event_names[] = {
    [DVDVIDEO_EV_PTT_NO_TITLE]        = "ptt-no-title",
    [DVDVIDEO_EV_PTT_UNRESOLVED]      = "ptt-unresolved",
    [DVDVIDEO_EV_TOO_MANY_AUDIO]      = "too-many-audio",
    [DVDVIDEO_EV_TOO_MANY_SUBP]       = "too-many-subp",
    [DVDVIDEO_EV_TITLE_EMPTY]         = "title-empty",
    [DVDVIDEO_EV_TITLE_SET_MISSING]   = "title-set-missing",
    [DVDVIDEO_EV_NO_CELL_LIST]        = "no-cell-list",
    [DVDVIDEO_EV_TITLE_UNUSABLE]      = "title-unusable",
    [DVDVIDEO_EV_ANGLE_BLOCK_BROKEN]  = "angle-block-broken",
    [DVDVIDEO_EV_ANGLE_COUNT]         = "angle-count",
    [DVDVIDEO_EV_VOBUS_TO_READ]       = "vobus-to-read",
    [DVDVIDEO_EV_SHORT]               = "short",
    [DVDVIDEO_EV_FAKE_LENGTH]         = "fake-length",
    [DVDVIDEO_EV_DUPLICATE]           = "duplicate",
    [DVDVIDEO_EV_TITLE]               = "title",
    [DVDVIDEO_EV_CELLWALK_FAILED]     = "cellwalk-failed",
    [DVDVIDEO_EV_CELLS_CUT_START]     = "cells-cut-start",
    [DVDVIDEO_EV_CELLS_CUT_END]       = "cells-cut-end",
    [DVDVIDEO_EV_FAKE_CELLS]          = "fake-cells",
    [DVDVIDEO_EV_ANGLE]               = "angle",
    [DVDVIDEO_EV_ANGLE_FAILED]        = "angle-failed",
    [DVDVIDEO_EV_NAV_INVALID]         = "nav-invalid",
};

const char *ff_dvdvideo_event_name(int kind)
{
    return kind >= 0 && kind < FF_ARRAY_ELEMS(event_names) && event_names[kind] ? event_names[kind] : "?";
}

/* Everything one title plan is built with. */
typedef struct Plan {
    void                    *log;
    DVDVideoDisc            *disc;
    const DVDVideoScan      *scan;          /* NULL or failed: no results */
    DVDVideoTitleOptions     opt;
    DVDVideoTitlePlan       *out;
    int                      class[100];    /* title set: 0 not classified, 2 suspicious, 4 normal */
    int                      error;         /* out of memory or a read error */
} Plan;

/* Record a finding of the title stage: the event (with its arguments,
 * tab-separated) and a log line in plain words. */
static void event(Plan *p, int level, int kind, const char *args, const char *fmt, ...)
{
    DVDVideoTitleEvent e = { .kind = kind };
    char text[512];
    va_list ap;

    av_strlcpy(e.args, args, sizeof(e.args));
    if (!av_dynarray2_add((void **)&p->out->events, &p->out->nb_events, sizeof(e), (const uint8_t *)&e))
        p->error = AVERROR(ENOMEM);
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    av_log(p->log, level, "%s\n", text);
}

static int nr_results(const Plan *p)
{
    return p->scan && !p->scan->failed ? p->scan->nb_results : 0;
}

/* The scan result for title `title` and program chain `pgcn` (low bytes). */
static int scan_entry(const Plan *p, int title, int pgcn)
{
    for (int i = 0; i < nr_results(p); i++)
        if (p->scan->results[i].title == (title & 0xff) && p->scan->results[i].pgcn == (pgcn & 0xff))
            return i;
    return -1;
}

/* Whether the scan found a program chain of title `title`. */
static int scan_has_title(const Plan *p, int title)
{
    for (int i = 0; i < nr_results(p); i++)
        if (p->scan->results[i].title == (title & 0xff) && p->scan->results[i].pgcn)
            return 1;
    return 0;
}

/* The cell mode in effect: automatic is cell walk with scan results, else
 * cell trim. */
static int cell_mode(const Plan *p)
{
    if (p->opt.cell_mode != DVDVIDEO_CELLS_AUTO)
        return p->opt.cell_mode;
    return nr_results(p) ? DVDVIDEO_CELLS_WALK : DVDVIDEO_CELLS_TRIM;
}

static uint32_t secs(const pgc_t *pgc, int i)
{
    return ff_dvdvideo_bcd_secs(ff_dvdvideo_dvd_time(&pgc->cell_playback[i].playback_time));
}

static int cflags(const pgc_t *pgc, int i)
{
    return ff_dvdvideo_cell_flags(pgc, i);
}

static int cmd_nr(const pgc_t *pgc, int i)
{
    return pgc->cell_playback[i].cell_cmd_nr;
}

/* "h:mm:ss" of s seconds, each part as two BCD digits written in hex. */
static void bcd_time(uint32_t s, char *buf, size_t size)
{
    uint32_t tens = s / 36000, h = (tens & 0xf) << 4 | (s / 3600 - tens * 10);
    uint32_t m = s / 60 - s / 3600 * 60, sec = s % 60;

    snprintf(buf, size, "%x:%02x:%02x", h, ((m / 10) & 0xf) << 4 | m % 10, (sec / 10) << 4 | sec % 10);
}

/* s seconds packed as BCD hours / minutes / seconds in bytes 3 / 2 / 1. */
static uint32_t bcd_packed(uint32_t s)
{
    uint32_t m = s / 60 - s / 3600 * 60, sec = s % 60;

    return ((sec / 10) << 4 | sec % 10) << 8 | (((m / 10) & 0xf) << 4 | m % 10) << 16 | (s / 36000) << 28 |
           (s / 3600 - s / 36000 * 10) * 0x01000000;
}

/* A BCD playback time as "h:mm:ss"; minute and second bytes of 0x80 and above
 * are written as negative numbers would be in hex. */
static void pgc_time(uint32_t t, char *buf, size_t size)
{
    snprintf(buf, size, "%x:%02x:%02x", t >> 24, (uint32_t)(int32_t)(int8_t)(t >> 16), (uint32_t)(int32_t)(int8_t)(t >> 8));
}

/* ---- titles ---- */

static void title_free(DVDVideoTitle *t)
{
    av_freep(&t->chapters);
    av_freep(&t->cells);
    for (int i = 0; i < t->nb_segments; i++) {
        av_freep(&t->segments[i].extents);
        av_freep(&t->segments[i].runs);
    }
    av_freep(&t->segments);
    t->nb_segments = t->nb_cells = t->nb_chapters = 0;
}

void ff_dvdvideo_titles_free(DVDVideoTitlePlan **pplan)
{
    DVDVideoTitlePlan *plan = *pplan;

    if (!plan)
        return;
    for (int i = 0; i < plan->nb_titles; i++)
        title_free(&plan->titles[i]);
    av_free(plan->titles);
    av_free(plan->events);
    av_freep(pplan);
}

static const title_info_t *tt_entry(const Plan *p, int t)
{
    const tt_srpt_t *tt = p->disc->vts[0].ifo->tt_srpt;

    return tt && t >= 0 && t < tt->nr_of_srpts ? &tt->title[t] : NULL;
}

static int add_cell(Plan *p, DVDVideoTitle *t, int c)
{
    if (!av_dynarray2_add((void **)&t->cells, &t->nb_cells, sizeof(c), (const uint8_t *)&c))
        return p->error = AVERROR(ENOMEM);
    return 0;
}

/* The parts of title of title search table entry t from its title set's
 * VTS_PTT_SRPT, each with its first cell. Program chains skipped between two
 * parts get a part of their own (program 1); parts whose cell cannot be found
 * are left out. 0 when the title has no parts. */
static int ptt_list(Plan *p, int t, DVDVideoChapter **out)
{
    const title_info_t *e = tt_entry(p, t);
    int vtsn = e->title_set_nr, ttn = e->vts_ttn, nb = 0;
    const ifo_handle_t *ifo = p->disc->vts[vtsn].ifo;
    const vts_ptt_srpt_t *srpt = ifo->vts_ptt_srpt;
    const ttu_t *tu;
    char args[64];

    *out = NULL;
    if (!srpt || !ttn || ttn > srpt->nr_of_srpts) {
        snprintf(args, sizeof(args), "%d\t%d", ttn, vtsn);
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_PTT_NO_TITLE, args, "Title set %d has no part-of-title list for its "
              "title %d (title %d of the disc)", vtsn, ttn, t + 1);
        return 0;
    }
    tu = &srpt->title[ttn - 1];
    for (int k = 0; k < tu->nr_of_ptts; k++) {
        int pgcn = tu->ptt[k].pgcn, pgn = tu->ptt[k].pgn;
        int from = nb && (*out)[nb - 1].pgcn < pgcn ? (*out)[nb - 1].pgcn + 1 : pgcn;

        /* program chains skipped between two parts get a part (program 1) */
        for (int q = from; q <= pgcn; q++) {
            int g = q == pgcn ? pgn : 1;
            const pgc_t *pgc = ff_dvdvideo_disc_pgc(p->disc, vtsn, q);
            DVDVideoChapter ch = { .pgcn = q, .pgn = g, .cell = -1, .segment = DVDVIDEO_INVALID_SEGMENT };

            if (!pgc || !g || !pgc->program_map || pgc->nr_of_programs < g || !pgc->program_map[g - 1] ||
                pgc->program_map[g - 1] > pgc->nr_of_cells || !pgc->cell_playback) {
                snprintf(args, sizeof(args), "%d\t%d\t%d\t%d", vtsn, ttn, q, pgc ? g : 0);
                event(p, AV_LOG_WARNING, DVDVIDEO_EV_PTT_UNRESOLVED, args, "Title set %d, title %d: part of title "
                      "with program chain %d, program %d cannot be found; it is left out", vtsn, ttn, q, g);
                continue;
            }
            ch.cell = pgc->program_map[g - 1] - 1;
            if (!av_dynarray2_add((void **)out, &nb, sizeof(ch), (const uint8_t *)&ch)) {
                av_freep(out);
                return p->error = AVERROR(ENOMEM);
            }
        }
    }
    return nb;
}

/* The title's entry fields, chapters, scan link and stream lists for angle
 * `angle`. 0 when the angle does not exist, the chapters have no program
 * chain, or the title set declares too many streams. */
static int init_title(Plan *p, DVDVideoTitle *t, const DVDVideoChapter *chapters, int nb_chapters, int angle)
{
    const title_info_t *e = tt_entry(p, t->index);
    const ifo_handle_t *ifo;
    const vtsi_mat_t *mat;
    const pgc_t *pgc;
    char args[64];
    int na, n_audio, n_subp, aspect, wide;

    t->title_type = e->pb_ty.zero_1 << 7 | e->pb_ty.multi_or_random_pgc_title << 6 |
                    e->pb_ty.jlc_exists_in_cell_cmd << 5 | e->pb_ty.jlc_exists_in_prepost_cmd << 4 |
                    e->pb_ty.jlc_exists_in_button_cmd << 3 | e->pb_ty.jlc_exists_in_tt_dom << 2 |
                    e->pb_ty.chapter_search_or_play << 1 | e->pb_ty.title_or_time_play;
    t->vtsn    = e->title_set_nr;
    t->vts_ttn = e->vts_ttn;
    na = e->nr_of_angles;
    if (!na)
        na = 1;
    else if (na > 8)
        na = 8;
    t->angles = na;
    if (na <= angle)
        return 0;
    t->angle = angle;
    av_freep(&t->chapters);
    if (!(t->chapters = av_memdup(chapters, nb_chapters * sizeof(*chapters))))
        return p->error = AVERROR(ENOMEM);
    t->nb_chapters = nb_chapters;
    ifo = p->disc->vts[t->vtsn].ifo;
    mat = ifo->vtsi_mat;
    if (!(pgc = ff_dvdvideo_disc_pgc(p->disc, t->vtsn, chapters[0].pgcn)) || chapters[0].cell < 0) {
        snprintf(args, sizeof(args), "%d\t%d\t%d\t0", t->vtsn, t->vts_ttn, chapters[0].pgcn);
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_PTT_UNRESOLVED, args, "Title set %d, title %d: program chain %d "
              "cannot be found", t->vtsn, t->vts_ttn, chapters[0].pgcn);
        return 0;
    }
    t->scan = scan_entry(p, t->index + 1, chapters[0].pgcn);

    /* the stream lists (the full track lists come with the tracks) */
    n_audio = mat->zero_19 << 8 | mat->nr_of_vts_audio_streams;
    if (n_audio >= 9) {
        snprintf(args, sizeof(args), "%d\t%d", n_audio, t->vtsn);
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_TOO_MANY_AUDIO, args, "Title set %d declares %d audio streams (at "
              "most 8 are allowed); its title %d is left out", t->vtsn, n_audio, t->vts_ttn);
        return 0;
    }
    aspect = mat->vts_video_attr.display_aspect_ratio;
    t->audio_mask = 0;
    t->nb_audio   = 0;
    for (int i = 0; i < n_audio; i++) {
        int b = pgc->audio_control[i] >> 8;

        if (b & 0x80) {
            t->audio_mask |= 1 << (b & 7);
            t->audio[t->nb_audio++] = b & 7;
        }
    }
    for (int s = 0; s < n_audio; s++)
        if (!(t->audio_mask >> s & 1))
            t->audio[t->nb_audio++] = s;
    n_subp = mat->zero_20[16] << 8 | mat->nr_of_vts_subp_streams;
    if (n_subp >= 0x21) {
        /* the audio count is what is reported here */
        snprintf(args, sizeof(args), "%d\t%d", n_audio, t->vtsn);
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_TOO_MANY_SUBP, args, "Title set %d declares %d subpicture streams "
              "(at most 32 are allowed); its title %d is left out", t->vtsn, n_subp, t->vts_ttn);
        return 0;
    }
    wide = aspect == 3;
    t->nb_subp = 0;
    for (int i = 0; i < n_subp; i++) {
        uint32_t sc = pgc->subp_control[i];
        int b0 = sc >> 24, b1 = sc >> 16 & 0xff, b2 = sc >> 8 & 0xff;

        if (!(b0 & 0x80))
            continue;
        t->subp[t->nb_subp++] = (aspect == 3 ? b1 : b0) & 0x1f;
        if (n_subp == 1 && wide && (b1 & 0x1f) == 1 && !(b2 & 0x1f))
            t->subp[t->nb_subp++] = 0;
    }
    return 1;
}

/* ---- cell lists ---- */

/* The cell to play for cell i: i itself outside blocks; in an angle block
 * (which must start at i) the cell of the title's angle (the last cell when
 * the block has fewer), checking that the block has as many cells as the
 * title has angles. -1 for a broken block. */
static int angle_cell(Plan *p, const DVDVideoTitle *t, const pgc_t *pgc, int i)
{
    int f = cflags(pgc, i), n = pgc->nr_of_cells;
    char args[64];

    if (!(f & 0x30))
        return i;
    if ((f >> 4 & 3) != 1)
        av_log(p->log, AV_LOG_DEBUG, "titles: cell %d: a block that is not an angle block\n", i + 1);
    if ((f & 0xc0) != 0x40)
        av_log(p->log, AV_LOG_DEBUG, "titles: cell %d: an angle block taken from a cell that does not start it\n",
               i + 1);
    for (int q = i;; q++) {
        int pf = cflags(pgc, q);

        if (pf > 0xbf) {
            int k = q - i, idx;

            if (k + 1 != t->angles) {
                snprintf(args, sizeof(args), "%d\t%d\t%d", i + 1, t->angles, k + 1);
                event(p, AV_LOG_WARNING, DVDVIDEO_EV_ANGLE_COUNT, args, "The angle block at cell %d has %d cells; "
                      "the title has %d angles", i + 1, k + 1, t->angles);
            }
            idx = i + FFMIN(t->angle, k);
            return idx < n ? idx : -1;
        }
        if ((pf & 0x30) != 0x10 || q + 1 >= n) {
            int at = (pf & 0x30) != 0x10 ? q + 1 : i + 1;

            snprintf(args, sizeof(args), "%d", at);
            event(p, AV_LOG_WARNING, DVDVIDEO_EV_ANGLE_BLOCK_BROKEN, args, "The angle block is broken at cell %d",
                  at);
            return -1;
        }
    }
}

/* The cell after cell i, or after the end of the block i is in; -1 at the end
 * of the chain or for a broken block. */
static int next_cell(Plan *p, const pgc_t *pgc, int i)
{
    int n = pgc->nr_of_cells, q = i;
    char args[64];

    if (cflags(pgc, i) & 0x30) {
        for (;; q++) {
            int pf = cflags(pgc, q);

            if (pf > 0xbf)
                break;
            if ((pf & 0x30) != 0x10 || q + 1 >= n) {
                int at = (pf & 0x30) != 0x10 ? q + 1 : i + 1;

                snprintf(args, sizeof(args), "%d", at);
                event(p, AV_LOG_WARNING, DVDVIDEO_EV_ANGLE_BLOCK_BROKEN, args, "The angle block is broken at "
                      "cell %d", at);
                return -1;
            }
        }
    }
    return q + 1 < n ? q + 1 : -1;
}

/* The title's cell list = cells first..last (one per angle block). 0 when an
 * angle block is broken. */
static int build_cell_list(Plan *p, DVDVideoTitle *t, const pgc_t *pgc, int first, int last)
{
    int n = pgc->nr_of_cells, c = first;
    char args[64];

    if (first) {
        snprintf(args, sizeof(args), "%d", first);
        event(p, AV_LOG_VERBOSE, DVDVIDEO_EV_CELLS_CUT_START, args, "Title %s: the first %d cells of its program "
              "chain are left out", t->name, first);
    }
    if (n - 1 != last) {
        snprintf(args, sizeof(args), "%d\t%d", last + 2, n);
        event(p, AV_LOG_VERBOSE, DVDVIDEO_EV_CELLS_CUT_END, args, "Title %s: cells %d to %d of its program chain "
              "are left out", t->name, last + 2, n);
    }
    t->nb_cells = 0;
    while (c <= last) {
        int e = angle_cell(p, t, pgc, c);

        if (e < 0)
            return 0;
        if (add_cell(p, t, e) < 0)
            return 0;
        if ((c = next_cell(p, pgc, e)) < 0)
            return 1;
    }
    return 1;
}

static int cell_of(const pgc_t *pgc, int num)
{
    return num >= 1 && num <= pgc->nr_of_cells ? num - 1 : -1;
}

/* Cell rejection, errors kept in the plan. */
static int rejected(Plan *p, int vtsn, const pgc_t *pgc, int i, int quick, int prev)
{
    int ret = ff_dvdvideo_cell_rejected(p->disc, vtsn, pgc, i, quick, prev);

    if (ret < 0) {
        p->error = ret;
        return 1;
    }
    return ret;
}

static int deep_failed(Plan *p, int vtsn, const pgc_t *pgc, int i)
{
    int ret = ff_dvdvideo_cell_deep_check_failed(p->disc, vtsn, pgc, i);

    if (ret < 0) {
        p->error = ret;
        return 1;
    }
    return ret;
}

/* Whether a run of cells fa..fb looks like content: walking it, at least one
 * cell passes the quick cell check and the playback times add up to 5 s or
 * more (the walk stops as soon as both hold; rejected cells' times count). */
static int run_has_content(Plan *p, int vtsn, const pgc_t *pgc, int fa, int fb)
{
    int all_rejected = 1;
    uint32_t sum = 0;

    for (int x = fa;; x++) {
        if (!rejected(p, vtsn, pgc, x, 1, -1))
            all_rejected = 0;
        sum += secs(pgc, x);
        if (x == fb)
            return !all_rejected && sum > 4;
        if (!all_rejected && sum >= 5)
            return 1;
        if (x + 1 >= pgc->nr_of_cells)
            return 0;
    }
}

/* Playback seconds of the cells of run a..b (cell numbers). */
static uint32_t run_seconds(const pgc_t *pgc, int a, int b)
{
    int x = cell_of(pgc, a), fb = cell_of(pgc, b);
    uint32_t sum = 0;

    if (x < 0 || fb < 0)
        return 0;
    for (;; x++) {
        sum += secs(pgc, x);
        if (x == fb || x + 1 >= pgc->nr_of_cells)
            return sum;
    }
}

/* The cell list from the cells the scan played: runs of consecutive cell
 * numbers in playing order; runs without content dropped from the front,
 * then from the back; of what is left the longer end run is kept until one
 * run remains (the first one on a tie), which becomes the cell list. */
static int cell_walk(Plan *p, DVDVideoTitle *t, const pgc_t *pgc, int pgcn)
{
    const DVDVideoScanResult *e;
    int (*runs)[2] = NULL, nb = 0, head = 0, ret = 0;

    if (t->scan < 0)
        return 0;
    e = &p->scan->results[t->scan];
    if (!e->nb_cells) {
        av_log(p->log, AV_LOG_DEBUG, "titles: title %d: the scan played no cells of its program chain\n", e->title);
        return 0;
    }
    if (pgcn != e->pgcn)
        return 0;
    for (int k = 0; k < e->nb_cells; k++) {
        int c = e->cells[k];

        if (nb && runs[nb - 1][1] + 1 == c) {
            runs[nb - 1][1] = c;
        } else {
            int r[2] = { c, c };

            if (!av_dynarray2_add((void **)&runs, &nb, sizeof(r), (const uint8_t *)r)) {
                p->error = AVERROR(ENOMEM);
                return 0;
            }
        }
    }
    /* runs without content at the front, then at the back */
    while (nb - head > 1) {
        int fa = cell_of(pgc, runs[head][0]), fb = cell_of(pgc, runs[head][1]);

        if (fa >= 0 && fb >= 0 && run_has_content(p, t->vtsn, pgc, fa, fb))
            break;
        head++;
    }
    while (nb - head > 1) {
        int fa = cell_of(pgc, runs[nb - 1][0]), fb = cell_of(pgc, runs[nb - 1][1]);

        if (fa >= 0 && fb >= 0 && run_has_content(p, t->vtsn, pgc, fa, fb))
            break;
        nb--;
    }
    /* the longer end run stays */
    while (nb - head > 1) {
        if (run_seconds(pgc, runs[head][0], runs[head][1]) < run_seconds(pgc, runs[nb - 1][0], runs[nb - 1][1]))
            head++;
        else
            nb--;
    }
    if (nb - head == 1) {
        int f = cell_of(pgc, runs[head][0]), l = cell_of(pgc, runs[head][1]);

        ret = f >= 0 && l >= 0 && build_cell_list(p, t, pgc, f, l);
    }
    av_free(runs);
    return ret;
}

/* The run of cells around cell i: back to a cell with a discontinuity
 * (included) or to just after a cell whose command may link, forward to a
 * cell whose command may link (included) or to just before a cell with a
 * discontinuity. */
static void run_around(const pgc_t *pgc, int i, int *ps, int *pe)
{
    int n = pgc->nr_of_cells, start = i, end = i;

    while (start && (cflags(pgc, start) & 0x32) != 2) {
        int prev = start - 1;

        if (ff_dvdvideo_cell_cmd_may_link(pgc, cmd_nr(pgc, prev)))
            break;
        start = prev;
    }
    while (end < n) {
        if (ff_dvdvideo_cell_cmd_may_link(pgc, cmd_nr(pgc, end)))
            break;
        if (end + 1 >= n || (cflags(pgc, end + 1) & 0x32) == 2)
            break;
        end++;
    }
    *ps = start;
    *pe = end;
}

/* Playback seconds of cells a..b of the chain. */
static uint32_t range_seconds(const pgc_t *pgc, int a, int b)
{
    uint32_t sum = 0;
    int x = a;

    while (x < b) {
        sum += secs(pgc, x);
        if (x + 1 >= pgc->nr_of_cells)
            return sum;
        x++;
    }
    return sum + secs(pgc, b);
}

/* From the `to` end of cells from..to (either direction), the first cell that
 * looks like content, else `from`. With strict a cell only has to pass the
 * cell check; otherwise it must also last more than 2 s, be in a block, span
 * several VOBUs or have more than frames of playback time, and (unless it
 * lasts over 30 s or is near `from`) the cells after it must last more than
 * 2 s. */
static int boundary(Plan *p, int vtsn, const pgc_t *pgc, int from, int to, int strict)
{
    int n = pgc->nr_of_cells, d = FFABS(from - to);

#define AT(k) (from < to ? from + (k) : from - (k))
    if (from == to)
        return from;
    if (strict) {
        for (int k = d; k >= 1; k--) {
            int c = AT(k);

            if (c >= 0 && c < n && !rejected(p, vtsn, pgc, c, 0, -1))
                return c;
        }
        return from;
    }
    for (int k = d;;) {
        int step = 1, c = AT(k);

        if (c >= 0 && c < n) {
            const cell_playback_t *cell = &pgc->cell_playback[c];
            uint32_t pt = ff_dvdvideo_dvd_time(&cell->playback_time);
            int looks_real = secs(pgc, c) > 2 &&
                             ((cflags(pgc, c) & 0x30) || cell->first_sector != cell->last_vobu_start_sector ||
                              pt > 0xff);

            if (looks_real && !rejected(p, vtsn, pgc, c, 0, -1)) {
                int c1 = AT(k - 1), c2 = AT(k - 2);
                uint32_t s1 = k >= 1 && c1 >= 0 && c1 < n ? secs(pgc, c1) : 0;
                uint32_t s2 = k >= 2 && c2 >= 0 && c2 < n ? secs(pgc, c2) : 0;

                if ((pt & 0xffffff00) > 0x3000 || k < 4 || s1 > 2)
                    return c;
                if (s2 > 2)
                    return c;
                step = 3;
            }
        }
        k -= step;
        if (k <= 0)
            return from;
    }
#undef AT
}

/* The longest run of cells in rising sector order within first..last;
 * accepted when the chain's other playing time is under 26 % of its declared
 * time (else, unless quiet, a finding). *a / *b = the run (-1 none). */
static int fake_share_ok(Plan *p, const pgc_t *pgc, int first, int last, int quiet, int *a, int *b)
{
    int n = pgc->nr_of_cells, start = first;
    uint32_t best = 0, total, kept, other, pct;
    char args[32];

    *a = *b = -1;
    while (start >= 0) {
        int e = start, next = -1;
        uint32_t d;

        while (e != last) {
            if (e + 1 >= n)
                break;
            if (pgc->cell_playback[e].first_sector <= pgc->cell_playback[e + 1].first_sector) {
                e++;
            } else {
                next = e + 1;
                break;
            }
        }
        d = range_seconds(pgc, start, e);
        if (best < d) {
            *a   = start;
            *b   = e;
            best = d;
        }
        start = next;
    }
    if (!best) {
        if (!quiet)
            event(p, AV_LOG_WARNING, DVDVIDEO_EV_FAKE_CELLS, "100", "Fake cells make up 100%% of the program "
                  "chain's playing time: the title is taken as fake");
        return 0;
    }
    total = ff_dvdvideo_bcd_secs(ff_dvdvideo_dvd_time(&pgc->playback_time));
    kept  = range_seconds(pgc, *a, *b);
    other = total > kept ? total - kept : 0;
    if (!total) {
        /* a declared time of 0 gives no share */
        av_log(p->log, AV_LOG_WARNING, "The program chain declares no playing time; its share of fake cells "
               "cannot be computed and its cells are not trimmed\n");
        return 0;
    }
    pct = other * 100 / total;
    if (pct < 26)
        return 1;
    if (!quiet) {
        snprintf(args, sizeof(args), "%"PRIu32, pct);
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_FAKE_CELLS, args, "Fake cells make up %"PRIu32"%% of the program "
              "chain's playing time: the title is taken as fake", pct);
    }
    return 0;
}

/* First trimming of a chain without a scan result (titles of at most 4
 * chapters): a 3..5-cell chain whose rejected first and last cells come from
 * other VOBs keeps its inner cells; a chain of 5 or more cells with rejected
 * ends keeps its one long run (10 s or more), trimmed to cells that pass the
 * cell check. */
static int cell_trim_edges(Plan *p, DVDVideoTitle *t, const pgc_t *pgc)
{
    const cell_position_t *pos = pgc->cell_position;
    int n = pgc->nr_of_cells, f = 0, l = n - 1, ls = -1, le = -1, found = 0, x, y;

    if (t->nb_chapters > 4)
        return 0;
    if (n >= 3 && n <= 5 && rejected(p, t->vtsn, pgc, f, 0, -1) && rejected(p, t->vtsn, pgc, l, 0, -1)) {
        int s = 1, e = n - 2;

        if (pos[f].vob_id_nr != pos[s].vob_id_nr && pos[l].vob_id_nr != pos[e].vob_id_nr) {
            int same_cell = pos[f].vob_id_nr == pos[l].vob_id_nr && pos[f].cell_nr == pos[l].cell_nr;
            int ok = (pos[f].cell_nr == 1 && pos[l].cell_nr == 1) || same_cell, prev = -1;

            if (ok && s != e) {
                for (int c = s;;) {
                    if (pos[c].vob_id_nr != pos[e].vob_id_nr || rejected(p, t->vtsn, pgc, c, 0, prev) ||
                        cmd_nr(pgc, c)) {
                        ok = 0;
                        break;
                    }
                    prev = c;
                    if (++c == e)
                        break;
                }
            }
            /* the last inner cell is checked in every case */
            if (!rejected(p, t->vtsn, pgc, e, 0, prev) && ok && build_cell_list(p, t, pgc, s, e))
                return 1;
        }
    }
    if (n < 5 || !rejected(p, t->vtsn, pgc, f, 0, -1) || !rejected(p, t->vtsn, pgc, l, 0, -1))
        return 0;
    for (int c = f; c <= l;) {
        int s, e;

        run_around(pgc, c, &s, &e);
        if (range_seconds(pgc, s, e) > 9) {
            if (found)
                return 0;
            found = 1;
            ls = s;
            le = e;
        }
        if (e == l || e + 1 >= n)
            break;
        c = e + 1;
    }
    if (!found)
        return 0;
    x = boundary(p, t->vtsn, pgc, le, ls, 1);
    y = boundary(p, t->vtsn, pgc, x, le, 1);
    return build_cell_list(p, t, pgc, x, y);
}

/* The entry cell of 1-based program pr, -1 when the chain has none. */
static int program_entry(const pgc_t *pgc, int pr)
{
    int e;

    if (pr < 1 || pr > pgc->nr_of_programs || !pgc->program_map)
        return -1;
    e = pgc->program_map[pr - 1];
    return e && e <= pgc->nr_of_cells ? e - 1 : -1;
}

/* The 1-based program cell i is in: the last program whose entry cell is not
 * after it (0 when the first program starts after it). */
static int program_of(const pgc_t *pgc, int i)
{
    int num = (i + 1) & 0xff;

    for (int k = 0; k < pgc->nr_of_programs; k++)
        if (num < pgc->program_map[k])
            return k;
    return pgc->nr_of_programs;
}

/* Titles of up to 4 chapters: the longest rising run between the first and
 * the last cell that pass both cell checks. */
static int trim_small(Plan *p, int vtsn, const pgc_t *pgc, int *a, int *b)
{
    int n = pgc->nr_of_cells, f = 0, l;

#define GOOD(i) (!rejected(p, vtsn, pgc, i, 0, -1) && !deep_failed(p, vtsn, pgc, i))
    if (!n)
        return 0;
    while (!GOOD(f)) {
        if (f == n - 1)
            return 0;
        f++;
    }
    l = n - 1;
    while (!GOOD(l)) {
        if (l == f)
            return 0;
        l--;
    }
#undef GOOD
    return fake_share_ok(p, pgc, f, l, 1, a, b);
}

/* The refinement of the range s..e around a middle chapter: start at the
 * entry of the program after the one s is in, extended back over earlier runs
 * of 30 s or more and over cells that pass the deep check; end before short
 * trailing discontinuity cells; both ends trimmed to content; then the
 * longest rising run of that. A range without such a start or end gives no
 * trimmed range. */
static int refine(Plan *p, int vtsn, const pgc_t *pgc, int s, int e, int *ra, int *rb)
{
    int a = program_entry(pgc, program_of(pgc, s) + 1), b = s, c, after, d = e, x, y, entry;

    if (a < 0) {
        av_log(p->log, AV_LOG_WARNING, "No program follows the one of cell %d; the title cannot be trimmed\n", s + 1);
        return 0;
    }
    if (a != s) {
        for (int z = a;;) {
            int start, prev, ps, pe;

            run_around(pgc, z, &start, &pe);
            if (start == s) {
                b = s;
                break;
            }
            b = start;
            if ((prev = start - 1) < 0) {
                av_log(p->log, AV_LOG_WARNING, "No cell before cell %d; the title cannot be trimmed\n", start + 1);
                return 0;
            }
            run_around(pgc, prev, &ps, &pe);
            z = ps;
            if (range_seconds(pgc, ps, pe) < 30)
                break;
            b = s;
            if (ps == s)
                break;
        }
    }
    /* back from the program entry while the cell before passes the deep check */
    for (c = a;;) {
        if (c == b) {
            after = b;
            break;
        }
        if (c - 1 < 0) {
            av_log(p->log, AV_LOG_WARNING, "No cell before cell %d; the title cannot be trimmed\n", c + 1);
            return 0;
        }
        if (deep_failed(p, vtsn, pgc, c - 1)) {
            after = c;
            break;
        }
        c--;
    }
    /* before short trailing cells that only carry a discontinuity */
    while (d != after) {
        if ((cflags(pgc, d) & 0xf2) == 2 && secs(pgc, d) < 2) {
            if (d - 1 < 0) {
                av_log(p->log, AV_LOG_WARNING, "No cell before cell %d; the title cannot be trimmed\n", d + 1);
                return 0;
            }
            d--;
        } else {
            break;
        }
    }
    x = boundary(p, vtsn, pgc, a, after, 0);
    if ((entry = program_entry(pgc, program_of(pgc, d))) < 0) {
        av_log(p->log, AV_LOG_WARNING, "The program of cell %d has no entry cell; the title cannot be trimmed\n",
               d + 1);
        return 0;
    }
    y = boundary(p, vtsn, pgc, entry, d, 0);
    return fake_share_ok(p, pgc, x, y, 0, ra, rb);
}

/* Second trimming of a chain without a scan result. Titles of up to 4
 * chapters keep the longest rising run between the first and last cells that
 * pass the cell checks; longer titles take the part around their middle
 * chapter bounded by cells whose commands may link, refined to program and
 * run boundaries. When nothing is found the cell list is left empty (still a
 * success); 0 only for a broken chapter list. */
static int cell_trim_runs(Plan *p, DVDVideoTitle *t, const pgc_t *pgc)
{
    int n = pgc->nr_of_cells, found, a = -1, b = -1;

    if (t->nb_chapters < 5) {
        found = trim_small(p, t->vtsn, pgc, &a, &b);
    } else {
        int first = t->chapters[0].cell, mid = t->chapters[t->nb_chapters / 2].cell, s, e;

        if (first < 0 || mid < 0)
            return 0;
        /* back to just after a cell whose command may link */
        for (s = mid; s != first;) {
            if (s - 1 < 0 || s - 1 >= n)
                return 0;
            if (ff_dvdvideo_cell_cmd_may_link(pgc, cmd_nr(pgc, s - 1)))
                break;
            s--;
        }
        /* forward to the first cell whose command may link */
        for (e = mid; e + 1 < n;) {
            e++;
            if (ff_dvdvideo_cell_cmd_may_link(pgc, cmd_nr(pgc, e)))
                break;
        }
        found = fake_share_ok(p, pgc, s, e, 0, &a, &b) && refine(p, t->vtsn, pgc, s, e, &a, &b);
    }
    if (found && a >= 0)
        return build_cell_list(p, t, pgc, a, b);
    t->nb_cells = 0;
    return 1;
}

static int pgc_rejected(Plan *p, int vtsn, int pgcn, const pgc_t *pgc)
{
    int reason, ret = ff_dvdvideo_pgc_rejection(p->disc, vtsn, pgcn, pgc, &reason);

    if (ret < 0) {
        p->error = ret;
        return 1;
    }
    return ret;
}

/* Step 1: the cell list, by the cell mode. */
static int choose_cells(Plan *p, DVDVideoTitle *t, const pgc_t *pgc)
{
    int mode = cell_mode(p);

#define TRIM() (cell_trim_edges(p, t, pgc) || cell_trim_runs(p, t, pgc))
    if (t->scan < 0 && pgc_rejected(p, t->vtsn, t->pgcn, pgc)) {
        t->nb_cells = 0;
        return 1;
    }
    switch (mode) {
    case DVDVIDEO_CELLS_WALK:
        if (t->scan < 0) {
            t->nb_cells = 0;
            return 1;
        }
        return cell_walk(p, t, pgc, t->pgcn);
    case DVDVIDEO_CELLS_TRIM:
        return TRIM();
    case DVDVIDEO_CELLS_WALK_TRIM:
        if (t->scan < 0)
            return TRIM();
        if (cell_walk(p, t, pgc, t->pgcn))
            return 1;
        event(p, AV_LOG_INFO, DVDVIDEO_EV_CELLWALK_FAILED, "", "Title %s: the cells the navigation plays do not "
              "give a cell list; its cells are trimmed instead", t->name);
        if (!pgc_rejected(p, t->vtsn, t->pgcn, pgc))
            return TRIM();
        t->nb_cells = 0;
        return 1;
    }
    return 0;
#undef TRIM
}

/* Step 2: declared seconds (sum of the cells' playback times) and measured
 * seconds (sum of the cells' NAV-pack durations). */
static void measure(Plan *p, DVDVideoTitle *t, const pgc_t *pgc)
{
    uint64_t ticks = 0;

    t->declared_secs = 0;
    for (int k = 0; k < t->nb_cells; k++) {
        uint32_t d = 0;
        int ret;

        t->declared_secs += secs(pgc, t->cells[k]);
        if ((ret = ff_dvdvideo_cell_duration(p->disc, t->vtsn, pgc, t->cells[k], 0, 0, &d)) < 0)
            p->error = ret;
        ticks += ret > 0 ? d : 0;
    }
    t->measured_secs = ticks / TICKS;
}

int ff_dvdvideo_lengths_disagree(uint32_t declared, uint32_t measured)
{
    uint32_t diff = declared > measured ? declared - measured : measured - declared;

    return !declared || (diff > 300 && diff * 100 / declared > 30);
}

/* Whether the title is left without segments as fake. */
static int looks_fake(Plan *p, const DVDVideoTitle *t, const pgc_t *pgc)
{
    const cell_playback_t *cells = pgc->cell_playback;
    int fake = t->nb_cells && ff_dvdvideo_lengths_disagree(t->declared_secs, t->measured_secs);

    if (!fake && t->nb_cells >= 2 && t->scan < 0 && ff_dvdvideo_disc_title_vobs_overlap(p->disc, t->vtsn)) {
        /* cells that are not joined seamlessly must follow each other in the
         * VOB: one that starts before the previous one ends overlaps, a gap
         * of more than 0x400 blocks is suspect */
        int n = t->nb_cells, overlaps = 0, last = 0;

        for (int i = 1; i < n; i++) {
            const cell_playback_t *c = &cells[t->cells[i]], *q = &cells[t->cells[i - 1]];

            if (cflags(pgc, t->cells[i]) & 0x08)
                continue;
            if (c->first_sector <= q->last_sector) {
                overlaps++;
                last = i;
            } else if (c->first_sector - q->last_sector > 0x400) {
                fake = 1;
                break;
            }
        }
        if (!fake && overlaps >= 2)
            fake = 1;
        if (!fake && overlaps == 1) {
            /* the overlap is fine when the cells around it, skipping
             * seamless ones, do not overlap each other */
            int j = last, k = last;

            for (;;) {
                int kk;

                if (j == 1) {
                    j = 0;
                    break;
                }
                kk = j--;
                if (!(cflags(pgc, t->cells[kk - 1]) & 0x08))
                    break;
            }
            while (k < n - 1 && (cflags(pgc, t->cells[k + 1]) & 0x08))
                k++;
            fake = cells[t->cells[k]].last_sector >= cells[t->cells[j]].first_sector;
        }
    }
    return fake;
}

/* Whether the VOBU at s of cell c must be found by reading NAV packs (rather
 * than from the VOBU map); the map lookups made to decide that record what
 * they read. */
static int needs_scan(Plan *p, int vtsn, const pgc_t *pgc, int c, uint32_t s)
{
    const cell_playback_t *cell = &pgc->cell_playback[c];
    int f = cflags(pgc, c);

    if (s < cell->last_vobu_start_sector && !(f & 0xf4)) {
        if (f & 0x01) {
            int usable = !p->disc->vts[vtsn].map_distrusted && ff_dvdvideo_vobu_chain_usable(p->disc, vtsn, cell) > 0 &&
                         ff_dvdvideo_vobu_next(p->disc, vtsn, s) >= 0;
            return !usable;
        }
        ff_dvdvideo_vobu_next(p->disc, vtsn, s);
        return 0;
    }
    return 1;
}

static int segment_append(Plan *p, DVDVideoSegment *seg, uint32_t sector, uint32_t count)
{
    DVDVideoExtent e = { .sector = sector, .count = count };

    if (seg->nb_extents)
        e.logical = seg->extents[seg->nb_extents - 1].logical + seg->extents[seg->nb_extents - 1].count;
    if (!av_dynarray2_add((void **)&seg->extents, &seg->nb_extents, sizeof(e), (const uint8_t *)&e))
        return p->error = AVERROR(ENOMEM);
    return 0;
}

/* Note cell c (0-based) in the segment's runs of consecutive cell numbers. */
static int segment_add_cell(Plan *p, DVDVideoSegment *seg, int c)
{
    int num = c + 1;

    if (seg->nb_runs) {
        int *r = seg->runs[seg->nb_runs - 1];

        if (num >= r[0] && num <= r[1])
            return 0;
        if (r[1] + 1 == num) {
            r[1]++;
            return 0;
        }
    }
    {
        int r[2] = { num, num };

        if (!av_dynarray2_add((void **)&seg->runs, &seg->nb_runs, sizeof(r), (const uint8_t *)r))
            return p->error = AVERROR(ENOMEM);
    }
    return 0;
}

/* Size in bytes and the label of its cell runs ("1-19", or "(1-3,5)"). */
static void segment_finish(DVDVideoSegment *seg)
{
    char part[32];

    seg->size = seg->nb_extents ? (uint64_t)(seg->extents[seg->nb_extents - 1].logical +
                                             seg->extents[seg->nb_extents - 1].count) << 11 : 0;
    seg->label[0] = 0;
    if (!seg->nb_runs)
        return;
    if (seg->nb_runs > 1)
        av_strlcat(seg->label, "(", sizeof(seg->label));
    for (int i = 0; i < seg->nb_runs; i++) {
        if (seg->runs[i][0] == seg->runs[i][1])
            snprintf(part, sizeof(part), "%s%d", i ? "," : "", seg->runs[i][0]);
        else
            snprintf(part, sizeof(part), "%s%d-%d", i ? "," : "", seg->runs[i][0], seg->runs[i][1]);
        av_strlcat(seg->label, part, sizeof(seg->label));
    }
    if (seg->nb_runs > 1)
        av_strlcat(seg->label, ")", sizeof(seg->label));
    av_freep(&seg->runs);
    seg->nb_runs = 0;
}

/* vobu_s_ptm / vobu_e_ptm of the VOBU at `sector`: from its record, or its NAV
 * pack read now (and recorded). 0 when it holds none. */
static int vobu_times(Plan *p, int vtsn, uint32_t sector, uint32_t *s, uint32_t *e)
{
    DVDVideoNavFields f;
    DVDVideoVobu rec;
    int64_t next;
    uint32_t len;
    int ret;

    if (ff_dvdvideo_vobu_get(p->disc, vtsn, sector, &rec)) {
        *s = rec.vobu_s_ptm;
        *e = rec.vobu_e_ptm;
        return rec.next != DVDVIDEO_VOBU_NOT_A_NAV_PACK;
    }
    if ((ret = ff_dvdvideo_vobu_read_nav(p->disc, vtsn, sector, &f)) <= 0) {
        if (ret < 0)
            p->error = ret;
        return 0;
    }
    if ((ret = ff_dvdvideo_vobu_register(p->disc, vtsn, &f, sector, 1, &next, &len)) < 0)
        p->error = ret;
    *s = f.vobu_s_ptm;
    *e = f.vobu_e_ptm;
    return 1;
}

/* The VOBU that holds the last block of a segment: from the VOBU map (the
 * entry before the first one not below it), or by stepping through the
 * segment's last cell. */
static uint32_t last_vobu(Plan *p, int vtsn, const pgc_t *pgc, const DVDVideoSegment *seg)
{
    const DVDVideoTitleSet *ts = &p->disc->vts[vtsn];
    const DVDVideoExtent *e = seg->nb_extents ? &seg->extents[seg->nb_extents - 1] : NULL;
    uint32_t x = e ? e->sector + e->count - 1 : (uint32_t)-1;
    const cell_playback_t *cell = &pgc->cell_playback[seg->last_cell];

    if (!ts->map_distrusted) {
        int lo = 0, hi = ts->nb_vobu_starts;

        while (lo < hi) {
            int mid = (lo + hi) / 2;

            if (ts->vobu_starts[mid] < x)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < ts->nb_vobu_starts && lo)
            return ts->vobu_starts[lo - 1];
    }
    if (cell->first_sector <= x && x <= cell->last_sector) {
        uint32_t s0 = cell->last_vobu_start_sector < x ? cell->last_vobu_start_sector : cell->first_sector;

        for (;;) {
            DVDVideoVobuStep st;
            int ret = ff_dvdvideo_vobu_step(p->disc, vtsn, cell, s0, &st);

            if (ret <= 0) {
                if (ret < 0)
                    p->error = ret;
                break;
            }
            if (x < st.next)
                return s0;
            s0 = st.next;
        }
    }
    av_log(p->log, AV_LOG_DEBUG, "titles: the last VOBU of a segment ending at %"PRIu32" was not found\n", x);
    return cell->last_vobu_start_sector;
}

/* PTS span (90 kHz) from the start of a segment to the end of the VOBU at
 * `sector`; -1 when a NAV pack is missing or the times are out of order. */
static int64_t span(Plan *p, int vtsn, const DVDVideoSegment *seg, uint32_t sector)
{
    uint32_t a, s1, e1, s2, e2;

    if (!seg->nb_extents)
        return 0;
    if ((a = seg->extents[0].sector) == sector)
        return 0;
    if (!vobu_times(p, vtsn, a, &s1, &e1) || !vobu_times(p, vtsn, sector, &s2, &e2))
        return -1;
    if (e1 < s1 || s2 < e1 || e2 < s2)
        return -1;
    return e2 - s1;
}

/* A cell ends before its last block: tell where (the playing time up to the
 * end of the segments so far), as a warning at most 14 times per title. */
static int report_nav_invalid(Plan *p, DVDVideoTitle *t, const pgc_t *pgc)
{
    uint64_t ticks = 0;
    char at[32];

    for (int i = 0; i < t->nb_segments; i++) {
        const DVDVideoSegment *seg = &t->segments[i];
        uint32_t last = last_vobu(p, t->vtsn, pgc, seg), s1, e1, s2, e2;

        if (!seg->nb_extents || seg->extents[0].sector == last)
            continue;
        if (!vobu_times(p, t->vtsn, seg->extents[0].sector, &s1, &e1) ||
            !vobu_times(p, t->vtsn, last, &s2, &e2) || !(s1 <= e1 && e1 <= s2 && s2 <= e2))
            return 0;
        ticks += e2 - s1;
    }
    bcd_time(ticks / TICKS, at, sizeof(at));
    t->nav_invalid_count++;
    event(p, t->nav_invalid_count <= NAV_INVALID_SHOWN ? AV_LOG_WARNING : AV_LOG_DEBUG, DVDVIDEO_EV_NAV_INVALID, at,
          "Title %s: the navigation information is invalid at %s (a cell ends before its last block)", t->name, at);
    return 1;
}

/* The next VOBU after s in cell c and the length of this one. In an
 * interleaved cell whose units chain up, the unit's own length is used when
 * the VOBU map has a VOBU right after it whose NAV pack agrees. */
static int step(Plan *p, int vtsn, const pgc_t *pgc, int c, uint32_t s, int il, DVDVideoVobuStep *out)
{
    const DVDVideoTitleSet *ts = &p->disc->vts[vtsn];
    const cell_playback_t *cell = &pgc->cell_playback[c];
    DVDVideoVobu rec;

    if (il && !ts->map_distrusted && ff_dvdvideo_vobu_get(p->disc, vtsn, s, &rec)) {
        uint32_t len = rec.ilvu_ea + 1, target = s + len;
        int lo = 0, hi = ts->nb_vobu_starts;

        while (lo < hi) {
            int mid = (lo + hi) / 2;

            if (ts->vobu_starts[mid] < target)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < ts->nb_vobu_starts && ts->vobu_starts[lo] == target && lo) {
            DVDVideoVobuStep st;
            uint32_t q = ts->vobu_starts[lo - 1];
            int ret = ff_dvdvideo_vobu_step_from_nav(p->disc, vtsn, cell, q, &st);

            if (ret < 0)
                return ret;
            if (ret && q + st.len == target) {
                out->next = st.next;
                out->len  = len;
                return 1;
            }
        }
    }
    return ff_dvdvideo_vobu_step(p->disc, vtsn, cell, s, out);
}

/* Step 3: the segments. A new segment starts at the first cell, at a cell
 * that does not follow the previous one in the chain, at a cell with a
 * discontinuity (also one on a skipped non-block cell); within a segment
 * contiguous VOBUs join into one extent. */
static int build_segments(Plan *p, DVDVideoTitle *t, const pgc_t *pgc)
{
    const DVDVideoTitleSet *ts = &p->disc->vts[t->vtsn];
    uint32_t size = ts->title_vobs_sectors, to_scan = 0, cells_to_scan = 0;
    char args[64];

    /* count the VOBUs that need NAV packs read */
    for (int k = 0; k < t->nb_cells; k++) {
        int c = t->cells[k], f = cflags(pgc, c);
        const cell_playback_t *cell = &pgc->cell_playback[c];
        int simple = cell->first_sector < cell->last_vobu_start_sector && !(f & 0xf4), probe_ok = 0;
        uint32_t s = cell->first_sector;

        if (simple)
            probe_ok = !(f & 0x01) ||
                       (!ts->map_distrusted && ff_dvdvideo_vobu_chain_usable(p->disc, t->vtsn, cell) > 0 &&
                        ff_dvdvideo_vobu_next(p->disc, t->vtsn, cell->first_sector) >= 0);
        if (simple && !(f & 0x01))
            ff_dvdvideo_vobu_next(p->disc, t->vtsn, cell->first_sector);
        if (probe_ok)
            continue;
        if (s >= size) {
            av_log(p->log, AV_LOG_DEBUG, "titles: title %s: VOBU %"PRIu32" lies past the title VOBs\n", t->name, s);
            return 0;
        }
        cells_to_scan++;
        for (;;) {
            int64_t n;

            if (needs_scan(p, t->vtsn, pgc, c, s))
                to_scan++;
            if (s == cell->last_vobu_start_sector || s > cell->last_sector)
                break;
            if ((n = ff_dvdvideo_vobu_next(p->disc, t->vtsn, s)) < 0) {
                av_log(p->log, AV_LOG_DEBUG, "titles: title %s: no VOBU follows block %"PRIu32"\n", t->name, s);
                /* asked once more before giving up, as the walk always does */
                ff_dvdvideo_vobu_next(p->disc, t->vtsn, s);
                return 0;
            }
            s = n;
            if (s >= size) {
                av_log(p->log, AV_LOG_DEBUG, "titles: title %s: VOBU %"PRIu32" lies past the title VOBs\n",
                       t->name, s);
                return 0;
            }
        }
    }
    if (to_scan > MANY_VOBUS) {
        snprintf(args, sizeof(args), "%"PRIu32"\t%"PRIu32, cells_to_scan, to_scan);
        event(p, AV_LOG_INFO, DVDVIDEO_EV_VOBUS_TO_READ, args, "Title %s: the VOBUs of %"PRIu32" cells (%"PRIu32") "
              "have to be found from their NAV packs; this can take a while", t->name, cells_to_scan, to_scan);
    }

    for (int i = 0; i < t->nb_cells; i++) {
        int c = t->cells[i], f = cflags(pgc, c), ns = !!(f & 0x02), new_seg = ns, il = 0;
        const cell_playback_t *cell = &pgc->cell_playback[c];
        uint32_t s = cell->first_sector, next = s;

        if (i) {
            int q = t->cells[i - 1];

            if (c <= q) {
                new_seg = 1;
            } else {
                for (int k = q + 1; k < c; k++) {
                    int kf = cflags(pgc, k);

                    if (!(kf & 0x30) && (kf & 0x02))
                        ns = 1;
                }
                new_seg = ns;
            }
        }
        if (f & 0x04) {
            int bt = f >> 4 & 3;

            if ((bt == 0 && f <= 0x3f) || (bt == 1 && f >> 6 >= 1 && f >> 6 <= 3)) {
                int ret = ff_dvdvideo_ilvu_chain_reaches(p->disc, t->vtsn, cell->first_sector, cell->last_sector + 1);

                if (ret < 0)
                    return p->error = ret, 0;
                il = ret;
            } else {
                av_log(p->log, AV_LOG_DEBUG, "titles: cell %d: interleaved with an unexpected block type\n", c + 1);
            }
        }
        while (next != DVDVIDEO_VOBU_END_OF_CELL && s < cell->last_sector) {
            DVDVideoVobuStep st;
            DVDVideoSegment *seg;
            int ret;

            if (s >= size) {
                av_log(p->log, AV_LOG_DEBUG, "titles: title %s: VOBU %"PRIu32" lies past the title VOBs\n",
                       t->name, s);
                return 0;
            }
            needs_scan(p, t->vtsn, pgc, c, s);
            if ((ret = step(p, t->vtsn, pgc, c, s, il, &st)) <= 0) {
                if (ret < 0)
                    p->error = ret;
                av_log(p->log, AV_LOG_DEBUG, "titles: title %s: the VOBU at block %"PRIu32" cannot be walked\n",
                       t->name, s);
                return 0;
            }
            next = st.next;
            if (!t->nb_segments || new_seg) {
                DVDVideoSegment empty = { .last_cell = c };

                if (!av_dynarray2_add((void **)&t->segments, &t->nb_segments, sizeof(empty), (const uint8_t *)&empty))
                    return p->error = AVERROR(ENOMEM), 0;
                if (segment_append(p, &t->segments[t->nb_segments - 1], s, st.len) < 0)
                    return 0;
            } else {
                DVDVideoSegment *last = &t->segments[t->nb_segments - 1];
                DVDVideoExtent *e = last->nb_extents ? &last->extents[last->nb_extents - 1] : NULL;

                if (e && e->sector + e->count == s)
                    e->count += st.len;
                else if (segment_append(p, last, s, st.len) < 0)
                    return 0;
            }
            seg = &t->segments[t->nb_segments - 1];
            seg->last_cell = c;
            if (segment_add_cell(p, seg, c) < 0)
                return 0;
            if (next == DVDVIDEO_VOBU_END_OF_CELL) {
                next = s + st.len;
                if (next < cell->last_sector && !report_nav_invalid(p, t, pgc))
                    return 0;
            }
            new_seg = 0;
            s = next;
        }
    }
    for (int i = 0; i < t->nb_segments; i++)
        segment_finish(&t->segments[i]);
    return 1;
}

/* The segment and byte offset of block x: the first segment with an extent
 * holding it. 0 when none. */
static int locate(const DVDVideoTitle *t, uint32_t x, uint32_t *pseg, uint64_t *poff)
{
    for (int k = 0; k < t->nb_segments; k++) {
        uint64_t before = 0;

        for (int j = 0; j < t->segments[k].nb_extents; j++) {
            const DVDVideoExtent *e = &t->segments[k].extents[j];

            if (e->sector <= x && x < e->sector + e->count) {
                *pseg = k;
                *poff = (before + (x - e->sector)) * DVDVIDEO_BLOCK_SIZE;
                return 1;
            }
            before += e->count;
        }
    }
    return 0;
}

/* Step 4: chapters to their angle's cell, then to a segment and byte offset
 * (and time); chapters that fall nowhere are dropped, and an invalid first
 * chapter may become the start of the title. */
static void place_chapters(Plan *p, DVDVideoTitle *t, const pgc_t *pgc)
{
    int have_prev = 0, k;
    uint32_t prev_seg = 0;
    uint64_t prev_off = 0;

    if (t->angle)
        for (k = 0; k < t->nb_chapters; k++)
            if (t->chapters[k].cell >= 0)
                t->chapters[k].cell = angle_cell(p, t, pgc, t->chapters[k].cell);
    for (k = 0; k < t->nb_chapters; k++) {
        DVDVideoChapter *ch = &t->chapters[k];
        uint64_t ok_sum = 0;
        int64_t here;
        uint32_t seg;
        uint64_t off;

        if (ch->cell < 0 || !locate(t, pgc->cell_playback[ch->cell].first_sector, &seg, &off)) {
            ch->segment = DVDVIDEO_INVALID_SEGMENT;
            continue;
        }
        ch->segment = seg;
        ch->offset  = off;
        /* a chapter at or before the previous one is dropped */
        if (have_prev && (seg < prev_seg || (seg == prev_seg && off <= prev_off))) {
            ch->segment = DVDVIDEO_INVALID_SEGMENT;
            continue;
        }
        for (uint32_t j = 0; j < seg; j++) {
            int64_t d = span(p, t->vtsn, &t->segments[j], last_vobu(p, t->vtsn, pgc, &t->segments[j]));

            if (d >= 0)
                ok_sum += d;
            else
                av_log(p->log, AV_LOG_DEBUG, "titles: segment %"PRIu32": no span\n", j);
        }
        here = span(p, t->vtsn, &t->segments[seg], pgc->cell_playback[ch->cell].first_sector);
        if (here < 0) {
            av_log(p->log, AV_LOG_DEBUG, "titles: segment %"PRIu32": no span to the chapter\n", seg);
            here = 0;
        }
        ch->time  = (here + ok_sum) * TIME_SCALE;
        have_prev = 1;
        prev_seg  = seg;
        prev_off  = off;
    }
    if (t->nb_chapters > 2) {
        while (t->chapters[0].segment == DVDVIDEO_INVALID_SEGMENT) {
            if (t->chapters[1].segment != DVDVIDEO_INVALID_SEGMENT) {
                if (t->chapters[1].segment || t->chapters[1].offset) {
                    DVDVideoChapter *ch = &t->chapters[0];

                    ch->segment = 0;
                    ch->offset  = 0;
                    ch->time    = 0;
                    ch->cell    = t->nb_cells ? t->cells[0] : -1;
                }
                break;
            }
            memmove(&t->chapters[0], &t->chapters[1], (t->nb_chapters - 1) * sizeof(*t->chapters));
            if (--t->nb_chapters <= 2)
                break;
        }
    }
    for (int i = k = 0; i < t->nb_chapters; i++)
        if (t->chapters[i].segment != DVDVIDEO_INVALID_SEGMENT)
            t->chapters[k++] = t->chapters[i];
    t->nb_chapters = k;
}

/* The title's cell list, lengths, segments and chapter positions. 0 when the
 * cell list cannot be made or a VOBU cannot be walked. A title that looks fake
 * is left without segments. */
static int build(Plan *p, DVDVideoTitle *t, const pgc_t *pgc)
{
    if (!choose_cells(p, t, pgc)) {
        av_log(p->log, AV_LOG_DEBUG, "titles: title %s: no cell list could be made\n", t->name);
        return 0;
    }
    measure(p, t, pgc);
    if (looks_fake(p, t, pgc)) {
        av_log(p->log, AV_LOG_DEBUG, "titles: title %s: looks fake; no segments are built\n", t->name);
        return 1;
    }
    if (!t->segments_built) {
        if (!build_segments(p, t, pgc))
            return 0;
        t->segments_built = 1;
    }
    place_chapters(p, t, pgc);
    return 1;
}

/* ---- the titles of the disc ---- */

static int push_title(Plan *p, DVDVideoTitle *t)
{
    if (!av_dynarray2_add((void **)&p->out->titles, &p->out->nb_titles, sizeof(*t), (const uint8_t *)t)) {
        title_free(t);
        return p->error = AVERROR(ENOMEM);
    }
    memset(t, 0, sizeof(*t));
    return 0;
}

/* Builds one title (and its further angles) from the parts of title
 * `chapters` of program chain pgcn of entry t. */
static void add_title(Plan *p, int t, const DVDVideoChapter *chapters, int nb_chapters, const pgc_t *pgc, int pgcn,
                      int sub)
{
    DVDVideoTitle title = { .index = t, .pgcn = pgcn, .scan = -1 };
    uint32_t declared_pgc = ff_dvdvideo_bcd_secs(ff_dvdvideo_dvd_time(&pgc->playback_time));
    char name[16], dur[32], args[128];
    int fake, angles;

    if (!nb_chapters)
        return;
    if (sub)
        snprintf(name, sizeof(name), "%d/%d", t + 1, sub);
    else
        snprintf(name, sizeof(name), "%d", t + 1);
    av_strlcpy(title.name, name, sizeof(title.name));
    if (!init_title(p, &title, chapters, nb_chapters, 0)) {
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_TITLE_UNUSABLE, name, "Title %s cannot be used", name);
        title_free(&title);
        return;
    }
    pgc_time(ff_dvdvideo_dvd_time(&pgc->playback_time), dur, sizeof(dur));
    if (!build(p, &title, pgc)) {
        snprintf(args, sizeof(args), "%s\t%s", name, dur);
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_NO_CELL_LIST, args, "Title %s (%s): no list of its cells could be "
              "made; it is left out", name, dur);
        title_free(&title);
        return;
    }
    bcd_time(title.declared_secs, dur, sizeof(dur));
    fake = title.nb_cells && ff_dvdvideo_lengths_disagree(title.declared_secs, title.measured_secs);
    if (!title.nb_segments && !fake) {
        av_log(p->log, AV_LOG_DEBUG, "titles: title %s: no segments; left out\n", name);
        title_free(&title);
        return;
    }
    if (fake) {
        uint32_t real = bcd_packed(title.measured_secs);
        char realstr[32];

        snprintf(realstr, sizeof(realstr), "%x:%02x:%02x", real >> 24, real >> 16 & 0xff, real >> 8 & 0xff);
        snprintf(args, sizeof(args), "%s\t%s\t%s", name, dur, realstr);
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_FAKE_LENGTH, args, "Title %s declares %s but its NAV packs play %s "
              "(%"PRIu32" cells, title set %d, program chain %d): it looks fake and is not selected", name, dur,
              realstr, title.nb_cells, title.vtsn, pgcn);
        title.not_selected = DVDVIDEO_TITLE_FAKE;
        push_title(p, &title);
        return;
    }
    if (declared_pgc < (uint32_t)p->opt.min_length || title.declared_secs < (uint32_t)p->opt.min_length) {
        uint32_t d = FFMIN(declared_pgc, title.declared_secs);

        snprintf(args, sizeof(args), "%s\t%"PRIu32"\t%d", name, d, p->opt.min_length);
        event(p, AV_LOG_VERBOSE, DVDVIDEO_EV_SHORT, args, "Title %s lasts %"PRIu32" s, less than the minimum length "
              "of %d s: it is not selected", name, d, p->opt.min_length);
        title.not_selected = DVDVIDEO_TITLE_SHORT;
    }
    snprintf(args, sizeof(args), "%s\t%d\t%s", name, title.nb_cells, dur);
    event(p, AV_LOG_VERBOSE, DVDVIDEO_EV_TITLE, args, "Title %s: %d cells, %s (title set %d, program chain %d)",
          name, title.nb_cells, dur, title.vtsn, pgcn);
    angles = title.angles;
    if (push_title(p, &title) < 0)
        return;
    for (int a = 1; a < angles; a++) {
        DVDVideoTitle t2 = { .index = t, .pgcn = pgcn, .scan = -1 };
        int ok;

        av_strlcpy(t2.name, name, sizeof(t2.name));
        ok = init_title(p, &t2, chapters, nb_chapters, a) && build(p, &t2, pgc) && t2.nb_segments;
        snprintf(args, sizeof(args), "%d\t%s", a + 1, name);
        if (!ok) {
            event(p, AV_LOG_WARNING, DVDVIDEO_EV_ANGLE_FAILED, args, "Angle %d of title %s cannot be used", a + 1,
                  name);
            title_free(&t2);
            return;
        }
        t2.not_selected = p->out->titles[p->out->nb_titles - 1].not_selected;
        if (push_title(p, &t2) < 0)
            return;
        event(p, AV_LOG_VERBOSE, DVDVIDEO_EV_ANGLE, args, "Angle %d of title %s added", a + 1, name);
    }
}

/* Builds the titles of title search table entry t (0-based). */
static void enumerate_title(Plan *p, int t)
{
    const title_info_t *e = tt_entry(p, t);
    int vtsn = e->title_set_nr, ttn = e->vts_ttn, nb, nb_pgcns = 0, *pgcns = NULL;
    DVDVideoChapter *list = NULL, *sub = NULL;
    char args[32];

    av_log(p->log, AV_LOG_DEBUG, "titles: title %d = VTS_%d/TTN_%d\n", t + 1, vtsn, ttn);
    if (cell_mode(p) == DVDVIDEO_CELLS_WALK && !scan_has_title(p, t + 1))
        return;
    if (!e->title_set_nr || !e->vts_ttn || !e->nr_of_ptts) {
        snprintf(args, sizeof(args), "%d", t);
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_TITLE_EMPTY, args, "Title %d of the title search table is empty", t + 1);
        return;
    }
    if (p->disc->nb_vts < vtsn)
        return;
    if (!p->disc->vts[vtsn].ifo) {
        snprintf(args, sizeof(args), "%d", t + 1);
        event(p, AV_LOG_WARNING, DVDVIDEO_EV_TITLE_SET_MISSING, args, "Title %d: its title set %d cannot be used",
              t + 1, vtsn);
        return;
    }
    /* the first title of a title set classifies it once: title VOBs shared
     * with another title set, more than 80 program chains, and every
     * multi-cell chain made only of discontinuous cells or rejected:
     * suspicious, its titles are left out */
    if (!p->class[vtsn]) {
        const pgcit_t *pgcit = p->disc->vts[vtsn].ifo->vts_pgcit;
        int suspicious = ff_dvdvideo_disc_title_vobs_overlap(p->disc, vtsn) && pgcit && pgcit->nr_of_pgci_srp > 80;

        for (int q = 0; suspicious && q < pgcit->nr_of_pgci_srp; q++) {
            const pgc_t *pgc = pgcit->pgci_srp[q].pgc;

            if (!pgc || pgc->nr_of_cells < 2)
                continue;
            for (int c = 1; c < pgc->nr_of_cells; c++) {
                if (!(cflags(pgc, c) & 0x02)) {
                    if (!pgc_rejected(p, vtsn, q + 1, pgc))
                        suspicious = 0;
                    break;
                }
            }
        }
        p->class[vtsn] = suspicious ? 2 : 4;
    }
    if (p->class[vtsn] == 2) {
        av_log(p->log, AV_LOG_WARNING, "Title set %d looks like a structure-protection set (shared title VOBs, more "
               "than 80 program chains of discontinuous or rejected cells); title %d is left out\n", vtsn, t + 1);
        return;
    }
    if ((nb = ptt_list(p, t, &list)) <= 0)
        return;
    /* the distinct program chains, ascending */
    for (int k = 0; k < nb; k++) {
        int have = 0;

        for (int j = 0; j < nb_pgcns && !have; j++)
            have = pgcns[j] == list[k].pgcn;
        if (!have && !av_dynarray2_add((void **)&pgcns, &nb_pgcns, sizeof(int), (const uint8_t *)&(int){ list[k].pgcn }))
            p->error = AVERROR(ENOMEM);
    }
    for (int i = 0; i < nb_pgcns; i++)
        for (int j = i + 1; j < nb_pgcns; j++)
            if (pgcns[j] < pgcns[i])
                FFSWAP(int, pgcns[i], pgcns[j]);
    if (!(sub = av_malloc_array(nb, sizeof(*sub)))) {
        p->error = AVERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < nb_pgcns && !p->error; i++) {
        const pgc_t *pgc = ff_dvdvideo_disc_pgc(p->disc, vtsn, pgcns[i]);
        int nb_sub = 0;

        if (!pgc)
            continue;
        if (pgc_rejected(p, vtsn, pgcns[i], pgc) && scan_entry(p, t + 1, pgcns[i]) < 0) {
            av_log(p->log, AV_LOG_DEBUG, "titles: title %d: program chain %d of title set %d is rejected\n",
                   t + 1, pgcns[i], vtsn);
            continue;
        }
        for (int k = 0; k < nb; k++)
            if (list[k].pgcn == pgcns[i])
                sub[nb_sub++] = list[k];
        add_title(p, t, sub, nb_sub, pgc, pgcns[i], i);
    }
end:
    av_free(sub);
    av_free(list);
    av_free(pgcns);
}

/* Whether titles a and b play the same blocks with the same streams. */
static int same_content(const Plan *p, const DVDVideoTitle *a, const DVDVideoTitle *b)
{
    uint32_t sa0 = p->disc->vts[a->vtsn].title_vobs_base, sb0 = p->disc->vts[b->vtsn].title_vobs_base;

    if (a->nb_segments != b->nb_segments)
        return 0;
    for (int i = 0; i < a->nb_segments; i++) {
        const DVDVideoSegment *x = &a->segments[i], *y = &b->segments[i];

        if (x->nb_extents != y->nb_extents)
            return 0;
        for (int j = 0; j < x->nb_extents; j++) {
            const DVDVideoExtent *ea = &x->extents[j], *eb = &y->extents[j];
            uint32_t xa = sa0 ? sa0 + ea->sector : 0, xb = sb0 ? sb0 + eb->sector : 0;

            if (ea->count != eb->count)
                return 0;
            if (!xa || !xb ? !(a->vtsn == b->vtsn && ea->sector == eb->sector) : xa != xb)
                return 0;
        }
    }
    if (b->nb_audio) {
        if (!a->nb_audio)
            return 0;
        for (int i = 0; i < b->nb_audio; i++) {
            int have = 0;

            for (int j = 0; j < a->nb_audio && !have; j++)
                have = a->audio[j] == b->audio[i];
            if (!have)
                return 0;
        }
    }
    if (b->nb_subp) {
        if (!a->nb_subp)
            return 0;
        for (int i = 0; i < b->nb_subp; i++) {
            int have = 0;

            for (int j = 0; j < a->nb_subp && !have; j++)
                have = a->subp[j] == b->subp[i];
            if (!have)
                return 0;
        }
    }
    if (b->nb_chapters > a->nb_chapters)
        return 0;
    return !(a->scan < 0 && b->scan >= 0);
}

/* Drops titles that repeat an earlier-checked title, starting over after each
 * removal. Titles that are not selected as fake take no part. */
static void remove_duplicates(Plan *p)
{
    DVDVideoTitlePlan *o = p->out;

again:
    for (int ai = 0; ai < o->nb_titles; ai++) {
        for (int bi = 0; bi < o->nb_titles; bi++) {
            DVDVideoTitle *a = &o->titles[ai], *b = &o->titles[bi];
            char args[64];

            if (ai == bi || a->not_selected == DVDVIDEO_TITLE_FAKE || b->not_selected == DVDVIDEO_TITLE_FAKE ||
                !same_content(p, a, b))
                continue;
            snprintf(args, sizeof(args), "%d\t#%02d\t#%02d", b->vtsn, a->index + 1, b->index + 1);
            event(p, AV_LOG_INFO, DVDVIDEO_EV_DUPLICATE, args, "Title %s repeats title %s (same blocks and streams); "
                  "it is left out", b->name, a->name);
            title_free(b);
            memmove(b, b + 1, (o->nb_titles - bi - 1) * sizeof(*b));
            o->nb_titles--;
            goto again;
        }
    }
}

int ff_dvdvideo_titles_plan(void *log, DVDVideoDisc *disc, const DVDVideoScan *scan, const DVDVideoTitleOptions *opt,
                            DVDVideoTitlePlan **out)
{
    Plan p = { .log = log, .disc = disc, .scan = scan, .opt = *opt };
    int n = disc->vts[0].ifo->tt_srpt ? disc->vts[0].ifo->tt_srpt->nr_of_srpts : 0, order = opt->title_order;

    *out = NULL;
    if (!(p.out = av_mallocz(sizeof(*p.out))))
        return AVERROR(ENOMEM);
    /* automatic: scan-first with the automatic or cell-walk mode, else the
     * title search table's order */
    if (order == DVDVIDEO_ORDER_AUTO)
        order = opt->cell_mode == DVDVIDEO_CELLS_AUTO || opt->cell_mode == DVDVIDEO_CELLS_WALK ?
                DVDVIDEO_ORDER_SCAN_FIRST : DVDVIDEO_ORDER_TABLE;
    if (order == DVDVIDEO_ORDER_SCAN_FIRST) {
        /* first the titles the scan reached, then the others */
        for (int t = 0; t < n && !p.error; t++)
            if (scan_has_title(&p, t + 1))
                enumerate_title(&p, t);
        for (int t = 0; t < n && !p.error; t++)
            if (!scan_has_title(&p, t + 1))
                enumerate_title(&p, t);
    } else {
        for (int t = 0; t < n && !p.error; t++)
            enumerate_title(&p, t);
    }
    if (!p.error)
        remove_duplicates(&p);
    if (p.error < 0) {
        ff_dvdvideo_titles_free(&p.out);
        return p.error;
    }
    *out = p.out;
    return 0;
}
