/*
 * disc-remuxer: the stream files of a title (see writer.h). Each track's
 * packets back to back in a file named <name>_<NN>_<lang>_<codec>
 * [_<channel layout>][ DELAY <ms>ms].<ext>; PCM as RIFF WAVE; sub-pictures
 * as a VobSub pair (.sub packs + .idx); the chapters as Matroska XML.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "log.h"
#include "msg.h"
#include "names.h"
#include "settings.h"
#include "writer.h"

/* One stream's result (times in ms on the title timeline). */
typedef struct StreamResult {
    int         index;
    const char *kind, *codec, *lang, *file;     /* file "" = not written */
    int64_t     packets, bytes;
    int64_t     first_ms, end_ms;
    int64_t     delay_ms;                       /* audio: the start delay in the file name */
} StreamResult;

/* The file extension and the codec's short name in file names. */
static const char *es_extension(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_MPEG1VIDEO:   return "m1v";
    case AV_CODEC_ID_MPEG2VIDEO:   return "m2v";
    case AV_CODEC_ID_VC1:          return "vc1";
    case AV_CODEC_ID_H264:         return "h264";
    case AV_CODEC_ID_HEVC:         return "hevc";
    case AV_CODEC_ID_AC3:          return "ac3";
    case AV_CODEC_ID_EAC3:         return "eac3";
    case AV_CODEC_ID_TRUEHD:       return "thd";
    case AV_CODEC_ID_MLP:          return "mlp";
    case AV_CODEC_ID_DTS:          return "dts";
    case AV_CODEC_ID_MP1:          return "mp1";
    case AV_CODEC_ID_MP2:          return "mp2";
    case AV_CODEC_ID_MP3:          return "mp3";
    case AV_CODEC_ID_AAC:          return "aac";
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:    return "wav";
    case AV_CODEC_ID_DVD_SUBTITLE: return "sub";
    default:                       return "bin";
    }
}

const char *codec_label(enum AVCodecID id, int profile)
{
    switch (id) {
    case AV_CODEC_ID_MPEG1VIDEO:   return "Mpeg1";
    case AV_CODEC_ID_MPEG2VIDEO:   return "Mpeg2";
    case AV_CODEC_ID_VC1:          return "VC-1";
    case AV_CODEC_ID_H264:         return "Mpeg4";
    case AV_CODEC_ID_HEVC:         return "MpegH";
    case AV_CODEC_ID_AC3:          return "DD";
    case AV_CODEC_ID_EAC3:         return "DDplus";
    case AV_CODEC_ID_TRUEHD:       return "TrueHD";
    case AV_CODEC_ID_MLP:          return "MLP";
    case AV_CODEC_ID_DTS:          return profile == AV_PROFILE_DTS_HD_MA ? "DTS-HD MA" :
                                          profile == AV_PROFILE_DTS_HD_HRA ? "DTS-HD HR" : "DTS";
    case AV_CODEC_ID_MP1:
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:          return "MPEG audio";
    case AV_CODEC_ID_AAC:          return "AAC";
    case AV_CODEC_ID_PCM_DVD:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:    return "LPCM";
    case AV_CODEC_ID_DVD_SUBTITLE: return "VobSub";
    default:                       return avcodec_get_name(id);
    }
}

#define HOLD_MAX 64   /* audio packets kept while their channel layout is not known yet */

typedef struct EsOut {
    StreamResult st;
    char         *name;
    FILE         *f;
    FILE         *idx;              /* sub-pictures: the VobSub index next to the .sub */
    int64_t       sub_pos;          /* bytes written to the .sub */
    int           wav;              /* a WAV header to complete at the end */
    int           opened;           /* the file name is chosen */
    char            layout[64];     /* audio: the channel layout in the name ("" unknown) */
    AVCodecContext *avctx;          /* audio: FFmpeg's decoder, for the channel layout */
    AVFrame        *frame;
    int             layout_done;    /* a frame was decoded (or decoding gave up) */
    AVPacket    **hold;
    int           nb_hold;
} EsOut;

static void put_le(FILE *f, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        fputc((v >> (8 * i)) & 0xFF, f);
}

/* RIFF WAVE header: WAVE_FORMAT_PCM for up to 2 channels of 16 bits, else
 * WAVE_FORMAT_EXTENSIBLE with the channel mask; sizes filled in at the end. */
