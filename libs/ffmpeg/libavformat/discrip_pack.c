/*
 * Disc packs (DVD-Video VOBs, HD DVD EVOBs): one 2048-byte sector holds an
 * MPEG-2 program stream pack (ISO/IEC 13818-1): a pack header and its PES
 * packets. The packets of a sector are read with the rules of the
 * reference's disc reader; the stream probe of the HD DVD demuxer and the
 * program stream demuxer of the disc formats (discrip_ps.c) both read packs
 * here.
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

#include "libavutil/intreadwrite.h"

#include "discrip.h"

#define PACK DR_PACK_SIZE

/* PES header. Packets of other streams than video, private stream 1 and
 * MPEG audio are not checked against the bytes available. A PTS is present
 * when PTS_DTS_flags say so; it is valid only when its three marker bits
 * are set. */
static int parse_pes(DRPesPacket *p, const uint8_t *b, int avail)
{
    int sid, f;

    if (avail < 6 || b[0] || b[1] || b[2] != 1)
        return 0;
    sid = b[3];
    if ((sid & 0xfe) != 0xbe && avail < 9)
        return 0;
    p->len         = AV_RB16(b + 4) ? AV_RB16(b + 4) + 6 : 0;
    p->key         = sid;
    p->pts_present = 0;
    p->pts_valid   = 0;
    p->pts         = 0;
    p->first_au    = -1;
    p->extra       = 0;
    if ((sid & 0xfe) == 0xbe) {     /* padding, private stream 2 */
        p->payload = 6;
        return 1;
    }
    if (sid == 0xe0 || sid == 0xe1 || sid == 0xe2 || sid == 0xfd || sid == 0xbd || (sid & 0xe0) == 0xc0) {
        f = b[7];
        if ((f & 0xc0) == 0xc0 || (f & 0xc0) == 0x80) {
            if (avail < 14)
                return 0;
            p->pts_present = 1;
            if ((b[9] & 1) && (b[11] & 1) && (b[13] & 1)) {
                p->pts_valid = 1;
                p->pts = (uint64_t)(b[9] & 0x0e) << 29 | b[10] << 22 | (b[11] & 0xfe) << 14 |
                         b[12] << 7 | b[13] >> 1;
            }
        }
        if ((f & 1) && !(f & 0x3e)) {   /* PES extension, no other optional field */
            int e = 9 + 5 * ((f >> 6) & 1) + 5 * (f >> 7), q;

            if (avail < e + 1)
                return 0;
            if (b[e] < 0x40) {
                q = e + ((b[e] & 0x10) ? 3 : 1);
                if (b[e] & 1) {
                    if (avail < q + 2)
                        return 0;
                    if (b[q] == 0x81 && b[q + 1] < 0x80)
                        p->key = b[q + 1] << 8 | sid;
                }
            }
        }
        if (avail < b[8] + 9)
            return 0;
    }
    p->payload = b[8] + 9;
    return 1;
}

/* PES header and, for private stream 1, the sub-stream header (DVD layout:
 * audio 4 bytes {id, frame count, first access unit pointer}, MLP 5 bytes,
 * sub-pictures 1 byte). lpcm5: the LPCM audio header is 5 bytes, not 3. */
static int parse_packet(DRPesPacket *p, const uint8_t *b, int avail, int lpcm5)
{
    int off, b0, cls;

    if (!parse_pes(p, b, avail))
        return 0;
    if (p->key != 0xbd)
        return 1;
    off = p->payload;
    if (avail - off < 2)
        return 0;
    b0  = b[off];
    cls = b0 & 0xf8;
    if (cls >= 0x20 && cls <= 0x38) {
        p->first_au = -2;
        off += 1;
    } else if (cls == 0x80 || cls == 0x88 || cls == 0xc0 || cls == 0xa0) {
        int nf, ptr, f;

        if (cls == 0xa0)
            p->extra = lpcm5 ? 5 : 3;
        if (avail - off < 5)
            return 0;
        nf  = b[off + 1];
        ptr = AV_RB16(b + off + 2);
        f   = ptr ? ptr - 1 : -1;
        p->first_au = (nf || cls == 0x88) ? f : -1;
        off += 4;
    } else if (cls == 0xb0) {
        int v;

        if (avail - off < 6)
            return 0;
        v = AV_RB16(b + off + 1);
        p->first_au = v >= 3 ? v - 3 : -1;
        off += 5;
    } else {
        off += 1;
    }
    p->key     = b0 << 8 | 0xbd;
    p->payload = off;
    return 1;
}

/* Pack header: its length (14 + stuffing; the stuffing only counts when its
 * first byte is not 0). */
static int pack_header_len(const uint8_t *s)
{
    if (AV_RB32(s) != 0x1ba || (s[4] & 0xc0) != 0x40)
        return 0;
    return (s[13] & 7) && s[14] ? 14 + (s[13] & 7) : 14;
}

