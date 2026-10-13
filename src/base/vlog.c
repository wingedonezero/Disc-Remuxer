/*
 * The library-line hook (see vlog.h).
 */

#include <stdarg.h>
#include <stddef.h>

#include "base/vlog.h"

static DrVlogFn vlog;

void dr_vlog_set(DrVlogFn fn)
{
    vlog = fn;
}

void dr_vlog(void *logctx, int level, const char *fmt, ...)
{
    va_list vl;

    if (!vlog)
        return;
    va_start(vl, fmt);
    vlog(logctx, level, fmt, vl);
    va_end(vl);
}
