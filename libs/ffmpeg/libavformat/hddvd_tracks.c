/*
 * HD DVD (Advanced Content): the tracks of a title. They come from the
 * attribute record of one EVOB of the title (the one with the most bytes,
 * the first of them on a tie): a video stream, up to 8 audio streams and up
 * to 32 sub-picture streams. Their languages come from the playlists; a
 * Dolby Digital Plus stream is probed (its first frames) to tell E-AC-3 from
 * plain AC-3.
 *
 * Attribute record (big-endian; offsets from the record's start): byte 0x02
 * video attribute (bits 7..5 coding: 0 / 1 MPEG-2, 2 AVC, 3 VC-1, bit 7 set
 * = none; byte 0x02 bits 4 and 2 and byte 0x04 bits 7..4 give the frame
 * size), 0x0e u16 audio stream count with 4-byte entries from 0x10 (byte 0
 * below 0x20: bits 4..2 = coding: AC-3, MLP, MPEG audio, (not known), LPCM,
 * LPCM, DTS, Dolby Digital Plus; 0x20 and up: none), 0xe4 u16 sub-picture stream
 * count with 5-byte entries from 0xe6 (byte 0 bits 7..5 = coding, bytes
 * 1..4 one stream number per display mode with bit 5 = present), 0x186 32
 * palette entries (two palettes of 16).
 *
 * The probe reads the EVOB's stream as MPEG-2 program stream packs from its
 * first block: the stream needs a PES packet with a PTS as the first packet
 * of one of its first 2000 packs; its first frame must be a valid header;
 * 4 frames all E-AC-3 -> E-AC-3, all AC-3 -> AC-3, anything else (also fewer
 * than 4 frames) -> the stream is not supported and left out. A probe that
 * fails leaves the whole title out.
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
#include <stdlib.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/crc.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "libavcodec/ac3_parser_internal.h"
#include "libavcodec/defs.h"
#include "libavcodec/dca_syncwords.h"
#include "libavcodec/get_bits.h"

#include "disclang.h"
#include "hddvd_internal.h"

#define PACK            2048
#define PTS_SCAN_PACKS  2000    /* packs searched for the stream's first PTS */
#define PROBE_FRAMES    4
#define RESYNC_GIVE_UP  0xa001  /* bytes without a frame start after which the data is dropped */

/* ---- one pack of an EVOB: MPEG-2 program stream (ISO/IEC 13818-1) ---- */

typedef struct Pkt {
    int      len;                               /**< PES_packet_length + 6; 0 when the field is 0 */
    int      key;           /**< stream_id; private stream 1: sub-stream id << 8 | 0xbd;
                                 with a PES extension 2: stream_id_extension << 8 | stream_id */
    int      pts_present;                       /**< PTS_DTS_flags say there is a PTS */
    uint64_t pts;                               /**< 33 bits, 90 kHz (0 when a marker bit is wrong) */
    int      payload;                           /**< payload offset in the packet */
    int      first_au;                          /**< offset of the first access unit in the payload, -1 none, -2 sub-picture */
    int      extra;                             /**< LPCM: bytes of the audio header at the payload's start */
    int      at;                                /**< offset of the packet in the pack */
} Pkt;

/* PES header. Packets of other streams than video, private stream 1 and
 * MPEG audio are not checked against the bytes available. */
static int parse_pes(Pkt *p, const uint8_t *b, int avail)
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
            if ((b[9] & 1) && (b[11] & 1) && (b[13] & 1))
                p->pts = (uint64_t)(b[9] & 0x0e) << 29 | b[10] << 22 | (b[11] & 0xfe) << 14 |
                         b[12] << 7 | b[13] >> 1;
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
static int parse_packet(Pkt *p, const uint8_t *b, int avail, int lpcm5)
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

/* The packet at *off of pack s (0 = the pack's start: the pack header first).
 * *off becomes the next packet's offset, PACK (or more) when the pack is
 * done: after private stream 2, at fill bytes (0x00 or 0xff to the end) and
 * after padding packets. */
static int pack_packet(const uint8_t *s, int *off, Pkt *p, int lpcm5)
{
    int at = *off, pos, lim, n = 0;

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
            return 0;
        } else {
            if (p->first_au >= 0 && p->first_au < p->extra)
                return 0;
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
        Pkt pad;
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
        Pkt t;
        int q = pos;
        if (!parse_packet(&t, s + pos, PACK - pos, 0) || !pack_packet(s, &q, &t, lpcm5))
            pos = PACK;
    }
    *off = pos;
    return 1;
}

/* ---- AC-3 / E-AC-3 frames (ATSC A/52, Annex E) ---- */

typedef struct FrameHdr {
    int size;                                   /**< bytes */
    int bsid;
} FrameHdr;

static int is_eac3(int bsid)
{
    return bsid >= 10 && bsid <= 16;
}

/* syncinfo + bsid. E-AC-3 (bsid 10..16): the frame size field only; AC-3
 * (bsid 0..8): FFmpeg's header parse (sample rate code 0..2, frame size code
 * 0..37); bsid 9 and 17.. are not frames. */
static int frame_header(FrameHdr *h, const uint8_t *b, int len)
{
    AC3HeaderInfo info, *pi = &info;

    if (len < 7)
        return 0;
    h->bsid = b[5] >> 3;
    if (is_eac3(h->bsid)) {
        h->size = ((AV_RB16(b + 2) & 0x7ff) + 1) * 2;
        return 1;
    }
    if (b[5] >= 0x48 || avpriv_ac3_parse_header(&pi, b, len) < 0)
        return 0;
    h->size = info.frame_size;
    return 1;
}

