/*
 * C glue between FFmpeg and the Rust side, for the parts that are awkward to
 * call from Rust directly (varargs log callback, option dictionaries).
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>

typedef void (*dr_log_sink)(int level, const char *line);

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static dr_log_sink log_sink;
static int log_max_level = AV_LOG_INFO;
static char log_pending[4096];
static size_t log_pending_len;
static int log_print_prefix = 1;

/* FFmpeg writes a line in several calls (av_dump_format, ...): pieces are
 * collected until the newline, then the whole line goes to the sink with the
 * level of its last piece. */
static void log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    char part[2048];
    size_t len;

    if (level > log_max_level)
        return;
    pthread_mutex_lock(&log_lock);
    av_log_format_line2(avcl, level, fmt, vl, part, sizeof(part), &log_print_prefix);
    len = strlen(part);
    if (len > sizeof(log_pending) - 1 - log_pending_len)
        len = sizeof(log_pending) - 1 - log_pending_len;
    memcpy(log_pending + log_pending_len, part, len);
    log_pending_len += len;
    log_pending[log_pending_len] = 0;
    if (log_pending_len > 0 && log_pending[log_pending_len - 1] == '\n') {
        log_pending[--log_pending_len] = 0;
        if (log_sink)
            log_sink(level, log_pending);
        log_pending_len = 0;
    }
    pthread_mutex_unlock(&log_lock);
}

void dr_log_install(dr_log_sink sink, int max_level)
{
    pthread_mutex_lock(&log_lock);
    log_sink = sink;
    log_max_level = max_level;
    pthread_mutex_unlock(&log_lock);
    av_log_set_level(max_level);
    av_log_set_callback(log_callback);
}

typedef void (*dr_parser_frame_cb)(void *opaque, int size, int64_t pts, int64_t dts);

/* Runs FFmpeg's parser for the codec named `codec_name` (a codec descriptor
 * name such as "ac3") over `nb` packets the way libavformat does: each
 * packet's timestamp is passed on its first parser call only, the rest of the
 * packet follows without one, and the parser is flushed at the end. `cb` gets
 * every frame the parser returns with the timestamps the parser gave it.
 * Returns 0 or a negative AVERROR. */
int dr_parser_run(const char *codec_name, const uint8_t *const *data,
                  const int *sizes, const int64_t *pts, int nb,
                  dr_parser_frame_cb cb, void *opaque)
{
    const AVCodecDescriptor *desc = avcodec_descriptor_get_by_name(codec_name);
    AVCodecParserContext *parser;
    AVCodecContext *avctx;
    int64_t pos = 0;
    uint8_t *out;
    int out_size;

    if (!desc)
        return AVERROR_DECODER_NOT_FOUND;
    parser = av_parser_init(desc->id);
    if (!parser)
        return AVERROR(ENOSYS);
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        av_parser_close(parser);
        return AVERROR(ENOMEM);
    }
    avctx->codec_id   = desc->id;
    avctx->codec_type = desc->type;

    for (int i = 0; i < nb; i++) {
        const uint8_t *p = data[i];
        int left = sizes[i];
        int64_t t = pts[i], packet_pos = pos;

        pos += sizes[i];
        while (left > 0) {
            int used = av_parser_parse2(parser, avctx, &out, &out_size, p, left,
                                        t, t, packet_pos);
            t          = AV_NOPTS_VALUE;
            packet_pos = -1;
            p    += used;
            left -= used;
            if (out_size)
                cb(opaque, out_size, parser->pts, parser->dts);
        }
    }
    do {
        av_parser_parse2(parser, avctx, &out, &out_size, NULL, 0,
                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);
        if (out_size)
            cb(opaque, out_size, parser->pts, parser->dts);
    } while (out_size);

    av_parser_close(parser);
    avcodec_free_context(&avctx);
    return 0;
}

