/*
 * DVD-Video: the details of a title's streams and which of them carry data.
 * The title's largest segment (the stream pass's) is read from its start
 * through the sector check and the disc packet rules (mpeg.c, disc mode);
 * each packet goes to the track of its stream (video 0xE0, MPEG audio
 * 0xC0 + n, private stream 1 AC-3 0x80 + n, DTS 0x88 + n, LPCM 0xA0 + n
 * with its 3-byte audio frame header, sub-pictures 0x20 + n):
 *
 * - details (with option details): the video's and each audio stream's
 *   payloads go through FFmpeg's parser and decoder of its codec until one
 *   frame is decoded (at most the first 64 MiB): picture size, aspect,
 *   frame rate, profile and level, the sequence header's bit rate; audio
 *   channels, layout, sample rate, bits and bit rate;
 * - data: a stream none of whose packets is found in the whole segment
 *   has no data: an audio stream is left out of the title, a sub-picture
 *   stream keeps its (empty) track, logged; a title whose video has none
 *   cannot be used. Reading goes on after the details only while a stream
 *   has had no packet yet.
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

#include "libavcodec/avcodec.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"
#include "mpeg.h"

#define DETAILS_LIMIT (64LL << 20)

/* One stream of the title: its key, and for details FFmpeg's parser and
 * decoder. */
typedef struct Probe {
    DiscStreamInfo       *ds;           /* NULL: only whether it has data */
    enum AVCodecID        codec;
    uint16_t              key;
    int                   lpcm;         /* feed the LPCM audio frame header too */
    int                   seen;         /* a packet of it was found */
    int                   done;         /* its details are known (or cannot be) */
    AVCodecParserContext *pc;
    AVCodecContext       *dec;
    AVPacket             *pkt;
    AVFrame              *frame;
} Probe;

typedef struct Details {
    Probe *p;
    int    np, unseen, open;
    int    details_on;                  /* still within the details' read limit */
} Details;

