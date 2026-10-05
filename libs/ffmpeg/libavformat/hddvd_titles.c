/*
 * HD DVD (Advanced Content): the title plan. Titles come from the
 * playlists' clip runs; each EVOB a title plays needs its time map
 * (HVDVD_TS/<name>.MAP), which also gives the EVOB's blocks.
 *
 * Time map (big-endian): 0x00 "HDDVD_TMAP00"; byte 0x14 bit 1 must be clear;
 * 0x37 u16 table count (at least 1); table i described by 10 bytes at
 * 0x180 + 0x20 * i: u32 offset of its entries, at +6 u16 entry count, at +8
 * u16 value (0 for the first table). Entry: 4 bytes, the low 13 bits of its
 * last u16 = a number of 2048-byte blocks. The first table's entries, in
 * order, cover the EVOB: each one a run of blocks, at the same place in the
 * EVOB's stream and in its EVO file. An entry whose stream block is already
 * taken (after an entry of 0 blocks) is not kept; its blocks still count in
 * the size.
 *
 * Times in playlists: "HH:MM:SS:FF" (exactly 11 characters, else 0), in
 * milliseconds HH*3600000 + MM*60000 + SS*1000 + FF*1000/fps (32-bit, frames
 * rounded down), fps from the TitleSet's timeBase: its first two characters
 * when it is 5 characters long ("60fps"), else none: then every time is 0.
 * A run's window starts at the clip's position times the duration of the
 * run's first clip (not the sum of the clips before it) and lasts the
 * duration of the run's clips; chapters in the window (both ends included)
 * are kept, measured from its start.
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
#include <stdlib.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "disclang.h"
#include "hddvd_internal.h"

#define URI_PREFIX_LEN 25   /* "file:///dvddisc/HVDVD_TS/" */

/* ---- the time map ---- */

/* Reads through one cached 2048-byte block, as the map is read in small pieces. */
typedef struct BlockReader {
    DiscIOFS   *fs;
    DiscIOFile *file;
    int64_t     cached;          /* block in buf, -1 none */
    uint8_t     buf[2048];
} BlockReader;

static int block_read(BlockReader *r, int64_t pos, uint8_t *dst, int len)
{
    while (len > 0) {
        int64_t blk = pos / 2048;
        int at = pos % 2048, n = FFMIN(len, 2048 - at), ret;

        if (blk != r->cached) {
            int64_t want = FFMIN(2048, r->file->size - blk * 2048);
            if (want <= 0)
                return AVERROR(EINVAL);
            if ((ret = ff_discio_file_read(r->fs, r->file, blk * 2048, r->buf, want)) < 0)
                return ret;
            if (want < 2048)
                memset(r->buf + want, 0, 2048 - want);
            r->cached = blk;
        }
        if (pos + n > r->file->size)
            return AVERROR(EINVAL);
        memcpy(dst, r->buf + at, n);
        pos += n;
        dst += n;
        len -= n;
    }
    return 0;
}

/* Sorted unique insert by stream block; an entry whose block is taken is dropped. */
static int add_extent(HDDVDClip *c, HDDVDExtent x)
{
    int lo = 0, hi = c->nb_extents;
    HDDVDExtent *e;

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (c->extents[mid].stream < x.stream)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < c->nb_extents && c->extents[lo].stream == x.stream)
        return 0;
    if (!(e = av_realloc_array(c->extents, c->nb_extents + 1, sizeof(*e))))
        return AVERROR(ENOMEM);
    c->extents = e;
    memmove(e + lo + 1, e + lo, (c->nb_extents - lo) * sizeof(*e));
    e[lo] = x;
    c->nb_extents++;
    return 1;
}

