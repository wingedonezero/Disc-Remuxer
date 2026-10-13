/*
 * The growing text buffer (see str.h).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/str.h"

void dr_str_init(DrStr *s)
{
    *s = DR_STR_INIT;
}

static int room(DrStr *s, size_t more)
{
    size_t need = s->len + more + 1, cap = s->cap ? s->cap : 256;
    char *p;

    if (s->failed)
        return 0;
    if (s->cap && need <= s->cap)
        return 1;
    while (cap < need)
        cap *= 2;
    if (!(p = dr_malloc(cap))) {
        s->failed = 1;
        return 0;
    }
    memcpy(p, s->str, s->len + (s->cap != 0));
    p[s->len] = 0;
    if (s->cap)
        dr_free(s->str);
    s->str = p;
    s->cap = cap;
    return 1;
}

void dr_str_catf(DrStr *s, const char *fmt, ...)
{
    va_list ap, ap2;
    int n;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    if (n >= 0 && room(s, n)) {
        vsnprintf(s->str + s->len, n + 1, fmt, ap2);
        s->len += n;
    } else if (n < 0) {
        s->failed = 1;
    }
    va_end(ap2);
    va_end(ap);
}

int dr_str_ok(const DrStr *s)
{
    return !s->failed;
}

char *dr_str_take(DrStr *s, int keep_empty)
{
    char *out = NULL;

    if (!s->failed && (s->cap || keep_empty)) {
        if (!s->cap)
            out = dr_strdup("");
        else
            out = s->str;
    } else if (s->cap) {
        dr_free(s->str);
    }
    *s = DR_STR_INIT;
    return out;
}

void dr_str_free(DrStr *s)
{
    if (s->cap)
        dr_free(s->str);
    *s = DR_STR_INIT;
}
