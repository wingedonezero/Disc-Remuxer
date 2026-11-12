/*
 * Lossless audio decoded and written as FLAC, cut between two samples
 * (flac_out.h).
 */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "base/err.h"
#include "convert/flac_out.h"

/* The FLAC encoder's settings: the strongest compression that stays in the
 * FLAC subset (a prediction order of at most 12 up to 48 kHz). */
#define FLAC_LEVEL      12
#define FLAC_MAX_ORDER  12

struct DrFlacOut {
    AVCodecContext  *dec, *enc;
    AVFormatContext *oc;
    AVStream        *st;
    AVPacket        *pkt, *opkt;
    AVFrame         *frame, *eframe;
    AVAudioFifo     *fifo;
    char            *path;
    int              codec;
    int64_t         *bounds;
    int              nb_bounds, part;
    int64_t          ticks;
    struct AVMD5   **md5;
    DrFlacRange     *ranges;
    /* the decoded sound (set by its first frame) */
    int              set, rate, channels, bits, fmt16;
    AVChannelLayout  layout;
    /* a frame's samples: interleaved in the encoder's format, and as FLAC's MD5 counts them */
    uint8_t         *il, *canon;
    int              cap;
    int64_t          units, decoded, before, kept, after, first, odd;
    int64_t          next;           /* where the samples so far end (ticks), INT64_MIN before the first unit */
    int64_t          enc_pts;
    int              pieces, restarts;
    int              finished, write_err;
    char             err[320];
};

static int fail(DrFlacOut *f, int code, const char *fmt, ...)
{
    va_list ap;

    if (!f->err[0]) {
        va_start(ap, fmt);
        vsnprintf(f->err, sizeof(f->err), fmt, ap);
        va_end(ap);
    }
    return code;
}

/* A library error as a base error, with its words kept. */
static int lib_fail(DrFlacOut *f, int averr, const char *what)
{
    char text[128];

    if (averr == AVERROR(ENOMEM))
        return fail(f, DR_ERR(ENOMEM), "%s: out of memory", what);
    av_strerror(averr, text, sizeof(text));
    return fail(f, averr == AVERROR_INVALIDDATA ? DR_ERR_INVALID : DR_ERR_EXTERNAL, "%s: %s", what, text);
}

/* A failed write to the file: errno kept (a full disk stops the rip). */
static int write_fail(DrFlacOut *f, int averr)
{
    int e = averr == AVERROR(ENOSPC) ? ENOSPC : averr == AVERROR(EDQUOT) ? EDQUOT : averr == AVERROR(EIO) ? EIO : 0;

    if (!e && f->oc && f->oc->pb && f->oc->pb->error) {
        int pe = f->oc->pb->error;
        e = pe == AVERROR(ENOSPC) ? ENOSPC : pe == AVERROR(EDQUOT) ? EDQUOT : EIO;
    }
    if (!e)
        return lib_fail(f, averr, "the FLAC file cannot be written");
    f->write_err = e;
    return fail(f, DR_ERR(e), "the FLAC file cannot be written: %s", strerror(e));
}