/* Opens one title with FFmpeg's DVD-Video demuxer, reads stream information
 * and logs FFmpeg's stream dump. Returns 0 or a negative AVERROR. */
int dr_probe_dvdvideo(const char *path, const char *options)
{
    const AVInputFormat *fmt = av_find_input_format("dvdvideo");
    AVFormatContext *ctx = NULL;
    AVDictionary *opts = NULL;
    int ret;

    if (!fmt)
        return AVERROR_DEMUXER_NOT_FOUND;
    if (options && (ret = av_dict_parse_string(&opts, options, "=", ":", 0)) < 0)
        return ret;
    ret = avformat_open_input(&ctx, path, fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0)
        return ret;
    ret = avformat_find_stream_info(ctx, NULL);
    if (ret >= 0) {
        av_dump_format(ctx, 0, path, 0);
        ret = 0;
    }
    avformat_close_input(&ctx);
    return ret;
}

/* ---- elementary-stream output ---- */

/* One stream's statistics after demuxing (timestamps in the stream's time
 * base, 90 kHz for the disc demuxers; decode timestamps where given). */
typedef struct DrStreamStats {
    int         index;
    const char *kind, *codec, *lang, *file;     /* file "" = not written (a track carried by another) */
    int64_t     packets, bytes, no_ts;
    int64_t     first_ts, end_ts;               /* first timestamp, last timestamp + duration */
    int64_t     overlaps, max_overlap;          /* packets starting before the previous one ended */
    int64_t     gaps, max_gap;                  /* packets starting after the previous one ended */
} DrStreamStats;

typedef void (*dr_demux_stream_cb)(void *opaque, const DrStreamStats *st);

static const char *es_extension(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_MPEG2VIDEO:   return "m2v";
    case AV_CODEC_ID_VC1:          return "vc1";
    case AV_CODEC_ID_H264:         return "h264";
    case AV_CODEC_ID_AC3:          return "ac3";
    case AV_CODEC_ID_EAC3:         return "eac3";
    case AV_CODEC_ID_TRUEHD:       return "thd";
    case AV_CODEC_ID_MLP:          return "mlp";
    case AV_CODEC_ID_DTS:          return "dts";
    case AV_CODEC_ID_MP2:          return "mp2";
    case AV_CODEC_ID_PCM_DVD:      return "lpcm";
    case AV_CODEC_ID_DVD_SUBTITLE: return "spu";
    default:                       return "bin";
    }
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

/* Opens title `title` of path with demuxer `format` (options "k=v:k=v",
 * key_files set as the demuxer's keydb option when not NULL) and writes every
 * stream's packets as they come to <outdir>/<prefix><index>_<lang>.<ext>, the
 * chapters to <outdir>/<prefix>chapters.xml. *nb_titles gets the demuxer's
 * "titles" metadata (or -1). cb gets each stream's statistics at the end.
 * Returns 0 or a negative AVERROR. */
