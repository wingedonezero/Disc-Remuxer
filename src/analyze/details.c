/*
 * The details probe and the end check of a written stream file
 * (analyze/details.h): the processing library's decoders answering
 * questions on copies of data.
 */

#include <stdio.h>
#include <string.h>

#include <libavcodec/bsf.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavutil/stereo3d.h>

#include "analyze/details.h"

#define PROBE_PACKETS 600   /* packets a decoder gets to give its first frame */

struct DetailsProbe {
    const AVCodec     *dec;
    AVCodecParameters *par;         /* until the decoder is open */
    AVCodecContext    *avctx;
    AVFrame           *frame;
    Details            d;
    int                done, packets;
};

/* Opens the decoder; a codec that needs its headers at opening (VC-1) and
 * has none gets them from the first packet (FFmpeg's extract_extradata). */
static int open_decoder(DetailsProbe *p, const AVPacket *first)
{
    int ret;

    if (!p->par->extradata_size && first) {
        const AVBitStreamFilter *f = av_bsf_get_by_name("extract_extradata");
        AVBSFContext *bsf = NULL;
        AVPacket *pkt = av_packet_clone(first);
        size_t size = 0;
        uint8_t *x;

        if (f && pkt && av_bsf_alloc(f, &bsf) >= 0 && avcodec_parameters_copy(bsf->par_in, p->par) >= 0 &&
            av_bsf_init(bsf) >= 0 && av_bsf_send_packet(bsf, pkt) >= 0 && av_bsf_receive_packet(bsf, pkt) >= 0 &&
            (x = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &size)) && size &&
            (p->par->extradata = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE))) {
            memcpy(p->par->extradata, x, size);
            p->par->extradata_size = size;
        }
        av_packet_free(&pkt);
        av_bsf_free(&bsf);
    }
    if (!(p->avctx = avcodec_alloc_context3(p->dec)))
        return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_to_context(p->avctx, p->par)) < 0 || (ret = avcodec_open2(p->avctx, p->dec, NULL)) < 0)
        avcodec_free_context(&p->avctx);
    return ret;
}

