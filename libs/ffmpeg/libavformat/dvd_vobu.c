/*
 * DVD-Video: the VOBUs of a title set's title VOBs. A VOBU starts with a
 * NAV pack (DVD-Video Book; libdvdread's nav_types.h: pci_t / dsi_t): its
 * DSI gives the VOBU's length (dsi_gi.vobu_ea), the end of its interleaved
 * unit (sml_pbi.ilvu_ea), its category (sml_pbi.category) and where the
 * next VOBU of the cell starts (vobu_sri.next_vobu); its PCI gives its
 * presentation start and end (vobu_s_ptm / vobu_e_ptm).
 *
 * VOBU records. What a NAV pack says is recorded per sector the first time
 * it is read (by the navigation scan, or here), and a sector that is no NAV
 * pack (or cannot be read) is recorded as such; later questions about the
 * sector are answered from the record. Title sets whose title VOBs lie at a
 * known place of a disc image keep their records on the disc by absolute
 * sector (title sets may share title VOBs), the others per title set.
 *
 * VOBU starts. The title set's VOBU address map (VTS_VOBU_ADMAP) lists
 * every VOBU start. While it is trusted it answers "is this a VOBU start"
 * and "where is the next VOBU"; once a sector that should start a VOBU is
 * missing from it, it is no longer trusted and the NAV packs answer
 * (records first, else the sector is read).
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
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"

static int grow(void **p, int *alloc, int n, size_t elem)
{
    if (n < *alloc)
        return 0;
    return av_reallocp_array(p, *alloc = FFMAX(16, *alloc * 2), elem);
}

/* ---- VOBU records ---- */

void ff_dvd_vobu_recs_free(DVDVobuRecs *v)
{
    av_freep(&v->recs);
    v->nb = v->alloc = 0;
}

static DVDVobuRecs *vobu_recs(DVDDisc *d, DVDIfo *f)
{
    return f->title_vob_base ? &d->vobus : &f->vobus;
}

