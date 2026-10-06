/*
 * disc-remuxer: the log. Every line goes to the terminal (coloured by level
 * when it is one), to the job's log file (plain), and in JSON mode as one
 * JSON record per line on stdout instead of the terminal text.
 *
 * Levels: error, warning, ok (a step that went fine), info, detail (only in
 * the debug log and with -v). FFmpeg's own lines come in through its log
 * callback, without FFmpeg's "[name @ 0x...]" prefix.
 */

#ifndef DISC_REMUXER_LOG_H
#define DISC_REMUXER_LOG_H

#include <stdint.h>

enum LogLevel { LOG_ERROR, LOG_WARNING, LOG_OK, LOG_INFO, LOG_DETAIL };

/* json: JSON records on stdout instead of the terminal text; verbose: detail
 * lines on the terminal too. Installs FFmpeg's log callback. */
void log_init(int json, int verbose);

/* The job's log file (plain text, written as it goes), and with debug a
 * second file with every detail line. NULL closes them. Returns 0 or -1. */
int  log_open_files(const char *path, const char *debug_path);
void log_close_files(void);

/* A numbered message of the program. */
void log_msg(int code, enum LogLevel level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* A step heading ("=== Title 2/11: HD_Feature ===") and a blank line before it. */
void log_step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Plain text lines (tables, overviews) at a level, no number. */
void log_text(enum LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Progress of a title (0..1): a progress record in JSON mode, an updating
 * line on a terminal, nothing in the files. */
void log_progress(int title, double done);

/* A JSON record (JSON mode only): {"type":"<type>",<fields>} where fields
 * is printf-formatted JSON members ("\"index\":%d,\"name\":%s"; strings
 * through json_str). */
void log_json(const char *type, const char *fields, ...) __attribute__((format(printf, 2, 3)));

/* Errors and warnings logged since the counters were reset (for summaries). */
void log_counts(int *errors, int *warnings);
void log_reset_counts(void);

/* A string as a JSON string literal (with quotes) into buf. */
const char *json_str(char *buf, int size, const char *s);

#endif
