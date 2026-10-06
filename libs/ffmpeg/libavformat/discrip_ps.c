/*
 * The program stream demuxer of the disc formats (DVD-Video VOBs, HD DVD
 * EVOBs): the stream is read one 2048-byte sector (one pack) at a time and
 * its packets with discrip_pack.c's rules (the reference's disc reader):
 * an empty sector has none; a pack holds at most two packets unless one is
 * MPEG audio (else: fail); bytes that are no packet start after a video or
 * private stream 1 packet drop the rest of the sector (elsewhere: fail); a
 * first packet running past the sector leaves the sector out (short header)
 * or fails; a PTS whose marker bits are wrong is no time; an LPCM packet
 * shorter than its audio frame header, or whose first access unit starts
 * inside it, fails.
 *
 * Each packet's payload is a packet of the stream whose id is FFmpeg's
 * MPEG-PS demuxer's (mpeg.c): video and MPEG audio 0x100 | stream_id, VC-1
 * 0xFD00 | stream_id_extension, private stream 1 the sub-stream id, after
 * the sub-stream header (sub-pictures: the id byte; audio: id, frame count,
 * first access unit pointer; TrueHD / MLP: 5 bytes). A sub-stream 0xA0-0xAF
 * is always linear PCM, its audio frame header kept in the payload (3 bytes
 * on DVD-Video, 5 on HD DVD: option lpcm_header) for the rip core; FFmpeg's
 * mpeg.c guesses MLP from that header's dynamic range byte and cuts 6 more
 * bytes, which ruins LPCM whose dynamic range is not 0x80.
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

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "demux.h"
#include "discrip.h"
#include "internal.h"

#define QUEUE 8                  /* packets of one sector (two, or more with MPEG audio) */

typedef struct DiscPsContext {
    const AVClass *class;
    int        lpcm_header;      /**< option: the LPCM audio frame header's length, 3 or 5 */
    int64_t    sector;           /**< sectors read */
    AVPacket  *q[QUEUE];         /**< the current sector's packets, not given out yet */
    int        q_head, q_n;
    int        sticky;           /**< a failed sector: every later read returns this */
} DiscPsContext;

/* The stream of a packet: FFmpeg's MPEG-PS ids and codecs; 0 when the
 * packet carries no stream (padding, private stream 2, system header,
 * sub-streams and stream ids no disc format uses). */
static int stream_of(const DRPesPacket *p, int *id, enum AVMediaType *type, enum AVCodecID *codec)
{
    int sid = p->key & 0xff;

    if (sid == 0xbd) {
        int sub = p->key >> 8;
        *id   = sub;
        *type = AVMEDIA_TYPE_AUDIO;
        if (sub >= 0x20 && sub <= 0x3f) {
            *type  = AVMEDIA_TYPE_SUBTITLE;
            *codec = AV_CODEC_ID_DVD_SUBTITLE;
        } else if (sub >= 0x80 && sub <= 0x87) {
            *codec = AV_CODEC_ID_AC3;
        } else if ((sub >= 0x88 && sub <= 0x8f) || (sub >= 0x98 && sub <= 0x9f)) {
            *codec = AV_CODEC_ID_DTS;
        } else if (sub >= 0xa0 && sub <= 0xaf) {
            *codec = AV_CODEC_ID_PCM_DVD;
        } else if (sub >= 0xb0 && sub <= 0xbf) {
            *codec = AV_CODEC_ID_TRUEHD;
        } else if (sub >= 0xc0 && sub <= 0xcf) {
            *codec = AV_CODEC_ID_AC3;        /* AC-3 and E-AC-3 */
        } else {
            return 0;
        }
        return 1;
    }
    if (sid == 0xfd) {
        int ext = p->key >> 8;
        if (ext < 0x55 || ext > 0x5f)
            return 0;
        *id    = 0xfd00 | ext;
        *type  = AVMEDIA_TYPE_VIDEO;
        *codec = AV_CODEC_ID_VC1;
        return 1;
    }
    if (p->key > 0xff)
        return 0;
    if (sid >= 0xe0 && sid <= 0xef) {
        *id    = 0x100 | sid;
        *type  = AVMEDIA_TYPE_VIDEO;
        *codec = sid == 0xe2 ? AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG2VIDEO;
        return 1;
    }
    if (sid >= 0xc0 && sid <= 0xdf) {
        *id    = 0x100 | sid;
        *type  = AVMEDIA_TYPE_AUDIO;
        *codec = AV_CODEC_ID_MP2;
        return 1;
    }
    return 0;
}

typedef struct SectorRead {
    AVFormatContext *s;
    int64_t          pos;        /* the sector's byte position */
} SectorRead;

