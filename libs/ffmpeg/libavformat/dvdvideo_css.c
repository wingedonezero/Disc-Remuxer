/*
 * DVD-Video demuxer: CSS sector rules. Which sectors are scrambled, and
 * whether a descrambled sector holds well-formed content (to confirm that a
 * title key is right).
 *
 * A sector is a 2048-byte MPEG-2 program stream pack (DVD-Video, ISO/IEC
 * 13818-1). CSS scrambles bytes 0x80-0x7ff; bytes 0x00-0x7f stay clear.
 *
 * Content check: a descrambled sector holds either an AC-3 frame (private
 * stream 1, substreams 0x80-0x87) whose header lies in the clear bytes,
 * followed exactly one frame later by a frame with the same header bytes, and
 * whose two CRCs check (ATSC A/52); or MPEG-2 video (stream 0xE0) whose
 * payload starts with a sequence header loading both quantiser matrices, the
 * non-intra matrix (in the scrambled part) rising the way real matrices do,
 * followed by a sequence extension and another start code (ISO/IEC 13818-2).
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

#include <stdint.h>
#include <string.h>

#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"

#include "dvdvideo_internal.h"

#define SECTOR      DVDVIDEO_BLOCK_SIZE
#define CLEAR       0x80    /* bytes of a sector that are never scrambled */

/* ---- packs and PES headers ---- */

/* Length of the pack header at the start of sec, 0 when sec does not start
 * with an MPEG-2 pack header. The stuffing length counts only when the first
 * stuffing byte is not zero. */
static int pack_header_len(const uint8_t *sec)
{
    int stuffing;

    if (AV_RB32(sec) != 0x000001BA || (sec[4] & 0xc0) != 0x40)
        return 0;
    stuffing = sec[13] & 7;
    return stuffing && sec[14] ? 14 + stuffing : 14;
}

int ff_dvdvideo_css_scrambled_pes(const uint8_t *sec)
{
    int h = pack_header_len(sec), id;

    if (!h || AV_RB24(sec + h) != 0x000001)
        return -1;
    id = sec[h + 3];
    if (id != 0xBD && id != 0xE0 && (id & 0xf8) != 0xC0)
        return 0;
    return sec[h + 6] & 0x30 ? h : 0;
}

/* The first PES packet of a sector, as far as the content check reads it. */
typedef struct Packet {
    uint32_t offset;        /* of the packet in the sector */
    uint32_t packet_len;    /* PES_packet_length + 6, 0 when the field is 0 */
    int      key;           /* stream_id; private stream 1: substream << 8 | 0xBD;
                               with a stream_id_extension: extension << 8 | stream_id */
    uint32_t payload_off;   /* in the packet, after a substream header */
    int      first_au;      /* first access unit in the payload: -1 none, -2 subpicture */
} Packet;

/* stream_ids whose header is read in full: video 0xE0-0xE2 and 0xFD, private
 * stream 1, MPEG audio 0xC0-0xDF */
static int has_timed_header(int sid)
{
    return (sid >= 0xE0 && sid <= 0xE2) || sid == 0xFD || sid == 0xBD || (sid & 0xe0) == 0xc0;
}

/* The PES header at pkt (len bytes available): 0, or -1 when there is none or
 * it is cut short. */
static int parse_pes(const uint8_t *pkt, int len, Packet *p)
{
    int sid, hdl, flags;

    if (len < 6 || AV_RB24(pkt) != 0x000001)
        return -1;
    sid           = pkt[3];
    p->packet_len = AV_RB16(pkt + 4) ? AV_RB16(pkt + 4) + 6 : 0;
    p->key        = sid;
    if ((sid & 0xfe) == 0xbe) {
        p->payload_off = 6;
        return 0;
    }
    if (len < 9)
        return -1;
    hdl = pkt[8];
    if (has_timed_header(sid)) {
        flags = pkt[7];
        if ((flags >= 0xc0 || (flags & 0xc0) == 0x80) && len < 0xe)
            return -1;   /* a PTS cut short */
        if ((flags & 1) && !(flags & 0x3e)) {
            /* the PES extension after the PTS / DTS fields */
            int e_at = 9 + ((flags & 0x40) >> 6) * 5 + (flags >> 7) * 5, e;

            if (len < e_at + 1)
                return -1;
            e = pkt[e_at];
            if (e < 0x40 && (e & 1)) {
                int q = e & 0x10 ? e_at + 3 : e_at + 1;

                if (len < q + 2)
                    return -1;
                if (pkt[q] == 0x81 && pkt[q + 1] < 0x80)
                    p->key = pkt[q + 1] << 8 | sid;
            }
        }
        if (len < hdl + 9)
            return -1;
    }
    p->payload_off = hdl + 9;
    return 0;
}

