/*
 * The stream pass of a title (the disc layer), the same for every format:
 * the title's tracks (DiscTrack, from its format part) checked against the
 * stream data of its largest segment, read the way the title reader reads
 * (the format part's block reader, FFmpeg's MPEG-PS or MPEG-TS demuxer in
 * disc mode, each payload to the track of its id or PID and keys).
 *
 * - Duplicates: an audio or subtitle track with the same codec, the same
 *   data (id, or PID and keys) and the same kind (taken out of another
 *   track or not, forced subtitles or not) as one before it is left out.
 * - Details (when asked for): each video and audio track's payloads go
 *   through FFmpeg's parser and decoder of its codec until one frame is
 *   decoded, within the first 64 MiB: picture size, aspect, frame rate,
 *   profile and level, the MPEG-1/2 sequence header's bit rate; audio
 *   channels, layout, sample rate, bits and bit rate. VC-1 opens its
 *   decoder with its sequence header and entry point; a 3-D second view
 *   gives its stereo profile and level (its subset SPS) and shows the base
 *   view's picture; a Dolby Vision enhancement layer gives the type (FEL /
 *   MEL from its metadata, the level by the base layer's picture rate). A
 *   caption track carried in the video has the video's bit rate.
 * - Data: reading goes on past the details while a video or audio track has
 *   had no packet (to the segment's end: a stream that never shows up is
 *   known to have none only there), and, when the format asks for it, a
 *   subtitle track too. A title whose video has no data cannot be used; an
 *   audio track without data is left out; a subtitle track without data
 *   keeps its (empty) track (a subtitle stream is described without data).
 * Results are kept per segment and track data: titles that share a segment
 * read it once.
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
#include "libavutil/avstring.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"

#include "avformat.h"
#include "disc.h"
#include "disctl.h"

#define DETAILS_LIMIT (64LL << 20)
#define DISC_BLOCK    2048
#define TS_UNIT       6144

enum { P_TRACK, P_MVC, P_LAYER };       /* a track's own data, a 3-D second view, a Dolby Vision layer */

/* One probe: a track's data (or its Dolby Vision enhancement layer), for
 * details FFmpeg's parser and decoder. */
typedef struct Probe {
    DiscTrack            *k;
    int                   kind;         /* P_* */
    int                   ps_id;        /* program stream: the id */
    DiscTsFeed            feed;         /* transport stream: PID and keys */
    enum AVCodecID        codec;
    const DRCodec        *tc;           /* its codec table entry, or NULL */
    int                   seen;         /* a packet of it was read */
    int                   done;         /* its details are known (or cannot be) */
    int                   opened;       /* the decoder is open (once its headers were found, when it needs them) */
    int                   cached;       /* taken from the cache */
    uint8_t              *hold;         /* the bytes before the decoder could open */
    int                   nb_hold;
    AVCodecParserContext *pc;
    AVCodecContext       *dec;
    AVPacket             *pkt;
    AVFrame              *frame;
} Probe;

/* A finished probe, kept per segment and data. */
typedef struct Cached {
    uint64_t       seg;
    int            kind, ps_id, details, seen;
    DiscTsFeed     feed;
    DiscStreamInfo ds;
} Cached;

struct DiscPassCache {
    Cached *c;
    int     n, cap;
};

void ff_disc_pass_cache_free(DiscPassCache **pc)
{
    if (*pc)
        av_freep(&(*pc)->c);
    av_freep(pc);
}

static int same_feed(const DiscTsFeed *a, const DiscTsFeed *b)
{
    return a->pid == b->pid && a->nb_keys == b->nb_keys && a->key[0] == b->key[0] && a->key[1] == b->key[1];
}

static Cached *cache_find(DiscPassCache *c, uint64_t seg, const Probe *p, int details)
{
    for (int i = 0; seg && c && i < c->n; i++)
        if (c->c[i].seg == seg && c->c[i].kind == p->kind && c->c[i].ps_id == p->ps_id &&
            same_feed(&c->c[i].feed, &p->feed) && (c->c[i].details || !details))
            return &c->c[i];
    return NULL;
}

static int cache_add(DiscPassCache *c, uint64_t seg, const Probe *p, int details)
{
    if (c->n == c->cap) {
        int cap = c->cap ? 2 * c->cap : 64;
        Cached *n = av_realloc_array(c->c, cap, sizeof(*n));
        if (!n)
            return AVERROR(ENOMEM);
        c->c   = n;
        c->cap = cap;
    }
    c->c[c->n++] = (Cached){ seg, p->kind, p->ps_id, details, p->seen, p->feed, p->k->info };
    return 0;
}

