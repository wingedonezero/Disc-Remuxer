/*
 * FFmpeg's log hook (see log_ffmpeg.h). FFmpeg logs in pieces: one line is
 * kept until its newline, then handed to the log as a library line.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include <libavutil/log.h>

#include "base/log.h"
#include "base/vlog.h"
#include "log_ffmpeg.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char pending[4096];
static size_t pending_len;
static int  pending_level;
static const char *pending_source;
static int  ffmpeg_detail;

void log_ffmpeg_detail(int on)
{
    ffmpeg_detail = on;
}

static enum LogLevel from_av(int level)
{
    if (ffmpeg_detail && level > AV_LOG_ERROR)
        return LOG_DETAIL;
    return level <= AV_LOG_ERROR ? LOG_ERROR : level <= AV_LOG_WARNING ? LOG_WARNING :
           level <= AV_LOG_INFO ? LOG_INFO : LOG_DETAIL;
}

/* TRACE lines only with -vv. */
static void av_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    char piece[4096];
    int n;

    if (level > AV_LOG_DEBUG && log_verbosity() < 2)
        return;
    if (level > AV_LOG_INFO && !log_debug_file_open() && !log_verbosity())
        return;
    n = vsnprintf(piece, sizeof(piece), fmt, vl);
    if (n < 0)
        return;
    if (n >= (int)sizeof(piece))
        n = sizeof(piece) - 1;

    pthread_mutex_lock(&lock);
    if (!pending_len) {
        AVClass *c = avcl ? *(AVClass **)avcl : NULL;
        pending_level  = level;
        pending_source = c ? c->item_name(avcl) : "ffmpeg";
    }
    for (int i = 0; i < n; i++) {
        if (piece[i] == '\n') {
            pending[pending_len] = 0;
            if (pending_len)
                log_library_line(from_av(pending_level), pending_source, pending);
            pending_len = 0;
            if (i + 1 < n) {
                AVClass *c = avcl ? *(AVClass **)avcl : NULL;
                pending_level  = level;
                pending_source = c ? c->item_name(avcl) : "ffmpeg";
            }
        } else if (pending_len < sizeof(pending) - 1) {
            pending[pending_len++] = piece[i];
        }
    }
    pthread_mutex_unlock(&lock);
}

/* The extracted modules' lines (base/vlog.h: src/io, src/mux, src/parse)
 * go through FFmpeg's log, so the source names stay those of the callers'
 * AVClass contexts, until the disc layer leaves libavformat. */
static void module_vlog(void *logctx, int level, const char *fmt, va_list vl)
{
    static const int map[] = { AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_INFO,
                               AV_LOG_VERBOSE, AV_LOG_DEBUG, AV_LOG_TRACE };
    av_vlog(logctx, map[level], fmt, vl);
}

void log_ffmpeg_install(void)
{
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_callback(av_callback);
    dr_vlog_set(module_vlog);
}