/* parse_pes(), then the DVD-Video substream header of private stream 1:
 * subpicture 0x20-0x3F (1 byte, first access unit -2); AC-3 0x80-0x87, DTS
 * 0x88-0x8F, LPCM 0xA0-0xA7 and 0xC0-0xC7 (4 bytes: id, number of frame
 * headers, first access unit pointer, which counts only when frames start
 * here, except for DTS); 0xB0-0xB7 (5 bytes, a pointer at +1 minus 3); any
 * other id a 1-byte header. */
static int parse_pes_substream(const uint8_t *pkt, int len, Packet *p)
{
    int off, rem, b, next, fau = -1;

    if (parse_pes(pkt, len, p) < 0)
        return -1;
    if (p->key != 0xbd) {
        p->first_au = -1;
        return 0;
    }
    off  = p->payload_off;
    rem  = len - off;
    if (rem < 2)
        return -1;
    b    = pkt[off];
    next = off + 1;
    switch (b & 0xf8) {
    case 0x20: case 0x28: case 0x30: case 0x38:
        fau = -2;
        break;
    case 0x80: case 0x88: case 0xa0: case 0xc0: {
        int ptr, f;

        if (rem < 5)
            return -1;
        ptr = AV_RB16(pkt + off + 2);
        f   = ptr ? ptr - 1 : -1;
        fau = pkt[off + 1] ? f : -1;
        if ((b & 0xf8) == 0x88)
            fau = f;
        next = off + 4;
        break;
    }
    case 0xb0: {
        int v;

        if (rem < 6)
            return -1;
        v    = AV_RB16(pkt + off + 1);
        fau  = v >= 3 ? v - 3 : -1;
        next = off + 5;
        break;
    }
    }
    p->first_au    = fau;
    p->key         = b << 8 | 0xbd;
    p->payload_off = next;
    return 0;
}

/* The first packet of a sector: its pack header, then its PES header. LPCM
 * audio headers are not taken off (the content check never reads LPCM). */
static int first_packet(const uint8_t *sec, Packet *p)
{
    int h = pack_header_len(sec);

    if (!h || parse_pes_substream(sec + h, SECTOR - h, p) < 0)
        return -1;
    p->offset = h;
    return 0;
}

/* ---- AC-3 ---- */

/* frame sizes in 16-bit words per frmsizecod and fscod (A/52 table 5.18) */
static const uint16_t ac3_frame_words[38][3] = {
    {  64,   69,   96 }, {  64,   70,   96 }, {  80,   87,  120 }, {  80,   88,  120 },
    {  96,  104,  144 }, {  96,  105,  144 }, { 112,  121,  168 }, { 112,  122,  168 },
    { 128,  139,  192 }, { 128,  140,  192 }, { 160,  174,  240 }, { 160,  175,  240 },
    { 192,  208,  288 }, { 192,  209,  288 }, { 224,  243,  336 }, { 224,  244,  336 },
    { 256,  278,  384 }, { 256,  279,  384 }, { 320,  348,  480 }, { 320,  349,  480 },
    { 384,  417,  576 }, { 384,  418,  576 }, { 448,  487,  672 }, { 448,  488,  672 },
    { 512,  557,  768 }, { 512,  558,  768 }, { 640,  696,  960 }, { 640,  697,  960 },
    { 768,  835, 1152 }, { 768,  836, 1152 }, { 896,  975, 1344 }, { 896,  976, 1344 },
    {1024, 1114, 1536 }, {1024, 1115, 1536 }, {1152, 1253, 1728 }, {1152, 1254, 1728 },
    {1280, 1393, 1920 }, {1280, 1394, 1920 },
};

/* The size of the AC-3 frame whose header starts at buf (7 bytes), 0 when
 * it is not an AC-3 header (bsid above 8, reserved codes). */
static uint32_t ac3_frame_size(const uint8_t *buf)
{
    int fscod = buf[4] >> 6, frmsizecod = buf[4] & 0x3f;

    if ((buf[5] >> 3) > 8 || fscod == 3 || frmsizecod >= 38)
        return 0;
    return ac3_frame_words[frmsizecod][fscod] * 2;
}

static int crc16_is_zero(const uint8_t *data, size_t len)
{
    return !av_crc(av_crc_get_table(AV_CRC_16_ANSI), 0, data, len);
}

