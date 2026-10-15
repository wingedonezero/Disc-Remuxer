/*
 * The log (see log.h). The modules' lines (base/vlog.h) come in through
 * the hook log_init() installs below; the processing library's own lines
 * through the analyze module's hook (src/analyze/log.c) — both end in
 * log_library_line().
 */

#define _GNU_SOURCE     /* vasprintf, asprintf (log_json) */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "base/log.h"
#include "base/vlog.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int  json_mode, verbose, color;
static FILE *log_file, *debug_file;
static int  progress_shown;
static int  n_errors, n_warnings;
static LogSink sink;            /* the caller's (a GUI's); members NULL: the terminal */

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
        } else if (c < 0x80) {
            buf[n++] = c;
        } else {
            /* UTF-8 as it is; bytes that are no UTF-8 (a library's raw
             * bytes) as U+FFFD, so the record stays valid JSON */
            int len = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0, ok = len > 0;
            for (int k = 1; ok && k < len; k++)
                ok = ((unsigned char)s[k] & 0xc0) == 0x80;
            if (ok && n + len < size - 8) {
                memcpy(buf + n, s, len);
                n += len;
                s += len - 1;
            } else {
                memcpy(buf + n, "\xef\xbf\xbd", 3);
                n += 3;
            }
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

    if (sink.line) {
        sink.line(sink.opaque, level, code, source, text, heading);
    } else if (json_mode) {
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

/* Library lines already shown in this run while a disc or a title was
 * opened (a title opened again repeats the disc's messages): a repeat is a
 * detail line. While a title is read (log_title_reading()) every line is
 * that title's own: always shown, and not kept as seen. */
#define SEEN_MAX 1024
static unsigned long seen[SEEN_MAX];
static int nb_seen;
static int title_reading;

void log_title_reading(int on)
{
    pthread_mutex_lock(&lock);
    title_reading = on;
    pthread_mutex_unlock(&lock);
}

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

/* ---- the modules' lines (base/vlog.h) ----
 * logctx is the module's source name (or NULL: "disc"); lines arrive whole
 * or in pieces, kept until the newline. ERROR / WARNING / INFO keep their
 * level; VERBOSE and DEBUG are detail lines (kept with -v or a debug
 * file); TRACE only with -vv. */

static pthread_mutex_t vlog_lock = PTHREAD_MUTEX_INITIALIZER;
static char vlog_pending[4096];
static size_t vlog_pending_len;
static enum LogLevel vlog_pending_level;
static const char *vlog_pending_source;

/* Whether lines of a module level would be kept (modules skip building
 * expensive dumps otherwise). */
static int vlog_wants(int level)
{
    if (level >= DR_VLOG_TRACE && verbose < 2)
        return 0;
    return level < DR_VLOG_VERBOSE || debug_file || verbose;
}

static void vlog_line(const void *logctx, int level, const char *fmt, va_list vl)
{
    static const enum LogLevel map[] = { LOG_ERROR, LOG_WARNING, LOG_INFO,
                                         LOG_DETAIL, LOG_DETAIL, LOG_DETAIL };
    const char *source = logctx ? logctx : "disc";
    char piece[4096];
    int n;

    if (!vlog_wants(level))
        return;
    n = vsnprintf(piece, sizeof(piece), fmt, vl);
    if (n < 0)
        return;
    if (n >= (int)sizeof(piece))
        n = sizeof(piece) - 1;
    pthread_mutex_lock(&vlog_lock);
    if (!vlog_pending_len) {
        vlog_pending_level  = map[level];
        vlog_pending_source = source;
    }
    for (int i = 0; i < n; i++) {
        if (piece[i] == '\n') {
            vlog_pending[vlog_pending_len] = 0;
            if (vlog_pending_len)
                log_library_line(vlog_pending_level, vlog_pending_source, vlog_pending);
            vlog_pending_len = 0;
            if (i + 1 < n) {
                vlog_pending_level  = map[level];
                vlog_pending_source = source;
            }
        } else if (vlog_pending_len < sizeof(vlog_pending) - 1) {
            vlog_pending[vlog_pending_len++] = piece[i];
        }
    }
    pthread_mutex_unlock(&vlog_lock);
}

void log_init(int json, int verbosity)
{
    const char *nc = getenv("NO_COLOR");

    json_mode = json;
    verbose   = verbosity;
    color     = !json && isatty(STDOUT_FILENO) && !(nc && *nc);
    dr_vlog_set(vlog_line);
    dr_vlog_set_wants(vlog_wants);
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
    if (sink.progress) {
        sink.progress(sink.opaque, title, done);
    } else if (json_mode) {
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
    char *body = NULL;
    va_list vl;
    int n;

    if (!json_mode)
        return;
    va_start(vl, fields);
    n = vasprintf(&body, fields, vl);   /* a record of any length (a title with many tracks) */
    va_end(vl);
    if (n < 0)
        return;
    pthread_mutex_lock(&lock);
    printf("{\"type\":\"%s\"%s%s}\n", type, *body ? "," : "", body);
    fflush(stdout);
    pthread_mutex_unlock(&lock);
    free(body);
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

void log_line(enum LogLevel level, int code, const char *source, const char *text, int heading)
{
    pthread_mutex_lock(&lock);
    emit(level, code, source, text, heading);
    pthread_mutex_unlock(&lock);
}

void log_library_line(enum LogLevel level, const char *source, const char *text)
{
    pthread_mutex_lock(&lock);
    if (!title_reading && (level == LOG_WARNING || level == LOG_INFO) && seen_before(text))
        level = LOG_DETAIL;
    emit(level, 0, source, text, 0);
    pthread_mutex_unlock(&lock);
}

int log_verbosity(void)
{
    return verbose;
}

int log_debug_file_open(void)
{
    return debug_file != NULL;
}

int log_json_mode(void)
{
    return json_mode;
}

void log_set_sink(const LogSink *s)
{
    pthread_mutex_lock(&lock);
    if (s)
        sink = *s;
    else
        memset(&sink, 0, sizeof(sink));
    pthread_mutex_unlock(&lock);
}
