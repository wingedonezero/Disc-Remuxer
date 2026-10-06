/*
 * disc-remuxer: the log (see log.h).
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/log.h>

#include "log.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int  json_mode, verbose, color;
static FILE *log_file, *debug_file;
static int  progress_shown;
static int  n_errors, n_warnings;

/* FFmpeg's lines arrive in pieces; one is kept until its newline */
static char pending[4096];
static size_t pending_len;
static int  pending_level;
static const char *pending_source;

static const char *const word[] = { "ERROR", "WARNING", "OK", "INFO", "DETAIL" };

static void stamp(char *buf, size_t size)
{
    struct timeval tv;
    struct tm tm;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    snprintf(buf, size, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, (long)(tv.tv_usec / 1000));
}

const char *json_str(char *buf, int size, const char *s)
{
    int n = 0;

    if (size < 3)
        return "\"\"";
    buf[n++] = '"';
    for (; s && *s && n < size - 8; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') {
            buf[n++] = '\\';
            buf[n++] = c;
        } else if (c == '\n') {
            buf[n++] = '\\'; buf[n++] = 'n';
        } else if (c == '\t') {
            buf[n++] = '\\'; buf[n++] = 't';
        } else if (c < 0x20) {
            n += snprintf(buf + n, size - n, "\\u%04x", c);
        } else {
            buf[n++] = c;
        }
    }
    buf[n++] = '"';
    buf[n]   = 0;
    return buf;
}

static void clear_progress(void)
{
    if (progress_shown) {
        fputs("\r\033[K", stdout);
        progress_shown = 0;
    }
}

/* One finished line: terminal (or JSON), log file, debug file. code 0 = not
 * a numbered message; source = where a library line came from (NULL: ours). */
static void emit(enum LogLevel level, int code, const char *source, const char *text, int heading)
{
    char ts[32];

    if (level == LOG_ERROR)
        n_errors++;
    else if (level == LOG_WARNING)
        n_warnings++;

    if (json_mode) {
        if (level != LOG_DETAIL || verbose) {
            char t[8192];
            static const char *const lw[] = { "error", "warning", "ok", "info", "detail" };
            printf("{\"type\":\"message\",\"level\":\"%s\",\"code\":%d,\"source\":\"%s\",\"text\":%s}\n",
                   lw[level], code, source ? source : "disc-remuxer", json_str(t, sizeof(t), text));
            fflush(stdout);
        }
    } else if (level != LOG_DETAIL || verbose) {
        clear_progress();
        if (heading)
            printf("\n%s%s%s\n", color ? "\033[1m" : "", text, color ? "\033[0m" : "");
        else if (level == LOG_ERROR)
            printf("%sERROR%s   %s\n", color ? "\033[1;31m" : "", color ? "\033[0m" : "", text);
        else if (level == LOG_WARNING)
            printf("%sWARNING%s %s\n", color ? "\033[1;33m" : "", color ? "\033[0m" : "", text);
        else if (level == LOG_OK)
            printf("%s%s%s\n", color ? "\033[32m" : "", text, color ? "\033[0m" : "");
        else if (level == LOG_DETAIL)
            printf("%s%s%s\n", color ? "\033[2m" : "", text, color ? "\033[0m" : "");
        else
            printf("%s\n", text);
        fflush(stdout);
    }

    if (log_file && level != LOG_DETAIL) {
        if (heading)
            fprintf(log_file, "\n%s\n", text);
        else if (level == LOG_ERROR)
            fprintf(log_file, "ERROR   %s\n", text);
        else if (level == LOG_WARNING)
            fprintf(log_file, "WARNING %s\n", text);
        else
            fprintf(log_file, "%s\n", text);
        fflush(log_file);
    }
    if (debug_file) {
        stamp(ts, sizeof(ts));
        if (code)
            fprintf(debug_file, "%s %-7s [%d] %s\n", ts, word[level], code, text);
        else if (source)
            fprintf(debug_file, "%s %-7s [%s] %s\n", ts, word[level], source, text);
        else
            fprintf(debug_file, "%s %-7s %s%s\n", ts, word[level], heading ? "=== " : "", text);
    }
}

/* Library lines already shown in this run (a title opened again repeats
 * the disc's messages): a repeat is a detail line. */
#define SEEN_MAX 1024
static unsigned long seen[SEEN_MAX];
static int nb_seen;