static void add(Details *d, const char *name, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void add(Details *d, const char *name, const char *fmt, ...)
{
    va_list ap;

    if (d->n == DETAILS_MAX)
        return;
    av_strlcpy(d->name[d->n], name, sizeof(d->name[0]));
    va_start(ap, fmt);
    vsnprintf(d->value[d->n], sizeof(d->value[0]), fmt, ap);
    va_end(ap);
    d->n++;
}

static double q(AVRational r)
{
    return r.den ? (double)r.num / r.den : 0;
}

/* The fields a video decoder gives after its first frame. */
static void video_fields(Details *d, const AVCodecContext *c, const AVFrame *f)
{
    const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(f->format);
    AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : c->sample_aspect_ratio;
    enum AVColorPrimaries prim = f->color_primaries != AVCOL_PRI_UNSPECIFIED ? f->color_primaries : c->color_primaries;
    enum AVColorTransferCharacteristic trc = f->color_trc != AVCOL_TRC_UNSPECIFIED ? f->color_trc : c->color_trc;
    enum AVColorSpace spc = f->colorspace != AVCOL_SPC_UNSPECIFIED ? f->colorspace : c->colorspace;
    enum AVColorRange rng = f->color_range != AVCOL_RANGE_UNSPECIFIED ? f->color_range : c->color_range;
    enum AVChromaLocation loc = f->chroma_location != AVCHROMA_LOC_UNSPECIFIED ? f->chroma_location : c->chroma_sample_location;
    const AVFrameSideData *sd;

    add(d, "Width", "%d", f->width);
    add(d, "Height", "%d", f->height);
    if (sar.num && sar.den) {
        AVRational dar;
        av_reduce(&dar.num, &dar.den, (int64_t)f->width * sar.num, (int64_t)f->height * sar.den, 1 << 20);
        add(d, "Pixel aspect ratio", "%d:%d", sar.num, sar.den);
        add(d, "Display aspect ratio", "%d:%d", dar.num, dar.den);
    }
    if (c->framerate.num && c->framerate.den)
        add(d, "Frame rate", "%d/%d (%.3f fps)", c->framerate.num, c->framerate.den, q(c->framerate));
    if (pd) {
        add(d, "Pixel format", "%s", pd->name);
        add(d, "Chroma subsampling", "%s", pd->log2_chroma_w ? (pd->log2_chroma_h ? "4:2:0" : "4:2:2") : "4:4:4");
        add(d, "Bit depth", "%d bits", pd->comp[0].depth);
    }
    if (rng != AVCOL_RANGE_UNSPECIFIED)
        add(d, "Colour range", "%s", av_color_range_name(rng));
    if (prim != AVCOL_PRI_UNSPECIFIED)
        add(d, "Colour primaries", "%s", av_color_primaries_name(prim));
    if (trc != AVCOL_TRC_UNSPECIFIED)
        add(d, "Transfer characteristics", "%s", av_color_transfer_name(trc));
    if (spc != AVCOL_SPC_UNSPECIFIED)
        add(d, "Matrix coefficients", "%s", av_color_space_name(spc));
    if (loc != AVCHROMA_LOC_UNSPECIFIED)
        add(d, "Chroma location", "%s", av_chroma_location_name(loc));
    if (c->field_order != AV_FIELD_UNKNOWN) {
        static const char *const fo[] = { "unknown", "progressive", "top field first", "bottom field first",
                                          "top coded first, bottom shown first", "bottom coded first, top shown first" };
        add(d, "Field order", "%s", c->field_order < FF_ARRAY_ELEMS(fo) ? fo[c->field_order] : "other");
    }
    add(d, "Scan type (first frame)", "%s", f->flags & AV_FRAME_FLAG_INTERLACED ? "interlaced" : "progressive");
    add(d, "Top field first (first frame)", "%s", f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST ? "yes" : "no");
    add(d, "Repeat (first frame)", "%d", f->repeat_pict);
    if (c->bit_rate > 0)
        add(d, "Bit rate (stream header)", "%"PRId64, c->bit_rate);
    if ((sd = av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))) {
        const AVMasteringDisplayMetadata *m = (const void *)sd->data;
        add(d, "Mastering display", "R %.4f,%.4f G %.4f,%.4f B %.4f,%.4f WP %.4f,%.4f, %.4f-%.1f cd/m2",
            q(m->display_primaries[0][0]), q(m->display_primaries[0][1]), q(m->display_primaries[1][0]),
            q(m->display_primaries[1][1]), q(m->display_primaries[2][0]), q(m->display_primaries[2][1]),
            q(m->white_point[0]), q(m->white_point[1]), q(m->min_luminance), q(m->max_luminance));
    }
    if ((sd = av_frame_get_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL))) {
        const AVContentLightMetadata *l = (const void *)sd->data;
        add(d, "Content light level", "MaxCLL %u, MaxFALL %u", l->MaxCLL, l->MaxFALL);
    }
    if (av_frame_get_side_data(f, AV_FRAME_DATA_DYNAMIC_HDR_PLUS))
        add(d, "HDR10+ (first frame)", "yes");
    if (av_frame_get_side_data(f, AV_FRAME_DATA_DOVI_RPU_BUFFER) || av_frame_get_side_data(f, AV_FRAME_DATA_DOVI_METADATA))
        add(d, "Dolby Vision RPU (first frame)", "yes");
    if (av_frame_get_side_data(f, AV_FRAME_DATA_A53_CC))
        add(d, "Captions in the video (first frame)", "A/53");
    if ((sd = av_frame_get_side_data(f, AV_FRAME_DATA_AFD)) && sd->size)
        add(d, "Active format (first frame)", "%d", sd->data[0]);
    if ((sd = av_frame_get_side_data(f, AV_FRAME_DATA_STEREO3D)))
        add(d, "Stereo 3D", "%s", av_stereo3d_type_name(((const AVStereo3D *)sd->data)->type));
}

