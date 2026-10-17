/*
 * Blu-ray: the disc's titles, built with the reference program's rules
 */

/*
 * A title is a list of segments; a segment is one STC sequence of a clip
 * (the whole sequence: its duration and bytes count, not the PlayItem's IN
 * / OUT times) or a run of them joined seamlessly (connection condition 6).
 * The order of the steps (and so of the titles) follows the reference:
 * 1. play items merged: two neighbouring items of the same clip and STC
 *    sequence joined by connection condition 1 or 5, the second starting
 *    within 35 s after the first ends, with compatible stream tables;
 *    neighbours with connection condition 1 that each cover their whole STC
 *    sequence (within 269 ticks) and have compatible stream tables are
 *    marked to join without starting a new title (0x7f); such neighbours
 *    with connection condition 5 keep it and are marked as a place the title
 *    may be split into parts (whole_join);
 * 2. each sequential playlist, in listing order: one title per part (a part
 *    ends at a connection condition other than 5, 6 or the mark), per angle;
 *    repeated blocks at the end of a part and runs of 7 or more identical
 *    items are trimmed; a part of one clip without chapters becomes no
 *    title (its clip may get a title of its own); the title keeps the
 *    streams every item has; the playlists a BD-J menu's project data names
 *    (bluray_menu.c) go first, in listing order, and a part of one clip of
 *    theirs keeps its title;
 *    BDAV playlists (and AVCHD playlists with maker data): titles for their
 *    seamless runs, else one for the whole playlist;
 * 3. one title per clip (application type 1) that no title uses with all
 *    its streams;
 * 4. titles equal to an earlier one are left out.
 * A title is accepted when it lasts the minimum length (playlist and clip
 * titles), is no duplicate of an accepted title and does not start with a
 * dependent-view (MVC) stream. Still to come with the tracks step: the
 * reference's stream check of a title (its first frames are read) and the
 * repair of a clip's stream list from its stream file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/str.h"
#include "base/macros.h"
#include "base/bytes.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "bluray_internal.h"

#define TOLERANCE   0x10d       /* ticks a PlayItem may lie off its STC sequence's start / end */
#define MERGE_GAP   0x180884    /* ticks between two items of a clip that may still be merged (35 s) */
#define MARK_UNIT   24000       /* 45 kHz ticks -> 1/1,080,000,000 s */
#define CC_JOINED   0x7f        /* a connection 1 found to join cleanly */
#define CODING_MVC  0x20
#define CODING_PG   0x90
#define CODING_TEXT 0x92

/* ---- small vectors ---- */

typedef struct U16Set {
    uint16_t *v;
    int       n, cap;
} U16Set;

static int u16_insert(U16Set *s, uint16_t x)
{
    int lo = 0, hi = s->n;

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s->v[mid] < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < s->n && s->v[lo] == x)
        return 0;
    if (s->n == s->cap) {
        uint16_t *v = dr_realloc_array(s->v, s->cap ? 2 * s->cap : 16, sizeof(*v));
        if (!v)
            return DR_ERR(ENOMEM);
        s->v   = v;
        s->cap = s->cap ? 2 * s->cap : 16;
    }
    memmove(s->v + lo + 1, s->v + lo, (s->n - lo) * sizeof(*s->v));
    s->v[lo] = x;
    s->n++;
    return 0;
}

static int u16_has(const U16Set *s, uint16_t x)
{
    int lo = 0, hi = s->n;

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s->v[mid] < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < s->n && s->v[lo] == x;
}

/* The PIDs titles use, per clip number. */
typedef struct ClipPids {
    uint32_t *clip;
    U16Set   *pids;
    int       n;
} ClipPids;

static U16Set *clip_pids(ClipPids *m, uint32_t clip, int create)
{
    for (int i = 0; i < m->n; i++)
        if (m->clip[i] == clip)
            return &m->pids[i];
    if (!create)
        return NULL;
    {
        uint32_t *c = dr_realloc_array(m->clip, m->n + 1, sizeof(*c));
        U16Set *p;
        if (!c)
            return NULL;
        m->clip = c;
        if (!(p = dr_realloc_array(m->pids, m->n + 1, sizeof(*p))))
            return NULL;
        m->pids = p;
        m->clip[m->n] = clip;
        memset(&m->pids[m->n], 0, sizeof(*p));
        return &m->pids[m->n++];
    }
}

typedef struct Ctx {
    void       *log;
    BlurayDisc *d;
    int         min_length;
    ClipPids    used;         /* PIDs per clip the accepted titles use */
    uint32_t   *clipset;      /* clips a title should be made for (sorted, unique) */
    int         nb_clipset;
    int         avchd_warned;
    /* playlist parts that are one piece of one clip: said once the title
     * list is done */
    struct OneClip { uint32_t pl, clip, len; int part; } *one;
    int         nb_one;
} Ctx;

static int clipset_add(Ctx *c, uint32_t id)
{
    int i = 0;
    uint32_t *v;

    while (i < c->nb_clipset && c->clipset[i] < id)
        i++;
    if (i < c->nb_clipset && c->clipset[i] == id)
        return 0;
    if (!(v = dr_realloc_array(c->clipset, c->nb_clipset + 1, sizeof(*v))))
        return DR_ERR(ENOMEM);
    c->clipset = v;
    memmove(v + i + 1, v + i, (c->nb_clipset - i) * sizeof(*v));
    v[i] = id;
    c->nb_clipset++;
    return 0;
}

static int clipset_has(const Ctx *c, uint32_t id)
{
    for (int i = 0; i < c->nb_clipset; i++)
        if (c->clipset[i] == id)
            return 1;
    return 0;
}

/* ---- clips and STC sequences ---- */

static BlurayClip *clip_by_name(BlurayDisc *d, const char *name)
{
    char b[6];
    uint32_t n;

    if (!name)
        return NULL;
    memcpy(b, name, 5);
    b[5] = 0;
    n = strtoul(b, NULL, 10);
    for (int i = 0; i < d->nb_clips; i++)
        if (d->clips[i].number == n)
            return &d->clips[i];
    return NULL;
}

/* The STC sequence of an id: the ATC sequences' ids must be contiguous
 * (a gap marks the clip, and no lookup succeeds from then on). */
static const BlurayStc *stc_lookup(void *log, BlurayClip *c, unsigned id)
{
    if (c->stc_gap)
        return NULL;
    for (int g = 0; g + 1 < c->nb_atc; g++)
        if (c->atc[g].offset_stc_id + c->atc[g].nb_stc != c->atc[g + 1].offset_stc_id) {
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: clip %05u: STC ids not contiguous\n", c->number);
            c->stc_gap = 1;
            return NULL;
        }
    for (int g = 0; g < c->nb_atc; g++)
        if (id >= c->atc[g].offset_stc_id && id < (unsigned)c->atc[g].offset_stc_id + c->atc[g].nb_stc)
            return &c->atc[g].stc[id - c->atc[g].offset_stc_id];
    dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: clip %05u has no STC sequence %u\n", c->number, id);
    return NULL;
}

/* Position of an STC sequence in the clip's list of all of them (0xff: not found). */
static int stc_index(const BlurayClip *c, const BlurayStc *s)
{
    int k = 0;
    for (int g = 0; g < c->nb_atc; g++)
        for (int j = 0; j < c->atc[g].nb_stc; j++, k++)
            if (&c->atc[g].stc[j] == s)
                return k;
    return 0xff;
}

/* The STC sequence after s in the clip (the next of its ATC sequence, or
 * the first of the next one), or NULL. */
static const BlurayStc *stc_next(const BlurayClip *c, const BlurayStc *s)
{
    for (int g = 0; g < c->nb_atc; g++)
        for (int j = 0; j < c->atc[g].nb_stc; j++)
            if (&c->atc[g].stc[j] == s) {
                if (j + 1 < c->atc[g].nb_stc)
                    return &c->atc[g].stc[j + 1];
                return g + 1 < c->nb_atc && c->atc[g + 1].nb_stc ? &c->atc[g + 1].stc[0] : NULL;
            }
    return NULL;
}

static const BlurayClipStream *clip_stream(const BlurayClip *c, uint16_t pid)
{
    for (int i = 0; i < c->program.nb_streams; i++)
        if (c->program.streams[i].pid == pid)
            return &c->program.streams[i];
    return NULL;
}

/* ---- stream table comparisons ---- */

/* The attribute bytes as the reference compares them: a coding type, then
 * byte 0x11 (the first attribute byte) and the 3 bytes from 0x12. */
typedef struct Attr {
    uint32_t coding;
    uint8_t  b[33];
} Attr;

static Attr item_attr(const BlurayStnStream *s)
{
    Attr a = { s->coding_type, { 0 } };
    memcpy(a.b, s->attr, 32);
    return a;
}

static Attr clip_attr(const BlurayClipStream *s)
{
    Attr a = { s->coding_type, { 0 } };
    memcpy(a.b, s->info, 32);
    return a;
}

static int code3(const uint8_t *p, char out[4])
{
    memcpy(out, p, 3);
    out[3] = 0;
    return 0;
}

/* coding type, first attribute byte, the high nibble of the second */
static int attr_video(const Attr *a, const Attr *b, int nibble)
{
    if (a->coding != b->coding || a->b[0] != b->b[0])
        return 0;
    return !nibble || (a->b[1] >> 4) == (b->b[1] >> 4);
}

/* coding type, first attribute byte, the 3-byte code after it */
static int attr_coded(const Attr *a, const Attr *b)
{
    char x[4], y[4];
    if (a->coding != b->coding || a->b[0] != b->b[0])
        return 0;
    code3(a->b + 1, x);
    code3(b->b + 1, y);
    return !strcmp(x, y);
}

/* the subtitle language: at byte 1 of a text subtitle, byte 0 of a PG stream */
static void sub_code(const Attr *a, char out[4])
{
    if (a->coding == CODING_TEXT)
        code3(a->b + 1, out);
    else if (a->coding == CODING_PG)
        code3(a->b, out);
    else
        out[0] = 0;
}

static int attr_sub(const Attr *a, const Attr *b)
{
    char x[4], y[4];
    if (a->coding != b->coding)
        return 0;
    sub_code(a, x);
    sub_code(b, y);
    return !strcmp(x, y);
}

static int pid_entry(const BlurayStnStream *s)
{
    return s->entry_type == 1 && s->entry_len >= 2;
}

static uint16_t entry_pid(const BlurayStnStream *s)
{
    return dr_rb16(s->entry);
}

/* Two play items can be read as one stream: the same video, audio and
 * subtitle streams, as their clips describe them too. */
