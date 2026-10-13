/*
 * Blu-ray: fake playlists that hide the main feature
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
 * Some discs with BD-J menus carry dozens of feature-length playlists of
 * which only one plays the film as made: the menu program knows which one.
 * The fakes seen so far play the real clips with a short scene added (as
 * several stream files that are byte-identical copies of one clip) or one
 * left out. Without running the menu program:
 * - a playlist of LONG_PLAYLIST or more that plays copies of one clip from
 *   different stream files more than once is likely fake (a playlist playing
 *   the same stream file again, like a menu loop, is not);
 * - with fakes found, a playlist of that length that plays no clip twice
 *   (neither a copy nor the same file) and is at least as long as the
 *   shortest fake without its repeated copies is likely real (a fake may add
 *   scenes, leave one out, or both);
 * - without fakes, SUSPECT_ORDERS or more different clip orders among the
 *   playlists of an hour or more (menu loops left out) are said as possibly
 *   fakes of another kind (LOOK INTO AGAIN LATER).
 * Only on discs with BD-J titles. Findings go to the standard log and the
 * disc info; the title list is not changed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/str.h"
#include "base/mem.h"

#include "bluray_internal.h"

#define LONG_PLAYLIST   (20 * 60 * 45000)   /**< 45 kHz */
#define HOUR            (60 * 60 * 45000)
#define SUSPECT_ORDERS  10                  /**< from 4 discs: 42 on one with fakes, at most 3 on others;
                                                 LOOK INTO AGAIN LATER with more discs */
#define CHUNK           (1 << 20)

/* The clip a PlayItem plays (its Clip_Information_file_name), -1 not loaded. */
static int clip_index(const BlurayDisc *d, const char *name)
{
    uint32_t n = strtoul(name, NULL, 10);

    for (int i = 0; i < d->nb_clips; i++)
        if (d->clips[i].number == n)
            return i;
    return -1;
}

static int64_t playlist_length(const BlurayPlaylist *pl)
{
    int64_t len = 0;

    for (int i = 0; i < pl->nb_items; i++)
        len += (int64_t)pl->items[i].out_time - pl->items[i].in_time;
    return len;
}

static void hms(char *buf, size_t size, int64_t t45)
{
    int64_t s = t45 / 45000;
    snprintf(buf, size, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
}

/* Whether the stream files of clips a and b are byte-identical (their sizes
 * are equal): 1, 0, < 0 one cannot be read (logged, taken as different). */
static int same_file(void *log, const BlurayDisc *d, const BlurayClip *a, const BlurayClip *b, uint8_t *buf)
{
    DiscIOFile *fa = NULL, *fb = NULL;
    int ret;

    if ((ret = d->fs->ops->open_file(d->fs, a->stream_path, &fa)) < 0 ||
        (ret = d->fs->ops->open_file(d->fs, b->stream_path, &fb)) < 0) {
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: fake playlist check: %s cannot be opened (%s): left out of the "
               "comparison\n", fa ? b->stream_path : a->stream_path, dr_err_text(ret));
        goto end;
    }
    for (int64_t pos = 0; pos < a->stream_size; pos += CHUNK) {
        int len = (int)DR_MIN(CHUNK, a->stream_size - pos);
        const char *bad = a->stream_path;
        if ((ret = ff_discio_file_read(d->fs, fa, pos, buf, len)) < 0 ||
            (bad = b->stream_path, ret = ff_discio_file_read(d->fs, fb, pos, buf + CHUNK, len)) < 0) {
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: fake playlist check: %s cannot be read at byte %"PRId64" (%s): "
                   "left out of the comparison\n", bad, pos, dr_err_text(ret));
            goto end;
        }
        if (memcmp(buf, buf + CHUNK, len)) {
            ret = 0;
            goto end;
        }
    }
    ret = 1;
end:
    ff_discio_file_free(&fa);
    ff_discio_file_free(&fb);
    return ret;
}

/* copy[i]: the clip whose content clip i has (the lowest clip index with the
 * same bytes; i itself when none), for the clips the long playlists play. */
static int find_copies(void *log, const BlurayDisc *d, const uint8_t *used, int *copy)
{
    uint8_t *buf = NULL;
    int compared = 0;

    for (int i = 0; i < d->nb_clips; i++)
        copy[i] = i;
    for (int i = 0; i < d->nb_clips; i++) {
        if (!used[i] || !d->clips[i].stream_path[0] || !d->clips[i].stream_size)
            continue;
        for (int j = 0; j < i; j++) {
            int ret;
            if (!used[j] || copy[j] != j || !d->clips[j].stream_path[0] ||
                d->clips[j].stream_size != d->clips[i].stream_size)
                continue;
            if (!buf && !(buf = dr_malloc(2 * CHUNK)))
                return DR_ERR(ENOMEM);
            compared++;
            if ((ret = same_file(log, d, &d->clips[j], &d->clips[i], buf)) > 0) {
                copy[i] = j;
                break;
            }
        }
    }
    dr_free(buf);
    dr_vlog(log, DR_VLOG_VERBOSE, "Blu-ray: fake playlist check: %d pair(s) of stream files of equal size compared\n",
           compared);
    return 0;
}

