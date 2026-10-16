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

static DrVlogWantsFn wants;

void dr_vlog_set_wants(DrVlogWantsFn fn)
{
    wants = fn;
}

int dr_vlog_wants(int level)
{
    return wants ? wants(level) : 1;
}

void dr_vlog(const void *logctx, int level, const char *fmt, ...)
{
    va_list vl;

    if (!vlog)
        return;
    va_start(vl, fmt);
    vlog(logctx, level, fmt, vl);
    va_end(vl);
}
