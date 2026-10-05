/*
 * DVD-Video demuxer: which program chains and cells are real content
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
 * The checks that keep structure-protection cells (fake, empty, unreadable,
 * out of place) out of titles: per cell (times, sectors, cell commands, the
 * NAV packs of its VOBUs) and per program chain (protection patterns of many
 * discontinuous cells on shared title VOBs, too little data per second of
 * play, a broken program map, cells that should join seamlessly but do not).
 *
 * Cell flags are cell_playback_t byte 0 (ff_dvdvideo_cell_flags()): bits 7-6
 * block_mode, 5-4 block_type, 3 seamless_play, 2 interleaved, 1
 * stc_discontinuity, 0 seamless_angle. Times are dvd_time_t as one 32-bit BCD
 * value (hours in the top byte, the frame byte at the bottom).
 */

#include "dvdvideo_internal.h"

uint32_t ff_dvdvideo_dvd_time(const dvd_time_t *t)
{
    return (uint32_t)t->hour << 24 | t->minute << 16 | t->second << 8 | t->frame_u;
}

uint32_t ff_dvdvideo_bcd_secs(uint32_t t)
{
    uint32_t h = (t >> 28) * 10 + (t >> 24 & 0xf);
    uint32_t m = (t >> 20 & 0xf) * 10 + (t >> 16 & 0xf);
    uint32_t s = (t >> 12 & 0xf) * 10 + (t >> 8 & 0xf);

    return h * 3600 + m * 60 + s;
}

static uint32_t cell_time(const pgc_t *pgc, int i)
{
    return ff_dvdvideo_dvd_time(&pgc->cell_playback[i].playback_time);
}

static int cell_cmd(const pgc_t *pgc, int i)
{
    return pgc->cell_playback[i].cell_cmd_nr;
}

/* A recorded VOBU between first and last (inclusive) marked as not a NAV pack. */
static int bad_record_between(DVDVideoDisc *d, int vtsn, uint32_t first, uint32_t last)
{
    uint32_t s = first;

    for (;;) {
        DVDVideoVobu rec;
        uint32_t k;

        if (ff_dvdvideo_vobu_get(d, vtsn, s, &rec) && rec.next == DVDVIDEO_VOBU_NOT_A_NAV_PACK)
            return 1;
        if (!ff_dvdvideo_vobu_next_recorded(d, vtsn, s, &k) || k > last)
            return 0;
        s = k;
    }
}

/* Whether the VOBU at `sector` is unusable: outside the title VOBs, recorded
 * as not a NAV pack, unreadable or not a NAV pack (then recorded so). A NAV
 * pack read now is recorded. */
static int vobu_is_bad(DVDVideoDisc *d, int vtsn, uint32_t sector)
{
    uint8_t buf[DVDVIDEO_BLOCK_SIZE];
    DVDVideoNavFields f;
    DVDVideoVobu rec;
    int64_t next;
    uint32_t len;
    int ret;

    if (sector >= d->vts[vtsn].title_vobs_sectors)
        return 1;
    if (ff_dvdvideo_vobu_get(d, vtsn, sector, &rec))
        return rec.next == DVDVIDEO_VOBU_NOT_A_NAV_PACK;
    if (ff_dvdvideo_source_vob_read(d->src, vtsn, 0, sector, buf, DVDVIDEO_NAV_READ_ATTEMPTS) < 0 ||
        !ff_dvdvideo_is_nav_pack(buf)) {
        DVDVideoVobu none = { .next = DVDVIDEO_VOBU_NOT_A_NAV_PACK };

        return (ret = ff_dvdvideo_vobu_put(d, vtsn, sector, &none)) < 0 ? ret : 1;
    }
    ff_dvdvideo_nav_fields(buf, &f);
    return (ret = ff_dvdvideo_vobu_register(d, vtsn, &f, sector, 1, &next, &len)) < 0 ? ret : 0;
}

