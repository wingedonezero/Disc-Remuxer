/*
 * DVD-Video: reading a title's segments sector by sector. A segment's
 * extents name sectors of its title set's title VOBs (the same title VOB
 * stream the navigation reads: an image's sectors from the IFO header's
 * title VOB start, a folder's VTS_nn_1..9.VOB joined); each sector read is
 * checked before it is used:
 *
 * - a sector that is no MPEG-2 pack (pack_start_code, '01' marker) or whose
 *   pack header is not followed by a packet start code is left out (no
 *   packet of it reaches the demuxer);
 * - a packet of the video stream 0xE0, private stream 1 or MPEG audio
 *   0xC0-0xC7 whose PES_scrambling_control is set is descrambled with its
 *   title set's CSS title key (DVD-Video, CSS) and the bits cleared; every
 *   other sector is used as it is.
 *
 * The pack header is 14 bytes plus pack_stuffing_length, the stuffing
 * counted only when its first byte is not zero.
 *
 * The title key is looked for when a title set's first scrambled sector is
 * read: a title set whose title VOBs lie inside those of a title set that
 * has its key takes that key; otherwise libdvdcss finds it (no drive: it
 * cracks the key from the scrambled sectors from there on). The key is
 * checked on the first sector after it that can tell a right key from a
 * wrong one: an AC-3 frame whose sync information lies in the sector's
 * clear part and which ends in its scrambled part (the frame's CRC, A/52
 * 7.10.1), or an MPEG-2 sequence header in the clear part whose
 * sequence_extension start code lies in the scrambled part (ISO/IEC
 * 13818-2 6.2.2: in MPEG-2 video a sequence_extension follows every
 * sequence header).
 * Without a key (none found, or the check fails) the title set's scrambled
 * sectors cannot be read.
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
#include <string.h>

#include <dvdcss/dvdcss.h>

#include "libavutil/crc.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"

#include "dvd_internal.h"

#define CLEAR       0x80        /* CSS leaves the first 128 bytes of a sector clear */
#define CHECK_LIMIT 8192        /* sectors after the key's start searched for one the key check can use */

/* The pack header's length, 0 when the sector is no MPEG-2 pack. */
static int pack_header_len(const uint8_t *b)
{
    int st;

    if (AV_RB32(b) != 0x000001ba || (b[4] & 0xc0) != 0x40)
        return 0;
    st = b[13] & 7;
    return st && b[14] ? 14 + st : 14;
}

/* 1: the sector's packet is of a stream CSS scrambles and is scrambled
 * (*h: its pack header's length); 0: it is used as it is; -1: left out. */
static int sector_kind(const uint8_t *b, int *h)
{
    int id;

    if (!(*h = pack_header_len(b)) || AV_RB24(b + *h) != 1)
        return -1;
    id = b[*h + 3];
    if (id != 0xbd && id != 0xe0 && (id & 0xf8) != 0xc0)
        return 0;
    return !!(b[*h + 6] & 0x30);
}

/* ---- the key check ---- */

/* An AC-3 frame's size in bytes from its syncinfo (A/52 table 5.18), 0
 * when fscod / frmsizecod are not valid. */
static int ac3_frame_size(const uint8_t *si)
{
    static const uint16_t words[19][3] = {     /* 48, 44.1, 32 kHz */
        {   64,   69,   96 }, {   80,   87,  120 }, {   96,  104,  144 }, {  112,  121,  168 },
        {  128,  139,  192 }, {  160,  174,  240 }, {  192,  208,  288 }, {  224,  243,  336 },
        {  256,  278,  384 }, {  320,  348,  480 }, {  384,  417,  576 }, {  448,  487,  672 },
        {  512,  557,  768 }, {  640,  696,  960 }, {  768,  835, 1152 }, {  896,  975, 1344 },
        { 1024, 1114, 1536 }, { 1152, 1253, 1728 }, { 1280, 1393, 1920 },
    };
    int fscod = si[4] >> 6, frmsizecod = si[4] & 0x3f;

    if (fscod == 3 || frmsizecod >= 38)
        return 0;
    return 2 * (words[frmsizecod >> 1][fscod] + (fscod == 1 ? frmsizecod & 1 : 0));
}

/* Whether a descrambled sector (pack header h bytes) shows the key right:
 * 1 right, 0 wrong, -1 the sector cannot tell (*what: 1 AC-3, 2 video).
 * mpeg2: the title set's video is MPEG-2 (MPEG-1 has no sequence_extension). */
