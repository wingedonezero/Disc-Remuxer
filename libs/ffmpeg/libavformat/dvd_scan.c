/*
 * DVD-Video: the navigation scan. The disc's navigation (libdvdnav, opened
 * on the disc layer by dvd_nav.c) is played the way a viewer would play it:
 * per region the disc allows, from the disc's start and from every title,
 * every menu button reachable from a menu's starting button pressed, and the
 * cells each path plays recorded per title and program chain. The result
 * names the recordings ("A1", ...), orders the titles and gives each title
 * its cells.
 *
 * Navigators. Index 0 is the navigator opened on the disc (no read-ahead,
 * navigation-only reading, title play following the title's pre-command
 * jumps, the C library's rand() sequence for the Rnd operation); every
 * other one is a copy of an earlier one (dvdnav_dup), kept to the end.
 * Stepping one (run) returns its next event; finite stills and waits are
 * skipped right away (the still's length advances the playback clock), an
 * endless still is returned again until a button is pressed.
 *
 * Reads. Every VOB sector the navigators read is looked at once (later
 * reads of it come from the first): a NAV pack of a menu VOB, or of title
 * VOBs with highlight information, keeps its PCI (the menus' buttons); a
 * title-VOB sector of a title set records its VOBU (NAV pack: length,
 * interleaved unit end, next VOBU, presentation times; else "no NAV pack").
 *
 * Paths. A stack of navigator states, each with a key (region, menu /
 * title set, NAV pack sector or start kind, button or title): a state is
 * played until it stops, reaches an endless still, a menu it has explored
 * before, 5 hours of playback time or 4096 cell changes. While it plays,
 * the cells of each title / program chain are collected (an angle block
 * counts whole); a menu whose buttons have been highlighted for 2 seconds
 * after 8.2 seconds of playback, or an endless still with buttons, has its
 * reachable buttons pressed, each in a copy pushed on the stack.
 * Collected cells: a sequence that runs through 1..n from any cell is
 * turned to 1..n; a stretch repeated at the start or end is kept once.
 *
 * Results. The recordings are sorted by title, program chain, length
 * (longest first), then by how many steps from a start their path lies (a
 * title start counts 1000) and by key; the first of each title / program
 * chain is kept. A region pass after the first one runs only when the
 * navigation read the region register (SPRM 20). Any failure fails the
 * scan.
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

#include <dvdnav/dvdnav.h>

#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"

#define PATH_TIME_LIMIT   18000000      /* ms of playback per path (5 hours) */
#define PATH_CELL_CHANGES 0x1000        /* cell changes per path */
#define MENU_PLAY_MS      8200          /* playback before menus are explored / a title is snapshot (more than) */
#define HLI_SHOWN_TICKS   0x2bec5       /* 90 kHz ticks buttons must show for (more than; just under 2 s) */
#define MAX_BUTTONS       36
#define KEY_REGION_ROOT   0xffffffffu   /* key Z of a region's start */
#define KEY_TITLE_START   0xfffffffeu   /* key Z of a title start */
#define KEY_MENU_SEEN     0x63          /* key X of an explored menu highlight */
#define REGION_SPRM       20
#define VMG_CATEGORY      0x22          /* VMGI_MAT vmg_category */

/* navigator parameters (dvdnav_set_* / play functions) */
enum { P_REGION, P_MENU_LANG, P_AUDIO_LANG, P_SPU_LANG, P_ANGLE, P_BUTTON, P_TITLE, P_PROGRAM, P_HIGHLIGHT };

/* ---- keys: X (bits 56-63) | region (48-55) | Y (32-47) | Z (0-31) ---- */

#define K_X(k)  ((unsigned)((k) >> 56))
#define K_R(k)  ((unsigned)((k) >> 48) & 0xff)
#define K_Y(k)  ((unsigned)((k) >> 32) & 0xffff)
#define K_Z(k)  ((uint32_t)(k))

static uint64_t key_of(unsigned x, unsigned r, unsigned y, uint32_t z)
{
    return (uint64_t)x << 56 | (uint64_t)(r & 0xff) << 48 | (uint64_t)(y & 0xffff) << 32 | z;
}

/* the order of keys: region, Y, Z, X */
static int key_less(uint64_t a, uint64_t b)
{
    if (K_R(a) != K_R(b))
        return K_R(a) < K_R(b);
    if (K_Y(a) != K_Y(b))
        return K_Y(a) < K_Y(b);
    if (K_Z(a) != K_Z(b))
        return K_Z(a) < K_Z(b);
    return K_X(a) < K_X(b);
}

/* ---- the C library's rand() (glibc TYPE_3, seed 1): the Rnd operation ---- */

typedef struct Rand {
    int32_t r[34];
    int     i;                          /* next index into r (ring of 34) */
} Rand;

static void rand_init(Rand *g)
{
    int32_t r[344];

    r[0] = 1;
    for (int i = 1; i < 31; i++) {
        /* 16807 * r mod (2^31 - 1), as glibc computes it */
        int32_t hi = r[i - 1] / 127773, lo = r[i - 1] % 127773, w = 16807 * lo - 2836 * hi;
        r[i] = w < 0 ? w + 2147483647 : w;
    }
    for (int i = 31; i < 34; i++)
        r[i] = r[i - 31];
    for (int i = 34; i < 344; i++)
        r[i] = (int32_t)((uint32_t)r[i - 31] + (uint32_t)r[i - 3]);
    memcpy(g->r, r + 344 - 34, sizeof(g->r));
    g->i = 0;
}

static int rand_next(void *p)
{
    Rand *g = p;
    /* r[k] = r[k-31] + r[k-3]; the ring holds r[k-34 .. k-1] */
    int32_t v = (int32_t)((uint32_t)g->r[(g->i + 3) % 34] + (uint32_t)g->r[(g->i + 31) % 34]);
    g->r[g->i] = v;
    g->i = (g->i + 1) % 34;
    return (int)((uint32_t)v >> 1);
}

/* ---- what the reads keep ---- */

