/*
 * DVD-Video: the stream pass of a title. The title's largest segment (the
 * first of the largest) is read from its start for what its title set's
 * attributes cannot say:
 *
 * - closed captions: the video packets (stream 0xE0) of its first 8 MiB
 *   are searched for the DVD caption user data (user_data_start_code
 *   00 00 01 B2, then 'C' 'C' 0x01 0xF8, its next byte's bit 6 clear, nine
 *   bytes or more to the payload's end); found: the title gets a caption
 *   track (English, after the sub-pictures);
 * - each audio stream's packets (MPEG audio stream_id 0xC0 + n, private
 *   stream 1 sub-streams AC-3 0x80 + n, DTS 0x88 + n, LPCM 0xA0 + n): none
 *   in the first 48 MiB and the program chain's audio_control does not list
 *   it -> left out; listed -> also the first 200 MiB, the 100 MiB from the
 *   middle and the last 100 MiB are read, none there -> left out as empty.
 *   Whether audio_control lists it is looked up by the stream's place in
 *   the title's audio list (audio_control's streams come first, in its
 *   order), not by its stream number; where the two differ a debug line
 *   says so. An audio stream of an unknown coding mode is left out.
 *
 * Every sector read goes through the sector check (dvd_read.c: CSS); the
 * stream keys of the first two packets of each sector are kept, each zone
 * is read only once (a later search goes on where an earlier one stopped),
 * and a search stops at the sector that has what it looks for. A sector
 * that cannot be read (or a scrambled one without a key) fails the pass:
 * the title cannot be used.
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

#define _GNU_SOURCE                     /* memmem */
#include <inttypes.h>
#include <string.h>

#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"
#include "mpeg.h"

#define ZONE0       (200LL << 20)       /* the segment's start; longer segments get the two zones below */
#define ZONE_SIZE   (100LL << 20)       /* from the middle; at the end */
#define CC_LIMIT    (8LL << 20)
#define AUDIO_FIRST (48LL << 20)
#define CC_KEY      0xffe0              /* kept with the keys: captions found */

typedef struct SegScan {
    DVDDisc        *d;
    const DVDTitle *t;
    int             seg;
    int64_t         start[3], len[3], done[3];
    uint16_t       *keys;               /* sorted, each once */
    int             nb_keys, alloc_keys;
} SegScan;

static int has_key(const SegScan *g, uint16_t k)
{
    int lo = 0, hi = g->nb_keys - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g->keys[mid] == k)
            return 1;
        if (g->keys[mid] < k)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

static int add_key(SegScan *g, uint16_t k)
{
    int at = 0;

    if (has_key(g, k))
        return 0;
    if (g->nb_keys == g->alloc_keys) {
        uint16_t *n = av_realloc_array(g->keys, g->alloc_keys + 16, sizeof(*n));
        if (!n)
            return AVERROR(ENOMEM);
        g->keys = n;
        g->alloc_keys += 16;
    }
    while (at < g->nb_keys && g->keys[at] < k)
        at++;
    memmove(g->keys + at + 1, g->keys + at, (g->nb_keys - at) * sizeof(*g->keys));
    g->keys[at] = k;
    g->nb_keys++;
    return 0;
}

/* The DVD caption user data in packet p's payload (the first match decides). */
static int caption_header(const uint8_t *sec, const MpegPsDiscPacket *p)
{
    static const uint8_t pat[8] = { 0x00, 0x00, 0x01, 0xb2, 'C', 'C', 0x01, 0xf8 };
    const uint8_t *pl = sec + p->at + p->payload, *q;
    int n = FFMIN(p->len, MPEGPS_DISC_SECTOR - p->at) - p->payload;

    if (n <= 7 || !(q = memmem(pl, n, pat, sizeof(pat))))
        return 0;
    return pl + n - q > 8 && !(q[8] & 0x40);
}

/* One sector's first two packets: their keys kept; 1 when one is want (cc:
 * a video packet with the caption user data). */
static int walk(SegScan *g, const uint8_t *sec, uint16_t want, int cc)
{
    MpegPsDiscPacket p;
    int off = 0, found = 0, ret;

    for (int n = 0; n < 2 && off < MPEGPS_DISC_SECTOR; n++) {
        if (!ff_mpegps_disc_packet(sec, &off, &p, 0))
            break;
        if ((ret = add_key(g, p.key)) < 0)
            return ret;
        if ((uint16_t)p.key != want)
            continue;
        if (!cc) {
            found = 1;
        } else if (caption_header(sec, &p)) {
            if ((ret = add_key(g, CC_KEY)) < 0)
                return ret;
            found = 1;
        }
    }
    return found;
}

/* Zone zone read on from where it stopped, up to max bytes of it, until a
 * sector has want: 0 done, < 0 a sector cannot be read. */