static int key_check(const uint8_t *sec, int h, int mpeg2, int *what)
{
    int id = sec[h + 3], len = AV_RB16(sec + h + 4), hdr = 9 + sec[h + 8], start = h + hdr, end = h + 6 + len;

    if (end > DVD_SECTOR || start >= end)
        return -1;
    if (id == 0xbd && (sec[start] & 0xf8) == 0x80 && end - start > 4) {
        /* AC-3: sub-stream id, frame count, first access unit pointer (from
         * the byte after it) */
        int first = AV_RB16(sec + start + 2), au = start + 3 + first, fs;

        if (!first || au + 7 > CLEAR || AV_RB16(sec + au) != 0x0b77 || !(fs = ac3_frame_size(sec + au)) ||
            au + fs > end || au + fs <= CLEAR)
            return -1;
        *what = 1;
        return !av_crc(av_crc_get_table(AV_CRC_16_ANSI), 0, sec + au + 2, fs - 2);
    }
    if (id == 0xe0 && mpeg2) {
        /* MPEG-2 video: a sequence header starting at the payload */
        int p = start, ext;

        if (p + 12 > CLEAR || AV_RB32(sec + p) != 0x000001b3)
            return -1;
        ext = p + 12;
        if (sec[p + 11] & 2)            /* load_intra_quantiser_matrix: 64 bytes, then the non-intra flag */
            ext += 64;
        if (ext > end || (sec[ext - 1] & 1 && (ext += 64) > end) || ext + 5 > end || ext + 4 <= CLEAR)
            return -1;
        *what = 2;
        return AV_RB32(sec + ext) == 0x000001b5 && (sec[ext + 4] >> 4) == 1;
    }
    return -1;
}

/* ---- the title key ---- */

typedef struct KeyStream {
    DVDDisc  *d;
    DVDIfo   *f;
    uint32_t  pos;              /* sector */
} KeyStream;

static int ks_seek(void *opaque, uint64_t pos)
{
    KeyStream *k = opaque;
    k->pos = pos / DVD_SECTOR;
    return 0;
}

static int ks_read(void *opaque, void *buf, int size)
{
    KeyStream *k = opaque;
    int done = 0;

    for (; done + DVD_SECTOR <= size && k->pos < k->f->title_vob_blocks; done += DVD_SECTOR, k->pos++)
        if (ff_dvd_vob_read(k->d, k->f, 0, k->pos, (uint8_t *)buf + done) < 0)
            return done ? done : -1;
    return done;
}

static int ks_readv(void *opaque, const void *iov, int blocks)
{
    return -1;
}

/* f's title VOBs lie inside o's (the same sectors of an image). */
static int key_shared(const DVDIfo *f, const DVDIfo *o)
{
    return o != f && o && o->css_state > 0 && f->title_vob_base && o->title_vob_base &&
           (o->title_vob_base == f->title_vob_base ||
            (o->title_vob_base < f->title_vob_base &&
             f->title_vob_blocks + f->title_vob_base <= o->title_vob_base + o->title_vob_blocks));
}

/* The key's check: the sectors from sector on, descrambled, until one tells. */
static int check_key(DVDDisc *d, DVDIfo *f, uint32_t sector, const uint8_t *key)
{
    int mpeg2 = f->header[DVD_VTS_VIDEO_ATTR] >> 6 == 1;
    uint8_t b[DVD_SECTOR];

    for (uint32_t s = sector; s < f->title_vob_blocks && s - sector < CHECK_LIMIT; s++) {
        int h, what = 0, ok;

        if (ff_dvd_vob_read(d, f, 0, s, b) < 0)
            return AVERROR(EIO);
        if (sector_kind(b, &h) != 1)
            continue;
        dvdcss_descramble(key, b);
        if ((ok = key_check(b, h, mpeg2, &what)) < 0)
            continue;
        av_log(d->log, ok ? AV_LOG_VERBOSE : AV_LOG_ERROR, "DVD: VTS_%02d: the CSS title key %s on sector %"PRIu32
               " (%s)\n", f->index, ok ? "is right" : "is wrong", s,
               what == 1 ? "an AC-3 frame's CRC" : "an MPEG-2 sequence header's extension");
        return ok;
    }
    av_log(d->log, AV_LOG_WARNING, "DVD: VTS_%02d: no sector within %d after %"PRIu32" can check the CSS title key: "
           "it is used unchecked [untested on real discs]\n", f->index, CHECK_LIMIT, sector);
    return 1;
}

/* f's title key, looked for from its scrambled sector on: 0 found (or
 * taken), < 0 none. */
