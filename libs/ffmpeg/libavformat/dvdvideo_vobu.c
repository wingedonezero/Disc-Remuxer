/*
 * DVD-Video demuxer: VOBU records and stepping through the title VOBs
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
 * What the NAV packs of the title VOBs said about their VOBUs: one record per
 * VOBU start (or per block found not to hold a NAV pack), filled while the
 * disc is navigated (the scan) and while titles are checked and built, and
 * read when cells are checked and their block ranges are built. Title sets
 * whose title VOBs have a known image position share their records by image
 * block, so title sets that share VOB data share what is known about it; the
 * others keep their own. The first record stored for a block is kept.
 *
 * Stepping from one VOBU to the next uses the title set's VOBU address map
 * while it can be trusted (a block the map should list but does not makes it
 * untrusted), else the maps of two or more other title sets that cover the
 * same VOB data when they agree, else the NAV packs (DSI vobu_ea and
 * next_vobu, with the repair of a VOBU that ends its cell inside an
 * interleaved unit that goes on).
 */

#include "libavutil/tree.h"

#include "dvdvideo_internal.h"

#define SRI_NEXT_END_OF_CELL    0x3fffffffU     /* vobu_sri.next_vobu: last VOBU of the cell */
#define IN_ILVU                 0x4000          /* sml_pbi.category: inside an interleaved unit */
#define KEY_PER_TITLE_SET       (1ULL << 48)    /* records of a title set without an image position */
#define CHAIN_CHECKED           0x10
#define CHAIN_USABLE            0x20

typedef struct VobuEntry {
    uint64_t       key;
    DVDVideoVobu   rec;
} VobuEntry;

typedef struct ChainEntry {
    uint64_t key;               /* the cell's playback information, by address */
    int      state;
} ChainEntry;

static int map_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

static int map_free_elem(void *opaque, void *elem)
{
    av_free(elem);
    return 0;
}

void ff_dvdvideo_vobu_free(DVDVideoDisc *d)
{
    av_tree_enumerate(d->vobus, NULL, NULL, map_free_elem);
    av_tree_destroy(d->vobus);
    d->vobus = NULL;
    av_tree_enumerate(d->chains, NULL, NULL, map_free_elem);
    av_tree_destroy(d->chains);
    d->chains = NULL;
}

static uint64_t vobu_key(const DVDVideoDisc *d, int vtsn, uint32_t sector)
{
    uint32_t base = d->vts[vtsn].title_vobs_base;

    return base ? (uint32_t)(base + sector) : KEY_PER_TITLE_SET | (uint64_t)vtsn << 32 | sector;
}

int ff_dvdvideo_vobu_get(const DVDVideoDisc *d, int vtsn, uint32_t sector, DVDVideoVobu *out)
{
    uint64_t key = vobu_key(d, vtsn, sector);
    const VobuEntry *e = av_tree_find(d->vobus, &key, map_cmp, NULL);

    if (e && out)
        *out = e->rec;
    return !!e;
}

int ff_dvdvideo_vobu_put(DVDVideoDisc *d, int vtsn, uint32_t sector, const DVDVideoVobu *rec)
{
    struct AVTreeNode *node = av_tree_node_alloc();
    VobuEntry *e = av_malloc(sizeof(*e));

    if (!node || !e) {
        av_free(node);
        av_free(e);
        return AVERROR(ENOMEM);
    }
    e->key = vobu_key(d, vtsn, sector);
    e->rec = *rec;
    av_tree_insert(&d->vobus, e, map_cmp, &node);
    if (node)
        av_free(e);     /* not inserted: a record is kept for the block already */
    av_free(node);
    return 0;
}

void ff_dvdvideo_nav_fields(const uint8_t *nav, DVDVideoNavFields *f)
{
    f->next_vobu  = AV_RB32(nav + 0x541);
    f->vobu_ea    = AV_RB32(nav + 0x40f);
    f->ilvu_ea    = AV_RB32(nav + 0x429);
    f->category   = AV_RB16(nav + 0x427);
    f->vobu_s_ptm = AV_RB32(nav + 0x39);
    f->vobu_e_ptm = AV_RB32(nav + 0x3d);
}