/* The items of playlist pl that play a copy (another stream file) of a clip
 * it played before; *any: also the same file played again; *rest: its
 * length without those copies (45 kHz). */
static int repeats(const BlurayDisc *d, const BlurayPlaylist *pl, const int *copy, int *any, int64_t *rest)
{
    int n = 0;

    *any  = 0;
    *rest = 0;
    for (int i = 0; i < pl->nb_items; i++) {
        int ci = clip_index(d, pl->items[i].clip), copied = 0, again = 0;
        if (ci < 0)
            continue;
        for (int j = 0; j < i; j++) {
            int cj = clip_index(d, pl->items[j].clip);
            if (cj < 0 || copy[cj] != copy[ci])
                continue;
            if (cj != ci)
                copied = 1;
            again = 1;
        }
        n      += copied;
        *any   |= again;
        if (!copied)
            *rest += (int64_t)pl->items[i].out_time - pl->items[i].in_time;
    }
    return n;
}

/* The copies a fake plays, as "00002, 00137, 00143 (one clip)": every clip
 * content it plays from more than one stream file. */
static void copies_text(const BlurayDisc *d, const BlurayPlaylist *pl, const int *copy, DrStr *bp)
{
    int groups = 0;

    for (int i = 0; i < pl->nb_items; i++) {
        int ci = clip_index(d, pl->items[i].clip), first = 1, files = 0, plays = 0;
        if (ci < 0)
            continue;
        for (int j = 0; j < i; j++) {          /* this content said already */
            int cj = clip_index(d, pl->items[j].clip);
            if (cj >= 0 && copy[cj] == copy[ci])
                first = 0;
        }
        if (!first)
            continue;
        for (int j = i; j < pl->nb_items; j++) {
            int cj = clip_index(d, pl->items[j].clip), seen = 0;
            if (cj < 0 || copy[cj] != copy[ci])
                continue;
            plays++;
            for (int k = i; k < j; k++)
                seen |= !strcmp(pl->items[k].clip, pl->items[j].clip);
            files += !seen;
        }
        if (files < 2)
            continue;
        dr_str_catf(bp, "%s%d times from %d copies of one clip (", groups++ ? "; " : "", plays, files);
        for (int j = i, n = 0; j < pl->nb_items; j++) {
            int cj = clip_index(d, pl->items[j].clip), seen = 0;
            if (cj < 0 || copy[cj] != copy[ci])
                continue;
            for (int k = i; k < j; k++)
                seen |= !strcmp(pl->items[k].clip, pl->items[j].clip);
            if (!seen)
                dr_str_catf(bp, "%s%s", n++ ? ", " : "", pl->items[j].clip);
        }
        dr_str_catf(bp, ")");
    }
}

/* Different clip orders (clip contents with their in and out times) among
 * the playlists of an hour or more that are not one clip played again and
 * again. */
static int long_orders(const BlurayDisc *d, const int *copy)
{
    int n = 0;

    for (int p = 0; p < d->nb_playlists; p++) {
        const BlurayPlaylist *pl = &d->playlists[p];
        int loop = pl->nb_items > 1, dup = 0;
        if (pl->bdav || playlist_length(pl) < HOUR)
            continue;
        for (int i = 1; i < pl->nb_items; i++)
            loop &= !strcmp(pl->items[i].clip, pl->items[0].clip);
        if (loop)
            continue;
        for (int q = 0; q < p && !dup; q++) {
            const BlurayPlaylist *o = &d->playlists[q];
            int same = !o->bdav && o->nb_items == pl->nb_items && playlist_length(o) >= HOUR;
            for (int i = 0; i < pl->nb_items && same; i++) {
                int a = clip_index(d, pl->items[i].clip), b = clip_index(d, o->items[i].clip);
                same = a >= 0 && b >= 0 && copy[a] == copy[b] &&
                       pl->items[i].in_time == o->items[i].in_time && pl->items[i].out_time == o->items[i].out_time;
            }
            dup = same;
        }
        n += !dup;
    }
    return n;
}