/* A/52 crc1 and crc2: bytes 2 .. 5/8 of the frame, then the remaining 3/8,
 * each give a remainder of 0. */
static int ac3_crc_ok(const uint8_t *b, int size)
{
    int n58 = size * 5 >> 3;

    const AVCRC *crc = av_crc_get_table(AV_CRC_16_ANSI);

    return !av_crc(crc, 0, b + 2, n58 - 2) && !av_crc(crc, 0, b + n58, size * 3 >> 3);
}

/* The stream header from a frame: E-AC-3 bitstream information up to the
 * channel map (the frame must hold it; a dependent stream's channel map must
 * give the channels its audio coding mode and LFE give); AC-3: syncinfo. */
static int stream_header(const uint8_t *b, int len)
{
    static const uint8_t acmod_channels[8] = { 2, 1, 2, 3, 3, 4, 4, 5 };
    FrameHdr h;
    GetBitContext gb;
    unsigned strmtyp, acmod, lfeon, chanmap = 0, chanmape = 0;
    int channels, expect;

    if (!frame_header(&h, b, len))
        return 0;
    if (!is_eac3(h.bsid))
        return 1;
    if (init_get_bits8(&gb, b + 2, len - 2) < 0)
        return 0;
    strmtyp = get_bits(&gb, 2);
    skip_bits_long(&gb, 3);                     /* substreamid */
    skip_bits_long(&gb, 11);                    /* frmsiz */
    skip_bits_long(&gb, 2);                     /* fscod */
    skip_bits_long(&gb, 2);                     /* fscod2 / numblkscod */
    acmod = get_bits(&gb, 3);
    lfeon = get_bits1(&gb);
    skip_bits_long(&gb, 5);                     /* bsid */
    skip_bits_long(&gb, 5);                     /* dialnorm */
    if (get_bits1(&gb))                            /* compre */
        skip_bits_long(&gb, 8);
    if (!acmod) {
        skip_bits_long(&gb, 5);
        if (get_bits1(&gb))
            skip_bits_long(&gb, 8);
    }
    if (strmtyp == 1 && (chanmape = get_bits1(&gb)))
        chanmap = get_bits(&gb, 16);
    if (get_bits_left(&gb) < 0)
        return 0;
    if (!chanmape)
        return 1;
    channels = acmod_channels[acmod] + lfeon;
    expect = 0;
    for (int i = 0; i < 16; i++) {
        /* bit 0 (first) .. 15: L C R Ls Rs Lc/Rc Lrs/Rrs Cs Ts Lsd/Rsd Lw/Rw Lvh/Rvh Cvh - LFE2 LFE */
        static const uint8_t weight[16] = { 1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1, 0, 1, 1 };
        if (chanmap & (0x8000 >> i))
            expect += weight[i];
    }
    return expect == channels;
}

/* AC-3 frame start: 0 no, 1 yes, 2 too few bytes to tell. */
static int frame_sync(const uint8_t *b, int len)
{
    FrameHdr h;

    if (len < 2)
        return 2;
    if (b[0] != 0x0b || b[1] != 0x77)
        return 0;
    if (len < 7)
        return 2;
    return frame_header(&h, b, len);
}

/* Bytes to skip to the next frame start: -2 need more data, -1 give up. */
static int frame_resync(const uint8_t *b, int len)
{
    if (len < 7)
        return -2;
    for (int pos = 0; pos < len - 2; pos++) {
        FrameHdr h;
        int rem = len - pos;

        if (b[pos] != 0x0b || b[pos + 1] != 0x77)
            continue;
        if (rem < 8)
            break;
        if (!frame_header(&h, b + pos, rem))
            return pos + 1;
        if (rem < h.size)
            return -2;
        if (is_eac3(h.bsid))
            return pos;
        return pos + !ac3_crc_ok(b + pos, h.size);
    }
    return len >= RESYNC_GIVE_UP ? -1 : -2;
}

/* ---- DTS core frames and DTS-HD extension substreams (ETSI TS 102 114) ---- */

static int dts_core_ok(const uint8_t *b, int len)
{
    return len >= 11 && AV_RB32(b) == DCA_SYNCWORD_CORE_BE;
}

static int dts_core_size(const uint8_t *b)
{
    return (AV_RB24(b + 5) >> 4 & 0x3fff) + 1;
}

/* Extension substream header: the fields the probe needs. */
typedef struct ExtSS {
    int      hdr_size;                          /**< header bytes */
    int      frame_size;                        /**< bytes of the substream frame */
    int      nb_assets;
    int      asset_size[8];                     /**< bytes of each asset */
} ExtSS;

/* The start of the header: user bits, substream index, header size, frame
 * size. 0 without the sync word, with fewer than 10 bytes or past the end. */
static int extss_start(ExtSS *h, GetBitContext *r, const uint8_t *b, int len, int *idx, int *blong)
{
    if (len < 10 || AV_RB32(b) != DCA_SYNCWORD_SUBSTREAM || init_get_bits8(r, b + 4, len - 4) < 0)
        return 0;
    skip_bits(r, 8);                            /* UserDefinedBits */
    *idx   = get_bits(r, 2);                    /* nExtSSIndex */
    *blong = get_bits1(r);                      /* bHeaderSizeType */
    if (get_bits_left(r) < 0)
        return 0;
    h->hdr_size   = get_bits(r, *blong ? 12 : 8) + 1;
    h->frame_size = get_bits_long(r, *blong ? 20 : 16) + 1;
    return get_bits_left(r) >= 0;
}

/* The whole header (through the first asset's descriptor when the static
 * fields are present): 1 valid, 0 not (also when it runs past the data). */