static int vobu_lower(const DVDVobuRecs *v, uint32_t key)
{
    int lo = 0, hi = v->nb;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (v->recs[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* insert, the first record of a key kept */
static int vobu_insert(DVDVobuRecs *v, const DVDVobuRec *r)
{
    int at = vobu_lower(v, r->key), ret;

    if (at < v->nb && v->recs[at].key == r->key)
        return 0;
    if ((ret = grow((void **)&v->recs, &v->alloc, v->nb, sizeof(*v->recs))) < 0)
        return ret;
    memmove(v->recs + at + 1, v->recs + at, (v->nb - at) * sizeof(*v->recs));
    v->recs[at] = *r;
    v->nb++;
    return 0;
}

const DVDVobuRec *ff_dvd_vobu_find(const DVDDisc *d, const DVDIfo *f, uint32_t sector, int64_t *next_key)
{
    const DVDVobuRecs *v = f->title_vob_base ? &d->vobus : &f->vobus;
    uint32_t key = sector + f->title_vob_base;
    int at = vobu_lower(v, key);

    if (at == v->nb) {
        if (next_key)
            *next_key = -1;
        return NULL;
    }
    if (v->recs[at].key == key) {
        if (next_key)
            *next_key = at + 1 < v->nb ? (int64_t)v->recs[at + 1].key - f->title_vob_base : -1;
        return &v->recs[at];
    }
    if (next_key)
        *next_key = (int64_t)v->recs[at].key - f->title_vob_base;
    return NULL;
}

int ff_dvd_vobu_mark_bad(DVDDisc *d, DVDIfo *f, uint32_t sector)
{
    DVDVobuRec none = { sector + f->title_vob_base, 0, 0, DVD_VOBU_NO_NAV, 0, 0 };
    return vobu_insert(vobu_recs(d, f), &none);
}

/* ---- NAV packs ---- */

/* A NAV pack exactly as DVD-Video lays it out: pack header, system header,
 * PCI and DSI packets. */
int ff_dvd_is_nav_pack(const uint8_t *b)
{
    return AV_RB32(b) == 0x000001ba && (b[4] & 0xc0) == 0x40 &&
           AV_RB32(b + 0x0e) == 0x000001bb && AV_RB16(b + 0x12) == 0x0012 &&
           AV_RB32(b + 0x26) == 0x000001bf && b[0x2a] == 0x03 && b[0x2b] == 0xd4 && b[0x2c] == 0x00 &&
           AV_RB32(b + 0x400) == 0x000001bf && b[0x404] == 0x03 && b[0x405] == 0xfa && b[0x406] == 0x01;
}

void ff_dvd_nav_parse(DVDNav *n, const uint8_t *b)
{
    n->vobu_s_ptm = AV_RB32(b + 0x39);
    n->vobu_e_ptm = AV_RB32(b + 0x3d);
    n->vobu_ea    = AV_RB32(b + 0x40f);
    n->category   = AV_RB16(b + 0x427);
    n->ilvu_ea    = AV_RB32(b + 0x429);
    n->next_vobu  = AV_RB32(b + 0x541);
}

int ff_dvd_nav_read(DVDDisc *d, DVDIfo *f, uint32_t sector, DVDNav *n)
{
    uint8_t b[DVD_SECTOR];
    int ret = ff_dvd_vob_read(d, f, 0, sector, b);

    if (ret >= 0 && ff_dvd_is_nav_pack(b)) {
        ff_dvd_nav_parse(n, b);
        return 1;
    }
    if (ret >= 0) {
        int blank = 1;
        for (int i = 0; i < 32; i++)
            blank &= b[i] == 0xff;
        if (blank && AV_RB32(b + 0x2d) == sector && AV_RB32(b + 0x40b) == sector && AV_RB32(b + 0x40f) <= 0xfffff) {
            /* a NAV pack whose headers were blanked: accepted, 0.5 s */
            memset(n, 0, sizeof(*n));
            n->vobu_e_ptm = 45000;
            n->vobu_ea    = AV_RB32(b + 0x40f);
            n->next_vobu  = n->vobu_ea + 1;
            av_log(d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: sector %"PRIu32" is a NAV pack with blanked "
                   "headers\n", f->index, sector);
            return 1;
        }
    }
    ff_dvd_vobu_mark_bad(d, f, sector);
    return 0;
}

int ff_dvd_vobu_register(DVDDisc *d, DVDIfo *f, const DVDNav *n, uint32_t sector, int allow_self,
                         uint32_t *next_out, uint32_t *len_out)
{
    uint32_t nx = n->next_vobu & 0x7fffffff, ea = n->vobu_ea;
    int64_t next = nx == 0x3fffffff ? -2 : (int64_t)sector + nx;

    if (next == sector) {
        if (!allow_self) {
            av_log(d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: the VOBU at %"PRIu32" names itself as the next "
                   "one\n", f->index, sector);
            return 0;
        }
        *next_out = DVD_VOBU_SELF;
        *len_out  = ea + 1;
        return 1;                       /* nothing recorded */
    }
    if (next == -2 && (n->category & 0x4000) && ea != n->ilvu_ea) {
        /* the cell ends inside an interleaved unit */
        if (n->ilvu_ea < ea) {
            av_log(d->log, AV_LOG_WARNING, "DVD: VTS_%02d: the title VOBs are corrupt at sector %"PRIu32" "
                   "(the interleaved unit ends before its VOBU): worked around\n", f->index, sector);
        } else {
            DVDNav n2;
            uint32_t nn, nl;
            next = (int64_t)sector + ea + 1;
            if (!ff_dvd_vobu_find(d, f, (uint32_t)next, NULL) &&
                (!ff_dvd_nav_read(d, f, (uint32_t)next, &n2) ||
                 !ff_dvd_vobu_register(d, f, &n2, (uint32_t)next, 0, &nn, &nl))) {
                av_log(d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: the VOBU after %"PRIu32" in its interleaved unit "
                       "cannot be read\n", f->index, sector);
                return 0;
            }
        }
    }
    *next_out = next == -2 ? DVD_VOBU_END : (uint32_t)next;
    *len_out  = ea + 1;
    if (ea < 0x10000 && n->ilvu_ea < 0x10000 && (next == -2 || next - sector < 0x10000000)) {
        DVDVobuRec r = { sector + f->title_vob_base, ea, n->ilvu_ea,
                         (uint32_t)(n->category & 0x6000) << 16 | (next == -2 ? 0x10000000 : (uint32_t)(next - sector)),
                         n->vobu_s_ptm, n->vobu_e_ptm };
        vobu_insert(vobu_recs(d, f), &r);
    } else {
        av_log(d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: the VOBU at %"PRIu32" does not fit a record\n",
               f->index, sector);
    }
    return 1;
}

/* ---- the title VOBs' place ---- */

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

/* ---- VOBU starts ---- */

static int map_lower(const DVDIfo *f, uint32_t sector)
{
    int lo = 0, hi = f->nb_vobu_map;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (f->vobu_map[mid] < sector)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int map_has(const DVDIfo *f, uint32_t sector)
{
    int at = map_lower(f, sector);
    return at < f->nb_vobu_map && f->vobu_map[at] == sector;
}

/* the VOBU after the one at sector by the map */
static int map_next(const DVDIfo *f, uint32_t sector, uint32_t *next)
{
    int at;

    if (f->vobu_map_distrusted)
        return 0;
    at = map_lower(f, sector);
    if (at == f->nb_vobu_map || f->vobu_map[at] != sector || at + 1 == f->nb_vobu_map)
        return 0;
    *next = f->vobu_map[at + 1];
    return 1;
}

int ff_dvd_map_find(const DVDIfo *f, uint32_t sector, int *exact)
{
    int at = map_lower(f, sector);
    *exact = at < f->nb_vobu_map && f->vobu_map[at] == sector;
    return at;
}

/* sector starts a VOBU by its NAV pack (recorded, else read) */
static int start_by_nav(DVDDisc *d, DVDIfo *f, uint32_t sector)
{
    const DVDVobuRec *r = ff_dvd_vobu_find(d, f, sector, NULL);
    DVDNav n;
    uint32_t next, len;

    if (r)
        return r->next != DVD_VOBU_NO_NAV;
    if (!ff_dvd_nav_read(d, f, sector, &n))
        return 0;
    ff_dvd_vobu_register(d, f, &n, sector, 1, &next, &len);
    return 1;
}

/* the other title sets whose title VOBs hold f's (same place, or around it) */
static int shares_title_vobs(const DVDIfo *f, const DVDIfo *o)
{
    return o && o != f && !o->vobu_map_distrusted && o->title_vob_base &&
           (o->title_vob_base == f->title_vob_base ||
            (o->title_vob_base <= f->title_vob_base &&
             f->title_vob_base + f->title_vob_blocks <= o->title_vob_base + o->title_vob_blocks));
}

int ff_dvd_vobu_start_check(DVDDisc *d, DVDIfo *f, uint32_t sector)
{
    if (!f->vobu_map_distrusted) {
        if (map_has(f, sector))
            return 1;
        av_log(d->log, AV_LOG_WARNING, "DVD: VTS_%02d: sector %"PRIu32" starts no VOBU of the VOBU address map: "
               "its VOBU starts are not trusted (the VOBs must be scanned)\n", f->index, sector);
        f->vobu_map_distrusted = 1;
        return start_by_nav(d, f, sector);
    }
    if (f->title_vob_base) {
        /* title sets sharing these title VOBs: two or more maps agreeing are enough
         * (the sector is asked as it is, relative to each title set's own VOBs) */
        int hits = 0;
        for (int k = 1; k < d->nb_ifos; k++) {
            const DVDIfo *o = d->ifos[k];
            if (!shares_title_vobs(f, o))
                continue;
            if (!map_has(o, sector))
                return start_by_nav(d, f, sector);
            hits++;
        }
        if (hits >= 2)
            return 1;
    }
    return start_by_nav(d, f, sector);
}

int ff_dvd_vobu_next(DVDDisc *d, DVDIfo *f, uint32_t sector, uint32_t *next)
{
    const DVDVobuRec *r;
    DVDNav n, n2;
    uint32_t nx, nl, after;

    if (!f->vobu_map_distrusted)
        return map_next(f, sector, next);
    if (f->title_vob_base) {
        /* the maps of title sets sharing these title VOBs, when two or more agree */
        int hits = 0, agree = 1;
        uint32_t cand = 0;
        for (int k = 1; k < d->nb_ifos && agree; k++) {
            const DVDIfo *o = d->ifos[k];
            uint32_t on;
            if (!shares_title_vobs(f, o))
                continue;
            if (!map_next(o, sector - o->title_vob_base + f->title_vob_base, &on)) {
                agree = 0;
                break;
            }
            on = on + o->title_vob_base - f->title_vob_base;
            if (hits && on != cand)
                agree = 0;
            cand = on;
            hits++;
        }
        if (agree && hits >= 2) {
            *next = cand;
            return 1;
        }
    }
    /* the NAV packs: the record of sector, else sector and the VOBU after it read */
    if ((r = ff_dvd_vobu_find(d, f, sector, NULL))) {
        if (r->next == DVD_VOBU_NO_NAV)
            return 0;
        *next = sector + 1 + r->vobu_ea;
        return 1;
    }
    if (!ff_dvd_nav_read(d, f, sector, &n))
        return 0;
    after = sector + n.vobu_ea + 1;
    if (!ff_dvd_nav_read(d, f, after, &n2))
        return 0;
    *next = after;
    ff_dvd_vobu_register(d, f, &n, sector, 1, &nx, &nl);
    ff_dvd_vobu_register(d, f, &n2, after, 1, &nx, &nl);
    return 1;
}

int ff_dvd_vobu_bad(DVDDisc *d, DVDIfo *f, uint32_t sector)
{
    const DVDVobuRec *r;
    uint8_t b[DVD_SECTOR];
    DVDNav n;
    uint32_t next, len;

    if (sector >= f->title_vob_blocks)
        return 1;
    if ((r = ff_dvd_vobu_find(d, f, sector, NULL)))
        return r->next == DVD_VOBU_NO_NAV;
    if (ff_dvd_vob_read(d, f, 0, sector, b) < 0 || !ff_dvd_is_nav_pack(b)) {
        ff_dvd_vobu_mark_bad(d, f, sector);
        return 1;
    }
    ff_dvd_nav_parse(&n, b);
    ff_dvd_vobu_register(d, f, &n, sector, 1, &next, &len);
    return 0;
}
