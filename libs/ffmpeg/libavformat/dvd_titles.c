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
#include "libavutil/lfg.h"
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
#define CHECK_SAMPLE        50          /* VOBUs a cell check reads when it does not read them all */

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
    AVLFG                   lfg;
    int                     lfg_ready;
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

/* The cell to play for cell i: in an angle block (its first cell) the
 * angle's cell, -1 when the block is broken. */
static int angle_cell(Titles *T, const DVDPgc *p, int i, int angle, int angles)
{
    uint8_t f = FLAGS(&p->cells[i]);
    int k = i;

    if (!BLOCK_TYPE(f))
        return i;
    if (BLOCK_TYPE(f) != 1 || BLOCK_MODE(f) != 1)
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: program chain %d cell %d: an angle cell that does not start an "
               "angle block\n", p->number + 1, i + 1);
    for (;;) {
        uint8_t g = FLAGS(&p->cells[k]);
        if (BLOCK_MODE(g) == 3) {
            int size = k - i + 1, pick = FFMIN(angle, size - 1);
            if (size != angles)
                av_log(T->d->log, AV_LOG_WARNING, "DVD: program chain %d: the angle block at cell %d has %d "
                       "cells, the title %d angles\n", p->number + 1, i + 1, size, angles);
            return i + pick < p->nr_of_cells ? i + pick : -1;
        }
        if (BLOCK_TYPE(g) != 1) {
            av_log(T->d->log, AV_LOG_WARNING, "DVD: program chain %d: broken angle block around cell %d\n",
                   p->number + 1, k + 1);
            return -1;
        }
        if (k + 1 >= p->nr_of_cells)
            break;
        k++;
    }
    av_log(T->d->log, AV_LOG_WARNING, "DVD: program chain %d: broken angle block around cell %d\n",
           p->number + 1, i + 1);
    return -1;
}

/* The cell after cell i (after its whole angle block), -1 when none. */
static int next_cell(Titles *T, const DVDPgc *p, int i)
{
    int k = i;

    if (!BLOCK_TYPE(FLAGS(&p->cells[i])))
        return i + 1 < p->nr_of_cells ? i + 1 : -1;
    for (;;) {
        uint8_t g = FLAGS(&p->cells[k]);
        if (BLOCK_MODE(g) == 3)
            return k + 1 < p->nr_of_cells ? k + 1 : -1;
        if (BLOCK_TYPE(g) != 1) {
            av_log(T->d->log, AV_LOG_WARNING, "DVD: program chain %d: broken angle block around cell %d\n",
                   p->number + 1, k + 1);
            return -1;
        }
        if (k + 1 >= p->nr_of_cells)
            break;
        k++;
    }
    av_log(T->d->log, AV_LOG_WARNING, "DVD: program chain %d: broken angle block around cell %d\n",
           p->number + 1, i + 1);
    return -1;
}

/* the cell of an interleaved angle block lying first (highest 0) / last
 * (highest 1) on the disc */
static int angle_block_extreme(Titles *T, const DVDPgc *p, int i, int highest)
{
    uint8_t f = FLAGS(&p->cells[i]);
    int first, last, best = i;

    if ((f & 0x34) != 0x14)
        return i;
    if (BLOCK_MODE(f) == 1) {
        first = i;
        for (last = i + 1; last < p->nr_of_cells && BLOCK_MODE(FLAGS(&p->cells[last])) == 2; last++)
            ;
        if (last >= p->nr_of_cells || BLOCK_MODE(FLAGS(&p->cells[last])) != 3) {
            av_log(T->d->log, AV_LOG_DEBUG, "DVD: program chain %d: the angle block of cell %d has no end\n",
                   p->number + 1, i + 1);
            return i;
        }
    } else if (BLOCK_MODE(f) == 3) {
        last = i;
        for (first = i - 1; first >= 0 && BLOCK_MODE(FLAGS(&p->cells[first])) == 2; first--)
            ;
        if (first < 0 || BLOCK_MODE(FLAGS(&p->cells[first])) != 1) {
            av_log(T->d->log, AV_LOG_DEBUG, "DVD: program chain %d: the angle block of cell %d has no start\n",
                   p->number + 1, i + 1);
            return i;
        }
    } else {
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: program chain %d: cell %d is inside an angle block\n",
               p->number + 1, i + 1);
        return i;
    }
    for (int k = first; k <= last; k++)
        if (highest ? p->cells[k].first_sector > p->cells[best].first_sector
                    : p->cells[k].first_sector < p->cells[best].first_sector)
            best = k;
    return best;
}

/* ---- VOBUs of a cell ---- */

static int cell_check_failed(Titles *T, DVDIfo *f, DVDPgc *p, int i);
static int cell_rejected(Titles *T, DVDIfo *f, DVDPgc *p, int i, int quick, int prev);

/* The next VOBU and the length of the one at sector, by its NAV pack
 * (recorded, else read): *next DVD_VOBU_END after the cell's last. */
static int nav_next_len(Titles *T, DVDIfo *f, const DVDCell *c, uint32_t s, uint32_t *next, uint32_t *len)
{
    const DVDVobuRec *r = ff_dvd_vobu_find(T->d, f, s, NULL);
    DVDNav n;

    if (!r) {
        if (!ff_dvd_nav_read(T->d, f, s, &n) || !ff_dvd_vobu_register(T->d, f, &n, s, 1, next, len)) {
            av_log(T->d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: no VOBU at sector %"PRIu32"\n", f->index, s);
            return 0;
        }
        if (*next == DVD_VOBU_SELF) {
            if (c->last_vobu_start_sector == s) {
                *next = DVD_VOBU_END;
            } else {
                av_log(T->d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: the VOBU at %"PRIu32" names itself as the next "
                       "one\n", f->index, s);
                *next = s + *len;
            }
        }
        return 1;
    }
    if (r->next == DVD_VOBU_NO_NAV)
        return 0;
    *next = r->next & 0x10000000 ? DVD_VOBU_END : (r->next & 0xfffffff) + s;
    *len  = r->vobu_ea + 1;
    return 1;
}

/* A seamless-angle cell's first VOBUs and last VOBU follow each other by
 * their NAV packs (answered once per cell). */
static int seamless_angle_ok(Titles *T, DVDIfo *f, DVDCell *c)
{
    uint32_t lvs = c->last_vobu_start_sector, s = c->first_sector, n, l;
    int ok = 0;

    if (FLAGS(c) & 0xf4)
        return 0;
    if (c->seamless_angle_checked)
        return c->seamless_angle_checked == 2;
    for (int k = 0; k < 5; k++) {
        if (!nav_next_len(T, f, c, s, &n, &l) || n != s + l)
            goto end;
        s = n;
        if (s >= lvs)
            break;
    }
    ok = nav_next_len(T, f, c, lvs, &n, &l);
end:
    if (!ok)
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: a seamless angle cell's VOBUs do not follow each other\n",
               f->index);
    c->seamless_angle_checked = ok ? 2 : 1;
    return ok;
}