static int extss_header(ExtSS *h, const uint8_t *b, int len)
{
    GetBitContext gb;
    int idx, blong, stat, nb_pres, chans;
    unsigned ss_mask[8] = { 0 };

#define OVERRUN (get_bits_left(&gb) < 0)
    if (!extss_start(h, &gb, b, len, &idx, &blong) || h->hdr_size > len)
        return 0;
    stat = get_bits1(&gb);                      /* bStaticFieldsPresent */
    if (OVERRUN)
        return 0;
    nb_pres = h->nb_assets = 1;
    if (stat) {
        skip_bits_long(&gb, 2);                 /* reference clock */
        skip_bits_long(&gb, 3);                 /* frame duration */
        if (get_bits1(&gb)) {                      /* time stamp */
            skip_bits_long(&gb, 32);
            skip_bits_long(&gb, 4);
        }
        nb_pres      = get_bits(&gb, 3) + 1;
        h->nb_assets = get_bits(&gb, 3) + 1;
        if (OVERRUN)
            return 0;
        for (int i = 0; i < nb_pres; i++)
            ss_mask[i] = get_bits_long(&gb, idx + 1);
        /* active asset masks: read in steps of 2 substreams, each time as bit 0
         * of the presentation's substream mask says */
        for (int i = 0; i < nb_pres; i++)
            for (int n = 0; n < idx + 1; n += 2)
                if (ss_mask[i] & 1)
                    skip_bits_long(&gb, 8);
        if (get_bits1(&gb)) {                      /* mixing metadata */
            int a, c;
            if (OVERRUN)
                return 0;
            skip_bits_long(&gb, 2);
            a = get_bits(&gb, 2);
            c = get_bits(&gb, 2);
            if (OVERRUN)
                return 0;
            skip_bits_long(&gb, (a + 1) * 4 * (c + 1));
        } else if (OVERRUN) {
            return 0;
        }
    }
    if (OVERRUN)
        return 0;
    for (int k = 0; k < h->nb_assets; k++)
        h->asset_size[k] = get_bits_long(&gb, blong ? 20 : 16) + 1;
    for (int k = 0; k < h->nb_assets; k++) {
        skip_bits_long(&gb, 9);                 /* descriptor size */
        skip_bits_long(&gb, 3);                 /* asset index */
        if (!stat)
            continue;
        if (get_bits1(&gb))                        /* asset type */
            skip_bits_long(&gb, 4);
        if (get_bits1(&gb))                        /* language */
            skip_bits_long(&gb, 24);
        if (get_bits1(&gb)) {                      /* info text */
            int n;
            if (OVERRUN)
                return 0;
            n = get_bits(&gb, 10) + 1;
            skip_bits_long(&gb, 8 * n);
        } else if (OVERRUN) {
            return 0;
        }
        skip_bits_long(&gb, 5);                 /* bit resolution */
        skip_bits_long(&gb, 4);                 /* maximum sample rate */
        chans = get_bits(&gb, 8) + 1;
        if (get_bits1(&gb)) {                      /* one-to-one map */
            if (chans >= 3)
                skip_bits_long(&gb, 1);
            if (chans >= 7)
                skip_bits_long(&gb, 1);
            if (get_bits1(&gb))                    /* speaker mask */
                skip_bits_long(&gb, (get_bits(&gb, 2) + 1) * 4);
        }
        break;                                  /* only the first asset's descriptor */
    }
    return !OVERRUN;
#undef OVERRUN
}

/* ---- the stream probe ---- */

typedef struct Probe Probe;                    /** How a stream's data is cut into units (the frame parsers' contract). */
typedef struct Framer {
    /** unit size: < 0 need more data, 0 none, else bytes */
    int (*size)(Probe *p, const uint8_t *b, int len);/** 1 a valid unit */
    int (*check)(Probe *p, const uint8_t *b, int len);/** 0 no unit starts here, 1 one does, 2 too few bytes to tell */
    int (*sync)(Probe *p, const uint8_t *b, int len);/** bytes to skip to the next unit start, -2 need more data, -1 give up */
    int (*resync)(Probe *p, const uint8_t *b, int len);/** a unit cut: 0 = the probe fails */
    int (*unit)(Probe *p, const uint8_t *b, int len);
    int limit;                                  /**< bytes of data held while looking for units */
    int want;                                   /**< units to collect */
} Framer;

struct Probe {
    void          *log;
    DiscIOFS      *fs;
    HDDVDAACS     *aacs;
    HDDVDClip     *clip;
    const Framer  *f;
    int            key;                         /**< the stream's packet key */
    uint32_t       pos, end;                    /**< blocks */
    uint8_t       *buf;                         /**< stream data not yet cut into units */
    int            used;
    int            synced;
    int            nb_units;
    int            bsid[PROBE_FRAMES];          /**< AC-3: the first frames' bsid */
    int            dts_undecided, dts_dep;      /**< DTS: whether an extension substream follows each core frame */
};                                              /* AC-3 / E-AC-3 framer: any of the two. */

static int ac3_size(Probe *p, const uint8_t *b, int len)
{
    FrameHdr h;
    if (len < 7)
        return -1;
    return frame_header(&h, b, len) ? h.size : -2;
}

static int ac3_check(Probe *p, const uint8_t *b, int len)
{
    FrameHdr h;
    return frame_header(&h, b, len);
}

static int ac3_sync(Probe *p, const uint8_t *b, int len)
{
    return frame_sync(b, len);
}

static int ac3_resync(Probe *p, const uint8_t *b, int len)
{
    return frame_resync(b, len);
}