static int items_compatible(const BlurayPlayItem *p1, const BlurayClip *c1,
                            const BlurayPlayItem *p2, const BlurayClip *c2)
{
    const BlurayClipStream *e1, *e2;
    Attr a1, a2, x1, x2;

    if (!pid_entry(&p1->video) || !pid_entry(&p2->video) || entry_pid(&p1->video) != entry_pid(&p2->video) ||
        p1->video.coding_type != p2->video.coding_type || p1->video.attr[0] != p2->video.attr[0])
        return 0;
    e1 = clip_stream(c1, entry_pid(&p1->video));
    e2 = clip_stream(c2, entry_pid(&p2->video));
    if (!e1 || e1->coding_type != p1->video.coding_type || e1->info[0] != p1->video.attr[0] ||
        !e2 || e2->coding_type != p2->video.coding_type || e2->info[0] != p2->video.attr[0])
        return 0;
    x1 = clip_attr(e1);
    x2 = clip_attr(e2);
    if (!attr_video(&x1, &x2, 1))
        return 0;
    if (p1->nb_audio != p2->nb_audio)
        return 0;
    for (int i = 0; i < p1->nb_audio; i++) {
        const BlurayStnStream *s1 = &p1->audio[i], *s2 = &p2->audio[i];
        if (pid_entry(s1)) {
            if (!pid_entry(s2) || entry_pid(s1) != entry_pid(s2))
                return 0;
            a1 = item_attr(s1);
            a2 = item_attr(s2);
            if (!attr_coded(&a1, &a2) || !(e1 = clip_stream(c1, entry_pid(s1))) ||
                !(e2 = clip_stream(c2, entry_pid(s2))))
                return 0;
            x1 = clip_attr(e1);
            x2 = clip_attr(e2);
            if (!attr_coded(&a1, &x1) || !attr_coded(&a2, &x2) || !attr_coded(&x1, &x2))
                return 0;
        } else if (pid_entry(s2)) {
            return 0;
        }
    }
    if (p1->nb_pg != p2->nb_pg)
        return 0;
    for (int i = 0; i < p1->nb_pg; i++) {
        const BlurayStnStream *s1 = &p1->pg[i], *s2 = &p2->pg[i];
        if (pid_entry(s1)) {
            char k1[4], k2[4], kc[4];
            if (!pid_entry(s2) || entry_pid(s1) != entry_pid(s2) || s1->coding_type != s2->coding_type)
                return 0;
            a1 = item_attr(s1);
            a2 = item_attr(s2);
            sub_code(&a1, k1);
            sub_code(&a2, k2);
            if (strcmp(k1, k2) || !(e1 = clip_stream(c1, entry_pid(s1))) || !(e2 = clip_stream(c2, entry_pid(s2))))
                return 0;
            x1 = clip_attr(e1);
            x2 = clip_attr(e2);
            if (x1.coding != s1->coding_type || (sub_code(&x1, kc), strcmp(k1, kc)) ||
                x2.coding != s2->coding_type || (sub_code(&x2, kc), strcmp(k2, kc)) || !attr_sub(&x1, &x2))
                return 0;
        } else if (pid_entry(s2)) {
            return 0;
        }
    }
    return 1;
}

/* The PIDs of an item's video, audio and subtitle streams of its own clip;
 * 0 when its video entry is no PID entry. */
static int item_pids(const BlurayPlayItem *it, U16Set *out)
{
    out->n = 0;
    if (!pid_entry(&it->video))
        return 0;
    if (u16_insert(out, entry_pid(&it->video)) < 0)
        return DR_ERR(ENOMEM);
    for (int i = 0; i < it->nb_audio; i++)
        if (pid_entry(&it->audio[i]) && u16_insert(out, entry_pid(&it->audio[i])) < 0)
            return DR_ERR(ENOMEM);
    for (int i = 0; i < it->nb_pg; i++)
        if (pid_entry(&it->pg[i]) && u16_insert(out, entry_pid(&it->pg[i])) < 0)
            return DR_ERR(ENOMEM);
    return 1;
}

/* The PIDs every item has. */
static int common_pids(void *log, const BlurayPlayItem *const *items, int n, U16Set *out)
{
    U16Set tmp = { 0 };
    int ret;

    out->n = 0;
    if (!n)
        return 0;
    if ((ret = item_pids(items[0], out)) <= 0)
        return ret;
    for (int k = 1; k < n; k++) {
        if ((ret = item_pids(items[k], &tmp)) <= 0) {
            dr_free(tmp.v);
            return ret;
        }
        for (int i = 0; i < out->n;)
            if (u16_has(&tmp, out->v[i])) {
                i++;
            } else {
                memmove(out->v + i, out->v + i + 1, (out->n - i - 1) * sizeof(*out->v));
                out->n--;
                i = 0;
            }
    }
    dr_free(tmp.v);
    return 1;
}

/* ---- segment records ---- */

typedef struct Rec {
    BlurayClip           *clip;
    const BlurayStc      *stc;
    BlurayClip           *dep;
    const BlurayPlayItem *item;
    int                   connection;
} Rec;

typedef struct Recs {
    Rec *v;
    int  n, cap;
} Recs;

static int recs_push(Recs *r, const Rec *x)
{
    if (r->n == r->cap) {
        Rec *v = dr_realloc_array(r->v, r->cap ? 2 * r->cap : 8, sizeof(*v));
        if (!v)
            return DR_ERR(ENOMEM);
        r->v   = v;
        r->cap = r->cap ? 2 * r->cap : 8;
    }
    r->v[r->n++] = *x;
    return 0;
}

/* The record of play item idx (angle 0 = its own clip): its clip (one ATC
 * sequence only), its STC sequence and, for a stereoscopic playlist
 * (extension SubPath of type 8 with one SubPlayItem per play item), the
 * dependent-view clip. 0 when the item cannot be used (why, if not NULL,
 * says why). */
static int item_record(void *log, BlurayDisc *d, const BlurayPlaylist *pl, int idx, int angle, Rec *rec,
                       char *why, size_t why_size)
{
    const BlurayPlayItem *it = &pl->items[idx];
    const char *name = angle == 0 ? it->clip : angle <= it->nb_angles ? it->angles[angle - 1].clip : NULL;
    uint8_t stc_id;
    const BluraySubPlayItem *sp;

    memset(rec, 0, sizeof(*rec));
    rec->item = it;
    if (!(rec->clip = clip_by_name(d, name))) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: playlist %05u item %d: no clip %.5s\n", pl->number, idx,
               name ? name : "-");
        if (why)
            snprintf(why, why_size, "clip %.5s has no clip information (its .clpi file is missing or cannot be read)",
                     name ? name : "-");
        return 0;
    }
    if (rec->clip->nb_atc != 1) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: clip %05u has %d ATC sequences\n", rec->clip->number, rec->clip->nb_atc);
        if (why)
            snprintf(why, why_size, "clip %05u has %d ATC sequences (a title takes clips with one)",
                     rec->clip->number, rec->clip->nb_atc);
        return 0;
    }
    stc_id = angle == 0 ? it->stc_id : it->angles[angle - 1].stc_id;
    if (!(rec->stc = stc_lookup(log, rec->clip, stc_id))) {
        if (why)
            snprintf(why, why_size, "clip %05u has no usable STC sequence %u", rec->clip->number, stc_id);
        return 0;
    }
    rec->connection = it->connection;
    if (pl->ext_subpath_type == 0xffff)
        return 1;
    if (pl->ext_subpath_type != 8 || pl->nb_items != pl->ext_subpath.nb_items) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: playlist %05u: SubPath extension of type %d not used\n", pl->number,
               pl->ext_subpath_type);
        return 1;
    }
    sp = &pl->ext_subpath.items[idx];
    if (sp->sync_play_item != idx || sp->nb_clips != it->nb_angles) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: playlist %05u item %d: dependent view does not match\n", pl->number, idx);
        return 1;
    }
    if (angle == 0) {
        if (sp->stc_id || it->stc_id)
            return 1;
        rec->dep = clip_by_name(d, sp->clip);
    } else {
        if (sp->clips[angle - 1].stc_id || it->angles[angle - 1].stc_id)
            return 1;
        rec->dep = clip_by_name(d, sp->clips[angle - 1].clip);
    }
    if (it->connection != CC_JOINED && it->connection != sp->connection)
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: playlist %05u item %d: connection %d, its dependent view's %d\n",
               pl->number, idx, it->connection, sp->connection);
    return 1;
}

/* ---- segments ---- */

/* Sectors of the 2048-byte block at byte pos of a disc file (0: not known,
 * e.g. a file of a disc folder). */
static uint32_t file_block(DiscIOFS *fs, const char *path, int64_t pos)
{
    DiscIOFile *f;
    uint32_t b = 0;
    int64_t at = 0;

    if (fs->ops->open_file(fs, path, &f) < 0)
        return 0;
    for (int i = 0; i < f->nb_extents && !b; i++) {
        int64_t bytes = (int64_t)f->extents[i].count * DISCIO_BLOCK_SIZE;
        if (pos < at + bytes && f->extents[i].sector != DISCIO_SECTOR_NOT_RECORDED)
            b = f->extents[i].sector + (pos - at) / DISCIO_BLOCK_SIZE;
        at += bytes;
    }
    ff_discio_file_free(&f);
    return b;
}

/* X's first extent is followed on the disc by Y's, and X's file spans both
 * files' sectors (a block that cannot be looked up counts as yes). */
static int physically_interleaved(DiscIOFS *fs, const BlurayClip *x, const BlurayClip *y)
{
    uint32_t x0 = file_block(fs, x->stream_path, 0), xe = file_block(fs, x->stream_path, x->stream_size - 2048);
    uint32_t y0, ye, e1;

    if (!x0 || !xe)
        return 1;
    y0 = file_block(fs, y->stream_path, 0);
    ye = file_block(fs, y->stream_path, y->stream_size - 2048);
    if (!y0 || !ye)
        return 1;
    if (x0 >= y0)
        return 0;
    e1 = x->nb_ss_extents != 1 ? x->ss_extents[1] : x->packets;
    return y0 == x0 + (e1 >> 5) * 3 && xe - x0 + 1 == ((x->packets >> 5) + (y->packets >> 5)) * 3;
}

/* The byte length of the base clip a and its dependent view c read
 * interleaved by their extent start points; 0 when they cannot be. */
