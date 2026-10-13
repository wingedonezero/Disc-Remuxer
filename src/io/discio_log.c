/*
 * The io module's log (see discio.h): lines go to the hook the program
 * installs; without one they are dropped.
 */

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>

#include "io/discio.h"

static DrIoVlog vlog;

void dr_io_set_vlog(DrIoVlog fn)
{
    vlog = fn;
}

void dr_io_log(void *logctx, int level, const char *fmt, ...)
{
    va_list vl;

    if (!vlog)
        return;
    va_start(vl, fmt);
    vlog(logctx, level, fmt, vl);
    va_end(vl);
}