/* Whether both CRCs of the AC-3 frame at the start of frame (len bytes) check. */
static int ac3_frame_crc_ok(const uint8_t *frame, size_t len)
{
    uint32_t fs, b, rest;

    if (len < 7 || !(fs = ac3_frame_size(frame)) || fs > len)
        return 0;
    b = (fs * 5) >> 3;
    if (b < 2 || !crc16_is_zero(frame + 2, b - 2))
        return 0;
    rest = (fs * 3) >> 3;
    return crc16_is_zero(frame + b, rest);
}

/* MSB-first bit reader keeping more than 24 bits cached; bits read past the
 * end of its data count as zeros, and are counted. */
typedef struct Bits {
    const uint8_t *data;
    int            len, pos;
    uint64_t       cache;
    unsigned       cnt, overrun;
} Bits;

static void bits_refill(Bits *b)
{
    while (b->cnt < 25) {
        uint8_t byte = 0;

        if (b->pos < b->len)
            byte = b->data[b->pos++];
        else
            b->overrun += 8;
        b->cache = b->cache << 8 | byte;
        b->cnt  += 8;
    }
}

static unsigned bits_read(Bits *b, unsigned n)
{
    unsigned v = (b->cache >> (b->cnt - n)) & ((1u << n) - 1);

    b->cnt -= n;
    bits_refill(b);
    return v;
}

static void bits_skip(Bits *b, unsigned n)
{
    b->cnt -= n;
    bits_refill(b);
}

/* no bit read so far came from past the end of the data */
static int bits_in_data(const Bits *b)
{
    return b->overrun <= b->cnt;
}

/* The AC-3 frame of a sector as far as the clear bytes tell. */
typedef struct AC3Head {
    uint32_t au;            /* offset of the frame in the sector */
    uint32_t avail;         /* bytes of the frame inside the packet */
    uint32_t frame_size;
    uint32_t bits;          /* header bits from byte 5 up to dialnorm */
} AC3Head;

static int ac3_head(const uint8_t *sec, AC3Head *h)
{
    Packet p;
    uint32_t start, clear, payload, fs;
    unsigned bsid, acmod;
    Bits b = { 0 };
    int fau;

    if (first_packet(sec, &p) < 0 || ((p.key & 0xf8ff) | 0x800) != 0x88bd)
        return 0;
    start = p.offset + p.payload_off;
    if (start >= CLEAR)
        return 0;
    fau     = p.first_au;
    clear   = CLEAR - start;
    payload = p.packet_len - p.payload_off;   /* wraps like the unsigned rule it follows */
    if (fau < 0 || (int32_t)clear <= fau || payload <= clear || (p.key & 0xf8ff) != 0x80bd)
        return 0;
    clear -= fau;
    if (clear < 7)
        return 0;
    h->au = start + fau;
    if (!(fs = ac3_frame_size(sec + h->au)))
        return 0;
    h->avail = payload - fau;
    b.data   = sec + h->au + 5;
    b.len    = clear - 5;
    bits_refill(&b);
    bsid = bits_read(&b, 5);
    if (!bits_in_data(&b) || (bsid != 8 && bsid != 6))
        return 0;
    bits_skip(&b, 3);              /* bsmod */
    acmod   = bits_read(&b, 3);
    h->bits = 11;
    if ((acmod & 1) && acmod != 1) {
        bits_skip(&b, 2);          /* cmixlev */
        h->bits = 13;
    }
    if (acmod >= 4 || acmod == 2) {
        bits_skip(&b, 2);          /* surmixlev / dsurmod */
        h->bits += 2;
    }
    bits_skip(&b, 6);              /* lfeon, dialnorm */
    if (!bits_in_data(&b) || clear <= 7 || fs + 8 > h->avail)
        return 0;
    h->frame_size = fs;
    return 1;
}

/* Whether the AC-3 frame h is followed by a matching frame and its CRCs check. */
static int ac3_frames_valid(const uint8_t *sec, const AC3Head *h)
{
    uint32_t au = h->au, fs = h->frame_size, end;
    unsigned nb = (h->bits + 6) & 7;

#define AT(i) ((uint64_t)au + (i) < SECTOR ? (int)sec[au + (i)] : -1)
    if (AT(fs) != 0x0b || AT(fs + 1) != 0x77 || AT(fs + 4) != AT(4) || AT(fs + 5) != AT(5) ||
        AT(fs + 6) != AT(6))
        return 0;
    if (nb) {
        int x = AT(fs + 7), y = AT(7);

        if (x < 0 || y < 0 || ((x ^ y) >> (8 - nb)))
            return 0;
    }
#undef AT
    end = FFMIN(au + h->avail, SECTOR);
    return ac3_frame_crc_ok(sec + au, end - au);
}