static int64_t interleaved_length(void *log, DiscIOFS *fs, BlurayClip *a, BlurayClip *c)
{
    BlurayClip *first, *second;
    int64_t len = 0;

    if (c->interleave == 1 || !a->stream_path[0] || !c->stream_path[0] || !a->nb_ss_extents ||
        a->nb_ss_extents != c->nb_ss_extents)
        return 0;
    if ((a->packets & 0x1f) || (c->packets & 0x1f) || a->ss_extents[0] || c->ss_extents[0])
        return 0;
    if (!c->interleave) {
        if (physically_interleaved(fs, c, a))
            c->interleave = 2;
        else if (physically_interleaved(fs, a, c))
            c->interleave = 3;
        else
            c->interleave = a->stream_size == (int64_t)a->packets * 192 &&
                            c->stream_size == (int64_t)c->packets * 192 ? 2 : 1;
        if (c->interleave == 1) {
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: clips %05u and %05u (a 3-D base view and its second view) cannot "
                   "be read as 3-D (their extents do not interleave on the disc, nor are both stream files whole): "
                   "a title with them reads only the base view\n", a->number, c->number);
            return 0;
        }
    }
    first  = c->interleave == 2 ? c : a;
    second = c->interleave == 2 ? a : c;
    for (int i = 0; i < first->nb_ss_extents; i++) {
        int64_t pf = (int64_t)first->ss_extents[i] * 192, ps = (int64_t)second->ss_extents[i] * 192;
        int64_t ef = i + 1 < first->nb_ss_extents ? (int64_t)first->ss_extents[i + 1] * 192 : (int64_t)first->packets * 192;
        int64_t es = i + 1 < second->nb_ss_extents ? (int64_t)second->ss_extents[i + 1] * 192 : (int64_t)second->packets * 192;
        if (ef <= pf || es <= ps)
            return 0;
        len += ef - pf + es - ps;
    }
    return len;
}

static void segment_free(BluraySegment *s)
{
    dr_freep(&s->items);
    s->nb_items = 0;
}

/* The byte ranges of a segment's items: each STC sequence of its clip
 * file (up to the next one, else to the end of the file); interleaved: the
 * whole base + dependent-view pair. */
static int segment_ranges(void *log, BlurayDisc *d, BluraySegment *s, int interleaved)
{
    for (int k = 0; k < s->nb_items; k++) {
        BluraySegItem *it = &s->items[k];
        const BlurayStc *next;

        if (interleaved) {
            if (!(it->length = interleaved_length(log, d->fs, it->clip, it->dep)))
                return 0;
            it->start = 0;
            continue;
        }
        it->start = (int64_t)it->stc->spn_stc_start * 192;
        if ((next = stc_next(it->clip, it->stc)))
            it->length = ((int64_t)next->spn_stc_start - it->stc->spn_stc_start) * 192;
        else
            it->length = it->clip->stream_size - it->start;
    }
    return 1;
}

/* The segment's path and description (for the log). */
static void segment_names(BluraySegment *s)
{
    const BluraySegItem *a0 = &s->items[0];
    int bdav = a0->clip->hdr.type == DR_MKBETAG('M', '2', 'T', 'S');
    char *p = s->path, *q = s->desc;
    size_t pn = sizeof(s->path), qn = sizeof(s->desc);
    int w;

    w = snprintf(p, pn, "/BD%cV/STREAM/%05u.%s", bdav ? 'A' : 'M', a0->clip->number,
                 a0->clip->stream_type == 2 ? "fmts" : "m2ts");
    for (int k = 1; k < DR_MIN(s->nb_items, 8) && w > 0 && (size_t)w < pn; k++)
        w += snprintf(p + w, pn - w, "+%05u.%s", s->items[k].clip->number,
                      s->items[k].clip->stream_type == 2 ? "fmts" : "m2ts");
    if (s->nb_items > 8 && w > 0 && (size_t)w < pn)
        snprintf(p + w, pn - w, "+...");
    w = 0;
    if (s->nb_items > 1)
        w += snprintf(q + w, qn - w, "(");
    for (int k = 0; k < s->nb_items && (size_t)w < qn; k++) {
        const BluraySegItem *it = &s->items[k];
        w += snprintf(q + w, qn - w, "%u", it->clip->number);
        if ((it->clip->nb_atc > 1 || it->clip->atc[0].nb_stc > 1) && (size_t)w < qn)
            w += snprintf(q + w, qn - w, ":%d", stc_index(it->clip, it->stc));
        if (it->dep && (size_t)w < qn)
            w += snprintf(q + w, qn - w, "/%u", it->dep->number);
        if (k + 1 < s->nb_items && (size_t)w < qn)
            w += snprintf(q + w, qn - w, ",");
    }
    if (s->nb_items > 1 && (size_t)w < qn)
        snprintf(q + w, qn - w, ")");
}

static int title_add_segment(BlurayTitle *t, BluraySegment *s)
{
    BluraySegment *v = dr_realloc_array(t->segs, t->nb_segs + 1, sizeof(*v));
    if (!v)
        return DR_ERR(ENOMEM);
    t->segs = v;
    t->segs[t->nb_segs++] = *s;
    return 0;
}

/* A segment of one PlayItem (not 3-D): an audio stream the PlayItem names
 * that its clip's information lacks is added to the clip (with the
 * PlayItem's attributes) when the stream data has PES packets on its PID
 * (the reference program's repair of such clips). */
static int repair_streams(void *log, BlurayDisc *d, BluraySegment *s)
{
    const BlurayPlayItem *it = s->items[0].item;
    BlurayClip *c = s->items[0].clip;

    for (int i = 0; i < it->nb_audio; i++) {
        const BlurayStnStream *e = &it->audio[i];
        BlurayClipStream *v, *ns;
        uint16_t pid;
        int has = 0, n;
        if (e->entry_type != 1 || e->entry_len < 2)
            continue;
        pid = dr_rb16(e->entry);
        for (int k = 0; k < c->program.nb_streams && !has; k++)
            has = c->program.streams[k].pid == pid;
        if (has)
            continue;
        if ((n = ff_bluray_pid_scan(log, d->fs, d->keys, s, pid, 5)) < 0) {
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: clip %05u: the repair of its streams stops at stream %d\n",
                   c->number, i);
            return 0;
        }
        if (!n)
            continue;
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: clip %05u lacks the stream on PID 0x%04X its playlist names; the "
               "stream data has it: added with the playlist's attributes (the clip information is corrupt)\n",
               c->number, pid);
        if (!(v = dr_realloc_array(c->program.streams, c->program.nb_streams + 1, sizeof(*v))))
            return DR_ERR(ENOMEM);
        c->program.streams = v;
        ns = &v[c->program.nb_streams++];
        memset(ns, 0, sizeof(*ns));
        ns->pid         = pid;
        ns->coding_type = e->coding_type;
        ns->info_len    = e->attr_len;
        memcpy(ns->info, e->attr, DR_MIN(sizeof(ns->info), sizeof(e->attr)));
    }
    return 0;
}

/* One segment of records r[0..n): interleaved (3-D) when every record has a
 * dependent-view clip of application type 8 whose first stream is MVC
 * video, if that can be read, else from the base clips. 1, 0 failed. */
static int build_segment(void *log, BlurayDisc *d, BlurayTitle *t, const Rec *r, int n)
{
    BluraySegment s = { 0 };
    int mode3d = 0, ret;

    if (!n)
        return 1;
    if (!(s.items = dr_calloc(n, sizeof(*s.items))))
        return DR_ERR(ENOMEM);
    s.nb_items = n;
    for (int k = 0; k < n; k++) {
        s.items[k].clip       = r[k].clip;
        s.items[k].stc        = r[k].stc;
        s.items[k].dep        = r[k].dep;
        s.items[k].item       = r[k].item;
        s.items[k].connection = r[k].connection;
    }
    if (r[0].dep && r[0].dep->app_type == 8) {
        mode3d = 1;
        for (int k = 0; k < n && mode3d; k++)
            mode3d = r[k].dep && r[k].dep->app_type == 8 && r[k].dep->program.nb_streams &&
                     r[k].dep->program.streams[0].coding_type == CODING_MVC;
    }
    if (mode3d && segment_ranges(log, d, &s, 1)) {
        s.interleaved = 1;
    } else if (!segment_ranges(log, d, &s, 0)) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: segment of clip %05u cannot be read\n", r[0].clip->number);
        segment_free(&s);
        return 0;
    }
    segment_names(&s);
    if (n == 1 && r[0].item && !r[0].dep && (ret = repair_streams(log, d, &s)) < 0) {
        segment_free(&s);
        return ret;
    }
    if ((ret = title_add_segment(t, &s)) < 0) {
        segment_free(&s);
        return ret;
    }
    return 1;
}

/* Segments of records: a record and the records after it with connection
 * condition 6 form one segment. */
static int build_segments(void *log, BlurayDisc *d, BlurayTitle *t, const Recs *r)
{
    for (int i = 0; i < r->n;) {
        int k = 0, ret;
        while (i + k + 1 < r->n && r->v[i + k + 1].connection == 6)
            k++;
        if ((ret = build_segment(log, d, t, &r->v[i], k + 1)) <= 0)
            return ret;
        i += k + 1;
    }
    return 1;
}

/* ---- titles ---- */

static void title_free(BlurayTitle *t)
{
    for (int i = 0; i < t->nb_segs; i++)
        segment_free(&t->segs[i]);
    dr_freep(&t->segs);
    for (int i = 0; i < t->nb_side; i++)
        segment_free(&t->side[i].seg);
    dr_freep(&t->side);
    dr_freep(&t->marks);
    dr_freep(&t->pids);
    dr_freep(&t->tracks);
    dr_freep(&t->title_name);
    dr_freep(&t->alt_name);
    memset(t, 0, sizeof(*t));
}

int64_t ff_bluray_title_duration(const BlurayTitle *t)
{
    int64_t ticks = 0;
    for (int i = 0; i < t->nb_segs; i++)
        for (int k = 0; k < t->segs[i].nb_items; k++)
            ticks += (uint32_t)(t->segs[i].items[k].stc->end - t->segs[i].items[k].stc->start);
    return ticks;
}

int64_t ff_bluray_title_size(const BlurayTitle *t)
{
    int64_t bytes = 0;
    for (int i = 0; i < t->nb_segs; i++)
        for (int k = 0; k < t->segs[i].nb_items; k++)
            bytes += t->segs[i].items[k].length;
    return bytes;
}

int ff_bluray_title_parts(const BlurayTitle *t, int *first)
{
    int n = 0;

    for (int i = 0; i < t->nb_segs; i++)
        if (!i || t->segs[i].items[0].connection == CC_JOINED ||
            (t->segs[i].items[0].item && t->segs[i].items[0].item->whole_join)) {
            if (first)
                first[n] = i;
            n++;
        }
    if (first)
        first[n] = t->nb_segs;
    return DR_MAX(n, 1);
}