int ff_dvdvideo_vobu_ptm(DVDVideoDisc *d, int vtsn, const pgc_t *pgc, int i, uint32_t sector, int force,
                         uint32_t *s_ptm, uint32_t *e_ptm)
{
    DVDVideoNavFields f;
    DVDVideoVobu rec;
    int64_t next;
    uint32_t len;
    int ret;

    if (ff_dvdvideo_vobu_get(d, vtsn, sector, &rec)) {
        *s_ptm = rec.vobu_s_ptm;
        *e_ptm = rec.vobu_e_ptm;
        return rec.next != DVDVIDEO_VOBU_NOT_A_NAV_PACK;
    }
    if (!force) {
        if ((ret = ff_dvdvideo_cell_rejected(d, vtsn, pgc, i, 1, i - 1)) < 0)
            return ret;
        if (ret && (ret = ff_dvdvideo_cell_deep_check_failed(d, vtsn, pgc, i)) != 0)
            return ret < 0 ? ret : 0;
    }
    if (ff_dvdvideo_vobu_get(d, vtsn, sector, &rec)) {
        *s_ptm = rec.vobu_s_ptm;
        *e_ptm = rec.vobu_e_ptm;
        return rec.next != DVDVIDEO_VOBU_NOT_A_NAV_PACK;
    }
    if ((ret = ff_dvdvideo_vobu_read_nav(d, vtsn, sector, &f)) <= 0)
        return ret;
    if ((ret = ff_dvdvideo_vobu_register(d, vtsn, &f, sector, 1, &next, &len)) < 0)
        return ret;
    *s_ptm = f.vobu_s_ptm;
    *e_ptm = f.vobu_e_ptm;
    return 1;
}

int ff_dvdvideo_cell_duration(DVDVideoDisc *d, int vtsn, const pgc_t *pgc, int i, int force, int quiet,
                              uint32_t *ticks)
{
    const cell_playback_t *c = &pgc->cell_playback[i];
    uint32_t s1, e1, s2, e2;
    int ret;

    if ((ret = ff_dvdvideo_vobu_ptm(d, vtsn, pgc, i, c->first_sector, force, &s1, &e1)) <= 0) {
        if (!ret && !quiet)
            av_log(d->log, AV_LOG_DEBUG, "title set %d: no times for the first VOBU %"PRIu32" of a cell\n",
                   vtsn, c->first_sector);
        return ret;
    }
    if ((ret = ff_dvdvideo_vobu_ptm(d, vtsn, pgc, i, c->last_vobu_start_sector, force, &s2, &e2)) <= 0) {
        if (!ret && !quiet)
            av_log(d->log, AV_LOG_DEBUG, "title set %d: no times for the last VOBU of a cell ending at %"PRIu32"\n",
                   vtsn, c->last_sector);
        return ret;
    }
    if (e1 < s1 || e2 < s2 || e2 < e1)
        return 0;
    *ticks = e2 - s1;
    return 1;
}

int ff_dvdvideo_cell_deep_check_failed(DVDVideoDisc *d, int vtsn, const pgc_t *pgc, int i)
{
    const cell_playback_t *c = &pgc->cell_playback[i];
    uint32_t s = c->first_sector, s1, e1, s2, e2;
    int ret = 0;

    if (c->last_vobu_start_sector < c->first_sector)
        return 1;
    if (bad_record_between(d, vtsn, c->first_sector, c->last_vobu_start_sector))
        return 1;
    if ((ret = vobu_is_bad(d, vtsn, c->first_sector)) != 0)
        return ret;
    if (c->last_vobu_start_sector != c->first_sector && (ret = vobu_is_bad(d, vtsn, c->last_vobu_start_sector)))
        return ret;
    /* every VOBU of the cell must be a NAV pack (all of them, not a random
     * sample: the result is the same every time); the VOBUs are walked first,
     * then checked */
    {
        uint32_t *starts = NULL;
        int nb = 0, bad = 0;

        while (s < c->last_vobu_start_sector) {
            int64_t n;

            if (!av_dynarray2_add((void **)&starts, &nb, sizeof(*starts), (const uint8_t *)&s)) {
                ret = AVERROR(ENOMEM);
                break;
            }
            if ((n = ff_dvdvideo_vobu_next(d, vtsn, s)) < 0) {
                ret = n < -1 ? n : 1;
                break;
            }
            s = n;
        }
        for (int k = 0; !ret && k < nb && !bad; k++)
            if ((bad = vobu_is_bad(d, vtsn, starts[k])) < 0)
                ret = bad;
        av_free(starts);
        if (ret)
            return ret;
        if (bad)
            return 1;
    }
    /* the last VOBU's times are read only when the first VOBU's are there */
    if ((ret = ff_dvdvideo_vobu_ptm(d, vtsn, pgc, i, c->first_sector, 1, &s1, &e1)) <= 0)
        return ret < 0 ? ret : 1;
    if ((ret = ff_dvdvideo_vobu_ptm(d, vtsn, pgc, i, c->last_vobu_start_sector, 1, &s2, &e2)) <= 0)
        return ret < 0 ? ret : 1;
    return !(s1 <= e1 && e1 <= e2 && s2 <= e2);
}

