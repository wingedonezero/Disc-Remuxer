/*
 * DVD-Video demuxer: the navigation scan
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
 * The navigation scan plays the disc's navigation the way a player would,
 * from every start point, and records which cells each title's program chain
 * actually plays.
 *
 * For each player region the disc allows (stopping after the first region
 * whose navigation never reads the region register, SPRM 20), the scan starts
 * at First Play with English language preferences, then starts every title of
 * the title search table from a snapshot taken in the title domain. Every path
 * is followed until it stops, loops, plays for five hours, changes cell 4096
 * times or reaches an infinite still. In menus, the buttons reachable from the
 * highlighted one are pressed, each on its own copy of the navigator, from the
 * highest button number down. Each path records the cell sequences it played
 * per title and program chain; the best sequence of each (title, PGC) is kept.
 *
 * The navigators (libdvdnav) run in navigation-only mode on files this scan
 * serves: the IFOs come from the disc source, every VOB sector goes through the
 * scan's read service, once per sector. A read the service refuses, or a
 * broken assumption a navigator records about the navigation data, fails the
 * whole scan. The VM's Rnd operation draws from one sequence per scan (glibc's
 * rand() sequence for seed 1), shared by all navigators.
 *
 * Positions are keyed by 64-bit values: byte 7 = button / title index (or 0x63
 * for a visited menu highlight), byte 6 = region index, bits 32..47 =
 * (menu << 8) | title set, bits 0..31 = NAV pack sector, with 0xffffffff = the
 * region's start and 0xfffffffe = a title start.
 */

#include <stdarg.h>
#include <stdio.h>

#include "libavutil/tree.h"

#include "dvdvideo_internal.h"

/* ---- glibc's rand() sequence ---- */

/*
 * glibc's default generator is the additive feedback generator of random(3)
 * with a 31-word state (TYPE_3): the state is filled from the seed by a
 * multiplicative congruential generator (multiplier 16807, modulus 2^31 - 1,
 * Schrage's method), the first 310 outputs are discarded, and each output is
 * r[i] = r[i-3] + r[i-31] (mod 2^32) shifted right by one bit.
 */
#define RAND_DEG    31
#define RAND_SEP    3

static uint32_t rand_step(DVDVideoRand *r)
{
    int rear = (r->front + RAND_DEG - RAND_SEP) % RAND_DEG;
    uint32_t sum = r->state[r->front] + r->state[rear];

    r->state[r->front] = sum;
    r->front = (r->front + 1) % RAND_DEG;
    return sum;
}

void ff_dvdvideo_rand_init(DVDVideoRand *r, uint32_t seed)
{
    int32_t word = seed ? (int32_t)seed : 1;

    r->state[0] = word;
    for (int i = 1; i < RAND_DEG; i++) {
        int32_t hi = word / 127773, lo = word % 127773;

        word = 16807 * lo - 2836 * hi;
        if (word < 0)
            word += 2147483647;
        r->state[i] = word;
    }
    r->front = RAND_SEP;
    for (int i = 0; i < 10 * RAND_DEG; i++)
        rand_step(r);
}

int ff_dvdvideo_rand_next(void *r)
{
    return rand_step(r) >> 1;
}

/* ---- limits of the walk ---- */

#define PATH_TIME_LIMIT_MS      18000000    /* five hours of playback */
#define PATH_CELL_CHANGES       0x1000
#define SETTLE_MS               8200        /* played before a snapshot / button press */
#define HIGHLIGHT_ACTIVE_TICKS  179910      /* 90 kHz, just under 2 s */
#define HIGHLIGHT_KEY           0x63ULL
#define NAV_READ_ATTEMPTS       3
#define SCAN_FILE_SIZE          (1ULL << 42)
#define MENU_VOB                0x100       /* VOB id bit: the menu VOB of the IFO */
#define KEY_IN_VOB              (1ULL << 48)    /* above every VOB id << 32 */

/* ---- NAV packs ---- */

/* The PCI fields the scan uses (libdvdread pci_t names) */
typedef struct ScanPCI {
    uint32_t nv_pck_lbn;
    uint32_t vobu_s_ptm;
    uint32_t vobu_e_ptm;
    uint32_t hli_s_ptm;
    uint32_t hli_e_ptm;
    uint32_t btn_se_e_ptm;
    uint8_t  hli_ss;
    uint8_t  btn_ns;
    uint8_t  fosl_btnn;
    uint8_t  up[36], down[36], left[36], right[36];
} ScanPCI;

static int is_nav_pack(const uint8_t *s)
{
    return AV_RB32(s) == 0x000001ba && (s[4] & 0xc0) == 0x40 && AV_RB32(s + 0x0e) == 0x000001bb &&
           AV_RB16(s + 0x12) == 0x0012 && AV_RB32(s + 0x26) == 0x000001bf &&
           s[0x2a] == 0x03 && s[0x2b] == 0xd4 && s[0x2c] == 0x00 && AV_RB32(s + 0x400) == 0x000001bf &&
           s[0x404] == 0x03 && s[0x405] == 0xfa && s[0x406] == 0x01;
}

/* The PCI starts at byte 0x2d of a NAV pack. */
static void parse_pci(const uint8_t *s, ScanPCI *p)
{
    memset(p, 0, sizeof(*p));
    p->nv_pck_lbn   = AV_RB32(s + 0x2d);
    p->vobu_s_ptm   = AV_RB32(s + 0x39);
    p->vobu_e_ptm   = AV_RB32(s + 0x3d);
    p->hli_ss       = s[0x8e] & 3;
    p->hli_s_ptm    = AV_RB32(s + 0x8f);
    p->hli_e_ptm    = AV_RB32(s + 0x93);
    p->btn_se_e_ptm = AV_RB32(s + 0x97);
    p->btn_ns       = s[0x9e];
    p->fosl_btnn    = s[0xa1];
    for (int i = 0; i < 36; i++) {
        const uint8_t *b = s + 0xbb + 18 * i;

        p->up[i]    = b[6] & 0x3f;
        p->down[i]  = b[7] & 0x3f;
        p->left[i]  = b[8] & 0x3f;
        p->right[i] = b[9] & 0x3f;
    }
}

/* ---- maps keyed by 64-bit values (every element starts with its key) ---- */

static int map_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

static void *map_get(struct AVTreeNode *root, uint64_t key)
{
    return av_tree_find(root, &key, map_cmp, NULL);
}

/* Store elem unless an element with its key is stored; returns the stored
 * element (elem, or the one kept), NULL when out of memory. */
static void *map_put(struct AVTreeNode **root, void *elem)
{
    struct AVTreeNode *node = av_tree_node_alloc();
    void *have;

    if (!node)
        return NULL;
    have = av_tree_insert(root, elem, map_cmp, &node);
    av_free(node);
    return have ? have : elem;
}

static int map_free_elem(void *opaque, void *elem)
{
    av_free(elem);
    return 0;
}

static void map_free(struct AVTreeNode **root)
{
    av_tree_enumerate(*root, NULL, NULL, map_free_elem);
    av_tree_destroy(*root);
    *root = NULL;
}

typedef struct SectorEntry {
    uint64_t key;
    uint8_t  data[DVDVIDEO_BLOCK_SIZE];
} SectorEntry;

typedef struct PCIEntry {
    uint64_t key;               /* VOB id << 32 | sector */
    ScanPCI  pci;
} PCIEntry;

/* What the NAV pack of a title-VOB sector said about its VOBU (or that the
 * sector holds no NAV pack) */
#define VOBU_NOT_A_NAV_PACK 0xffffffffU
#define VOBU_NO_NEXT        0x10000000U
#define IN_ILVU             0x4000

typedef struct VobuEntry {
    uint64_t key;
    uint16_t vobu_ea;
    uint16_t ilvu_ea;
    uint32_t next;              /* bits 28..0 distance to the next VOBU (VOBU_NO_NEXT: none),
                                 * bits 30..29 sml_pbi.category bits 14..13; VOBU_NOT_A_NAV_PACK */
    uint32_t vobu_s_ptm;
    uint32_t vobu_e_ptm;
} VobuEntry;

typedef struct VisitedEntry {
    uint64_t ord;               /* the key in key order */
    uint64_t key;
    uint64_t parent;
} VisitedEntry;

/* ---- the scan ---- */

typedef struct ScanEvent {
    int      code;
    uint32_t time_ms;
    uint16_t title_id;          /* 0 in menus; 0xffff: no part of title leads here, or unknown */
    uint16_t pgc_id;
    uint8_t  vtsn;
    uint8_t  cell_id;
    uint8_t  still_time;        /* 0xff: infinite */
    uint8_t  highlight;         /* SPRM 8 >> 10 */
    uint32_t vobu;              /* nv_pck_lbn of the last NAV pack */
    uint32_t sprm_flags;
} ScanEvent;

typedef struct Pending {
    uint64_t key;
    uint64_t parent;
    int      nav;
} Pending;

typedef struct PathRecord {
    uint8_t *cells;
    int      nb_cells;
    uint64_t key;
    uint8_t  title;
    uint8_t  pgc;
    int      depth;
    int      index;
} PathRecord;