typedef struct Pci {
    int      id;                        /* (menu ? 0x100 : 0) | set */
    uint32_t lbn, vobu_s_ptm, vobu_e_ptm;
    uint32_t hli_s_ptm, hli_e_ptm, btn_se_e_ptm;
    uint8_t  hli_ss, btn_ns, fosl_btnn;
    uint8_t  links[MAX_BUTTONS][4];     /* up, down, left, right */
} Pci;

typedef struct Event {
    int      type;                      /* 1: an event, 0: an error */
    int      code;
    uint32_t time;                      /* playback clock, ms */
    uint32_t vobu;
    uint32_t sprm_flags;
    uint16_t title_id, pgc_id;
    uint8_t  vtsn, cell_id, still_time, highlight;
} Event;

typedef struct Pending { uint64_t key, parent; int nav; } Pending;
typedef struct Visited { uint64_t key, parent; } Visited;

typedef struct Seq { int len; uint8_t c[]; } Seq;

typedef struct Rec {
    Seq      *seq;
    uint64_t  key;
    uint8_t   title, pgc;
} Rec;

typedef struct Scan {
    DVDDisc   *d;
    void     (*trace)(void *opaque, const char *line);
    void      *opaque;
    int        dead;                    /* a navigator refused a command: no command goes through any more */
    Rand       rnd;

    dvdnav_t **navs;
    int        nb_navs;

    /* sectors already read: per (set, menu) relative, title VOBs with a base absolute */
    uint64_t  *seen;
    int        nb_seen, alloc_seen;
    Pci      **pcis;                    /* sorted by (id, lbn) */
    int        nb_pcis, alloc_pcis;

    /* the walk */
    int        root;                    /* the region passes' source navigator */
    int        snapshot;                /* navigator copy to start titles from (0: none) */
    unsigned   region;
    int        region_read;             /* a path read SPRM 20 */
    int        no_snapshot;
    unsigned   snap_title;
    unsigned   cur_title;               /* 16 bits */
    unsigned   cur_pgc;                 /* 8 bits */
    uint32_t   seq_start;               /* playback time the sequence started (0: not yet) */
    uint8_t   *cells;
    int        nb_cells, alloc_cells;
    Pending   *pending;
    int        nb_pending, alloc_pending;
    Visited   *visited;
    int        nb_visited, alloc_visited;
    Rec       *recs;
    int        nb_recs, alloc_recs;
    Seq      **seqs;                    /* interned sequences, sorted */
    int        nb_seqs, alloc_seqs;
} Scan;

static void tracef(Scan *s, const char *fmt, ...) av_printf_format(2, 3);
static void tracef(Scan *s, const char *fmt, ...)
{
    char line[256];
    va_list va;

    if (!s->trace)
        return;
    va_start(va, fmt);
    vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    s->trace(s->opaque, line);
}

static int grow(void **p, int *alloc, int n, size_t elem)
{
    if (n < *alloc)
        return 0;
    return av_reallocp_array(p, *alloc = FFMAX(16, *alloc * 2), elem);
}

/* ---- what a NAV pack says ---- */

/* The PCI fields the walk uses (pci_t: pci_gi, hli.hl_gi, hli.btnit). */
static void parse_pci(Pci *p, const uint8_t *b)
{
    memset(p, 0, sizeof(*p));
    p->lbn          = AV_RB32(b + 0x2d);
    p->vobu_s_ptm   = AV_RB32(b + 0x39);
    p->vobu_e_ptm   = AV_RB32(b + 0x3d);
    p->hli_ss       = b[0x8e] & 3;
    p->hli_s_ptm    = AV_RB32(b + 0x8f);
    p->hli_e_ptm    = AV_RB32(b + 0x93);
    p->btn_se_e_ptm = AV_RB32(b + 0x97);
    p->btn_ns       = b[0x9e];
    p->fosl_btnn    = b[0xa1];
    for (int i = 0; i < MAX_BUTTONS; i++) {
        const uint8_t *btn = b + 0xbb + 18 * i;
        for (int k = 0; k < 4; k++)
            p->links[i][k] = btn[6 + k] & 0x3f;
    }
}

static int pci_cmp(int id, uint32_t lbn, const Pci *p)
{
    if (id != p->id)
        return id < p->id ? -1 : 1;
    return lbn < p->lbn ? -1 : lbn > p->lbn;
}

static Pci *pci_find(Scan *s, int id, uint32_t lbn, int *at)
{
    int lo = 0, hi = s->nb_pcis;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (pci_cmp(id, lbn, s->pcis[mid]) > 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (at)
        *at = lo;
    return lo < s->nb_pcis && !pci_cmp(id, lbn, s->pcis[lo]) ? s->pcis[lo] : NULL;
}

/* A VOB sector read by a navigator: looked at the first time. */
static int vob_read(void *opaque, int set, int menu, uint32_t sector, const uint8_t *b)
{
    Scan *s = opaque;
    DVDIfo *f = set < s->d->nb_ifos ? s->d->ifos[set] : NULL;
    int id = (menu ? 0x100 : 0) | set, at, ret;
    uint64_t key;

    if (!f)
        return AVERROR(EINVAL);
    /* title VOBs with a base by absolute sector, the rest per file */
    key = !menu && set && f->title_vob_base ? (uint64_t)1 << 40 | (uint32_t)(f->title_vob_base + sector)
                                            : (uint64_t)id << 32 | sector;
    {
        int lo = 0, hi = s->nb_seen;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (s->seen[mid] < key)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < s->nb_seen && s->seen[lo] == key)
            return 0;
        if ((ret = grow((void **)&s->seen, &s->alloc_seen, s->nb_seen, sizeof(*s->seen))) < 0)
            return ret;
        memmove(s->seen + lo + 1, s->seen + lo, (s->nb_seen - lo) * sizeof(*s->seen));
        s->seen[lo] = key;
        s->nb_seen++;
    }
    tracef(s, "read %d %"PRIu32, id, sector);

    if (ff_dvd_is_nav_pack(b)) {
        Pci p;
        parse_pci(&p, b);
        if (p.lbn != sector) {
            av_log(s->d->log, AV_LOG_DEBUG, "DVD: %s %d: the NAV pack at sector %"PRIu32" names sector %"PRIu32"\n",
                   menu ? "menu VOB" : "title VOBs", set, sector, p.lbn);
        } else if ((menu || p.hli_ss) && !pci_find(s, id, p.lbn, &at)) {
            Pci *n = av_malloc(sizeof(*n));
            if (!n || (ret = grow((void **)&s->pcis, &s->alloc_pcis, s->nb_pcis, sizeof(*s->pcis))) < 0) {
                av_free(n);
                return AVERROR(ENOMEM);
            }
            *n = p;
            n->id = id;
            memmove(s->pcis + at + 1, s->pcis + at, (s->nb_pcis - at) * sizeof(*s->pcis));
            s->pcis[at] = n;
            s->nb_pcis++;
        }
    } else {
        av_log(s->d->log, AV_LOG_DEBUG, "DVD: %s %d: sector %"PRIu32" is no NAV pack\n",
               menu ? "menu VOB" : "title VOBs", set, sector);
    }
    if (!menu && set && (ret = ff_dvd_vobu_note(s->d, f, sector, b)) < 0)
        return ret;
    return 0;
}