static int read_map(void *logctx, BlockReader *r, const char *path, HDDVDClip *c)
{
    uint8_t hdr[0x41], d[10], v[4];
    int nb_tables, flag, ret;
    uint32_t stream = 0, file = 0;

    if (block_read(r, 0, hdr, sizeof(hdr)) < 0) {
        av_log(logctx, AV_LOG_WARNING, "%s: cannot read the time map header\n", path);
        return 0;
    }
    if (memcmp(hdr, "HDDVD_TMAP00", 12)) {
        av_log(logctx, AV_LOG_WARNING, "%s: identifier \"%.12s\" (\"HDDVD_TMAP00\")\n", path, (const char *)hdr);
        return 0;
    }
    flag      = (hdr[0x14] >> 1) & 1;
    nb_tables = AV_RB16(hdr + 0x37);
    if (!nb_tables) {
        av_log(logctx, AV_LOG_WARNING, "%s: no time map table\n", path);
        return 0;
    }
    for (int i = 0; i < nb_tables; i++) {
        uint32_t off;
        int cnt, val;

        if (block_read(r, 0x180 + 0x20 * i, d, sizeof(d)) < 0) {
            av_log(logctx, AV_LOG_WARNING, "%s: cannot read table %d's description\n", path, i + 1);
            return 0;
        }
        off = AV_RB32(d);
        cnt = AV_RB16(d + 6);
        val = AV_RB16(d + 8);
        if (i == 0 && val)
            av_log(logctx, AV_LOG_WARNING, "%s: the first time map table has the value %d (0 expected)\n", path, val);
        for (int j = 0; j < cnt; j++) {
            uint32_t n;
            if (block_read(r, off + 4 * (int64_t)j, v, 4) < 0) {
                av_log(logctx, AV_LOG_WARNING, "%s: cannot read entry %d of table %d\n", path, j + 1, i + 1);
                return 0;
            }
            if (i)
                continue;
            n = AV_RB16(v + 2) & 0x1fff;
            if ((ret = add_extent(c, (HDDVDExtent){ stream, file, n })) < 0)
                return ret;
            if (!ret)
                av_log(logctx, AV_LOG_WARNING, "%s: entry %d starts at stream block %"PRIu32", "
                       "already taken: its %"PRIu32" blocks are not mapped\n", path, j + 1, stream, n);
            c->size += (uint64_t)n * 2048;
            stream  += n;
            file    += n;
        }
    }
    if (flag) {
        av_log(logctx, AV_LOG_WARNING, "%s: header byte 0x14 has bit 1 set (0x%02x): not supported\n", path, hdr[0x14]);
        return 0;
    }
    av_log(logctx, AV_LOG_DEBUG, "%s: %d table(s), %d extents, %"PRIu64" bytes\n",
           path, nb_tables, c->nb_extents, c->size);
    return 1;
}

int ff_hddvd_clip_load(void *logctx, DiscIOFS *fs, const char *folder, HDDVDClip *c)
{
    const char *name = c->evob->name, *dot = strrchr(name, '.');
    char map[300], evo[300];
    DiscIOFile *m = NULL, *e = NULL;
    BlockReader *r;
    int ret, rm, re;

    if (c->state)
        return c->state > 0;
    c->state = -1;
    if (!dot) {
        av_log(logctx, AV_LOG_WARNING, "EVOB %s: the name has no extension; it cannot be used\n", name);
        return 0;
    }
    snprintf(map, sizeof(map), "/%s/%.*s.MAP", folder, (int)(dot - name), name);
    snprintf(evo, sizeof(evo), "/%s/%s", folder, name);
    rm = fs->ops->open_file(fs, map, &m);
    re = fs->ops->open_file(fs, evo, &e);
    if (re < 0 || rm < 0) {
        ff_discio_file_free(&e);
        av_log(logctx, AV_LOG_WARNING, "EVOB %s: %s%s%s; it cannot be used\n", name,
               re < 0 ? "no EVO file" : "", re < 0 && rm < 0 ? ", " : "", rm < 0 ? "no time map" : "");
        ff_discio_file_free(&m);
        return 0;
    }
    if (!(r = av_mallocz(sizeof(*r)))) {
        ff_discio_file_free(&m);
        ff_discio_file_free(&e);
        return AVERROR(ENOMEM);
    }
    r->fs = fs;
    r->file = m;
    r->cached = -1;
    ret = read_map(logctx, r, map, c);
    av_free(r);
    ff_discio_file_free(&m);
    if (ret <= 0)
        ff_discio_file_free(&e);
    if (ret < 0)
        return ret;
    if (!ret) {
        av_freep(&c->extents);
        c->nb_extents = 0;
        c->size = 0;
        av_log(logctx, AV_LOG_WARNING, "EVOB %s: its time map is not usable; it cannot be used\n", name);
        return 0;
    }
    c->evo   = e;
    c->state = 1;
    return 1;
}

