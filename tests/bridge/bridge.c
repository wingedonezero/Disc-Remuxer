/*
 * The C side of the test bridge: the headers the tests call into, and a few
 * helpers for what is awkward to do from Python (FFmpeg's varargs log
 * callback, running a parser the way libavformat does).
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/codec_desc.h"
#include "libavformat/avformat.h"
#include "libavutil/log.h"

#include "libavformat/discio.h"
#include "libavformat/discrip.h"
#include "libavformat/dvdvideo_internal.h"
#include "libavformat/hddvd_internal.h"

#include <dvdread/dvd_udf.h>
#include <dvdcss/dvdcss.h>

/* dvdnav_selftest.c and libdvdnav's VM (src/vm/rand.h) */
uint32_t vm_rand_next(uint32_t *state);
void vm_rand_seed(uint32_t *state, uint32_t value);
int vm_rand_shuffle(uint32_t state, unsigned int bound, unsigned int step);
int dr_selftest_vm_exec(const uint8_t command[8], int nr_of_programs, int pgN,
                        unsigned *failures, const char **first);
int dr_selftest_vm_run(const uint8_t *commands, int nb_commands, int nr_of_programs, int pgN,
                       uint32_t ticks_between, int (*rnd)(void *), void *rnd_priv,
                       uint16_t gprm[16], int *pg_n, unsigned *failures,
                       unsigned *ignored, int *ign_reg, int *ign_value);

typedef void (*tb_log_sink)(int level, const char *line);

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static tb_log_sink log_sink;
static int log_max_level = AV_LOG_INFO;
static char log_pending[4096];
static size_t log_pending_len;
static int log_print_prefix = 1;

/* FFmpeg writes a line in several calls: pieces are collected until the
 * newline, then the whole line goes to the sink with the level of its last
 * piece. */
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

/* Route FFmpeg's log (and the libraries' through it) to sink, up to
 * max_level. */
static void tb_log_install(tb_log_sink sink, int max_level)
{
    pthread_mutex_lock(&log_lock);
    log_sink = sink;
    log_max_level = max_level;
    pthread_mutex_unlock(&log_lock);
    av_log_set_level(max_level);
    av_log_set_callback(log_callback);
}

typedef void (*tb_parser_frame_cb)(void *opaque, int size, int64_t pts, int64_t dts);

/* Runs FFmpeg's parser for the codec named codec_name (a codec descriptor
 * name such as "ac3") over nb packets the way libavformat does: each
 * packet's timestamp is passed on its first parser call only, the rest of the
 * packet follows without one, and the parser is flushed at the end. cb gets
 * every frame the parser returns with the timestamps the parser gave it.
 * Returns 0 or a negative AVERROR. */
static int tb_parser_run(const char *codec_name, const uint8_t *const *data,
                         const int *sizes, const int64_t *pts, int nb,
                         tb_parser_frame_cb cb, void *opaque)
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

/* libdvdcss: its log callback takes a va_list, which cannot reach Python;
 * the shim passes the level and the format string on. */
void tb_py_css_log(void *p_log, int level, const char *format);

static void css_log(void *p_log, int level, const char *format, va_list args)
{
    tb_py_css_log(p_log, level, format);
}

/* dvdcss_open_stream_uncached() with the log going to tb_py_css_log (with_log)
 * or nowhere. */
static dvdcss_t tb_dvdcss_open_stream_uncached(void *p_stream, dvdcss_stream_cb *p_stream_cb,
                                               int with_log, void *p_log)
{
    return dvdcss_open_stream_uncached(p_stream, p_stream_cb, with_log ? css_log : NULL, p_log);
}