/* The next VOBU and the length of the VOBU at s in cell c: by the VOBU
 * address map inside the cell, by the NAV packs for its last VOBU and in
 * angle blocks / interleaved cells. */
static int next_len(Titles *T, DVDIfo *f, DVDCell *c, uint32_t s, uint32_t *next, uint32_t *len)
{
    uint32_t n;

    if (!ff_dvd_vobu_start_check(T->d, f, s)) {
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: sector %"PRIu32" starts no VOBU\n", f->index, s);
        return 0;
    }
    if (s < c->last_vobu_start_sector && !(FLAGS(c) & 0xf4)) {
        int ok;
        if (!(FLAGS(c) & 1))
            ok = ff_dvd_vobu_next(T->d, f, s, &n);
        else
            ok = ff_dvd_map_trusted(T->d, f) && seamless_angle_ok(T, f, c) && ff_dvd_vobu_next(T->d, f, s, &n);
        if (ok && ff_dvd_map_trusted(T->d, f)) {
            *next = n;
            *len  = n - s;
            return 1;
        }
    }
    return nav_next_len(T, f, c, s, next, len);
}

/* The presentation start and end of the VOBU at sector s of cell i (read
 * when not recorded; without read_anyway only when the cell passes its
 * checks). */
static int vobu_ptm(Titles *T, DVDIfo *f, DVDPgc *p, int i, uint32_t s, uint32_t *sp, uint32_t *ep, int read_anyway)
{
    const DVDVobuRec *r = ff_dvd_vobu_find(T->d, f, s, NULL);
    DVDNav n;
    uint32_t next, len;

    if (!r && !read_anyway && cell_rejected(T, f, p, i, 1, i ? i - 1 : -1) && cell_check_failed(T, f, p, i))
        return 0;
    if (!r)
        r = ff_dvd_vobu_find(T->d, f, s, NULL);
    if (r) {
        if (r->next == DVD_VOBU_NO_NAV)
            return 0;
        *sp = r->vobu_s_ptm;
        *ep = r->vobu_e_ptm;
        return 1;
    }
    if (!ff_dvd_nav_read(T->d, f, s, &n))
        return 0;
    *sp = n.vobu_s_ptm;
    *ep = n.vobu_e_ptm;
    ff_dvd_vobu_register(T->d, f, &n, s, 1, &next, &len);
    return 1;
}

/* The cell's real length in 90 kHz ticks: its last VOBU's end against its
 * first VOBU's start. */
static int cell_duration(Titles *T, DVDIfo *f, DVDPgc *p, int i, uint64_t *dur, int read_anyway)
{
    const DVDCell *c = &p->cells[i];
    uint32_t s1, e1, s2, e2;

    if (!vobu_ptm(T, f, p, i, c->first_sector, &s1, &e1, read_anyway) ||
        !vobu_ptm(T, f, p, i, c->last_vobu_start_sector, &s2, &e2, read_anyway))
        return 0;
    if (s1 > e1 || e2 < s2 || e2 < e1)
        return 0;
    *dur = e2 - s1;
    return 1;
}

/* a known broken VOBU among the cell's VOBUs */
static int cell_has_bad_vobu(Titles *T, DVDIfo *f, const DVDCell *c)
{
    uint32_t s = c->first_sector;
    do {
        int64_t nk;
        const DVDVobuRec *r = ff_dvd_vobu_find(T->d, f, s, &nk);
        if (r && r->next == DVD_VOBU_NO_NAV)
            return 1;
        s = (uint32_t)nk;
    } while (s <= c->last_vobu_start_sector);
    return 0;
}