/* ---- candidates ---- */

typedef struct Candidate {
    const char      **names;
    int               nb_names;
    const char       *lang;
    HDDVDChapterMark *marks;
    int               nb_marks;
} Candidate;

typedef struct Planner {
    void             *log;
    DiscIOFS         *fs;
    const HDDVDVTI   *vti;
    HDDVDXpl *const  *xpls;
    int               nb_xpls;
    HDDVDTitlePlan   *plan;
    Candidate        *cands;
    int               nb_cands;
} Planner;

static void cand_free(Candidate *c)
{
    av_freep(&c->names);
    av_freep(&c->marks);
}

static int cand_cmp(const Candidate *a, const Candidate *b)
{
    if (a->nb_names != b->nb_names)
        return a->nb_names < b->nb_names ? -1 : 1;
    for (int i = 0; i < a->nb_names; i++) {
        int r = av_strcasecmp(a->names[i], b->names[i]);
        if (r)
            return r;
    }
    return 0;
}

/* Insert keeping the list sorted; a candidate already there is dropped. */
static int cand_insert(Planner *p, Candidate *c)
{
    int lo = 0, hi = p->nb_cands;
    Candidate *v;

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cand_cmp(&p->cands[mid], c) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < p->nb_cands && !cand_cmp(&p->cands[lo], c)) {
        cand_free(c);
        return 0;
    }
    if (!(v = av_realloc_array(p->cands, p->nb_cands + 1, sizeof(*v)))) {
        cand_free(c);
        return AVERROR(ENOMEM);
    }
    p->cands = v;
    memmove(v + lo + 1, v + lo, (p->nb_cands - lo) * sizeof(*v));
    v[lo] = *c;
    p->nb_cands++;
    return 0;
}

static int uri_prefix_ok(const char *s)
{
    return !av_strncasecmp(s, "file:///dvddisc/HVDVD_TS/", URI_PREFIX_LEN) ||
           !av_strncasecmp(s, "file:///dvddisc/HDDVD_TS/", URI_PREFIX_LEN);
}

static int ends_with_ci(const char *s, const char *end)
{
    size_t n = strlen(s), k = strlen(end);
    return n >= k && !av_strcasecmp(s + n - k, end);
}

static uint32_t fps_of(const HDDVDXplNode *title_set)
{
    const char *tb = ff_hddvd_xpl_str(title_set, "timeBase");
    char two[3];

    if (!tb || strlen(tb) != 5)
        return 0;
    memcpy(two, tb, 2);
    two[2] = 0;
    return strtoul(two, NULL, 10);
}

static uint32_t ms_of(const char *t, uint32_t fps)
{
    char b[12];
    uint32_t hh, mm, ss, ff;

    if (!t || strlen(t) != 11)
        return 0;
    memcpy(b, t, 12);
    b[2] = b[5] = b[8] = 0;
    hh = strtoul(b, NULL, 10);
    mm = strtoul(b + 3, NULL, 10);
    ss = strtoul(b + 6, NULL, 10);
    ff = strtoul(b + 9, NULL, 10);
    return (hh * 3600 + mm * 60 + ss) * 1000 + ff * 1000 / fps;
}