int ff_dvdvideo_vobu_read_nav(DVDVideoDisc *d, int vtsn, uint32_t sector, DVDVideoNavFields *f)
{
    DVDVideoVobu none = { .next = DVDVIDEO_VOBU_NOT_A_NAV_PACK };
    uint8_t buf[DVDVIDEO_BLOCK_SIZE];
    int ret = ff_dvdvideo_source_vob_read(d->src, vtsn, 0, sector, buf, DVDVIDEO_NAV_READ_ATTEMPTS);

    if (ret < 0) {
        av_log(d->log, AV_LOG_DEBUG, "title set %d: the NAV pack at block %"PRIu32" could not be read\n",
               vtsn, sector);
    } else if (ff_dvdvideo_is_nav_pack(buf)) {
        ff_dvdvideo_nav_fields(buf, f);
        return 1;
    } else {
        /* a NAV pack blanked out (first 32 bytes 0xFF) whose PCI and DSI still
         * name the block: a VOBU of vobu_ea + 1 blocks and half a second,
         * followed directly by the next VOBU */
        int blanked = AV_RB32(buf + 0x2d) == sector && AV_RB32(buf + 0x40b) == sector &&
                      AV_RB32(buf + 0x40f) <= 0xfffff;

        for (int i = 0; i < 32 && blanked; i++)
            blanked = buf[i] == 0xff;
        if (blanked) {
            av_log(d->log, AV_LOG_DEBUG, "title set %d: block %"PRIu32" is a blanked NAV pack, taken as one\n",
                   vtsn, sector);
            memset(f, 0, sizeof(*f));
            f->vobu_ea    = AV_RB32(buf + 0x40f);
            f->next_vobu  = f->vobu_ea + 1;
            f->vobu_e_ptm = 45000;
            return 1;
        }
        av_log(d->log, AV_LOG_TRACE, "title set %d: block %"PRIu32" is not a NAV pack\n", vtsn, sector);
    }
    return ff_dvdvideo_vobu_put(d, vtsn, sector, &none);
}

int ff_dvdvideo_vobu_register(DVDVideoDisc *d, int vtsn, const DVDVideoNavFields *f, uint32_t sector,
                              int allow_self, int64_t *pnext, uint32_t *plen)
{
    uint32_t nx = f->next_vobu & 0x7fffffff, distance;
    int64_t next = nx == SRI_NEXT_END_OF_CELL ? (int64_t)DVDVIDEO_VOBU_ENDS_CELL : (int64_t)(uint32_t)(sector + nx);
    int ret;

    *plen = f->vobu_ea + 1;
    if (next == sector) {
        if (!allow_self) {
            av_log(d->log, AV_LOG_DEBUG, "title set %d: the NAV pack at block %"PRIu32" points at itself\n",
                   vtsn, sector);
            return 0;
        }
        *pnext = DVDVIDEO_VOBU_POINTS_AT_ITSELF;
        return 1;
    }
    if (next == DVDVIDEO_VOBU_ENDS_CELL && (f->category & IN_ILVU) && f->vobu_ea != f->ilvu_ea) {
        if (f->ilvu_ea < f->vobu_ea) {
            av_log(d->log, AV_LOG_WARNING, "Title set %d: the interleaved unit of the VOBU at block %"PRIu32" of its "
                   "title VOBs (byte %"PRIu64") ends before the VOBU does; the disc data is damaged there\n",
                   vtsn, sector, (uint64_t)sector * DVDVIDEO_BLOCK_SIZE);
        } else {
            /* the interleaved unit goes on after this VOBU */
            uint32_t following = sector + f->vobu_ea + 1;

            next = following;
            if (!ff_dvdvideo_vobu_get(d, vtsn, following, NULL)) {
                DVDVideoNavFields f2;
                int64_t n2;
                uint32_t l2;

                if ((ret = ff_dvdvideo_vobu_read_nav(d, vtsn, following, &f2)) <= 0) {
                    av_log(d->log, AV_LOG_DEBUG, "title set %d: the VOBU after block %"PRIu32" (%"PRIu32") could "
                           "not be read\n", vtsn, sector, following);
                    return ret;
                }
                if ((ret = ff_dvdvideo_vobu_register(d, vtsn, &f2, following, 0, &n2, &l2)) <= 0) {
                    if (!ret)
                        av_log(d->log, AV_LOG_DEBUG, "title set %d: the VOBU at block %"PRIu32" could not be "
                               "recorded\n", vtsn, following);
                    return ret;
                }
            }
        }
    }
    *pnext   = next;
    distance = next == DVDVIDEO_VOBU_ENDS_CELL ? DVDVIDEO_VOBU_NO_NEXT : (uint32_t)next - sector;
    if ((next == DVDVIDEO_VOBU_ENDS_CELL || distance < DVDVIDEO_VOBU_NO_NEXT) &&
        f->vobu_ea < 0x10000 && f->ilvu_ea < 0x10000) {
        DVDVideoVobu rec = {
            .vobu_ea    = f->vobu_ea,
            .ilvu_ea    = f->ilvu_ea,
            .next       = (uint32_t)(f->category & 0x6000) << 16 | distance,
            .vobu_s_ptm = f->vobu_s_ptm,
            .vobu_e_ptm = f->vobu_e_ptm,
        };
        if ((ret = ff_dvdvideo_vobu_put(d, vtsn, sector, &rec)) < 0)
            return ret;
    } else {
        av_log(d->log, AV_LOG_DEBUG, "title set %d: the VOBU at block %"PRIu32" is too large to record\n",
               vtsn, sector);
    }
    return 1;
}