typedef struct Scan {
    void            *log;
    DVDVideoSource  *src;
    DVDVideoScanTrace trace;
    void            *trace_opaque;

    ifo_handle_t    *ifo[100];          /* [0] = the VMG; NULL: not open (not given to the navigator) */
    int              nb_vts;
    uint32_t         title_vobs_base[100]; /* absolute sector of the title VOBs (images), 0 unknown */

    /* read service */
    struct AVTreeNode *sectors;
    struct AVTreeNode *pcis;
    struct AVTreeNode *vobus;
    int              refused;
    char             refusal[256];
    uint64_t         nb_reads;
    char           **requests;          /* "read <id> <sector>" lines not yet traced */
    int              nb_requests;

    /* navigators */
    DVDVideoRand     rand;
    dvdnav_t       **navs;
    int              nb_navs;
    int              failed;
    char             failure[512];

    /* explorer */
    int              root;
    int              snapshot;          /* navigator titles start from (0: none) */
    Pending         *pending;
    int              nb_pending;
    struct AVTreeNode *visited;
    int              region;
    int              reads_region;
    int              no_snapshot;
    uint16_t         snapshot_title;
    uint16_t         title;
    uint8_t          pgc;
    uint8_t         *cells;
    int              nb_cells;
    uint32_t         start_time;
    PathRecord      *records;
    int              nb_records;
    uint8_t          entered[100];
} Scan;

static void trace_line(Scan *sc, const char *fmt, ...)
{
    char line[1024];
    va_list ap;

    if (!sc->trace)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sc->trace(sc->trace_opaque, line);
}

static void vob_name(int id, char *buf, size_t size)
{
    int n = id & 0xff;

    if (id & MENU_VOB)
        snprintf(buf, size, n ? "VTS_%02d_0.VOB" : "VIDEO_TS.VOB", n);
    else
        snprintf(buf, size, "the title VOBs of title set %d", n);
}

static int fail(Scan *sc, const char *fmt, ...)
{
    va_list ap;

    if (!sc->failed) {
        sc->failed = 1;
        va_start(ap, fmt);
        vsnprintf(sc->failure, sizeof(sc->failure), fmt, ap);
        va_end(ap);
        av_log(sc->log, AV_LOG_DEBUG, "scan failed: %s\n", sc->failure);
    }
    return AVERROR_EXTERNAL;
}

/* ---- read service ---- */

static uint64_t sector_key(Scan *sc, int id, uint32_t sector)
{
    uint32_t base = (id & MENU_VOB) ? 0 : sc->title_vobs_base[id & 0xff];

    return base ? (uint32_t)(base + sector) : KEY_IN_VOB | (uint64_t)id << 32 | sector;
}

static VobuEntry *vobu_get(Scan *sc, int vtsn, uint32_t sector)
{
    return map_get(sc->vobus, sector_key(sc, vtsn, sector));
}

static int vobu_put(Scan *sc, int vtsn, uint32_t sector, const VobuEntry *rec)
{
    VobuEntry *e = av_memdup(rec, sizeof(*rec));
    VobuEntry *kept;

    if (!e)
        return AVERROR(ENOMEM);
    e->key = sector_key(sc, vtsn, sector);
    if (!(kept = map_put(&sc->vobus, e))) {
        av_free(e);
        return AVERROR(ENOMEM);
    }
    if (kept != e)
        av_free(e);
    return 0;
}

/* The NAV pack fields a VOBU record keeps */
typedef struct NavFields {
    uint32_t next_vobu;         /* vobu_sri.next_vobu */
    uint32_t vobu_ea;           /* dsi_gi.vobu_ea */
    uint32_t ilvu_ea;           /* sml_pbi.ilvu_ea */
    uint16_t category;          /* sml_pbi.category */
    uint32_t vobu_s_ptm;
    uint32_t vobu_e_ptm;
} NavFields;

static void nav_fields(const uint8_t *nav, NavFields *f)
{
    f->next_vobu  = AV_RB32(nav + 0x541);
    f->vobu_ea    = AV_RB32(nav + 0x40f);
    f->ilvu_ea    = AV_RB32(nav + 0x429);
    f->category   = AV_RB16(nav + 0x427);
    f->vobu_s_ptm = AV_RB32(nav + 0x39);
    f->vobu_e_ptm = AV_RB32(nav + 0x3d);
}

/* Read the NAV pack at a title-VOB sector straight from the disc (not through
 * the read service). A sector whose first 32 bytes are 0xFF but whose PCI and
 * DSI still name the sector (a NAV pack blanked out) counts as the NAV pack of
 * a VOBU of vobu_ea + 1 sectors and half a second, followed directly by the
 * next VOBU. A sector that cannot be read or holds no NAV pack is recorded as
 * such. 1 = a NAV pack, 0 = none, < 0 = error. */
static int read_nav_pack(Scan *sc, int vtsn, uint32_t sector, NavFields *f)
{
    VobuEntry none = { .next = VOBU_NOT_A_NAV_PACK };
    uint8_t buf[DVDVIDEO_BLOCK_SIZE];
    int ret = ff_dvdvideo_source_vob_read(sc->src, vtsn, 0, sector, buf, NAV_READ_ATTEMPTS);

    if (ret < 0) {
        av_log(sc->log, AV_LOG_DEBUG, "scan: title set %d: the NAV pack at sector %"PRIu32" could not be read\n",
               vtsn, sector);
    } else if (is_nav_pack(buf)) {
        nav_fields(buf, f);
        return 1;
    } else {
        int blanked = AV_RB32(buf + 0x2d) == sector && AV_RB32(buf + 0x40b) == sector &&
                      AV_RB32(buf + 0x40f) <= 0xfffff;

        for (int i = 0; i < 32 && blanked; i++)
            blanked = buf[i] == 0xff;
        if (blanked) {
            av_log(sc->log, AV_LOG_DEBUG, "scan: title set %d: sector %"PRIu32" is a blanked NAV pack, taken as "
                   "one\n", vtsn, sector);
            memset(f, 0, sizeof(*f));
            f->vobu_ea    = AV_RB32(buf + 0x40f);
            f->next_vobu  = f->vobu_ea + 1;
            f->vobu_e_ptm = 45000;
            return 1;
        }
        av_log(sc->log, AV_LOG_TRACE, "scan: title set %d: sector %"PRIu32" is not a NAV pack\n", vtsn, sector);
    }
    return vobu_put(sc, vtsn, sector, &none);
}

/* Record the VOBU whose NAV pack was read at title-VOB sector `sector`. *ok is
 * set when the NAV pack is usable: not pointing at itself (allowed only with
 * allow_self, and then nothing is recorded) and, for a VOBU that ends its cell
 * inside an interleaved unit that goes on, the VOBU after it readable. */
static int register_vobu(Scan *sc, int vtsn, const NavFields *f, uint32_t sector, int allow_self, int *ok)
{
    uint32_t nx = f->next_vobu & 0x7fffffff;
    int64_t next = nx == SRI_END_OF_CELL ? -2 : (uint32_t)(sector + nx);
    uint32_t distance;
    int ret;

    *ok = 0;
    if (next == sector) {
        if (!allow_self)
            av_log(sc->log, AV_LOG_DEBUG, "scan: title set %d: the NAV pack at sector %"PRIu32" points at "
                   "itself\n", vtsn, sector);
        else
            *ok = 1;
        return 0;
    }
    if (next == -2 && (f->category & IN_ILVU) && f->vobu_ea != f->ilvu_ea) {
        if (f->ilvu_ea < f->vobu_ea) {
            av_log(sc->log, AV_LOG_WARNING, "Title set %d: the interleaved unit of the VOBU at sector %"PRIu32" "
                   "of its title VOBs ends before the VOBU does; the disc data is damaged there\n", vtsn, sector);
        } else {
            /* the interleaved unit goes on after this VOBU */
            uint32_t following = sector + f->vobu_ea + 1;

            next = following;
            if (!vobu_get(sc, vtsn, following)) {
                NavFields f2;
                int ok2;

                if ((ret = read_nav_pack(sc, vtsn, following, &f2)) <= 0) {
                    av_log(sc->log, AV_LOG_DEBUG, "scan: title set %d: the VOBU after sector %"PRIu32" (%"PRIu32") "
                           "could not be read\n", vtsn, sector, following);
                    return ret;
                }
                if ((ret = register_vobu(sc, vtsn, &f2, following, 0, &ok2)) < 0)
                    return ret;
                if (!ok2) {
                    av_log(sc->log, AV_LOG_DEBUG, "scan: title set %d: the VOBU at sector %"PRIu32" could not "
                           "be recorded\n", vtsn, following);
                    return 0;
                }
            }
        }
    }
    *ok = 1;
    distance = next == -2 ? VOBU_NO_NEXT : (uint32_t)next - sector;
    if ((next == -2 || distance < VOBU_NO_NEXT) && f->vobu_ea < 0x10000 && f->ilvu_ea < 0x10000) {
        VobuEntry rec = {
            .vobu_ea    = f->vobu_ea,
            .ilvu_ea    = f->ilvu_ea,
            .next       = (uint32_t)(f->category & 0x6000) << 16 | distance,
            .vobu_s_ptm = f->vobu_s_ptm,
            .vobu_e_ptm = f->vobu_e_ptm,
        };
        return vobu_put(sc, vtsn, sector, &rec);
    }
    av_log(sc->log, AV_LOG_DEBUG, "scan: title set %d: the VOBU at sector %"PRIu32" is too large to record\n",
           vtsn, sector);
    return 0;
}

static int refuse(Scan *sc, int id, uint32_t sector, const char *reason)
{
    char name[64];

    vob_name(id, name, sizeof(name));
    av_log(sc->log, AV_LOG_DEBUG, "scan read of sector %"PRIu32" of %s refused: %s\n", sector, name, reason);
    if (!sc->refused) {
        sc->refused = 1;
        snprintf(sc->refusal, sizeof(sc->refusal), "reading sector %"PRIu32" of %s was refused: %s",
                 sector, name, reason);
    }
    return AVERROR(EIO);
}

