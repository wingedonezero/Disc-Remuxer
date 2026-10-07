/*
 * Blu-ray: movie playlists (BDMV/PLAYLIST/NNNNN.mpls)
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

/*
 * Rules kept from the reference program's parser (each one a reason the
 * playlist is refused, unless said otherwise):
 * - type indicator "MPLS"; AppInfoPlayList length + 4 must be above 5;
 * - every PlayItem ends inside the PlayList block; exactly one primary video
 *   stream per PlayItem; only the video, audio and presentation graphics
 *   stream entries are read (the other counts are skipped with their
 *   entries, which follow them); a stream entry or attribute block of length
 *   0; an angle block with an angle count of 0;
 * - every SubPath ends inside the PlayList block and has a SubPlayItem;
 * - extension 2/1 (STN_table_SS): one block per PlayItem that tiles the
 *   entry exactly; extension 2/2 (SubPath entries): its first SubPath is
 *   read (a count other than 1 is logged);
 * - a failed read of any part.
 * Stream entries keep at most 8 bytes and stream attributes 32 bytes after
 * their type byte; the file position always moves by the recorded length.
 */

#include <string.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "bluray_internal.h"

#define RD(off, len, buf) do { if (!ff_bluray_read(f, (off), (buf), (len))) return 0; } while (0)

/* A stream entry and its attributes at *pos (moved past them). */
static int stn_stream(const BlurayFile *f, BlurayStnStream *r, uint32_t *pos)
{
    uint8_t h[2], a[2];
    uint32_t p = *pos;

    memset(r, 0, sizeof(*r));
    RD(p, 2, h);
    if (!h[0])
        return 0;
    r->entry_len  = h[0] - 1;
    r->entry_type = h[1];
    RD(p + 2, FFMIN(h[0] - 1, 8), r->entry);
    p += 1 + h[0];
    RD(p, 2, a);
    if (!a[0])
        return 0;
    r->coding_type = a[1];
    r->attr_len    = a[0] - 1;
    RD(p + 2, FFMIN(a[0] - 1, 32), r->attr);
    *pos = p + 1 + a[0];
    return 1;
}

/* The angle (PlayItem) or clip (SubPlayItem) list at *pos when the item has
 * one: a count (the item's own clip included), then 10-byte entries. */
static int angles(const BlurayFile *f, int present, BlurayAngleClip **out, int *nb, uint32_t *pos)
{
    uint8_t h[2], e[10];

    if (!present)
        return 1;
    RD(*pos, 2, h);
    *pos += 2;
    if (!h[0])
        return 0;
    if (!(*out = av_calloc(h[0] - 1 ? h[0] - 1 : 1, sizeof(**out))))
        return AVERROR(ENOMEM);
    *nb = h[0] - 1;
    for (int k = 0; k < h[0] - 1; k++) {
        BlurayAngleClip *r = &(*out)[k];
        RD(*pos, 10, e);
        memcpy(r->clip, e, 5);
        memcpy(r->codec, e + 5, 4);
        r->pos    = *pos + 5;
        r->stc_id = e[9];
        *pos += 10;
    }
    return 1;
}

static int sub_play_item(const BlurayFile *f, BluraySubPlayItem *r, uint32_t off)
{
    uint8_t b[0x18], c[6];
    uint32_t q;

    RD(off, 0x18, b);
    memcpy(r->clip, b + 2, 5);
    memcpy(r->codec, b + 7, 4);
    r->pos        = off + 7;
    r->connection = (b[0xe] >> 1) & 0xf;
    r->multi_clip = b[0xe] & 1;
    r->stc_id     = b[0xf];
    r->in_time    = AV_RB32(b + 0x10);
    r->out_time   = AV_RB32(b + 0x14);
    RD(off + 0x18, 6, c);
    r->sync_play_item = AV_RB16(c);
    r->sync_start_pts = AV_RB32(c + 2);
    q = off + 0x1e;
    return angles(f, r->multi_clip, &r->clips, &r->nb_clips, &q);
}

static int sub_path(void *log, const BlurayFile *f, BluraySubPath *sp, uint32_t off)
{
    uint8_t h[10], l[2];
    uint32_t p;
    int n, ret;

    RD(off, 10, h);
    sp->type = h[5];
    n = h[9];
    if (!n)
        return 0;
    RD(off + 10, 2, l);
    if (!(sp->items = av_calloc(n, sizeof(*sp->items))))
        return AVERROR(ENOMEM);
    sp->nb_items = 1;
    if ((ret = sub_play_item(f, &sp->items[0], off + 10)) <= 0) {
        av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: SubPlayItem 0 cannot be read\n", f->path);
        return ret;
    }
    p = off + 12 + AV_RB16(l);
    for (int k = 1; k < n; k++) {
        RD(p, 2, l);
        sp->nb_items = k + 1;
        if ((ret = sub_play_item(f, &sp->items[k], p)) <= 0) {
            av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: SubPlayItem %d cannot be read\n", f->path, k);
            return ret;
        }
        p += 2 + AV_RB16(l);
    }
    return 1;
}