static int scan(SegScan *g, uint16_t want, int cc, int zone, int64_t max)
{
    int64_t lim = FFMIN(g->len[zone], max), pos = g->start[zone] + g->done[zone], end = g->start[zone] + lim;
    uint8_t buf[DVD_SECTOR];
    int ret;

    if (g->done[zone] >= g->len[zone] || pos >= end)
        return 0;
    for (; pos < end; pos += DVD_SECTOR) {
        if ((ret = ff_dvd_segment_read(g->d, g->t, g->seg, pos / DVD_SECTOR, buf)) < 0) {
            av_log(g->d->log, AV_LOG_ERROR, "DVD: title %s: sector %"PRId64" of segment %d cannot be read: the "
                   "title's streams cannot be found\n", g->t->name, pos / DVD_SECTOR, g->seg + 1);
            return ret;
        }
        g->done[zone] += DVD_SECTOR;
        if (ret && (ret = walk(g, buf, want, cc)) != 0)
            return ret < 0 ? ret : 0;
    }
    return 0;
}

/* An audio stream's key (by its title set's attributes), 0 when its coding
 * mode is not known. */
static uint16_t audio_key(const DVDTitle *t, int i, int *coding)
{
    const uint8_t *a = t->vts->header + DVD_VTS_AUDIO_ATTR + 8 * t->audio[i].attr;
    int s = t->audio[i].stream;

    switch (*coding = a[0] >> 5) {
    case 0:  return (0x80 + s) << 8 | 0xbd;        /* AC-3 */
    case 2:
    case 3:  return 0xc0 + s;                       /* MPEG audio */
    case 4:  return (0xa0 + s) << 8 | 0xbd;        /* LPCM */
    case 6:  return (0x88 + s) << 8 | 0xbd;        /* DTS */
    default: return 0;
    }
}

int ff_dvd_title_streams(DVDDisc *d, DVDTitle *t)
{
    SegScan g = { d, t };
    int64_t size, best = -1;
    int ret = 0;

    t->audio_dropped = 0;
    t->captions = 0;
    if (!t->nb_segments)
        return 0;
    for (int k = 0; k < t->nb_segments; k++)
        if (t->segments[k].size > best) {
            best = t->segments[k].size;
            g.seg = k;
        }
    size = (int64_t)ff_dvd_segment_blocks(&t->segments[g.seg]) * DVD_SECTOR;
    g.len[0] = FFMIN(size, ZONE0);
    if (size > ZONE0) {
        int64_t h = size / 2 - size / 2 % DVD_SECTOR;
        g.start[1] = h > ZONE0 ? h : ZONE0;
        g.len[1]   = g.start[1] + ZONE_SIZE <= size ? ZONE_SIZE : size - g.start[1];
        g.start[2] = FFMAX(g.start[1] + g.len[1], size - ZONE_SIZE);
        g.len[2]   = FFMIN(ZONE_SIZE, size - g.start[2]);
    }

    if ((ret = scan(&g, 0xe0, 1, 0, CC_LIMIT)) < 0)
        goto end;
    if ((t->captions = has_key(&g, CC_KEY)))
        av_log(d->log, AV_LOG_VERBOSE, "DVD: title %s: closed captions in the video: a caption track\n", t->name);

    for (int i = 0; i < t->nb_audio; i++) {
        int coding, s = t->audio[i].stream, listed = t->audio_listed >> i & 1;
        uint16_t key = audio_key(t, i, &coding);

        if (!key) {
            av_log(d->log, AV_LOG_WARNING, "DVD: title %s: audio stream %d has coding mode %d, which is not known: "
                   "left out\n", t->name, s, coding);
            t->audio_dropped |= 1 << i;
            continue;
        }
        if (has_key(&g, key))
            continue;
        if ((ret = scan(&g, key, 0, 0, AUDIO_FIRST)) < 0)
            goto end;
        if (has_key(&g, key))
            continue;
        if (listed != (t->audio_listed >> s & 1))
            av_log(d->log, AV_LOG_DEBUG, "DVD: title %s: audio stream %d is audio track %d: whether audio_control "
                   "lists it is taken from track %d's place (%s), not from the stream number (%s)\n", t->name, s,
                   i, i, listed ? "listed" : "not listed", listed ? "not listed" : "listed");
        if (!listed) {
            av_log(d->log, AV_LOG_INFO, "DVD: title %s: audio stream %d has no data in the first 48 MiB and the "
                   "program chain does not list it: left out\n", t->name, s);
            t->audio_dropped |= 1 << i;
            continue;
        }
        for (int z = 0; z < 3 && !has_key(&g, key); z++)
            if ((ret = scan(&g, key, 0, z, z ? ZONE_SIZE : ZONE0)) < 0)
                goto end;
        if (has_key(&g, key))
            continue;
        av_log(d->log, AV_LOG_WARNING, "DVD: title %s: audio stream %d (track %d) looks empty (no data in the first "
               "200 MiB, the 100 MiB from the middle and the last 100 MiB of segment %d): left out\n", t->name, s, i,
               g.seg + 1);
        t->audio_dropped |= 1 << i;
    }
end:
    av_free(g.keys);
    return ret;
}