int64_t ff_bluray_segs_duration(const BlurayTitle *t, int s0, int s1)
{
    int64_t ticks = 0;
    for (int i = s0; i < s1; i++)
        for (int k = 0; k < t->segs[i].nb_items; k++)
            ticks += (uint32_t)(t->segs[i].items[k].stc->end - t->segs[i].items[k].stc->start);
    return ticks;
}

int ff_bluray_segs_marks(const BlurayTitle *t, int s0, int s1, uint64_t *out)
{
    uint64_t start = (uint64_t)ff_bluray_segs_duration(t, 0, s0) * MARK_UNIT;
    uint64_t end   = start + (uint64_t)ff_bluray_segs_duration(t, s0, s1) * MARK_UNIT;
    int n = 0;

    /* the title's marks are ascending and within it; a mark at the end of
     * an earlier part's item lies on this part's start, where this part's
     * own first mark may already be: kept once */
    for (int k = 0; k < t->nb_marks; k++)
        if (t->marks[k] >= start && t->marks[k] < end && t->marks[k] - start < end - start - 1080000000ULL &&
            !(t->marks[k] == start && n))
            out[n++] = t->marks[k] - start;
    return n;
}

static int set_pids(BlurayTitle *t, const U16Set *p)
{
    dr_freep(&t->pids);
    t->nb_pids = 0;
    if (p->n && !(t->pids = dr_malloc_array(p->n, sizeof(*t->pids))))
        return DR_ERR(ENOMEM);
    memcpy(t->pids, p->v, p->n * sizeof(*t->pids));
    t->nb_pids = p->n;
    return 0;
}

static int set_pids_from_clip(BlurayTitle *t, const BlurayClip *c)
{
    U16Set s = { 0 };
    int ret = 0;

    for (int i = 0; i < c->program.nb_streams && ret >= 0; i++)
        ret = u16_insert(&s, c->program.streams[i].pid);
    if (ret >= 0)
        ret = set_pids(t, &s);
    dr_free(s.v);
    return ret;
}

/* a counts as equal to b: the same segments (clips, STC sequences,
 * dependent views, byte ranges; b's items only where a has them), a has
 * at least b's chapters (but not more than 4 times as many when b has 6 or
 * more) and all of b's streams. */
static int title_equal(const BlurayTitle *a, const BlurayTitle *b)
{
    if (a->nb_segs != b->nb_segs)
        return 0;
    for (int i = 0; i < a->nb_segs; i++) {
        const BluraySegment *x = &a->segs[i], *y = &b->segs[i];
        if (x->nb_items != y->nb_items)
            return 0;
        for (int j = 0; j < x->nb_items; j++) {
            const BluraySegItem *r = &x->items[j], *s = &y->items[j];
            if (r->clip != s->clip || r->stc != s->stc || r->dep != s->dep)
                return 0;
            if (s->item && !r->item)
                return 0;
            if (r->start != s->start || r->length != s->length)
                return 0;
        }
    }
    if (a->nb_marks < b->nb_marks)
        return 0;
    if (b->nb_marks >= 6 && (uint32_t)((a->nb_marks * 10) / b->nb_marks) > 40)
        return 0;
    if (a->nb_pids < b->nb_pids)
        return 0;
    for (int i = 0; i < b->nb_pids; i++) {
        int found = 0;
        for (int j = 0; j < a->nb_pids && !found; j++)
            found = a->pids[j] == b->pids[i];
        if (!found)
            return 0;
    }
    return 1;
}

/* Accept a title (taken over on success; freed when refused): the minimum
 * length (when check), no duplicate of an accepted title, no item whose
 * clip starts with a dependent-view (MVC) stream. 1 accepted, 0 refused. */
static int add_title(Ctx *c, BlurayTitle *t, int nangles, int angle, int check)
{
    BlurayDisc *d = c->d;
    BlurayTitle *v;

    if (check) {
        uint32_t secs = (uint64_t)ff_bluray_title_duration(t) / 45000;
        if (secs < (uint32_t)c->min_length) {
            dr_vlog(c->log, DR_VLOG_VERBOSE, "Blu-ray: %s lasts %u s, less than the minimum length of %d s: "
                   "left out\n", t->name, secs, c->min_length);
            title_free(t);
            return 0;
        }
    }
    for (int i = 0; i < d->nb_titles; i++)
        if (title_equal(&d->titles[i], t)) {
            dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: %s equals title %s: left out\n", t->name, d->titles[i].name);
            title_free(t);
            return 0;
        }
    for (int i = 0; i < t->nb_segs; i++)
        for (int k = 0; k < t->segs[i].nb_items; k++) {
            const BlurayClip *cl = t->segs[i].items[k].clip;
            if (!cl || (cl->program.nb_streams && cl->program.streams[0].coding_type == CODING_MVC)) {
                dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: %s cannot be a title (an item's clip starts with a dependent-view "
                       "stream): left out\n", t->name);
                title_free(t);
                return 0;
            }
        }
    /* its tracks (with the stream probes): a title whose tracks cannot be
     * taken is left out */
    if (d->fs) {
        int ret = ff_bluray_title_tracks(c->log, d->fs, d->keys, t);
        if (ret == DR_ERR(ENOMEM)) {
            title_free(t);
            return ret;
        }
        if (ret < 0) {
            dr_vlog(c->log, DR_VLOG_WARNING, "Blu-ray: title %s was skipped (its tracks cannot be taken)\n", t->name);
            title_free(t);
            return 0;
        }
    }
    {
        /* its segments: clip[:STC index][/dependent-view clip], runs in parentheses */
        char map[512] = "";
        for (int i = 0; i < t->nb_segs; i++)
            dr_strlcatf(map, sizeof(map), "%s%s", i ? "," : "", t->segs[i].desc);
        if (nangles >= 2) {
            t->angle = angle + 1;
            dr_vlog(c->log, DR_VLOG_VERBOSE, "Blu-ray: %s angle %d is title %d (segments %s)\n", t->name, angle + 1,
                   d->nb_titles, map);
        } else {
            dr_vlog(c->log, DR_VLOG_VERBOSE, "Blu-ray: %s is title %d (segments %s)\n", t->name, d->nb_titles, map);
        }
    }
    if (!(v = dr_realloc_array(d->titles, d->nb_titles + 1, sizeof(*v)))) {
        title_free(t);
        return DR_ERR(ENOMEM);
    }
    d->titles = v;
    d->titles[d->nb_titles++] = *t;
    memset(t, 0, sizeof(*t));
    return 1;
}

/* After an accepted title: the PIDs it uses, per clip of its segments. */
static int note_used(Ctx *c, const BlurayTitle *t, const Recs *r, int all_clip_pids)
{
    for (int i = 0; i < r->n; i++) {
        U16Set *s = clip_pids(&c->used, r->v[i].clip->number, 1);
        if (!s)
            return DR_ERR(ENOMEM);
        if (all_clip_pids) {
            for (int k = 0; k < r->v[i].clip->program.nb_streams; k++)
                if (u16_insert(s, r->v[i].clip->program.streams[k].pid) < 0)
                    return DR_ERR(ENOMEM);
        } else {
            for (int k = 0; k < t->nb_pids; k++)
                if (u16_insert(s, t->pids[k]) < 0)
                    return DR_ERR(ENOMEM);
        }
    }
    return 0;
}

/* ---- a part of a playlist ---- */

static int is_join(int cc)
{
    return cc == CC_JOINED || cc == 5 || cc == 6;
}

static int same_item(const BlurayPlayItem *a, const BlurayPlayItem *b)
{
    return !a->nb_angles && !b->nb_angles && !memcmp(a->clip, b->clip, 5) && a->stc_id == b->stc_id;
}

typedef struct Part {
    Recs      recs;
    uint64_t *marks;
    int       nb_marks, cap_marks;
    const BlurayPlayItem **items;
    int       nb_items;
    BluraySide *side;
    int       nb_side;
    int       nangles;
} Part;