int dr_flac_out_open(DrFlacOut **pf, int codec, const char *path, const int64_t *bounds, int nb_bounds, int part,
                     int64_t ticks)
{
    const AVCodec *dec = avcodec_find_decoder((enum AVCodecID)codec);
    DrFlacOut *f;
    int ret;

    *pf = NULL;
    if (!dec)
        return DR_ERR(ENOSYS);
    if (nb_bounds < 0 || part < 0 || part > nb_bounds || ticks <= 0 || !path)
        return DR_ERR(EINVAL);
    for (int i = 1; i < nb_bounds; i++)
        if (bounds[i] <= bounds[i - 1])
            return DR_ERR(EINVAL);
    if (!(f = av_mallocz(sizeof(*f))))
        return DR_ERR(ENOMEM);
    f->codec     = codec;
    f->nb_bounds = nb_bounds;
    f->part      = part;
    f->ticks     = ticks;
    f->first     = INT64_MIN;
    f->next      = INT64_MIN;
    if (!(f->path = av_asprintf("file:%s", path)) ||
        !(f->bounds = av_calloc(nb_bounds + 1, sizeof(*f->bounds))) ||
        !(f->ranges = av_calloc(nb_bounds + 1, sizeof(*f->ranges))) ||
        !(f->md5 = av_calloc(nb_bounds + 1, sizeof(*f->md5))) ||
        !(f->dec = avcodec_alloc_context3(dec)) || !(f->pkt = av_packet_alloc()) ||
        !(f->opkt = av_packet_alloc()) || !(f->frame = av_frame_alloc()) || !(f->eframe = av_frame_alloc())) {
        dr_flac_out_close(&f);
        return DR_ERR(ENOMEM);
    }
    if (nb_bounds)
        memcpy(f->bounds, bounds, nb_bounds * sizeof(*bounds));
    for (int i = 0; i <= nb_bounds; i++) {
        if (!(f->md5[i] = av_md5_alloc())) {
            dr_flac_out_close(&f);
            return DR_ERR(ENOMEM);
        }
        av_md5_init(f->md5[i]);
    }
    /* strict: a failed integrity check, and a frame of a lossless stream
     * that does not come out lossless, is an error */
    f->dec->err_recognition = AV_EF_CRCCHECK | AV_EF_EXPLODE;
    if ((ret = avcodec_open2(f->dec, dec, NULL)) < 0) {
        dr_flac_out_close(&f);
        return ret == AVERROR(ENOMEM) ? DR_ERR(ENOMEM) : DR_ERR_EXTERNAL;
    }
    *pf = f;
    return 0;
}

/* The first sample at or after time at of samples that start at t0 (its
 * number from 0; sample n at t0 + n / rate), at most n. */
static int sample_at(const DrFlacOut *f, int64_t t0, int64_t at, int n)
{
    int64_t d = at - t0, k;

    if (d <= 0)
        return 0;
    /* past the frame's end (also where the product below would not fit) */
    if (d / f->ticks > n / f->rate + 1)
        return n;
    k = (d * f->rate + f->ticks - 1) / f->ticks;
    return k > n ? n : (int)k;
}

/* The decoder's frame as interleaved samples in the encoder's format (il)
 * and as FLAC's MD5 counts them (canon: little-endian, 2 bytes for 16 bits,
 * the upper 3 of 4 for 24). 0, or a negative base error. */
static int samples_of(DrFlacOut *f, const AVFrame *fr, int fmt16, int channels, uint8_t **il, uint8_t **canon, int *cap)
{
    int n = fr->nb_samples, planar = av_sample_fmt_is_planar(fr->format), bps = fmt16 ? 2 : 4;

    if (n > *cap) {
        uint8_t *a = av_realloc_array(*il, n, (size_t)channels * 4);
        uint8_t *b;
        if (!a)
            return DR_ERR(ENOMEM);
        *il = a;
        if (!(b = av_realloc_array(*canon, n, (size_t)channels * 3)))
            return DR_ERR(ENOMEM);
        *canon = b;
        *cap   = n;
    }
    if (planar) {
        for (int c = 0; c < channels; c++) {
            const uint8_t *src = fr->extended_data[c];
            for (int i = 0; i < n; i++)
                memcpy(*il + ((size_t)i * channels + c) * bps, src + (size_t)i * bps, bps);
        }
    } else
        memcpy(*il, fr->extended_data[0], (size_t)n * channels * bps);
    if (fmt16) {
        const int16_t *s = (const int16_t *)*il;
        uint8_t *o = *canon;
        for (size_t i = 0; i < (size_t)n * channels; i++) {
            *o++ = (uint8_t)s[i];
            *o++ = (uint8_t)((uint16_t)s[i] >> 8);
        }
    } else {
        const int32_t *s = (const int32_t *)*il;
        uint8_t *o = *canon;
        uint32_t low = 0;
        for (size_t i = 0; i < (size_t)n * channels; i++) {
            uint32_t v = (uint32_t)s[i];
            low |= v;
            *o++ = (uint8_t)(v >> 8);
            *o++ = (uint8_t)(v >> 16);
            *o++ = (uint8_t)(v >> 24);
        }
        /* a FLAC file of 24 bits holds the upper 24 of the 32: nothing may sit below them */
        if (low & 0xff)
            return fail(f, DR_ERR_INVALID, "a decoded sample has more than 24 bits: FLAC would lose them");
    }
    return 0;
}