/* A long (30 min or more) single cell made of 1000 or more tiny VOBUs of one
 * fixed size (at most 11 blocks): filler, not content. */
static int cell_is_uniform_filler(DVDVideoDisc *d, int vtsn, const cell_playback_t *c)
{
    int64_t nx, s, t;
    uint32_t dist, n = 0, k;

    if (ff_dvdvideo_bcd_secs(ff_dvdvideo_dvd_time(&c->playback_time)) < 1800 ||
        c->first_sector == c->last_vobu_start_sector)
        return 0;
    if ((nx = ff_dvdvideo_vobu_next(d, vtsn, c->first_sector)) < 0)
        return nx < -1 ? nx : 1;
    dist = (uint32_t)nx - c->first_sector;
    if (dist > 11)
        return 0;
    if (nx == c->last_vobu_start_sector || nx > c->last_sector)
        return 0;
    for (s = nx;;) {
        if ((t = ff_dvdvideo_vobu_next(d, vtsn, s)) < 0)
            return t < -1 ? t : 1;
        s = t;
        n++;
        if (s == c->last_vobu_start_sector || s > c->last_sector)
            break;
    }
    if (n < 1000)
        return 0;
    k = 9 * n / 10;
    s = c->first_sector;
    for (uint32_t j = 0; j < k; j++) {
        if ((t = ff_dvdvideo_vobu_next(d, vtsn, s)) < 0)
            return t < -1 ? t : 1;
        if ((uint32_t)t - (uint32_t)s != dist)
            return 0;
        s = t;
    }
    return 1;
}

int ff_dvdvideo_cell_rejected(DVDVideoDisc *d, int vtsn, const pgc_t *pgc, int i, int quick, int prev)
{
    const cell_playback_t *c = &pgc->cell_playback[i];
    uint32_t t = cell_time(pgc, i), tt = t & 0xffffff00, size = d->vts[vtsn].title_vobs_sectors, secs;
    int check_command, ret;

    if (c->first_sector > c->last_vobu_start_sector)
        return 1;
    if (bad_record_between(d, vtsn, c->first_sector, c->last_vobu_start_sector))
        return 1;
    if (!(t & 0xffffff3f))
        return 1;
    if (c->last_vobu_start_sector >= size || c->last_sector >= size || c->first_sector >= size)
        return 1;
    if (tt <= 0x100 && !quick) {
        if (c->first_sector == c->last_vobu_start_sector) {
            int ok_prev = prev >= 0 && prev + 1 == i && prev + 2 != pgc->nr_of_cells;

            if (!ok_prev || cell_cmd(pgc, i))
                return 1;
            return ff_dvdvideo_cell_deep_check_failed(d, vtsn, pgc, i);
        }
        check_command = 1;
    } else {
        check_command = tt <= 0x500;
    }
    if (check_command && cell_cmd(pgc, i))
        return 1;
    if (pgc->nr_of_cells == 1 && (ret = cell_is_uniform_filler(d, vtsn, c)) != 0)
        return ret;
    secs = ff_dvdvideo_bcd_secs(t);
    if (!((secs < 1801 && tt >= 0x501) || quick) && (ret = ff_dvdvideo_cell_deep_check_failed(d, vtsn, pgc, i)))
        return ret;
    if (quick)
        return 0;
    {
        uint32_t ticks;

        ret = ff_dvdvideo_cell_duration(d, vtsn, pgc, i, 1, 1, &ticks);
        return ret < 0 ? ret : !ret;
    }
}

/* For a cell of an interleaved angle block: the block's cell with the highest
 * (or lowest) first sector; any other cell is returned unchanged. */
