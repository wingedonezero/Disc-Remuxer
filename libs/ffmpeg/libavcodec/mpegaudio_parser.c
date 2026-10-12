/*
 * MPEG Audio parser
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2003 Michael Niedermayer
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

#include "parser.h"
#include "get_bits.h"
#include "mpegaudio.h"
#include "mpegaudiodata.h"
#include "mpegaudiodecheader.h"
#include "parser_internal.h"
#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavformat/apetag.h" // for APE tag.
#include "libavformat/id3v1.h" // for ID3v1_TAG_SIZE

typedef struct MpegAudioParseContext {
    ParseContext pc;
    int frame_size;
    uint32_t header;
    int header_count;
    int no_bitrate;
    /* disc mode (PARSER_FLAG_DISC) */
    FFDiscFrames disc;
    int disc_dvd;                       ///< a DVD-Video disc's stream (PARSER_FLAG_DISC_DVD)
    int disc_free;                      ///< the free-format frame size found (0 none yet)
} MpegAudioParseContext;

#define MPA_HEADER_SIZE 4

/* header + layer + freq + lsf/mpeg25 */
#define SAME_HEADER_MASK \
   (0xffe00000 | (3 << 17) | (3 << 10) | (3 << 19))

/* ---- Disc mode (PARSER_FLAG_DISC): the disc reader's MPEG audio frame rules ----
 * A frame starts with a header FFmpeg's header check accepts (ISO/IEC 11172-3
 * 2.4.1.3; not the reserved version); on a DVD-Video disc only Layer II at
 * 48 kHz that is not dual channel. Its size is the header's; a free-format
 * frame (bitrate index 0) is as long as the distance L (16 to 6143 bytes) at
 * which the headers at L, 2L, 3L, 4L and 5L all match it (sync, ID, layer,
 * sampling frequency), found once and kept. Where no frame starts, the next
 * one is a header that passes the same checks and, when it has a CRC, whose
 * CRC is right (CRC-16, polynomial 0x8005 from 0xFFFF, over header bytes 2
 * and 3 and the bits of the side information the CRC covers: Layer I
 * 128 + 4 x bound x (channels - 1); Layer II the bit allocation and the
 * scale factor selection; Layer III 17 / 32 bytes (MPEG-1) or 9 / 17 bytes
 * (MPEG-2) for one / two channels); with more than 0x10000 bytes and none,
 * the kept bytes are given up. */

#define DISC_FREE_MIN   16
#define DISC_FREE_MAX   0x1800
#define DISC_GIVE_UP    0x10000

typedef struct DiscMpa {
    int layer, lsf, channels, sample_rate, bit_rate, mode, mode_ext, crc, size;
} DiscMpa;

/* The frame header at b: 1 a header the rules accept, 0 not. */
static int disc_header(const MpegAudioParseContext *c, const uint8_t *b, DiscMpa *h)
{
    MPADecodeHeader d = { 0 };          /* free format: no frame size set */
    uint32_t head = AV_RB32(b);

    if (ff_mpa_check_header(head) < 0 || avpriv_mpegaudio_decode_header(&d, head) < 0)
        return 0;
    h->layer       = d.layer;
    h->lsf         = d.lsf;
    h->channels    = d.nb_channels;
    h->sample_rate = d.sample_rate;
    h->bit_rate    = d.bit_rate;
    h->mode        = d.mode;
    h->mode_ext    = d.mode_ext;
    h->crc         = d.error_protection;
    h->size        = d.frame_size;
    if (c->disc_dvd && (h->layer != 2 || h->sample_rate != 48000 || h->mode == MPA_DUAL))
        return 0;
    return 1;
}

static int disc_same(uint32_t a, uint32_t b)
{
    return !((a ^ b) & 0xfffe0c00);     /* sync, ID, layer, sampling frequency */
}

static int disc_sync(void *ctx, const uint8_t *b, int len)
{
    DiscMpa h;

    if (len < 4)
        return 2;
    return disc_header(ctx, b, &h);
}