static int on_packet(void *opaque, const uint8_t *sec, const DRPesPacket *p)
{
    SectorRead *r = opaque;
    AVFormatContext *s = r->s;
    DiscPsContext *c = s->priv_data;
    enum AVMediaType type;
    enum AVCodecID codec;
    AVStream *st = NULL;
    AVPacket *pkt;
    int id, start, end, ret;

    if (!stream_of(p, &id, &type, &codec))
        return 0;
    start = p->at + p->payload - p->extra;          /* LPCM: its audio frame header stays */
    end   = p->len ? FFMIN(p->at + p->len, DR_PACK_SIZE) : DR_PACK_SIZE;
    if (end <= start)
        return 0;
    for (int i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->id == id)
            st = s->streams[i];
    if (!st) {
        if (!(st = avformat_new_stream(s, NULL)))
            return AVERROR(ENOMEM);
        st->id                   = id;
        st->codecpar->codec_type = type;
        st->codecpar->codec_id   = codec;
        avpriv_set_pts_info(st, 33, 1, 90000);
    }
    if (st->discard >= AVDISCARD_ALL)
        return 0;
    if (p->pts_present && !p->pts_valid)
        av_log(s, AV_LOG_WARNING, "Disc pack reader: sector %"PRId64", stream 0x%x: the PTS's marker bits are "
               "wrong: the packet has no time\n", r->pos / DR_PACK_SIZE, id);
    if (c->q_n == QUEUE)
        return AVERROR_BUG;
    if (!(pkt = av_packet_alloc()))
        return AVERROR(ENOMEM);
    if ((ret = av_new_packet(pkt, end - start)) < 0) {
        av_packet_free(&pkt);
        return ret;
    }
    memcpy(pkt->data, sec + start, end - start);
    pkt->pts          = p->pts_valid ? (int64_t)p->pts : AV_NOPTS_VALUE;
    pkt->dts          = AV_NOPTS_VALUE;
    pkt->pos          = r->pos + p->at;
    pkt->stream_index = st->index;
    c->q[(c->q_head + c->q_n++) % QUEUE] = pkt;
    return 0;
}

static int ps_read_header(AVFormatContext *s)
{
    s->ctx_flags |= AVFMTCTX_NOHEADER;
    return 0;
}

static int ps_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    DiscPsContext *c = s->priv_data;
    uint8_t sec[DR_PACK_SIZE];

    for (;;) {
        SectorRead r = { s, avio_tell(s->pb) };
        const char *why;
        int n, at, ret;

        if (c->q_n) {
            AVPacket *q = c->q[c->q_head];
            c->q_head = (c->q_head + 1) % QUEUE;
            c->q_n--;
            av_packet_move_ref(pkt, q);
            av_packet_free(&q);
            return 0;
        }
        if (c->sticky)
            return c->sticky;
        n = avio_read(s->pb, sec, DR_PACK_SIZE);
        if (n == AVERROR_EOF || n == 0)
            return AVERROR_EOF;
        if (n < 0)
            return n;
        if (n < DR_PACK_SIZE) {
            av_log(s, AV_LOG_ERROR, "Disc pack reader: the stream ends %d bytes into sector %"PRId64"\n", n,
                   c->sector);
            return c->sticky = AVERROR_INVALIDDATA;
        }
        ret = ff_discrip_pack_sector(sec, c->lpcm_header == 5, on_packet, &r, &why, &at);
        c->sector++;
        switch (ret) {
        case DR_SECTOR_OK:
        case DR_SECTOR_EMPTY:
            break;
        case DR_SECTOR_BAD:
            av_log(s, AV_LOG_WARNING, "Disc pack reader: sector %"PRId64": %s: left out\n", c->sector - 1, why);
            break;
        case DR_SECTOR_REST_DROPPED:
            av_log(s, AV_LOG_WARNING, "Disc pack reader: sector %"PRId64": %s (byte %d): the rest of the sector "
                   "is left out\n", c->sector - 1, why, at);
            break;
        case DR_SECTOR_FAIL:
            av_log(s, AV_LOG_ERROR, "Disc pack reader: sector %"PRId64" (byte %d): %s\n", c->sector - 1, at, why);
            return c->sticky = AVERROR_INVALIDDATA;
        default:
            if (ret < 0)
                return c->sticky = ret;
        }
    }
}

static int ps_read_close(AVFormatContext *s)
{
    DiscPsContext *c = s->priv_data;

    while (c->q_n) {
        av_packet_free(&c->q[c->q_head]);
        c->q_head = (c->q_head + 1) % QUEUE;
        c->q_n--;
    }
    return 0;
}

#define OFFSET(x) offsetof(DiscPsContext, x)
static const AVOption ps_options[] = {
    { "lpcm_header", "length of the LPCM audio frame header (3 DVD-Video, 5 HD DVD)", OFFSET(lpcm_header),
      AV_OPT_TYPE_INT, { .i64 = 3 }, 3, 5, AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};

static const AVClass ps_class = {
    .class_name = "disc pack reader",
    .item_name  = av_default_item_name,
    .option     = ps_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_discrip_ps_demuxer = {
    .p.name         = "discrip_ps",
    .p.long_name    = NULL_IF_CONFIG_SMALL("MPEG-PS of a disc format (DVD-Video, HD DVD)"),
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_TS_DISCONT,
    .p.priv_class   = &ps_class,
    .priv_data_size = sizeof(DiscPsContext),
    .read_header    = ps_read_header,
    .read_packet    = ps_read_packet,
    .read_close     = ps_read_close,
};
