/*
 * A growing text buffer: formatted pieces appended to one heap string.
 * Start it with DR_STR_INIT (or dr_str_init()), append with dr_str_catf(),
 * read s->str / s->len, check dr_str_ok() once at the end (an allocation
 * failure makes every later append a no-op), free with dr_str_free() —
 * or hand the string over with dr_str_take().
 */

#ifndef DISC_BASE_STR_H
#define DISC_BASE_STR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DrStr {
    char   *str;        /**< the text so far ("" while empty); always 0-terminated while ok */
    size_t  len;        /**< bytes in it (the terminator not counted) */
    size_t  cap;
    int     failed;     /**< an allocation failed: the text is incomplete */
} DrStr;

#define DR_STR_INIT ((DrStr){ "", 0, 0, 0 })

void dr_str_init(DrStr *s);

/** Append the formatted text (nothing on a failed buffer). */
void dr_str_catf(DrStr *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/** Append len bytes as they are. */
void dr_str_cat_data(DrStr *s, const char *data, size_t len);

/** 1 while everything appended is in the buffer, 0 after a failure. */
int dr_str_ok(const DrStr *s);

/** The string, handed to the caller (dr_free(); NULL after a failure or
 *  when nothing was appended and keep_empty is 0); the buffer is reset. */
char *dr_str_take(DrStr *s, int keep_empty);

void dr_str_free(DrStr *s);

#ifdef __cplusplus
}
#endif

#endif /* DISC_BASE_STR_H */