static int disc_size(void *ctx, const uint8_t *b, int len)
{
    MpegAudioParseContext *c = ctx;
    DiscMpa h;

    if (len < 4)
        return -1;
    if (!disc_header(c, b, &h))
        return -2;
    if (h.bit_rate || h.size)
        return h.size;
    if (c->disc_free)
        return c->disc_free;
    for (int l = DISC_FREE_MIN; l < DISC_FREE_MAX && 5 * l + 4 <= len; l++) {
        uint32_t head = AV_RB32(b);
        int k;
        for (k = 1; k <= 5 && disc_same(head, AV_RB32(b + k * l)); k++)
            ;
        if (k > 5)
            return c->disc_free = l;
    }
    return -1;
}

static int disc_check(void *ctx, const uint8_t *b, int len)
{
    return 1;
}

/* The bits of side information a frame's CRC covers (0: none known). */
static int disc_crc_bits(const DiscMpa *h, const uint8_t *b, int len)
{
    int bound = h->mode == MPA_JSTEREO ? 4 + 4 * h->mode_ext : 32;
    GetBitContext gb;
    int table, sblimit, bits = 0, ch_rate;
    const unsigned char *alloc;
    uint8_t al[2][32] = { { 0 } };

    if (h->layer == 1)
        return 128 + 4 * bound * (h->channels - 1);
    if (h->layer == 3)
        return h->lsf ? (h->channels == 1 ? 72 : 136) : (h->channels == 1 ? 136 : 256);
    /* Layer II: the allocation table as the bitrate per channel selects it */
    if (h->lsf) {
        table = 4;
    } else if (!h->bit_rate) {
        table = h->sample_rate != 48000;
    } else {
        ch_rate = h->channels == 2 ? h->bit_rate / 2 : h->bit_rate;
        if (ch_rate > 192000)
            return 0;
        table = ch_rate <= 48000 ? 2 + (h->sample_rate == 32000) : ch_rate > 80000 ? h->sample_rate != 48000 : 0;
    }
    sblimit = ff_mpa_sblimit_table[table];
    alloc   = ff_mpa_alloc_tables[table];
    if (init_get_bits8(&gb, b + 6, len - 6) < 0)
        return 0;
    for (int sb = 0, j = 0; sb < sblimit; sb++) {
        int nbal = alloc[j];
        for (int ch = 0; ch < (sb < bound ? h->channels : 1); ch++) {
            al[ch][sb] = get_bits(&gb, nbal);
            bits += nbal;
        }
        if (sb >= bound)
            al[1][sb] = al[0][sb];
        j += 1 << nbal;
    }
    if (get_bits_left(&gb) < 0)
        return 0;
    for (int sb = 0; sb < sblimit; sb++)
        for (int ch = 0; ch < h->channels; ch++)
            bits += al[ch][sb] ? 2 : 0;
    return bits;
}

static unsigned disc_crc_add(unsigned crc, unsigned bit)
{
    unsigned top = crc >> 15 & 1;
    crc = crc << 1 & 0xffff;
    return top != bit ? crc ^ 0x8005 : crc;
}

/* The frame's CRC (after its header) against the one it carries. */
static int disc_crc_ok(const DiscMpa *h, const uint8_t *b, int len)
{
    int bits = disc_crc_bits(h, b, len);
    unsigned crc = 0xffff;

    if (!bits || 6 + (bits + 7) / 8 > len)
        return 0;
    for (int i = 0; i < 16; i++)
        crc = disc_crc_add(crc, b[2 + i / 8] >> (7 - i % 8) & 1);
    for (int i = 0; i < bits; i++)
        crc = disc_crc_add(crc, b[6 + i / 8] >> (7 - i % 8) & 1);
    return crc == AV_RB16(b + 4);
}

static int disc_resync(void *ctx, const uint8_t *b, int len)
{
    if (len < 4)
        return -2;
    for (int pos = 0; pos < len - 4; pos++) {
        DiscMpa h;
        if (b[pos] != 0xff || b[pos + 1] < 0xe0 || !disc_header(ctx, b + pos, &h))
            continue;
        if (!h.crc)
            return pos;
        if (len - pos > 6 && disc_crc_ok(&h, b + pos, len - pos))
            return pos;
    }
    return len > DISC_GIVE_UP ? -1 : -2;
}