static uint32_t clip_ms(const HDDVDXplNode *clip, uint32_t fps)
{
    return ms_of(ff_hddvd_xpl_str(clip, "titleTimeEnd"), fps) - ms_of(ff_hddvd_xpl_str(clip, "titleTimeBegin"), fps);
}

/* The candidates of one clip list: title is NULL for the FirstPlayTitle. */
static int collect(Planner *p, const HDDVDXplNode *title_set, const HDDVDXplNode *title,
                   const HDDVDXplNode *owner)
{
    int nb = ff_hddvd_xpl_count(owner, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP);
    const HDDVDXplNode *chapters = title ? ff_hddvd_xpl_child(title, HDDVD_XPL_CHAPTER_LIST, 0) : NULL;
    int i = 0, ret;

    while (i < nb) {
        const HDDVDXplNode *first = ff_hddvd_xpl_child(owner, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, i);
        Candidate c = { 0 };
        int k = 1;

        while (i + k < nb && ff_hddvd_xpl_num(ff_hddvd_xpl_child(owner, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, i + k),
                                               "seamless"))
            k++;
        c.lang = ff_hddvd_xpl_str(title_set, "defaultLanguage");
        if (!(c.names = av_calloc(k, sizeof(*c.names))))
            return AVERROR(ENOMEM);
        c.nb_names = k;
        for (int j = 0; j < k; j++) {
            const char *s = ff_hddvd_xpl_str(ff_hddvd_xpl_child(owner, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, i + j), "src");
            c.names[j] = "";
            if (s && uri_prefix_ok(s)) {
                if (ends_with_ci(s, ".map"))
                    c.names[j] = s + URI_PREFIX_LEN;
                else
                    av_log(p->log, AV_LOG_DEBUG, "XPL: clip src \"%s\" does not end in .map\n", s);
            }
        }
        if (title) {
            uint32_t fps = fps_of(title_set), lo = 0, hi;

            if (i && fps)
                for (int j = 0; j < i; j++)
                    lo += clip_ms(first, fps);      /* the run's first clip, i times */
            hi = lo;
            for (int j = 0; j < k && fps; j++)
                hi += clip_ms(ff_hddvd_xpl_child(owner, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, i + j), fps);
            for (int j = 0; chapters && j < ff_hddvd_xpl_count(chapters, HDDVD_XPL_CHAPTER); j++) {
                const HDDVDXplNode *ch = ff_hddvd_xpl_child(chapters, HDDVD_XPL_CHAPTER, j);
                uint32_t t = fps ? ms_of(ff_hddvd_xpl_str(ch, "titleTimeBegin"), fps) : 0;
                HDDVDChapterMark m = { ff_hddvd_xpl_str(ch, "displayName"), t - lo };

                if (t < lo || t > hi)
                    continue;
                if (av_dynarray2_add((void **)&c.marks, &c.nb_marks, sizeof(m), (const uint8_t *)&m) == NULL) {
                    cand_free(&c);
                    return AVERROR(ENOMEM);
                }
            }
        }
        if ((ret = cand_insert(p, &c)) < 0)
            return ret;
        i += k;
    }
    return 0;
}

/* ---- titles ---- */

static const char *keep(HDDVDTitlePlan *plan, const char *s)
{
    char *d;

    if (!s)
        return NULL;
    if (!(d = av_strdup(s)) || av_dynarray_add_nofree(&plan->strings, &plan->nb_strings, d) < 0) {
        av_free(d);
        return NULL;
    }
    return d;
}

static HDDVDClip *clip_of(Planner *p, const HDDVDEvob *e, int *err)
{
    HDDVDClip **slot = &p->plan->clips[e->slot - 1];
    int ret;

    if (!*slot) {
        if (!(*slot = av_mallocz(sizeof(**slot)))) {
            *err = AVERROR(ENOMEM);
            return NULL;
        }
        (*slot)->evob = e;
        (*slot)->keybase = (uint32_t)e->playlist << 8;
    }
    if ((ret = ff_hddvd_clip_load(p->log, p->fs, p->vti->folder, *slot)) < 0)
        *err = ret;
    return ret > 0 ? *slot : NULL;
}