/* Read a sector from the disc and record what it says. */
static int fetch(Scan *sc, int id, uint32_t sector, uint8_t *buf)
{
    int menu = !!(id & MENU_VOB), vtsn = id & 0xff, nav, ret;
    char reason[128];

    if (!sc->ifo[vtsn])
        return refuse(sc, id, sector, "its IFO is not open");
    ret = ff_dvdvideo_source_vob_read(sc->src, vtsn, menu, sector, buf, NAV_READ_ATTEMPTS);
    if (ret < 0) {
        if (ret == AVERROR(ENOENT))
            snprintf(reason, sizeof(reason), "the VOB does not exist");
        else if (ret == AVERROR_EOF)
            snprintf(reason, sizeof(reason), "past the end of the VOB");
        else
            snprintf(reason, sizeof(reason), "%s", av_err2str(ret));
        return refuse(sc, id, sector, reason);
    }
    sc->nb_reads++;

    nav = is_nav_pack(buf);
    if (nav) {
        ScanPCI pci;

        parse_pci(buf, &pci);
        if (pci.nv_pck_lbn != sector) {
            av_log(sc->log, AV_LOG_DEBUG, "scan: the NAV pack at sector %"PRIu32" of VOB %x names sector "
                   "%"PRIu32"\n", sector, id, pci.nv_pck_lbn);
        } else if (menu || pci.hli_ss) {
            PCIEntry *e = av_malloc(sizeof(*e)), *kept;

            if (!e)
                return AVERROR(ENOMEM);
            e->key = (uint64_t)id << 32 | pci.nv_pck_lbn;
            e->pci = pci;
            if (!(kept = map_put(&sc->pcis, e))) {
                av_free(e);
                return AVERROR(ENOMEM);
            }
            if (kept != e)
                av_free(e);
        }
    } else {
        av_log(sc->log, AV_LOG_TRACE, "scan: sector %"PRIu32" of VOB %x is not a NAV pack\n", sector, id);
    }
    if (!menu && vtsn) {
        if (nav) {
            NavFields f;
            int ok;

            nav_fields(buf, &f);
            if ((ret = register_vobu(sc, vtsn, &f, sector, 1, &ok)) < 0)
                return ret;
        } else {
            VobuEntry none = { .next = VOBU_NOT_A_NAV_PACK };

            if ((ret = vobu_put(sc, vtsn, sector, &none)) < 0)
                return ret;
        }
    }
    return 0;
}

/* Fill out with sector `sector` of VOB `id`. After a refusal every read is
 * refused. */
static int service_read(Scan *sc, int id, uint32_t sector, uint8_t *out)
{
    uint64_t key = sector_key(sc, id, sector);
    SectorEntry *e;
    char *line;
    int ret;

    if (sc->refused)
        return AVERROR(EIO);
    if ((e = map_get(sc->sectors, key))) {
        memcpy(out, e->data, DVDVIDEO_BLOCK_SIZE);
        return 0;
    }
    if (sc->trace) {
        if (!(line = av_asprintf("read %d %"PRIu32, id, sector)) ||
            av_dynarray_add_nofree(&sc->requests, &sc->nb_requests, line) < 0) {
            av_free(line);
            return AVERROR(ENOMEM);
        }
    }
    if (!(e = av_malloc(sizeof(*e))))
        return AVERROR(ENOMEM);
    if ((ret = fetch(sc, id, sector, e->data)) < 0) {
        av_free(e);
        return ret;
    }
    e->key = key;
    if (!map_put(&sc->sectors, e)) {
        av_free(e);
        return AVERROR(ENOMEM);
    }
    memcpy(out, e->data, DVDVIDEO_BLOCK_SIZE);
    return 0;
}

/* ---- the files libdvdread sees during the scan ----
 *
 * "/" holds VIDEO_TS, which holds VIDEO_TS.IFO / .BUP and VTS_nn_0.IFO / .BUP
 * of every open IFO (served from the disc source), plus VIDEO_TS.VOB,
 * VTS_nn_0.VOB (menu VOBs) and VTS_nn_1.VOB (all title VOBs of the title set
 * as one file). The VOBs report a size far beyond any disc, so the read
 * service alone decides what can be read. */

typedef struct ScanFS {
    Scan                    *sc;
    dvd_reader_filesystem_h *inner;     /* the disc source's files */
} ScanFS;

typedef struct ScanDir {
    char names[100 * 5][16];
    int  nb, next;
} ScanDir;

typedef struct ScanFile {
    Scan *sc;
    int   vob_id;                       /* -1: an IFO / BUP of the source */
    void *inner;
    dvd_reader_filesystem_h *inner_fs;
    int64_t pos;
} ScanFile;

/* Parse a path into the VIDEO_TS file name it names: 0 = "/", 1 = VIDEO_TS,
 * 2 = a file (its name upper-cased in name), -1 = nothing. */
static int scan_path(const char *path, char *name, size_t size)
{
    char comp[3][64];
    int n = 0;

    for (const char *p = path; *p;) {
        size_t len;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        len = strcspn(p, "/");
        if (n == 3 || len >= sizeof(comp[0]))
            return -1;
        memcpy(comp[n], p, len);
        comp[n++][len] = 0;
        p += len;
    }
    if (!n)
        return 0;
    if (av_strcasecmp(comp[0], "VIDEO_TS"))
        return -1;
    if (n == 1)
        return 1;
    if (n != 2)
        return -1;
    av_strlcpy(name, comp[1], size);
    for (char *c = name; *c; c++)
        *c = av_toupper(*c);
    return 2;
}

/* What a VIDEO_TS name is: 0 IFO / BUP (*ifo set), 1 VOB (*vob_id set), -1 none. */
static int scan_file_kind(Scan *sc, const char *name, int *ifo, int *vob_id)
{
    int n;

    if (!strcmp(name, "VIDEO_TS.IFO") || !strcmp(name, "VIDEO_TS.BUP")) {
        *ifo = 0;
        return sc->ifo[0] ? 0 : -1;
    }
    if (!strcmp(name, "VIDEO_TS.VOB")) {
        *vob_id = MENU_VOB;
        return sc->ifo[0] ? 1 : -1;
    }
    if (strlen(name) != 12 || strncmp(name, "VTS_", 4) || !av_isdigit(name[4]) || !av_isdigit(name[5]) ||
        name[6] != '_')
        return -1;
    n = (name[4] - '0') * 10 + name[5] - '0';
    if (n < 1 || !sc->ifo[n])
        return -1;
    if (!strcmp(name + 7, "0.IFO") || !strcmp(name + 7, "0.BUP")) {
        *ifo = n;
        return 0;
    }
    if (!strcmp(name + 7, "0.VOB")) {
        *vob_id = n | MENU_VOB;
        return 1;
    }
    if (!strcmp(name + 7, "1.VOB")) {
        *vob_id = n;
        return 1;
    }
    return -1;
}

static void scan_fs_close(dvd_reader_filesystem_h *fs)
{
    ScanFS *f = fs->internal;

    f->inner->close(f->inner);
    av_free(f);
    av_free(fs);
}

static int scan_fs_stat(dvd_reader_filesystem_h *fs, const char *path, dvdstat_t *st)
{
    ScanFS *f = fs->internal;
    char name[64], real[600];
    int kind, ifo, vob;

    switch (scan_path(path, name, sizeof(name))) {
    case 0:
    case 1:
        st->size    = 0;
        st->st_mode = DVD_S_IFDIR;
        return 0;
    case 2:
        kind = scan_file_kind(f->sc, name, &ifo, &vob);
        if (kind == 1) {
            st->size    = SCAN_FILE_SIZE;
            st->st_mode = DVD_S_IFREG;
            return 0;
        }
        if (kind == 0 && ff_dvdvideo_source_find(f->sc->src, name, real, sizeof(real)) >= 0)
            return f->inner->stat(f->inner, real, st);
        return -1;
    }
    return -1;
}

static void *scan_fs_dir_open(dvd_reader_filesystem_h *fs, const char *path)
{
    ScanFS *f = fs->internal;
    Scan *sc = f->sc;
    char name[64];
    ScanDir *d;
    int kind = scan_path(path, name, sizeof(name));

    if (kind != 0 && kind != 1)
        return NULL;
    if (!(d = av_mallocz(sizeof(*d))))
        return NULL;
    if (kind == 0) {
        av_strlcpy(d->names[d->nb++], "VIDEO_TS", sizeof(d->names[0]));
        return d;
    }
    for (int n = 0; n <= sc->nb_vts; n++) {
        static const char *const vmg[] = { "VIDEO_TS.IFO", "VIDEO_TS.BUP", "VIDEO_TS.VOB" };
        static const char *const vts[] = { "0.IFO", "0.BUP", "0.VOB", "1.VOB" };

        if (!sc->ifo[n])
            continue;
        if (!n)
            for (int i = 0; i < 3; i++)
                av_strlcpy(d->names[d->nb++], vmg[i], sizeof(d->names[0]));
        else
            for (int i = 0; i < 4; i++)
                snprintf(d->names[d->nb++], sizeof(d->names[0]), "VTS_%02d_%s", n, vts[i]);
    }
    return d;
}

static int scan_fs_dir_read(void *dir, dvd_dirent_t *entry)
{
    ScanDir *d = dir;

    if (d->next >= d->nb)
        return 1;
    av_strlcpy(entry->d_name, d->names[d->next++], sizeof(entry->d_name));
    return 0;
}

static void scan_fs_dir_close(void *dir)
{
    av_free(dir);
}

static void *scan_fs_file_open(dvd_reader_filesystem_h *fs, const char *path)
{
    ScanFS *f = fs->internal;
    ScanFile *h;
    char name[64], real[600];
    int kind = -1, ifo, vob;

    if (scan_path(path, name, sizeof(name)) == 2)
        kind = scan_file_kind(f->sc, name, &ifo, &vob);
    if (kind < 0) {
        av_log(f->sc->log, AV_LOG_DEBUG, "scan: libdvdread asked for %s, which does not exist\n", path);
        return NULL;
    }
    if (!(h = av_mallocz(sizeof(*h))))
        return NULL;
    h->sc     = f->sc;
    h->vob_id = -1;
    if (kind == 1) {
        h->vob_id = vob;
        return h;
    }
    h->inner_fs = f->inner;
    if (ff_dvdvideo_source_find(f->sc->src, name, real, sizeof(real)) < 0 ||
        !(h->inner = f->inner->file_open(f->inner, real))) {
        av_free(h);
        return NULL;
    }
    return h;
}