static int marks(const BlurayFile *f, BlurayPlaylist *pl, uint32_t off)
{
    uint8_t h[6], r[14];
    int n;

    RD(off, 6, h);
    if (!(n = AV_RB16(h + 4)))
        return 1;
    if (!(pl->marks = av_calloc(n, sizeof(*pl->marks))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < n; i++) {
        BlurayMark *m = &pl->marks[i];
        RD(off + 6 + 14 * i, 14, r);
        m->type      = r[1];
        m->play_item = AV_RB16(r + 2);
        m->time      = AV_RB32(r + 4);
        m->pid       = AV_RB16(r + 8);
        m->duration  = AV_RB32(r + 10);
        pl->nb_marks = i + 1;
    }
    return 1;
}

static int play_item(void *log, const BlurayFile *f, BlurayPlayItem *it, uint32_t o)
{
    uint8_t b[0x16], s[0x10], x[4];
    uint32_t q;
    int ret;

    RD(o, 0x16, b);
    memcpy(it->clip, b + 2, 5);
    memcpy(it->codec, b + 7, 4);
    it->pos         = o + 7;
    it->connection  = b[0xc] & 0xf;
    it->multi_angle = (b[0xc] >> 4) & 1;
    it->stc_id      = b[0xd];
    it->in_time     = AV_RB32(b + 0xe);
    it->out_time    = AV_RB32(b + 0x12);
    RD(o + 0x1e, 4, x);                   /* read, not used */
    q = o + 0x22;
    if ((ret = angles(f, it->multi_angle, &it->angles, &it->nb_angles, &q)) <= 0)
        return ret;
    RD(q, 0x10, s);
    q += 0x10;
    it->nb_ig = s[7];
    if (s[4] != 1) {
        av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: a PlayItem with %d primary video streams\n", f->path, s[4]);
        return 0;
    }
    if (!stn_stream(f, &it->video, &q))
        return 0;
    if (s[5] && !(it->audio = av_calloc(s[5], sizeof(*it->audio))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < s[5]; i++, it->nb_audio++)
        if (!stn_stream(f, &it->audio[i], &q))
            return 0;
    if (s[6] && !(it->pg = av_calloc(s[6], sizeof(*it->pg))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < s[6]; i++, it->nb_pg++)
        if (!stn_stream(f, &it->pg[i], &q))
            return 0;
    return 1;
}

/* Extension 2/1: one STN_table_SS block per PlayItem. */
static int stn_ss(void *log, const BlurayFile *f, BlurayPlaylist *pl, const BlurayExtEntry *e)
{
    uint32_t p = e->start, lim = e->start + e->length;

    for (int i = 0; i < pl->nb_items; i++) {
        BlurayPlayItem *it = &pl->items[i];
        uint8_t l[2], b[2], c[2];
        uint32_t nb, q, r;

        if (p + 2 > lim)
            return 0;
        RD(p, 2, l);
        nb = p + (uint16_t)(AV_RB16(l) + 2);
        if (nb > lim)
            return 0;
        RD(p + 2, 2, b);
        it->ss.present            = 1;
        it->ss.fixed_offset_popup = b[0] >> 7;
        q = p + 4;
        if (!stn_stream(f, &it->ss.dep_video, &q))
            return 0;
        if (q + 2 > nb) {
            av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: STN_table_SS block of PlayItem %d too short\n", f->path, i);
            return 0;
        }
        RD(q, 2, c);
        it->ss.nb_offset_seq = c[1] & 0x3f;
        if (it->nb_pg && !(it->ss.pg = av_calloc(it->nb_pg, sizeof(*it->ss.pg))))
            return AVERROR(ENOMEM);
        r = q + 2;
        for (int k = 0; k < it->nb_pg; k++) {
            uint8_t *R = it->ss.pg[k], d[2], e1[2], e2[2], g[2];
            uint32_t t;

            if (r + 2 > nb) {
                av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: STN_table_SS PG record %d of PlayItem %d too short\n",
                       f->path, k, i);
                return 0;
            }
            RD(r, 2, d);
            R[0] = d[0];
            R[1] = d[1];
            if (!(d[1] & 7)) {
                r += 2;
                continue;
            }
            if (r + 3 > nb)
                return 0;
            RD(r + 2, 2, e1);
            if (!e1[0])
                return 0;
            R[3] = e1[0] - 1;
            R[2] = e1[1];
            RD(r + 4, FFMIN(e1[0] - 1, 8), R + 4);
            t = r + 3 + e1[0];
            if (R[1] & 4) {
                if (t + 1 > nb)
                    return 0;
                RD(t, 2, e2);
                if (!e2[0])
                    return 0;
                R[0xd] = e2[0] - 1;
                R[0xc] = e2[1];
                RD(t + 2, FFMIN(e2[0] - 1, 8), R + 0xe);
                t += 1 + e2[0];
            }
            if (t + 2 > nb)
                return 0;
            RD(t, 2, g);
            R[0x16] = g[1];
            r = t + 2;
        }
        p = nb;
    }
    if (p != lim) {
        av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: STN_table_SS blocks end at %u, the entry at %u\n", f->path, p, lim);
        return 0;
    }
    return 1;
}

/* The extension entries this parser reads itself. */
static int mpls_parsed_directly(uint16_t id1, uint16_t id2)
{
    return id1 == 2 && (uint16_t)(id2 - 1) < 2;
}

int ff_bluray_mpls_parse(void *log, const BlurayFile *f, BlurayPlaylist *pl)
{
    uint8_t H[0x28], A[0x12] = { 0 }, T[10], l[4];
    uint32_t P, M, X, L, end, o;
    int nPI, nSP, ret;

    pl->ext_subpath_type = 0xffff;
    RD(0, 0x28, H);
    ff_bluray_header(H, &pl->hdr);
    if (pl->hdr.type != MKBETAG('M', 'P', 'L', 'S'))
        return 0;
    P = AV_RB32(H + 8);
    M = AV_RB32(H + 0xc);
    X = AV_RB32(H + 0x10);

    /* AppInfoPlayList */
    RD(0x28, 4, l);
    L = AV_RB32(l) + 4;
    if (L <= 5) {
        av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: AppInfoPlayList length %u\n", f->path, L);
        return 0;
    }
    RD(0x28, FFMIN(L, 0x12), A);
    pl->playback_type  = A[5];
    pl->playback_count = AV_RB16(A + 6);
    pl->app_flags      = AV_RB16(A + 0x10);

    /* PlayList: PlayItems, SubPaths */
    RD(P, 10, T);
    end = P + AV_RB32(T) + 4;
    nPI = AV_RB16(T + 6);
    nSP = AV_RB16(T + 8);
    if (nPI && !(pl->items = av_calloc(nPI, sizeof(*pl->items))))
        return AVERROR(ENOMEM);
    o = P + 10;
    for (int i = 0; i < nPI; i++) {
        uint32_t next;
        RD(o, 2, l);
        next = o + 2 + AV_RB16(l);
        if (next > end)
            return 0;
        pl->nb_items = i + 1;
        if ((ret = play_item(log, f, &pl->items[i], o)) <= 0) {
            av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: PlayItem %d cannot be read\n", f->path, i);
            return ret;
        }
        o = next;
    }
    if (nSP && !(pl->subpaths = av_calloc(nSP, sizeof(*pl->subpaths))))
        return AVERROR(ENOMEM);
    for (int j = 0; j < nSP; j++) {
        uint32_t next;
        RD(o, 4, l);
        next = o + AV_RB32(l) + 4;
        if (next > end)
            return 0;
        pl->nb_subpaths = j + 1;
        if ((ret = sub_path(log, f, &pl->subpaths[j], o)) <= 0) {
            av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: SubPath %d cannot be read\n", f->path, j);
            return ret;
        }
        o = next;
    }

    /* PlayListMark, ExtensionData */
    if (M && (ret = marks(f, pl, M)) <= 0)
        return ret;
    if (X && (ret = ff_bluray_ext_data(log, f, X, mpls_parsed_directly, &pl->ext, &pl->nb_ext)) <= 0)
        return ret;
    for (int i = 0; i < pl->nb_ext; i++) {
        const BlurayExtEntry *e = &pl->ext[i];

        if (e->id1 == 2 && e->id2 == 1 && (ret = stn_ss(log, f, pl, e)) <= 0)
            return ret;
        if (e->id1 == 2 && e->id2 == 2) {
            uint8_t h[6];
            int n;
            RD(e->start, 6, h);
            n = AV_RB16(h + 4);
            if (n != 1)
                av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: %d SubPath extension entries: the first is read\n",
                       f->path, n);
            if (n) {
                if ((ret = sub_path(log, f, &pl->ext_subpath, e->start + 6)) <= 0)
                    return ret;
                pl->ext_subpath_type = pl->ext_subpath.type;
            }
        }
    }
    return 1;
}

static void sub_path_free(BluraySubPath *sp)
{
    for (int i = 0; i < sp->nb_items; i++)
        av_freep(&sp->items[i].clips);
    av_freep(&sp->items);
    sp->nb_items = 0;
}

void ff_bluray_playlist_free(BlurayPlaylist *pl)
{
    for (int i = 0; i < pl->nb_items; i++) {
        BlurayPlayItem *it = &pl->items[i];
        av_freep(&it->angles);
        av_freep(&it->audio);
        av_freep(&it->pg);
        av_freep(&it->ss.pg);
    }
    av_freep(&pl->items);
    for (int j = 0; j < pl->nb_subpaths; j++)
        sub_path_free(&pl->subpaths[j]);
    av_freep(&pl->subpaths);
    sub_path_free(&pl->ext_subpath);
    av_freep(&pl->marks);
    ff_bluray_ext_free(&pl->ext, &pl->nb_ext);
    pl->nb_items = pl->nb_subpaths = pl->nb_marks = 0;
}