static int probe_open(Probe *p)
{
    const AVCodec *codec = avcodec_find_decoder(p->codec);

    if (!codec)
        return AVERROR_DECODER_NOT_FOUND;
    if (!(p->dec = avcodec_alloc_context3(codec)) || !(p->pkt = av_packet_alloc()) ||
        !(p->frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    p->pc = av_parser_init(p->codec);   /* NULL (LPCM): each payload is a packet */
    p->dec->thread_count = 1;
    return avcodec_open2(p->dec, codec, NULL);
}

static void probe_close(Probe *p)
{
    av_parser_close(p->pc);
    avcodec_free_context(&p->dec);
    av_packet_free(&p->pkt);
    av_frame_free(&p->frame);
}

/* The decoded frame's values into the track's details. */
static void take(Probe *p, const AVFrame *f)
{
    DiscStreamInfo *ds = p->ds;
    AVCodecContext *d = p->dec;

    p->done = 1;
    ds->profile = d->profile;
    ds->level   = d->level;
    if (d->codec_type == AVMEDIA_TYPE_VIDEO) {
        AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : d->sample_aspect_ratio;
        ds->width  = f->width;
        ds->height = f->height;
        if (!sar.num || !sar.den)
            sar = (AVRational){ 1, 1 };
        if (d->codec_id != AV_CODEC_ID_MPEG1VIDEO)  /* MPEG-1: a pel aspect ratio (a pixel shape), no display
                                                       aspect */
            av_reduce(&ds->aspect.num, &ds->aspect.den, (int64_t)f->width * sar.num, (int64_t)f->height * sar.den,
                      1 << 30);
        ds->frame_rate = d->framerate;
        /* MPEG-1 / MPEG-2: the sequence header's (MPEG-2: its maximum) */
        ds->bit_rate   = d->codec_id == AV_CODEC_ID_MPEG2VIDEO ? d->rc_max_rate : d->bit_rate;
    } else {
        ds->channels    = f->ch_layout.nb_channels;
        ds->layout      = f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? f->ch_layout.u.mask : 0;
        ds->sample_rate = f->sample_rate;
        ds->bits        = d->bits_per_raw_sample ? d->bits_per_raw_sample :
                          d->codec_id == AV_CODEC_ID_PCM_DVD ? d->bits_per_coded_sample : 0;
        ds->bit_rate    = d->bit_rate;
    }
}

/* One cut frame (or NULL: the end) to the decoder. */
static int decode(Probe *p, const uint8_t *data, int size)
{
    int ret;

    if (data) {
        av_packet_unref(p->pkt);
        if (av_new_packet(p->pkt, size) < 0)
            return AVERROR(ENOMEM);
        memcpy(p->pkt->data, data, size);
    }
    ret = avcodec_send_packet(p->dec, data ? p->pkt : NULL);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        return 0;                       /* a damaged or partial frame: the next one */
    if (avcodec_receive_frame(p->dec, p->frame) >= 0) {
        take(p, p->frame);
        av_frame_unref(p->frame);
    }
    return 0;
}

static int feed(Probe *p, const uint8_t *data, int size)
{
    while (size > 0 && !p->done) {
        const uint8_t *out = data;
        int n = size, out_size = size, ret;

        if (p->pc) {
            n = av_parser_parse2(p->pc, p->dec, (uint8_t **)&out, &out_size, data, size,
                                 AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (n < 0)
                return 0;
        }
        data += n;
        size -= n;
        if (out_size && (ret = decode(p, out, out_size)) < 0)
            return ret;
    }
    return 0;
}

static int flush(Probe *p)
{
    const uint8_t *out;
    int out_size = 0, ret;

    if (p->done || !p->dec)
        return 0;
    if (p->pc) {
        av_parser_parse2(p->pc, p->dec, (uint8_t **)&out, &out_size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (out_size && (ret = decode(p, out, out_size)) < 0)
            return ret;
    }
    return p->done ? 0 : decode(p, NULL, 0);
}

/* A packet of the sector to its stream's probe. */
static int packet(void *opaque, const uint8_t *sec, const MpegPsDiscPacket *k)
{
    Details *D = opaque;
    int start = k->at + k->payload, end = k->at + k->len, from, ret;

    if (end > MPEGPS_DISC_SECTOR)
        end = MPEGPS_DISC_SECTOR;
    for (int i = 0; i < D->np; i++) {
        Probe *p = &D->p[i];
        if (p->key != (uint16_t)k->key)
            continue;
        if (!p->seen) {
            p->seen = 1;
            D->unseen--;
        }
        if (!D->details_on || p->done || !p->dec)
            continue;
        from = start - (p->lpcm ? k->extra : 0);
        if (from < end && (ret = feed(p, sec + from, end - from)) < 0)
            return ret;
        if (p->done)
            D->open--;
    }
    return 0;
}

/* An audio entry's codec and key by its title set's attributes (0: not known). */
static uint16_t audio_stream(const DVDTitle *t, int i, enum AVCodecID *codec)
{
    const uint8_t *a = t->vts->header + DVD_VTS_AUDIO_ATTR + 8 * t->audio[i].attr;
    int s = t->audio[i].stream;

    switch (a[0] >> 5) {
    case 0:  *codec = AV_CODEC_ID_AC3;     return (0x80 + s) << 8 | 0xbd;
    case 2:
    case 3:  *codec = AV_CODEC_ID_MP2;     return 0xc0 + s;
    case 4:  *codec = AV_CODEC_ID_PCM_DVD; return (0xa0 + s) << 8 | 0xbd;
    case 6:  *codec = AV_CODEC_ID_DTS;     return (0x88 + s) << 8 | 0xbd;
    default: *codec = AV_CODEC_ID_NONE;    return 0;
    }
}

int ff_dvd_title_details(DVDDisc *d, DVDTitle *t, int details)
{
    const DVDSegment *g;
    Details D = { 0 };
    Probe p[1 + 8 + 33];
    int64_t size;
    int ret = 0, seg = 0;
    int64_t best = -1;
    uint8_t buf[DVD_SECTOR];

    memset(p, 0, sizeof(p));
    memset(t->details, 0, sizeof(t->details));
    for (int i = 0; i < FF_ARRAY_ELEMS(t->details); i++) {
        t->details[i].profile = AV_PROFILE_UNKNOWN;
        t->details[i].level   = AV_LEVEL_UNKNOWN;
    }
    if (!t->nb_segments)
        return 0;
    for (int k = 0; k < t->nb_segments; k++)
        if (t->segments[k].size > best) {
            best = t->segments[k].size;
            seg = k;
        }
    g = &t->segments[seg];
    size = (int64_t)ff_dvd_segment_blocks(g) * DVD_SECTOR;

    /* the video, the audio streams the stream pass kept, the sub-pictures */
    p[D.np++] = (Probe){ .ds = &t->details[0], .key = 0xe0,
                         .codec = (t->vts->header[DVD_VTS_VIDEO_ATTR] >> 6) == 0 ? AV_CODEC_ID_MPEG1VIDEO :
                                  AV_CODEC_ID_MPEG2VIDEO };
    for (int i = 0; i < t->nb_audio; i++) {
        enum AVCodecID codec;
        uint16_t key = audio_stream(t, i, &codec);
        if (t->audio_dropped >> i & 1)
            continue;
        p[D.np++] = (Probe){ .ds = &t->details[1 + i], .key = key, .codec = codec,
                             .lpcm = codec == AV_CODEC_ID_PCM_DVD };
    }
    for (int i = 0; i < t->nb_subp; i++)
        p[D.np++] = (Probe){ .key = (0x20 + t->subp[i].stream) << 8 | 0xbd };
    D.p = p;
    for (int i = 0; i < D.np; i++) {
        p[i].done = !details || !p[i].ds;
        if (!p[i].done && (ret = probe_open(&p[i])) < 0) {
            if (ret == AVERROR(ENOMEM))
                goto end;
            av_log(d->log, AV_LOG_VERBOSE, "DVD: title %s: no decoder for %s: its details are not known\n",
                   t->name, avcodec_get_name(p[i].codec));
            probe_close(&p[i]);
            p[i].done = 1;
            ret = 0;
        }
        D.open   += !p[i].done;
        D.unseen++;
    }
    D.details_on = 1;
    for (int64_t pos = 0; pos < size && (D.unseen || (D.details_on && D.open)); pos += DVD_SECTOR) {
        const char *why;
        int at;

        if (D.details_on && pos >= DETAILS_LIMIT) {
            for (int i = 0; i < D.np; i++)
                if ((ret = flush(&p[i])) < 0)
                    goto end;
            D.details_on = 0;
        }
        if ((ret = ff_dvd_segment_read(d, t, seg, pos / DVD_SECTOR, buf)) < 0) {
            av_log(d->log, AV_LOG_ERROR, "DVD: title %s: sector %"PRId64" of segment %d cannot be read: its "
                   "streams' data cannot be found\n", t->name, pos / DVD_SECTOR, seg + 1);
            goto end;
        }
        if (!ret)
            continue;
        if ((ret = ff_mpegps_disc_sector(buf, 0, packet, &D, &why, &at)) < 0)
            goto end;
    }
    ret = 0;
    for (int i = 0; i < D.np && D.details_on; i++)
        if ((ret = flush(&p[i])) < 0)
            goto end;
    for (int i = 0; i < D.np; i++)
        if (p[i].dec && !p[i].done)
            av_log(d->log, AV_LOG_VERBOSE, "DVD: title %s: %s (stream key 0x%04x): no frame decoded in the first "
                   "%d MiB: its details are not known\n", t->name, avcodec_get_name(p[i].codec), p[i].key,
                   (int)(DETAILS_LIMIT >> 20));

    /* the streams without data */
    if (!p[0].seen) {
        av_log(d->log, AV_LOG_ERROR, "DVD: title %s: its segment %d has no video packet: the title cannot be "
               "used\n", t->name, seg + 1);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    for (int i = 0, k = 1; i < t->nb_audio; i++) {
        if (t->audio_dropped >> i & 1)
            continue;
        if (!p[k++].seen) {
            av_log(d->log, AV_LOG_WARNING, "DVD: title %s: audio stream %d has no packet in segment %d: left out\n",
                   t->name, t->audio[i].stream, seg + 1);
            t->audio_dropped |= 1 << i;
        }
    }
    for (int i = 0; i < t->nb_subp; i++)
        if (!p[D.np - t->nb_subp + i].seen)
            av_log(d->log, AV_LOG_WARNING, "DVD: title %s: sub-picture stream %d has no packet in segment %d: its "
                   "track is empty\n", t->name, t->subp[i].stream, seg + 1);
end:
    for (int i = 0; i < D.np; i++)
        probe_close(&p[i]);
    return ret;
}