/* What of a cached result belongs to the probe's track. */
static void cache_take(const Cached *k, Probe *p, int details)
{
    DiscStreamInfo *ds = &p->k->info;

    p->seen = k->seen;
    if (!details)
        return;
    if (p->kind == P_LAYER) {
        memcpy(ds->dolby_vision, k->ds.dolby_vision, sizeof(ds->dolby_vision));
        return;
    }
    ds->profile = k->ds.profile;
    ds->level   = k->ds.level;
    if (p->kind == P_MVC)
        return;
    ds->aspect      = k->ds.aspect;
    ds->frame_rate  = k->ds.frame_rate;
    ds->channels    = k->ds.channels;
    ds->layout      = k->ds.layout;
    ds->sample_rate = k->ds.sample_rate;
    ds->bits        = k->ds.bits;
    ds->bit_rate    = k->ds.bit_rate;
    if (ds->type == AVMEDIA_TYPE_VIDEO) {
        ds->width    = k->ds.width;
        ds->height   = k->ds.height;
        ds->max_cll  = k->ds.max_cll;
        ds->max_fall = k->ds.max_fall;
        memcpy(ds->hdr, k->ds.hdr, sizeof(ds->hdr));
        memcpy(ds->colour, k->ds.colour, sizeof(ds->colour));
        memcpy(ds->mastering, k->ds.mastering, sizeof(ds->mastering));
    }
}

/* ---- details: FFmpeg's parser and decoder ---- */

/* The decoder: allocated, and opened unless it needs the stream's headers
 * first (the codec table: VC-1 opens with its sequence header and entry
 * point as extradata). */