int ff_discrip_pack_packet(const uint8_t *s, int *off, DRPesPacket *p, int lpcm5)
{
    int at = *off, pos, lim, n = 0;

    p->why = "not a valid pack";
    if (!at && !(at = pack_header_len(s)))
        return 0;
    if (!parse_packet(p, s + at, PACK - at, lpcm5))
        return 0;
    p->at = at;
    if (p->extra) {
        int d = p->len - p->payload;
        if (d == p->extra) {        /* nothing but the LPCM header: like padding */
            p->extra = 0;
            p->key   = 0xbe;
        } else if (d < p->extra) {
            p->why = "an LPCM packet is shorter than its audio frame header";
            return 0;
        } else {
            if (p->first_au >= 0 && p->first_au < p->extra) {
                p->why = "an LPCM packet's first audio frame starts inside its audio frame header";
                return 0;
            }
            if (p->first_au >= 0)
                p->first_au -= p->extra;
            p->payload += p->extra;
        }
    }
    if (at >= 0x7f7 && !p->len && p->key == 0xbe) {
        *off = PACK;
        return 1;
    }
    pos = at + p->len;
    lim = p->key == 0xbf ? PACK : pos;
    if (lim >= PACK) {
        *off = lim;
        return 1;
    }
    for (;;) {
        DRPesPacket pad;
        int k;

        for (k = pos + 1; k < PACK && !s[k]; k++)
            ;
        if (!s[pos] && k == PACK)
            break;
        for (k = pos; k < PACK && s[k] == 0xff; k++)
            ;
        if (k == PACK || pos > 0x7f9)
            break;
        if (AV_RB32(s + pos) != 0x1be)
            goto next;
        if (!parse_pes(&pad, s + pos, PACK - pos))
            goto next;
        if (pos >= 0x7f7 && !pad.len) {
            if (pad.key == 0xbe)
                pos = PACK;
            goto next;
        }
        if (!pad.len)
            goto next;
        pos += pad.len;
        if (pos > PACK)
            break;
        n++;
        if (pos == PACK)
            break;
    }
    *off = PACK;
    return 1;
next:
    if (n && pos >= 0x601 && pos <= 0x7ff) {   /* after padding near the end: is it a packet? */
        DRPesPacket t;
        int q = pos;
        if (!parse_packet(&t, s + pos, PACK - pos, 0) || !ff_discrip_pack_packet(s, &q, &t, lpcm5))
            pos = PACK;
    }
    *off = pos;
    return 1;
}

int ff_discrip_pack_sector(const uint8_t *sec, int lpcm5, DRPackPacketCb cb, void *opaque, const char **why,
                           int *at)
{
    DRPesPacket k;
    int off = 0, count = 1, mpa, ret;

    *why = NULL;
    *at  = 0;
    if (!sec[4]) {
        int all = sec[0] == 0x00 || sec[0] == 0xff;
        for (int i = 1; all && i < PACK; i++)
            all = sec[i] == sec[0];
        if (all)
            return DR_SECTOR_EMPTY;
    }
    if (!ff_discrip_pack_packet(sec, &off, &k, lpcm5)) {
        *why = k.why;
        return DR_SECTOR_FAIL;
    }
    if (off > PACK) {
        *at = k.at;
        if (k.payload + k.at > 0x3f) {
            *why = "its first packet runs past the sector's end";
            return DR_SECTOR_FAIL;
        }
        *why = "its first packet runs past the sector's end";
        return DR_SECTOR_BAD;
    }
    mpa = (k.key & 0xfff0) == 0xd0;
    if ((ret = cb(opaque, sec, &k)) < 0)
        return ret;
    while (off != PACK) {
        int rem = PACK - off;

        if (!mpa && count >= 2) {
            *at  = off;
            *why = "it holds more than two packets";
            return DR_SECTOR_FAIL;
        }
        if ((rem < 6 || AV_RB24(sec + off) != 1 || ((sec[off + 3] & 0xfe) != 0xbe && rem < 9)) &&
            off >= 1 && off <= 0x7fc && (k.key == 0xe0 || (k.key & 0xff) == 0xbd)) {
            *at  = off;
            *why = "no packet starts after its video / private stream 1 packet";
            return DR_SECTOR_REST_DROPPED;
        }
        if (!ff_discrip_pack_packet(sec, &off, &k, lpcm5)) {
            *at  = off;
            *why = k.why;
            return DR_SECTOR_FAIL;
        }
        mpa |= (k.key & 0xfff0) == 0xd0;
        if ((ret = cb(opaque, sec, &k)) < 0)
            return ret;
        count++;
    }
    return DR_SECTOR_OK;
}