static int angle_block_extreme_cell(const pgc_t *pgc, int i, int want_highest)
{
    int f = ff_dvdvideo_cell_flags(pgc, i), first, last, best = i;

    if ((f & 0x34) != 0x14)
        return i;
    switch (f >> 6) {
    case 1:
        for (int k = i + 1;; k++) {
            if (k >= pgc->nr_of_cells)
                return i;
            if (ff_dvdvideo_cell_flags(pgc, k) >> 6 == 3) {
                first = i;
                last  = k;
                break;
            }
            if (ff_dvdvideo_cell_flags(pgc, k) >> 6 != 2)
                return i;
        }
        break;
    case 3:
        for (int k = i;;) {
            if (!k)
                return i;
            k--;
            if (ff_dvdvideo_cell_flags(pgc, k) >> 6 == 1) {
                first = k;
                last  = i;
                break;
            }
            if (ff_dvdvideo_cell_flags(pgc, k) >> 6 != 2)
                return i;
        }
        break;
    default:
        return i;
    }
    for (int p = first; p <= last; p++) {
        uint32_t ps = pgc->cell_playback[p].first_sector, bs = pgc->cell_playback[best].first_sector;

        if (want_highest ? ps > bs : ps < bs)
            best = p;
    }
    return best;
}

int ff_dvdvideo_ilvu_chain_reaches(DVDVideoDisc *d, int vtsn, uint32_t from, uint32_t to)
{
    const DVDVideoTitleSet *ts = &d->vts[vtsn];
    int ret;

    if (from > to)
        return 0;
    while (from != to) {
        DVDVideoVobu rec;
        uint32_t ea;

        if (from > to)
            return 0;
        if ((ret = ff_dvdvideo_vobu_check_start(d, vtsn, from)) <= 0)
            return ret;
        if (ff_dvdvideo_vobu_get(d, vtsn, from, &rec)) {
            if ((rec.next & 0x60000000) != 0x60000000)
                return 0;
            ea = rec.ilvu_ea;
        } else {
            DVDVideoNavFields f;
            int64_t next;
            uint32_t len;

            if ((ret = ff_dvdvideo_vobu_read_nav(d, vtsn, from, &f)) <= 0)
                return ret;
            if ((ret = ff_dvdvideo_vobu_register(d, vtsn, &f, from, 0, &next, &len)) <= 0)
                return ret;
            if ((f.category & 0x6000) != 0x6000 || f.ilvu_ea > 0x7ffffffe)
                return 0;
            ea = f.ilvu_ea;
        }
        from += ea + 1;
        if (!ts->map_distrusted) {
            int lo = 0, hi = ts->nb_vobu_starts;

            while (lo < hi) {
                int mid = (lo + hi) / 2;

                if (ts->vobu_starts[mid] < from)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < ts->nb_vobu_starts && ts->vobu_starts[lo] == from && lo) {
                uint32_t prev = ts->vobu_starts[lo - 1];

                if (!ff_dvdvideo_vobu_get(d, vtsn, prev, NULL)) {
                    DVDVideoNavFields f;
                    int64_t next;
                    uint32_t len;

                    if ((ret = ff_dvdvideo_vobu_read_nav(d, vtsn, prev, &f)) < 0)
                        return ret;
                    if (ret && (ret = ff_dvdvideo_vobu_register(d, vtsn, &f, prev, 1, &next, &len)) < 0)
                        return ret;
                }
            }
        }
    }
    return 1;
}

int ff_dvdvideo_cell_cmd_may_link(const pgc_t *pgc, int cmd_nr)
{
    const pgc_command_tbl_t *t = pgc->command_tbl;
    const uint8_t *c;
    int a, b, r = 0;
    uint16_t w;

    if (!cmd_nr || !t || t->nr_of_cell < cmd_nr || !t->cell_cmds)
        return 0;
    c = t->cell_cmds[cmd_nr - 1].bytes;
    w = AV_RB16(c);
    a = w & 0xff0f;
    b = w & 0xf0ff;
    if (a == 0x2001 && !c[7])
        return 0;
    if ((b == 0x7001 || b == 0x6001) && !c[7])
        return 0;
    if (a == 0x2001 || a == 0x2005 || a == 0x2006 || a == 0x2007 || a == 0x3008) {
        int op = c[1] >> 4 & 7, k;

        if (c[1] & 0x80) {
            k = op == 1 && !AV_RB16(c + 4) ? 2 : op == 0;
        } else {
            int same = c[5] == c[3];

            k = op == 0 ? 1 : op == 1 ? 0 : (op == 2 || op == 4 || op == 6) ? same : 2 * same;
        }
        r = k != 2;
    }
    if (b == 0x6001 || b == 0x6004 || b == 0x6005 || b == 0x6006 || b == 0x6007 ||
        b == 0x7001 || b == 0x7004 || b == 0x7005 || b == 0x7006 || b == 0x7007)
        return 1;
    return r;
}

