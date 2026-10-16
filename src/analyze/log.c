/*
 * The processing library's log, routed into ours: lines without the
 * library's "[name @ 0x...]" prefix, the source named from the AVClass,
 * levels mapped, each line handed to log_library_line(). The library logs
 * in pieces: one line is kept until its newline. Only analyze's own use of
 * the library logs this way; the modules log through base/vlog.h.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include <libavutil/log.h>

#include "analyze/analyze.h"
#include "base/log.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char pending[4096];
static size_t pending_len;
static int  pending_level;
static const char *pending_source;
static int  read_back;

void dr_analyze_log_detail(int on)
{
    read_back = on;
}

static enum LogLevel from_av(int level)
{
    if (read_back && level > AV_LOG_ERROR)
        return LOG_DETAIL;
    return level <= AV_LOG_ERROR ? LOG_ERROR : level <= AV_LOG_WARNING ? LOG_WARNING :
           level <= AV_LOG_INFO ? LOG_INFO : LOG_DETAIL;
}

/* TRACE lines only with -vv. */
static void av_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    AVClass *c = avcl ? *(AVClass **)avcl : NULL;
    const char *source = c ? c->item_name(avcl) : "ffmpeg";
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
        pending_level  = level;
        pending_source = source;
    }
    for (int i = 0; i < n; i++) {
        if (piece[i] == '\n') {
            pending[pending_len] = 0;
            if (pending_len)
                log_library_line(from_av(pending_level), pending_source, pending);
            pending_len = 0;
            if (i + 1 < n) {
                pending_level  = level;
                pending_source = source;
            }
        } else if (pending_len < sizeof(pending) - 1) {
            pending[pending_len++] = piece[i];
        }
    }
    pthread_mutex_unlock(&lock);
}

void dr_analyze_log_install(void)
{
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_callback(av_callback);
}