/* Whether a clip's src names EVOB evo ("file:///dvddisc/HVDVD_TS/" or
 * "HDDVD_TS/" + evo's base name + ".map", case not regarded; evo ends ".evo"). */
static int src_names_evob(void *logctx, const char *s, const char *evo)
{
    size_t n = strlen(evo);

    if (!s) {
        av_log(logctx, AV_LOG_DEBUG, "XPL: a clip without src\n");
        return 0;
    }
    if (n < 5) {
        av_log(logctx, AV_LOG_DEBUG, "EVOB name \"%s\" is shorter than 5 characters\n", evo);
        return 0;
    }
    if (!uri_prefix_ok(s))
        return 0;
    if (!ends_with_ci(s, ".map")) {
        av_log(logctx, AV_LOG_DEBUG, "XPL: clip src \"%s\" does not end in .map\n", s);
        return 0;
    }
    if (strlen(s + URI_PREFIX_LEN) != n)
        return 0;
    if (!ends_with_ci(evo, ".evo")) {
        av_log(logctx, AV_LOG_DEBUG, "EVOB name \"%s\" does not end in .evo\n", evo);
        return 0;
    }
    return !av_strncasecmp(evo, s + URI_PREFIX_LEN, n - 4);
}

void ff_hddvd_evob_marks(void *logctx, HDDVDVTI *vti, HDDVDXpl *const *xpls, int nb_xpls)
{
    for (int q = nb_xpls - 1; q > 0; q--) {
        const HDDVDXplNode *root = &xpls[q]->root, *ts, *fpt;
        int nts = ff_hddvd_xpl_count(root, HDDVD_XPL_TITLE_SET);

        for (int x = 0; x < HDDVD_VTI_MAX_EVOBS; x++) {
            HDDVDEvob *e = vti->evobs[x];
            int found = 0;

            if (!e || e->playlist >= q)
                continue;
            if (nts != 1) {
                av_log(logctx, AV_LOG_DEBUG, "Playlist %d has %d TitleSets: not looked at for EVOB %s\n",
                       q, nts, e->name);
                continue;
            }
            ts = ff_hddvd_xpl_child(root, HDDVD_XPL_TITLE_SET, 0);
            if ((fpt = ff_hddvd_xpl_child(ts, HDDVD_XPL_FIRST_PLAY_TITLE, 0)))
                for (int c = 0; !found && c < ff_hddvd_xpl_count(fpt, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP); c++)
                    found = src_names_evob(logctx, ff_hddvd_xpl_str(ff_hddvd_xpl_child(fpt,
                                           HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, c), "src"), e->name);
            for (int t = 0; !found && t < ff_hddvd_xpl_count(ts, HDDVD_XPL_TITLE); t++) {
                const HDDVDXplNode *title = ff_hddvd_xpl_child(ts, HDDVD_XPL_TITLE, t);
                for (int c = 0; !found && c < ff_hddvd_xpl_count(title, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP); c++)
                    found = src_names_evob(logctx, ff_hddvd_xpl_str(ff_hddvd_xpl_child(title,
                                           HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, c), "src"), e->name);
            }
            if (found) {
                e->playlist = q;
                av_log(logctx, AV_LOG_VERBOSE, "EVOB %s: named by playlist %d (title key ids from %d)\n",
                       e->name, q, q << 8);
            }
        }
    }
}

/* The Title of a playlist that plays EVOB evo: its name (displayName,
 * description, id, the first not empty, else ""), NULL when none plays it. */