static uint64_t random64(Titles *T)
{
    if (!T->lfg_ready) {
        av_lfg_init(&T->lfg, av_get_random_seed());
        T->lfg_ready = 1;
    }
    return (uint64_t)av_lfg_get(&T->lfg) << 32 | av_lfg_get(&T->lfg);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* The full cell check: 1 when the cell fails (a broken VOBU known among its
 * VOBUs, its first or last VOBU or one of the VOBUs read no NAV pack, or
 * presentation times out of order). */
static int cell_check_failed(Titles *T, DVDIfo *f, DVDPgc *p, int i)
{
    DVDCell *c = &p->cells[i];
    uint32_t *list = NULL, s = c->first_sector, s1, e1, s2, e2;
    int nb = 0, alloc = 0, failed = 1;

    if (c->last_vobu_start_sector < c->first_sector || cell_has_bad_vobu(T, f, c))
        return 1;
    if (ff_dvd_vobu_bad(T->d, f, c->first_sector) ||
        (c->last_vobu_start_sector != c->first_sector && ff_dvd_vobu_bad(T->d, f, c->last_vobu_start_sector)))
        return 1;
    while (s < c->last_vobu_start_sector) {
        if (grow((void **)&list, &alloc, nb, sizeof(*list)) < 0)
            goto end;
        list[nb++] = s;
        if (!ff_dvd_vobu_next(T->d, f, s, &s))
            goto end;
    }
    if (nb < CHECK_SAMPLE || T->o->cell_check == DVD_CELL_CHECK_ALL) {
        for (int k = 0; k < nb; k++)
            if (ff_dvd_vobu_bad(T->d, f, list[k]))
                goto end;
    } else if (T->o->cell_check == DVD_CELL_CHECK_SPREAD) {
        for (int k = 0; k < CHECK_SAMPLE; k++)
            if (ff_dvd_vobu_bad(T->d, f, list[(int64_t)k * nb / CHECK_SAMPLE]))
                goto end;
    } else {
        uint32_t pick[CHECK_SAMPLE];
        int left = nb;
        for (int k = 0; k < CHECK_SAMPLE; k++) {
            int at = random64(T) % left;
            pick[k] = list[at];
            memmove(list + at, list + at + 1, (left - at - 1) * sizeof(*list));
            left--;
        }
        qsort(pick, CHECK_SAMPLE, sizeof(*pick), cmp_u32);
        for (int k = 0; k < CHECK_SAMPLE; k++)
            if (ff_dvd_vobu_bad(T->d, f, pick[k]))
                goto end;
    }
    if (vobu_ptm(T, f, p, i, c->first_sector, &s1, &e1, 1) &&
        vobu_ptm(T, f, p, i, c->last_vobu_start_sector, &s2, &e2, 1) && s1 <= e1)
        failed = !(e1 <= e2 && s2 <= e2);
end:
    av_log(T->d->log, AV_LOG_DEBUG, "DVD: cell check, VTS_%02d program chain %d cell %d (%d VOBUs, %s): %s\n",
           f->index, p->number + 1, i + 1, nb, nb < CHECK_SAMPLE || T->o->cell_check == DVD_CELL_CHECK_ALL ? "all read" :
           T->o->cell_check == DVD_CELL_CHECK_SPREAD ? "50 evenly spread" : "50 at random", failed ? "failed" : "passed");
    av_free(list);
    return failed;
}

/* A long single cell made of tiny VOBUs of one size: filler. */
static int single_cell_fake(Titles *T, DVDIfo *f, const DVDCell *c)
{
    uint32_t first = c->first_sector, n, s, size;
    unsigned count = 0, steps = 0;

    if (bcd_seconds(c->playback_time) <= 1799 || first == c->last_vobu_start_sector)
        return 0;
    if (!ff_dvd_vobu_next(T->d, f, first, &n))
        return 1;
    size = n - first;
    if (size >= 12)
        return 0;
    s = n;
    if (s == c->last_vobu_start_sector || s > c->last_sector)
        return 0;
    do {
        if (!ff_dvd_vobu_next(T->d, f, s, &s))
            return 1;
        count++;
        steps += 9;
    } while (s != c->last_vobu_start_sector && s <= c->last_sector);
    if (count < 1000)
        return 0;
    s = first;
    for (unsigned k = steps / 10; k; k--) {
        if (!ff_dvd_vobu_next(T->d, f, s, &n))
            break;
        if (n - s != size)
            return 0;
        s = n;
    }
    return 1;
}

/* A cell to leave out: broken VOBUs known, no playback time, outside the
 * title VOBs, a short cell with a cell command, a single long filler cell;
 * the full test (quick 0) also runs the cell check and measures the cell. */
static int cell_rejected(Titles *T, DVDIfo *f, DVDPgc *p, int i, int quick, int prev)
{
    DVDCell *c = &p->cells[i];
    uint32_t t;
    uint64_t dur;

    if (c->first_sector > c->last_vobu_start_sector || cell_has_bad_vobu(T, f, c))
        return 1;
    if (!(c->playback_time & 0xffffff3f))
        return 1;
    if (c->last_vobu_start_sector >= f->title_vob_blocks || c->last_sector >= f->title_vob_blocks ||
        c->first_sector >= f->title_vob_blocks)
        return 1;
    t = c->playback_time & 0xffffff00;
    if (t <= 0x100 && !quick) {
        if (c->first_sector == c->last_vobu_start_sector) {
            if (prev >= 0 && prev + 1 == i && !CMD_NR(c) && prev + 2 != p->nr_of_cells &&
                !cell_check_failed(T, f, p, i))
                return 0;
            return 1;
        }
        if (CMD_NR(c))
            return 1;
    } else if (t <= 0x500 && CMD_NR(c)) {
        return 1;
    }
    if (p->nr_of_cells == 1 && single_cell_fake(T, f, c))
        return 1;
    if ((bcd_seconds(c->playback_time) < 1801 && t > 0x500) || quick || !cell_check_failed(T, f, p, i)) {
        if (quick)
            return 0;
        return !cell_duration(T, f, p, i, &dur, 1);
    }
    return 1;
}

/* The cells from..to (VOBU starts of title VOBs) walk interleaved unit by
 * interleaved unit, each unit's first VOBU starting it. */
static int ilvu_walk(Titles *T, DVDIfo *f, uint32_t from, uint32_t to)
{
    if (to < from)
        return 0;
    while (from != to) {
        const DVDVobuRec *r;
        uint32_t u, next, len;
        if (to < from || !ff_dvd_vobu_start_check(T->d, f, from))
            return 0;
        if (!(r = ff_dvd_vobu_find(T->d, f, from, NULL))) {
            DVDNav n;
            if (!ff_dvd_nav_read(T->d, f, from, &n) || !ff_dvd_vobu_register(T->d, f, &n, from, 0, &next, &len) ||
                (~n.category & 0x6000) || n.ilvu_ea > 0x7ffffffe)
                return 0;
            u = n.ilvu_ea;
        } else {
            if (~r->next & 0x60000000)
                return 0;
            u = r->ilvu_ea;
        }
        from = from + 1 + u;
        if (ff_dvd_map_trusted(T->d, f)) {
            int exact, at = ff_dvd_map_find(f, from, &exact);
            if (exact && at) {
                uint32_t q = f->vobu_map[at - 1];
                DVDNav n;
                if (!ff_dvd_vobu_find(T->d, f, q, NULL) && ff_dvd_nav_read(T->d, f, q, &n))
                    ff_dvd_vobu_register(T->d, f, &n, q, 1, &next, &len);
            }
        }
    }
    return 1;
}

/* ---- the program chain check ---- */

static int pair_ok(const DVDCell *p, const DVDCell *q)
{
    const DVDCell *lo, *hi;
    uint32_t end;

    if (p->first_sector == q->first_sector && p->last_vobu_start_sector == q->last_vobu_start_sector &&
        p->last_sector == q->last_sector)
        return 1;
    if ((p->first_sector < q->first_sector && q->first_sector < p->last_vobu_start_sector) ||
        (q->first_sector < p->first_sector && p->first_sector < q->last_vobu_start_sector))
        return 1;
    if (p->last_sector == p->first_ilvu_end_sector && q->last_sector == q->first_ilvu_end_sector)
        return 1;
    lo = p->first_sector <= q->first_sector ? p : q;
    hi = lo == p ? q : p;
    end = FFMAX(lo->last_sector, lo->first_ilvu_end_sector);
    if (p->last_sector < p->first_ilvu_end_sector) {
        if (q->last_sector < q->first_ilvu_end_sector && end + 1 <= hi->first_sector)
            return 1;
        return 0;
    }
    return !p->first_ilvu_end_sector && !q->first_ilvu_end_sector && end + 1 <= hi->first_sector;
}

/* an interleaved angle block starting at cell x whose cells' places agree */
static int angle_block_ok(const DVDPgc *p, int x)
{
    int n = 1, k = x;

    for (;;) {
        uint8_t g;
        if (++k >= p->nr_of_cells || (FLAGS(&p->cells[k]) & 0x35) != 0x14)
            return 0;
        g = BLOCK_MODE(FLAGS(&p->cells[k]));
        n++;
        if (g == 3)
            break;
        if (g != 2)
            return 0;
    }
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (!pair_ok(&p->cells[x + a], &p->cells[x + b]))
                return 0;
    return 1;
}

/* The program chain is broken or a decoy: the reason (0-9), -1 when it
 * passes. */
static int pgc_rejected(Titles *T, DVDIfo *f, DVDPgc *p)
{
    int n = p->nr_of_cells, ov;

    if (!n)
        return 0;
    ov = ff_dvd_title_vobs_overlap(T->d, f->index);
    if (ov && n >= 2) {
        int all = 1, n2 = n, mono = 1;
        for (int k = 1; k < n && all; k++)
            all = FLAGS(&p->cells[k]) & 2;
        if (all && n >= 12)
            return 1;
        if (n > 3 && (cell_rejected(T, f, p, n - 1, 0, n - 2) ||
                      ((p->cells[n - 1].playback_time & 0xffffff00) < 0x301 &&
                       p->cells[n - 1].vob_id_nr != p->cells[n - 2].vob_id_nr && p->cells[n - 1].cell_nr == 1)))
            n2 = n - 1;
        for (int k = 1; k < n2 && mono; k++)
            mono = p->cells[k - 1].first_sector <= p->cells[k].first_sector;
        if (all && !mono && n >= 7)
            return 2;
    }
    if (n >= 4) {
        uint32_t secs = 0, sectors = 0;
        for (int k = 0; k < n; k++) {
            if (!cell_rejected(T, f, p, k, 1, k - 1))
                secs += bcd_seconds(p->cells[k].playback_time);
            sectors += p->cells[k].last_sector - p->cells[k].first_sector + 1;
        }
        if (!secs)
            return 3;
        if (sectors / secs < 50)
            return 4;
    }
    if (!p->nr_of_programs)
        return -1;
    if (!p->program_map[0] || p->program_map[0] > n)
        return 5;
    for (int k = 1; k < p->nr_of_programs; k++) {
        if (!p->program_map[k] || p->program_map[k] > n)
            return 5;
        if (p->program_map[k] < p->program_map[k - 1])
            return 6;
    }
    if (p->nr_of_programs < 3)
        return -1;
    /* programs 2.. : seamless cells without cell commands follow each other on the disc */
    for (int pg = 1; pg < p->nr_of_programs; pg++) {
        for (int a = p->program_map[pg] - 1, b = a + 1; b < n; a++, b++) {
            uint8_t fa = FLAGS(&p->cells[a]), fb = FLAGS(&p->cells[b]), fx;
            int x = a, y = b;
            if (!(fb & 8) || CMD_NR(&p->cells[a]) || CMD_NR(&p->cells[b]))
                break;
            fx = fa;
            if (!((fa | fb) & 1)) {
                int join;
                if (!(fa & 4))
                    join = !(fa & 0x30) && fa < 0x40 &&
                           ((!(fb & 4) && !(fb & 0x30) && fb < 0x40) ||
                            ((fb & 4) && (fb & 0x30) == 0x10 && (fb & 0xc0) == 0x40));
                else
                    join = (fa & 0x30) == 0x10 && fa >= 0xc0 && !(fb & 4) && !(fb & 0x30) && fb <= 0x3f;
                if (join) {
                    x = angle_block_extreme(T, p, a, 1);
                    y = angle_block_extreme(T, p, b, 0);
                    if (p->cells[x].last_sector + 1 != p->cells[y].first_sector)
                        return 7;
                    fx = FLAGS(&p->cells[x]);
                }
            }
            if (fx < 0x10 && ((fx & 4) ? (FLAGS(&p->cells[y]) & 0xf4) == 0 : (FLAGS(&p->cells[y]) & 0xf4) == 4) &&
                p->cells[x].last_sector + 1 != p->cells[y].first_sector &&
                !ilvu_walk(T, f, p->cells[x].last_sector + 1, p->cells[y].first_sector))
                return 8;
            if (ov && (FLAGS(&p->cells[x]) & 0xf5) == 0x54 && !angle_block_ok(p, x))
                return 9;
        }
    }
    return -1;
}

/* ---- a title's cell list ---- */

static int push_cell(DVDTitle *t, int c)
{
    int ret;
    if ((ret = grow((void **)&t->cells, &t->alloc_cells, t->nb_cells, sizeof(*t->cells))) < 0)
        return ret;
    t->cells[t->nb_cells++] = c;
    return 0;
}

/* cells first..last of the title's program chain, one per angle block:
 * 1 done, 0 a broken angle block, < 0 an error */
static int set_cells(Titles *T, DVDTitle *t, int first, int last)
{
    const DVDPgc *p = t->pgc;
    int c = first, ret;

    if (first)
        av_log(T->d->log, AV_LOG_VERBOSE, "DVD: title %s: cells 1-%d left out at the start\n", t->name, first);
    if (last != p->nr_of_cells - 1)
        av_log(T->d->log, AV_LOG_VERBOSE, "DVD: title %s: cells %d-%d left out at the end\n", t->name, last + 2,
               p->nr_of_cells);
    t->nb_cells = 0;
    for (;;) {
        int e;
        if (c > last)
            return 1;
        if ((e = angle_cell(T, p, c, t->angle, t->nr_of_angles)) < 0)
            return 0;
        if ((ret = push_cell(t, e)) < 0)
            return ret;
        if ((c = next_cell(T, p, e)) < 0)
            return 1;
    }
}

typedef struct Run { uint8_t first, last; } Run;

/* a run kept at an end: one of its cells accepted and 5 seconds (stopping
 * at both); -1 when its cells are not the program chain's */
static int run_good(Titles *T, DVDTitle *t, Run r)
{
    DVDPgc *p = t->pgc;
    int all_rejected = 1;
    uint32_t sum = 0;

    if (r.first - 1 >= (unsigned)p->nr_of_cells || r.last - 1 >= (unsigned)p->nr_of_cells)
        return -1;
    for (int x = r.first - 1;; x++) {
        if (!cell_rejected(T, t->vts, p, x, 1, -1))
            all_rejected = 0;
        sum += bcd_seconds(p->cells[x].playback_time);
        if (x == r.last - 1 || (!all_rejected && sum >= 5))
            break;
    }
    return !all_rejected && sum > 4;
}

static uint32_t run_seconds(const DVDPgc *p, Run r)
{
    uint32_t sum = 0;
    for (int x = r.first - 1; x <= r.last - 1; x++)
        sum += bcd_seconds(p->cells[x].playback_time);
    return sum;
}

/* The cells the navigation played in the title's program chain: one run of
 * consecutive cells. 1 done, 0 not possible, < 0 an error. */
static int cell_walk(Titles *T, DVDTitle *t)
{
    const DVDScanResult *e = t->scan;
    const DVDPgc *p = t->pgc;
    Run *runs;
    int nb = 0, ret;

    if (!e)
        return 0;
    if (!e->nb_cells) {
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: the navigation played no cell\n", t->name);
        return 0;
    }
    if (((p->number + 1) & 0xffff) != e->pgc)
        return 0;
    if (!(runs = av_malloc_array(e->nb_cells, sizeof(*runs))))
        return AVERROR(ENOMEM);
    for (int k = 0; k < e->nb_cells; k++) {
        uint8_t c = e->cells[k];
        if (nb && runs[nb - 1].last + 1 == c)
            runs[nb - 1].last = c;
        else
            runs[nb++] = (Run){ c, c };
    }
    /* the start, then the end: runs without an accepted cell or 5 seconds */
    while (nb > 1) {
        int g = run_good(T, t, runs[0]);
        if (g > 0)
            break;
        memmove(runs, runs + 1, --nb * sizeof(*runs));
    }
    while (nb > 1) {
        int g = run_good(T, t, runs[nb - 1]);
        if (g > 0)
            break;
        nb--;
    }
    /* the longer end */
    while (nb > 1) {
        if (run_seconds(p, runs[0]) < run_seconds(p, runs[nb - 1]))
            memmove(runs, runs + 1, --nb * sizeof(*runs));
        else
            nb--;
    }
    if (runs[0].first - 1 < (unsigned)p->nr_of_cells && runs[0].last - 1 < (unsigned)p->nr_of_cells)
        ret = set_cells(T, t, runs[0].first - 1, runs[0].last - 1);
    else
        ret = 0;
    av_free(runs);
    return ret;
}

/* ---- the cell list without the navigation (trim) ---- */

/* The cell command cmd_nr (1-based) of program chain p may jump away: a
 * Goto / Link / Jump that is not a plain "go on" (DVD-Video Book command
 * groups: link, jump and set-with-link instructions). */
static int cmd_jumps(const DVDPgc *p, unsigned cmd_nr)
{
    const uint8_t *c;
    unsigned u, u7, u5, op;
    int b;

    if (!cmd_nr || cmd_nr > p->nr_of_cell || !p->commands)
        return 0;
    c  = p->commands + 8 * (p->nr_of_pre + p->nr_of_post + cmd_nr - 1);
    u  = c[0] << 8 | c[1];
    u7 = u & 0xff0f;
    u5 = u & 0xf0ff;
    if (u7 == 0x2001 && !c[7])
        return 0;
    if ((u5 == 0x7001 || u5 == 0x6001) && !c[7])
        return 0;
    if (u7 == 0x2001 || u7 == 0x2005 || u7 == 0x2006 || u7 == 0x2007 || u7 == 0x3008) {
        op = c[1] >> 4 & 7;
        if (!(c[1] & 0x80))
            b = !((op == 3 || op == 5 || op == 7) && c[5] == c[3]);
        else
            b = op != 1 || (c[4] << 8 | c[5]) != 0;
    } else {
        b = 0;
    }
    if (u5 == 0x6001 || (u5 >= 0x6004 && u5 <= 0x6007) || u5 == 0x7001 || (u5 >= 0x7004 && u5 <= 0x7007))
        return 1;
    return b;
}

/* the run of cells around cell i on one timeline between cell commands
 * that jump away: *first, *last */
static void segment_around(const DVDPgc *p, int i, int *first, int *last)
{
    int k = i;

    while (k && (FLAGS(&p->cells[k]) & 0x32) != 2 && !cmd_jumps(p, CMD_NR(&p->cells[k - 1])))
        k--;
    *first = k;
    k = i;
    while (!cmd_jumps(p, CMD_NR(&p->cells[k])) && k + 1 < p->nr_of_cells &&
           (FLAGS(&p->cells[k + 1]) & 0x32) != 2)
        k++;
    *last = k;
}

static uint32_t cells_seconds(const DVDPgc *p, int first, int last)
{
    uint32_t sum = 0;
    for (int k = first; k <= last; k++)
        sum += bcd_seconds(p->cells[k].playback_time);
    return sum;
}

/* From cell b toward cell a, the first cell that is not filler; a when none
 * (simple: the first one passing the full test). */
static int pick_end(Titles *T, DVDTitle *t, int a, int b, int simple)
{
    DVDPgc *p = t->pgc;
    int dir = a < b ? 1 : -1, i = FFABS(b - a);

    if (a == b)
        return a;
    if (simple) {
        for (int j = i; j >= 1; j--)
            if (!cell_rejected(T, t->vts, p, a + dir * j, 0, -1))
                return a + dir * j;
        return a;
    }
    while (i) {
        int c = a + dir * i, step = 1;
        if (bcd_seconds(p->cells[c].playback_time) > 2 && !cell_rejected(T, t->vts, p, c, 0, -1)) {
            if ((p->cells[c].playback_time & 0xffffff00) > 0x3000 || i < 4 ||
                bcd_seconds(p->cells[a + dir * (i - 1)].playback_time) > 2 ||
                bcd_seconds(p->cells[a + dir * (i - 2)].playback_time) > 2)
                return c;
            step = 3;                   /* a short cell after two of at most 2 s: skipped with them */
        }
        i -= step;
    }
    return a;
}

/* The longest run of cells first..last lying in disc order; 1 when it is
 * not mostly filler against the program chain's declared length (else
 * logged unless quiet). */
static int best_run(Titles *T, DVDTitle *t, int first, int last, int *rf, int *rl, int quiet)
{
    const DVDPgc *p = t->pgc;
    uint32_t best = 0, declared, run, pct;
    int start = first, cur = first;

    for (;;) {
        int end = cur, next = -1;
        if (cur != last && cur + 1 >= p->nr_of_cells) {
            av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: cell %d lies after cell %d\n", t->name, first + 1,
                   last + 1);
        } else if (cur != last) {
            next = cur + 1;
            if (p->cells[cur].first_sector <= p->cells[next].first_sector) {
                cur = next;
                continue;
            }
        }
        if ((run = cells_seconds(p, start, end)) > best) {
            *rf  = start;
            *rl  = end;
            best = run;
        }
        if (next < 0)
            break;
        start = cur = next;
    }
    if (!best) {
        if (!quiet)
            av_log(T->d->log, AV_LOG_INFO, "DVD: title %s: fake cells make up 100%% of it: its cells are left "
                   "out\n", t->name);
        return 0;
    }
    declared = bcd_seconds(p->playback_time);
    run = cells_seconds(p, *rf, *rl);
    pct = declared && declared > run ? (uint32_t)((uint64_t)(declared - run) * 100 / declared) : 0;
    if (!declared)
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: its program chain declares no length [untested on real "
               "discs]\n", t->name);
    if (pct < 26)
        return 1;
    if (!quiet)
        av_log(T->d->log, AV_LOG_INFO, "DVD: title %s: fake cells make up %"PRIu32"%% of it: its cells are left "
               "out\n", t->name, pct);
    return 0;
}