/* Whether the seamless join of cells with flags fa -> fb is tested for
 * contiguity: plain -> plain, plain -> first cell of an interleaved angle
 * block, last cell of an interleaved angle block -> plain ("plain" = not
 * interleaved, no block). */
static int join_is_tested(int fa, int fb)
{
    if (!(fa & 0x04))
        return !(fa & 0x30) && fa < 0x40 &&
               ((!(fb & 0x04) && !(fb & 0x30) && fb < 0x40) ||
                ((fb & 0x04) && (fb & 0x30) == 0x10 && (fb & 0xc0) == 0x40));
    return (fa & 0x30) == 0x10 && fa >= 0xc0 && !(fb & 0x04) && !(fb & 0x30) && fb <= 0x3f;
}

/* Two cells p (earlier in the block) and q of one angle block fit together:
 * the same first, last VOBU start and last sectors; or one starts strictly
 * inside the other's [first, last VOBU start); or each is one interleaved unit
 * (first_ilvu_end_sector == last_sector); or the lower cell's unit ends before
 * the higher cell starts, where both have last_sector below their
 * first_ilvu_end_sector, or neither has a unit end recorded (0). The lower
 * cell is p when the first sectors are equal. */
static int angle_pair_ok(const cell_playback_t *p, const cell_playback_t *q)
{
    const cell_playback_t *lo, *hi;
    int same = p->first_sector == q->first_sector && p->last_vobu_start_sector == q->last_vobu_start_sector &&
               p->last_sector == q->last_sector;
    int ends_before;

#define STARTS_INSIDE(s, o) ((o)->first_sector < (s)->first_sector && (s)->first_sector < (o)->last_vobu_start_sector)
    if (same || STARTS_INSIDE(p, q) || STARTS_INSIDE(q, p))
        return 1;
#undef STARTS_INSIDE
    if (p->last_sector == p->first_ilvu_end_sector && q->last_sector == q->first_ilvu_end_sector)
        return 1;
    lo = p->first_sector <= q->first_sector ? p : q;
    hi = lo == p ? q : p;
    ends_before = (uint32_t)(FFMAX(lo->last_sector, lo->first_ilvu_end_sector) + 1) <= hi->first_sector;
    if (p->last_sector < p->first_ilvu_end_sector)
        return q->last_sector < q->first_ilvu_end_sector && ends_before;
    return !p->first_ilvu_end_sector && !q->first_ilvu_end_sector && ends_before;
}

/* The interleaved angle block starting at cell first: every following cell an
 * interleaved angle cell without seamless angle change, in block mode 2 up to
 * one in block mode 3; every pair of its cells fits together. */
static int angle_block_ok(const pgc_t *pgc, int first)
{
    int last = first;

    for (;;) {
        int f;

        if (++last >= pgc->nr_of_cells)
            return 0;
        f = ff_dvdvideo_cell_flags(pgc, last);
        if ((f & 0x35) != 0x14)
            return 0;
        if (f >> 6 == 3)
            break;
        if (f >> 6 != 2)
            return 0;
    }
    for (int k = first; k <= last; k++)
        for (int j = k + 1; j <= last; j++)
            if (!angle_pair_ok(&pgc->cell_playback[k], &pgc->cell_playback[j]))
                return 0;
    return 1;
}