static const char *playlist_name(void *logctx, const HDDVDXpl *x, const char *evo)
{
    for (int a = 0; a < ff_hddvd_xpl_count(&x->root, HDDVD_XPL_TITLE_SET); a++) {
        const HDDVDXplNode *ts = ff_hddvd_xpl_child(&x->root, HDDVD_XPL_TITLE_SET, a);
        for (int b = 0; b < ff_hddvd_xpl_count(ts, HDDVD_XPL_TITLE); b++) {
            const HDDVDXplNode *t = ff_hddvd_xpl_child(ts, HDDVD_XPL_TITLE, b);
            for (int c = 0; c < ff_hddvd_xpl_count(t, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP); c++) {
                const char *s = ff_hddvd_xpl_str(ff_hddvd_xpl_child(t, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, c), "src");
                static const char *const keys[] = { "displayName", "description", "id" };

                if (!src_names_evob(logctx, s, evo))
                    continue;
                for (int k = 0; k < 3; k++) {
                    const char *v = ff_hddvd_xpl_str(t, keys[k]);
                    if (v && *v)
                        return v;
                }
                return "";
            }
        }
    }
    return NULL;
}

static int finish_title(Planner *p, HDDVDTitle *t, int min_length)
{
    const HDDVDEvob *e0 = t->clips[0]->evob;
    const char *name = NULL;
    uint64_t secs;

    for (int i = 0; i < p->nb_xpls; i++)
        if ((name = playlist_name(p->log, p->xpls[i], e0->name)))
            break;
    t->name = keep(p->plan, name && *name ? name : e0->base);
    for (int i = 0; i < t->nb_clips; i++) {
        const HDDVDEvob *e = t->clips[i]->evob;
        t->duration += (e->start_ptm <= e->end_ptm ? e->end_ptm : e->end_ptm + (1ULL << 32)) - e->start_ptm;
        t->size     += t->clips[i]->size;
    }
    if (!t->name)
        return AVERROR(ENOMEM);
    secs = t->duration / 90000;
    if (secs < (uint64_t)min_length) {
        t->not_selected = 1;
        av_log(p->log, AV_LOG_VERBOSE, "Title %s lasts %"PRIu64" s, less than the minimum length of %d s: "
               "it is not selected\n", t->name, secs, min_length);
    }
    return 0;
}