/* Trim, first way: a program chain of 3-5 cells whose first and last are
 * filler around one VOB (keep the inner cells), or of 5+ cells with filler
 * at both ends and one run of 10 seconds or more (keep that run, its
 * filler ends dropped). 1 done, 0 no such shape, < 0 an error. */
static int trim_edges(Titles *T, DVDTitle *t)
{
    DVDPgc *p = t->pgc;
    DVDIfo *f = t->vts;
    int n = p->nr_of_cells, F = 0, L = n - 1, gs = -1, ge = -1, ret;

    if (t->nb_chapters > 4)
        return 0;
    if (n >= 3 && n <= 5 && cell_rejected(T, f, p, F, 0, -1) && cell_rejected(T, f, p, L, 0, -1)) {
        int S = 1, E = n - 2, prev = -1, ok;
        if (p->cells[F].vob_id_nr != p->cells[S].vob_id_nr && p->cells[L].vob_id_nr != p->cells[E].vob_id_nr) {
            int same = p->cells[F].vob_id_nr == p->cells[L].vob_id_nr && p->cells[F].cell_nr == p->cells[L].cell_nr;
            ok = p->cells[F].cell_nr == 1 && p->cells[L].cell_nr == 1 ? 1 : same;
            for (int x = S; ok && x != E; x++) {
                if (p->cells[x].vob_id_nr != p->cells[E].vob_id_nr || cell_rejected(T, f, p, x, 0, prev) ||
                    CMD_NR(&p->cells[x])) {
                    ok = 0;
                    break;
                }
                prev = x;
            }
            if (!cell_rejected(T, f, p, E, 0, prev) && ok && (ret = set_cells(T, t, S, E)) != 0)
                return ret;
        }
    }
    if (n < 5 || !cell_rejected(T, f, p, F, 0, -1) || !cell_rejected(T, f, p, L, 0, -1))
        return 0;
    for (int c = F; c <= L;) {
        int s, e;
        segment_around(p, c, &s, &e);
        if (cells_seconds(p, s, e) > 9) {
            if (gs >= 0)
                return 0;               /* two long runs */
            gs = s;
            ge = e;
        }
        if (e == L || e + 1 >= n)
            break;
        c = e + 1;
    }
    if (gs < 0)
        return 0;
    gs = pick_end(T, t, ge, gs, 1);
    return set_cells(T, t, gs, pick_end(T, t, gs, ge, 1));
}

