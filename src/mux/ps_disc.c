/*
 * MPEG-2 program stream packs as a disc reader reads them: one 2048-byte
 * sector = one pack (DVD-Video VOBs, HD DVD EVOBs), its packets with the
 * reference disc reader's rules (see ps_disc.h; the rules in short: an
 * empty sector has none; a pack holds at most two packets unless one is
 * MPEG audio; bytes that are no packet start after a video or private
 * stream 1 packet drop the rest of the sector; a first packet running past
 * the sector leaves the sector out or fails; a PTS whose marker bits are
 * wrong is no time; an LPCM packet shorter than its audio frame header, or
 * whose first access unit starts inside it, fails).
 *
 * Once a disc mode of ours in FFmpeg's mpeg.c; run by the clip demuxer
 * (src/disc/clip_demux.c). Tests: tests/unit/test_dvd_pack.py,
 * test_mpeg_disc.py.
 */

#include <stddef.h>
#include <stdint.h>

#include "base/bytes.h"

#include "mux/ps_disc.h"

/* PES header. Packets of other streams than video, private stream 1 and
 * MPEG audio are not checked against the bytes available. A PTS is present
 * when PTS_DTS_flags say so; it is valid only when its three marker bits
 * are set. */
static int disc_parse_pes(MpegPsDiscPacket *p, const uint8_t *b, int avail)
{
    int sid, f;

    if (avail < 6 || b[0] || b[1] || b[2] != 1)
        return 0;
    sid = b[3];
    if ((sid & 0xfe) != 0xbe && avail < 9)
        return 0;
    p->len         = dr_rb16(b + 4) ? dr_rb16(b + 4) + 6 : 0;
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
static int disc_parse_packet(MpegPsDiscPacket *p, const uint8_t *b, int avail, int lpcm5)
{
    int off, b0, cls;

    if (!disc_parse_pes(p, b, avail))
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
        ptr = dr_rb16(b + off + 2);
        f   = ptr ? ptr - 1 : -1;
        p->first_au = (nf || cls == 0x88) ? f : -1;
        off += 4;
    } else if (cls == 0xb0) {
        int v;

        if (avail - off < 6)
            return 0;
        v = dr_rb16(b + off + 1);
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
static int disc_pack_header_len(const uint8_t *s)
{
    if (dr_rb32(s) != 0x1ba || (s[4] & 0xc0) != 0x40)
        return 0;
    return (s[13] & 7) && s[14] ? 14 + (s[13] & 7) : 14;
}

int ff_mpegps_disc_packet(const uint8_t *s, int *off, MpegPsDiscPacket *p, int lpcm5)
{
    int at = *off, pos, lim, n = 0;

    p->why = "not a valid pack";
    if (!at && !(at = disc_pack_header_len(s)))
        return 0;
    if (!disc_parse_packet(p, s + at, MPEGPS_DISC_SECTOR - at, lpcm5))
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
        *off = MPEGPS_DISC_SECTOR;
        return 1;
    }
    pos = at + p->len;
    lim = p->key == 0xbf ? MPEGPS_DISC_SECTOR : pos;
    if (lim >= MPEGPS_DISC_SECTOR) {
        *off = lim;
        return 1;
    }
    for (;;) {
        MpegPsDiscPacket pad;
        int k;

        for (k = pos + 1; k < MPEGPS_DISC_SECTOR && !s[k]; k++)
            ;
        if (!s[pos] && k == MPEGPS_DISC_SECTOR)
            break;
        for (k = pos; k < MPEGPS_DISC_SECTOR && s[k] == 0xff; k++)
            ;
        if (k == MPEGPS_DISC_SECTOR || pos > 0x7f9)
            break;
        if (dr_rb32(s + pos) != 0x1be)
            goto next;
        if (!disc_parse_pes(&pad, s + pos, MPEGPS_DISC_SECTOR - pos))
            goto next;
        if (pos >= 0x7f7 && !pad.len) {
            if (pad.key == 0xbe)
                pos = MPEGPS_DISC_SECTOR;
            goto next;
        }
        if (!pad.len)
            goto next;
        pos += pad.len;
        if (pos > MPEGPS_DISC_SECTOR)
            break;
        n++;
        if (pos == MPEGPS_DISC_SECTOR)
            break;
    }
    *off = MPEGPS_DISC_SECTOR;
    return 1;
next:
    if (n && pos >= 0x601 && pos <= 0x7ff) {   /* after padding near the end: is it a packet? */
        MpegPsDiscPacket t;
        int q = pos;
        if (!disc_parse_packet(&t, s + pos, MPEGPS_DISC_SECTOR - pos, 0) || !ff_mpegps_disc_packet(s, &q, &t, lpcm5))
            pos = MPEGPS_DISC_SECTOR;
    }
    *off = pos;
    return 1;
}

int ff_mpegps_disc_sector(const uint8_t *sec, int lpcm5, MpegPsDiscPacketCb cb, void *opaque, const char **why,
                           int *at)
{
    MpegPsDiscPacket k;
    int off = 0, count = 1, mpa, ret;

    *why = NULL;
    *at  = 0;
    if (!sec[4]) {
        int all = sec[0] == 0x00 || sec[0] == 0xff;
        for (int i = 1; all && i < MPEGPS_DISC_SECTOR; i++)
            all = sec[i] == sec[0];
        if (all)
            return MPEGPS_DISC_EMPTY;
    }
    if (!ff_mpegps_disc_packet(sec, &off, &k, lpcm5)) {
        *why = k.why;
        return MPEGPS_DISC_FAIL;
    }
    if (off > MPEGPS_DISC_SECTOR) {
        *at = k.at;
        if (k.payload + k.at > 0x3f) {
            *why = "its first packet runs past the sector's end";
            return MPEGPS_DISC_FAIL;
        }
        *why = "its first packet runs past the sector's end";
        return MPEGPS_DISC_BAD;
    }
    mpa = (k.key & 0xfff0) == 0xd0;
    if ((ret = cb(opaque, sec, &k)) < 0)
        return ret;
    while (off != MPEGPS_DISC_SECTOR) {
        int rem = MPEGPS_DISC_SECTOR - off;

        if (!mpa && count >= 2) {
            *at  = off;
            *why = "it holds more than two packets";
            return MPEGPS_DISC_FAIL;
        }
        if ((rem < 6 || dr_rb24(sec + off) != 1 || ((sec[off + 3] & 0xfe) != 0xbe && rem < 9)) &&
            off >= 1 && off <= 0x7fc && (k.key == 0xe0 || (k.key & 0xff) == 0xbd)) {
            *at  = off;
            *why = "no packet starts after its video / private stream 1 packet";
            return MPEGPS_DISC_REST_DROPPED;
        }
        if (!ff_mpegps_disc_packet(sec, &off, &k, lpcm5)) {
            *at  = off;
            *why = k.why;
            return MPEGPS_DISC_FAIL;
        }
        mpa |= (k.key & 0xfff0) == 0xd0;
        if ((ret = cb(opaque, sec, &k)) < 0)
            return ret;
        count++;
    }
    return MPEGPS_DISC_OK;
}