static int add_title(Planner *p, HDDVDTitle *t)
{
    if (!av_dynarray2_add((void **)&p->plan->titles, &p->plan->nb_titles, sizeof(*t), (const uint8_t *)t)) {
        av_free(t->clips);
        av_free(t->marks);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int title_from_candidate(Planner *p, const Candidate *c, int min_length)
{
    HDDVDTitle t = { .from_playlist = 1 };
    int err = 0, ret;

    for (int i = 0; i < c->nb_names; i++) {
        const char *s = c->names[i], *dot = strchr(s, '.');
        const HDDVDEvob *e = NULL;
        HDDVDClip *clip;

        if (strlen(s) > 0xff || !dot || dot - s + 1 > 0xfd) {
            av_log(p->log, AV_LOG_VERBOSE, "Playlist title with clip \"%s\": not a time map name; title left out\n", s);
            goto drop;
        }
        for (int x = 0; x < HDDVD_VTI_MAX_EVOBS && !e; x++)
            if (p->vti->evobs[x] && !av_strncasecmp(p->vti->evobs[x]->name, s, dot - s + 1))
                e = p->vti->evobs[x];
        if (!e) {
            av_log(p->log, AV_LOG_VERBOSE, "Playlist title with clip \"%s\": no such EVOB; title left out\n", s);
            goto drop;
        }
        if (!(clip = clip_of(p, e, &err))) {
            if (err < 0)
                goto fail;
            av_log(p->log, AV_LOG_VERBOSE, "Playlist title with clip \"%s\": the EVOB cannot be used; title left out\n", s);
            goto drop;
        }
        if (av_dynarray_add_nofree(&t.clips, &t.nb_clips, clip) < 0) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
    }
    if (!t.nb_clips)
        goto drop;
    if (c->nb_marks) {
        if (!(t.marks = av_calloc(c->nb_marks, sizeof(*t.marks)))) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        for (int i = 0; i < c->nb_marks; i++)
            t.marks[i] = (HDDVDChapterMark){ keep(p->plan, c->marks[i].name), c->marks[i].ms };
        t.nb_marks = c->nb_marks;
    }
    if (c->lang) {
        const char *code = ff_disc_lang_code(c->lang);
        t.lang = keep(p->plan, code ? code : c->lang);
    }
    if ((ret = finish_title(p, &t, min_length)) < 0) {
        err = ret;
        goto fail;
    }
    return add_title(p, &t);
drop:
    av_free(t.clips);
    return 0;
fail:
    av_free(t.clips);
    av_free(t.marks);
    return err;
}

static int plays(const HDDVDTitlePlan *plan, const HDDVDClip *clip)
{
    for (int i = 0; i < plan->nb_titles; i++) {
        const HDDVDTitle *t = &plan->titles[i];
        if (t->not_selected)
            continue;
        for (int k = 0; k < t->nb_clips; k++)
            if (t->clips[k] == clip)
                return 1;
    }
    return 0;
}

static int nb_selected(const HDDVDTitlePlan *plan)
{
    int n = 0;
    for (int i = 0; i < plan->nb_titles; i++)
        n += !plan->titles[i].not_selected;
    return n;
}

int ff_hddvd_titles_plan(void *logctx, DiscIOFS *fs, const HDDVDVTI *vti, HDDVDXpl *const *xpls,
                         int nb_xpls, int min_length, HDDVDTitlePlan **out)
{
    Planner p = { logctx, fs, vti, xpls, nb_xpls };
    int ret = 0;

    *out = NULL;
    if (!(p.plan = av_mallocz(sizeof(*p.plan))))
        return AVERROR(ENOMEM);

    for (int q = nb_xpls - 1; q >= 0; q--) {
        const HDDVDXplNode *root = &xpls[q]->root, *ts, *fpt;
        int nts = ff_hddvd_xpl_count(root, HDDVD_XPL_TITLE_SET);

        if (nts != 1) {
            av_log(logctx, AV_LOG_ERROR, "Playlist VPLST%03d.XPL has %d TitleSets (1 expected): "
                   "the titles cannot be read\n", xpls[q]->file, nts);
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        ts = ff_hddvd_xpl_child(root, HDDVD_XPL_TITLE_SET, 0);
        if ((fpt = ff_hddvd_xpl_child(ts, HDDVD_XPL_FIRST_PLAY_TITLE, 0)) &&
            (ret = collect(&p, ts, NULL, fpt)) < 0)
            goto end;
        for (int i = 0; i < ff_hddvd_xpl_count(ts, HDDVD_XPL_TITLE); i++)
            if ((ret = collect(&p, ts, ff_hddvd_xpl_child(ts, HDDVD_XPL_TITLE, i),
                               ff_hddvd_xpl_child(ts, HDDVD_XPL_TITLE, i))) < 0)
                goto end;
    }
    av_log(logctx, AV_LOG_VERBOSE, "HD DVD: %d title candidates\n", p.nb_cands);
    for (int i = p.nb_cands - 1; i >= 0; i--)
        if ((ret = title_from_candidate(&p, &p.cands[i], min_length)) < 0)
            goto end;

    /* EVOBs no title plays */
    for (int x = 0; x < HDDVD_VTI_MAX_EVOBS && nb_selected(p.plan); x++) {
        const HDDVDEvob *e = vti->evobs[x];
        HDDVDTitle t = { 0 };
        HDDVDClip *clip;
        int err = 0;

        if (!e)
            continue;
        if (!(clip = clip_of(&p, e, &err))) {
            if (err < 0) {
                ret = err;
                goto end;
            }
            continue;
        }
        if (plays(p.plan, clip))
            continue;
        if (av_dynarray_add_nofree(&t.clips, &t.nb_clips, clip) < 0) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        if ((ret = finish_title(&p, &t, min_length)) < 0 || (ret = add_title(&p, &t)) < 0) {
            av_free(t.clips);
            goto end;
        }
        av_log(logctx, AV_LOG_VERBOSE, "EVOB %s is played by no playlist title: title %s of its own\n",
               e->name, t.name);
    }
    for (int i = 0; i < p.plan->nb_titles; i++) {
        const HDDVDTitle *t = &p.plan->titles[i];
        av_log(logctx, AV_LOG_VERBOSE, "Title %d: %s, %d EVOB(s), %d chapter(s), %"PRIu64" s, %"PRIu64" bytes%s\n",
               i, t->name, t->nb_clips, t->nb_marks, t->duration / 90000, t->size,
               t->not_selected ? ", not selected" : "");
    }
end:
    for (int i = 0; i < p.nb_cands; i++)
        cand_free(&p.cands[i]);
    av_free(p.cands);
    if (ret < 0)
        ff_hddvd_titles_free(&p.plan);
    *out = p.plan;
    return ret;
}

HDDVDClip *ff_hddvd_titles_clip(const HDDVDTitlePlan *plan, int slot)
{
    return slot >= 1 && slot <= HDDVD_VTI_MAX_EVOBS ? plan->clips[slot - 1] : NULL;
}

void ff_hddvd_titles_free(HDDVDTitlePlan **pplan)
{
    HDDVDTitlePlan *plan = *pplan;

    if (!plan)
        return;
    for (int i = 0; i < plan->nb_titles; i++) {
        av_free(plan->titles[i].clips);
        av_free(plan->titles[i].marks);
    }
    av_free(plan->titles);
    for (int i = 0; i < HDDVD_VTI_MAX_EVOBS; i++) {
        if (plan->clips[i]) {
            av_free(plan->clips[i]->extents);
            ff_discio_file_free(&plan->clips[i]->evo);
        }
        av_free(plan->clips[i]);
    }
    for (int i = 0; i < plan->nb_strings; i++)
        av_free(plan->strings[i]);
    av_free(plan->strings);
    av_freep(pplan);
}

char *ff_hddvd_titles_dump(const HDDVDTitlePlan *plan)
{
    AVBPrint bp;
    char *s;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (int i = 0; i < plan->nb_titles; i++) {
        const HDDVDTitle *t = &plan->titles[i];
        av_bprintf(&bp, "title %d|%s|%s|%s|%d|%"PRIu64"|%"PRIu64"|%"PRIu64"|%s|%d|", i, t->name,
                   t->lang ? t->lang : "(null)", t->from_playlist ? "playlist" : "evob", !t->not_selected,
                   t->duration, t->duration / 90000, t->size, t->clips[0]->evob->name, t->nb_clips);
        for (int k = 0; k < t->nb_clips; k++)
            av_bprintf(&bp, "%s%s", k ? "," : "", t->clips[k]->evob->base);
        av_bprintf(&bp, "|%d\n", t->nb_marks);
        for (int k = 0; k < t->nb_marks; k++)
            av_bprintf(&bp, "  chapter %d|%"PRIu32"|%s\n", k + 1, t->marks[k].ms, t->marks[k].name);
    }
    for (int x = 0; x < HDDVD_VTI_MAX_EVOBS; x++) {
        const HDDVDClip *c = plan->clips[x];
        if (!c)
            continue;
        av_bprintf(&bp, "clip %d|%s|%s|%"PRIu64"|", x + 1, c->evob->name,
                   c->state > 0 ? "usable" : "not usable", c->size);
        for (int k = 0; k < c->nb_extents; k++)
            av_bprintf(&bp, "%s%"PRIu32":%"PRIu32":%"PRIu32, k ? "," : "", c->extents[k].stream,
                       c->extents[k].file, c->extents[k].count);
        av_bprintf(&bp, "\n");
    }
    if (av_bprint_finalize(&bp, &s) < 0)
        return NULL;
    return s;
}
