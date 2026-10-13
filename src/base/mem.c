/*
 * Memory (see mem.h).
 */

#define _GNU_SOURCE     /* vasprintf */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "base/mem.h"

void *dr_malloc(size_t size)
{
    return malloc(size ? size : 1);
}

void *dr_mallocz(size_t size)
{
    return calloc(1, size ? size : 1);
}

void *dr_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size)
        return NULL;
    return calloc(n ? n : 1, size ? size : 1);
}

void *dr_realloc(void *p, size_t size)
{
    return realloc(p, size ? size : 1);
}

void *dr_realloc_array(void *p, size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size)
        return NULL;
    return realloc(p, n && size ? n * size : 1);
}

void *dr_malloc_array(size_t n, size_t size)
{
    return dr_realloc_array(NULL, n, size);
}

void dr_freep(void *p)
{
    void **pp = p;

    free(*pp);
    *pp = NULL;
}

char *dr_strdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

void *dr_memdup(const void *p, size_t size)
{
    void *out;

    if (!p)
        return NULL;
    out = dr_malloc(size);
    if (out)
        memcpy(out, p, size);
    return out;
}

size_t dr_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);

    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

size_t dr_strlcat(char *dst, const char *src, size_t size)
{
    size_t used = 0;

    while (used < size && dst[used])
        used++;
    if (used == size)
        return size + strlen(src);
    return used + dr_strlcpy(dst + used, src, size - used);
}

int dr_asprintf(char **out, const char *fmt, ...)
{
    va_list vl;
    int n;

    va_start(vl, fmt);
    n = vasprintf(out, fmt, vl);
    va_end(vl);
    if (n < 0)
        *out = NULL;
    return n;
}