/* Read the NAV pack at `sector` and record it: 1 = a NAV pack, 0 = none. */
static int read_and_register(DVDVideoDisc *d, int vtsn, uint32_t sector, DVDVideoNavFields *f)
{
    int64_t next;
    uint32_t len;
    int ret = ff_dvdvideo_vobu_read_nav(d, vtsn, sector, f);

    if (ret <= 0)
        return ret;
    if ((ret = ff_dvdvideo_vobu_register(d, vtsn, f, sector, 1, &next, &len)) < 0)
        return ret;
    return 1;
}

/* The title sets whose own VOBU maps are trusted and whose title VOBs cover
 * those of vtsn (same image start, or a range around them). */
static int covering_title_sets(const DVDVideoDisc *d, int vtsn, int *out)
{
    const DVDVideoTitleSet *me = &d->vts[vtsn];
    uint32_t dc = me->title_vobs_base;
    int nb = 0;

    if (!dc)
        return 0;
    for (int k = 1; k <= d->nb_vts; k++) {
        const DVDVideoTitleSet *o = &d->vts[k];
        uint32_t odc = o->title_vobs_base;

        if (k == vtsn || !o->ifo || o->map_distrusted || !odc)
            continue;
        if (odc == dc || (odc < dc && odc + o->title_vobs_sectors >= dc + me->title_vobs_sectors))
            out[nb++] = k;
    }
    return nb;
}

/* The VOBU after `sector` by a title set's own VOBU address map; -1 when the
 * map is distrusted, or `sector` is not in it or is its last entry. */
static int64_t next_from_map(const DVDVideoTitleSet *ts, uint32_t sector)
{
    int lo = 0, hi = ts->nb_vobu_starts;

    if (ts->map_distrusted)
        return -1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;

        if (ts->vobu_starts[mid] < sector)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= ts->nb_vobu_starts || ts->vobu_starts[lo] != sector || lo + 1 >= ts->nb_vobu_starts)
        return -1;
    return ts->vobu_starts[lo + 1];
}

int64_t ff_dvdvideo_vobu_next(DVDVideoDisc *d, int vtsn, uint32_t sector)
{
    const DVDVideoTitleSet *ts = &d->vts[vtsn];
    DVDVideoNavFields f1, f2;
    DVDVideoVobu rec;
    uint32_t n;
    int others[100], nb_others, hits = 0, agree = 1, ret;
    int64_t value = 0;

    if (!ts->map_distrusted)
        return next_from_map(ts, sector);
    nb_others = covering_title_sets(d, vtsn, others);
    for (int i = 0; i < nb_others && agree; i++) {
        const DVDVideoTitleSet *o = &d->vts[others[i]];
        int64_t t = next_from_map(o, sector + ts->title_vobs_base - o->title_vobs_base);
        int64_t v;

        if (t < 0) {
            agree = 0;
            break;
        }
        v = (uint32_t)(t + o->title_vobs_base - ts->title_vobs_base);
        if (hits && v != value)
            agree = 0;
        value = v;
        hits++;
    }
    if (nb_others && agree && hits >= 2)
        return value;
    if (ff_dvdvideo_vobu_get(d, vtsn, sector, &rec))
        return rec.next == DVDVIDEO_VOBU_NOT_A_NAV_PACK ? -1 : (int64_t)(uint32_t)(sector + 1 + rec.vobu_ea);
    if ((ret = ff_dvdvideo_vobu_read_nav(d, vtsn, sector, &f1)) <= 0)
        return ret < 0 ? ret : -1;
    n = sector + 1 + f1.vobu_ea;
    if ((ret = ff_dvdvideo_vobu_read_nav(d, vtsn, n, &f2)) <= 0)
        return ret < 0 ? ret : -1;
    {
        int64_t x;
        uint32_t l;

        if ((ret = ff_dvdvideo_vobu_register(d, vtsn, &f1, sector, 1, &x, &l)) < 0 ||
            (ret = ff_dvdvideo_vobu_register(d, vtsn, &f2, n, 1, &x, &l)) < 0)
            return ret;
    }
    return n;
}