/* The first frame gives the stream header. */
static int ac3_unit(Probe *p, const uint8_t *b, int len)
{
    FrameHdr h;

    if (!p->nb_units && !stream_header(b, len)) {
        av_log(p->log, AV_LOG_WARNING, "EVOB %s: stream 0x%04x: its first frame is not a valid header\n",
               p->clip->evob->name, p->key);
        return 0;
    }
    frame_header(&h, b, len);
    if (p->nb_units < PROBE_FRAMES)
        p->bsid[p->nb_units] = h.bsid;
    return 1;
}

/* DTS framer: a core frame and, when one follows the first core frame, an
 * extension substream frame. */

static int dts_core_sync(const uint8_t *b, int len)
{
    if (len < 4)
        return 2;
    if (AV_RB32(b) != DCA_SYNCWORD_CORE_BE)
        return 0;
    return len < 11 ? 2 : 1;
}

static int dts_extss_sync(const uint8_t *b, int len)
{
    ExtSS h;
    GetBitContext r;
    int idx, blong;

    if (len < 4)
        return 2;
    if (AV_RB32(b) != DCA_SYNCWORD_SUBSTREAM)
        return 0;
    if (len < 10)
        return 2;
    return extss_start(&h, &r, b, len, &idx, &blong);
}

static int dts_size(Probe *p, const uint8_t *b, int len)
{
    int n, s;

    if (!dts_core_ok(b, len))
        return -1;
    n = dts_core_size(b);
    if (p->dts_undecided) {
        if (n >= len)
            return -1;
        if ((s = dts_extss_sync(b + n, len - n)) == 2)
            return -1;
        p->dts_undecided = 0;
        p->dts_dep = s == 1;
    }
    if (!p->dts_dep)
        return n;
    if (n >= len)
        return -1;
    s = dts_extss_sync(b + n, len - n);
    if (!s)
        return -2;
    if (s == 2)
        return -1;
    {
        ExtSS h;
        GetBitContext r;
        int idx, blong;
        if (len - n < 10 || !extss_start(&h, &r, b + n, len - n, &idx, &blong))
            return -1;
        return n + h.frame_size;
    }
}

static int dts_check(Probe *p, const uint8_t *b, int len)
{
    ExtSS h;
    int n;

    if (!dts_core_ok(b, len))
        return 0;
    if (!p->dts_dep)
        return 1;
    n = dts_core_size(b);
    if (n + 1 > len || dts_extss_sync(b + n, len - n) != 1)
        return 0;
    return extss_header(&h, b + n, len - n);
}

static int dts_sync(Probe *p, const uint8_t *b, int len)
{
    return dts_core_sync(b, len);
}

static int dts_resync(Probe *p, const uint8_t *b, int len)
{
    return -1;                                  /* drop what is held */
}

/* The first unit gives the stream header: a core frame and, with an
 * extension substream, its header, its frame within the unit and its assets
 * within the frame. */
static int dts_unit(Probe *p, const uint8_t *b, int len)
{
    ExtSS h;
    int n, off;

    if (p->nb_units)
        return 1;
    if (!dts_core_ok(b, len))
        goto bad;
    if (!p->dts_dep)
        return 1;
    n = dts_core_size(b);
    if (!extss_header(&h, b + n, len - n) || h.frame_size > len - n)
        goto bad;
    off = h.hdr_size;
    for (int k = 0; k < h.nb_assets; k++)
        off += h.asset_size[k];
    if ((unsigned)off <= (unsigned)(len - n))
        return 1;
bad:
    av_log(p->log, AV_LOG_WARNING, "EVOB %s: stream 0x%04x: its first frame is not a valid header\n",
           p->clip->evob->name, p->key);
    return 0;
}

static const Framer ac3_framer = { ac3_size, ac3_check, ac3_sync, ac3_resync, ac3_unit, 0x4000, PROBE_FRAMES };
static const Framer dts_framer = { dts_size, dts_check, dts_sync, dts_resync, dts_unit, 0x14000, 1 };

/* A block of the stream as the probe sees it: made usable where it can be
 * (a pack that cannot be is passed on as read: the pack rules decide);
 * 0 only when it cannot be read. */
static int probe_block(Probe *p, uint32_t block, uint8_t *sec)
{
    int ret = ff_hddvd_clip_block(p->log, p->aacs, p->fs, p->clip, block, sec);

    if (ret < 0)
        av_log(p->log, AV_LOG_WARNING, "EVOB %s: block %"PRIu32" cannot be read for the stream probe: %s\n",
               p->clip->evob->name, block, av_err2str(ret));
    else if (!ret)
        av_log(p->log, AV_LOG_DEBUG, "EVOB %s: block %"PRIu32" is probed as read\n", p->clip->evob->name, block);
    return ret >= 0;
}

/* The stream's first packet with a PTS that is the first packet of its pack. */
static int probe_first_pts(Probe *p)
{
    uint32_t n = p->end < PTS_SCAN_PACKS ? p->end : PTS_SCAN_PACKS;
    uint8_t sec[PACK];

    for (uint32_t i = 0; i < n; i++) {
        Pkt k;
        int off = 0;

        if (!probe_block(p, i, sec))
            return 0;
        if (!pack_packet(sec, &off, &k, 1)) {
            av_log(p->log, AV_LOG_WARNING, "EVOB %s: block %"PRIu32" is not a valid pack\n", p->clip->evob->name, i);
            return 0;
        }
        if (k.key == p->key && k.pts_present)
            return 1;
    }
    av_log(p->log, AV_LOG_WARNING, "EVOB %s: stream 0x%04x: no PES packet with a PTS starts one of the first "
           "%"PRIu32" packs\n", p->clip->evob->name, p->key, n);
    return 0;
}