/* The fields an audio decoder gives after its first frame. */
static void audio_fields(Details *d, const AVCodecContext *c, const AVFrame *f)
{
    char layout[128];
    const AVFrameSideData *sd;

    add(d, "Sample rate", "%d", f->sample_rate);
    add(d, "Channels", "%d", f->ch_layout.nb_channels);
    if (av_channel_layout_describe(&f->ch_layout, layout, sizeof(layout)) > 0)
        add(d, "Channel layout", "%s", layout);
    add(d, "Sample format", "%s", av_get_sample_fmt_name(f->format));
    if (c->bits_per_raw_sample > 0)
        add(d, "Bit depth", "%d bits", c->bits_per_raw_sample);
    if (c->bit_rate > 0)
        add(d, "Bit rate", "%"PRId64, c->bit_rate);
    /* plain PCM has no frames of its own: a packet's samples are only how it is cut */
    if (!av_get_exact_bits_per_sample(c->codec_id))
        add(d, "Samples per frame", "%d", f->nb_samples);
    if ((sd = av_frame_get_side_data(f, AV_FRAME_DATA_MATRIXENCODING)) && sd->size >= sizeof(int)) {
        static const char *const me[] = { "none", "Dolby", "Dolby Pro Logic II", "Dolby Pro Logic IIx",
                                          "Dolby Pro Logic IIz", "Dolby EX", "Dolby Headphone" };
        int v = *(const int *)sd->data;
        add(d, "Matrix encoding", "%s", v >= 0 && v < (int)FF_ARRAY_ELEMS(me) ? me[v] : "other");
    }
    if ((sd = av_frame_get_side_data(f, AV_FRAME_DATA_AUDIO_SERVICE_TYPE)) && sd->size >= sizeof(int))
        add(d, "Audio service type", "%d", *(const int *)sd->data);
}

static void fill(DetailsProbe *p)
{
    AVCodecContext *c = p->avctx;
    const char *prof = avcodec_profile_name(c->codec_id, c->profile);

    p->d.valid = 1;
    add(&p->d, "Format", "%s", avcodec_get_name(c->codec_id));
    if (prof)
        add(&p->d, "Profile", "%s", prof);
    else if (c->profile != AV_PROFILE_UNKNOWN)
        add(&p->d, "Profile", "%d", c->profile);
    if (c->level != AV_LEVEL_UNKNOWN && c->level > 0)
        add(&p->d, "Level", "%d", c->level);
    if (c->codec_type == AVMEDIA_TYPE_VIDEO)
        video_fields(&p->d, c, p->frame);
    else
        audio_fields(&p->d, c, p->frame);
}

int details_probe_open(DetailsProbe **pp, const AVCodecParameters *par)
{
    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    DetailsProbe *p;
    int ret;

    *pp = NULL;
    if (!dec)
        return AVERROR_DECODER_NOT_FOUND;
    if (!(p = av_mallocz(sizeof(*p))))
        return AVERROR(ENOMEM);
    p->dec = dec;
    if (!(p->frame = av_frame_alloc()) || !(p->par = avcodec_parameters_alloc())) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    if ((ret = avcodec_parameters_copy(p->par, par)) < 0)
        goto fail;
    /* without headers the decoder may only open on the first packet */
    if (open_decoder(p, NULL) < 0 && par->extradata_size && (ret = AVERROR_INVALIDDATA))
        goto fail;
    *pp = p;
    return 0;
fail:
    details_probe_close(&p);
    return ret;
}

int details_probe_packet(DetailsProbe *p, const AVPacket *pkt)
{
    if (p->done)
        return 1;
    if (!p->avctx && open_decoder(p, pkt) < 0) {
        p->done = 1;                       /* not decodable: no details (the check says so) */
        return 1;
    }
    if (avcodec_send_packet(p->avctx, pkt) >= 0 && avcodec_receive_frame(p->avctx, p->frame) >= 0) {
        fill(p);
        p->done = 1;
    } else if (++p->packets >= PROBE_PACKETS) {
        p->done = 1;
    }
    return p->done;
}

void details_probe_get(DetailsProbe *p, Details *d)
{
    if (!p->done && p->avctx && avcodec_send_packet(p->avctx, NULL) >= 0 &&
        avcodec_receive_frame(p->avctx, p->frame) >= 0)
        fill(p);
    p->done = 1;
    *d = p->d;
}

void details_probe_close(DetailsProbe **pp)
{
    DetailsProbe *p = *pp;

    if (!p)
        return;
    av_frame_free(&p->frame);
    avcodec_free_context(&p->avctx);
    avcodec_parameters_free(&p->par);
    av_freep(pp);
}

