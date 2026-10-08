/*
 * Blu-ray: the stream details of a title's tracks for the title list (what
 * a player or MediaInfo shows: picture size, aspect, frame rate, profile and
 * level, Dolby Vision type; audio channels, layout, sample rate, bits, bit
 * rate and kind, such as DTS-HD Master Audio or TrueHD with Atmos).
 *
 * The segment the tracks are described from is read from its start (6144-
 * byte units, decrypted); each track's PES payloads (its PID and PES keys,
 * as the title reader takes them) go through FFmpeg's parser and decoder of
 * its codec until one frame is decoded: its values are the track's. A Dolby
 * Vision title's enhancement layer (PID 0x1015) is decoded too, for its
 * metadata (RPU): profile 7, the level by the base layer's picture rate,
 * and FEL / MEL (a minimal enhancement layer carries no residual: every
 * colour component's non-linear quantization is the identity).
 * Results are kept per clip, PID and keys: titles that share a clip read it
 * once. Reading stops when every track has its details or after 64 MiB.
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

#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "bluray_internal.h"
#include "discinfo.h"

#define UNIT        BLURAY_UNIT
#define PACKET      192
#define READ_LIMIT  (64 << 20)

/* One track's probe: its PID and keys, FFmpeg's parser and decoder. */
typedef struct Probe {
    DiscStreamInfo       *ds;
    enum AVCodecID        codec;
    uint16_t              pid;
    uint16_t              key[2];
    int                   nb_keys;
    int                   el;           /* the Dolby Vision enhancement layer of ds */
    int                   mvc;          /* the 3-D second view: its subset SPS only */
    int                   in_pes;       /* the PID's current PES is one of ours */
    int                   done;
    int                   opened;       /* the decoder is open (VC-1: once its headers were found) */
    uint8_t              *hold;         /* VC-1: the bytes before the decoder could open */
    int                   nb_hold;
    AVCodecParserContext *pc;
    AVCodecContext       *dec;
    AVPacket             *pkt;
    AVFrame              *frame;
} Probe;

/* A finished probe, kept per clip. */
typedef struct Cached {
    uint32_t        clip;
    uint16_t        pid, key[2];
    int             el, mvc;
    DiscStreamInfo  ds;
} Cached;

struct BlurayDetailsCache {
    Cached *c;
    int     n, cap;
};

void ff_bluray_details_cache_free(BlurayDetailsCache **pc)
{
    if (*pc)
        av_freep(&(*pc)->c);
    av_freep(pc);
}

static int ts_payload(const uint8_t *p, uint16_t *pid, int *pusi, const uint8_t **pl, int *len)
{
    int off = 4;

    if (p[0] != 0x47 || (p[1] & 0x80))
        return 0;
    *pusi = p[1] & 0x40;
    *pid  = (p[1] & 0x1f) << 8 | p[2];
    if (p[3] & 0x20) {
        if (p[4] >= 0xb8)
            return 0;
        off = p[4] + 5;
    }
    if (!(p[3] & 0x10))
        return 0;
    *pl  = p + off;
    *len = 188 - off;
    return 1;
}

/* The decoder: allocated, and opened unless it needs the stream's headers
 * first (VC-1 opens with its sequence header and entry point as
 * extradata). */