/* Stream data in: cut it into units. */
static int probe_data(Probe *p, const uint8_t *data, int len)
{
    const Framer *f = p->f;
    int off = 0;

    if (len) {
        if (f->limit - p->used < len) {
            av_log(p->log, AV_LOG_DEBUG, "EVOB %s: stream 0x%04x: %d bytes without a unit dropped\n",
                   p->clip->evob->name, p->key, p->used);
            p->used = p->synced = 0;
            return 1;
        }
        memcpy(p->buf + p->used, data, len);
        p->used += len;
    }
    while (p->used) {
        const uint8_t *b = p->buf + off;
        int s;

        if (!p->synced) {
            int r = f->sync(p, b, p->used);
            if (!r) {
                int k = f->resync(p, b, p->used);
                if (k == -2)
                    break;
                if (k == -1) {
                    p->used = p->synced = 0;
                    return 1;
                }
                if (!k)
                    k = 1;
                p->used -= k;
                off += k;
                continue;
            }
            if (r == 1)
                p->synced = 1;
        }
        s = f->size(p, b, p->used);
        if (s <= 0 || s > p->used)
            break;
        p->used -= s;
        off += s;
        p->synced = 0;
        if (f->check(p, b, s) == 1) {
            if (!f->unit(p, b, s))
                return 0;
            p->nb_units++;
        }
    }
    if (off && p->used)
        memmove(p->buf, p->buf + off, p->used);
    return 1;
}

/* A packet's payload: the bytes before its first access unit, then the rest. */
static int probe_deliver(Probe *p, const uint8_t *sec, const Pkt *k)
{
    const uint8_t *d = sec + k->at + k->payload;
    int n = k->len - k->payload, fau = k->first_au;

    if (k->key != p->key || n <= 0)
        return 1;
    if (k->at + k->payload + n > PACK)
        n = PACK - k->at - k->payload;
    if (fau > 0 && n > fau)
        return probe_data(p, d, fau) && probe_data(p, d + fau, n - fau);
    return probe_data(p, d, n);
}

/* One pack of the stream: its packets of the probed stream go to the unit
 * cutter. A pack holds at most 2 packets unless one is MPEG audio. */
static int probe_pack(Probe *p)
{
    uint8_t sec[PACK];
    Pkt k;
    int off = 0, count = 1, mpa;
    uint32_t b = p->pos++;

    if (!probe_block(p, b, sec))
        return 0;
    if (!sec[4]) {
        int all = sec[0] == 0x00 || sec[0] == 0xff;
        for (int i = 1; all && i < PACK; i++)
            all = sec[i] == sec[0];
        if (all)
            return 1;                           /* empty pack */
    }
    if (!pack_packet(sec, &off, &k, 1)) {
        av_log(p->log, AV_LOG_WARNING, "EVOB %s: block %"PRIu32" is not a valid pack\n", p->clip->evob->name, b);
        return 0;
    }
    if (off > PACK) {
        if (k.payload + k.at > 0x3f) {
            av_log(p->log, AV_LOG_WARNING, "EVOB %s: block %"PRIu32": the first packet runs past the pack\n",
                   p->clip->evob->name, b);
            return 0;
        }
        return 1;                               /* bad pack: skipped */
    }
    mpa = (k.key & 0xfff0) == 0xd0;
    if (!probe_deliver(p, sec, &k))
        return 0;
    while (off != PACK) {
        int rem = PACK - off;

        if (!mpa && count >= 2) {
            av_log(p->log, AV_LOG_WARNING, "EVOB %s: block %"PRIu32" holds more than two packets\n",
                   p->clip->evob->name, b);
            return 0;
        }
        if ((rem < 6 || AV_RB24(sec + off) != 1 || ((sec[off + 3] & 0xfe) != 0xbe && rem < 9)) &&
            off >= 1 && off <= 0x7fc && (k.key == 0xe0 || (k.key & 0xff) == 0xbd))
            return 1;                           /* no packet starts here: the rest of the pack is dropped */
        if (!pack_packet(sec, &off, &k, 1)) {
            av_log(p->log, AV_LOG_WARNING, "EVOB %s: block %"PRIu32" is not a valid pack\n", p->clip->evob->name, b);
            return 0;
        }
        mpa |= (k.key & 0xfff0) == 0xd0;
        if (!probe_deliver(p, sec, &k))
            return 0;
        count++;
    }
    return 1;
}

/* Read the clip from its start until the framer has its units or the clip
 * ends. 1 done, 0 failed. */
static int probe_run(Probe *p)
{
    if (!probe_first_pts(p))
        return 0;
    while (p->nb_units < p->f->want && p->pos < p->end)
        if (!probe_pack(p))
            return 0;
    return 1;
}

static int probe_init(Probe *p, void *logctx, DiscIOFS *fs, HDDVDAACS *aacs, HDDVDClip *clip,
                      const Framer *f, int key)
{
    *p = (Probe){ .log = logctx, .fs = fs, .aacs = aacs, .clip = clip, .f = f, .key = key,
                  .end = clip->size / PACK, .dts_undecided = 1 };
    return (p->buf = av_mallocz(f->limit + AV_INPUT_BUFFER_PADDING_SIZE)) ? 0 : AVERROR(ENOMEM);
}

/* Dolby Digital Plus stream `number` of clip: *codec = E-AC-3, AC-3 or
 * AV_CODEC_ID_NONE (not supported). 1 done, 0 failed. */