/* ---- MPEG-2 video ---- */

/* Offset just past a sequence header at the start of the payload that loads
 * both quantiser matrices (the non-intra matrix starts there), read from the
 * clear bytes; 0 otherwise. */
static uint32_t video_probe(const uint8_t *sec)
{
    Packet p;
    uint32_t start, s, base;

    if (first_packet(sec, &p) < 0)
        return 0;
    start = p.payload_off + p.offset;
    if (p.key != 0xe0 || start > 0x7f || start - 0x36 < 0x4b)
        return 0;
    s = start;
    if (sec[s] || sec[s + 1])
        return 0;
    base = s - 1;
    if (sec[s + 2] == 0) {
        for (;;) {
            int c;

            if (base + 4 >= SECTOR)
                return 0;
            c = sec[base + 4];
            base++;
            if (c) {
                if (c != 1)
                    return 0;
                break;
            }
        }
    } else if (sec[s + 2] != 1) {
        return 0;
    }
    /* the start code must lie in the first 0x78 bytes (tested before the byte
     * after it is read: a zero run can reach the end of the sector) */
    if (base - 0x74 < 0xffffff7fu || sec[base + 4] != 0xb3)
        return 0;
    if ((sec[base + 0xc] & 2) && base + 0x4d < 0x80 && (sec[base + 0x4c] & 1))
        return base + 0x4d;
    return 0;
}

/* Bytes from v to the end of the packet when the video test applies, else 0. */
static uint32_t video_room(const uint8_t *sec, uint32_t v)
{
    Packet p;
    uint32_t room;

    if (first_packet(sec, &p) < 0)
        return 0;
    room = p.packet_len + p.offset - v;
    return (int32_t)room > 0 && room > 0x4b ? room : 0;
}

/* The rising non-intra matrix, the sequence extension and the next start code
 * after offset v. */
static int video_tail_valid(const uint8_t *sec, uint32_t v, uint32_t room)
{
    uint8_t col[8];
    uint32_t i, u;

#define AT(k) ((uint64_t)v + (k) < SECTOR ? (int)sec[v + (k)] : -1)
    for (int k = 0; k < 8; k++) {
        if (AT(k * 8) < 0)
            return 0;
        col[k] = AT(k * 8);
    }
    if (col[1] < col[0] ? col[0] - col[1] >= 0x10 : (uint8_t)(col[1] - col[0]) >= 0x80)
        return 0;
    for (int k = 2; k < 8; k++)
        if (col[k] < col[k - 1] || (uint8_t)(col[k] - col[k - 1]) >= 0x80)
            return 0;
    if (AT(0x40) != 0 || AT(0x41) != 0)
        return 0;
    i = 0x3f;
    if (AT(0x42) == 0) {
        for (;;) {
            int c;

            u = i + 4;
            i++;
            c = AT(u);
            if (c == 1)
                break;
            if (c != 0)
                return 0;
        }
    } else if (AT(0x42) != 1) {
        return 0;
    }
    if (AT(i + 4) != 0xb5 || room <= i + 7 || AT(i + 5) < 0 || (AT(i + 5) & 0xf0) != 0x10)
        return 0;
    u = i + 0xf;
    if (room < u)
        return 0;
    for (;;) {
        int c;

        if (AT(u - 4) != 0 || AT(u - 3) != 0)
            return 0;
        c = AT(u - 2);
        u++;
        if (c == 1)
            return u <= room;
        if (c != 0)
            return 0;
    }
#undef AT
}

/* ---- the checks ---- */

int ff_dvdvideo_css_content_valid(const uint8_t *sec)
{
    AC3Head h;
    Packet p;
    uint32_t v, room;

    if (ac3_head(sec, &h))
        return ac3_frames_valid(sec, &h);
    if (first_packet(sec, &p) < 0 || ((p.key & 0xf8ff) | 0x800) == 0x88bd)
        return 0;
    if (!(v = video_probe(sec)) || !(room = video_room(sec, v)))
        return 0;
    return video_tail_valid(sec, v, room);
}

int ff_dvdvideo_css_can_test(const uint8_t *sec)
{
    AC3Head h;
    uint32_t v;

    return ac3_head(sec, &h) || ((v = video_probe(sec)) && video_room(sec, v));
}