static ssize_t scan_fs_file_read(void *file, char *buf, size_t size)
{
    ScanFile *h = file;
    uint8_t sector[DVDVIDEO_BLOCK_SIZE];
    size_t done = 0;

    if (h->vob_id < 0)
        return h->inner_fs->file_read(h->inner, buf, size);
    while (done < size) {
        int64_t at  = h->pos + done;
        size_t off  = at % DVDVIDEO_BLOCK_SIZE;
        size_t n    = FFMIN(size - done, DVDVIDEO_BLOCK_SIZE - off);

        if (at / DVDVIDEO_BLOCK_SIZE > UINT32_MAX ||
            service_read(h->sc, h->vob_id, at / DVDVIDEO_BLOCK_SIZE, sector) < 0)
            return -1;
        memcpy(buf + done, sector + off, n);
        done += n;
    }
    h->pos += done;
    return done;
}

static off64_t scan_fs_file_seek(void *file, off64_t offset, int whence)
{
    ScanFile *h = file;
    int64_t pos;

    if (h->vob_id < 0)
        return h->inner_fs->file_seek(h->inner, offset, whence);
    pos = whence == SEEK_SET ? offset : whence == SEEK_CUR ? h->pos + offset :
          whence == SEEK_END ? (int64_t)SCAN_FILE_SIZE + offset : -1;
    if (pos < 0)
        return -1;
    h->pos = pos;
    return pos;
}

static int scan_fs_file_close(void *file)
{
    ScanFile *h = file;

    if (h->inner)
        h->inner_fs->file_close(h->inner);
    av_free(h);
    return 0;
}

static dvd_reader_filesystem_h *scan_fs_new(Scan *sc)
{
    dvd_reader_filesystem_h *fs = av_mallocz(sizeof(*fs));
    ScanFS *f = av_mallocz(sizeof(*f));

    if (!fs || !f || !(f->inner = ff_dvdvideo_source_files(sc->src))) {
        av_free(fs);
        av_free(f);
        return NULL;
    }
    f->sc          = sc;
    fs->internal   = f;
    fs->close      = scan_fs_close;
    fs->stat       = scan_fs_stat;
    fs->dir_open   = scan_fs_dir_open;
    fs->dir_read   = scan_fs_dir_read;
    fs->dir_close  = scan_fs_dir_close;
    fs->file_open  = scan_fs_file_open;
    fs->file_read  = scan_fs_file_read;
    fs->file_seek  = scan_fs_file_seek;
    fs->file_close = scan_fs_file_close;
    return fs;
}

/* ---- navigators ---- */

enum ScanParam {
    PARAM_REGION_MASK,
    PARAM_MENU_LANGUAGE,
    PARAM_AUDIO_LANGUAGE,
    PARAM_SPU_LANGUAGE,
    PARAM_BUTTON,           /* select button n of the current NAV pack and run its command */
    PARAM_TITLE,            /* play title n, following its pre commands */
    PARAM_HIGHLIGHT,        /* select (highlight) button n without running it */
};

static const char *const param_names[] = {
    "region mask", "menu language", "audio language", "subpicture language", "button", "title", "highlight",
};

static void scan_libdvdnav_log(void *opaque, dvdnav_logger_level_t level, const char *msg, va_list args)
{
    char buf[DVDVIDEO_LIBDVDX_LOG_BUFFER_SIZE];

    vsnprintf(buf, sizeof(buf), msg, args);
    av_log(opaque, level <= DVDNAV_LOGGER_LEVEL_WARN ? AV_LOG_DEBUG : AV_LOG_TRACE, "scan: libdvdnav: %s\n", buf);
}

/* Trace the reads of the call just made, then its result line. */
static void trace_requests(Scan *sc)
{
    for (int i = 0; i < sc->nb_requests; i++) {
        if (sc->trace)
            sc->trace(sc->trace_opaque, sc->requests[i]);
        av_free(sc->requests[i]);
    }
    av_freep(&sc->requests);
    sc->nb_requests = 0;
}

static void trace_failure(Scan *sc)
{
    trace_requests(sc);
    trace_line(sc, "fail %s", sc->failure);
}

/* Write a line of the scan's own log (debug log and the trace). */
static void scan_log(Scan *sc, const char *what, uint64_t key)
{
    av_log(sc->log, AV_LOG_DEBUG, "scan: %s %u:%02u:%08X:%02u\n", what, (unsigned)(key >> 48 & 0xff),
           (unsigned)(key >> 32 & 0xffff), (unsigned)key, (unsigned)(key >> 56));
    trace_line(sc, "log %s %u:%02u:%08X:%02u", what, (unsigned)(key >> 48 & 0xff), (unsigned)(key >> 32 & 0xffff),
               (unsigned)key, (unsigned)(key >> 56));
}

/* Navigator i, or the failure that ended the scan. */
static dvdnav_t *nav_get(Scan *sc, int i)
{
    if (sc->failed)
        return NULL;
    if (i < 0 || i >= sc->nb_navs) {
        fail(sc, "navigator %d does not exist", i);
        return NULL;
    }
    return sc->navs[i];
}

/* After a call on navigator i: a refused read or broken navigation data
 * fails the scan. */
static int nav_check(Scan *sc, int i)
{
    const char *first = NULL;

    if (sc->refused)
        return fail(sc, "%s", sc->refusal);
    if (dvdnav_get_vm_failures(sc->navs[i], &first) > 0)
        return fail(sc, "navigator %d: broken navigation data: %s", i, first ? first : "");
    return 0;
}

static int nav_copy(Scan *sc, int src)
{
    dvdnav_t *s = nav_get(sc, src), *d = NULL;
    int ret;

    if (!s) {
        trace_failure(sc);
        return AVERROR_EXTERNAL;
    }
    if (dvdnav_dup(&d, s) != DVDNAV_STATUS_OK || !d) {
        fail(sc, "navigator %d could not be copied", src);
        trace_failure(sc);
        return AVERROR_EXTERNAL;
    }
    if ((ret = av_dynarray_add_nofree(&sc->navs, &sc->nb_navs, d)) < 0) {
        dvdnav_free_dup(d);
        fail(sc, "out of memory");
        trace_failure(sc);
        return ret;
    }
    trace_requests(sc);
    trace_line(sc, "nav %d", sc->nb_navs - 1);
    return sc->nb_navs - 1;
}

static int nav_set(Scan *sc, int i, enum ScanParam param, int32_t v)
{
    dvdnav_t *n = nav_get(sc, i);
    dvdnav_status_t st = DVDNAV_STATUS_ERR;
    char lang[3] = { v & 0xff, v >> 8 & 0xff, 0 };

    if (!n) {
        trace_failure(sc);
        return AVERROR_EXTERNAL;
    }
    switch (param) {
    case PARAM_REGION_MASK:    st = dvdnav_set_region_mask(n, v);                                       break;
    case PARAM_MENU_LANGUAGE:  st = dvdnav_menu_language_select(n, lang);                               break;
    case PARAM_AUDIO_LANGUAGE: st = dvdnav_audio_language_select(n, lang);                              break;
    case PARAM_SPU_LANGUAGE:   st = dvdnav_spu_language_select(n, lang);                                break;
    case PARAM_BUTTON:         st = dvdnav_button_select_and_activate(n, dvdnav_get_current_nav_pci(n), v); break;
    case PARAM_TITLE:          st = dvdnav_title_play(n, v);                                            break;
    case PARAM_HIGHLIGHT:      st = dvdnav_button_select(n, dvdnav_get_current_nav_pci(n), v);          break;
    }
    av_log(sc->log, AV_LOG_TRACE, "scan: navigator %d: %s %"PRId32" -> %s\n", i, param_names[param], v,
           st == DVDNAV_STATUS_OK ? "ok" : "rejected");
    if (nav_check(sc, i) < 0 ||
        (st != DVDNAV_STATUS_OK &&
         fail(sc, "navigator %d: %s %"PRId32" was rejected: %s", i, param_names[param], v,
              dvdnav_err_to_string(n)) < 0)) {
        trace_failure(sc);
        return AVERROR_EXTERNAL;
    }
    trace_requests(sc);
    trace_line(sc, "set");
    return 0;
}

/* The position of a still, NAV packet or cell change event. */
static void nav_position(dvdnav_t *n, ScanEvent *ev)
{
    int32_t title, vtsn, pgcn, pgn, celln;

    if (dvdnav_current_title_program2(n, &title, &vtsn, &pgcn, &pgn, &celln) == DVDNAV_STATUS_OK) {
        ev->vtsn     = vtsn;
        ev->pgc_id   = pgcn;
        ev->cell_id  = celln;
        ev->title_id = title < 0 ? 0xffff : title;
    } else {
        ev->title_id = 0xffff;
    }
    ev->vobu = dvdnav_get_current_nav_pci(n)->pci_gi.nv_pck_lbn;
}

/* Step navigator i by one libdvdnav event: finite stills and waits are
 * skipped as they come, an infinite still is reported each step. */