/* the entry cell (index) of the first program starting after cell i (next
 * 1) or of the program holding it (next 0); -1 none */
static int program_entry(const DVDPgc *p, int i, int next)
{
    int k = 1, np = p->nr_of_programs, at;

    if (!np)
        return -1;
    for (; k <= np; k++)
        if (p->program_map[k - 1] > i + 1)
            break;
    at = next ? k : k - 1;               /* the program number, from 1 */
    if (!at || at > np || !p->program_map[at - 1] || p->program_map[at - 1] > p->nr_of_cells)
        return -1;
    return p->program_map[at - 1] - 1;
}

/* Trim, second way: with few chapters the cells between the first and last
 * that pass the full test; with five or more, the run around the middle
 * chapter between cell commands that jump away, widened over long runs
 * before it and trimmed of filler at both ends. 1 done (no cells when it
 * finds none), 0 failed, < 0 an error. */
static int trim_runs(Titles *T, DVDTitle *t)
{
    DVDPgc *p = t->pgc;
    DVDIfo *f = t->vts;
    int n = p->nr_of_cells, rf, rl;

    if (t->nb_chapters < 5) {
        int fi = 0, li = n - 1;
        if (!n)
            goto clear;
        while (cell_rejected(T, f, p, fi, 0, -1) || cell_check_failed(T, f, p, fi))
            if (++fi > n - 1)
                goto clear;
        while (cell_rejected(T, f, p, li, 0, -1) || cell_check_failed(T, f, p, li)) {
            if (li == fi)
                goto clear;
            li--;
        }
        if (best_run(T, t, fi, li, &rf, &rl, 1))
            return set_cells(T, t, rf, rl);
    } else {
        int first = t->chapters[0].cell, mid = t->chapters[t->nb_chapters / 2].cell, S, E, A, B, C, D, x, y;
        if (first < 0 || mid < 0) {
            av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: a chapter without a cell\n", t->name);
            return 0;
        }
        for (S = mid; S != first; S--) {
            if (S - 1 < 0) {
                av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: no cell before cell %d\n", t->name, S + 1);
                return 0;
            }
            if (cmd_jumps(p, CMD_NR(&p->cells[S - 1])))
                break;
        }
        for (E = mid; E + 1 < n;)
            if (cmd_jumps(p, CMD_NR(&p->cells[++E])))
                break;
        if (!best_run(T, t, S, E, &rf, &rl, 0))
            goto clear;
        if ((A = program_entry(p, S, 1)) < 0) {
            av_log(T->d->log, AV_LOG_WARNING, "DVD: title %s: the middle chapter's cells lie in the last program: "
                   "they cannot be trimmed [untested on real discs]\n", t->name);
            return 0;
        }
        /* back from A over runs of 30 seconds or more, not past S */
        B = S;
        for (int at = A; at != S;) {
            int s, e, s2, e2;
            segment_around(p, at, &s, &e);
            B = s;
            if (s == S || s - 1 < 0)
                break;
            segment_around(p, s - 1, &s2, &e2);
            if (cells_seconds(p, s2, e2) < 30)
                break;
            B = S;
            if (s2 == S)
                break;
            at = s2;
        }
        /* back from A over cells passing the full check, not past B */
        for (C = A; C != B && !cell_check_failed(T, f, p, C - 1); C--)
            ;
        /* back from E over short new-timeline cells, not past C */
        for (D = E; D != C && (FLAGS(&p->cells[D]) & 0xf2) == 2 && bcd_seconds(p->cells[D].playback_time) < 2;) {
            if (!D) {
                av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: no cell before cell 1\n", t->name);
                return 0;
            }
            D--;
        }
        x = pick_end(T, t, A, C, 0);
        y = program_entry(p, D, 0);
        if (y < 0) {
            av_log(T->d->log, AV_LOG_WARNING, "DVD: title %s: cell %d lies in no program [untested on real "
                   "discs]\n", t->name, D + 1);
            return 0;
        }
        y = pick_end(T, t, y, D, 0);
        if (best_run(T, t, x, y, &rf, &rl, 0))
            return set_cells(T, t, rf, rl);
    }
clear:
    t->nb_cells = 0;
    return 1;
}