static void wav_header(FILE *f, const AVCodecParameters *par, int64_t data)
{
    int ch = par->ch_layout.nb_channels, bits = par->codec_id == AV_CODEC_ID_PCM_S24LE ? 24 : 16;
    int ext = ch > 2 || bits > 16, fmt = ext ? 40 : 16;
    uint64_t riff = 4 + 8 + fmt + 8 + data;

    fputs("RIFF", f); put_le(f, riff > 0xFFFFFFFFu ? 0xFFFFFFFFu : riff, 4); fputs("WAVE", f);
    fputs("fmt ", f); put_le(f, fmt, 4);
    put_le(f, ext ? 0xFFFE : 1, 2); put_le(f, ch, 2); put_le(f, par->sample_rate, 4);
    put_le(f, (uint64_t)par->sample_rate * ch * bits / 8, 4); put_le(f, ch * bits / 8, 2); put_le(f, bits, 2);
    if (ext) {
        static const uint8_t pcm_guid[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                              0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        put_le(f, 22, 2); put_le(f, bits, 2);
        put_le(f, par->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? par->ch_layout.u.mask : 0, 4);
        put_le(f, 1, 2); fwrite(pcm_guid, 1, sizeof(pcm_guid), f);
    }
    fputs("data", f); put_le(f, data > 0xFFFFFFFFu ? 0xFFFFFFFFu : data, 4);
}

/* Matroska chapter XML (mkvmerge --chapters). */
static int write_chapters(const AVFormatContext *ctx, const char *path)
{
    FILE *f;

    if (!ctx->nb_chapters)
        return 0;
    if (!(f = fopen(path, "w")))
        return AVERROR(errno);
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE Chapters SYSTEM \"matroskachapters.dtd\">\n"
               "<Chapters>\n  <EditionEntry>\n");
    for (unsigned i = 0; i < ctx->nb_chapters; i++) {
        const AVChapter *c = ctx->chapters[i];
        const AVDictionaryEntry *t = av_dict_get(c->metadata, "title", NULL, 0);
        int64_t ns = av_rescale_q(c->start, c->time_base, (AVRational){ 1, 1000000000 });
        fprintf(f, "    <ChapterAtom>\n      <ChapterTimeStart>%02"PRId64":%02"PRId64":%02"PRId64".%09"PRId64"</ChapterTimeStart>\n",
                ns / INT64_C(3600000000000), ns / INT64_C(60000000000) % 60, ns / INT64_C(1000000000) % 60, ns % INT64_C(1000000000));
        fprintf(f, "      <ChapterDisplay>\n        <ChapterString>");
        if (t && *t->value) {
            for (const char *p = t->value; *p; p++)
                switch (*p) {
                case '<': fputs("&lt;", f); break;
                case '>': fputs("&gt;", f); break;
                case '&': fputs("&amp;", f); break;
                default:  fputc(*p, f);
                }
        } else {
            fprintf(f, "Chapter %02u", i + 1);
        }
        fprintf(f, "</ChapterString>\n      </ChapterDisplay>\n    </ChapterAtom>\n");
    }
    fprintf(f, "  </EditionEntry>\n</Chapters>\n");
    return fclose(f) ? AVERROR(errno) : 0;
}

/* ---- VobSub (.sub: the sub-picture units in MPEG-2 program stream packs
 * of 2048 bytes, private stream 1 sub-stream 0x20; .idx: the index header
 * and one "timestamp: ..., filepos: ..." line per unit) ---- */

#define PACK 2048

static void put_ts(uint8_t *p, int marker, int64_t ts)
{
    p[0] = marker << 4 | ((ts >> 29) & 0x0E) | 1;
    p[1] = ts >> 22;
    p[2] = ((ts >> 14) & 0xFE) | 1;
    p[3] = ts >> 7;
    p[4] = (ts << 1) | 1;
}

/* One unit (PTS in 90 kHz) as packs: the PTS in the first packet; the last
 * pack is filled with stuffing bytes in its PES header (fewer than 6
 * bytes left) or a padding packet. */