static int nav_step(Scan *sc, int i, ScanEvent *ev)
{
    dvdnav_t *n = nav_get(sc, i);
    uint8_t buf[DVDVIDEO_BLOCK_SIZE];
    int32_t code = 0, len = 0;
    dvdnav_status_t st;

    memset(ev, 0, sizeof(*ev));
    if (!n) {
        trace_failure(sc);
        return AVERROR_EXTERNAL;
    }
    st = dvdnav_get_next_block(n, buf, &code, &len);
    if (nav_check(sc, i) < 0 ||
        (st != DVDNAV_STATUS_OK && fail(sc, "navigator %d: %s", i, dvdnav_err_to_string(n)) < 0)) {
        trace_failure(sc);
        return AVERROR_EXTERNAL;
    }
    ev->code       = code;
    ev->time_ms    = dvdnav_get_absolute_time(n) / 90;
    ev->sprm_flags = dvdnav_get_prm(n, DVDNAV_PRM_SPRM_FLAGS, 0);
    ev->highlight  = dvdnav_get_prm(n, DVDNAV_PRM_SPRM, 8) >> 10;
    switch (code) {
    case DVDNAV_STILL_FRAME: {
        dvdnav_still_event_t still;

        memcpy(&still, buf, sizeof(still));
        ev->still_time = still.length;
        nav_position(n, ev);
        if (ev->still_time != 0xff)
            dvdnav_still_skip(n);
        break;
    }
    case DVDNAV_WAIT:
        dvdnav_wait_skip(n);
        break;
    case DVDNAV_NAV_PACKET:
    case DVDNAV_CELL_CHANGE:
        nav_position(n, ev);
        break;
    }
    /* the position report can find broken navigation data too */
    if (nav_check(sc, i) < 0) {
        trace_failure(sc);
        return AVERROR_EXTERNAL;
    }
    trace_requests(sc);
    trace_line(sc, "ev %d %"PRIu32" %u %u %u %u %u %u %"PRIu32" %08"PRIx32, ev->code, ev->time_ms,
               ev->title_id, ev->pgc_id, ev->vtsn, ev->cell_id, ev->still_time, ev->highlight, ev->vobu,
               ev->sprm_flags);
    return 0;
}

/* Open navigator 0 (not started) on the scan's files. */
static int nav_open(Scan *sc)
{
    dvdnav_logger_cb log_cb = { .pf_log = scan_libdvdnav_log };
    dvd_reader_filesystem_h *fs = scan_fs_new(sc);
    dvdnav_t *root = NULL;
    int ret;

    if (!fs)
        return AVERROR(ENOMEM);
    /* the navigator takes the files over in every case */
    if (dvdnav_open_files(&root, sc->log, &log_cb, "/", fs) != DVDNAV_STATUS_OK || !root) {
        fail(sc, "libdvdnav could not open the disc");
        return AVERROR_EXTERNAL;
    }
    if ((ret = av_dynarray_add_nofree(&sc->navs, &sc->nb_navs, root)) < 0) {
        dvdnav_close(root);
        return ret;
    }
    dvdnav_set_readahead_flag(root, 0);
    dvdnav_set_nav_only_flag(root, 1);
    dvdnav_set_random_source(root, ff_dvdvideo_rand_next, &sc->rand);
    dvdnav_set_title_play_follows_jumps(root, 1);
    av_log(sc->log, AV_LOG_DEBUG, "scan: navigator 0 opened\n");
    return 0;
}

/* ---- the explorer ---- */

/* Key order: byte 6, bits 32..47, bits 0..31, byte 7. */
static uint64_t key_ord(uint64_t k)
{
    return (k >> 48 & 0xff) << 56 | (k >> 32 & 0xffff) << 40 | (k & 0xffffffff) << 8 | k >> 56;
}

static uint64_t region_root(int r)
{
    return (uint64_t)r << 48 | 0xffffffff;
}

static VisitedEntry *visited_get(Scan *sc, uint64_t key)
{
    return map_get(sc->visited, key_ord(key));
}

static int visited_put(Scan *sc, uint64_t key, uint64_t parent)
{
    VisitedEntry *e = av_malloc(sizeof(*e)), *kept;

    if (!e)
        return AVERROR(ENOMEM);
    *e = (VisitedEntry) { key_ord(key), key, parent };
    if (!(kept = map_put(&sc->visited, e))) {
        av_free(e);
        return AVERROR(ENOMEM);
    }
    if (kept != e)
        av_free(e);
    return 0;
}

/* The key is visited or pending. */
static int known(Scan *sc, uint64_t key)
{
    if (visited_get(sc, key))
        return 1;
    for (int i = 0; i < sc->nb_pending; i++)
        if (sc->pending[i].key == key)
            return 1;
    return 0;
}

static int add_pending(Scan *sc, uint64_t key, uint64_t parent, int nav)
{
    Pending p = { key, parent, nav };

    scan_log(sc, "pending", key);
    return av_dynarray2_add((void **)&sc->pending, &sc->nb_pending, sizeof(p), (const uint8_t *)&p) ? 0
                                                                                                    : AVERROR(ENOMEM);
}

/* The title search table entry of title t (1-based): title set and the
 * title's own number in it; -1 when there is none. */
static int tt_entry(Scan *sc, int t, int *vtsn)
{
    const tt_srpt_t *tt = sc->ifo[0]->tt_srpt;

    if (!tt || t < 1 || t > tt->nr_of_srpts)
        return -1;
    *vtsn = tt->title[t - 1].title_set_nr;
    return 0;
}

static int nr_of_titles(Scan *sc)
{
    return sc->ifo[0]->tt_srpt ? sc->ifo[0]->tt_srpt->nr_of_srpts : 0;
}

/* Program chain pgcn (by search pointer) of title set vtsn. */
static pgc_t *vts_pgc(Scan *sc, int vtsn, int pgcn)
{
    const ifo_handle_t *ifo = vtsn >= 0 && vtsn < 100 ? sc->ifo[vtsn] : NULL;

    if (!ifo || !vtsn || !ifo->vts_pgcit || pgcn < 1 || pgcn > ifo->vts_pgcit->nr_of_pgci_srp)
        return NULL;
    return ifo->vts_pgcit->pgci_srp[pgcn - 1].pgc;
}

/* The first byte of a cell's playback information (block mode, block type,
 * seamless play, interleaved, STC discontinuity, seamless angle). */
static int cell_flags(const pgc_t *pgc, int k)
{
    const cell_playback_t *c = &pgc->cell_playback[k];

    return c->block_mode << 6 | c->block_type << 4 | c->seamless_play << 3 | c->interleaved << 2 |
           c->stc_discontinuity << 1 | c->seamless_angle;
}

/* The cells of cell c's angle block (all angles), or just c. */
static void cell_range(Scan *sc, int t, int pgcn, int c, int *lo, int *hi)
{
    const pgc_t *p;
    int vtsn, i, first, last, f, b;

    *lo = *hi = c;
    if (tt_entry(sc, t, &vtsn) < 0 || !pgcn || !(p = vts_pgc(sc, vtsn, pgcn)) || !p->cell_playback)
        return;
    i = c - 1;
    if (i < 0 || i >= p->nr_of_cells)
        return;
    f = cell_flags(p, i);
    if ((f >> 4 & 3) != 1)
        return;
    /* back to the block's first cell */
    first = i;
    b = f;
    while ((b & 0xc0) != 0x40) {
        if (b < 0x40 || (b & 0x30) != 0x10 || !(first & 0xff))
            return;
        first = (first & 0xff) - 1;
        b = cell_flags(p, first);
    }
    /* forward to its last cell */
    last = i;
    b = f;
    for (;;) {
        if (b >> 6 == 3) {
            *lo = (first & 0xff) + 1;
            *hi = (last & 0xff) + 1;
            return;
        }
        if ((b & 0x30) != 0x10 || b >> 6 < 1 || b >> 6 > 2)
            return;
        if ((last & 0xff) + 1 >= p->nr_of_cells)
            return;
        last = (last & 0xff) + 1;
        b = cell_flags(p, last);
    }
}

/* Make a cell sequence canonical: a sequence that walks the cells 1..max
 * cyclically becomes 1..max; then repeated blocks at its start and at its end
 * are removed, for block lengths 1, 2, ... */
static void normalise_cells(uint8_t *s, int *nb)
{
    int n = *nb, mx = 0, mn = 255;

    for (int i = 0; i < n; i++) {
        mx = FFMAX(mx, s[i]);
        mn = FFMIN(mn, s[i]);
    }
    if (mn == 1 && mx <= n) {
        int d = s[0] - 1, cyclic = 1;

        for (int i = 0; i < n && cyclic; i++)
            cyclic = s[i] == (d + i) % mx + 1;
        if (cyclic && !(mx == n && s[0] == 1)) {
            for (int i = 0; i < mx; i++)
                s[i] = i + 1;
            n = mx;
        }
    }
    for (int p = 1; p <= n / 2; p++) {
        while (n >= 2 * p && !memcmp(s, s + p, p)) {
            memmove(s, s + p, n - p);
            n -= p;
        }
        while (n >= 2 * p && !memcmp(s + n - p, s + n - 2 * p, p))
            n -= p;
    }
    *nb = n;
}

/* End the current cell sequence: record it (made canonical) for the current
 * title and PGC. */
static int flush(Scan *sc, uint64_t key)
{
    PathRecord r = { 0 };

    sc->start_time = 0;
    if (!sc->nb_cells) {
        sc->title = 0;
        return 0;
    }
    normalise_cells(sc->cells, &sc->nb_cells);
    r.cells    = sc->cells;
    r.nb_cells = sc->nb_cells;
    r.key      = key;
    r.title    = sc->title & 0xff;
    r.pgc      = sc->pgc;
    r.index    = sc->nb_records;
    sc->cells    = NULL;
    sc->nb_cells = 0;
    sc->title    = 0;
    if (!av_dynarray2_add((void **)&sc->records, &sc->nb_records, sizeof(r), (const uint8_t *)&r)) {
        av_free(r.cells);
        return AVERROR(ENOMEM);
    }
    return 0;
}