/* The cell list without the navigation: the program chain's cells, decoys
 * at the start and end trimmed. */
static int cell_trim(Titles *T, DVDTitle *t)
{
    int ret = trim_edges(T, t);
    return ret ? ret : trim_runs(T, t);
}

/* ---- segments ---- */

static int add_extent(DVDSegment *g, uint32_t sector, uint32_t count)
{
    int ret;
    uint32_t logical = g->nb_extents ? g->extents[g->nb_extents - 1].logical + g->extents[g->nb_extents - 1].count : 0;
    if ((ret = grow((void **)&g->extents, &g->alloc_extents, g->nb_extents, sizeof(*g->extents))) < 0)
        return ret;
    g->extents[g->nb_extents++] = (DVDExtent){ logical, sector, count };
    return 0;
}

static int add_segment(DVDTitle *t)
{
    int ret;
    if ((ret = grow((void **)&t->segments, &t->alloc_segments, t->nb_segments, sizeof(*t->segments))) < 0)
        return ret;
    memset(&t->segments[t->nb_segments++], 0, sizeof(*t->segments));
    return 0;
}

static int add_cell_to_runs(DVDSegment *g, int cell)
{
    unsigned c = cell + 1;
    int ret;

    if (g->nb_runs) {
        uint16_t *r = g->runs[g->nb_runs - 1];
        if (c >= r[0] && c <= r[1])
            return 0;
        if (r[1] + 1 == c) {
            r[1]++;
            return 0;
        }
    }
    if ((ret = grow((void **)&g->runs, &g->alloc_runs, g->nb_runs, sizeof(*g->runs))) < 0)
        return ret;
    g->runs[g->nb_runs][0] = g->runs[g->nb_runs][1] = c;
    g->nb_runs++;
    return 0;
}