static int vobsub_unit(EsOut *o, const uint8_t *data, int size, int64_t pts)
{
    int first = 1;

    while (size > 0 || first) {
        uint8_t pk[PACK];
        int64_t scr = FFMAX(pts, 0);
        int hdr = first ? 5 : 0, room, n, left, stuff = 0, i = 0;

        memset(pk, 0xFF, sizeof(pk));
        pk[i++] = 0; pk[i++] = 0; pk[i++] = 1; pk[i++] = 0xBA;
        pk[i++] = 0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 0x03);
        pk[i++] = scr >> 20;
        pk[i++] = ((scr >> 12) & 0xF8) | 0x04 | ((scr >> 13) & 0x03);
        pk[i++] = scr >> 5;
        pk[i++] = ((scr << 3) & 0xF8) | 0x04;
        pk[i++] = 0x01;                                  /* SCR extension 0, marker */
        pk[i++] = 0x01; pk[i++] = 0x89; pk[i++] = 0xC3;  /* program mux rate 25200 (x 50 bytes/s) */
        pk[i++] = 0xF8;                                  /* no pack stuffing */
        room = PACK - i - 6 - 3 - hdr - 1;
        n    = FFMIN(size, room);
        left = room - n;
        if (left > 0 && left < 6)                        /* too little for a padding packet */
            stuff = left;
        pk[i++] = 0; pk[i++] = 0; pk[i++] = 1; pk[i++] = 0xBD;
        AV_WB16(pk + i, 3 + hdr + stuff + 1 + n); i += 2;
        pk[i++] = 0x81;
        pk[i++] = first ? 0x80 : 0x00;
        pk[i++] = hdr + stuff;
        if (first) {
            put_ts(pk + i, 2, pts);
            i += 5;
        }
        i += stuff;                                      /* 0xFF stuffing bytes */
        pk[i++] = 0x20;
        memcpy(pk + i, data, n);
        i += n;
        if (PACK - i >= 6) {                             /* padding packet */
            pk[i] = 0; pk[i + 1] = 0; pk[i + 2] = 1; pk[i + 3] = 0xBE;
            AV_WB16(pk + i + 4, PACK - i - 6);
        }
        if (fwrite(pk, 1, PACK, o->f) != PACK)
            return AVERROR(EIO);
        o->sub_pos += PACK;
        data  += n;
        size  -= n;
        first  = 0;
    }
    return 0;
}

static int vobsub_write(EsOut *o, const AVStream *st, const AVPacket *pkt)
{
    int64_t pts = pkt->pts == AV_NOPTS_VALUE ? 0 : av_rescale_q(pkt->pts, st->time_base, (AVRational){ 1, 90000 });
    int64_t ms  = pkt->pts == AV_NOPTS_VALUE ? 0 : av_rescale_q(pkt->pts, st->time_base, (AVRational){ 1, 1000 });

    if (ms < 0)
        ms = 0;
    fprintf(o->idx, "timestamp: %02"PRId64":%02"PRId64":%02"PRId64":%03"PRId64", filepos: %09"PRIx64"\n",
            ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000, (uint64_t)o->sub_pos);
    o->st.bytes += pkt->size;
    return vobsub_unit(o, pkt->data, pkt->size, pts);
}

/* Chooses the stream's file name and opens it: <prefix>_<NN>_<lang>_<codec>
 * [_<channel layout>][ DELAY <ms>ms].<ext>; audio carries its start delay
 * (its first time against the title start, which is the video's). */
static int es_open(const AVFormatContext *ctx, const AVStream *st, EsOut *o, const char *prefix, int64_t first)
{
    const AVCodecParameters *par = st->codecpar;
    char layout[64] = "", delay[32] = "";
    int audio = par->codec_type == AVMEDIA_TYPE_AUDIO;
    int profile = o->avctx ? o->avctx->profile : par->profile;

    o->opened = 1;
    if (audio) {
        const AVChannelLayout *cl = par->ch_layout.nb_channels ? &par->ch_layout :
                                    o->frame && o->frame->ch_layout.nb_channels ? &o->frame->ch_layout : NULL;
        if (cl) {
            layout[0] = '_';
            av_channel_layout_describe(cl, layout + 1, sizeof(layout) - 1);
            snprintf(o->layout, sizeof(o->layout), "%s", layout + 1);
        } else {
            log_text(LOG_WARNING, "Track %d: channel layout not known: not in the file name", st->index);
        }
        o->st.delay_ms = av_rescale_q_rnd(first, st->time_base, (AVRational){ 1, 1000 }, AV_ROUND_NEAR_INF);
        snprintf(delay, sizeof(delay), " DELAY %"PRId64"ms", o->st.delay_ms);
    }
    if (!(o->name = av_asprintf("%s_%02d_%s_%s%s%s.%s", prefix, st->index, *o->st.lang ? o->st.lang : "und",
                                codec_label(par->codec_id, profile), layout, delay, es_extension(par->codec_id))))
        return AVERROR(ENOMEM);
    o->st.file = o->name;
    if (!(o->f = fopen(o->name, "wb")))
        return AVERROR(errno);
    if (par->codec_id == AV_CODEC_ID_PCM_S16LE || par->codec_id == AV_CODEC_ID_PCM_S24LE) {
        o->wav = 1;
        wav_header(o->f, par, 0);
    }
    if (par->codec_id == AV_CODEC_ID_DVD_SUBTITLE) {
        char *idx = av_strdup(o->name);
        if (!idx)
            return AVERROR(ENOMEM);
        memcpy(idx + strlen(idx) - 3, "idx", 3);
        o->idx = fopen(idx, "w");
        av_free(idx);
        if (!o->idx)
            return AVERROR(errno);
        /* a VobSub index starts with its format line (readers check it) */
        if (par->extradata_size < 22 || memcmp(par->extradata, "# VobSub index file, v", 22))
            fputs("# VobSub index file, v7 (do not modify this line!)\n", o->idx);
        if (par->extradata_size)
            fwrite(par->extradata, 1, par->extradata_size, o->idx);
        fprintf(o->idx, "\n# %s\nid: %s, index: 0\n", *o->st.lang ? o->st.lang : "und", *o->st.lang ? o->st.lang : "und");
    }
    (void)ctx;
    return 0;
}