/* A cell started playing (first block after a cell change). */
static int cell_played(Scan *sc, uint64_t key, uint16_t t, uint16_t pgcn, uint8_t vtsn, uint8_t c, uint32_t time_ms)
{
    int ret;

    /* the PGC is kept as one byte, but compared in full */
    if (t != sc->title || pgcn != sc->pgc) {
        if ((ret = flush(sc, key)) < 0)
            return ret;
        sc->title = t;
        sc->pgc   = pgcn;
    }
    if (t != 0 && t != 0xffff) {
        int lo, hi;
        uint8_t *cells;

        cell_range(sc, t, pgcn, c, &lo, &hi);
        if (!(cells = av_realloc(sc->cells, sc->nb_cells + hi - lo + 1)))
            return AVERROR(ENOMEM);
        sc->cells = cells;
        for (int k = lo; k <= hi; k++)
            sc->cells[sc->nb_cells++] = k;
    }
    if (!sc->start_time)
        sc->start_time = time_ms;
    if (vtsn < 100)
        sc->entered[vtsn] = 1;
    return 0;
}

/* The PCI of the NAV pack an event reports: a recorded PCI or, for title
 * VOBs, a VOBU record with only its times. */
static int nav_record(Scan *sc, const ScanEvent *ev, int menu, ScanPCI *out)
{
    int id = (menu ? MENU_VOB : 0) | ev->vtsn;
    PCIEntry *p = map_get(sc->pcis, (uint64_t)id << 32 | ev->vobu);
    VobuEntry *v;

    if (p) {
        *out = p->pci;
        return 1;
    }
    if (menu || !ev->vtsn || !sc->ifo[ev->vtsn] || !(v = vobu_get(sc, ev->vtsn, ev->vobu)))
        return 0;
    memset(out, 0, sizeof(*out));
    out->nv_pck_lbn = ev->vobu;
    out->vobu_s_ptm = v->vobu_s_ptm;
    out->vobu_e_ptm = v->vobu_e_ptm;
    return 1;
}

/* A highlight h is active at the VOBU of NAV pack rec. */
static int highlight_active(const ScanPCI *rec, const ScanPCI *h)
{
    return h->vobu_s_ptm <= rec->vobu_s_ptm && h->vobu_s_ptm <= rec->vobu_e_ptm &&
           h->hli_s_ptm <= rec->vobu_s_ptm && rec->vobu_s_ptm <= h->hli_e_ptm && h->btn_ns &&
           rec->vobu_s_ptm <= h->btn_se_e_ptm - 1;
}

/* Two highlights are the same (same buttons and times). */
static int same_highlight(const ScanPCI *h, const ScanPCI *x)
{
    return h->btn_ns == x->btn_ns && h->hli_s_ptm == x->hli_s_ptm && h->hli_e_ptm == x->hli_e_ptm &&
           h->btn_se_e_ptm == x->btn_se_e_ptm;
}

/* Buttons reachable from button by the up / down / left / right links. */
static void reachable_buttons(uint64_t *mask, const ScanPCI *pci, uint8_t button)
{
    while (button) {
        int i;

        if (button > 36 || button > pci->btn_ns)
            return;
        i = button - 1;
        if (*mask & (1ULL << i))
            return;
        *mask |= 1ULL << i;
        reachable_buttons(mask, pci, pci->up[i]);
        reachable_buttons(mask, pci, pci->down[i]);
        reachable_buttons(mask, pci, pci->left[i]);
        button = pci->right[i];
    }
}

/* Press every button reachable in highlight hli, each on its own copy of
 * navigator ctx, from the highest number down. */
static int press_buttons(Scan *sc, int ctx, uint64_t parent, const ScanEvent *ev, const ScanPCI *hli)
{
    uint8_t n = hli->btn_ns, start;
    uint64_t mask = 0;
    int ret;

    if (!n)
        return 0;
    start = (uint8_t)(hli->fosl_btnn - 1) < n ? hli->fosl_btnn : FFMIN(FFMAX(ev->highlight, 1), n);
    reachable_buttons(&mask, hli, start);
    for (int b = n; b >= 1; b--) {
        uint64_t key;
        int c;

        if (b > 64 || !(mask & (1ULL << (b - 1))))
            continue;
        key = (uint64_t)(b - 1) << 56 | (uint64_t)sc->region << 48 | (uint64_t)(ev->title_id == 0) << 40 |
              (uint64_t)ev->vtsn << 32 | ev->vobu;
        if (known(sc, key))
            continue;
        if ((c = nav_copy(sc, ctx)) < 0)
            return c;
        if ((ret = nav_set(sc, c, PARAM_BUTTON, b)) < 0 || (ret = add_pending(sc, key, parent, c)) < 0)
            return ret;
    }
    return 0;
}

/* Take the title-domain snapshot once a title has played long enough (when
 * title play is not prohibited there). */
static int maybe_snapshot(Scan *sc, int ctx, const ScanEvent *ev)
{
    const pgc_t *p;
    int vtsn;

    if (tt_entry(sc, ev->title_id, &vtsn) < 0)
        return 0;
    if (vtsn != ev->vtsn)
        av_log(sc->log, AV_LOG_DEBUG, "scan: title %u is in title set %d, the navigator says %u\n",
               ev->title_id, vtsn, ev->vtsn);
    if (!sc->ifo[vtsn] || !ev->pgc_id || !(p = vts_pgc(sc, vtsn, ev->pgc_id)))
        return 0;
    if (!p->prohibited_ops.title_play && !sc->snapshot) {
        int n = nav_copy(sc, ctx);

        if (n < 0)
            return n;
        sc->snapshot       = n;
        sc->snapshot_title = ev->title_id;
    }
    return 0;
}

/* Play one path; record its sequences. */
static int walk_path(Scan *sc, uint64_t key, int ctx)
{
    ScanPCI hli, last_rec, explored, rec, cand;
    int have_hli = 0, have_last = 0, have_explored = 0, have_cand;
    uint32_t *seen = NULL, limit = 0, cell_changes = 0, group = 0;
    int nb_seen = 0, first = 1, pending_cell = 0, ret = 0;
    uint16_t c_title = 0, c_pgc = 0;
    uint8_t c_vtsn = 0, c_cell = 0;
    ScanEvent ev;

    sc->title    = 0;
    sc->pgc      = 0;
    sc->nb_cells = 0;
    scan_log(sc, "path", key);
    if ((ret = nav_step(sc, ctx, &ev)) < 0)
        goto end;
    for (;;) {
        if (first) {
            first = 0;
            limit = ev.time_ms + PATH_TIME_LIMIT_MS;
        }
        if (ev.time_ms > limit)
            break;
        switch (ev.code) {
        case DVDNAV_BLOCK_OK:
            if (pending_cell && (ret = cell_played(sc, key, c_title, c_pgc, c_vtsn, c_cell, ev.time_ms)) < 0)
                goto end;
            pending_cell = 0;
            break;
        case DVDNAV_STILL_FRAME:
            if (have_last && last_rec.nv_pck_lbn != ev.vobu)
                av_log(sc->log, AV_LOG_DEBUG, "scan: still at sector %"PRIu32" after NAV pack %"PRIu32"\n",
                       ev.vobu, last_rec.nv_pck_lbn);
            if (ev.still_time != 0xff) {
                if ((ret = nav_step(sc, ctx, &ev)) < 0)
                    goto end;
                continue;
            }
            if (have_last && have_hli && highlight_active(&last_rec, &hli) &&
                !(have_explored && same_highlight(&hli, &explored))) {
                ScanPCI h = hli;

                if ((ret = press_buttons(sc, ctx, key, &ev, &h)) < 0)
                    goto end;
            }
            goto done;
        case DVDNAV_VTS_CHANGE:
            if ((ret = flush(sc, key)) < 0)
                goto end;
            have_hli = 0;
            break;
        case DVDNAV_CELL_CHANGE:
            if (++cell_changes > PATH_CELL_CHANGES)
                goto done;
            c_title = ev.title_id;
            c_pgc   = ev.pgc_id;
            c_vtsn  = ev.vtsn;
            c_cell  = ev.cell_id;
            pending_cell = 1;
            break;
        case DVDNAV_NAV_PACKET: {
            uint32_t dt = sc->start_time ? ev.time_ms - sc->start_time : 0;
            int menu = ev.title_id == 0, same, seen_vobu = 0;
            uint32_t g;
            uint64_t hk;

            if (!menu && dt > SETTLE_MS && ev.title_id != 0xffff && !sc->snapshot && !sc->no_snapshot &&
                (ret = maybe_snapshot(sc, ctx, &ev)) < 0)
                goto end;
            g = (uint32_t)ev.pgc_id << 16 | (menu ? MENU_VOB : 0) | ev.vtsn;
            same = g == group;
            group = g;
            if (!nav_record(sc, &ev, menu, &rec)) {
                have_last = 0;
                if (!same)
                    have_hli = 0;
                if ((ret = nav_step(sc, ctx, &ev)) < 0)
                    goto end;
                continue;
            }
            last_rec  = rec;
            have_last = 1;
            have_cand = 0;
            if (rec.hli_ss == 1) {
                cand = rec;
                have_cand = 1;
            } else if (rec.hli_ss && same && have_hli) {
                cand = hli;
                have_cand = 1;
            }
            if (have_cand && cand.btn_ns) {
                int b = cand.fosl_btnn ? (cand.btn_ns < cand.fosl_btnn ? 0 : cand.fosl_btnn)
                                       : ev.highlight > cand.btn_ns ? cand.btn_ns : 0;

                if (b && nav_set(sc, ctx, PARAM_HIGHLIGHT, b) < 0)
                    av_log(sc->log, AV_LOG_DEBUG, "scan: highlighting button %d failed\n", b);
            }
            hk = HIGHLIGHT_KEY << 56 | (uint64_t)sc->region << 48 | (uint64_t)menu << 40 |
                 (uint64_t)ev.vtsn << 32 | ev.vobu;
            for (int i = 0; i < nb_seen && !seen_vobu; i++)
                seen_vobu = seen[i] == ev.vobu;
            if (have_explored && seen_vobu && known(sc, hk))
                goto done;
            if (!have_cand) {
                have_hli = 0;
                if ((ret = nav_step(sc, ctx, &ev)) < 0)
                    goto end;
                continue;
            }
            hli      = cand;
            have_hli = 1;
            if (highlight_active(&rec, &cand) && rec.vobu_e_ptm - cand.hli_s_ptm >= HIGHLIGHT_ACTIVE_TICKS &&
                dt > SETTLE_MS && !(have_explored && same_highlight(&cand, &explored))) {
                if (known(sc, hk))
                    goto done;
                if ((ret = visited_put(sc, hk, key)) < 0)
                    goto end;
                if (!av_dynarray2_add((void **)&seen, &nb_seen, sizeof(*seen), (const uint8_t *)&ev.vobu)) {
                    ret = AVERROR(ENOMEM);
                    goto end;
                }
                if ((ret = press_buttons(sc, ctx, key, &ev, &cand)) < 0)
                    goto end;
                explored      = cand;
                have_explored = 1;
            }
            break;
        }
        case DVDNAV_STOP:
            if (!sc->snapshot) {
                int n = nav_copy(sc, ctx);

                if (n < 0) {
                    ret = n;
                    goto end;
                }
                sc->snapshot       = n;
                sc->snapshot_title = 0;
            }
            goto done;
        case DVDNAV_NOP:
        case DVDNAV_SPU_STREAM_CHANGE:
        case DVDNAV_AUDIO_STREAM_CHANGE:
        case DVDNAV_HIGHLIGHT:
        case DVDNAV_SPU_CLUT_CHANGE:
        case DVDNAV_HOP_CHANNEL:
        case DVDNAV_WAIT:
            break;
        default:
            ret = fail(sc, "navigator %d: unexpected event %d", ctx, ev.code);
            goto end;
        }
        if ((ret = nav_step(sc, ctx, &ev)) < 0)
            goto end;
    }
done:
    if ((ret = flush(sc, key)) < 0)
        goto end;
    if (ev.sprm_flags & (1 << 20))
        sc->reads_region = 1;
end:
    av_free(seen);
    return ret;
}