/* the VOBU of the segment's last sector (the start of its last VOBU) */
static uint32_t segment_last_vobu(Titles *T, DVDTitle *t, const DVDSegment *g)
{
    const DVDExtent *e = &g->extents[g->nb_extents - 1];
    uint32_t x = e->sector + e->count - 1;
    DVDCell *c = &t->pgc->cells[g->last_cell];
    DVDIfo *f = t->vts;

    if (ff_dvd_map_trusted(T->d, f)) {
        int exact, at = ff_dvd_map_find(f, x, &exact);
        if (at < f->nb_vobu_map && at)
            return f->vobu_map[at - 1];
    }
    if (c->first_sector <= x && x <= c->last_sector) {
        uint32_t s = x > c->last_vobu_start_sector ? c->last_vobu_start_sector : c->first_sector, n, l;
        for (;;) {
            if (!next_len(T, f, c, s, &n, &l))
                break;
            if (x < n)
                return s;
            s = n;
        }
    }
    av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: the last VOBU of a segment is not found (sector %"PRIu32")\n",
           t->name, x);
    return c->last_vobu_start_sector;
}

/* the presentation start / end of the VOBU at s (recorded, else read) */
static int vobu_times(Titles *T, DVDIfo *f, uint32_t s, uint32_t *sp, uint32_t *ep)
{
    const DVDVobuRec *r = ff_dvd_vobu_find(T->d, f, s, NULL);
    DVDNav n;
    uint32_t next, len;

    if (r) {
        if (r->next == DVD_VOBU_NO_NAV)
            return 0;
        *sp = r->vobu_s_ptm;
        *ep = r->vobu_e_ptm;
        return 1;
    }
    if (!ff_dvd_nav_read(T->d, f, s, &n))
        return 0;
    *sp = n.vobu_s_ptm;
    *ep = n.vobu_e_ptm;
    ff_dvd_vobu_register(T->d, f, &n, s, 1, &next, &len);
    return 1;
}

/* From the segment's first VOBU's start to the end of the VOBU at s (90 kHz). */
static int segment_time(Titles *T, DVDTitle *t, const DVDSegment *g, uint32_t s, int64_t *time)
{
    uint32_t s1, e1, s2, e2;

    *time = 0;
    if (!g->nb_extents || g->extents[0].sector == s)
        return 1;
    if (!vobu_times(T, t->vts, g->extents[0].sector, &s1, &e1)) {
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: no NAV pack at sector %"PRIu32"\n", t->name,
               g->extents[0].sector);
        return 0;
    }
    if (!vobu_times(T, t->vts, s, &s2, &e2)) {
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: no NAV pack at sector %"PRIu32"\n", t->name, s);
        return 0;
    }
    if (e1 < s1 || s2 < e1 || e2 < s2)
        return 0;
    *time = e2 - s1;
    return 1;
}

/* a VOBU needs reading one by one (angle blocks, interleaved cells, a
 * cell's last VOBU) */
static int vobu_needs_scan(Titles *T, DVDIfo *f, DVDCell *c, uint32_t s)
{
    uint32_t n;

    if (s < c->last_vobu_start_sector && !(FLAGS(c) & 0xf4)) {
        if (FLAGS(c) & 1)
            return !(ff_dvd_map_trusted(T->d, f) && seamless_angle_ok(T, f, c) && ff_dvd_vobu_next(T->d, f, s, &n));
        ff_dvd_vobu_next(T->d, f, s, &n);
        return 0;
    }
    return 1;
}

/* Where the navigation is invalid inside a cell (a VOBU says the cell ends
 * early): the place, for the warning. < 0 when the segments' times are
 * broken. */
static int nav_invalid(Titles *T, DVDTitle *t)
{
    uint64_t pos = 0;
    uint32_t secs;

    for (int k = 0; k < t->nb_segments; k++) {
        const DVDSegment *g = &t->segments[k];
        uint32_t last, s1, e1, s2, e2;
        if (!g->nb_extents || g->extents[0].sector == (last = segment_last_vobu(T, t, g)))
            continue;
        if (!vobu_times(T, t->vts, g->extents[0].sector, &s1, &e1) || !vobu_times(T, t->vts, last, &s2, &e2) ||
            s1 > e1 || e1 > s2 || s2 > e2) {
            av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: segment %d: presentation times out of order\n",
                   t->name, k + 1);
            return -1;
        }
        pos += e2 - s1;
    }
    secs = pos / 90000;
    if (++t->nav_invalid <= NAV_INVALID_TOLD)
        av_log(T->d->log, AV_LOG_WARNING, "DVD: title %s: the navigation information is invalid around "
               "%"PRIu32":%02"PRIu32":%02"PRIu32" (check the output for audio / video sync)\n",
               t->name, secs / 3600, secs / 60 % 60, secs % 60);
    else
        av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: the navigation information is invalid around "
               "%"PRIu32":%02"PRIu32":%02"PRIu32"\n", t->name, secs / 3600, secs / 60 % 60, secs % 60);
    return 0;
}

