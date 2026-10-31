/*
 * The cut editor's picture decoder (preview.h).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>

#include "analyze/preview.h"
#include "disc/disc.h"

struct DrPreview {
    AVCodecContext    *ctx;
    AVPacket          *pkt;
    AVFrame           *frame;
    struct SwsContext *sws;
    int                sws_w, sws_h, sws_ow, sws_oh, sws_fmt;
};

int dr_preview_open(DrPreview **pp, const struct DrDiscTrack *t)
{
    const AVCodec *dec = avcodec_find_decoder((enum AVCodecID)t->codec);
    DrPreview *p;
    int ret;

    *pp = NULL;
    if (!dec)
        return AVERROR_DECODER_NOT_FOUND;
    if (!(p = av_mallocz(sizeof(*p))))
        return AVERROR(ENOMEM);
    if (!(p->ctx = avcodec_alloc_context3(dec)) || !(p->pkt = av_packet_alloc()) || !(p->frame = av_frame_alloc())) {
        dr_preview_close(&p);
        return AVERROR(ENOMEM);
    }
    if (t->extradata_size > 0) {
        if (!(p->ctx->extradata = av_mallocz(t->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE))) {
            dr_preview_close(&p);
            return AVERROR(ENOMEM);
        }
        memcpy(p->ctx->extradata, t->extradata, t->extradata_size);
        p->ctx->extradata_size = t->extradata_size;
    }
    if ((ret = avcodec_open2(p->ctx, dec, NULL)) < 0) {
        dr_preview_close(&p);
        return ret;
    }
    *pp = p;
    return 0;
}

int dr_preview_send(DrPreview *p, const uint8_t *data, int size, int64_t time)
{
    int ret;

    if (!data)
        return avcodec_send_packet(p->ctx, NULL);
    av_packet_unref(p->pkt);
    if ((ret = av_new_packet(p->pkt, size)) < 0)
        return ret;
    memcpy(p->pkt->data, data, size);
    p->pkt->pts = p->pkt->dts = time;
    ret = avcodec_send_packet(p->ctx, p->pkt);
    /* a picture the decoder cannot take (damaged): left, the next goes on */
    return ret == AVERROR(EAGAIN) ? AVERROR_BUG : ret == AVERROR_INVALIDDATA ? 0 : ret;
}

/* The decoded picture's planes packed (as FFmpeg's framemd5 hashes them). */
static void frame_md5(const AVFrame *f, char out[33])
{
    int size = av_image_get_buffer_size(f->format, f->width, f->height, 1);
    uint8_t *buf, sum[16];

    out[0] = 0;
    if (size <= 0 || !(buf = av_malloc(size)))
        return;
    if (av_image_copy_to_buffer(buf, size, (const uint8_t *const *)f->data, f->linesize, f->format, f->width,
                                f->height, 1) >= 0) {
        av_md5_sum(sum, buf, size);
        for (int i = 0; i < 16; i++)
            snprintf(out + 2 * i, 3, "%02x", sum[i]);
    }
    av_free(buf);
}

int dr_preview_receive(DrPreview *p, int width, int height, DrPreviewImage *img)
{
    AVFrame *f = p->frame;
    uint8_t *dst[4] = { 0 };
    int dst_stride[4] = { 0 }, ret;

    memset(img, 0, sizeof(*img));
    ret = avcodec_receive_frame(p->ctx, f);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    if (ret < 0)
        return ret;
    if (width <= 0 || height <= 0) {
        width  = f->width;
        height = f->height;
    }
    if (!p->sws || p->sws_w != f->width || p->sws_h != f->height || p->sws_fmt != f->format ||
        p->sws_ow != width || p->sws_oh != height) {
        sws_freeContext(p->sws);
        p->sws = sws_getContext(f->width, f->height, f->format, width, height, AV_PIX_FMT_BGRA, SWS_BICUBIC,
                                NULL, NULL, NULL);
        if (!p->sws) {
            av_frame_unref(f);
            return AVERROR(EINVAL);
        }
        p->sws_w = f->width, p->sws_h = f->height, p->sws_fmt = f->format, p->sws_ow = width, p->sws_oh = height;
    }
    img->width  = width;
    img->height = height;
    img->stride = width * 4;
    img->time   = f->pts;
    img->type   = av_get_picture_type_char(f->pict_type);
    if (!(img->bgra = av_malloc((size_t)img->stride * height))) {
        av_frame_unref(f);
        return AVERROR(ENOMEM);
    }
    dst[0]        = img->bgra;
    dst_stride[0] = img->stride;
    sws_scale(p->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_stride);
    frame_md5(f, img->md5);
    av_frame_unref(f);
    return 1;
}

void dr_preview_reset(DrPreview *p)
{
    avcodec_flush_buffers(p->ctx);
}

void dr_preview_close(DrPreview **pp)
{
    DrPreview *p = *pp;

    if (!p)
        return;
    sws_freeContext(p->sws);
    av_frame_free(&p->frame);
    av_packet_free(&p->pkt);
    avcodec_free_context(&p->ctx);
    av_freep(pp);
}

void dr_preview_image_free(DrPreviewImage *img)
{
    av_freep(&img->bgra);
}