static int es_write(EsOut *o, const AVPacket *pkt)
{
    if (o->idx)
        return AVERROR_BUG;   /* sub-pictures go through vobsub_write() */
    if (fwrite(pkt->data, 1, pkt->size, o->f) != (size_t)pkt->size)
        return AVERROR(EIO);
    o->st.bytes += pkt->size;
    return 0;
}

static int es_flush_hold(const AVFormatContext *ctx, const AVStream *st, EsOut *o, const char *prefix)
{
    int ret = 0;

    if (!o->opened && o->nb_hold && (ret = es_open(ctx, st, o, prefix, o->hold[0]->pts)) < 0)
        return ret;
    for (int i = 0; i < o->nb_hold; i++) {
        if (ret >= 0)
            ret = es_write(o, o->hold[i]);
        av_packet_free(&o->hold[i]);
    }
    o->nb_hold = 0;
    return ret;
}

int open_title(const char *format, const char *source, int title, AVFormatContext **ctx)
{
    const AVInputFormat *fmt = av_find_input_format(format);
    AVDictionary *opts = NULL;
    const char *const *keys;
    int nb_keys = setting_list("aacs.key_files", &keys), ret;

    *ctx = NULL;
    if (!fmt)
        return AVERROR_DEMUXER_NOT_FOUND;
    av_dict_set_int(&opts, "title", title, 0);
    av_dict_set_int(&opts, "read_attempts", setting_int("read.attempts"), 0);
    av_dict_set(&opts, "udf_reader", setting_text("read.udf_reader"), 0);
    if (nb_keys) {
        AVBPrint bp;
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        for (int i = 0; i < nb_keys; i++)
            av_bprintf(&bp, "%s%s", i ? "," : "", keys[i]);
        av_dict_set(&opts, "keydb", bp.str, 0);
        av_bprint_finalize(&bp, NULL);
    }
    ret = avformat_open_input(ctx, source, fmt, &opts);
    av_dict_free(&opts);
    return ret;
}

static int64_t meta_int(AVDictionary *m, const char *key)
{
    const AVDictionaryEntry *e = av_dict_get(m, key, NULL, 0);
    return e ? strtoll(e->value, NULL, 10) : 0;
}

static void hms(char *buf, size_t size, double s)
{
    int64_t t = (int64_t)(s + 0.5);
    snprintf(buf, size, "%d:%02d:%02d", (int)(t / 3600), (int)(t / 60 % 60), (int)(t % 60));
}