/* The cells' VOBUs as extents, in segments. 1 done, 0 failed, < 0 error. */
static int build_segments(Titles *T, DVDTitle *t)
{
    DVDIfo *f = t->vts;
    DVDPgc *p = t->pgc;
    unsigned cells_to_scan = 0, vobus_to_scan = 0;
    int ret;

    for (int i = 0; i < t->nb_cells; i++) {
        DVDCell *c = &p->cells[t->cells[i]];
        uint32_t s = c->first_sector, n;
        if (s < c->last_vobu_start_sector && !(FLAGS(c) & 0xf4)) {
            if (!(FLAGS(c) & 1)) {
                ff_dvd_vobu_next(T->d, f, s, &n);
                continue;
            }
            if (ff_dvd_map_trusted(T->d, f) && seamless_angle_ok(T, f, c) && ff_dvd_vobu_next(T->d, f, s, &n))
                continue;
        }
        if (s >= f->title_vob_blocks)
            goto outside;
        cells_to_scan++;
        for (;;) {
            vobus_to_scan += vobu_needs_scan(T, f, c, s);
            if (s == c->last_vobu_start_sector || s > c->last_sector)
                break;
            if (!ff_dvd_vobu_next(T->d, f, s, &n)) {
                ff_dvd_vobu_next(T->d, f, s, &n);
                av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: no VOBU after sector %"PRIu32"\n", t->name, s);
                return 0;
            }
            s = n;
            if (s >= f->title_vob_blocks)
                goto outside;
        }
    }
    if (vobus_to_scan > COMPLEX_VOBUS)
        av_log(T->d->log, AV_LOG_INFO, "DVD: title %s: %u cells and %u VOBUs are read one by one (a complex "
               "multiplex); this takes a while\n", t->name, cells_to_scan, vobus_to_scan);

    for (int i = 0; i < t->nb_cells; i++) {
        int ci = t->cells[i];
        DVDCell *c = &p->cells[ci];
        uint8_t fl = FLAGS(c);
        int ns = !!(fl & 2), new_seg = ns, il = 0;
        uint32_t s = c->first_sector, next = 0;

        if (i) {
            int pi = t->cells[i - 1];
            if (ci <= pi) {
                new_seg = 1;
            } else {
                for (int k = pi + 1; k < ci; k++)
                    if (!BLOCK_TYPE(FLAGS(&p->cells[k])) && (FLAGS(&p->cells[k]) & 2))
                        ns = 1;
                new_seg = ns;
            }
        }
        if (fl & 4) {
            if ((BLOCK_TYPE(fl) == 0 && BLOCK_MODE(fl) == 0) || (BLOCK_TYPE(fl) == 1 && BLOCK_MODE(fl) >= 1))
                il = ilvu_walk(T, f, c->first_sector, c->last_sector + 1);
            else
                av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: interleaved cell %d outside an angle block\n",
                       t->name, ci + 1);
        }
        while (next != DVD_VOBU_END && s < c->last_sector) {
            uint32_t len = 0;
            int done = 0;
            if (s >= f->title_vob_blocks)
                goto outside;
            vobu_needs_scan(T, f, c, s);
            if (il) {
                const DVDVobuRec *e = ff_dvd_vobu_find(T->d, f, s, NULL);
                if (e && ff_dvd_map_trusted(T->d, f)) {
                    uint32_t len0 = e->ilvu_ea + 1, target = s + len0, n2, l2;
                    int exact, at = ff_dvd_map_find(f, target, &exact);
                    if (exact && at) {
                        uint32_t q = f->vobu_map[at - 1];
                        if (nav_next_len(T, f, c, q, &n2, &l2) && q + l2 == target) {
                            next = n2;
                            len  = len0;
                            done = 1;
                        }
                    }
                }
            }
            if (!done && !next_len(T, f, c, s, &next, &len))
                return 0;
            if (!t->nb_segments || new_seg) {
                if ((ret = add_segment(t)) < 0)
                    return ret;
                if ((ret = add_extent(&t->segments[t->nb_segments - 1], s, len)) < 0)
                    return ret;
            } else {
                DVDSegment *g = &t->segments[t->nb_segments - 1];
                DVDExtent *e = &g->extents[g->nb_extents - 1];
                if (e->sector + e->count == s)
                    e->count += len;
                else if ((ret = add_extent(g, s, len)) < 0)
                    return ret;
            }
            t->segments[t->nb_segments - 1].last_cell = ci;
            if ((ret = add_cell_to_runs(&t->segments[t->nb_segments - 1], ci)) < 0)
                return ret;
            if (next == DVD_VOBU_END) {
                next = s + len;
                if (next < c->last_sector && nav_invalid(T, t) < 0)
                    return 0;
            }
            new_seg = 0;
            s = next;
        }
    }
    for (int k = 0; k < t->nb_segments; k++) {
        DVDSegment *g = &t->segments[k];
        AVBPrint bp;
        if (g->nb_extents)
            g->size = (int64_t)(g->extents[g->nb_extents - 1].logical + g->extents[g->nb_extents - 1].count) *
                      DVD_SECTOR;
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
    t->built = 1;
    return 1;
outside:
    av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: a VOBU outside the title VOBs\n", t->name);
    return 0;
}

/* The chapters' places in the segments; ones out of order dropped. */
static void place_chapters(Titles *T, DVDTitle *t)
{
    DVDChapter *prev = NULL;
    int n;

    if (t->angle)
        for (int k = 0; k < t->nb_chapters; k++)
            if (t->chapters[k].cell >= 0)
                t->chapters[k].cell = angle_cell(T, t->pgc, t->chapters[k].cell, t->angle, t->nr_of_angles);
    for (int k = 0; k < t->nb_chapters; k++) {
        DVDChapter *ch = &t->chapters[k];
        uint32_t x;
        int seg = -1;
        int64_t off = 0;
        ch->segment = DVD_CHAPTER_NONE;
        if (ch->cell < 0)
            continue;
        x = t->pgc->cells[ch->cell].first_sector;
        for (int g = 0; g < t->nb_segments && seg < 0; g++) {
            const DVDSegment *sg = &t->segments[g];
            int64_t before = 0;
            for (int e = 0; e < sg->nb_extents; e++) {
                const DVDExtent *ex = &sg->extents[e];
                if (ex->sector <= x && x < ex->sector + ex->count) {
                    seg = g;
                    off = (before + x - ex->sector) * DVD_SECTOR;
                    break;
                }
                before += ex->count;
            }
        }
        if (seg < 0)
            continue;
        if (prev && (seg < prev->segment || (seg == prev->segment && off <= prev->offset)))
            continue;
        ch->segment = seg;
        ch->offset  = off;
        ch->time    = 0;
        for (int g = 0; g < seg; g++) {
            int64_t tm;
            if (!segment_time(T, t, &t->segments[g], segment_last_vobu(T, t, &t->segments[g]), &tm))
                av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: segment %d has no length\n", t->name, g + 1);
            else
                ch->time += tm;
        }
        {
            int64_t tm;
            if (!segment_time(T, t, &t->segments[seg], x, &tm))
                av_log(T->d->log, AV_LOG_DEBUG, "DVD: title %s: chapter %d has no time\n", t->name, k + 1);
            else
                ch->time += tm;
        }
        prev = ch;
    }
    /* a leading chapter that cannot be placed: dropped, or (the next one
     * not at the start) put at the start */
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
    place_chapters(T, t);
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
