/*
 * Errors (see err.h).
 */

#define _GNU_SOURCE     /* strerror_r, the GNU form */

#include <stdio.h>
#include <string.h>

#include "base/err.h"

const char *dr_err_str(int err, char *buf, size_t size)
{
    const char *text = NULL;

    switch (err) {
    case DR_OK:            text = "success";                       break;
    case DR_ERR_EOF:       text = "end of the data";               break;
    case DR_ERR_INVALID:   text = "data that breaks its own rules"; break;
    case DR_ERR_NOMEM:     text = "out of memory";                 break;
    case DR_ERR_LIMIT:     text = "a bound was reached";           break;
    case DR_ERR_CANCELLED: text = "stopped";                       break;
    case DR_ERR_EXTERNAL:  text = "a library or helper failed";    break;
    case DR_ERR_UNSUPPORTED: text = "data the program does not handle"; break;
    }
    if (text) {
        snprintf(buf, size, "%s", text);
    } else if (err < 0) {
        const char *t = strerror_r(-err, buf, size);
        if (t != buf)
            snprintf(buf, size, "%s", t);
    } else {
        snprintf(buf, size, "error %d", err);
    }
    return buf;
}