/* ---- the navigators ---- */

static int nav_clone(Scan *s, int *out, int src)
{
    dvdnav_t *n;
    int ret;

    if (s->dead)
        return AVERROR_EXTERNAL;
    if (!s->nb_navs) {
        DVDNavHooks hooks = { s, vob_read };
        if ((ret = ff_dvd_nav_open(s->d, &hooks, &n)) < 0)
            return ret;
        dvdnav_set_readahead_flag(n, 0);
        dvdnav_set_nav_only_flag(n, 1);
        dvdnav_set_title_play_follows_jumps(n, 1);
        dvdnav_set_random_source(n, rand_next, &s->rnd);
        if ((ret = av_dynarray_add_nofree(&s->navs, &s->nb_navs, n)) < 0) {
            dvdnav_close(n);
            return ret;
        }
    }
    if (src < 0 || src >= s->nb_navs || dvdnav_dup(&n, s->navs[src]) != DVDNAV_STATUS_OK) {
        av_log(s->d->log, AV_LOG_ERROR, "DVD: the navigation scan cannot copy navigator %d\n", src);
        s->dead = 1;
        return AVERROR_EXTERNAL;
    }
    if ((ret = av_dynarray_add_nofree(&s->navs, &s->nb_navs, n)) < 0) {
        dvdnav_free_dup(n);
        return ret;
    }
    *out = s->nb_navs - 1;
    tracef(s, "nav %d", *out);
    return 0;
}

static int nav_set(Scan *s, int nav, int id, uint32_t value)
{
    dvdnav_t *n = s->navs[nav];
    char lang[5] = { value, value >> 8, value >> 16, value >> 24, 0 };
    dvdnav_status_t st;

    if (s->dead)
        return AVERROR_EXTERNAL;
    switch (id) {
    case P_REGION:     st = dvdnav_set_region_mask(n, value);                                  break;
    case P_MENU_LANG:  st = dvdnav_menu_language_select(n, lang);                              break;
    case P_AUDIO_LANG: st = dvdnav_audio_language_select(n, lang);                             break;
    case P_SPU_LANG:   st = dvdnav_spu_language_select(n, lang);                               break;
    case P_ANGLE:      st = dvdnav_angle_change(n, value);                                     break;
    case P_BUTTON:     st = dvdnav_button_select_and_activate(n, dvdnav_get_current_nav_pci(n), value); break;
    case P_TITLE:      st = dvdnav_title_play(n, value);                                       break;
    case P_PROGRAM:    st = dvdnav_program_play(n, value >> 16, (value >> 8) & 0xff, value & 0xff); break;
    case P_HIGHLIGHT:  st = dvdnav_button_select(n, dvdnav_get_current_nav_pci(n), value);     break;
    default:           st = DVDNAV_STATUS_ERR;
    }
    if (st != DVDNAV_STATUS_OK) {
        av_log(s->d->log, AV_LOG_ERROR, "DVD: the navigation scan: navigator %d refuses parameter %d = %"PRIu32"\n",
               nav, id, value);
        s->dead = 1;
        return AVERROR_EXTERNAL;
    }
    tracef(s, "set");
    return 0;
}

static void position(dvdnav_t *n, Event *ev)
{
    int32_t title, vtsn, pgcn, pgn, celln;
    pci_t *pci;

    if (dvdnav_current_title_program2(n, &title, &vtsn, &pgcn, &pgn, &celln) != DVDNAV_STATUS_OK) {
        ev->title_id = 0xffff;
    } else {
        ev->vtsn     = vtsn;
        ev->pgc_id   = pgcn;
        ev->cell_id  = celln;
        ev->title_id = title >= 0 ? title : 0xffff;
    }
    if ((pci = dvdnav_get_current_nav_pci(n)))
        ev->vobu = pci->pci_gi.nv_pck_lbn;
}