/* The encoder's packets into the file. */
static int drain(DrFlacOut *f)
{
    int ret;

    while ((ret = avcodec_receive_packet(f->enc, f->opkt)) >= 0) {
        av_packet_rescale_ts(f->opkt, f->enc->time_base, f->st->time_base);
        f->opkt->stream_index = 0;
        ret = av_write_frame(f->oc, f->opkt);
        av_packet_unref(f->opkt);
        if (ret < 0 || f->oc->pb->error)
            return write_fail(f, ret < 0 ? ret : f->oc->pb->error);
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : lib_fail(f, ret, "the FLAC encoder failed");
}

/* n samples from the fifo to the encoder (a block, or the last short one). */
static int encode_block(DrFlacOut *f, int n)
{
    int ret;

    av_frame_unref(f->eframe);
    f->eframe->format      = f->enc->sample_fmt;
    f->eframe->sample_rate = f->enc->sample_rate;
    f->eframe->nb_samples  = n;
    if ((ret = av_channel_layout_copy(&f->eframe->ch_layout, &f->enc->ch_layout)) < 0 ||
        (ret = av_frame_get_buffer(f->eframe, 0)) < 0)
        return lib_fail(f, ret, "the FLAC encoder's frame");
    if (av_audio_fifo_read(f->fifo, (void **)f->eframe->data, n) != n)
        return fail(f, DR_ERR_BUG, "the samples kept for the FLAC encoder are short");
    f->eframe->pts = f->enc_pts;
    f->enc_pts    += n;
    if ((ret = avcodec_send_frame(f->enc, f->eframe)) < 0)
        return lib_fail(f, ret, "the FLAC encoder failed");
    return drain(f);
}

/* The FLAC encoder and the file, at the first kept sample. */
static int open_file(DrFlacOut *f)
{
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    int ret;

    if (!enc)
        return fail(f, DR_ERR(ENOSYS), "the processing library has no FLAC encoder");
    if (!(f->enc = avcodec_alloc_context3(enc)))
        return fail(f, DR_ERR(ENOMEM), "out of memory");
    f->enc->sample_fmt          = f->fmt16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
    f->enc->sample_rate         = f->rate;
    f->enc->bits_per_raw_sample = f->fmt16 ? 16 : 24;
    f->enc->time_base           = (AVRational){ 1, f->rate };
    f->enc->compression_level   = FLAC_LEVEL;
    f->enc->flags              |= AV_CODEC_FLAG_BITEXACT;
    if ((ret = av_channel_layout_copy(&f->enc->ch_layout, &f->layout)) < 0 ||
        (ret = av_opt_set_int(f->enc->priv_data, "max_prediction_order", FLAC_MAX_ORDER, 0)) < 0 ||
        (ret = avcodec_open2(f->enc, enc, NULL)) < 0)
        return lib_fail(f, ret, "the FLAC encoder cannot be opened");
    if (f->enc->frame_size <= 0)
        return fail(f, DR_ERR_BUG, "the FLAC encoder names no block size");
    if (!(f->fifo = av_audio_fifo_alloc(f->enc->sample_fmt, f->channels, f->enc->frame_size * 2)))
        return fail(f, DR_ERR(ENOMEM), "out of memory");
    if ((ret = avformat_alloc_output_context2(&f->oc, NULL, "flac", NULL)) < 0)
        return lib_fail(f, ret, "the FLAC file writer");
    f->oc->flags |= AVFMT_FLAG_BITEXACT;
    if (!(f->st = avformat_new_stream(f->oc, NULL)))
        return fail(f, DR_ERR(ENOMEM), "out of memory");
    f->st->time_base = f->enc->time_base;
    if ((ret = avcodec_parameters_from_context(f->st->codecpar, f->enc)) < 0)
        return lib_fail(f, ret, "the FLAC file writer");
    if ((ret = avio_open(&f->oc->pb, f->path, AVIO_FLAG_WRITE)) < 0) {
        f->write_err = ret == AVERROR(ENOSPC) ? ENOSPC : ret == AVERROR(EACCES) ? EACCES : ret == AVERROR(ENOENT) ?
                       ENOENT : EIO;
        return lib_fail(f, ret, "the FLAC file cannot be created");
    }
    if ((ret = avformat_write_header(f->oc, NULL)) < 0)
        return write_fail(f, ret);
    return 0;
}

/* A decoded frame whose first sample is at t0. */
static int take_frame(DrFlacOut *f, const AVFrame *fr, int64_t t0)
{
    int n = fr->nb_samples, fmt16, ret, from = 0;
    size_t step;

    if (fr->format == AV_SAMPLE_FMT_S16 || fr->format == AV_SAMPLE_FMT_S16P)
        fmt16 = 1;
    else if (fr->format == AV_SAMPLE_FMT_S32 || fr->format == AV_SAMPLE_FMT_S32P)
        fmt16 = 0;
    else
        return fail(f, DR_ERR(ENOSYS), "the decoder gives %s samples, not whole numbers of 16 or up to 24 bits",
                    av_get_sample_fmt_name(fr->format) ? av_get_sample_fmt_name(fr->format) : "unknown");
    if (!f->set) {
        if (fr->sample_rate <= 0 || fr->ch_layout.nb_channels < 1 || fr->ch_layout.nb_channels > 8)
            return fail(f, DR_ERR(ENOSYS), "%d channels at %d Hz: FLAC holds 1 to 8 channels",
                        fr->ch_layout.nb_channels, fr->sample_rate);
        if ((ret = av_channel_layout_copy(&f->layout, &fr->ch_layout)) < 0)
            return lib_fail(f, ret, "the channel layout");
        f->rate     = fr->sample_rate;
        f->channels = fr->ch_layout.nb_channels;
        f->fmt16    = fmt16;
        f->bits     = f->dec->bits_per_raw_sample > 0 ? f->dec->bits_per_raw_sample : fmt16 ? 16 : 24;
        f->set      = 1;
        if (!fmt16 && f->bits > 24)
            return fail(f, DR_ERR(ENOSYS), "%d-bit samples: only up to 24 bits are written as FLAC", f->bits);
    } else if (fr->sample_rate != f->rate || fmt16 != f->fmt16 || av_channel_layout_compare(&fr->ch_layout, &f->layout))
        return fail(f, DR_ERR_INVALID, "the stream's format changes at unit %"PRId64" (sample rate, sample size or "
                    "channels): one FLAC file cannot hold both", f->units);
    if ((ret = samples_of(f, fr, fmt16, f->channels, &f->il, &f->canon, &f->cap)) < 0)
        return ret;
    step = (size_t)f->channels * (fmt16 ? 2 : 3);
    for (int r = 0; r <= f->nb_bounds; r++) {
        int to = r < f->nb_bounds ? sample_at(f, t0, f->bounds[r], n) : n;

        if (to < from)
            to = from;
        if (to > from) {
            av_md5_update(f->md5[r], f->canon + from * step, (to - from) * step);
            f->ranges[r].samples += to - from;
            if (r < f->part)
                f->before += to - from;
            else if (r > f->part)
                f->after += to - from;
            else {
                uint8_t *p = f->il + (size_t)from * f->channels * (fmt16 ? 2 : 4);
                if (!f->oc && (ret = open_file(f)) < 0)
                    return ret;
                if (f->first == INT64_MIN)
                    f->first = t0 + av_rescale(from, f->ticks, f->rate);
                if (av_audio_fifo_write(f->fifo, (void **)&p, to - from) != to - from)
                    return fail(f, DR_ERR(ENOMEM), "out of memory");
                f->kept += to - from;
                while (av_audio_fifo_size(f->fifo) >= f->enc->frame_size)
                    if ((ret = encode_block(f, f->enc->frame_size)) < 0)
                        return ret;
            }
        }
        from = to;
    }
    f->decoded += n;
    return 0;
}

/* The decoder's frames after a unit (or at the end). */
static int take_frames(DrFlacOut *f, int64_t time)
{
    int ret;

    while ((ret = avcodec_receive_frame(f->dec, f->frame)) >= 0) {
        int64_t t0 = time != AV_NOPTS_VALUE ? time : f->next;

        /* what the stream is, as the decoder finds it frame by frame */
        if (f->codec == AV_CODEC_ID_TRUEHD && f->dec->profile == AV_PROFILE_TRUEHD_ATMOS)
            ret = fail(f, DR_ERR(ENOSYS), "the stream carries Dolby Atmos (found at unit %"PRId64"): decoding would "
                       "drop it", f->units);
        else if (f->codec == AV_CODEC_ID_DTS && (f->dec->profile == AV_PROFILE_DTS_HD_MA_X ||
                                                 f->dec->profile == AV_PROFILE_DTS_HD_MA_X_IMAX))
            ret = fail(f, DR_ERR(ENOSYS), "the stream carries DTS:X (found at frame %"PRId64"): decoding would drop "
                       "it", f->units);
        else if (f->codec == AV_CODEC_ID_DTS && f->dec->profile != AV_PROFILE_DTS_HD_MA)
            ret = fail(f, DR_ERR_INVALID, "frame %"PRId64" has no lossless part (the decoder gives its lossy core)",
                       f->units);
        else if (t0 == INT64_MIN)
            ret = fail(f, DR_ERR_INVALID, "the stream's first unit has no time");
        else {
            /* a unit that does not start where the samples before it end (more than a sample off) */
            if (f->next != INT64_MIN && time != AV_NOPTS_VALUE &&
                (time - f->next > f->ticks / f->rate || f->next - time > f->ticks / f->rate))
                f->odd++;
            ret = take_frame(f, f->frame, t0);
            if (ret >= 0)
                f->next = t0 + av_rescale(f->frame->nb_samples, f->ticks, f->rate);
        }
        av_frame_unref(f->frame);
        if (ret < 0)
            return ret;
        time = AV_NOPTS_VALUE;          /* a unit's second frame follows its first */
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 :
           lib_fail(f, ret, f->codec == AV_CODEC_ID_TRUEHD ? "the TrueHD decoder refuses the stream (strict: an "
                    "integrity check failed or the data is damaged)" : "the DTS decoder refuses the stream (strict: "
                    "a frame did not decode lossless or the data is damaged)");
}

int dr_flac_out_unit(DrFlacOut *f, const uint8_t *data, int size, int64_t time, int piece)
{
    int ret;

    if (f->err[0] || f->finished)
        return DR_ERR_INVALID;
    /* a new piece of the stream: a TrueHD major sync restarts the decoder
     * (every frame of the units before it has been taken) */
    if (piece && f->units) {
        f->pieces++;
        if (f->codec == AV_CODEC_ID_TRUEHD && size >= 8 && data[4] == 0xF8 && data[5] == 0x72 && data[6] == 0x6F &&
            data[7] == 0xBA) {
            avcodec_flush_buffers(f->dec);
            f->restarts++;
        }
    }
    av_packet_unref(f->pkt);
    if ((ret = av_new_packet(f->pkt, size)) < 0)
        return fail(f, DR_ERR(ENOMEM), "out of memory");
    memcpy(f->pkt->data, data, size);
    ret = avcodec_send_packet(f->dec, f->pkt);
    av_packet_unref(f->pkt);
    if (ret < 0)
        return lib_fail(f, ret, f->codec == AV_CODEC_ID_TRUEHD ? "the TrueHD decoder refuses a unit (strict: an "
                        "integrity check failed or the data is damaged)" : "the DTS decoder refuses a frame (strict: "
                        "it did not decode lossless or the data is damaged)");
    ret = take_frames(f, time);
    f->units++;
    return ret;
}

int dr_flac_out_finish(DrFlacOut *f, DrFlacResult *res)
{
    int ret;

    memset(res, 0, sizeof(*res));
    if (f->err[0])
        return DR_ERR_INVALID;
    if (!f->finished) {
        /* what the decoder still holds */
        if ((ret = avcodec_send_packet(f->dec, NULL)) < 0 && ret != AVERROR_EOF)
            return lib_fail(f, ret, "the decoder's end");
        if ((ret = take_frames(f, AV_NOPTS_VALUE)) < 0)
            return ret;
        if (f->oc) {
            int left = av_audio_fifo_size(f->fifo);
            if (left > 0 && (ret = encode_block(f, left)) < 0)
                return ret;
            /* the encoder's end: its last packet carries the header's final
             * values (the samples' MD5, the frame sizes), written by the trailer */
            if ((ret = avcodec_send_frame(f->enc, NULL)) < 0)
                return lib_fail(f, ret, "the FLAC encoder's end");
            if ((ret = drain(f)) < 0)
                return ret;
            if ((ret = av_write_trailer(f->oc)) < 0 || f->oc->pb->error)
                return write_fail(f, ret < 0 ? ret : f->oc->pb->error);
            if ((ret = avio_closep(&f->oc->pb)) < 0)
                return write_fail(f, ret);
        }
        for (int i = 0; i <= f->nb_bounds; i++) {
            uint8_t d[16];
            av_md5_final(f->md5[i], d);
            for (int k = 0; k < 16; k++)
                snprintf(f->ranges[i].md5 + 2 * k, 3, "%02x", d[k]);
        }
        f->finished = 1;
    }
    res->rate      = f->rate;
    res->channels  = f->channels;
    res->bits      = f->bits;
    res->flac_bits = f->fmt16 ? 16 : 24;
    res->layout    = f->layout.order == AV_CHANNEL_ORDER_NATIVE ? f->layout.u.mask : 0;
    /* the layouts FLAC has for a channel count (a 5.0 / 5.1 with side or with back channels both) */
    res->native_layout = res->layout == AV_CH_LAYOUT_MONO || res->layout == AV_CH_LAYOUT_STEREO ||
                         res->layout == AV_CH_LAYOUT_SURROUND || res->layout == AV_CH_LAYOUT_QUAD ||
                         res->layout == AV_CH_LAYOUT_5POINT0 || res->layout == AV_CH_LAYOUT_5POINT0_BACK ||
                         res->layout == AV_CH_LAYOUT_5POINT1 || res->layout == AV_CH_LAYOUT_5POINT1_BACK ||
                         res->layout == AV_CH_LAYOUT_6POINT1 || res->layout == AV_CH_LAYOUT_7POINT1;
    res->units     = f->units;
    res->decoded   = f->decoded;
    res->before    = f->before;
    res->kept      = f->kept;
    res->after     = f->after;
    res->first     = f->first;
    res->odd_units = f->odd;
    res->pieces    = f->pieces;
    res->restarts  = f->restarts;
    res->block     = f->enc ? f->enc->frame_size : 0;
    res->ranges    = f->ranges;
    res->nb_ranges = f->nb_bounds + 1;
    memcpy(res->md5, f->ranges[f->part].md5, sizeof(res->md5));
    return 0;
}

const char *dr_flac_out_error(const DrFlacOut *f)
{
    return f ? f->err : "";
}

int dr_flac_out_write_error(const DrFlacOut *f)
{
    return f ? f->write_err : 0;
}

void dr_flac_out_close(DrFlacOut **pf)
{
    DrFlacOut *f = *pf;

    if (!f)
        return;
    if (f->oc) {
        if (f->oc->pb)
            avio_closep(&f->oc->pb);
        avformat_free_context(f->oc);
    }
    avcodec_free_context(&f->dec);
    avcodec_free_context(&f->enc);
    av_packet_free(&f->pkt);
    av_packet_free(&f->opkt);
    av_frame_free(&f->frame);
    av_frame_free(&f->eframe);
    if (f->fifo)
        av_audio_fifo_free(f->fifo);
    for (int i = 0; f->md5 && i <= f->nb_bounds; i++)
        av_free(f->md5[i]);
    av_free(f->md5);
    av_free(f->ranges);
    av_free(f->bounds);
    av_channel_layout_uninit(&f->layout);
    av_free(f->il);
    av_free(f->canon);
    av_free(f->path);
    av_freep(pf);
}

int dr_flac_read_back(const char *path, int64_t *samples, char *md5, int64_t *header_samples, char *header_md5,
                      char *why, int why_size)
{
    DrFlacOut tmp = { 0 };              /* samples_of()'s error text */
    AVFormatContext *ic = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    struct AVMD5 *ctx = av_md5_alloc();
    const AVInputFormat *fmt = av_find_input_format("flac");
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_FLAC);
    char *url = av_asprintf("file:%s", path);
    uint8_t *il = NULL, *canon = NULL, d[16];
    int cap = 0, ret, eof = 0;
    const AVCodecParameters *par;

    *samples = *header_samples = 0;
    md5[0] = header_md5[0] = 0;
    if (why_size > 0)
        why[0] = 0;
    if (!pkt || !frame || !ctx || !url) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    if (!fmt || !codec) {
        snprintf(why, why_size, "the processing library has no FLAC reader");
        ret = DR_ERR(ENOSYS);
        goto end;
    }
    av_md5_init(ctx);
    if ((ret = avformat_open_input(&ic, url, fmt, NULL)) < 0 || ic->nb_streams < 1) {
        snprintf(why, why_size, "the file cannot be opened as FLAC");
        ret = DR_ERR_EXTERNAL;
        goto end;
    }
    par = ic->streams[0]->codecpar;
    /* the header's STREAMINFO: the total samples (36 bits) and the samples' MD5 */
    if (par->extradata_size >= 34) {
        const uint8_t *x = par->extradata;
        *header_samples = (int64_t)(x[13] & 0x0f) << 32 | (int64_t)x[14] << 24 | x[15] << 16 | x[16] << 8 | x[17];
        for (int k = 0; k < 16; k++)
            snprintf(header_md5 + 2 * k, 3, "%02x", x[18 + k]);
    }
    if (!(dec = avcodec_alloc_context3(codec))) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    dec->err_recognition = AV_EF_CRCCHECK | AV_EF_EXPLODE;
    if ((ret = avcodec_parameters_to_context(dec, par)) < 0 || (ret = avcodec_open2(dec, codec, NULL)) < 0) {
        snprintf(why, why_size, "the FLAC decoder cannot be opened");
        ret = DR_ERR_EXTERNAL;
        goto end;
    }
    while (!eof) {
        ret = av_read_frame(ic, pkt);
        if (ret == AVERROR_EOF) {
            eof = 1;
            ret = avcodec_send_packet(dec, NULL);
        } else if (ret < 0) {
            snprintf(why, why_size, "the file cannot be read to its end");
            ret = DR_ERR_EXTERNAL;
            goto end;
        } else {
            ret = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
        }
        if (ret < 0 && ret != AVERROR_EOF) {
            snprintf(why, why_size, "a FLAC frame does not decode (after %"PRId64" samples)", *samples);
            ret = DR_ERR_INVALID;
            goto end;
        }
        while ((ret = avcodec_receive_frame(dec, frame)) >= 0) {
            int fmt16 = frame->format == AV_SAMPLE_FMT_S16 || frame->format == AV_SAMPLE_FMT_S16P;
            int ch = frame->ch_layout.nb_channels;

            if (!fmt16 && frame->format != AV_SAMPLE_FMT_S32 && frame->format != AV_SAMPLE_FMT_S32P) {
                snprintf(why, why_size, "the FLAC decoder gives a sample format that is not expected");
                ret = DR_ERR_EXTERNAL;
                goto end;
            }
            if ((ret = samples_of(&tmp, frame, fmt16, ch, &il, &canon, &cap)) < 0) {
                snprintf(why, why_size, "%s", tmp.err[0] ? tmp.err : "out of memory");
                goto end;
            }
            av_md5_update(ctx, canon, (size_t)frame->nb_samples * ch * (fmt16 ? 2 : 3));
            *samples += frame->nb_samples;
            av_frame_unref(frame);
        }
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            snprintf(why, why_size, "a FLAC frame does not decode (after %"PRId64" samples)", *samples);
            ret = DR_ERR_INVALID;
            goto end;
        }
    }
    av_md5_final(ctx, d);
    for (int k = 0; k < 16; k++)
        snprintf(md5 + 2 * k, 3, "%02x", d[k]);
    ret = 0;
end:
    if (ret == DR_ERR(ENOMEM) && why_size > 0 && !why[0])
        snprintf(why, why_size, "out of memory");
    avcodec_free_context(&dec);
    avformat_close_input(&ic);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_free(ctx);
    av_free(il);
    av_free(canon);
    av_free(url);
    return ret;
}