/* Whether a NAV pack is at `sector` (recorded result, or read now). */
static int vobu_start_by_scan(DVDVideoDisc *d, int vtsn, uint32_t sector)
{
    DVDVideoNavFields f;
    DVDVideoVobu rec;

    if (ff_dvdvideo_vobu_get(d, vtsn, sector, &rec))
        return rec.next != DVDVIDEO_VOBU_NOT_A_NAV_PACK;
    return read_and_register(d, vtsn, sector, &f);
}

int ff_dvdvideo_vobu_check_start(DVDVideoDisc *d, int vtsn, uint32_t sector)
{
    DVDVideoTitleSet *ts = &d->vts[vtsn];
    int others[100], nb_others, hits = 0, all = 1;

    if (!ts->map_distrusted) {
        if (ff_dvdvideo_disc_is_vobu_start(ts, sector))
            return 1;
        av_log(d->log, AV_LOG_WARNING, "Title set %d: block %"PRIu32" of its title VOBs is not in the VOBU address "
               "map (VTS_VOBU_ADMAP); the map cannot be trusted, VOBU starts are taken from the NAV packs\n",
               vtsn, sector);
        ts->map_distrusted = 1;
        return vobu_start_by_scan(d, vtsn, sector);
    }
    nb_others = covering_title_sets(d, vtsn, others);
    for (int i = 0; i < nb_others; i++) {
        /* the block is looked up as given (not moved by the image start) */
        if (!ff_dvdvideo_disc_is_vobu_start(&d->vts[others[i]], sector)) {
            all = 0;
            break;
        }
        hits++;
    }
    if (nb_others && all && hits >= 2)
        return 1;
    return vobu_start_by_scan(d, vtsn, sector);
}

int ff_dvdvideo_vobu_step_from_nav(DVDVideoDisc *d, int vtsn, const cell_playback_t *cell, uint32_t sector,
                                   DVDVideoVobuStep *step)
{
    DVDVideoNavFields f;
    DVDVideoVobu rec;
    int64_t next;
    uint32_t len;
    int ret;

    if (ff_dvdvideo_vobu_get(d, vtsn, sector, &rec)) {
        if (rec.next == DVDVIDEO_VOBU_NOT_A_NAV_PACK)
            return 0;
        step->next = rec.next & DVDVIDEO_VOBU_NO_NEXT ? DVDVIDEO_VOBU_END_OF_CELL
                                                      : sector + (rec.next & 0x0fffffff);
        step->len  = rec.vobu_ea + 1;
        return 1;
    }
    if ((ret = ff_dvdvideo_vobu_read_nav(d, vtsn, sector, &f)) <= 0) {
        if (!ret)
            av_log(d->log, AV_LOG_DEBUG, "title set %d: no NAV pack at block %"PRIu32"\n", vtsn, sector);
        return ret;
    }
    if ((ret = ff_dvdvideo_vobu_register(d, vtsn, &f, sector, 1, &next, &len)) <= 0) {
        if (!ret)
            av_log(d->log, AV_LOG_DEBUG, "title set %d: the NAV pack at block %"PRIu32" could not be recorded\n",
                   vtsn, sector);
        return ret;
    }
    if (next == DVDVIDEO_VOBU_POINTS_AT_ITSELF) {
        if (cell->last_vobu_start_sector == sector) {
            next = DVDVIDEO_VOBU_END_OF_CELL;
        } else {
            av_log(d->log, AV_LOG_DEBUG, "title set %d: the NAV pack at block %"PRIu32" points at itself\n",
                   vtsn, sector);
            next = (uint32_t)(sector + len);
        }
    } else if (next == DVDVIDEO_VOBU_ENDS_CELL) {
        next = DVDVIDEO_VOBU_END_OF_CELL;
    }
    step->next = next;
    step->len  = len;
    return 1;
}