static const FFDiscFrameRule disc_rule = { disc_sync, disc_size, disc_check, disc_resync, DISC_GIVE_UP };

static int mpegaudio_parse(AVCodecParserContext *s1,
                           AVCodecContext *avctx,
                           const uint8_t **poutbuf, int *poutbuf_size,
                           const uint8_t *buf, int buf_size)
{
    MpegAudioParseContext *s = s1->priv_data;
    ParseContext *pc = &s->pc;
    uint32_t state= pc->state;
    int i;
    int next= END_NOT_FOUND;
    int flush = !buf_size;

    if (s1->flags & PARSER_FLAG_DISC) {
        s->disc_dvd = !!(s1->flags & PARSER_FLAG_DISC_DVD);
        return ff_parser_disc_frames(s1, s, &disc_rule, &s->disc, poutbuf, poutbuf_size, buf, buf_size);
    }

    for(i=0; i<buf_size; ){
        if(s->frame_size){
            int inc= FFMIN(buf_size - i, s->frame_size);
            i += inc;
            s->frame_size -= inc;
            state = 0;

            if(!s->frame_size){
                next= i;
                break;
            }
        }else{
            while(i<buf_size){
                int ret, sr, channels, bit_rate, frame_size;
                enum AVCodecID codec_id = avctx->codec_id;

                state= (state<<8) + buf[i++];

                ret = ff_mpa_decode_header(state, &sr, &channels, &frame_size, &bit_rate, &codec_id);
                if (ret < 4) {
                    if (i > 4)
                        s->header_count = -2;
                } else {
                    int header_threshold = avctx->codec_id != AV_CODEC_ID_NONE && avctx->codec_id != codec_id;
                    if((state&SAME_HEADER_MASK) != (s->header&SAME_HEADER_MASK) && s->header)
                        s->header_count= -3;
                    s->header= state;
                    s->header_count++;
                    s->frame_size = ret-4;

                    if (s->header_count > header_threshold) {
                        avctx->sample_rate= sr;
                        av_channel_layout_uninit(&avctx->ch_layout);
                        av_channel_layout_default(&avctx->ch_layout, channels);
                        s1->duration      = frame_size;
                        avctx->codec_id   = codec_id;
                        if (s->no_bitrate || !avctx->bit_rate) {
                            s->no_bitrate = 1;
                            avctx->bit_rate += (bit_rate - avctx->bit_rate) / (s->header_count - header_threshold);
                        }
                    }

                    if (s1->flags & PARSER_FLAG_COMPLETE_FRAMES) {
                        s->frame_size = 0;
                        next = buf_size;
                    } else if (codec_id == AV_CODEC_ID_MP3ADU) {
                        avpriv_report_missing_feature(avctx,
                            "MP3ADU full parser");
                        *poutbuf = NULL;
                        *poutbuf_size = 0;
                        return buf_size; /* parsers must not return error codes */
                    }

                    break;
                }
            }
        }
    }

    pc->state= state;
    if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
        *poutbuf = NULL;
        *poutbuf_size = 0;
        return buf_size;
    }

    if (flush && buf_size >= ID3v1_TAG_SIZE && memcmp(buf, "TAG", 3) == 0) {
        *poutbuf = NULL;
        *poutbuf_size = 0;
        return next;
    }

    if (flush && buf_size >= APE_TAG_FOOTER_BYTES && memcmp(buf, APE_TAG_PREAMBLE, 8) == 0) {
        *poutbuf = NULL;
        *poutbuf_size = 0;
        return next;
    }

    *poutbuf = buf;
    *poutbuf_size = buf_size;
    return next;
}


static av_cold void mpegaudio_close(AVCodecParserContext *s1)
{
    MpegAudioParseContext *s = s1->priv_data;

    ff_parser_disc_cut_close(&s->disc.cut);
    ff_parse_close(s1);
}

const FFCodecParser ff_mpegaudio_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_MP1, AV_CODEC_ID_MP2,
                      AV_CODEC_ID_MP3, AV_CODEC_ID_MP3ADU),
    .priv_data_size = sizeof(MpegAudioParseContext),
    .parse          = mpegaudio_parse,
    .close          = mpegaudio_close,
};
