/*
 * Blu-ray: BDAV (recordable disc) playlists (BDAV/PLAYLIST/NNNNN.rpls)
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

#include "base/bytes.h"
#include "base/vlog.h"
#include "base/mem.h"

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
    if (pl->hdr.type != DR_MKBETAG('P', 'L', 'S', 'T'))
        return 0;
    memcpy(pl->bdav_hdr, H, sizeof(pl->bdav_hdr));
    pl->name_len = H[0x58];
    memcpy(pl->name, H + 0x59, 0xff);
    P = dr_rb32(H + 8);
    if (!ff_bluray_read(f, P, T, 10))
        return 0;
    pl->bdav_type = T[5] & 0xf;
    n   = dr_rb16(T + 6);
    end = P + 4 + dr_rb32(T);
    if (n > MAX_ITEMS) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: %d PlayItems (at most %d)\n", f->path, n, MAX_ITEMS);
        return 0;
    }
    if (n && !(pl->items = dr_calloc(n, sizeof(*pl->items))))
        return DR_ERR(ENOMEM);
    pos = P + 10;
    for (int i = 0; i < n; i++) {
        BlurayPlayItem *it = &pl->items[i];
        uint32_t next;

        if (!ff_bluray_read(f, pos, l, 2))
            return 0;
        next = pos + 2 + dr_rb16(l);
        if (next > end)
            return 0;
        if (!ff_bluray_read(f, pos, b, 0x16)) {
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: PlayItem %d cannot be read\n", f->path, i);
            return 0;
        }
        memcpy(it->clip, b + 2, 5);
        memcpy(it->codec, b + 7, 4);
        it->pos        = pos + 7;
        it->connection = b[0xc] & 0xf;
        it->stc_id     = pl->bdav_type == 1 ? b[0xd] : 0xff;
        it->in_time    = dr_rb32(b + 0xe);
        it->out_time   = dr_rb32(b + 0x12);
        pl->nb_items   = i + 1;
        pos = next;
    }
    return 1;
}