int ff_dvdvideo_pgc_rejection(DVDVideoDisc *d, int vtsn, int pgcn, const pgc_t *pgc, int *reason)
{
    const cell_playback_t *cells = pgc->cell_playback;
    int n = cells ? pgc->nr_of_cells : 0, ov, ret;
    const uint8_t *pm = pgc->program_map;
    int npm = pm ? pgc->nr_of_programs : 0;

#define REJECT(r) do {                                                                              \
        *reason = r;                                                                                \
        av_log(d->log, AV_LOG_DEBUG, "title set %d: program chain %d rejected, reason %d\n", vtsn, pgcn, r); \
        return 1;                                                                                   \
    } while (0)

    if (!n)
        REJECT(0);
    ov = ff_dvdvideo_disc_title_vobs_overlap(d, vtsn);

    /* title VOBs shared with another title set: a chain whose cells after the
     * first all carry stc_discontinuity */
    if (ov && n >= 2) {
        int all_flagged = 1, n2 = n, in_order = 1;

        for (int i = 1; i < n && all_flagged; i++)
            all_flagged = !!(ff_dvdvideo_cell_flags(pgc, i) & 0x02);
        if (all_flagged && n >= 12)
            REJECT(1);
        /* the last cell is left out of the order test when it is rejected or
         * is a short cell (3 s or less) starting a new VOB; this check runs
         * whether or not every cell is flagged */
        if (n > 3) {
            if ((ret = ff_dvdvideo_cell_rejected(d, vtsn, pgc, n - 1, 0, n - 2)) < 0)
                return ret;
            if (ret || ((cell_time(pgc, n - 1) & 0xffffff00) < 0x301 &&
                        pgc->cell_position[n - 1].vob_id_nr != pgc->cell_position[n - 2].vob_id_nr &&
                        pgc->cell_position[n - 1].cell_nr == 1))
                n2 = n - 1;
        }
        for (int i = 0; i + 1 < n2 && in_order; i++)
            in_order = cells[i].first_sector <= cells[i + 1].first_sector;
        if (all_flagged && !in_order && n >= 7)
            REJECT(2);
    }

    /* data rate: blocks of every cell over the seconds of the accepted cells,
     * each cell checked with the cell before it, accepted or not */
    if (n >= 4) {
        uint32_t seconds = 0, sectors = 0;

        for (int i = 0; i < n; i++) {
            if ((ret = ff_dvdvideo_cell_rejected(d, vtsn, pgc, i, 1, i - 1)) < 0)
                return ret;
            if (!ret)
                seconds += ff_dvdvideo_bcd_secs(cell_time(pgc, i));
            sectors += cells[i].last_sector - cells[i].first_sector + 1;
        }
        if (!seconds)
            REJECT(3);
        if (sectors / seconds < 50)
            REJECT(4);
    }

    /* program map: entry cells in range and in order */
    if (!npm)
        return 0;
    if (!pm[0] || pm[0] > n)
        REJECT(5);
    for (int k = 1; k < npm; k++) {
        if (!pm[k] || pm[k] > n)
            REJECT(5);
        if (pm[k] < pm[k - 1])
            REJECT(6);
    }
    if (npm < 3)
        return 0;

    /* seamless joins inside programs 2.. (program 1 is not examined): from
     * each program's entry cell, every following pair of cells joined by
     * seamless_play with no cell commands */
    for (int e = 1; e < npm; e++) {
        for (int a = pm[e] - 1; a < n - 1; a++) {
            int b = a + 1, x = a, y = b, fx, fy, gap_needs_units;

            if (!(ff_dvdvideo_cell_flags(pgc, b) & 0x08) || cell_cmd(pgc, a) || cell_cmd(pgc, b))
                break;
            fx = ff_dvdvideo_cell_flags(pgc, a);
            if (!((fx | ff_dvdvideo_cell_flags(pgc, b)) & 0x01) && join_is_tested(fx, ff_dvdvideo_cell_flags(pgc, b))) {
                x = angle_block_extreme_cell(pgc, a, 1);
                y = angle_block_extreme_cell(pgc, b, 0);
                if (cells[x].last_sector + 1 != cells[y].first_sector)
                    REJECT(7);
                fx = ff_dvdvideo_cell_flags(pgc, x);
            }
            /* between an interleaved and a non-interleaved cell (neither in a
             * block), a gap must be made of interleaved units */
            fy = ff_dvdvideo_cell_flags(pgc, y);
            gap_needs_units = fx < 0x10 && (!(fx & 0x04) ? (fy & 0xf4) == 0x04 : !(fy & 0xf4));
            if (gap_needs_units) {
                uint32_t from = cells[x].last_sector + 1;

                if (from != cells[y].first_sector) {
                    if ((ret = ff_dvdvideo_ilvu_chain_reaches(d, vtsn, from, cells[y].first_sector)) < 0)
                        return ret;
                    if (!ret)
                        REJECT(8);
                }
            }
            /* the first cell of an interleaved angle block (no seamless angle
             * change): the block must be well formed and its cells'
             * interleaved units must fit together */
            if (ov && (fx & 0xf5) == 0x54 && !angle_block_ok(pgc, x))
                REJECT(9);
        }
    }
    return 0;
#undef REJECT
}
