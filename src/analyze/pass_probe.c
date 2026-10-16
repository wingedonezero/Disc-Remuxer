/*
 * The stream pass's details probe (disc/probe.h): a track's
 * payloads through the library's parser and decoder of its codec until one
 * frame is decoded, within the pass's limit; the values go into the track's
 * DiscStreamInfo. Moved out of the disc layer's pass: the library
 * answering questions on copies of data.
 */

#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/avstring.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/frame.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "disc/info.h"
#include "disc/probe.h"

#include "analyze/analyze.h"
#include "timeline/disctl.h"

struct DiscDetailsProbe {
    const DRCodec        *tc;           /* the codec table entry, or NULL */
    DiscStreamInfo       *ds;           /* where the values go */
    int                   layer;        /* a Dolby Vision enhancement layer */
    int                   done;         /* the details are known (or cannot be) */
    int                   opened;       /* the decoder is open (once its headers were found, when it needs them) */
    uint8_t              *hold;         /* the bytes before the decoder could open */
    int                   nb_hold;
    AVCodecParserContext *pc;
    AVCodecContext       *dec;
    AVPacket             *pkt;
    AVFrame              *frame;
};

/* The decoder: allocated, and opened unless it needs the stream's headers
 * first (the codec table: VC-1 opens with its sequence header and entry
 * point as extradata). */
static int probe_open(DiscDetailsProbe *p, enum AVCodecID codec_id)
{
    const AVCodec *codec = avcodec_find_decoder(codec_id);

    if (!codec)
        return AVERROR_DECODER_NOT_FOUND;
    if (!(p->dec = avcodec_alloc_context3(codec)) || !(p->pkt = av_packet_alloc()) ||
        !(p->frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    p->pc = av_parser_init(codec_id);   /* NULL (LPCM): each payload is a packet */
    p->dec->thread_count = 1;           /* one frame: no threads (and none of their warnings) */
    if (p->tc && p->tc->decoder_headers)
        return 0;
    p->opened = 1;
    return avcodec_open2(p->dec, codec, NULL);
}

static void probe_close(DiscDetailsProbe *p)
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
static void take(DiscDetailsProbe *p, const AVFrame *f)
{
    DiscStreamInfo *ds = p->ds;
    AVCodecContext *d = p->dec;

    p->done = 1;
    if (p->layer) {
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
        ds->frame_rate = (DrRational){ d->framerate.num, d->framerate.den };
        hdr_text(ds, f);
        ds->full_range = (f->color_range != AVCOL_RANGE_UNSPECIFIED ? f->color_range : d->color_range) ==
                         AVCOL_RANGE_JPEG;
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
static int decode(DiscDetailsProbe *p, const uint8_t *data, int size)
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

/* A decoder that opens with the stream's headers as extradata (the codec
 * table's decoder_headers): the bytes held until they are found, which
 * then follow. */
static int headers_open(DiscDetailsProbe *p, const uint8_t *data, int size)
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

static int parse(DiscDetailsProbe *p, const uint8_t *data, int size)
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
static int probe_feed(DiscDetailsProbe *p, const uint8_t *data, int size)
{
    if (p->done)
        return 0;
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
static int probe_flush(DiscDetailsProbe *p)
{
    const uint8_t *out;
    int out_size = 0, ret;

    if (p->done || !p->opened)
        return 0;
    if (p->pc) {
        av_parser_parse2(p->pc, p->dec, (uint8_t **)&out, &out_size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (out_size && (ret = decode(p, out, out_size)) < 0)
            return ret;
    }
    return p->done ? 0 : decode(p, NULL, 0);
}


/* ---- the ops the disc layer calls (disc/probe.h) ---- */

static int ops_open(DiscDetailsProbe **pp, DRCodecID codec, const struct DRCodec *tc,
                    int layer, struct DiscStreamInfo *ds)
{
    DiscDetailsProbe *p = av_mallocz(sizeof(*p));
    int ret;

    *pp = NULL;
    if (!p)
        return AVERROR(ENOMEM);
    p->tc    = tc;
    p->ds    = ds;
    p->layer = layer;
    if ((ret = probe_open(p, codec)) < 0) {
        probe_close(p);
        av_free(p);
        return ret;
    }
    *pp = p;
    return 0;
}

static int ops_feed(DiscDetailsProbe *p, const uint8_t *data, int size)
{
    return probe_feed(p, data, size);
}

static int ops_flush(DiscDetailsProbe *p)
{
    return probe_flush(p);
}

static int ops_done(const DiscDetailsProbe *p)
{
    return p->done;
}

static void ops_close(DiscDetailsProbe **pp)
{
    if (!*pp)
        return;
    probe_close(*pp);
    av_freep(pp);
}

static const DiscProbeOps ops = { ops_open, ops_feed, ops_flush, ops_done, ops_close };

void dr_analyze_use_probe(void)
{
    ff_disc_set_probe(&ops);
}