static int probe_ddplus(void *logctx, DiscIOFS *fs, HDDVDAACS *aacs, HDDVDClip *clip, int number,
                        enum AVCodecID *codec)
{
    Probe p;
    unsigned mask;
    int ret, k;

    if ((ret = probe_init(&p, logctx, fs, aacs, clip, &ac3_framer, (0xc0 + number) << 8 | 0xbd)) < 0)
        return ret;
    if (!(ret = probe_run(&p)))
        goto end;
    k = FFMIN(p.nb_units, PROBE_FRAMES);
    mask = k << 8;
    for (int i = 0; i < k; i++)
        if (is_eac3(p.bsid[i]))
            mask |= 1 << i;
    *codec = mask == 0x40f ? AV_CODEC_ID_EAC3 : mask == 0x400 ? AV_CODEC_ID_AC3 : AV_CODEC_ID_NONE;
    av_log(logctx, *codec == AV_CODEC_ID_NONE ? AV_LOG_WARNING : AV_LOG_VERBOSE,
           "EVOB %s: Dolby Digital Plus stream %d: %d frame(s) found, %s\n", clip->evob->name, number, k,
           *codec == AV_CODEC_ID_EAC3 ? "E-AC-3" : *codec == AV_CODEC_ID_AC3 ? "AC-3 only" :
           "a mix of AC-3 and E-AC-3 or fewer than 4 frames: not supported, the stream is left out "
           "[untested on real discs]");
end:
    av_free(p.buf);
    return ret;
}

/* DTS stream `number` of clip: *hd = 1 when an extension substream follows
 * its first core frame (DTS-HD: a track with a core, plus the core as a track
 * of its own). 1 done, 0 failed (also when no unit is found). */
static int probe_dts(void *logctx, DiscIOFS *fs, HDDVDAACS *aacs, HDDVDClip *clip, int number, int *hd)
{
    Probe p;
    int ret;

    if ((ret = probe_init(&p, logctx, fs, aacs, clip, &dts_framer, (0x88 + number) << 8 | 0xbd)) < 0)
        return ret;
    if ((ret = probe_run(&p)) && !p.nb_units) {
        av_log(logctx, AV_LOG_WARNING, "EVOB %s: DTS stream %d: no frame found\n", clip->evob->name, number);
        ret = 0;
    }
    if (ret) {
        *hd = p.dts_dep;
        av_log(logctx, AV_LOG_WARNING, "EVOB %s: DTS stream %d: %s [untested on real discs]\n", clip->evob->name,
               number, *hd ? "DTS-HD (core + extension substream): listed with its core as a second track"
                           : "DTS core");
    }
    av_free(p.buf);
    return ret;
}

/* ---- playlist metadata of a stream ---- */

/* The language of audio (audio = 1) or sub-picture stream `number` (1-based)
 * of EVOB evo: the first Title (playlist, TitleSet, Title order) one of whose
 * clips names the EVOB and has an Audio / Subtitle element with that
 * streamNumber, and whose only TrackNavigationList has an AudioTrack /
 * SubtitleTrack with the same track: its langcode "LLL:N", LLL as an ISO
 * 639-2 code when known. 1 found (lang set), 0 not. */
static int stream_lang(void *logctx, HDDVDXpl *const *xpls, int nb_xpls, const char *evo, int audio,
                       uint32_t number, char lang[4])
{
    int ecls = audio ? HDDVD_XPL_AUDIO : HDDVD_XPL_SUBTITLE;
    int tcls = audio ? HDDVD_XPL_AUDIO_TRACK : HDDVD_XPL_SUBTITLE_TRACK;

    for (int q = 0; q < nb_xpls; q++) {
        const HDDVDXplNode *root = &xpls[q]->root;
        for (int a = 0; a < ff_hddvd_xpl_count(root, HDDVD_XPL_TITLE_SET); a++) {
            const HDDVDXplNode *ts = ff_hddvd_xpl_child(root, HDDVD_XPL_TITLE_SET, a);
            for (int t = 0; t < ff_hddvd_xpl_count(ts, HDDVD_XPL_TITLE); t++) {
                const HDDVDXplNode *title = ff_hddvd_xpl_child(ts, HDDVD_XPL_TITLE, t), *el = NULL, *nav;
                const char *track, *lc, *colon;
                char code[4] = { 0 };

                for (int c = 0; !el && c < ff_hddvd_xpl_count(title, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP); c++) {
                    const HDDVDXplNode *clip = ff_hddvd_xpl_child(title, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, c);
                    if (!ff_hddvd_src_names_evob(logctx, ff_hddvd_xpl_str(clip, "src"), evo))
                        continue;
                    for (int s = 0; !el && s < ff_hddvd_xpl_count(clip, ecls); s++) {
                        const HDDVDXplNode *x = ff_hddvd_xpl_child(clip, ecls, s);
                        const char *sn = ff_hddvd_xpl_str(x, "streamNumber");
                        if (sn && strtoul(sn, NULL, 10) == number)
                            el = x;
                    }
                }
                if (!el || ff_hddvd_xpl_count(title, HDDVD_XPL_TRACK_NAVIGATION_LIST) != 1)
                    continue;
                nav   = ff_hddvd_xpl_child(title, HDDVD_XPL_TRACK_NAVIGATION_LIST, 0);
                track = ff_hddvd_xpl_str(el, "track");
                lc    = NULL;
                for (int s = 0; !lc && track && s < ff_hddvd_xpl_count(nav, tcls); s++) {
                    const HDDVDXplNode *x = ff_hddvd_xpl_child(nav, tcls, s);
                    const char *tr = ff_hddvd_xpl_str(x, "track");
                    if (tr && !strcmp(tr, track)) {
                        lc = ff_hddvd_xpl_str(x, "langcode");
                        if (!lc)
                            break;
                    }
                }
                if (!lc || !(colon = strchr(lc, ':')) || colon - lc > 3)
                    continue;
                memcpy(code, lc, colon - lc);
                if (code[0]) {
                    const char *iso = ff_disc_lang_code(code);
                    if (iso)
                        av_strlcpy(code, iso, sizeof(code));
                }
                memcpy(lang, code, 4);
                return 1;
            }
        }
    }
    return 0;
}