static int seen_before(const char *text)
{
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        h = h * 33 + *p;
    for (int i = 0; i < nb_seen; i++)
        if (seen[i] == h)
            return 1;
    if (nb_seen < SEEN_MAX)
        seen[nb_seen++] = h;
    return 0;
}

static enum LogLevel from_av(int level)
{
    return level <= AV_LOG_ERROR ? LOG_ERROR : level <= AV_LOG_WARNING ? LOG_WARNING :
           level <= AV_LOG_INFO ? LOG_INFO : LOG_DETAIL;
}

/* FFmpeg's log callback: lines without FFmpeg's prefix, levels mapped;
 * TRACE lines only with -vv. */
static void av_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    char piece[4096];
    int n;

    if (level > AV_LOG_DEBUG && verbose < 2)
        return;
    if (level > AV_LOG_INFO && !debug_file && !verbose)
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
            if (pending_len) {
                enum LogLevel l = from_av(pending_level);
                if ((l == LOG_WARNING || l == LOG_INFO) && seen_before(pending))
                    l = LOG_DETAIL;
                emit(l, 0, pending_source, pending, 0);
            }
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

void log_init(int json, int verbosity)
{
    const char *nc = getenv("NO_COLOR");

    json_mode = json;
    verbose   = verbosity;
    color     = !json && isatty(STDOUT_FILENO) && !(nc && *nc);
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_callback(av_callback);
}

int log_open_files(const char *path, const char *debug_path)
{
    pthread_mutex_lock(&lock);
    if (path && !(log_file = fopen(path, "w"))) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    if (debug_path && !(debug_file = fopen(debug_path, "w"))) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

void log_close_files(void)
{
    pthread_mutex_lock(&lock);
    if (log_file)
        fclose(log_file);
    if (debug_file)
        fclose(debug_file);
    log_file = debug_file = NULL;
    pthread_mutex_unlock(&lock);
}

void log_msg(int code, enum LogLevel level, const char *fmt, ...)
{
    char text[4096];
    va_list vl;

    va_start(vl, fmt);
    vsnprintf(text, sizeof(text), fmt, vl);
    va_end(vl);
    pthread_mutex_lock(&lock);
    emit(level, code, NULL, text, 0);
    pthread_mutex_unlock(&lock);
}

void log_step(const char *fmt, ...)
{
    char text[1024];
    va_list vl;

    va_start(vl, fmt);
    vsnprintf(text, sizeof(text), fmt, vl);
    va_end(vl);
    pthread_mutex_lock(&lock);
    emit(LOG_INFO, 0, NULL, text, 1);
    pthread_mutex_unlock(&lock);
}

void log_text(enum LogLevel level, const char *fmt, ...)
{
    char text[4096];
    va_list vl;

    va_start(vl, fmt);
    vsnprintf(text, sizeof(text), fmt, vl);
    va_end(vl);
    pthread_mutex_lock(&lock);
    emit(level, 0, NULL, text, 0);
    pthread_mutex_unlock(&lock);
}

void log_progress(int title, double done)
{
    static int last_title = -1, last_pct = -1;
    int pct = (int)(done * 100 + 0.5);

    if (pct == last_pct && title == last_title)
        return;
    last_title = title;
    last_pct   = pct;
    pthread_mutex_lock(&lock);
    if (json_mode) {
        printf("{\"type\":\"progress\",\"title\":%d,\"done\":%.3f}\n", title, done);
        fflush(stdout);
    } else if (isatty(STDOUT_FILENO)) {
        printf("\r\033[K  %d%%", pct);
        fflush(stdout);
        progress_shown = 1;
    }
    pthread_mutex_unlock(&lock);
}

void log_json(const char *type, const char *fields, ...)
{
    char body[16384];
    va_list vl;

    if (!json_mode)
        return;
    va_start(vl, fields);
    vsnprintf(body, sizeof(body), fields, vl);
    va_end(vl);
    pthread_mutex_lock(&lock);
    printf("{\"type\":\"%s\"%s%s}\n", type, *body ? "," : "", body);
    fflush(stdout);
    pthread_mutex_unlock(&lock);
}

void log_counts(int *errors, int *warnings)
{
    *errors   = n_errors;
    *warnings = n_warnings;
}

void log_reset_counts(void)
{
    n_errors = n_warnings = 0;
}