static void part_free(Part *p)
{
    dr_freep(&p->recs.v);
    dr_freep(&p->marks);
    dr_freep(&p->items);
    dr_freep(&p->side);
    memset(p, 0, sizeof(*p));
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* h:mm:ss of a time in 45 kHz ticks (for the log). */
static void hms45(char *buf, size_t size, uint64_t t45)
{
    uint64_t sec = t45 / 45000;
    snprintf(buf, size, "%d:%02d:%02d", (int)(sec / 3600), (int)(sec / 60 % 60), (int)(sec % 60));
}

/* The records, chapter marks and items of part `part` of a playlist for one
 * angle. 1, 0 nothing (not sequential, no items, the first item unusable). */
static int part_segments(void *log, BlurayDisc *d, BlurayPlaylist *pl, int part, int angle, Part *out, int with_side)
{
    int n = pl->nb_items, s = 0, L = 1, longest = 0;
    uint64_t total = 0;
    char why[128];
    uint32_t maxdur = 0;
    int na = 0;

    part_free(out);
    if (pl->playback_type != 1 || !n)
        return 0;
    if (n >= 2) {
        if (part) {
            int i = 0, c = 0;
            do {
                i++;
                if (i >= n)
                    break;
                if (!is_join(pl->items[i].connection))
                    c++;
            } while (c != part);
            s = i;
        }
        if (s >= n)
            return 0;
        for (int j = s + 1; j < n && is_join(pl->items[j].connection); j++)
            L++;
    }
    if (pl->items[s].connection != 1) {
        dr_vlog(log, DR_VLOG_INFO, "Blu-ray: playlist %05u part %d: item %d, its first, has connection condition %d: "
               "taken as 1 (a part starts on its own)\n", pl->number, part, s, pl->items[s].connection);
        pl->items[s].connection = 1;
    }
    /* repeated blocks at the end of the part */
    while (L >= 2) {
        int best = 0;
        int e = s + L - 1;
        for (int p = 1; p <= DR_MAX(L / 2, 1); p++) {
            int m = 0;
            while ((m + 1) * p <= L) {
                int ok = 1;
                for (int q = 0; q < p && ok; q++)
                    ok = same_item(&pl->items[e - (m + 1) * p + 1 + q], &pl->items[e - p + 1 + q]);
                if (!ok)
                    break;
                m++;
            }
            if (m >= 2)
                best = DR_MAX(best, (m - 1) * p);
        }
        if (!best) {
            for (int p = 1; p <= L / 2; p++) {
                int m = 0;
                while ((m + 1) * p <= L) {
                    int ok = 1;
                    for (int q = 0; q < p && ok; q++)
                        ok = same_item(&pl->items[s + m * p + q], &pl->items[s + q]);
                    if (!ok)
                        break;
                    m++;
                }
                if (m >= 2)
                    best = DR_MAX(best, (m - 1) * p);
            }
            if (!best)
                break;
        }
        if (!angle)
            dr_vlog(log, DR_VLOG_INFO, "Blu-ray: playlist %05u part %d: its last %d item(s) (%d-%d) repeat the ones "
                   "before them: left out\n", pl->number, part, best, s + L - best, s + L - 1);
        L -= best;
        if (L <= 1)
            break;
    }
    if (!(out->items = dr_calloc(L + 1, sizeof(*out->items))))
        return DR_ERR(ENOMEM);
    for (int k = 0; k < L; k++) {
        int idx = s + k, ac = pl->items[idx].nb_angles, a;
        const BlurayPlayItem *it = &pl->items[idx];
        Rec rec;
        uint32_t dur;
        int ret;

        if (ac)
            na = DR_MAX(na, ac + 1);
        a = ac + 1 > angle ? angle : 0;
        if (!item_record(log, d, pl, idx, a, &rec, why, sizeof(why))) {
            char ang[24] = "", left[16], at[16];
            uint64_t rest = 0;
            for (int j = idx; j < s + L; j++)
                if (pl->items[j].out_time > pl->items[j].in_time)
                    rest += pl->items[j].out_time - pl->items[j].in_time;
            if (angle)
                snprintf(ang, sizeof(ang), " angle %d", angle + 1);
            hms45(left, sizeof(left), rest);
            if (!k) {
                dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: playlist %05u part %d%s: its first item %d cannot be used, "
                       "%s: the part (%s) is left out\n", pl->number, part, ang, idx, why, left);
                return 0;
            }
            hms45(at, sizeof(at), total);
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: playlist %05u part %d%s: item %d cannot be used, %s: items "
                   "%d-%d (%s) are left out, the title ends after item %d at %s\n", pl->number, part, ang, idx, why,
                   idx, s + L - 1, left, idx - 1, at);
            break;
        }
        if ((ret = recs_push(&out->recs, &rec)) < 0)
            return ret;
        out->items[out->nb_items++] = it;
        for (int m = 0; m < pl->nb_marks; m++) {
            const BlurayMark *mk = &pl->marks[m];
            if (mk->type == 1 && mk->play_item == idx && rec.stc->start <= mk->time && mk->time <= rec.stc->end) {
                if (out->nb_marks == out->cap_marks) {
                    uint64_t *v = dr_realloc_array(out->marks, out->cap_marks ? 2 * out->cap_marks : 16,
                                                   sizeof(*v));
                    if (!v)
                        return DR_ERR(ENOMEM);
                    out->marks     = v;
                    out->cap_marks = out->cap_marks ? 2 * out->cap_marks : 16;
                }
                out->marks[out->nb_marks++] = ((uint64_t)(mk->time - rec.stc->start) + total) * MARK_UNIT;
            }
        }
        dur = rec.stc->end - rec.stc->start;
        if (dur > maxdur) {
            maxdur  = dur;
            longest = out->nb_items - 1;
        }
        /* of a run of 7 or more identical items, the first and the last two */
        if (L - k >= 3 && !it->nb_angles) {
            int r = 1;
            while (r < L - k && same_item(&pl->items[idx + r], it))
                r++;
            if (r >= 7) {
                if (!angle)
                    dr_vlog(log, DR_VLOG_INFO, "Blu-ray: playlist %05u part %d: items %d-%d play the same piece of clip "
                           "%05u %d times: only the first and the last two are read\n", pl->number, part, idx,
                           idx + r - 1, rec.clip->number, r);
                k += r - 3;
            }
        }
        total += dur;
    }
    qsort(out->marks, out->nb_marks, sizeof(*out->marks), cmp_u64);
    while (out->nb_marks && out->marks[out->nb_marks - 1] >= total * MARK_UNIT - 1080000000ULL)
        out->nb_marks--;
    out->nangles = na ? na : 1;
    if (!with_side)
        return 1;

    /* text subtitle streams of the longest item, read from their SubPaths */
    {
        const BlurayPlayItem *l0 = out->items[longest];
        int cnt = 0;
        for (int e = 0; e < l0->nb_pg; e++)
            cnt += l0->pg[e].coding_type == CODING_TEXT;
        if (!cnt)
            return 1;
        if (!(out->side = dr_calloc(cnt, sizeof(*out->side))))
            return DR_ERR(ENOMEM);
        for (int e = 0; e < l0->nb_pg; e++) {
            const BlurayStnStream *E = &l0->pg[e];
            const BluraySubPath *sp;
            const BluraySubPlayItem *si;
            const char *ref, *mref;
            BluraySide *x;
            unsigned spi, sc, sy;

            if (E->coding_type != CODING_TEXT)
                continue;
            if (E->entry_type != 2 || E->entry_len <= 3)
                continue;
            spi = E->entry[0];
            sc  = E->entry[1];
            if (spi >= (unsigned)pl->nb_subpaths)
                continue;
            sp = &pl->subpaths[spi];
            if (sp->type != 4 || sp->nb_items != 1)
                continue;
            si = &sp->items[0];
            if (sc >= (unsigned)si->nb_clips + 1)
                dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: playlist %05u: text subtitle clip %u out of range\n",
                       pl->number, sc);
            sy = si->sync_play_item;
            if (sy >= (unsigned)n)
                continue;
            ref = sc == 0 ? si->clip : sc < (unsigned)si->nb_clips + 1 ? si->clips[sc - 1].clip : NULL;
            x = &out->side[out->nb_side];
            memset(x, 0, sizeof(*x));
            if (!(x->clip = clip_by_name(d, ref)) || !(x->stc = stc_lookup(log, x->clip, 0)))
                continue;
            mref = angle == 0 ? pl->items[sy].clip :
                   angle < pl->items[sy].nb_angles + 1 ? pl->items[sy].angles[angle - 1].clip : NULL;
            if (!(x->sync_clip = clip_by_name(d, mref)) ||
                !(x->sync_stc = stc_lookup(log, x->sync_clip, angle == 0 ? pl->items[sy].stc_id :
                                                              pl->items[sy].angles[angle - 1].stc_id)))
                continue;
            x->entry     = E;
            x->sync_pts  = si->sync_start_pts;
            x->in_time   = si->in_time;
            out->nb_side++;
        }
    }
    return 1;
}

/* Text subtitle segments (from their own clips) for a title. */
static void add_side(void *log, BlurayDisc *d, BlurayTitle *t, const Part *p)
{
    if (!p->nb_side || !(t->side = dr_calloc(p->nb_side, sizeof(*t->side))))
        return;
    for (int i = 0; i < p->nb_side; i++) {
        BluraySide x = p->side[i];
        BluraySegItem *it;

        memset(&x.seg, 0, sizeof(x.seg));
        if (!(it = dr_calloc(1, sizeof(*it))))
            return;
        it->clip       = x.clip;
        it->stc        = x.stc;
        it->connection = 1;
        x.seg.items    = it;
        x.seg.nb_items = 1;
        if (!segment_ranges(log, d, &x.seg, 0)) {
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: text subtitle clip %05u cannot be read\n", x.clip->number);
            segment_free(&x.seg);
            continue;
        }
        segment_names(&x.seg);
        t->side[t->nb_side++] = x;
    }
}

static int set_marks(BlurayTitle *t, const Part *p)
{
    dr_freep(&t->marks);
    t->nb_marks = 0;
    if (p->nb_marks && !(t->marks = dr_malloc_array(p->nb_marks, sizeof(*t->marks))))
        return DR_ERR(ENOMEM);
    memcpy(t->marks, p->marks, p->nb_marks * sizeof(*t->marks));
    t->nb_marks = p->nb_marks;
    qsort(t->marks, t->nb_marks, sizeof(*t->marks), cmp_u64);
    return 0;
}

/* The titles of a BDMV playlist: one per part and angle. named: the menu's
 * project data names it (a part of one clip keeps its title). */