static int probe_open(Probe *p)
{
    const AVCodec *codec = avcodec_find_decoder(p->codec);

    if (p->kind == P_MVC)
        return 0;
    if (!codec)
        return AVERROR_DECODER_NOT_FOUND;
    if (!(p->dec = avcodec_alloc_context3(codec)) || !(p->pkt = av_packet_alloc()) ||
        !(p->frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    p->pc = av_parser_init(p->codec);   /* NULL (LPCM): each payload is a packet */
    p->dec->thread_count = 1;           /* one frame: no threads (and none of their warnings) */
    if (p->tc && p->tc->decoder_headers)
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

/* HDR from a decoded picture: its colour description (transfer PQ:
 * HDR10, HDR10+ with its dynamic metadata; HLG; else SDR when stated), the
 * mastering display (SMPTE ST 2086) and content light level side data. */
static void hdr_text(DiscStreamInfo *ds, const AVFrame *f)
{
    const AVFrameSideData *md = av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    const AVFrameSideData *cl = av_frame_get_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    int stated = f->color_primaries != AVCOL_PRI_UNSPECIFIED || f->color_trc != AVCOL_TRC_UNSPECIFIED ||
                 f->colorspace != AVCOL_SPC_UNSPECIFIED;

    if (f->color_trc == AVCOL_TRC_SMPTE2084)
        av_strlcpy(ds->hdr, av_frame_get_side_data(f, AV_FRAME_DATA_DYNAMIC_HDR_PLUS) ? "HDR10+" : "HDR10",
                   sizeof(ds->hdr));
    else if (f->color_trc == AVCOL_TRC_ARIB_STD_B67)
        av_strlcpy(ds->hdr, "HLG", sizeof(ds->hdr));
    else if (stated)
        av_strlcpy(ds->hdr, "SDR", sizeof(ds->hdr));
    if (stated)
        snprintf(ds->colour, sizeof(ds->colour), "%s / %s / %s, %s range",
                 av_color_primaries_name(f->color_primaries) ? av_color_primaries_name(f->color_primaries) : "?",
                 av_color_transfer_name(f->color_trc) ? av_color_transfer_name(f->color_trc) : "?",
                 av_color_space_name(f->colorspace) ? av_color_space_name(f->colorspace) : "?",
                 f->color_range == AVCOL_RANGE_JPEG ? "full" : "limited");
    if (md) {
        const AVMasteringDisplayMetadata *m = (const AVMasteringDisplayMetadata *)md->data;
        if (m->has_luminance)
            snprintf(ds->mastering, sizeof(ds->mastering), "luminance %.4f - %.0f cd/m2",
                     av_q2d(m->min_luminance), av_q2d(m->max_luminance));
        if (m->has_primaries)
            snprintf(ds->mastering + strlen(ds->mastering), sizeof(ds->mastering) - strlen(ds->mastering),
                     "%sprimaries R %.3f,%.3f G %.3f,%.3f B %.3f,%.3f white %.4f,%.4f", *ds->mastering ? ", " : "",
                     av_q2d(m->display_primaries[0][0]), av_q2d(m->display_primaries[0][1]),
                     av_q2d(m->display_primaries[1][0]), av_q2d(m->display_primaries[1][1]),
                     av_q2d(m->display_primaries[2][0]), av_q2d(m->display_primaries[2][1]),
                     av_q2d(m->white_point[0]), av_q2d(m->white_point[1]));
    }
    if (cl) {
        const AVContentLightMetadata *c = (const AVContentLightMetadata *)cl->data;
        ds->max_cll  = c->MaxCLL;
        ds->max_fall = c->MaxFALL;
    }
}

/* The decoded frame's values into the track's details. */
static void take(Probe *p, const AVFrame *f)
{
    DiscStreamInfo *ds = &p->k->info;
    AVCodecContext *d = p->dec;

    p->done = 1;
    if (p->kind == P_LAYER) {
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
        if (!p->tc || !p->tc->no_display_aspect)
            av_reduce(&ds->aspect.num, &ds->aspect.den, (int64_t)f->width * sar.num, (int64_t)f->height * sar.den,
                      1 << 30);
        ds->frame_rate = d->framerate;
        hdr_text(ds, f);
        /* the sequence header's (MPEG-2: its maximum), else none */
        ds->bit_rate   = !p->tc ? 0 : p->tc->video_bit_rate == DR_RATE_MAX ? d->rc_max_rate :
                         p->tc->video_bit_rate == DR_RATE_STATED ? d->bit_rate : 0;
    } else {
        ds->channels    = f->ch_layout.nb_channels;
        ds->layout      = f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? f->ch_layout.u.mask : 0;
        ds->sample_rate = f->sample_rate;
        ds->bits        = d->bits_per_raw_sample ? d->bits_per_raw_sample :
                          p->tc && p->tc->bits_coded ? d->bits_per_coded_sample : 0;
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
            p->k->info.profile = data[i + 4];
            p->k->info.level   = data[i + 6];
            p->done = 1;
            return;
        }
}

/* A decoder that opens with the stream's headers as extradata (the codec
 * table's decoder_headers): the bytes held until they are found, which
 * then follow. */
static int headers_open(Probe *p, const uint8_t *data, int size)
{
    uint8_t *h;
    int start, end, ret;

    if (p->nb_hold + size > (4 << 20)) {
        p->done = 1;                    /* no headers in 4 MiB */
        return 0;
    }
    if (!(h = av_realloc(p->hold, p->nb_hold + size)))
        return AVERROR(ENOMEM);
    p->hold = h;
    memcpy(p->hold + p->nb_hold, data, size);
    p->nb_hold += size;
    if (!p->tc->decoder_headers(h, p->nb_hold, &start, &end))
        return 0;
    if (!(p->dec->extradata = av_mallocz(end - start + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);
    memcpy(p->dec->extradata, h + start, end - start);
    p->dec->extradata_size = end - start;
    if ((ret = avcodec_open2(p->dec, p->dec->codec, NULL)) < 0) {
        p->done = 1;
        return 0;
    }
    p->opened = 1;
    return 0;
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

/* A payload of the track through parser and decoder. */
static int feed(Probe *p, const uint8_t *data, int size)
{
    if (p->kind == P_MVC) {
        mvc_bytes(p, data, size);
        return 0;
    }
    if (!p->opened) {
        int ret = headers_open(p, data, size);
        if (ret < 0 || !p->opened)
            return ret;
        data = p->hold;
        size = p->nb_hold;
    }
    return parse(p, data, size);
}

/* The end of what is read for details: the parser's last frame and the
 * decoder's delayed one. */
static int flush(Probe *p)
{
    const uint8_t *out;
    int out_size = 0, ret;

    if (p->done || !p->opened || p->kind == P_MVC)
        return 0;
    if (p->pc) {
        av_parser_parse2(p->pc, p->dec, (uint8_t **)&out, &out_size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (out_size && (ret = decode(p, out, out_size)) < 0)
            return ret;
    }
    return p->done ? 0 : decode(p, NULL, 0);
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

/* Whether a track's stream must have data to be described (the codec
 * table; a codec without an entry: video and audio do, subtitles not). The
 * title's video is required apart (its title cannot be used without). */
static int needs_data(const Probe *p)
{
    return p->tc ? !p->tc->info_without_data : p->k->kind != DR_KIND_SUBTITLE;
}

/* ---- the tracks ---- */

/* Track i left out: the tracks after it move up, links to them follow
 * (a link to it is cut). */
static void track_remove(DiscTrack *tracks, int *n, int i)
{
    memmove(tracks + i, tracks + i + 1, (*n - i - 1) * sizeof(*tracks));
    (*n)--;
    for (int j = 0; j < *n; j++) {
        DiscStreamInfo *k = &tracks[j].info;
        k->from_track  = k->from_track  == i ? -1 : k->from_track  > i ? k->from_track  - 1 : k->from_track;
        k->inner_track = k->inner_track == i ? -1 : k->inner_track > i ? k->inner_track - 1 : k->inner_track;
    }
}

static int same_data(const DiscTrack *a, const DiscTrack *b, int ts)
{
    return ts ? same_feed(&a->feed, &b->feed) : a->id == b->id;
}

/* The duplicates: an audio or subtitle track like one before it (each
 * judged as described, before any is taken out; dup: room for n). */
static void duplicates(AVFormatContext *s, const char *fmt, DiscTrack *tracks, int *n, int ts, int *dup)
{
    int nb = 0;

    for (int i = 1; i < *n; i++) {
        const DiscTrack *k = &tracks[i];
        if ((k->kind != DR_KIND_AUDIO && k->kind != DR_KIND_SUBTITLE) || (k->flags & DISC_TRACK_SIDE))
            continue;
        for (int j = 1; j < i; j++) {
            const DiscTrack *o = &tracks[j];
            if (o->kind == k->kind && !(o->flags & DISC_TRACK_SIDE) && o->info.codec == k->info.codec &&
                same_data(o, k, ts) && o->info.forced == k->info.forced &&
                (o->info.from_track >= 0) == (k->info.from_track >= 0)) {
                av_log(s, AV_LOG_WARNING, "%s: %s stream #%d is identical to stream #%d and was skipped\n", fmt,
                       k->kind == DR_KIND_AUDIO ? "Audio" : "Subtitle", k->number, o->number);
                dup[nb++] = i;
                break;
            }
        }
    }
    while (nb--)
        track_remove(tracks, n, dup[nb]);
}

/* ---- reading ---- */

typedef struct Reader {
    const DiscPassSource *src;
    uint32_t              block;        /* the next block */
    int                   bs;
    int                   err;          /* a block could not be read */
} Reader;

static int pass_read(void *opaque, uint8_t *buf, int size)
{
    Reader *r = opaque;
    int ret;

    if (size < r->bs)
        return AVERROR(EINVAL);
    for (;;) {
        ret = r->src->read(r->src->opaque, r->block, buf);
        if (ret == AVERROR_EOF)
            return ret;
        if (ret < 0) {
            r->err = ret;
            return ret;
        }
        r->block++;
        if (ret)
            return r->bs;
    }
}

static int probe_wants(const Probe *p, int ts, int id)
{
    uint16_t key = (uint32_t)id >> 16;

    if (!ts)
        return p->ps_id == id;
    if (p->feed.pid != (id & 0xffff))
        return 0;
    if (!p->feed.nb_keys || !p->feed.key[0])
        return 1;
    for (int j = 0; j < p->feed.nb_keys; j++)
        if (p->feed.key[j] == key)
            return 1;
    return 0;
}

int ff_disc_stream_pass(AVFormatContext *s, const char *title, const DiscPassSource *src, DiscTrack *tracks,
                        int *nb_tracks, int details)
{
    DiscContext *d = s->priv_data;
    const char *fmt = d->fmt->log_name;
    Probe *p;
    int np = 0, ret = 0, open = 0, unseen = 0, *pids = NULL, nb_pids = 0, *drop = NULL, nb_drop = 0;
    Reader r = { src, 0, src->ts ? TS_UNIT : DISC_BLOCK, 0 };
    DiscDemux dm = { 0 };
    AVPacket *pkt = NULL;

    if (!(drop = av_calloc(*nb_tracks + 1, sizeof(*drop)))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    duplicates(s, fmt, tracks, nb_tracks, src->ts, drop);
    if (!(p = av_calloc(2 * *nb_tracks + 1, sizeof(*p))) ||
        (src->ts && !(pids = av_calloc(2 * *nb_tracks + 1, sizeof(*pids)))) ||
        (src->key && !d->pass_cache && !(d->pass_cache = av_mallocz(sizeof(*d->pass_cache))))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    /* the probes: every track with data of its own (not a caption track in
     * the video, not a subtitle track from a clip of its own), a Dolby
     * Vision layer */
    for (int i = 0; i < *nb_tracks; i++) {
        DiscTrack *k = &tracks[i];
        if (k->kind == DR_KIND_CAPTION || (k->flags & DISC_TRACK_SIDE))
            continue;
        p[np++] = (Probe){ .k = k, .kind = (k->flags & DISC_TRACK_SECOND_VIEW) ? P_MVC : P_TRACK, .ps_id = k->id,
                           .feed = k->feed, .codec = k->info.codec, .tc = ff_disctl_codec(k->info.codec) };
        if (k->flags & DISC_TRACK_LAYER2)
            p[np++] = (Probe){ .k = k, .kind = P_LAYER, .ps_id = -1, .feed = k->layer_feed,
                               .codec = AV_CODEC_ID_HEVC, .tc = ff_disctl_codec(AV_CODEC_ID_HEVC) };
    }
    /* what titles before found already; a codec FFmpeg cannot open stays
     * without details */
    for (int i = 0; i < np; i++) {
        const Cached *c = cache_find(d->pass_cache, src->key, &p[i], details);
        int want = details && (p[i].k->kind == DR_KIND_VIDEO || p[i].k->kind == DR_KIND_AUDIO);
        if (c) {
            cache_take(c, &p[i], details);
            p[i].done = p[i].cached = 1;
            continue;
        }
        /* LPCM with another header than DVD-Video's 3 bytes (HD DVD's 5):
         * FFmpeg's decoder reads DVD-Video's */
        if (want && p[i].codec == AV_CODEC_ID_PCM_DVD && src->lpcm_header != 3) {
            av_log(s, AV_LOG_VERBOSE, "%s: title %s: LPCM with a %d-byte header: its details are not read "
                   "[untested on real discs]\n", fmt, title, src->lpcm_header);
            want = 0;
        }
        p[i].done = !want;
        if (want && (ret = probe_open(&p[i])) < 0) {
            if (ret == AVERROR(ENOMEM))
                goto end;
            av_log(s, AV_LOG_VERBOSE, "%s: title %s: %s: no decoder: its details are not known\n", fmt, title,
                   avcodec_get_name(p[i].codec));
            p[i].done = 1;
            ret = 0;
        }
        open   += !p[i].done;
        unseen += p[i].kind == P_TRACK && !p[i].seen;
    }
    if (open || unseen) {
        int details_on = 1;

        if (src->ts) {
            for (int i = 0; i < np; i++) {
                int have = 0;
                for (int j = 0; j < nb_pids && !have; j++)
                    have = pids[j] == p[i].feed.pid;
                if (!have)
                    pids[nb_pids++] = p[i].feed.pid;
            }
            ret = ff_disc_ts_open(&dm, s, pids, nb_pids, pass_read, &r);
        } else {
            ret = ff_disc_ps_open(&dm, s, src->lpcm_header, pass_read, &r);
        }
        if (ret < 0 || !(pkt = av_packet_alloc())) {
            ret = ret < 0 ? ret : AVERROR(ENOMEM);
            goto end;
        }
        for (;;) {
            int more = 0;
            const AVStream *sub;

            /* read on: details still open (within the limit); a track
             * that needs data to be described (or, when asked, any
             * track) without a packet yet */
            for (int i = 0; i < np && !more; i++)
                more = p[i].kind == P_TRACK && !p[i].seen && !p[i].cached && (needs_data(&p[i]) || src->read_all);
            if (details_on && (int64_t)r.block * r.bs >= DETAILS_LIMIT) {
                for (int i = 0; i < np; i++)
                    if ((ret = flush(&p[i])) < 0)
                        goto end;
                details_on = 0;
            }
            if (!more && !(details_on && open))
                break;
            if ((ret = av_read_frame(dm.ctx, pkt)) < 0)
                break;
            sub = dm.ctx->streams[pkt->stream_index];
            for (int i = 0; i < np; i++) {
                if (!probe_wants(&p[i], src->ts, sub->id))
                    continue;
                p[i].seen = 1;
                if (!details_on || p[i].done)
                    continue;
                if ((ret = feed(&p[i], pkt->data, pkt->size)) < 0)
                    goto end;
                open -= p[i].done;
            }
            av_packet_unref(pkt);
        }
        if (r.err) {
            av_log(s, AV_LOG_ERROR, "%s: title %s: block %"PRIu32" of segment %d cannot be read: its streams' data "
                   "cannot be found\n", fmt, title, r.block, src->index + 1);
            ret = r.err;
            goto end;
        }
        if (ret != AVERROR_EOF && ret < 0)
            goto end;
        ret = 0;
        /* the end of a short segment: its last frames */
        for (int i = 0; i < np && details_on; i++)
            if ((ret = flush(&p[i])) < 0)
                goto end;
    }
    for (int i = 0; i < np && details; i++)
        if (!p[i].done)
            av_log(s, AV_LOG_VERBOSE, "%s: title %s: %s%s: no frame decoded in the first %d MiB: its details are not "
                   "known\n", fmt, title, avcodec_get_name(p[i].codec),
                   p[i].kind == P_LAYER ? " (Dolby Vision layer)" : "", (int)(DETAILS_LIMIT >> 20));
    /* the 3-D second view shows the base view's picture size and rate */
    for (int i = 0; i < *nb_tracks && details; i++) {
        DiscStreamInfo *k = &tracks[i].info;
        if ((tracks[i].flags & DISC_TRACK_SECOND_VIEW) && k->from_track >= 0) {
            const DiscStreamInfo *b = &tracks[k->from_track].info;
            k->width      = b->width;
            k->height     = b->height;
            k->aspect     = b->aspect;
            k->frame_rate = b->frame_rate;
        }
    }
    /* what was found, for titles that share the segment (taken whole) */
    for (int i = 0; i < np && src->key; i++)
        if (!p[i].cached && (!details || p[i].done) &&
            (ret = cache_add(d->pass_cache, src->key, &p[i], details)) < 0)
            goto end;
    /* the Dolby Vision type in full: profile 7 (dual layer, HEVC base
     * layer), level by the picture rate */
    for (int i = 0; i < np; i++)
        if (p[i].kind == P_LAYER && p[i].k->info.dolby_vision[0] == 'B') {
            char type[16];
            av_strlcpy(type, p[i].k->info.dolby_vision, sizeof(type));
            snprintf(p[i].k->info.dolby_vision, sizeof(p[i].k->info.dolby_vision), "dvhe.07.%02d %s",
                     dovi_level(&p[i].k->info), type);
        }
    /* the data */
    if (np && p[0].k == &tracks[0] && !p[0].seen) {
        av_log(s, AV_LOG_ERROR, "%s: title %s: its segment %d has no video packet: the title cannot be used\n", fmt,
               title, src->index + 1);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    for (int i = 0; i < np; i++) {
        const DiscTrack *k = p[i].k;
        if (p[i].kind != P_TRACK || p[i].seen)
            continue;
        if (needs_data(&p[i])) {
            av_log(s, AV_LOG_WARNING, "%s: title %s: %s stream %d has no packet in segment %d: left out\n", fmt,
                   title, av_get_media_type_string(k->info.type), k->stream, src->index + 1);
            drop[nb_drop++] = k - tracks;
        } else if (src->read_all) {
            av_log(s, AV_LOG_WARNING, "%s: title %s: %s stream %d has no packet in segment %d: its track is "
                   "empty\n", fmt, title, k->info.codec == AV_CODEC_ID_DVD_SUBTITLE ? "sub-picture" : "subtitle",
                   k->stream, src->index + 1);
        }
    }
    /* a caption track in the video: the video's rate */
    for (int i = 1; i < *nb_tracks && details; i++)
        if (tracks[i].kind == DR_KIND_CAPTION)
            tracks[i].info.bit_rate = tracks[0].info.bit_rate;
    /* the probes point into the tracks: the tracks left out last, the
     * highest first */
    for (int i = nb_drop - 1; i >= 0; i--)
        track_remove(tracks, nb_tracks, drop[i]);
end:
    for (int i = 0; i < np; i++)
        probe_close(&p[i]);
    av_packet_free(&pkt);
    ff_disc_demux_close(&dm);
    av_free(pids);
    av_free(drop);
    av_free(p);
    return ret;
}
