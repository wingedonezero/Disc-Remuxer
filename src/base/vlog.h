/*
 * The library-line hook: how the extracted modules (src/io, src/mux,
 * src/parse) log. One process-wide hook; the program points it at its log
 * (today through FFmpeg's, so the lines read exactly as before; the test
 * bridge the same). Without a hook, lines are dropped. logctx is the
 * caller's opaque log context, passed back as given.
 */

#ifndef DISC_BASE_VLOG_H
#define DISC_BASE_VLOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

enum DrVlogLevel {
    DR_VLOG_ERROR, DR_VLOG_WARNING, DR_VLOG_INFO,
    DR_VLOG_VERBOSE, DR_VLOG_DEBUG, DR_VLOG_TRACE,
};

typedef void (*DrVlogFn)(const void *logctx, int level, const char *fmt, va_list vl);
/** Whether lines of level would be kept (modules skip building expensive
 *  dumps otherwise); without one, every level is wanted. */
typedef int (*DrVlogWantsFn)(int level);

void dr_vlog_set(DrVlogFn fn);
void dr_vlog_set_wants(DrVlogWantsFn fn);
int  dr_vlog_wants(int level);

void dr_vlog(const void *logctx, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif

#endif