/* One step of a navigator: its next event. < 0 when a read failed hard. */
static int nav_run(Scan *s, int nav, Event *ev)
{
    dvdnav_t *n = s->navs[nav];
    uint8_t buf[DVD_SECTOR];
    int32_t code, len = DVD_SECTOR;
    dvdnav_status_t st;

    memset(ev, 0, sizeof(*ev));
    if (s->dead)
        return AVERROR_EXTERNAL;
    st = dvdnav_get_next_block(n, buf, &code, &len);
    ev->time       = (uint32_t)(dvdnav_get_absolute_time(n) / 90);
    ev->sprm_flags = dvdnav_get_prm(n, DVDNAV_PRM_SPRM_FLAGS, 0);
    ev->highlight  = (uint8_t)(dvdnav_get_prm(n, DVDNAV_PRM_SPRM, 8) >> 10);
    if (st != DVDNAV_STATUS_OK) {
        ev->type = 0;
        ev->code = st;
        tracef(s, "fail");
        return 0;
    }
    ev->type = 1;
    ev->code = code;
    switch (code) {
    case DVDNAV_STILL_FRAME:
        ev->still_time = (uint8_t)((dvdnav_still_event_t *)buf)->length;
        position(n, ev);
        if (ev->still_time != 0xff)
            dvdnav_still_skip(n);
        break;
    case DVDNAV_WAIT:
        dvdnav_wait_skip(n);
        break;
    case DVDNAV_NAV_PACKET:
    case DVDNAV_CELL_CHANGE:
        position(n, ev);
        break;
    }
    tracef(s, "ev %d %"PRIu32" %u %u %u %u %u %u %"PRIu32" %08"PRIx32, ev->code, ev->time, ev->title_id, ev->pgc_id,
           ev->vtsn, ev->cell_id, ev->still_time, ev->highlight, ev->vobu, ev->sprm_flags);
    return 0;
}

static void nav_log(Scan *s, const char *text)
{
    char esc[256];
    int k = 0;

    for (const char *p = text; *p && k < (int)sizeof(esc) - 3; p++) {
        if (*p == '\n') {
            esc[k++] = '\\';
            esc[k++] = 'n';
        } else {
            esc[k++] = *p;
        }
    }
    esc[k] = 0;
    tracef(s, "log %s", esc);
    av_log(s->d->log, AV_LOG_DEBUG, "DVD: navigation scan %s", text);
}

/* ---- the disc's tables ---- */

/* title (1-based) of the title table: its title set, VTS title and pb_ty & 3 */
static int tt_lookup(const DVDDisc *d, unsigned title, unsigned *vtsn, unsigned *ttn, unsigned *uop)
{
    uint32_t sz = d->tt_srpt.size, o = 12 * title;

    if (sz < 2 || title - 1 >= AV_RB16(d->tt_srpt.data))
        return 0;
    *vtsn = o + 3 <= sz ? d->tt_srpt.data[o + 2] : 0;
    *ttn  = o + 4 <= sz ? d->tt_srpt.data[o + 3] : 0;
    *uop  = o - 3 <= sz ? d->tt_srpt.data[o - 4] & 3 : 0;
    return 1;
}

static unsigned nr_of_titles(const DVDDisc *d)
{
    return d->tt_srpt.size >= 2 ? AV_RB16(d->tt_srpt.data) : 0;
}

static const DVDPgc *pgc_of(const DVDDisc *d, unsigned vtsn, unsigned pgcn)
{
    const DVDIfo *f = vtsn < (unsigned)d->nb_ifos ? d->ifos[vtsn] : NULL;

    if (!f || !pgcn || pgcn > (unsigned)f->nb_pgcs)
        return NULL;
    return f->pgcs[pgcn - 1];
}

/* ---- the walk's bookkeeping ---- */