int ff_bluray_decoys_check(void *log, BlurayDisc *d)
{
    uint8_t *used = NULL;
    int *copy = NULL, *any = NULL, *order = NULL, ret = 0;
    int64_t shortest = INT64_MAX;
    DrStr bp;

    dr_freep(&d->pl_check);
    dr_freep(&d->pl_repeats);
    d->nb_fake = d->nb_real = d->fake_suspected = 0;
    if (d->bdav || !ff_bluray_has_bdj(d) || !d->nb_playlists)
        return 0;
    used          = dr_calloc(DR_MAX(d->nb_clips, 1), sizeof(*used));
    copy          = dr_calloc(DR_MAX(d->nb_clips, 1), sizeof(*copy));
    any           = dr_calloc(d->nb_playlists, sizeof(*any));
    d->pl_check   = dr_calloc(d->nb_playlists, sizeof(*d->pl_check));
    d->pl_repeats = dr_calloc(d->nb_playlists, sizeof(*d->pl_repeats));
    order         = dr_calloc(d->nb_playlists, sizeof(*order));
    if (!used || !copy || !any || !d->pl_check || !d->pl_repeats || !order) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    /* the findings are said in playlist number order */
    for (int i = 0; i < d->nb_playlists; i++) {
        int j = i;
        for (; j > 0 && d->playlists[order[j - 1]].number > d->playlists[i].number; j--)
            order[j] = order[j - 1];
        order[j] = i;
    }
    for (int p = 0; p < d->nb_playlists; p++) {
        const BlurayPlaylist *pl = &d->playlists[p];
        if (pl->bdav || playlist_length(pl) < LONG_PLAYLIST)
            continue;
        for (int i = 0; i < pl->nb_items; i++) {
            int ci = clip_index(d, pl->items[i].clip);
            if (ci >= 0)
                used[ci] = 1;
        }
    }
    if ((ret = find_copies(log, d, used, copy)) < 0)
        goto end;
    for (int i = 0; i < d->nb_clips; i++) {   /* each set of byte-identical stream files, once */
        int n = 0;
        if (copy[i] != i)
            continue;
        dr_str_init(&bp);
        for (uint32_t last = 0;;) {           /* in number order */
            int next = -1;
            for (int j = i; j < d->nb_clips; j++)
                if (copy[j] == i && (!n || d->clips[j].number > last) &&
                    (next < 0 || d->clips[j].number < d->clips[next].number))
                    next = j;
            if (next < 0)
                break;
            last = d->clips[next].number;
            dr_str_catf(&bp, "%s%05u", n++ ? ", " : "", last);
        }
        if (n > 1)
            dr_vlog(log, DR_VLOG_INFO, "Blu-ray: clips %s have byte-identical stream files (%"PRId64" bytes each)\n",
                   bp.str, d->clips[i].stream_size);
        dr_str_free(&bp);
    }
    for (int p = 0; p < d->nb_playlists; p++) {
        const BlurayPlaylist *pl = &d->playlists[p];
        int64_t rest;
        if (pl->bdav || playlist_length(pl) < LONG_PLAYLIST)
            continue;
        d->pl_repeats[p] = repeats(d, pl, copy, &any[p], &rest);
        if (d->pl_repeats[p]) {
            d->pl_check[p] = BLURAY_PL_FAKE;
            d->nb_fake++;
            shortest = DR_MIN(shortest, rest);
        }
    }
    if (d->nb_fake) {
        DrStr reals;
        char t[16];
        dr_str_init(&reals);
        for (int o = 0; o < d->nb_playlists; o++) {
            int p = order[o];
            const BlurayPlaylist *pl = &d->playlists[p];
            if (pl->bdav || d->pl_check[p] || any[p] || playlist_length(pl) < DR_MAX(shortest, LONG_PLAYLIST))
                continue;
            d->pl_check[p] = BLURAY_PL_REAL;
            dr_str_catf(&reals, "%s%05u", d->nb_real++ ? ", " : "", pl->number);
        }
        hms(t, sizeof(t), shortest);
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: fake playlists: %d long playlist(s) play copies of one clip more than "
               "once, a way to hide the main feature among fakes; likely real (no clip played twice, at least as "
               "long as the shortest fake without its repeated copies, %s): %s\n", d->nb_fake, t, d->nb_real ? reals.str : "none");
        dr_str_free(&reals);
        for (int o = 0; o < d->nb_playlists; o++) {
            int p = order[o];
            const BlurayPlaylist *pl = &d->playlists[p];
            hms(t, sizeof(t), playlist_length(pl));
            if (d->pl_check[p] == BLURAY_PL_FAKE) {
                dr_str_init(&bp);
                copies_text(d, pl, copy, &bp);
                dr_vlog(log, DR_VLOG_INFO, "Blu-ray: playlist %05u (%s): likely fake: plays %s\n", pl->number, t,
                       bp.str);
                dr_str_free(&bp);
            } else if (d->pl_check[p] == BLURAY_PL_REAL)
                dr_vlog(log, DR_VLOG_INFO, "Blu-ray: playlist %05u (%s): likely real\n", pl->number, t);
        }
    } else {
        int orders = long_orders(d, copy);
        if (orders >= SUSPECT_ORDERS) {
            d->fake_suspected = 1;
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: %d different clip orders among the playlists of an hour or more, "
                   "but none plays a copy of a clip twice: possibly fake playlists of a kind not recognised "
                   "(LOOK INTO AGAIN LATER)\n", orders);
        } else
            dr_vlog(log, DR_VLOG_VERBOSE, "Blu-ray: fake playlist check: none found (%d different clip orders "
                   "among the playlists of an hour or more)\n", orders);
    }
end:
    dr_free(used);
    dr_free(copy);
    dr_free(any);
    dr_free(order);
    if (ret < 0) {
        dr_freep(&d->pl_check);
        dr_freep(&d->pl_repeats);
    }
    return ret;
}