static int playlist_titles(Ctx *c, BlurayPlaylist *pl, int named)
{
    BlurayDisc *d = c->d;
    int nparts = 1, ret = 0;
    Part p = { 0 };
    U16Set pids = { 0 };

    if (pl->playback_type != 1 || !pl->nb_items)
        return 0;
    for (int i = 1; i < pl->nb_items; i++)
        if (!is_join(pl->items[i].connection))
            nparts++;
    for (int part = 0; part < nparts; part++) {
        BlurayTitle t = { 0 };
        char name[32];
        int n;

        if ((ret = part_segments(c->log, d, pl, part, 0, &p, 1)) < 0)
            goto end;
        if (!ret || !p.recs.n)
            continue;
        if (p.recs.n == 1 && p.nb_marks <= 1 && !p.recs.v[0].dep && !named) {
            /* one clip without chapters: no title of the playlist, its clip may get one */
            {
                struct OneClip *v = dr_realloc_array(c->one, c->nb_one + 1, sizeof(*v));
                if (!v) {
                    ret = DR_ERR(ENOMEM);
                    goto end;
                }
                c->one = v;
                v[c->nb_one++] = (struct OneClip){ pl->number, p.recs.v[0].clip->number,
                                                   p.recs.v[0].stc->end - p.recs.v[0].stc->start, part };
            }
            if ((ret = clipset_add(c, p.recs.v[0].clip->number)) < 0)
                goto end;
            continue;
        }
        if ((ret = common_pids(c->log, p.items, p.nb_items, &pids)) < 0)
            goto end;
        if (!ret || !pids.n) {
            dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: playlist %05u part %d: no stream that all its items have: left "
                   "out\n", pl->number, part);
            continue;
        }
        if ((ret = build_segments(c->log, d, &t, &p.recs)) <= 0) {
            title_free(&t);
            if (ret < 0)
                goto end;
            dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: playlist %05u part %d: its segments cannot be built: left out\n",
                   pl->number, part);
            continue;
        }
        add_side(c->log, d, &t, &p);
        if ((ret = set_marks(&t, &p)) < 0 || (ret = set_pids(&t, &pids)) < 0) {
            title_free(&t);
            goto end;
        }
        t.pl = pl;
        snprintf(name, sizeof(name), "%05u.mpls", pl->number);
        if (part)
            snprintf(name + 10, sizeof(name) - 10, "(%u)", part);
        dr_strlcpy(t.name, name, sizeof(t.name));
        /* items that play only part of their STC sequence: the title reads
         * whole sequences (the reference program's way), so it holds more
         * than the playlist plays. LOOK INTO AGAIN LATER */
        {
            int nb = 0;
            uint64_t before = 0, after = 0;
            for (int k = 0; k < p.nb_items && k < p.recs.n; k++) {
                const BlurayPlayItem *it = p.items[k];
                const BlurayStc *st = p.recs.v[k].stc;
                int part_only = 0;
                if (it->in_time > st->start) {
                    before += it->in_time - st->start;
                    part_only = 1;
                }
                if (it->out_time < st->end) {
                    after += st->end - it->out_time;
                    part_only = 1;
                }
                nb += part_only;
            }
            if (nb)
                dr_vlog(c->log, DR_VLOG_WARNING, "Blu-ray: title %s: %d of its items play only part of their clip's "
                       "STC sequence (%.3f s before their IN times, %.3f s after their OUT times): the title holds "
                       "that too (whole STC sequences are read, the reference program's way; LOOK INTO AGAIN "
                       "LATER)\n", name, nb, before / 45000.0, after / 45000.0);
        }
        n = p.nangles;
        {
            Recs recs = p.recs;     /* the clips of this title, for the PIDs used */
            p.recs = (Recs){ 0 };
            if ((ret = add_title(c, &t, n, 0, 1)) > 0)
                ret = note_used(c, &d->titles[d->nb_titles - 1], &recs, 0);
            dr_free(recs.v);
            if (ret < 0)
                goto end;
        }
        for (int a = 1; a < n; a++) {
            BlurayTitle ta = { 0 };
            if ((ret = part_segments(c->log, d, pl, part, a, &p, 1)) < 0)
                goto end;
            if (!ret) {
                dr_vlog(c->log, DR_VLOG_WARNING, "Blu-ray: playlist %05u part %d angle %d cannot be read: left out\n",
                       pl->number, part, a + 1);
                continue;
            }
            if ((ret = build_segments(c->log, d, &ta, &p.recs)) <= 0) {
                title_free(&ta);
                if (ret < 0)
                    goto end;
                dr_vlog(c->log, DR_VLOG_WARNING, "Blu-ray: angle %d of playlist %05u cannot be read: left out\n", a + 1,
                       pl->number);
                continue;
            }
            add_side(c->log, d, &ta, &p);
            if ((ret = common_pids(c->log, p.items, p.nb_items, &pids)) < 0) {
                title_free(&ta);
                goto end;
            }
            if (!ret || !pids.n) {
                title_free(&ta);
                continue;
            }
            if ((ret = set_marks(&ta, &p)) < 0 || (ret = set_pids(&ta, &pids)) < 0) {
                title_free(&ta);
                goto end;
            }
            ta.pl = pl;
            dr_strlcpy(ta.name, name, sizeof(ta.name));
            {
                Recs recs = p.recs;
                p.recs = (Recs){ 0 };
                if ((ret = add_title(c, &ta, n, a, 1)) > 0)
                    ret = note_used(c, &d->titles[d->nb_titles - 1], &recs, 0);
                else if (!ret)
                    dr_vlog(c->log, DR_VLOG_VERBOSE, "Blu-ray: angle %d of playlist %05u is no title\n", a + 1,
                           pl->number);
                dr_free(recs.v);
                if (ret < 0)
                    goto end;
            }
        }
    }
    ret = 0;
end:
    part_free(&p);
    dr_free(pids.v);
    return ret;
}

/* ---- BDAV and AVCHD titles ---- */

static int bcd(uint8_t x)
{
    return (x >> 4) * 10 + (x & 0xf);
}

static char *trimmed(char *s)
{
    char *e;
    if (!s)
        return NULL;
    e = s + strlen(s);
    while (e > s && e[-1] == ' ')
        *--e = 0;
    if (*s == ' ')
        memmove(s, s + strspn(s, " "), strlen(s + strspn(s, " ")) + 1);
    if (!*s)
        dr_freep(&s);
    return s;
}

/* The recording time and names of a BDAV playlist for its title. */
static void bdav_info(void *log, BlurayTitle *t, const BlurayPlaylist *b)
{
    const uint8_t *H = b->bdav_hdr;
    int year = bcd(H[0x32]) * 100 + bcd(H[0x33]);
    unsigned v = dr_rb16(H + 0x40);
    char *na = NULL, *nb = NULL;
    int idx;

    t->year = year;
    if ((unsigned)(year - 1900) < 301) {
        t->month  = bcd(H[0x34]);
        t->day    = bcd(H[0x35]);
        t->hour   = bcd(H[0x36]);
        t->minute = bcd(H[0x37]);
        t->second = bcd(H[0x38]);
    } else {
        t->year = t->month = t->day = t->hour = t->minute = t->second = 0;
    }
    /* the first name's bytes may run on into the second's, as recorded */
    if (H[0x43])
        na = trimmed(ff_bluray_text(log, H[0x2e], H + 0x44, DR_MIN(H[0x43], 0x158 - 0x44)));
    if (H[0x58])
        nb = trimmed(ff_bluray_text(log, H[0x2e], H + 0x59, H[0x58]));
    idx = (nb != NULL) | (na != NULL) << 1 | (v - 1 < 0x7fff) << 2;
    switch (idx) {
    case 0: break;
    case 1: t->title_name = nb; nb = NULL; break;
    case 2: case 6: t->alt_name = na; na = NULL; break;
    case 3: case 7: dr_asprintf((char **)&t->title_name, "%s: %s", na, nb); break;
    case 4: dr_asprintf((char **)&t->alt_name, "%u", v); break;
    case 5: dr_asprintf((char **)&t->title_name, "%u: %s", v, nb); break;
    }
    dr_free(na);
    dr_free(nb);
}

/* An AVCHD playlist's maker data ("PLEX" extension entry). */
static const BlurayExtEntry *plex_entry(const BlurayPlaylist *p)
{
    for (int i = 0; i < p->nb_ext; i++) {
        const BlurayExtEntry *e = &p->ext[i];
        if (e->id1 == 0x1000 && (e->id2 | 0x10) == 0x110 && e->copied >= 4 && !memcmp(e->data, "PLEX", 4))
            return e;
    }
    return NULL;
}

typedef struct ClipMeta {
    int  year, month, day, hour, minute, second;
    char name[32];
} ClipMeta;

static const char *const month_names[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

/* One clean-up of an AVCHD name: a leading or trailing space or tab, or a
 * trailing "00:00:00", "(multiAVCHD)", "[Restricted]" or "[NTSC]" removed. */
static int avchd_trim(char *s)
{
    size_t n = strlen(s);

    if (!n)
        return 0;
    if (n >= 2 && (s[0] == ' ' || s[0] == '\t')) {
        memmove(s, s + 1, n - 1);
        s[n - 1] = 0;
        return 1;
    }
    if (s[n - 1] == '\t' || s[n - 1] == ' ') {
        s[n - 1] = 0;
        return 1;
    }
    if (n >= 8 && !memcmp(s + n - 8, "00:00:00", 8)) {
        s[n - 8] = 0;
        return 1;
    }
    if (n >= 12 && (!memcmp(s + n - 12, "(multiAVCHD)", 12) || !memcmp(s + n - 12, "[Restricted]", 12))) {
        s[n - 12] = 0;
        return 1;
    }
    if (n >= 6 && !memcmp(s + n - 6, "[NTSC]", 6)) {
        s[n - 6] = 0;
        return 1;
    }
    return 0;
}

/* avchd_trim, and a name that is only the recording date dropped. */
static int avchd_clean(char *s, const ClipMeta *m)
{
    char b[128];

    if (avchd_trim(s))
        return 1;
    if (m->month < 1 || m->month > 12)
        return 0;
    snprintf(b, sizeof(b), "%s %2u %u", month_names[m->month - 1], m->day, m->year);
    if (strcmp(s, b)) {
        snprintf(b, sizeof(b), "%u.%02u.%02u", m->year, m->month, m->day);
        if (strcmp(s, b))
            return 0;
    }
    s[0] = 0;
    return 1;
}

/* The maker data of clip `clip` in an AVCHD playlist: recording time and
 * name. 1, 0 none. */
static int plex_clip(void *log, const BlurayPlaylist *p, uint32_t clip, ClipMeta *md)
{
    const BlurayExtEntry *e = plex_entry(p);
    const uint8_t *D, *r = NULL;
    char title[257];
    int idx = 999999, n = 0, cnt = 0, nrec, skip, y, L, tl;

    memset(md, 0, sizeof(*md));
    if (!e)
        return 0;
    /* offsets as the reference counts them: from the extension record, whose
     * data begins 0x10 bytes in; its length checks compare those offsets
     * with the data's length */
#define AT(off) (D + (off) - 0x10)
    D = e->data;
    if (dr_rb32(AT(0x18)) != 0x144) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: playlist %05u: AVCHD maker data version %08x\n", p->number,
               dr_rb32(AT(0x18)));
        return 0;
    }
    if (e->copied < 0x14a)
        return 0;
    tl = DR_MIN(*AT(0x50), (int)e->copied - 0x41);
    memcpy(title, AT(0x51), tl);
    title[tl] = 0;
    while (avchd_trim(title))
        ;
    for (int i = 0; i < p->nb_items; i++) {
        const BlurayPlayItem *it = &p->items[i];
        char b[6];
        if (it->nb_angles)
            return 0;
        if (it->connection == 1 || it->connection == 5) {
            memcpy(b, it->clip, 5);
            b[5] = 0;
            if (strtoul(b, NULL, 10) == clip)
                idx = n;
            n++;
        } else if (it->connection != 6) {
            return 0;
        }
    }
    if (!p->nb_items || idx == 999999)
        return 0;
    nrec = dr_rb16(AT(0x158));
    for (int k = 0; k < DR_MAX(nrec, 1); k++) {
        const uint8_t *R = AT(0x14a + 0x42 * k);
        if (0x14a + 0x42 * (k + 1) > (int)e->copied)
            return 0;
        cnt += dr_rb16(R + 0x1b) != 0 && dr_rb16(R + 0x1b) != 0xffff;
    }
    if (!((n == 1 && cnt > 1) || cnt == n))
        return 0;
    skip = idx;
    for (int k = 0; k < DR_MAX(nrec, 1) && !r; k++) {
        const uint8_t *R = AT(0x14a + 0x42 * k);
        if (dr_rb16(R + 0x1b) == 0 || dr_rb16(R + 0x1b) == 0xffff)
            continue;
        if (skip-- == 0)
            r = R;
    }