static int visited_find(const Scan *s, uint64_t key)
{
    int lo = 0, hi = s->nb_visited;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (key_less(s->visited[mid].key, key))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int visited_add(Scan *s, uint64_t key, uint64_t parent)
{
    int at = visited_find(s, key), ret;

    if (at < s->nb_visited && !key_less(key, s->visited[at].key))
        return 0;
    if ((ret = grow((void **)&s->visited, &s->alloc_visited, s->nb_visited, sizeof(*s->visited))) < 0)
        return ret;
    memmove(s->visited + at + 1, s->visited + at, (s->nb_visited - at) * sizeof(*s->visited));
    s->visited[at] = (Visited){ key, parent };
    s->nb_visited++;
    return 0;
}

static const Visited *visited_get(const Scan *s, uint64_t key)
{
    int at = visited_find(s, key);
    return at < s->nb_visited && !key_less(key, s->visited[at].key) ? &s->visited[at] : NULL;
}

/* a state walked before or waiting to be */
static int known(const Scan *s, uint64_t key)
{
    if (visited_get(s, key))
        return 1;
    for (int i = 0; i < s->nb_pending; i++)
        if (s->pending[i].key == key)
            return 1;
    return 0;
}

static int push(Scan *s, uint64_t key, uint64_t parent, int nav)
{
    int ret = grow((void **)&s->pending, &s->alloc_pending, s->nb_pending, sizeof(*s->pending));
    if (ret < 0)
        return ret;
    s->pending[s->nb_pending++] = (Pending){ key, parent, nav };
    return 0;
}

static void log_key(Scan *s, const char *kind, uint64_t key)
{
    char text[64];
    snprintf(text, sizeof(text), "%s%1u:%02u:%08"PRIX32":%02u\n", kind, K_R(key), K_Y(key), K_Z(key), K_X(key));
    nav_log(s, text);
}

static int add_cell(Scan *s, unsigned c)
{
    int ret = grow((void **)&s->cells, &s->alloc_cells, s->nb_cells, 1);
    if (ret < 0)
        return ret;
    s->cells[s->nb_cells++] = c;
    return 0;
}

static int seq_less(const Seq *a, const Seq *b)
{
    if (a->len != b->len)
        return a->len < b->len;
    return memcmp(a->c, b->c, a->len) < 0;
}

/* The collected cells of the title / program chain played so far become a
 * recording of the path key. */
static int flush(Scan *s, uint64_t key)
{
    uint8_t *c = s->cells;
    int n = s->nb_cells, mx = 0, mn = 0x100, lo, hi, ret;
    Seq *q;

    s->seq_start = 0;
    if (!n) {
        s->cur_title = 0;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        mx = FFMAX(mx, c[i]);
        mn = FFMIN(mn, c[i]);
    }
    /* a run through 1..mx from any cell: turned to 1..mx */
    if (mn == 1 && mx <= n) {
        unsigned d = c[0] - 1;
        int cyc = 1;
        for (int i = 0; i < n && cyc; i++)
            cyc = (int)((d + i) % mx) + 1 == c[i];
        if (cyc && !(mx == n && c[0] == 1)) {
            unsigned k = mx - d;
            if (k + mx > (unsigned)n) {
                uint8_t tmp[256];
                n = mx;                                     /* the first mx, rotated left by k */
                k = FFMAX(k, 1) % mx;
                memcpy(tmp, c, n);
                for (int i = 0; i < n; i++)
                    c[i] = tmp[(i + k) % n];
            } else {
                memmove(c, c + k, mx);                       /* cells k .. k+mx-1 */
                n = mx;
            }
        }
    }
    /* a stretch repeated at the start or the end is kept once */
    for (int p = 1; p <= n / 2; p++) {
        while (n >= 2 * p && !memcmp(c, c + p, p)) {
            memmove(c, c + p, n - p);
            n -= p;
        }
        while (n >= 2 * p && !memcmp(c + n - p, c + n - 2 * p, p))
            n -= p;
    }
    if (!(q = av_malloc(sizeof(*q) + n)))
        return AVERROR(ENOMEM);
    q->len = n;
    memcpy(q->c, c, n);
    /* interned */
    for (lo = 0, hi = s->nb_seqs; lo < hi; ) {
        int mid = (lo + hi) / 2;
        if (seq_less(s->seqs[mid], q))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < s->nb_seqs && !seq_less(q, s->seqs[lo])) {
        av_free(q);
        q = s->seqs[lo];
    } else {
        if ((ret = grow((void **)&s->seqs, &s->alloc_seqs, s->nb_seqs, sizeof(*s->seqs))) < 0) {
            av_free(q);
            return ret;
        }
        memmove(s->seqs + lo + 1, s->seqs + lo, (s->nb_seqs - lo) * sizeof(*s->seqs));
        s->seqs[lo] = q;
        s->nb_seqs++;
    }
    if ((ret = grow((void **)&s->recs, &s->alloc_recs, s->nb_recs, sizeof(*s->recs))) < 0)
        return ret;
    s->recs[s->nb_recs++] = (Rec){ q, key, (uint8_t)s->cur_title, (uint8_t)s->cur_pgc };
    s->cur_title = 0;
    s->nb_cells  = 0;
    return 0;
}

/* buttons reachable from button b (1-based) by the arrow links */
static void reachable(uint64_t *mask, const Pci *h, unsigned b)
{
    while (b) {
        if (b > MAX_BUTTONS || b > h->btn_ns)
            return;
        b--;
        if (*mask >> b & 1)
            return;
        *mask |= (uint64_t)1 << b;
        reachable(mask, h, h->links[b][0]);
        reachable(mask, h, h->links[b][1]);
        reachable(mask, h, h->links[b][2]);
        b = h->links[b][3];
    }
}

/* Press every button of highlight h reachable from its starting button, each
 * in a copy of navigator ctx, pushed as a state below key. */
static int explore_buttons(Scan *s, int ctx, uint64_t parent, const Event *ev, const Pci *h)
{
    unsigned n = h->btn_ns, start = h->fosl_btnn;
    uint64_t mask = 0;
    int ret;

    if (!n)
        return 0;
    if ((uint8_t)(start - 1) >= n) {
        start = ev->highlight ? ev->highlight : 1;
        if (start >= n)
            start = n;
    }
    reachable(&mask, h, start);
    for (unsigned b = n; b >= 1; b--) {
        uint64_t key;
        int nc;
        if (!(mask >> (b - 1) & 1))
            continue;
        key = key_of(b - 1, s->region, (ev->title_id == 0) << 8 | ev->vtsn, ev->vobu);
        if (known(s, key))
            continue;
        if ((ret = nav_clone(s, &nc, ctx)) < 0 || (ret = nav_set(s, nc, P_BUTTON, b)) < 0)
            return ret;
        log_key(s, "AP: ", key);
        if ((ret = push(s, key, parent, nc)) < 0)
            return ret;
    }
    return 0;
}

static int hli_active(const Pci *rec, const Pci *h)
{
    return h->vobu_s_ptm <= rec->vobu_s_ptm && h->vobu_s_ptm <= rec->vobu_e_ptm &&
           h->hli_s_ptm <= rec->vobu_s_ptm && rec->vobu_s_ptm <= h->hli_e_ptm &&
           h->btn_ns && rec->vobu_s_ptm <= h->btn_se_e_ptm - 1;
}

static int same_hli(const Pci *h, const Pci *x)
{
    return h == x || (h->btn_ns == x->btn_ns && h->hli_s_ptm == x->hli_s_ptm && h->hli_e_ptm == x->hli_e_ptm &&
                      h->btn_se_e_ptm == x->btn_se_e_ptm);
}

/* The cells a cell change into cell c of a title's program chain adds:
 * c, or its whole angle block. */
static int add_title_cells(Scan *s, unsigned title, unsigned pgcn, unsigned c)
{
    unsigned vtsn, ttn, uop, lo = c, hi = c;
    const DVDPgc *p;
    int ret;

    if (tt_lookup(s->d, title, &vtsn, &ttn, &uop) && pgcn && (p = pgc_of(s->d, vtsn, pgcn)) &&
        c - 1 < p->nr_of_cells && p->cells) {
        unsigned first = c - 1, last = c - 1, f = p->cells[c - 1].category[0];
        if ((f >> 4 & 3) == 1) {        /* an angle block: back to its first cell */
            int ok = 1;
            while ((f & 0xc0) != 0x40) {
                if (f < 0x40 || (f & 0x30) != 0x10 || first == 0) {
                    ok = 0;
                    break;
                }
                first--;
                f = p->cells[first].category[0];
            }
            if (ok) {
                unsigned g = p->cells[last].category[0];
                for (;;) {
                    if (g >> 6 == 3) {
                        lo = first + 1;
                        hi = last + 1;
                        break;
                    }
                    if ((g & 0x30) != 0x10 || (g >> 6) - 1 > 1 || last + 1 >= p->nr_of_cells)
                        break;
                    last++;
                    g = p->cells[last].category[0];
                }
            }
        }
    }
    for (unsigned k = lo; k <= hi; k++)
        if ((ret = add_cell(s, k)) < 0)
            return ret;
    return 0;
}

/* Walk every pending state. 0, or < 0 when the scan failed. */
static int walk(Scan *s)
{
    int ret;

    while (s->nb_pending) {
        Pending st = s->pending[--s->nb_pending];
        uint64_t key = st.key;
        Event ev;
        int first = 1, pending_cell = 0, ok = 0;
        unsigned cellchg = 0, grp = 0;
        uint32_t limit = 0;
        const Pci *lastrec = NULL, *hli = NULL, *explored = NULL;
        Pci local;
        uint32_t *menu_vobus = NULL;
        int nb_menu_vobus = 0, alloc_menu_vobus = 0;
        unsigned c_title = 0, c_pgc = 0, c_vtsn = 0, c_cell = 0;

        if (visited_get(s, key))
            continue;
        if ((ret = visited_add(s, key, st.parent)) < 0)
            return ret;
        s->cur_title = 0;
        s->cur_pgc   = 0;
        s->nb_cells  = 0;
        log_key(s, "\n\n\nRP: ", key);
        if ((ret = nav_run(s, st.nav, &ev)) < 0)
            goto fail;
        for (;;) {
            if (ev.type != 1) {
                av_log(s->d->log, AV_LOG_ERROR, "DVD: the navigation scan: navigator %d failed (status %d)\n",
                       st.nav, ev.code);
                goto fail;
            }
            if (first) {
                first = 0;
                limit = ev.time + PATH_TIME_LIMIT;
            }
            if (limit < ev.time)
                goto finish;
            switch (ev.code) {
            case DVDNAV_BLOCK_OK:
                if (pending_cell) {
                    if (s->cur_title != c_title || c_pgc != s->cur_pgc) {
                        if ((ret = flush(s, key)) < 0)
                            goto fail;
                        s->cur_title = c_title;
                        s->cur_pgc   = (uint8_t)c_pgc;
                    }
                    if (c_title != 0 && c_title != 0xffff &&
                        (ret = add_title_cells(s, c_title, c_pgc, c_cell)) < 0)
                        goto fail;
                    if (!s->seq_start)
                        s->seq_start = ev.time;
                    if (c_vtsn < (unsigned)s->d->nb_ifos && s->d->ifos[c_vtsn])
                        s->d->ifos[c_vtsn]->entered_by_navigation = 1;
                }
                pending_cell = 0;
                break;
            case DVDNAV_STILL_FRAME:
                if (lastrec && lastrec->lbn != ev.vobu)
                    av_log(s->d->log, AV_LOG_DEBUG, "DVD: navigation scan: still at %"PRIu32" after the NAV pack "
                           "at %"PRIu32"\n", ev.vobu, lastrec->lbn);
                if (ev.still_time != 0xff)
                    break;
                if (lastrec && hli && hli_active(lastrec, hli) && !(explored && same_hli(hli, explored)) &&
                    (ret = explore_buttons(s, st.nav, key, &ev, hli)) < 0)
                    goto fail;
                goto finish;
            case DVDNAV_VTS_CHANGE:
                if ((ret = flush(s, key)) < 0)
                    goto fail;
                hli = NULL;
                break;
            case DVDNAV_CELL_CHANGE:
                if (++cellchg > PATH_CELL_CHANGES)
                    goto finish;
                c_title = ev.title_id;
                c_pgc   = ev.pgc_id;
                c_vtsn  = ev.vtsn;
                c_cell  = ev.cell_id;
                pending_cell = 1;
                break;
            case DVDNAV_NAV_PACKET: {
                uint32_t dt = s->seq_start ? ev.time - s->seq_start : 0;
                int menu = ev.title_id == 0;
                unsigned g, vtsn2, ttn, uop;
                const Pci *rec, *same, *cand;
                Pci *hit;

                if (!menu && dt > MENU_PLAY_MS && ev.title_id != 0xffff && !s->snapshot && !s->no_snapshot &&
                    tt_lookup(s->d, ev.title_id, &vtsn2, &ttn, &uop)) {
                    const DVDPgc *p;
                    if (vtsn2 != ev.vtsn)
                        av_log(s->d->log, AV_LOG_DEBUG, "DVD: navigation scan: title %u is in title set %u by the "
                               "title table, %u by the navigation\n", ev.title_id, vtsn2, ev.vtsn);
                    if (!(uop & 4) && (p = pgc_of(s->d, vtsn2, ev.pgc_id)) && !(p->prohibited_ops & 4)) {
                        if ((ret = nav_clone(s, &s->snapshot, st.nav)) < 0)
                            goto fail;
                        s->snap_title = ev.title_id;
                    }
                }
                g = (unsigned)ev.pgc_id << 16 | (menu ? 0x100u : 0) | ev.vtsn;
                same = g == grp ? hli : NULL;
                grp  = g;
                hit  = pci_find(s, (menu ? 0x100 : 0) | ev.vtsn, ev.vobu, NULL);
                rec  = hit;
                if (!rec && !menu && ev.vtsn && ev.vtsn < s->d->nb_ifos && s->d->ifos[ev.vtsn]) {
                    const DVDVobuRec *v = ff_dvd_vobu_find(s->d, s->d->ifos[ev.vtsn], ev.vobu, NULL);
                    if (v) {
                        memset(&local, 0, sizeof(local));
                        local.lbn        = ev.vobu;
                        local.vobu_s_ptm = v->vobu_s_ptm;
                        local.vobu_e_ptm = v->vobu_e_ptm;
                        rec = &local;
                    }
                }
                if (!rec) {
                    lastrec = NULL;
                    hli = same;
                    break;
                }
                lastrec = rec;
                cand = !rec->hli_ss ? NULL : rec->hli_ss == 1 ? rec : same;
                if (cand && cand->btn_ns) {
                    unsigned b = cand->fosl_btnn;
                    if (b > cand->btn_ns)
                        b = 0;
                    else if (!b)
                        b = ev.highlight > cand->btn_ns ? cand->btn_ns : 0;
                    if (b && nav_set(s, st.nav, P_HIGHLIGHT, b) < 0)
                        av_log(s->d->log, AV_LOG_DEBUG, "DVD: navigation scan: button %u cannot be "
                               "highlighted\n", b);
                }
                if (explored) {
                    /* back at a menu highlight this path explored: a loop */
                    int lo = 0, hi2 = nb_menu_vobus;
                    while (lo < hi2) {
                        int mid = (lo + hi2) / 2;
                        if (menu_vobus[mid] < ev.vobu)
                            lo = mid + 1;
                        else
                            hi2 = mid;
                    }
                    if (lo < nb_menu_vobus && menu_vobus[lo] == ev.vobu &&
                        known(s, key_of(KEY_MENU_SEEN, s->region, menu << 8 | ev.vtsn, ev.vobu)))
                        goto finish;
                }
                if (!cand) {
                    hli = NULL;
                    break;
                }
                hli = cand;
                if (hli_active(rec, cand) && rec->vobu_e_ptm - cand->hli_s_ptm > HLI_SHOWN_TICKS &&
                    dt > MENU_PLAY_MS && (!explored || !same_hli(cand, explored))) {
                    uint64_t k = key_of(KEY_MENU_SEEN, s->region, menu << 8 | ev.vtsn, ev.vobu);
                    int lo = 0, hi2 = nb_menu_vobus;
                    if (known(s, k))
                        goto finish;
                    if ((ret = visited_add(s, k, key)) < 0)
                        goto fail;
                    while (lo < hi2) {
                        int mid = (lo + hi2) / 2;
                        if (menu_vobus[mid] < ev.vobu)
                            lo = mid + 1;
                        else
                            hi2 = mid;
                    }
                    if (!(lo < nb_menu_vobus && menu_vobus[lo] == ev.vobu)) {
                        if ((ret = grow((void **)&menu_vobus, &alloc_menu_vobus, nb_menu_vobus,
                                        sizeof(*menu_vobus))) < 0)
                            goto fail;
                        memmove(menu_vobus + lo + 1, menu_vobus + lo, (nb_menu_vobus - lo) * sizeof(*menu_vobus));
                        menu_vobus[lo] = ev.vobu;
                        nb_menu_vobus++;
                    }
                    if ((ret = explore_buttons(s, st.nav, key, &ev, cand)) < 0)
                        goto fail;
                    explored = cand;
                }
                break;
            }
            case DVDNAV_STOP:
                if (!s->snapshot) {
                    if ((ret = nav_clone(s, &s->snapshot, st.nav)) < 0)
                        goto fail;
                    s->snap_title = 0;
                }
                goto finish;
            case DVDNAV_BLOCK_OK + 1:   /* NOP */
            case DVDNAV_SPU_STREAM_CHANGE:
            case DVDNAV_AUDIO_STREAM_CHANGE:
            case DVDNAV_HIGHLIGHT:
            case DVDNAV_SPU_CLUT_CHANGE:
            case DVDNAV_HOP_CHANNEL:
            case DVDNAV_WAIT:
                break;
            default:
                av_log(s->d->log, AV_LOG_ERROR, "DVD: the navigation scan: unexpected event %d\n", ev.code);
                goto fail;
            }
            if ((ret = nav_run(s, st.nav, &ev)) < 0)
                goto fail;
        }
finish:
        if ((ret = flush(s, key)) < 0)
            goto fail;
        ok = 1;
        if (ev.sprm_flags & 1u << REGION_SPRM)
            s->region_read = 1;
fail:
        av_free(menu_vobus);
        if (!ok) {
            av_log(s->d->log, AV_LOG_ERROR, "DVD: the navigation scan failed on path %u:%02u:%08"PRIX32":%02u\n",
                   K_R(key), K_Y(key), K_Z(key), K_X(key));
            return ret < 0 ? ret : AVERROR_EXTERNAL;
        }
    }
    return 0;
}

/* One region's pass: the disc's start, then every title. */
static int region_pass(Scan *s, unsigned r)
{
    uint64_t k0 = key_of(0, r, 0, KEY_REGION_ROOT);
    int nav, ret;

    if ((ret = nav_clone(s, &nav, s->root)) < 0 || (ret = nav_set(s, nav, P_REGION, 1u << r)) < 0)
        return ret;
    s->region      = r;
    s->snapshot    = 0;
    s->snap_title  = 0;
    s->seq_start   = 0;
    s->region_read = 0;
    for (int p = P_MENU_LANG; p <= P_SPU_LANG; p++)
        if ((ret = nav_set(s, nav, p, 'e' | 'n' << 8)) < 0)
            return ret;
    if (!known(s, k0)) {
        log_key(s, "AP: ", k0);
        if ((ret = push(s, k0, k0, nav)) < 0)
            return ret;
    }
    if ((ret = walk(s)) < 0)
        return ret;
    if (!s->snapshot) {
        av_log(s->d->log, AV_LOG_VERBOSE, "DVD: navigation scan, region %u: the navigation never stopped or played "
               "a title long enough: titles are not started\n", r + 1);
        return 0;
    }
    for (unsigned t = 1; t - 1 < nr_of_titles(s->d); t++) {
        uint64_t kt = key_of(t - 1, r, 0, KEY_TITLE_START);
        int nt;
        if (t == s->snap_title)
            continue;
        if ((ret = nav_clone(s, &nt, s->snapshot)) < 0 || (ret = nav_set(s, nt, P_TITLE, t)) < 0)
            return ret;
        if (!known(s, kt)) {
            log_key(s, "AP: ", kt);
            if ((ret = push(s, kt, key_of(0, r, 0, KEY_REGION_ROOT), nt)) < 0)
                return ret;
        }
    }
    return walk(s);
}

/* how many steps from a start a state lies (a title start counts 1000) */
static unsigned depth(const Scan *s, uint64_t k)
{
    unsigned n = 0;
    const Visited *v;

    if (K_Z(k) == KEY_REGION_ROOT)
        return 0;
    for (;;) {
        if (K_Z(k) == KEY_TITLE_START)
            n += 1000;
        if (!(v = visited_get(s, k)))
            return n;
        k = v->parent;
        n++;
        if (K_Z(k) == KEY_REGION_ROOT)
            return n;
    }
}

static const Scan *sort_scan;

static int rec_cmp(const void *pa, const void *pb)
{
    const Rec *a = pa, *b = pb;
    unsigned da, db;

    if (a->title != b->title)
        return a->title < b->title ? -1 : 1;
    if (a->pgc != b->pgc)
        return a->pgc < b->pgc ? -1 : 1;
    if (a->seq->len != b->seq->len)
        return a->seq->len > b->seq->len ? -1 : 1;
    if (a->key == b->key) {
        int c = memcmp(a->seq->c, b->seq->c, a->seq->len);
        return (c > 0) - (c < 0);
    }
    da = depth(sort_scan, a->key);
    db = depth(sort_scan, b->key);
    if (da != db)
        return da < db ? -1 : 1;
    return key_less(a->key, b->key) ? -1 : key_less(b->key, a->key);
}

static int results(Scan *s, DVDScan *out)
{
    uint64_t *keys = NULL;
    int nb_keys = 0, alloc_keys = 0, n = 0, ret = 0;
    unsigned gt = 0, gp = 0;

    /* sorted (a stable merge order is not needed: equal elements are equal) */
    sort_scan = s;
    qsort(s->recs, s->nb_recs, sizeof(*s->recs), rec_cmp);
    /* the first recording of each title / program chain; (0, 0) none */
    for (int i = 0; i < s->nb_recs; i++) {
        Rec *r = &s->recs[i];
        uint64_t k = r->key & 0x00ffffffffffffffULL;
        int lo = 0, hi = nb_keys;
        if (r->title == gt && r->pgc == gp)
            continue;
        gt = r->title;
        gp = r->pgc;
        s->recs[n++] = *r;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (key_less(keys[mid], k))
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < nb_keys && !key_less(k, keys[lo]))
            continue;
        if ((ret = grow((void **)&keys, &alloc_keys, nb_keys, sizeof(*keys))) < 0)
            goto end;
        memmove(keys + lo + 1, keys + lo, (nb_keys - lo) * sizeof(*keys));
        keys[lo] = k;
        nb_keys++;
    }
    if (!(out->results = av_calloc(FFMAX(n, 1), sizeof(*out->results)))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < n; i++) {
        const Rec *r = &s->recs[i];
        DVDScanResult *o = &out->results[i];
        uint64_t k = r->key & 0x00ffffffffffffffULL;
        unsigned idx = nb_keys;
        char *p = o->name;
        for (int j = 0; j < nb_keys; j++)
            if (keys[j] == k) {
                idx = j;
                break;
            }
        /* letters, least significant first, then the key's X + 1 */
        do {
            *p++ = 'A' + idx % 26;
        } while ((idx /= 26) && p < o->name + 8);
        snprintf(p, o->name + sizeof(o->name) - p, "%u", K_X(r->key) + 1);
        o->title    = r->title;
        o->pgc      = r->pgc;
        o->nb_cells = r->seq->len;
        if (!(o->cells = av_malloc(FFMAX(r->seq->len, 1)))) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        memcpy(o->cells, r->seq->c, r->seq->len);
        out->nb_results++;
    }
end:
    av_free(keys);
    return ret;
}

