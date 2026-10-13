/*
 * Errors: 0 = success, negative = an error. System errors are -errno;
 * the program's own conditions are the DR_ERR_* block (far outside errno's
 * range). Modules return int; dr_err_str() puts any of them into words.
 */

#ifndef DISC_BASE_ERR_H
#define DISC_BASE_ERR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DR_OK 0

/* A system error from errno (always negative, even if e is 0). */
#define DR_ERR(e) ((e) > 0 ? -(e) : -1)

enum {
    DR_ERR_EOF       = -0x44520001,   /* end of the data */
    DR_ERR_INVALID   = -0x44520002,   /* data that breaks its own format's rules */
    DR_ERR_NOMEM     = -0x44520003,   /* allocation failed */
    DR_ERR_LIMIT     = -0x44520004,   /* a bound of ours was reached */
    DR_ERR_CANCELLED = -0x44520005,   /* the run was stopped */
    DR_ERR_EXTERNAL  = -0x44520006,   /* a library or helper program failed */
};

/* The error in words (errno text, or the DR_ERR_* meaning), into buf;
 * returns buf. */
const char *dr_err_str(int err, char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