/* Walk every pending path (the last added first). */
static int walk(Scan *sc)
{
    while (sc->nb_pending) {
        Pending p = sc->pending[--sc->nb_pending];
        int ret;

        if (visited_get(sc, p.key))
            continue;
        if ((ret = visited_put(sc, p.key, p.parent)) < 0)
            return ret;
        if ((ret = walk_path(sc, p.key, p.nav)) < 0) {
            av_log(sc->log, AV_LOG_DEBUG, "scan: the path %u:%02u:%08X:%02u failed (region %d)\n",
                   (unsigned)(p.key >> 48 & 0xff), (unsigned)(p.key >> 32 & 0xffff), (unsigned)p.key,
                   (unsigned)(p.key >> 56), sc->region);
            return ret;
        }
    }
    return 0;
}

/* One region pass: First Play, then every title from the snapshot. */
static int region_pass(Scan *sc, int r)
{
    static const int32_t english = 'e' | 'n' << 8;
    uint64_t k0 = region_root(r);
    int n, ret;

    if ((n = nav_copy(sc, sc->root)) < 0)
        return n;
    if ((ret = nav_set(sc, n, PARAM_REGION_MASK, 1 << r)) < 0)
        return ret;
    sc->region         = r;
    sc->snapshot       = 0;
    sc->snapshot_title = 0;
    sc->start_time     = 0;
    sc->reads_region   = 0;
    if ((ret = nav_set(sc, n, PARAM_MENU_LANGUAGE, english)) < 0 ||
        (ret = nav_set(sc, n, PARAM_AUDIO_LANGUAGE, english)) < 0 ||
        (ret = nav_set(sc, n, PARAM_SPU_LANGUAGE, english)) < 0)
        return ret;
    if (!known(sc, k0) && (ret = add_pending(sc, k0, k0, n)) < 0)
        return ret;
    if ((ret = walk(sc)) < 0)
        return ret;
    if (!sc->snapshot) {
        av_log(sc->log, AV_LOG_DEBUG, "scan: region %d: no title snapshot, titles not started one by one\n", r + 1);
        return 0;
    }
    for (int t = 1; t <= nr_of_titles(sc); t++) {
        uint64_t kt;
        int c;

        if (t == sc->snapshot_title)
            continue;
        kt = (uint64_t)(t - 1) << 56 | (uint64_t)r << 48 | 0xfffffffe;
        if ((c = nav_copy(sc, sc->snapshot)) < 0)
            return c;
        if ((ret = nav_set(sc, c, PARAM_TITLE, t)) < 0)
            return ret;
        if (!known(sc, kt) && (ret = add_pending(sc, kt, k0, c)) < 0)
            return ret;
    }
    return walk(sc);
}

/* Steps from a key back to a region start (title starts count 1000). */
static int key_depth(Scan *sc, uint64_t k)
{
    int n = 0;

    if ((uint32_t)k == 0xffffffff)
        return 0;
    for (;;) {
        VisitedEntry *v;

        if ((uint32_t)k == 0xfffffffe)
            n += 1000;
        if (!(v = visited_get(sc, k)))
            return n;
        k = v->parent;
        n++;
        if ((uint32_t)k == 0xffffffff)
            return n;
    }
}

/* The order results are chosen in: by title and PGC, then the longest
 * sequence, then (same start) the sequence bytes, else the start reached in
 * fewest steps, then the start's key; equal records keep their order. */
static int record_cmp(const void *pa, const void *pb)
{
    const PathRecord *a = pa, *b = pb;

    if (a->title != b->title)
        return a->title - b->title;
    if (a->pgc != b->pgc)
        return a->pgc - b->pgc;
    if (a->nb_cells != b->nb_cells)
        return b->nb_cells - a->nb_cells;
    if (a->key == b->key) {
        int c = memcmp(a->cells, b->cells, a->nb_cells);

        if (c)
            return c;
    } else {
        uint64_t oa = key_ord(a->key), ob = key_ord(b->key);

        if (a->depth != b->depth)
            return a->depth - b->depth;
        if (oa != ob)
            return oa < ob ? -1 : 1;
    }
    return a->index - b->index;
}

static int ord_cmp(const void *pa, const void *pb)
{
    uint64_t a = key_ord(*(const uint64_t *)pa), b = key_ord(*(const uint64_t *)pb);

    return (a > b) - (a < b);
}

/* The best record of each (title, PGC), named after its start: letters for
 * the start position, then the button / title number. */
static int make_results(Scan *sc, DVDVideoScan *out)
{
    uint64_t *keys = NULL;
    int nb_keys = 0, nb_kept = 0, g_title = 0, g_pgc = 0, ret = 0;
    PathRecord **kept = NULL;

    for (int i = 0; i < sc->nb_records; i++)
        sc->records[i].depth = key_depth(sc, sc->records[i].key);
    qsort(sc->records, sc->nb_records, sizeof(*sc->records), record_cmp);
    if (sc->nb_records && (!(keys = av_malloc_array(sc->nb_records, sizeof(*keys))) ||
                           !(kept = av_malloc_array(sc->nb_records, sizeof(*kept))))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < sc->nb_records; i++) {
        PathRecord *r = &sc->records[i];
        uint64_t k = r->key & 0x00ffffffffffffffULL;
        int have = 0;

        if (r->title == g_title && r->pgc == g_pgc)
            continue;
        for (int j = 0; j < nb_keys && !have; j++)
            have = key_ord(keys[j]) == key_ord(k);
        if (!have) {
            keys[nb_keys++] = k;
            qsort(keys, nb_keys, sizeof(*keys), ord_cmp);
        }
        g_title = r->title;
        g_pgc   = r->pgc;
        kept[nb_kept++] = r;
    }
    if (nb_kept && !(out->results = av_calloc(nb_kept, sizeof(*out->results)))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < nb_kept; i++) {
        DVDVideoScanResult *res = &out->results[i];
        PathRecord *r = kept[i];
        char *name = res->name, num[16];
        int len = 0;

        if (!nb_keys) {
            name[len++] = 'A';
        } else {
            uint64_t want = key_ord(r->key & 0x00ffffffffffffffULL);
            int k = nb_keys;

            for (int j = 0; j < nb_keys; j++)
                if (key_ord(keys[j]) == want) {
                    k = j;
                    break;
                }
            for (;;) {
                int old = k;

                if (len < (int)sizeof(res->name) - 5)
                    name[len++] = 'A' + k % 26;
                k /= 26;
                if (old <= 25)
                    break;
            }
        }
        snprintf(num, sizeof(num), "%u", (unsigned)(r->key >> 56) + 1);
        num[3] = 0;
        memcpy(name + len, num, strlen(num) + 1);
        res->title    = r->title;
        res->pgcn     = r->pgc;
        res->cells    = r->cells;
        res->nb_cells = r->nb_cells;
        r->cells = NULL;
        out->nb_results++;
    }
end:
    av_free(keys);
    av_free(kept);
    return ret;
}

/* ---- setting up ---- */

/* Where the IFO puts a VOB group against the files on an image: a warning
 * when they start at different blocks, or when the files reach past the
 * backup IFO. Returns the absolute start of the title VOBs when both
 * agree (and they are one run of blocks), else 0. */