int ff_dvdvideo_vobu_chain_usable(DVDVideoDisc *d, int vtsn, const cell_playback_t *cell)
{
    int f = cell->block_mode << 6 | cell->block_type << 4 | cell->interleaved << 2 | cell->seamless_angle;
    uint64_t key = (uintptr_t)cell;
    ChainEntry *e = av_tree_find(d->chains, &key, map_cmp, NULL);
    DVDVideoVobuStep st;
    uint32_t s = cell->first_sector;
    int ok = 1, ret;

    if (f & 0xf4)
        return 0;
    if (!(f & 1))
        av_log(d->log, AV_LOG_DEBUG, "title set %d: VOBU chain check of a cell without seamless angle\n", vtsn);
    if (e)
        return !!(e->state & CHAIN_USABLE);
    for (int k = 0; k < 5; k++) {
        if ((ret = ff_dvdvideo_vobu_step_from_nav(d, vtsn, cell, s, &st)) < 0)
            return ret;
        if (!ret || st.next != (uint32_t)(s + st.len)) {
            ok = 0;
            break;
        }
        s = st.next;
        if (k < 4 && s >= cell->last_vobu_start_sector)
            break;
    }
    if (ok) {
        if ((ret = ff_dvdvideo_vobu_step_from_nav(d, vtsn, cell, cell->last_vobu_start_sector, &st)) < 0)
            return ret;
        ok = ret;
    }
    if (!ok)
        av_log(d->log, AV_LOG_DEBUG, "title set %d: the VOBU chain of a seamless-angle cell does not check out\n",
               vtsn);
    if (!(e = av_malloc(sizeof(*e))))
        return AVERROR(ENOMEM);
    e->key   = key;
    e->state = CHAIN_CHECKED | (ok ? CHAIN_USABLE : 0);
    {
        struct AVTreeNode *node = av_tree_node_alloc();

        if (!node) {
            av_free(e);
            return AVERROR(ENOMEM);
        }
        av_tree_insert(&d->chains, e, map_cmp, &node);
        av_free(node);
    }
    return ok;
}

int ff_dvdvideo_vobu_step(DVDVideoDisc *d, int vtsn, const cell_playback_t *cell, uint32_t sector,
                          DVDVideoVobuStep *step)
{
    int f = cell->block_mode << 6 | cell->block_type << 4 | cell->interleaved << 2 | cell->seamless_angle;
    int ret;

    if ((ret = ff_dvdvideo_vobu_check_start(d, vtsn, sector)) <= 0) {
        if (!ret)
            av_log(d->log, AV_LOG_DEBUG, "title set %d: block %"PRIu32" is not a VOBU start\n", vtsn, sector);
        return ret;
    }
    if (sector < cell->last_vobu_start_sector && !(f & 0xf4)) {
        int64_t n = -1;

        if (!(f & 1)) {
            n = ff_dvdvideo_vobu_next(d, vtsn, sector);
            if (n < -1)
                return n;
            /* a failed map step leaves no next VOBU: the NAV packs are used instead */
            if (d->vts[vtsn].map_distrusted)
                n = -1;
        } else if (!d->vts[vtsn].map_distrusted) {
            if ((ret = ff_dvdvideo_vobu_chain_usable(d, vtsn, cell)) < 0)
                return ret;
            if (ret) {
                n = ff_dvdvideo_vobu_next(d, vtsn, sector);
                if (n < -1)
                    return n;
                if (d->vts[vtsn].map_distrusted)
                    n = -1;
            }
        }
        if (n >= 0) {
            step->next = n;
            step->len  = (uint32_t)n - sector;
            return 1;
        }
    }
    return ff_dvdvideo_vobu_step_from_nav(d, vtsn, cell, sector, step);
}
