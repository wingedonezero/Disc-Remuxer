/*
 * Blu-ray: BDAV (recordable disc) playlists (BDAV/PLAYLIST/NNNNN.rpls)
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
 * Read as the reference program reads them: the first 0x158 bytes (type
 * indicator "PLST", the UIAppInfoPlayList with the playlist's name: its
 * length at 0x58, up to 255 bytes from 0x59), then the PlayList block: its
 * type (byte 5, low 4 bits) and at most 200 PlayItems, each ending inside
 * the block: clip name, codec identifier, connection condition, STC id (only
 * for PlayList type 1, else 0xff), IN / OUT times. No angles, no stream
 * table, no marks are read.
 */

#include <string.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "bluray_internal.h"

#define MAX_ITEMS 200

int ff_bluray_rpls_parse(void *log, const BlurayFile *f, BlurayPlaylist *pl)
{
    uint8_t H[0x158], T[10], l[2], b[0x16];
    uint32_t P, end, pos;
    int n;

    pl->bdav             = 1;
    pl->ext_subpath_type = 0xffff;
    if (!ff_bluray_read(f, 0, H, sizeof(H)))
        return 0;
    ff_bluray_header(H, &pl->hdr);
    if (pl->hdr.type != MKBETAG('P', 'L', 'S', 'T'))
        return 0;
    memcpy(pl->bdav_hdr, H, sizeof(pl->bdav_hdr));
    pl->name_len = H[0x58];
    memcpy(pl->name, H + 0x59, 0xff);
    P = AV_RB32(H + 8);
    if (!ff_bluray_read(f, P, T, 10))
        return 0;
    pl->bdav_type = T[5] & 0xf;
    n   = AV_RB16(T + 6);
    end = P + 4 + AV_RB32(T);
    if (n > MAX_ITEMS) {
        av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: %d PlayItems (at most %d)\n", f->path, n, MAX_ITEMS);
        return 0;
    }
    if (n && !(pl->items = av_calloc(n, sizeof(*pl->items))))
        return AVERROR(ENOMEM);
    pos = P + 10;
    for (int i = 0; i < n; i++) {
        BlurayPlayItem *it = &pl->items[i];
        uint32_t next;

        if (!ff_bluray_read(f, pos, l, 2))
            return 0;
        next = pos + 2 + AV_RB16(l);
        if (next > end)
            return 0;
        if (!ff_bluray_read(f, pos, b, 0x16)) {
            av_log(log, AV_LOG_DEBUG, "Blu-ray: %s: PlayItem %d cannot be read\n", f->path, i);
            return 0;
        }
        memcpy(it->clip, b + 2, 5);
        memcpy(it->codec, b + 7, 4);
        it->pos        = pos + 7;
        it->connection = b[0xc] & 0xf;
        it->stc_id     = pl->bdav_type == 1 ? b[0xd] : 0xff;
        it->in_time    = AV_RB32(b + 0xe);
        it->out_time   = AV_RB32(b + 0x12);
        pl->nb_items   = i + 1;
        pos = next;
    }
    return 1;
}