/* ---- the tracks of a title ---- */

static const enum AVCodecID video_codecs[4] = {
    AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_H264, AV_CODEC_ID_VC1,
};

static const enum AVCodecID audio_codecs[8] = {   /* coding 3: no codec */
    AV_CODEC_ID_AC3, AV_CODEC_ID_TRUEHD, AV_CODEC_ID_MP2, AV_CODEC_ID_NONE,
    AV_CODEC_ID_PCM_DVD, AV_CODEC_ID_PCM_DVD, AV_CODEC_ID_DTS, AV_CODEC_ID_EAC3,
};

/* Dolby Digital Plus (coding 7) and DTS (coding 6) are probed; coding 3 has
 * no stream handler: a title with such a stream cannot be built. */
#define CODING_UNKNOWN  3
#define CODING_DTS      6
#define CODING_DDPLUS   7

typedef struct Builder {
    void            *log;
    DiscIOFS        *fs;
    HDDVDAACS       *aacs;
    const HDDVDVTI  *vti;
    HDDVDXpl *const *xpls;
    int              nb_xpls;
    unsigned         untested;                  /**< features already reported as untested */
} Builder;                                      /* Report once per disc that a feature met has not been seen on a real disc. */
static void untested(Builder *b, unsigned bit, const char *what)
{
    if (b->untested & bit)
        return;
    b->untested |= bit;
    av_log(b->log, AV_LOG_WARNING, "HD DVD: %s [untested on real discs]\n", what);
}

/* Content probe of every audio stream of clip that needs one (once per
 * clip). 1 done, 0 a probe failed. */
static int probe_clip(Builder *b, HDDVDClip *clip, const HDDVDEvobAttr *a)
{
    for (int j = 0; j < a->nb_audio; j++) {
        uint8_t c0 = a->raw[0x10 + 4 * j];
        int coding = c0 < 0x20 ? c0 >> 2 : -1, ret, hd = 0;
        enum AVCodecID codec = coding >= 0 ? audio_codecs[coding] : AV_CODEC_ID_NONE;

        if (clip->probed[j])
            continue;
        if (coding == CODING_DDPLUS) {
            if ((ret = probe_ddplus(b->log, b->fs, b->aacs, clip, j, &codec)) <= 0)
                return ret;
        } else if (coding == CODING_DTS) {
            if ((ret = probe_dts(b->log, b->fs, b->aacs, clip, j, &hd)) <= 0)
                return ret;
        }
        clip->probed[j]      = 1;
        clip->probe_codec[j] = codec;
        clip->probe_core[j]  = hd;
    }
    return 1;
}

/* Frame size of sub-pictures from the video attribute (bytes 0x02..0x04). */
static void subpic_size(const uint8_t *v, int *w, int *h)
{
    *h = (v[0] & 0x14) == 4 ? 576 : 480;
    *w = 352;
    switch (v[2] >> 4) {
    case 0:  *h /= 2;                   break;
    case 1:                             break;
    case 2:  *w = 480;                  break;
    case 3:  *w = 544;                  break;
    case 4:  *w = 704;                  break;
    case 5:  *w = 720;                  break;
    case 8:  *w = 1280; *h = 720;       break;
    case 9:  *w = 960;  *h = 1080;      break;
    case 10: *w = 1280; *h = 1080;      break;
    case 11: *w = 1440; *h = 1080;      break;
    case 12: *w = 1920; *h = 1080;      break;
    default: *w = 0;    *h = 0;         break;
    }
}

/* The track of stream index i of clip: 1 described (codec NONE: no track),
 * 0 no such stream. */
static int describe(Builder *b, const HDDVDClip *clip, const HDDVDEvobAttr *a, int i, HDDVDTrack *t)
{
    memset(t, 0, sizeof(*t));
    t->index = i;
    if (!i) {
        uint8_t v = a->raw[2];
        t->type  = AVMEDIA_TYPE_VIDEO;
        t->codec = v < 0x80 ? video_codecs[v >> 5] : AV_CODEC_ID_NONE;
        return 1;
    }
    if (i <= a->nb_audio) {
        int j = i - 1;
        uint8_t c0 = a->raw[0x10 + 4 * j];

        t->type   = AVMEDIA_TYPE_AUDIO;
        t->number = j;
        t->coding = c0 < 0x20 ? c0 >> 2 : -1;
        t->codec  = clip->probed[j] ? clip->probe_codec[j] : t->coding >= 0 ? audio_codecs[t->coding] : AV_CODEC_ID_NONE;
        if (t->codec != AV_CODEC_ID_NONE)
            stream_lang(b->log, b->xpls, b->nb_xpls, clip->evob->name, 1, j + 1, t->lang);
        return 1;
    }
    if (i <= a->nb_audio + a->nb_subpic) {
        const uint8_t *p = a->raw + 0xe6 + 5 * (i - 1 - a->nb_audio);
        int k;

        t->type  = AVMEDIA_TYPE_SUBTITLE;
        t->codec = AV_CODEC_ID_NONE;
        for (k = 1; k <= 4 && !(p[k] & 0x20); k++)
            ;
        if (k <= 4) {
            t->number = p[k] & 0x1f;
            t->coding = p[0] >> 5;
            if (t->coding == 0 || t->coding == 1 || t->coding == 4) {
                t->codec   = AV_CODEC_ID_DVD_SUBTITLE;
                subpic_size(a->raw + 2, &t->width, &t->height);
                t->palette = a->words + ((p[1] & 0x20) ? 16 : 0);
            }
        }
        stream_lang(b->log, b->xpls, b->nb_xpls, clip->evob->name, 0, t->number + 1, t->lang);
        return 1;
    }
    return 0;
}

