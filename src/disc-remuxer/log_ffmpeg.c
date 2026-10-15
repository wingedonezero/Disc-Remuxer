/*
 * FFmpeg's log hook (see log_ffmpeg.h). FFmpeg logs in pieces: one line is
 * kept until its newline, then handed to the log as a library line.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include <libavutil/log.h>
#include "analyze/analyze.h"

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

/* One piece of a line (lines arrive whole or in pieces; kept until the
 * newline), under the lock. */
static void emit_piece(int level, const char *source, const char *piece, int n)
{
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

/* Lines of these levels are dropped (TRACE lines only with -vv). */
static int drop_av_level(int level)
{
    return (level > AV_LOG_DEBUG && log_verbosity() < 2) ||
           (level > AV_LOG_INFO && !log_debug_file_open() && !log_verbosity());
}

static void av_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    AVClass *c = avcl ? *(AVClass **)avcl : NULL;
    char piece[4096];
    int n;

    if (drop_av_level(level))
        return;
    n = vsnprintf(piece, sizeof(piece), fmt, vl);
    if (n < 0)
        return;
    if (n >= (int)sizeof(piece))
        n = sizeof(piece) - 1;
    emit_piece(level, c ? c->item_name(avcl) : "ffmpeg", piece, n);
}

static const int to_av[] = { AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_INFO,
                             AV_LOG_VERBOSE, AV_LOG_DEBUG, AV_LOG_TRACE };

/* The modules' lines (base/vlog.h: the disc layer and everything under
 * it): logctx is the module's source name (dr_disc_open()'s log), or NULL. */
static void module_vlog(const void *logctx, int level, const char *fmt, va_list vl)
{
    char piece[4096];
    int n;

    if (drop_av_level(to_av[level]))
        return;
    n = vsnprintf(piece, sizeof(piece), fmt, vl);
    if (n < 0)
        return;
    if (n >= (int)sizeof(piece))
        n = sizeof(piece) - 1;
    emit_piece(to_av[level], logctx ? (const char *)logctx : "disc", piece, n);
}

/* Whether module lines of the level would be kept (the modules skip
 * building expensive dumps otherwise). */
static int module_wants(int level)
{
    return !drop_av_level(to_av[level]);
}

void log_ffmpeg_install(void)
{
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_callback(av_callback);
    dr_vlog_set(module_vlog);
    dr_vlog_set_wants(module_wants);
    dr_analyze_use();
}