int demux_title(const TitleJob *job, TitleOutcome *res)
{
    AVFormatContext *ctx = NULL;
    const AVDictionaryEntry *e;
    EsOut *out = NULL;
    AVPacket *pkt = NULL;
    char dir[4096], prefix[4200], dur[32], js[4300];
    int ret;

    memset(res, 0, sizeof(*res));
    if ((ret = open_title(job->format, job->source, job->title, &ctx)) < 0) {
        log_step("=== Title %d/%d: #%d ===", job->ordinal, job->count, job->title);
        log_msg(MSG_TITLE_OPEN_FAILED, LOG_ERROR, "Title #%d cannot be opened: %s", job->title, av_err2str(ret));
        res->failed = 1;
        return ret;
    }
    e = av_dict_get(ctx->metadata, "title", NULL, 0);
    title_name(setting_text("output.file_name_template"), e ? e->value : NULL, NULL, NULL, job->title,
               res->name, sizeof(res->name));
    hms(dur, sizeof(dur), ctx->duration > 0 ? ctx->duration / (double)AV_TIME_BASE : 0);
    log_step("=== Title %d/%d: #%d %s (%s) ===", job->ordinal, job->count, job->title, e ? e->value : "", dur);
    log_json("title_start", "\"title\":%d,\"name\":%s,\"ordinal\":%d,\"count\":%d", job->title,
             json_str(js, sizeof(js), res->name), job->ordinal, job->count);

    snprintf(dir, sizeof(dir), "%s%s%s", job->out_dir, job->sub_folder ? "/" : "", job->sub_folder ? res->name : "");
    if (mkdir(dir, 0777) < 0 && errno != EEXIST) {
        ret = AVERROR(errno);
        log_msg(MSG_FOLDER_FAILED, LOG_ERROR, "The folder %s cannot be created: %s", dir, strerror(errno));
        goto end;
    }
    snprintf(prefix, sizeof(prefix), "%s/%s", dir, res->name);
    log_text(LOG_DETAIL, "Files: %s_*", prefix);

    out = av_calloc(ctx->nb_streams, sizeof(*out));
    pkt = av_packet_alloc();
    if (!out || !pkt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (unsigned i = 0; i < ctx->nb_streams; i++) {
        const AVStream *st = ctx->streams[i];
        const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        EsOut *o = &out[i];

        o->st.index    = i;
        o->st.kind     = av_get_media_type_string(st->codecpar->codec_type);
        o->st.codec    = avcodec_get_name(st->codecpar->codec_id);
        o->st.lang     = lang ? lang->value : "";
        o->st.file     = "";
        o->st.first_ms = AV_NOPTS_VALUE;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !st->codecpar->ch_layout.nb_channels) {
            const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
            if (!(o->hold = av_calloc(HOLD_MAX, sizeof(*o->hold)))) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            if (!dec || !(o->avctx = avcodec_alloc_context3(dec)) || !(o->frame = av_frame_alloc()) ||
                avcodec_open2(o->avctx, dec, NULL) < 0) {
                log_text(LOG_DETAIL, "Track %u: no decoder for %s: its channel layout is not known", i,
                         avcodec_get_name(st->codecpar->codec_id));
                avcodec_free_context(&o->avctx);
                o->layout_done = 1;
            }
        }
    }

    while ((ret = av_read_frame(ctx, pkt)) >= 0) {
        const AVStream *st = ctx->streams[pkt->stream_index];
        EsOut *o = &out[pkt->stream_index];
        int64_t progress;

        if (av_opt_get_int(ctx, "progress", AV_OPT_SEARCH_CHILDREN, &progress) >= 0)
            log_progress(job->title, progress / 10000.0);
        o->st.packets++;
        if (pkt->pts != AV_NOPTS_VALUE) {
            int64_t ms  = av_rescale_q(pkt->pts, st->time_base, (AVRational){ 1, 1000 });
            int64_t end = av_rescale_q(pkt->pts + pkt->duration, st->time_base, (AVRational){ 1, 1000 });
            if (o->st.first_ms == AV_NOPTS_VALUE)
                o->st.first_ms = ms;
            o->st.end_ms = FFMAX(o->st.end_ms, end);
        }
        if (!o->opened && o->hold) {
            if (!o->layout_done && avcodec_send_packet(o->avctx, pkt) >= 0 &&
                avcodec_receive_frame(o->avctx, o->frame) >= 0)
                o->layout_done = 1;
            if (!(o->hold[o->nb_hold++] = av_packet_clone(pkt))) {
                ret = AVERROR(ENOMEM);
                av_packet_unref(pkt);
                goto end;
            }
            if ((o->layout_done || o->nb_hold == HOLD_MAX) && (ret = es_flush_hold(ctx, st, o, prefix)) < 0) {
                av_packet_unref(pkt);
                goto end;
            }
            av_packet_unref(pkt);
            continue;
        }
        if (!o->opened && (ret = es_open(ctx, st, o, prefix, pkt->pts)) < 0) {
            av_packet_unref(pkt);
            goto end;
        }
        ret = o->idx ? vobsub_write(o, st, pkt) : es_write(o, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    if (ret == AVERROR_EOF)
        ret = 0;
    for (unsigned i = 0; ret >= 0 && i < ctx->nb_streams; i++)
        ret = es_flush_hold(ctx, ctx->streams[i], &out[i], prefix);
    if (ret >= 0) {
        char *chap = av_asprintf("%s_chapters.xml", prefix);
        ret = chap ? write_chapters(ctx, chap) : AVERROR(ENOMEM);
        av_free(chap);
    }

end:
    for (unsigned i = 0; ctx && out && i < ctx->nb_streams; i++) {
        EsOut *o = &out[i];
        if (o->f && o->wav && ret >= 0) {
            if (fseek(o->f, 0, SEEK_SET) == 0)
                wav_header(o->f, ctx->streams[i]->codecpar, o->st.bytes);
            else
                ret = AVERROR(errno);
        }
        if (o->f && fclose(o->f) && ret >= 0)
            ret = AVERROR(errno);
        if (o->idx && fclose(o->idx) && ret >= 0)
            ret = AVERROR(errno);
    }
    if (ret < 0) {
        log_msg(MSG_TITLE_FAILED, LOG_ERROR, "Title #%d failed: %s", job->title, av_err2str(ret));
        res->failed = 1;
    } else {
        int64_t untested = meta_int(ctx->metadata, "untested");
        log_text(LOG_INFO, "Tracks:");
        for (unsigned i = 0; i < ctx->nb_streams; i++) {
            const AVStream *st = ctx->streams[i];
            EsOut *o = &out[i];
            int64_t frames = meta_int(st->metadata, "frames"), warn = meta_int(st->metadata, "warnings");
            char what[160], state[96], delay[48] = "", jc[64], jl[32], jy[96], jf[4300];
            const char *unit = st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE ? "subtitles" : "frames";

            snprintf(what, sizeof(what), "%s %s%s%s", *o->st.lang ? o->st.lang : "   ",
                     codec_label(st->codecpar->codec_id, st->codecpar->profile), *o->layout ? " " : "", o->layout);
            if (!o->opened)
                snprintf(state, sizeof(state), "no data: no file");
            else if (warn)
                snprintf(state, sizeof(state), "%"PRId64" warning%s", warn, warn == 1 ? "" : "s");
            else
                snprintf(state, sizeof(state), "OK");
            if (o->opened && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                snprintf(delay, sizeof(delay), ", delay %"PRId64" ms", o->st.delay_ms);
            log_text(!o->opened ? LOG_WARNING : warn ? LOG_INFO : LOG_OK, "  %02u  %-30s %8"PRId64" %-9s  %s%s",
                     i, what, frames, unit, state, delay);
            if (o->opened)
                log_text(LOG_DETAIL, "      %s", o->name);
            res->warnings += warn;
            log_json("track", "\"title\":%d,\"index\":%u,\"codec\":%s,\"language\":%s,\"layout\":%s,"
                     "\"frames\":%"PRId64",\"warnings\":%"PRId64",\"delay_ms\":%"PRId64",\"file\":%s", job->title, i,
                     json_str(jc, sizeof(jc), codec_label(st->codecpar->codec_id, st->codecpar->profile)),
                     json_str(jl, sizeof(jl), o->st.lang), json_str(jy, sizeof(jy), o->layout), frames, warn,
                     o->st.delay_ms, json_str(jf, sizeof(jf), o->opened ? o->name : ""));
        }
        log_msg(MSG_CHAPTERS, LOG_INFO, "Chapters: %u", ctx->nb_chapters);
        if (untested) {
            log_msg(MSG_UNTESTED, LOG_ERROR, "Title #%d: %"PRId64" feature(s) met that no real disc has tested: "
                    "its files are written, but check the log before using them", job->title, untested);
            res->failed = 1;
        } else if (res->warnings) {
            log_msg(MSG_TITLE_DONE, LOG_INFO, "Title #%d: done with %"PRId64" warning%s", job->title, res->warnings,
                    res->warnings == 1 ? "" : "s");
        } else {
            log_msg(MSG_TITLE_DONE, LOG_OK, "Title #%d: done", job->title);
        }
    }
    log_json("title_result", "\"title\":%d,\"status\":\"%s\",\"warnings\":%"PRId64",\"folder\":%s", job->title,
             res->failed ? "failed" : "ok", res->warnings, json_str(js, sizeof(js), dir));
    for (unsigned i = 0; ctx && out && i < ctx->nb_streams; i++) {
        EsOut *o = &out[i];
        for (int k = 0; k < o->nb_hold; k++)
            av_packet_free(&o->hold[k]);
        av_free(o->hold);
        av_frame_free(&o->frame);
        avcodec_free_context(&o->avctx);
        av_free(o->name);
    }
    av_free(out);
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return ret;
}