#undef AT
    if (!r)
        return 0;
    y = dr_rb16(r + 0x1b);
    if (y < 0x1900 || y > 0x2fff)
        return 0;
    md->year   = bcd(y >> 8) * 100 + bcd(y & 0xff);
    md->month  = bcd(r[0x1d]);
    md->day    = bcd(r[0x1e]);
    md->hour   = bcd(r[0x1f]);
    md->minute = bcd(r[0x20]);
    md->second = bcd(r[0x21]);
    if ((L = r[0x23]) >= 0x19)
        return 0;
    memcpy(md->name, r + 0x24, L);
    md->name[L] = 0;
    while (avchd_clean(md->name, md))
        ;
    if (n == 1 && !md->name[0] && title[0]) {
        dr_strlcpy(md->name, title, sizeof(md->name));
        while (avchd_clean(md->name, md))
            ;
    }
    return 1;
}

/* A title of BDAV / AVCHD records (number: 0 the whole playlist, else the
 * run's number). */
static int recorded_title(Ctx *c, const BlurayPlaylist *avchd, const BlurayPlaylist *bdav, Recs *run, int number)
{
    BlurayTitle t = { 0 };
    ClipMeta md = { 0 };
    const BlurayPlaylist *src = avchd ? avchd : bdav;
    int ret;

    if (avchd && !plex_clip(c->log, avchd, run->v[0].clip->number, &md)) {
        if (!c->avchd_warned++)
            dr_vlog(c->log, DR_VLOG_WARNING, "Blu-ray: AVCHD maker data that cannot be read: names and recording "
                   "times left out [untested on real discs]\n");
        memset(&md, 0, sizeof(md));
    }
    if ((ret = build_segments(c->log, c->d, &t, run)) <= 0) {
        title_free(&t);
        dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: playlist %05u: its segments cannot be built: left out\n", src->number);
        return ret;
    }
    if ((ret = set_pids_from_clip(&t, run->v[0].clip)) < 0) {
        title_free(&t);
        return ret;
    }
    if (!avchd) {
        bdav_info(c->log, &t, bdav);
    } else {
        if (md.name[0])
            t.title_name = dr_strdup(md.name);
        if (md.year) {
            t.year = md.year; t.month = md.month; t.day = md.day;
            t.hour = md.hour; t.minute = md.minute; t.second = md.second;
        }
    }
    t.pl = src;
    if (number)
        snprintf(t.name, sizeof(t.name), "%05u.%s(%u)", src->number, avchd ? "mpls" : "rpls", number);
    else
        snprintf(t.name, sizeof(t.name), "%05u.%s", src->number, avchd ? "mpls" : "rpls");
    if ((ret = add_title(c, &t, 1, 0, 0)) > 0)
        ret = note_used(c, NULL, run, 1);
    return ret < 0 ? ret : 1;
}

/* The titles of a BDAV playlist, or of an AVCHD playlist with maker data:
 * one per seamless run, else (BDAV) one for the whole playlist. */
static int recorded_titles(Ctx *c, BlurayPlaylist *avchd, BlurayPlaylist *bdav)
{
    BlurayDisc *d = c->d;
    Recs segs = { 0 };
    Part p = { 0 };
    int titles = 0, ret = 1;

    if (avchd) {
        if ((ret = part_segments(c->log, d, avchd, 0, 0, &p, 0)) <= 0)
            goto end;
        segs   = p.recs;
        p.recs = (Recs){ 0 };
    } else {
        for (int i = 0; i < bdav->nb_items; i++)
            if (memcmp(bdav->items[i].codec, "M2TS", 4))
                goto split;
        for (int idx = 0; idx < bdav->nb_items; idx++) {
            const BlurayPlayItem *it = &bdav->items[idx];
            BlurayClip *cl = clip_by_name(d, it->clip);
            const BlurayStc *stc = NULL;
            Rec r = { 0 };
            if (!cl) {
                dr_vlog(c->log, DR_VLOG_DEBUG, "Blu-ray: playlist %05u item %d: no clip %.5s\n", bdav->number, idx,
                       it->clip);
                ret = 0;
                goto end;
            }
            if (bdav->bdav_type == 1 && !cl->stc_gap)
                stc = stc_lookup(c->log, cl, it->stc_id);
            if (!stc) {
                int many = cl->nb_atc > 1 || cl->atc[0].nb_stc > 1;
                for (int g = 0; g < cl->nb_atc && !stc; g++)
                    for (int k = 0; k < cl->atc[g].nb_stc && !stc; k++)
                        if (cl->atc[g].stc[k].start <= it->in_time && it->out_time <= cl->atc[g].stc[k].end)
                            stc = &cl->atc[g].stc[k];
                if (!stc) {
                    if (many) {
                        dr_vlog(c->log, DR_VLOG_DEBUG, "Blu-ray: playlist %05u item %d: no STC sequence fits\n",
                               bdav->number, idx);
                        ret = 0;
                        goto end;
                    }
                    stc = &cl->atc[0].stc[0];
                }
            }
            r.clip       = cl;
            r.stc        = stc;
            r.connection = it->connection == 5 || it->connection == 6 ? it->connection : 1;
            if (!idx)
                r.connection = 1;
            if ((ret = recs_push(&segs, &r)) < 0)
                goto end;
        }
    }
split:
    while (segs.n) {
        int i = 0, j;
        Recs run = { 0 };
        while (i < segs.n && segs.v[i].connection != 6)
            i++;
        if (i == segs.n)
            break;
        if (!i) {
            dr_vlog(c->log, DR_VLOG_DEBUG, "Blu-ray: a seamless run at a playlist's start\n");
            break;
        }
        for (j = i; j < segs.n && segs.v[j].connection == 6; j++)
            ;
        for (int k = i - 1; k < j && ret >= 0; k++)
            ret = recs_push(&run, &segs.v[k]);
        memmove(segs.v, segs.v + j, (segs.n - j) * sizeof(*segs.v));
        segs.n -= j;
        titles++;
        if (ret >= 0)
            ret = recorded_title(c, avchd, bdav, &run, titles);
        dr_free(run.v);
        if (ret <= 0)
            goto end;
    }
    ret = 1;
    if (bdav && !titles && segs.n >= 2)
        ret = recorded_title(c, NULL, bdav, &segs, 0);
end:
    dr_free(segs.v);
    part_free(&p);
    return ret;
}

/* ---- the disc ---- */

/* Play items merged where they join cleanly; connection 1 items marked to
 * join (0x7f) where the whole STC sequences fit. */
static int merge_items(Ctx *c, BlurayPlaylist *pl)
{
    for (int i = 1; i < pl->nb_items;) {
        BlurayPlayItem *I = &pl->items[i], *prev = &pl->items[i - 1];
        Rec A, B;
        uint32_t s, pIN, pOUT, iIN, iOUT;

        if ((I->connection | 4) != 5 || prev->nb_angles || I->nb_angles) {
            i++;
            continue;
        }
        if (!item_record(c->log, c->d, pl, i - 1, 0, &A, NULL, 0) ||
            !item_record(c->log, c->d, pl, i, 0, &B, NULL, 0))
            return 0;
        s = A.stc->start; pIN = prev->in_time; pOUT = prev->out_time; iIN = I->in_time; iOUT = I->out_time;
        if (A.clip == B.clip && A.stc == B.stc && !A.dep && !B.dep &&
            (s <= pIN || s - pIN <= TOLERANCE) && iIN >= pIN && iOUT >= pIN && iOUT >= pOUT && iIN >= pOUT &&
            iIN - pOUT <= MERGE_GAP && items_compatible(prev, A.clip, I, A.clip)) {
            int ret;
            if ((ret = clipset_add(c, A.clip->number)) < 0)
                return ret;
            dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: playlist %05u: items %d and %d play one stretch of clip %05u "
                   "(the second goes on where the first ends): merged into one item\n", pl->number, i - 1, i,
                   A.clip->number);
            prev->out_time = iOUT;
            for (int m = 0; m < pl->nb_marks; m++)
                if (pl->marks[m].play_item >= i)
                    pl->marks[m].play_item--;
            dr_freep(&I->angles);
            dr_freep(&I->audio);
            dr_freep(&I->pg);
            dr_freep(&I->ss.pg);
            memmove(I, I + 1, (pl->nb_items - i - 1) * sizeof(*I));
            pl->nb_items--;
            continue;
        }
        i++;
    }
    for (int i = 1; i < pl->nb_items; i++) {
        BlurayPlayItem *I = &pl->items[i], *prev = &pl->items[i - 1];
        int nA = DR_MAX(prev->nb_angles + 1, I->nb_angles + 1), ok = 1;

        if (I->connection != 1 && I->connection != 5)
            continue;
        for (int k = 0; k < nA && ok; k++) {
            Rec A, B;
            if (!item_record(c->log, c->d, pl, i - 1, k < prev->nb_angles + 1 ? k : 0, &A, NULL, 0) ||
                !item_record(c->log, c->d, pl, i, k < I->nb_angles + 1 ? k : 0, &B, NULL, 0))
                return 0;
            ok = DR_ABS((int64_t)prev->in_time - A.stc->start) <= TOLERANCE &&
                 DR_ABS((int64_t)prev->out_time - A.stc->end) <= TOLERANCE &&
                 DR_ABS((int64_t)I->in_time - B.stc->start) <= TOLERANCE &&
                 DR_ABS((int64_t)I->out_time - B.stc->end) <= TOLERANCE &&
                 items_compatible(prev, A.clip, I, B.clip);
        }
        /* a 5 keeps its connection (already a join of the title); it is only
         * marked as a place the title may be split */
        if (ok && I->connection == 5)
            I->whole_join = 1;
        else if (ok)
            I->connection = CC_JOINED;
    }
    /* said once per playlist: the runs of whole items joined seamlessly (a
     * place the title may be split) */
    {
        char runs[256] = "";
        int len = 0, first = -1;
        for (int i = 1; i <= pl->nb_items; i++) {
            int joined = i < pl->nb_items && pl->items[i].whole_join;
            if (joined && first < 0)
                first = i - 1;
            if (!joined && first >= 0) {
                len += snprintf(runs + len, sizeof(runs) - DR_MIN((size_t)len, sizeof(runs)), "%s%d-%d", len ? ", " : "",
                                first, i - 1);
                first = -1;
            }
        }
        if (len)
            dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: playlist %05u: items %s are whole clips with matching streams joined "
                   "seamlessly (connection condition 5): the title may be split there into parts\n", pl->number, runs);
    }
    /* said once per playlist: the runs of items kept together */
    {
        char runs[256] = "";
        int len = 0, first = -1;
        for (int i = 1; i <= pl->nb_items; i++) {
            int joined = i < pl->nb_items && pl->items[i].connection == CC_JOINED;
            if (joined && first < 0)
                first = i - 1;
            if (!joined && first >= 0) {
                len += snprintf(runs + len, sizeof(runs) - DR_MIN((size_t)len, sizeof(runs)), "%s%d-%d", len ? ", " : "",
                                first, i - 1);
                first = -1;
            }
        }
        if (len)
            dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: playlist %05u: items %s are whole clips with matching streams: kept "
                   "as one title across their non-seamless breaks\n", pl->number, runs);
    }
    return 1;
}