int dr_demux(const char *format, const char *path, const char *options, const char *key_files,
             const char *outdir, const char *prefix, int *nb_titles, dr_demux_stream_cb cb, void *opaque)
{
    const AVInputFormat *fmt = av_find_input_format(format);
    AVFormatContext *ctx = NULL;
    AVDictionary *opts = NULL;
    const AVDictionaryEntry *e;
    FILE **files = NULL;
    DrStreamStats *stats = NULL;
    char **names = NULL;
    AVPacket *pkt = NULL;
    int ret;

    *nb_titles = -1;
    if (!fmt)
        return AVERROR_DEMUXER_NOT_FOUND;
    if (options && (ret = av_dict_parse_string(&opts, options, "=", ":", 0)) < 0)
        return ret;
    if (key_files && *key_files && (ret = av_dict_set(&opts, "keydb", key_files, 0)) < 0)
        goto end;
    if ((ret = avformat_open_input(&ctx, path, fmt, &opts)) < 0)
        goto end;
    if ((e = av_dict_get(ctx->metadata, "titles", NULL, 0)))
        *nb_titles = atoi(e->value);
    files = av_calloc(ctx->nb_streams, sizeof(*files));
    stats = av_calloc(ctx->nb_streams, sizeof(*stats));
    names = av_calloc(ctx->nb_streams, sizeof(*names));
    pkt   = av_packet_alloc();
    if (!files || !stats || !names || !pkt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (unsigned i = 0; i < ctx->nb_streams; i++) {
        const AVStream *st = ctx->streams[i];
        const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        DrStreamStats *s = &stats[i];

        s->index     = i;
        s->kind      = av_get_media_type_string(st->codecpar->codec_type);
        s->codec     = avcodec_get_name(st->codecpar->codec_id);
        s->lang      = lang ? lang->value : "";
        s->first_ts  = s->end_ts = AV_NOPTS_VALUE;
        if (st->disposition & AV_DISPOSITION_DEPENDENT) {
            s->file = "";       /* a track carried by another one's packets */
            continue;
        }
        if (!(names[i] = av_asprintf("%s/%s%02u_%s.%s", outdir, prefix, i, lang ? lang->value : "und",
                                     es_extension(st->codecpar->codec_id)))) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        s->file = names[i];
        if (!(files[i] = fopen(names[i], "wb"))) {
            ret = AVERROR(errno);
            goto end;
        }
    }
    while ((ret = av_read_frame(ctx, pkt)) >= 0) {
        DrStreamStats *s = &stats[pkt->stream_index];
        int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;

        s->packets++;
        s->bytes += pkt->size;
        if (files[pkt->stream_index] && fwrite(pkt->data, 1, pkt->size, files[pkt->stream_index]) != (size_t)pkt->size) {
            ret = AVERROR(EIO);
            av_packet_unref(pkt);
            goto end;
        }
        if (ts == AV_NOPTS_VALUE) {
            s->no_ts++;
        } else {
            if (s->first_ts == AV_NOPTS_VALUE)
                s->first_ts = ts;
            else if (ts < s->end_ts) {
                av_log(ctx, AV_LOG_VERBOSE, "stream %d: packet at %.3f s starts %.3f ms before the previous one ended\n",
                       pkt->stream_index, ts / 90000.0, (s->end_ts - ts) / 90.0);
                s->overlaps++;
                s->max_overlap = FFMAX(s->max_overlap, s->end_ts - ts);
            } else if (ts > s->end_ts && pkt->duration > 0) {
                av_log(ctx, AV_LOG_VERBOSE, "stream %d: packet at %.3f s starts %.3f ms after the previous one ended\n",
                       pkt->stream_index, ts / 90000.0, (ts - s->end_ts) / 90.0);
                s->gaps++;
                s->max_gap = FFMAX(s->max_gap, ts - s->end_ts);
            }
            s->end_ts = ts + FFMAX(pkt->duration, 0);
        }
        av_packet_unref(pkt);
    }
    if (ret == AVERROR_EOF)
        ret = 0;
    if (ret >= 0) {
        char *chap = av_asprintf("%s/%schapters.xml", outdir, prefix);
        ret = chap ? write_chapters(ctx, chap) : AVERROR(ENOMEM);
        av_free(chap);
    }
end:
    for (unsigned i = 0; ctx && files && i < ctx->nb_streams; i++)
        if (files[i] && fclose(files[i]) && ret >= 0)
            ret = AVERROR(errno);
    for (unsigned i = 0; ctx && stats && ret >= 0 && i < ctx->nb_streams; i++)
        cb(opaque, &stats[i]);
    for (unsigned i = 0; ctx && names && i < ctx->nb_streams; i++)
        av_free(names[i]);
    av_free(names);
    av_free(files);
    av_free(stats);
    av_packet_free(&pkt);
    av_dict_free(&opts);
    avformat_close_input(&ctx);
    return ret;
}
