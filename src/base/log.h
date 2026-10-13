/*
 * The log: the one funnel for the program and every library under it. A
 * line goes to the terminal (coloured by level when it is one), to the
 * job's log file (plain), and in JSON mode as one JSON record per line on
 * stdout instead of the terminal text — or, when a sink is installed (a
 * GUI), to the sink's callbacks instead of the terminal.
 *
 * Levels: error, warning, ok (a step that went fine), info, detail (only in
 * the debug log and with -v). Library lines come in through each library's
 * log hook calling log_library_line() with the library named as source.
 */

#ifndef DISC_BASE_LOG_H
#define DISC_BASE_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum LogLevel { LOG_ERROR, LOG_WARNING, LOG_OK, LOG_INFO, LOG_DETAIL };

/* Where the log goes besides the run's log files: NULL members print to the
 * terminal (text, or JSON records with json). level: LOG_* above; code: the
 * message number (msg.h), 0 none; source: the library a line comes from,
 * NULL ours; heading: a step's heading. record: one JSON record. */
typedef struct EngineSink {
    void (*line)(void *opaque, int level, int code, const char *source, const char *text, int heading);
    void (*progress)(void *opaque, int title, double done);
    void (*record)(void *opaque, const char *json);
    void  *opaque;
} EngineSink;

/* Log lines, progress and records to sink (NULL: the terminal again). */
void engine_set_sink(const EngineSink *sink);

/* json: JSON records on stdout instead of the terminal text; verbose:
 * detail lines on the terminal too (2: trace lines from the libraries). */
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

/* One finished line as it is: a library hook's line, or a line built
 * already. code 0 = not a numbered message; source = where a library line
 * came from (NULL: ours). */
void log_line(enum LogLevel level, int code, const char *source, const char *text, int heading);

/* A library's line: a repeat of one already shown this run (a title opened
 * again repeats the disc's messages) is demoted to a detail line, except
 * lines that are a title's own (the title timeline's findings and checks). */
void log_library_line(enum LogLevel level, const char *source, const char *text);

/* What a library hook may ask before building a line: the verbosity log_init
 * was given, and whether a debug file is open (detail lines kept). */
int  log_verbosity(void);
int  log_debug_file_open(void);

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

#ifdef __cplusplus
}
#endif

#endif