int details_of_file(const char *path, enum AVMediaType type, Details *d)
{
    AVFormatContext *fc = NULL;
    DetailsProbe *p = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVDictionary *opts = NULL;
    AVCodecParameters *par;
    int ret, idx;

    memset(d, 0, sizeof(*d));
    if (!pkt)
        return AVERROR(ENOMEM);
    /* room for enough pictures of a 2160p stream to measure its rate (the
     * default 5 MB holds a few dozen) */
    av_dict_set(&opts, "probesize", "67108864", 0);
    ret = avformat_open_input(&fc, path, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0 || (ret = avformat_find_stream_info(fc, NULL)) < 0)
        goto end;
    if ((ret = idx = av_find_best_stream(fc, type, -1, -1, NULL, 0)) < 0)
        goto end;
    /* a RIFF WAVE with WAVE_FORMAT_PCM (tag 1) has no channel mask: its one
     * or two channels are mono, or left and right, by the format's
     * definition (FFmpeg reads them as channels of no layout) */
    par = fc->streams[idx]->codecpar;
    if (!strcmp(fc->iformat->name, "wav") && par->codec_tag == 1 && par->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC &&
        par->ch_layout.nb_channels >= 1 && par->ch_layout.nb_channels <= 2)
        av_channel_layout_default(&par->ch_layout, par->ch_layout.nb_channels);
    if ((ret = details_probe_open(&p, par)) < 0)
        goto end;
    while ((ret = av_read_frame(fc, pkt)) >= 0) {
        int done = pkt->stream_index == idx && details_probe_packet(p, pkt);
        av_packet_unref(pkt);
        if (done)
            break;
    }
    details_probe_get(p, d);
    ret = 0;
end:
    details_probe_close(&p);
    av_packet_free(&pkt);
    avformat_close_input(&fc);
    return ret;
}

int details_compare(const Details *disc, const Details *file, char *diff, size_t size)
{
    AVBPrint b;
    int n = 0;

    av_bprint_init(&b, 0, AV_BPRINT_SIZE_AUTOMATIC);
    for (int i = 0; i < disc->n; i++) {
        int k;
        for (k = 0; k < file->n && strcmp(file->name[k], disc->name[i]); k++)
            ;
        if (k < file->n && !strcmp(file->value[k], disc->value[i]))
            continue;
        av_bprintf(&b, "%s%s: %s -> %s", n ? "; " : "", disc->name[i], disc->value[i],
                   k < file->n ? file->value[k] : "missing");
        n++;
    }
    av_strlcpy(diff, b.str, size);
    av_bprint_finalize(&b, NULL);
    return n;
}

/* ---- the channel-layout probe ---- */

struct DrLayoutProbe {
    AVCodecContext *avctx;
    AVFrame        *frame;
    int             done;
};

int dr_layout_probe_open(DrLayoutProbe **pp, const AVCodecParameters *par)
{
    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    DrLayoutProbe *p;

    *pp = NULL;
    if (!dec)
        return AVERROR_DECODER_NOT_FOUND;
    if (!(p = av_mallocz(sizeof(*p))))
        return AVERROR(ENOMEM);
    if (!(p->avctx = avcodec_alloc_context3(dec)) || !(p->frame = av_frame_alloc()) ||
        avcodec_open2(p->avctx, dec, NULL) < 0) {
        dr_layout_probe_close(&p);
        return AVERROR(ENOSYS);
    }
    *pp = p;
    return 0;
}

int dr_layout_probe_packet(DrLayoutProbe *p, const AVPacket *pkt)
{
    if (!p->done && avcodec_send_packet(p->avctx, pkt) >= 0 &&
        avcodec_receive_frame(p->avctx, p->frame) >= 0)
        p->done = 1;
    return p->done;
}

const AVChannelLayout *dr_layout_probe_layout(const DrLayoutProbe *p)
{
    return p && p->done && p->frame->ch_layout.nb_channels ? &p->frame->ch_layout : NULL;
}

int dr_layout_probe_profile(const DrLayoutProbe *p, int fallback)
{
    return p && p->avctx ? p->avctx->profile : fallback;
}

void dr_layout_probe_close(DrLayoutProbe **pp)
{
    DrLayoutProbe *p = *pp;

    if (!p)
        return;
    av_frame_free(&p->frame);
    avcodec_free_context(&p->avctx);
    av_freep(pp);
}