static int probe_open(Probe *p)
{
    const AVCodec *codec = avcodec_find_decoder(p->codec);

    if (p->mvc)
        return 0;
    if (!codec)
        return AVERROR_DECODER_NOT_FOUND;
    if (!(p->dec = avcodec_alloc_context3(codec)) || !(p->pkt = av_packet_alloc()) ||
        !(p->frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    p->pc = av_parser_init(p->codec);   /* NULL: payloads go to the decoder as they come */
    p->dec->thread_count = 0;
    p->dec->thread_type  = FF_THREAD_SLICE;
    if (p->codec == AV_CODEC_ID_VC1)
        return 0;
    p->opened = 1;
    return avcodec_open2(p->dec, codec, NULL);
}

static void probe_close(Probe *p)
{
    av_parser_close(p->pc);
    p->pc = NULL;
    avcodec_free_context(&p->dec);
    av_packet_free(&p->pkt);
    av_frame_free(&p->frame);
    av_freep(&p->hold);
}

/* Dolby Vision from an enhancement-layer picture's metadata: MEL when every
 * colour component's non-linear quantization is the identity (offset 0, in
 * max 1, no dead zone), else FEL. */
static void dovi_text(DiscStreamInfo *ds, const AVFrame *f)
{
    const AVFrameSideData *sd = av_frame_get_side_data(f, AV_FRAME_DATA_DOVI_METADATA);
    const AVDOVIMetadata *m;
    const AVDOVIRpuDataHeader *h;
    const AVDOVIDataMapping *map;
    int mel = 1;

    if (!sd)
        return;
    m   = (const AVDOVIMetadata *)sd->data;
    h   = av_dovi_get_header(m);
    map = av_dovi_get_mapping(m);
    if (h->disable_residual_flag || h->coef_data_type)
        mel = 0;                        /* (the reference names such pictures FEL too) */
    for (int c = 0; c < 3; c++) {
        const AVDOVINLQParams *q = &map->nlq[c];
        if (q->nlq_offset || q->linear_deadzone_slope || q->linear_deadzone_threshold ||
            q->vdr_in_max != (1ULL << h->coef_log2_denom))
            mel = 0;
    }
    snprintf(ds->dolby_vision, sizeof(ds->dolby_vision), "BL+%s+RPU", mel ? "MEL" : "FEL");
}

/* The decoded frame's values into the track's details. */
static void take(Probe *p, const AVFrame *f)
{
    DiscStreamInfo *ds = p->ds;
    AVCodecContext *d = p->dec;

    p->done = 1;
    if (p->el) {
        dovi_text(ds, f);
        return;
    }
    ds->profile = d->profile;
    ds->level   = d->level;
    if (d->codec_type == AVMEDIA_TYPE_VIDEO) {
        AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : d->sample_aspect_ratio;
        ds->width  = f->width;
        ds->height = f->height;
        if (!sar.num || !sar.den)
            sar = (AVRational){ 1, 1 };
        av_reduce(&ds->aspect.num, &ds->aspect.den, (int64_t)f->width * sar.num, (int64_t)f->height * sar.den,
                  1 << 30);
        ds->frame_rate = d->framerate;
        /* MPEG-1 / MPEG-2: the sequence header's (MPEG-2: its maximum) */
        ds->bit_rate   = d->codec_id == AV_CODEC_ID_MPEG2VIDEO ? d->rc_max_rate :
                         d->codec_id == AV_CODEC_ID_MPEG1VIDEO ? d->bit_rate : 0;
    } else {
        ds->channels    = f->ch_layout.nb_channels;
        ds->layout      = f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? f->ch_layout.u.mask : 0;
        ds->sample_rate = f->sample_rate;
        ds->bits        = d->bits_per_raw_sample ? d->bits_per_raw_sample :
                          d->codec_id == AV_CODEC_ID_PCM_BLURAY ? d->bits_per_coded_sample : 0;
        ds->bit_rate    = d->bit_rate;
    }
}

/* One cut frame (or NULL: the end) to the decoder; a decoded frame's
 * values are taken. */
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

/* The 3-D second view: its subset SPS (nal_unit_type 15) gives the stereo
 * profile and level (its first bytes: profile_idc, constraint flags,
 * level_idc). */
static void mvc_bytes(Probe *p, const uint8_t *data, int size)
{
    for (int i = 0; i + 6 < size; i++)
        if (!data[i] && !data[i + 1] && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 15) {
            p->ds->profile = data[i + 4];
            p->ds->level   = data[i + 6];
            p->done = 1;
            return;
        }
}

/* VC-1: the decoder opens with the sequence header and entry point (start
 * codes 0x0F, 0x0E up to the first frame 0x0D) as extradata; the bytes held
 * until then follow. */
static int vc1_open(Probe *p, const uint8_t *data, int size)
{
    uint8_t *h;
    int seq = -1, frame = -1, ret;

    if (p->nb_hold + size > (4 << 20)) {
        p->done = 1;                    /* no sequence header in 4 MiB */
        return 0;
    }
    if (!(h = av_realloc(p->hold, p->nb_hold + size)))
        return AVERROR(ENOMEM);
    p->hold = h;
    memcpy(p->hold + p->nb_hold, data, size);
    p->nb_hold += size;
    for (int i = 0; i + 3 < p->nb_hold; i++)
        if (!h[i] && !h[i + 1] && h[i + 2] == 1) {
            if (h[i + 3] == 0x0f && seq < 0)
                seq = i;
            else if (h[i + 3] == 0x0d && seq >= 0) {
                frame = i;
                break;
            }
        }
    if (frame < 0)
        return 0;
    if (!(p->dec->extradata = av_mallocz(frame - seq + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);
    memcpy(p->dec->extradata, h + seq, frame - seq);
    p->dec->extradata_size = frame - seq;
    if ((ret = avcodec_open2(p->dec, p->dec->codec, NULL)) < 0) {
        p->done = 1;
        return 0;
    }
    p->opened = 1;
    return 0;
}

static int parse(Probe *p, const uint8_t *data, int size);

/* Bytes of one of the track's PES payloads through parser and decoder. */
static int feed(Probe *p, const uint8_t *data, int size)
{
    if (p->mvc) {
        mvc_bytes(p, data, size);
        return 0;
    }
    if (!p->opened) {
        int ret = vc1_open(p, data, size);
        if (ret < 0 || !p->opened)
            return ret;
        data = p->hold;
        size = p->nb_hold;
    }
    return parse(p, data, size);
}

static int parse(Probe *p, const uint8_t *data, int size)
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

/* The end of what is read: the parser's last frame and the decoder's
 * delayed one. */
static int flush(Probe *p)
{
    const uint8_t *out;
    int out_size = 0, ret;

    if (p->done || !p->opened || p->mvc)
        return 0;
    if (p->pc) {
        av_parser_parse2(p->pc, p->dec, (uint8_t **)&out, &out_size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (out_size && (ret = decode(p, out, out_size)) < 0)
            return ret;
    }
    return p->done ? 0 : decode(p, NULL, 0);
}

/* A PES header's stream key (stream_id, or 0xFD | extension << 8) and its
 * header length; < 0 when it is not a usable header. */
static int pes_head(const uint8_t *p, int len, uint16_t *key)
{
    uint8_t flags;
    int hl;

    if (len < 9 || p[0] || p[1] || p[2] != 1)
        return -1;
    *key  = p[3];
    flags = p[7];
    hl    = 9 + p[8];
    if (hl > len)
        return -1;
    if (p[3] == 0xfd && (flags & 1)) {
        int o = 9 + 5 * ((flags >> 7) & 1) + 5 * ((flags >> 6) & 1) + ((flags & 0x20) ? 6 : 0) +
                ((flags & 0x10) ? 3 : 0) + ((flags & 0x08) ? 1 : 0) + ((flags & 0x04) ? 1 : 0) +
                ((flags & 0x02) ? 2 : 0);
        if (o < hl && (p[o] & 1)) {
            int q = o + 1 + ((p[o] & 0x80) ? 16 : 0) + ((p[o] & 0x40) ? 1 : 0) + ((p[o] & 0x20) ? 2 : 0) +
                    ((p[o] & 0x10) ? 2 : 0);
            if (q + 1 < hl && (p[q] & 0x7f) >= 1 && !(p[q + 1] & 0x80))
                *key = p[q + 1] << 8 | 0xfd;
        }
    }
    return hl;
}

static int wants(const Probe *p, uint16_t key)
{
    for (int i = 0; i < p->nb_keys; i++)
        if (!p->key[i] || p->key[i] == key)
            return 1;
    return 0;
}

static Cached *cache_find(BlurayDetailsCache *c, uint32_t clip, const Probe *p)
{
    for (int i = 0; c && i < c->n; i++)
        if (c->c[i].clip == clip && c->c[i].pid == p->pid && c->c[i].el == p->el && c->c[i].mvc == p->mvc &&
            c->c[i].key[0] == p->key[0] && c->c[i].key[1] == p->key[1])
            return &c->c[i];
    return NULL;
}

static int cache_add(BlurayDetailsCache *c, uint32_t clip, const Probe *p)
{
    if (c->n == c->cap) {
        int cap = c->cap ? 2 * c->cap : 64;
        Cached *n = av_realloc_array(c->c, cap, sizeof(*n));
        if (!n)
            return AVERROR(ENOMEM);
        c->c   = n;
        c->cap = cap;
    }
    c->c[c->n++] = (Cached){ clip, p->pid, { p->key[0], p->key[1] }, p->el, p->mvc, *p->ds };
    return 0;
}

/* What of a cached result belongs to the probe's stream (the enhancement
 * layer gives only the Dolby Vision type). */
static void cache_take(const Cached *k, Probe *p)
{
    if (p->mvc) {
        p->ds->profile = k->ds.profile;
        p->ds->level   = k->ds.level;
        return;
    }
    if (p->el) {
        memcpy(p->ds->dolby_vision, k->ds.dolby_vision, sizeof(p->ds->dolby_vision));
        return;
    }
    p->ds->profile     = k->ds.profile;
    p->ds->level       = k->ds.level;
    p->ds->aspect      = k->ds.aspect;
    p->ds->frame_rate  = k->ds.frame_rate;
    p->ds->channels    = k->ds.channels;
    p->ds->layout      = k->ds.layout;
    p->ds->sample_rate = k->ds.sample_rate;
    p->ds->bits        = k->ds.bits;
    p->ds->bit_rate    = k->ds.bit_rate;
    if (p->ds->type == AVMEDIA_TYPE_VIDEO) {
        p->ds->width  = k->ds.width;
        p->ds->height = k->ds.height;
    }
}

/* Dolby Vision's level by the base layer's picture rate (width x height x
 * frames per second), the levels of the profile 7 / 8 definition. */
static int dovi_level(const DiscStreamInfo *ds)
{
    static const int64_t max_rate[] = { 22118400, 27648000, 49766400, 62208000, 124416000, 199065600,
                                        248832000, 398131200, 497664000 };
    int64_t fps = ds->frame_rate.den ? (ds->frame_rate.num + ds->frame_rate.den / 2) / ds->frame_rate.den : 0;
    int64_t rate = (int64_t)ds->width * ds->height * fps;

    for (int i = 0; i < FF_ARRAY_ELEMS(max_rate); i++)
        if (rate <= max_rate[i])
            return i + 1;
    return 0;
}

int ff_bluray_title_details(void *log, DiscIOFS *fs, BlurayAACS *aacs, const BlurayTitle *t,
                            DiscStreamInfo *const *ds, BlurayDetailsCache **pcache)
{
    const BluraySegment *seg = &t->segs[t->track_seg];
    const BlurayClip *c0 = seg->items[0].clip;
    int bdav = c0->hdr.type == MKBETAG('M', '2', 'T', 'S'), np = 0, open = 0, ret = 0, mvc = 0;
    Probe *p = av_calloc(2 * t->nb_tracks + 1, sizeof(*p));
    uint8_t *cached = av_calloc(2 * t->nb_tracks + 1, 1);
    BluraySegReader u;
    uint8_t buf[UNIT];

    if (!p || !cached || (!*pcache && !(*pcache = av_mallocz(sizeof(**pcache))))) {
        av_free(p);
        av_free(cached);
        return AVERROR(ENOMEM);
    }
    for (int i = 0; i < t->nb_tracks; i++) {
        const BlurayTrack *tk = &t->tracks[i];
        uint16_t key[2] = { 0 };
        if (tk->type != AVMEDIA_TYPE_VIDEO && tk->type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (tk->type == AVMEDIA_TYPE_VIDEO && tk->from_track >= 0) {
            /* the 3-D second view: its own PID (sel1), read with both views */
            p[np++] = (Probe){ .ds = ds[i], .codec = tk->codec, .pid = tk->sel1, .mvc = 1, .key = { 0xe0 },
                               .nb_keys = 1 };
            mvc = seg->interleaved;
            continue;
        }
        p[np] = (Probe){ .ds = ds[i], .codec = tk->codec, .pid = tk->pid };
        p[np].nb_keys = ff_bluray_track_keys(tk, bdav, key);
        memcpy(p[np].key, key, sizeof(key));
        np++;
        if (tk->flags & BLURAY_TRACK_LAYER2)
            p[np++] = (Probe){ .ds = ds[i], .codec = AV_CODEC_ID_HEVC, .pid = tk->sel1, .el = 1, .key = { 0xe1 },
                               .nb_keys = 1 };
    }
    /* what titles before read already; a codec FFmpeg cannot open stays
     * without details */
    for (int i = 0; i < np; i++) {
        const Cached *k = cache_find(*pcache, c0->number, &p[i]);
        if (k) {
            cache_take(k, &p[i]);
            p[i].done = cached[i] = 1;
        } else if ((ret = probe_open(&p[i])) < 0) {
            if (ret == AVERROR(ENOMEM))
                goto end;
            av_log(log, AV_LOG_VERBOSE, "Blu-ray: title %s: PID 0x%04x (%s): no decoder: its details are not "
                   "known\n", t->name, p[i].pid, avcodec_get_name(p[i].codec));
            p[i].done = 1;
            ret = 0;
        }
    }
    for (int i = 0; i < np; i++)
        open += !p[i].done;
    ff_bluray_seg_reader_init(&u, log, fs, aacs, seg);
    if (mvc)
        ff_bluray_seg_reader_both(&u);
    for (int64_t pos = 0; open && pos < READ_LIMIT; pos += UNIT) {
        if ((ret = ff_bluray_seg_read(&u, pos, buf)) == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0)
            break;
        for (int k = 0; k < UNIT / PACKET; k++) {
            const uint8_t *pl;
            uint16_t pid, key = 0;
            int pusi, len, hl = 0;
            if (!ts_payload(buf + k * PACKET + 4, &pid, &pusi, &pl, &len))
                continue;
            if (pusi)
                hl = pes_head(pl, len, &key);
            for (int i = 0; i < np; i++) {
                if (p[i].pid != pid || p[i].done)
                    continue;
                if (pusi)
                    p[i].in_pes = hl >= 0 && wants(&p[i], key);
                if (p[i].in_pes && (ret = feed(&p[i], pl + (pusi ? hl : 0), len - (pusi ? hl : 0))) < 0)
                    goto close;
                if (p[i].done)
                    open--;
            }
        }
    }
    /* a short clip: its last frames */
    for (int i = 0; i < np && ret >= 0; i++)
        ret = flush(&p[i]);
close:
    ff_bluray_seg_reader_close(&u);
    /* the 3-D second view shows the base view's picture size and rate */
    for (int i = 0; i < t->nb_tracks; i++) {
        const BlurayTrack *tk = &t->tracks[i];
        if (tk->type == AVMEDIA_TYPE_VIDEO && tk->from_track >= 0) {
            const DiscStreamInfo *b = ds[tk->from_track];
            ds[i]->width      = b->width;
            ds[i]->height     = b->height;
            ds[i]->aspect     = b->aspect;
            ds[i]->frame_rate = b->frame_rate;
        }
    }
    for (int i = 0; i < np && ret >= 0; i++)
        if (p[i].done && !cached[i] && (p[i].opened || p[i].mvc))
            ret = cache_add(*pcache, c0->number, &p[i]);
    for (int i = 0; i < np; i++)
        if (!p[i].done)
            av_log(log, AV_LOG_VERBOSE, "Blu-ray: title %s: PID 0x%04x%s: no frame decoded in the first %d MiB: "
                   "its details are not known\n", t->name, p[i].pid, p[i].el ? " (Dolby Vision layer)" : "",
                   READ_LIMIT >> 20);
    /* the Dolby Vision type in full: profile 7 (dual layer, HEVC base
     * layer), level by the picture rate */
    for (int i = 0; i < np; i++)
        if (p[i].el && p[i].ds->dolby_vision[0] == 'B') {
            char type[16];
            av_strlcpy(type, p[i].ds->dolby_vision, sizeof(type));
            snprintf(p[i].ds->dolby_vision, sizeof(p[i].ds->dolby_vision), "dvhe.07.%02d %s",
                     dovi_level(p[i].ds), type);
        }
end:
    for (int i = 0; i < np; i++)
        probe_close(&p[i]);
    av_free(p);
    av_free(cached);
    return ret;
}