static int find_key(DVDDisc *d, DVDIfo *f, uint32_t sector)
{
    KeyStream ks = { d, f, 0 };
    dvdcss_stream_cb cb = { ks_seek, ks_read, ks_readv };
    dvdcss_t css;
    int ret;

    for (int i = 1; i < d->nb_ifos; i++)
        if (key_shared(f, d->ifos[i])) {
            memcpy(f->css_key, d->ifos[i]->css_key, sizeof(f->css_key));
            f->css_state = 1;
            av_log(d->log, AV_LOG_VERBOSE, "DVD: VTS_%02d: CSS title key taken from VTS_%02d (its title VOBs hold "
                   "these)\n", f->index, i);
            return 0;
        }
    av_log(d->log, AV_LOG_INFO, "DVD: VTS_%02d: sector %"PRIu32" is scrambled: looking for the CSS title key\n",
           f->index, sector);
    f->css_state = -1;
    if (!(css = dvdcss_open_stream(&ks, &cb))) {
        av_log(d->log, AV_LOG_ERROR, "DVD: VTS_%02d: libdvdcss cannot be opened on the title VOBs\n", f->index);
        return AVERROR_EXTERNAL;
    }
    ret = dvdcss_title_key(css, sector, f->css_key);
    dvdcss_close(css);
    if (ret <= 0) {
        av_log(d->log, AV_LOG_ERROR, "DVD: VTS_%02d: no CSS title key found (%s): its scrambled sectors cannot be "
               "read\n", f->index, ret < 0 ? "libdvdcss failed" : "no scrambled data from there on");
        return AVERROR_INVALIDDATA;
    }
    av_log(d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: CSS title key %02x %02x %02x %02x %02x\n", f->index, f->css_key[0],
           f->css_key[1], f->css_key[2], f->css_key[3], f->css_key[4]);
    if ((ret = check_key(d, f, sector, f->css_key)) <= 0)
        return ret < 0 ? ret : AVERROR_INVALIDDATA;
    f->css_state = 1;
    av_log(d->log, AV_LOG_INFO, "DVD: VTS_%02d: CSS title key found\n", f->index);
    return 0;
}

int ff_dvd_title_vob_sector(DVDDisc *d, DVDIfo *f, uint32_t sector, uint8_t *buf)
{
    int ret, h, kind;

    if ((ret = ff_dvd_vob_read(d, f, 0, sector, buf)) < 0)
        return ret;
    if ((kind = sector_kind(buf, &h)) < 0) {
        av_log(d->log, AV_LOG_DEBUG, "DVD: VTS_%02d: title VOB sector %"PRIu32" is no MPEG-2 pack with a packet: "
               "left out\n", f->index, sector);
        return 0;
    }
    if (!kind)
        return 1;
    if (f->css_state < 0)
        return AVERROR_INVALIDDATA;
    if (!f->css_state && (ret = find_key(d, f, sector)) < 0)
        return ret;
    dvdcss_descramble(f->css_key, buf);
    buf[h + 6] &= 0xcf;
    return 1;
}

uint32_t ff_dvd_segment_blocks(const DVDSegment *g)
{
    const DVDExtent *e = g->nb_extents ? &g->extents[g->nb_extents - 1] : NULL;
    return e ? e->logical + e->count : 0;
}

/* The title VOB sector of a segment's block; < 0 when none holds it. */
static int segment_sector(const DVDSegment *g, uint32_t block, uint32_t *sector)
{
    int lo = 0, hi = g->nb_extents - 1;

    while (lo <= hi) {                  /* the extent holding block (the extents follow each other) */
        int mid = (lo + hi) / 2;
        const DVDExtent *e = &g->extents[mid];
        if (block < e->logical)
            hi = mid - 1;
        else if (block >= e->logical + e->count)
            lo = mid + 1;
        else {
            *sector = e->sector + (block - e->logical);
            return 0;
        }
    }
    return AVERROR(EINVAL);
}

int ff_dvd_segment_read(DVDDisc *d, const DVDTitle *t, int seg, uint32_t block, uint8_t *buf)
{
    uint32_t sector;
    int ret = segment_sector(&t->segments[seg], block, &sector);

    return ret < 0 ? ret : ff_dvd_title_vob_sector(d, t->vts, sector, buf);
}

int ff_dvd_segment_place(DVDDisc *d, const DVDTitle *t, int seg, uint32_t block, char *file, int size,
                         int64_t *offset)
{
    uint32_t sector;
    int ret = segment_sector(&t->segments[seg], block, &sector);

    return ret < 0 ? ret : ff_dvd_title_vob_place(d, t->vts, sector, file, size, offset);
}
