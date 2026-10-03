/*
 * C glue between FFmpeg and the Rust side, for the parts that are awkward to
 * call from Rust directly (varargs log callback, option dictionaries).
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>

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

/* Opens one title with FFmpeg's DVD-Video demuxer, reads stream information
 * and logs FFmpeg's stream dump. Returns 0 or a negative AVERROR. */
int dr_probe_dvdvideo(const char *path, int title)
{
    const AVInputFormat *fmt = av_find_input_format("dvdvideo");
    AVFormatContext *ctx = NULL;
    AVDictionary *opts = NULL;
    char value[16];
    int ret;

    if (!fmt)
        return AVERROR_DEMUXER_NOT_FOUND;
    snprintf(value, sizeof(value), "%d", title);
    av_dict_set(&opts, "title", value, 0);
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