/* One title per clip that no title uses with all its streams. */
static int clip_titles(Ctx *c)
{
    BlurayDisc *d = c->d;

    for (int i = 0; i < d->nb_clips; i++) {
        BlurayClip *cl = &d->clips[i];
        U16Set *u = clip_pids(&c->used, cl->number, 0);
        BlurayTitle t = { 0 };
        int check = 1, ok = 1, ret;

        if (!(clipset_has(c, cl->number) || !u || u->n != cl->program.nb_streams) || cl->app_type != 1)
            continue;
        for (int g = 0; g < cl->nb_atc && ok; g++)
            for (int k = 0; k < cl->atc[g].nb_stc && ok; k++) {
                Rec r = { .clip = cl, .stc = &cl->atc[g].stc[k], .connection = 1 };
                if ((ret = build_segment(c->log, d, &t, &r, 1)) < 0) {
                    title_free(&t);
                    return ret;
                }
                ok = ret;
            }
        if (!ok) {
            dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: clip %05u: its segments cannot be built: no title of its own\n",
                   cl->number);
            title_free(&t);
            continue;
        }
        if ((ret = set_pids_from_clip(&t, cl)) < 0) {
            title_free(&t);
            return ret;
        }
        /* AVCHD: the maker data of a playlist that plays the clip */
        for (int p = 0; p < d->nb_playlists && check; p++) {
            const BlurayPlaylist *pl = &d->playlists[p];
            if (pl->bdav || !plex_entry(pl) || !pl->nb_items)
                continue;
            for (int k = 0; k < pl->nb_items; k++) {
                char num[8];
                ClipMeta md;
                if (pl->items[k].nb_angles)
                    continue;
                snprintf(num, sizeof(num), "%05u", cl->number);
                if (memcmp(pl->items[k].clip, num, 5))
                    continue;
                if (plex_clip(c->log, pl, cl->number, &md)) {
                    if (md.name[0])
                        t.title_name = dr_strdup(md.name);
                    if (md.year) {
                        t.year = md.year; t.month = md.month; t.day = md.day;
                        t.hour = md.hour; t.minute = md.minute; t.second = md.second;
                    }
                    check = 0;
                } else if (!c->avchd_warned++) {
                    dr_vlog(c->log, DR_VLOG_WARNING, "Blu-ray: AVCHD maker data that cannot be read: names and "
                           "recording times left out [untested on real discs]\n");
                }
                break;
            }
        }
        /* BDAV: the playlist that plays only this clip */
        for (int p = 0; p < d->nb_playlists; p++) {
            const BlurayPlaylist *pl = &d->playlists[p];
            char num[8];
            int all = pl->bdav && pl->nb_items && !memcmp(pl->items[0].codec, "M2TS", 4);
            snprintf(num, sizeof(num), "%05u", cl->number);
            for (int k = 0; k < pl->nb_items && all; k++)
                all = !memcmp(pl->items[k].codec, "M2TS", 4) && !memcmp(pl->items[k].clip, num, 5);
            if (all) {
                bdav_info(c->log, &t, pl);
                break;
            }
        }
        snprintf(t.name, sizeof(t.name), "%05u.%s", cl->number, cl->stream_type == 2 ? "fmts" : "m2ts");
        if ((ret = add_title(c, &t, 1, 0, check)) < 0)
            return ret;
    }
    return 0;
}

void ff_bluray_titles_free(BlurayDisc *d)
{
    for (int i = 0; i < d->nb_titles; i++)
        title_free(&d->titles[i]);
    dr_freep(&d->titles);
    d->nb_titles = 0;
}

void ff_bluray_title_remove(BlurayDisc *d, int i)
{
    title_free(&d->titles[i]);
    memmove(d->titles + i, d->titles + i + 1, (d->nb_titles - i - 1) * sizeof(*d->titles));
    d->nb_titles--;
}

/* The playlist parts that are one piece of one clip with at most one
 * chapter (the reference program makes no playlist title of them; the clip
 * gets a title of its own): those of a minute or more one line each, the
 * shorter ones in one line; whether the clip has a title now. On a disc
 * with BD-J titles the reference program first asks the disc's navigation
 * which playlists it plays (those keep their title): not done here. */
static int one_clips_said(Ctx *c)
{
    BlurayDisc *d = c->d;
    DrStr shorts, untitled;
    int nb_short = 0, nb_untitled = 0;

    if (!c->nb_one)
        return 0;
    if (ff_bluray_has_bdj(d) && !d->pl_menu_name)
        dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: the disc has BD-J titles: its navigation, which can keep a "
               "playlist of one clip piece as a title of its own, is not read (LOOK INTO AGAIN LATER)\n");
    /* in playlist order */
    for (int i = 1; i < c->nb_one; i++)
        for (int j = i; j > 0 && (c->one[j - 1].pl > c->one[j].pl ||
                                  (c->one[j - 1].pl == c->one[j].pl && c->one[j - 1].part > c->one[j].part)); j--) {
            struct OneClip t = c->one[j];
            c->one[j] = c->one[j - 1];
            c->one[j - 1] = t;
        }
    dr_str_init(&shorts);
    dr_str_init(&untitled);
    for (int i = 0; i < c->nb_one; i++) {
        const struct OneClip *o = &c->one[i];
        char clip[16], name[16];
        int titled = 0;

        snprintf(clip, sizeof(clip), "%05u.m2ts", o->clip);
        for (int t = 0; t < d->nb_titles && !titled; t++)
            titled = !strncmp(d->titles[t].name, clip, 5) && d->titles[t].name[5] == '.' &&
                     strlen(d->titles[t].name) == 10;     /* "NNNNN.m2ts" / "NNNNN.fmts": the clip's own title */
        snprintf(name, sizeof(name), o->part ? "%05u(%d)" : "%05u", o->pl, o->part);
        if (o->len >= 60 * 45000) {
            if (titled)
                dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: playlist %s is one piece of clip %05u (%u:%02u, at most one "
                       "chapter): no playlist title; the clip is listed as its own title instead (%s)\n", name,
                       o->clip, o->len / 45000 / 60, o->len / 45000 % 60, clip);
            else
                dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: playlist %s is one piece of clip %05u (%u:%02u, at most one "
                       "chapter): no playlist title; its clip has no title of its own either (equal to another "
                       "title, or no movie clip)\n", name, o->clip, o->len / 45000 / 60, o->len / 45000 % 60);
            continue;
        }
        dr_str_catf(&shorts, "%s%s", nb_short++ ? ", " : "", name);
        if (!titled)
            dr_str_catf(&untitled, "%s%05u", nb_untitled++ ? ", " : "", o->clip);
    }
    if (nb_short)
        dr_vlog(c->log, DR_VLOG_INFO, "Blu-ray: %d playlist(s) under 1 min are each one piece of a clip with at most one "
               "chapter: no playlist titles; their clips are listed as titles of their own instead%s%s%s: %s\n",
               nb_short, nb_untitled ? " (but clips " : "", nb_untitled ? untitled.str : "",
               nb_untitled ? ", equal to another title or no movie clips)" : "", shorts.str);
    dr_str_free(&shorts);
    dr_str_free(&untitled);
    return 0;
}

int ff_bluray_titles_build(void *log, BlurayDisc *d, int min_length)
{
    Ctx c = { .log = log, .d = d, .min_length = min_length };
    int ret = 0;

    ff_bluray_titles_free(d);
    /* 1. play items merged (not on AVCHD playlists with maker data) */
    for (int p = 0; p < d->nb_playlists; p++) {
        BlurayPlaylist *pl = &d->playlists[p];
        if (pl->bdav || pl->nb_items < 2 || plex_entry(pl))
            continue;
        if ((ret = merge_items(&c, pl)) < 0)
            goto end;
        if (!ret)
            dr_vlog(log, DR_VLOG_INFO, "Blu-ray: playlist %05u: an item cannot be resolved (its clip is missing or "
                   "cannot be used)\n", pl->number);
    }
    /* 2. playlist titles */
    if (d->bdav) {
        for (int p = 0; p < d->nb_playlists; p++)
            if ((ret = recorded_titles(&c, NULL, &d->playlists[p])) < 0)
                goto end;
    } else {
        /* the playlists the menu's project data names first, then the others */
        for (int named = !!d->pl_menu_name; named >= 0; named--)
            for (int p = 0; p < d->nb_playlists; p++) {
                BlurayPlaylist *pl = &d->playlists[p];
                if (d->pl_menu_name && !!d->pl_menu_name[p] != named)
                    continue;
                ret = plex_entry(pl) ? recorded_titles(&c, pl, NULL) : playlist_titles(&c, pl, named);
                if (ret < 0)
                    goto end;
            }
    }
    /* 3. clip titles */
    if ((ret = clip_titles(&c)) < 0)
        goto end;
    /* 4. duplicates */
restart:
    for (int i = 0; i < d->nb_titles; i++)
        for (int j = 0; j < d->nb_titles; j++)
            if (i != j && title_equal(&d->titles[i], &d->titles[j])) {
                dr_vlog(log, DR_VLOG_INFO, "Blu-ray: title %s equals title %s: left out\n", d->titles[j].name,
                       d->titles[i].name);
                title_free(&d->titles[j]);
                memmove(&d->titles[j], &d->titles[j + 1], (d->nb_titles - j - 1) * sizeof(*d->titles));
                if (!--d->nb_titles)
                    goto done;
                goto restart;
            }
done:
    one_clips_said(&c);
    dr_vlog(log, DR_VLOG_VERBOSE, "Blu-ray: %d titles\n", d->nb_titles);
    ret = 0;
end:
    for (int i = 0; i < c.used.n; i++)
        dr_free(c.used.pids[i].v);
    dr_free(c.used.pids);
    dr_free(c.used.clip);
    dr_free(c.clipset);
    dr_free(c.one);
    return ret;
}