int ff_dvd_scan(DVDDisc *d, void (*trace)(void *opaque, const char *line), void *opaque, DVDScan *out)
{
    Scan s = { 0 };
    uint32_t prohibited;
    int ret, overlap = 0;
    unsigned titles = nr_of_titles(d);

    memset(out, 0, sizeof(*out));
    s.d      = d;
    s.trace  = trace;
    s.opaque = opaque;
    rand_init(&s.rnd);
    /* no title snapshot on discs with many titles (or title sets sharing title VOBs and 16 or more titles) */
    for (int v = 1; v < d->nr_of_title_sets && !overlap; v++)
        overlap = ff_dvd_title_vobs_overlap(d, v);
    s.no_snapshot = overlap ? titles >= 16 : titles > 96;

    if ((ret = nav_clone(&s, &s.root, 0)) < 0)
        goto end;
    prohibited = ff_dvd_header32(d->ifos[0], VMG_CATEGORY) >> 16 & 0xff;   /* regions the disc is not for */
    for (unsigned r = 0; r < 8; r++) {
        if (prohibited >> r & 1)
            continue;
        if ((ret = region_pass(&s, r)) < 0) {
            av_log(d->log, AV_LOG_ERROR, "DVD: the navigation scan failed in region %u\n", r + 1);
            goto end;
        }
        if (r < 7 && !s.region_read)
            break;
    }
    ret = results(&s, out);
end:
    if (ret < 0) {
        ff_dvd_scan_free(out);
        av_log(d->log, AV_LOG_WARNING, "DVD: the navigation scan failed: the titles keep the order of the title "
               "table\n");
    }
    for (int i = s.nb_navs - 1; i > 0; i--)
        dvdnav_free_dup(s.navs[i]);
    if (s.nb_navs)
        dvdnav_close(s.navs[0]);
    av_free(s.navs);
    av_free(s.seen);
    for (int i = 0; i < s.nb_pcis; i++)
        av_free(s.pcis[i]);
    av_free(s.pcis);
    av_free(s.cells);
    av_free(s.pending);
    av_free(s.visited);
    av_free(s.recs);
    for (int i = 0; i < s.nb_seqs; i++)
        av_free(s.seqs[i]);
    av_free(s.seqs);
    return ret;
}

void ff_dvd_scan_free(DVDScan *s)
{
    for (int i = 0; i < s->nb_results; i++)
        av_freep(&s->results[i].cells);
    av_freep(&s->results);
    s->nb_results = 0;
}