static int add_track(HDDVDTitle *t, const HDDVDTrack *k)
{
    return av_dynarray2_add((void **)&t->tracks, &t->nb_tracks, sizeof(*k), (const uint8_t *)k) ? 0 : AVERROR(ENOMEM);
}

/* The tracks of title t: 1 built, 0 the title cannot be used, < 0 error. */
static int build_title(Builder *b, HDDVDTitle *t)
{
    HDDVDClip *clip;
    const HDDVDEvobAttr *a;
    HDDVDTrack k;
    int seg = 0, n, ret;

    for (int i = 1; i < t->nb_clips; i++)
        if (t->clips[i]->size > t->clips[seg]->size)
            seg = i;
    clip = t->clips[seg];
    if (clip->evob->attr < 1 || clip->evob->attr > b->vti->nb_attrs)
        return 0;
    a = &b->vti->attrs[clip->evob->attr - 1];
    n = 1 + a->nb_audio + a->nb_subpic;
    t->segment = seg;

    if ((ret = probe_clip(b, clip, a)) <= 0) {
        if (!ret)
            av_log(b->log, AV_LOG_WARNING, "Title %s: the streams of EVOB %s cannot be probed: "
                   "the title is left out\n", t->name, clip->evob->name);
        return ret;
    }
    /* video: the first stream with a video codec */
    for (int i = 0; i < n; i++) {
        describe(b, clip, a, i, &k);
        if (k.type == AVMEDIA_TYPE_VIDEO && k.codec != AV_CODEC_ID_NONE) {
            if ((ret = add_track(t, &k)) < 0)
                return ret;
            break;
        }
    }
    if (!t->nb_tracks) {
        av_log(b->log, AV_LOG_WARNING, "Title %s: EVOB %s has no video stream: the title is left out\n",
               t->name, clip->evob->name);
        return 0;
    }
    /* audio, then sub-pictures; a stream of the same codec and number as an
     * earlier one is left out */
    for (int pass = 0; pass < 2; pass++) {
        enum AVMediaType type = pass ? AVMEDIA_TYPE_SUBTITLE : AVMEDIA_TYPE_AUDIO;
        for (int i = 0; i < n; i++) {
            int dup = 0;

            describe(b, clip, a, i, &k);
            if (k.type == AVMEDIA_TYPE_AUDIO && type == k.type && k.coding == CODING_UNKNOWN) {
                untested(b, 1, "audio stream with coding mode 3: a title with one cannot be used");
                av_log(b->log, AV_LOG_WARNING, "Title %s: audio stream %d has coding mode 3, which has no "
                       "stream handler: the title is left out\n", t->name, i);
                return 0;
            }
            if (k.type != type || k.codec == AV_CODEC_ID_NONE)
                continue;
            for (int m = 1; m < t->nb_tracks && !dup; m++)
                dup = t->tracks[m].codec == k.codec && t->tracks[m].number == k.number &&
                      t->tracks[m].core == (k.type == AVMEDIA_TYPE_AUDIO && clip->probe_core[k.number]);
            if (dup) {
                av_log(b->log, AV_LOG_WARNING, "Title %s: %s stream %d is the same as an earlier one "
                       "and is left out\n", t->name, pass ? "sub-picture" : "audio", i);
                continue;
            }
            if (k.type == AVMEDIA_TYPE_AUDIO && clip->probe_core[k.number]) {
                k.core = 1;
                if ((ret = add_track(t, &k)) < 0)
                    return ret;
                k.core = 2;                     /* its core, as a track of its own */
            }
            if (k.codec == AV_CODEC_ID_PCM_DVD)
                untested(b, 2, "LPCM audio");
            else if (k.codec == AV_CODEC_ID_MP2)
                untested(b, 4, "MPEG audio");
            if ((ret = add_track(t, &k)) < 0)
                return ret;
        }
    }
    if (t->tracks[0].codec == AV_CODEC_ID_H264)
        untested(b, 8, "AVC video");
    return 1;
}

int ff_hddvd_tracks_build(void *logctx, DiscIOFS *fs, HDDVDAACS *aacs, const HDDVDVTI *vti,
                          HDDVDXpl *const *xpls, int nb_xpls, HDDVDTitlePlan *plan)
{
    Builder b = { logctx, fs, aacs, vti, xpls, nb_xpls };
    int i = 0;

    while (i < plan->nb_titles) {
        HDDVDTitle *t = &plan->titles[i];
        int ret = build_title(&b, t);

        if (ret < 0)
            return ret;
        if (ret) {
            av_log(logctx, AV_LOG_VERBOSE, "Title %d (%s): %d track(s) from EVOB %s\n", i, t->name,
                   t->nb_tracks, t->clips[t->segment]->evob->name);
            i++;
            continue;
        }
        av_free(t->clips);
        av_free(t->marks);
        av_free(t->tracks);
        memmove(t, t + 1, (plan->nb_titles - i - 1) * sizeof(*t));
        plan->nb_titles--;
    }
    return 0;
}