static uint32_t check_vob_layout(Scan *sc, int vtsn)
{
    const ifo_handle_t *ifo = sc->ifo[vtsn];
    char name[32], what_set[32];
    int64_t ifo_sector, sectors, image_sector;
    uint32_t base = 0;

    snprintf(name, sizeof(name), vtsn ? "VTS_%02d_0.IFO" : "VIDEO_TS.IFO", vtsn);
    snprintf(what_set, sizeof(what_set), vtsn ? "Title set %d" : "The video manager", vtsn);
    if ((ifo_sector = ff_dvdvideo_source_file_sector(sc->src, name)) < 0)
        return 0;   /* a folder: the files are all there is */
    for (int menu = 1; menu >= (vtsn ? 0 : 1); menu--) {
        uint32_t bup = vtsn ? ifo->vtsi_mat->vts_last_sector - (ifo->vtsi_mat->vtsi_last_sector & 0x1ffff)
                            : ifo->vmgi_mat->vmg_last_sector - (ifo->vmgi_mat->vmgi_last_sector & 0x1ffff);
        uint32_t from = vtsn ? (menu ? ifo->vtsi_mat->vtsm_vobs : ifo->vtsi_mat->vtstt_vobs)
                             : ifo->vmgi_mat->vmgm_vobs;
        uint32_t to   = vtsn && menu && ifo->vtsi_mat->vtstt_vobs > from ? ifo->vtsi_mat->vtstt_vobs : bup;
        const char *what = menu ? "menu VOBs" : "title VOBs";
        int ret = ff_dvdvideo_source_vob_layout(sc->src, vtsn, menu, &sectors, &image_sector);

        if (!from || ret == AVERROR(ENOENT))
            continue;
        if (ret < 0 || image_sector < 0) {
            av_log(sc->log, AV_LOG_WARNING, "%s: the %s are not stored as one run of image blocks\n",
                   what_set, what);
            continue;
        }
        if (ifo_sector + from != image_sector)
            av_log(sc->log, AV_LOG_WARNING, "%s: the IFO puts its %s at image block %"PRId64", the file "
                   "system at block %"PRId64"\n", what_set, what, ifo_sector + from, image_sector);
        else if (!menu)
            base = image_sector;
        /* the IFO's range ends where the backup IFO starts; authoring can
         * leave unrecorded blocks before it (16 or fewer on the corpus), so
         * only files reaching past it are a disagreement */
        if (to > from && sectors > to - from)
            av_log(sc->log, AV_LOG_WARNING, "%s: the files of its %s hold %"PRId64" blocks, %"PRId64" more than "
                   "the IFO leaves before the backup IFO\n", what_set, what, sectors, sectors - (to - from));
        else if (to > from && sectors < to - from)
            av_log(sc->log, AV_LOG_DEBUG, "scan: %s: %"PRId64" blocks between the end of its %s and the backup "
                   "IFO\n", what_set, to - from - sectors, what);
    }
    return base;
}

/* The title VOBs' sector range of title set vts (from the title search
 * table's start of the title set + vtstt_vobs, up to where the backup IFO
 * starts); 0 when there is none. */
static int title_vob_range(Scan *sc, int vts, uint32_t *s, uint32_t *e)
{
    const ifo_handle_t *o = sc->ifo[vts];
    const tt_srpt_t *tt = sc->ifo[0]->tt_srpt;
    uint32_t tv, last, ifo_last, b, base = 0;

    if (!o)
        return 0;
    tv       = o->vtsi_mat->vtstt_vobs;
    last     = o->vtsi_mat->vts_last_sector;
    ifo_last = o->vtsi_mat->vtsi_last_sector & 0x1ffff;
    if (last <= ifo_last || (b = last - ifo_last) <= tv)
        return 0;
    /* the start of the title set: its first title that is not empty */
    for (int i = 0; tt && i < tt->nr_of_srpts; i++) {
        const title_info_t *t = &tt->title[i];

        if (!t->title_set_nr || !t->vts_ttn || !t->nr_of_ptts)
            continue;
        if (t->title_set_nr == vts) {
            base = t->title_set_sector;
            break;
        }
    }
    *s = base + tv;
    *e = base + b;
    return 1;
}

/* Whether the title VOBs of title set vts overlap those of another one. */
static int title_vobs_overlap(Scan *sc, int vts)
{
    uint32_t s, e, s2, e2;

    if (!title_vob_range(sc, vts, &s, &e))
        return 0;
    for (int k = 1; k <= sc->nb_vts; k++)
        if (k != vts && title_vob_range(sc, k, &s2, &e2) && ((s <= s2 && s2 < e) || (s2 <= s && s < e2)))
            return 1;
    return 0;
}

static void scan_close(Scan *sc)
{
    /* copies share navigator 0's reader: they go first */
    for (int i = sc->nb_navs - 1; i >= 1; i--)
        dvdnav_free_dup(sc->navs[i]);
    if (sc->nb_navs)
        dvdnav_close(sc->navs[0]);
    av_log(sc->log, AV_LOG_DEBUG, "scan ended: %d navigators, %"PRIu64" sectors read\n", sc->nb_navs, sc->nb_reads);
    av_freep(&sc->navs);
    for (int i = 0; i < 100; i++)
        if (sc->ifo[i])
            ifoClose(sc->ifo[i]);
    trace_requests(sc);
    map_free(&sc->sectors);
    map_free(&sc->pcis);
    map_free(&sc->vobus);
    map_free(&sc->visited);
    av_freep(&sc->pending);
    av_freep(&sc->cells);
    for (int i = 0; i < sc->nb_records; i++)
        av_free(sc->records[i].cells);
    av_freep(&sc->records);
}

void ff_dvdvideo_scan_free(DVDVideoScan **pscan)
{
    DVDVideoScan *scan = *pscan;

    if (!scan)
        return;
    for (int i = 0; i < scan->nb_results; i++)
        av_free(scan->results[i].cells);
    av_free(scan->results);
    av_freep(pscan);
}

static void scan_libdvdread_log(void *opaque, dvd_logger_level_t level, const char *msg, va_list args)
{
    char buf[DVDVIDEO_LIBDVDX_LOG_BUFFER_SIZE];

    vsnprintf(buf, sizeof(buf), msg, args);
    av_log(opaque, level <= DVD_LOGGER_LEVEL_WARN ? AV_LOG_DEBUG : AV_LOG_TRACE, "scan: libdvdread: %s\n", buf);
}

int ff_dvdvideo_scan(void *log, DVDVideoSource *src, DVDVideoScanTrace trace, void *trace_opaque,
                     DVDVideoScan **out)
{
    Scan sc = { .log = log, .src = src, .trace = trace, .trace_opaque = trace_opaque };
    dvd_logger_cb log_cb = { .pf_log = scan_libdvdread_log };
    dvd_reader_filesystem_h *files;
    dvd_reader_t *dvdread = NULL;
    DVDVideoScan *scan;
    uint32_t prohibited;
    int overlapping = 0, ret;

    *out = NULL;
    if (!(scan = av_mallocz(sizeof(*scan))))
        return AVERROR(ENOMEM);
    ff_dvdvideo_rand_init(&sc.rand, 1);

    /* the IFOs, read as the demuxer reads them */
    if (!(files = ff_dvdvideo_source_files(src))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if (!(dvdread = DVDOpenFiles(log, &log_cb, "/", files))) {
        files->close(files);
        av_log(log, AV_LOG_ERROR, "The navigation scan cannot open the DVD-Video structure\n");
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    if (!(sc.ifo[0] = ifoOpen(dvdread, 0))) {
        av_log(log, AV_LOG_ERROR, "The navigation scan cannot open the VMG (VIDEO_TS.IFO)\n");
        ret = AVERROR_EXTERNAL;
        goto end;
    }
    sc.nb_vts = FFMIN(sc.ifo[0]->vmgi_mat->vmg_nr_of_title_sets, 99);
    for (int n = 1; n <= sc.nb_vts; n++)
        if (!(sc.ifo[n] = ifoOpen(dvdread, n)))
            av_log(log, AV_LOG_DEBUG, "scan: VTS_%02d_0.IFO is not open, not given to the navigator\n", n);
    check_vob_layout(&sc, 0);
    for (int n = 1; n <= sc.nb_vts; n++)
        if (sc.ifo[n])
            sc.title_vobs_base[n] = check_vob_layout(&sc, n);
    for (int n = 1; n < sc.nb_vts && !overlapping; n++)
        overlapping = title_vobs_overlap(&sc, n);
    sc.no_snapshot = overlapping ? nr_of_titles(&sc) >= 16 : nr_of_titles(&sc) > 0x60;

    if ((ret = nav_open(&sc)) < 0 || (sc.root = nav_copy(&sc, 0)) < 0)
        goto done;
    prohibited = sc.ifo[0]->vmgi_mat->vmg_category;
    for (int r = 0; r < 8; r++) {
        if (prohibited & (0x10000 << r))
            continue;
        if ((ret = region_pass(&sc, r)) < 0) {
            av_log(log, AV_LOG_DEBUG, "scan: the pass of region %d failed\n", r + 1);
            goto done;
        }
        if (r < 7 && !sc.reads_region)
            break;
    }
    ret = make_results(&sc, scan);

done:
    if (sc.failed) {
        av_log(log, AV_LOG_WARNING, "The navigation scan failed: %s\n", sc.failure);
        scan->failed = 1;
        av_strlcpy(scan->failure, sc.failure, sizeof(scan->failure));
        ret = 0;
    }
    memcpy(scan->entered, sc.entered, sizeof(scan->entered));
end:
    scan_close(&sc);
    if (dvdread)
        DVDClose(dvdread);
    if (ret < 0)
        ff_dvdvideo_scan_free(&scan);
    *out = scan;
    return ret;
}
