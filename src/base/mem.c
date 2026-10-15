/*
 * Memory (see mem.h).
 */

#define _GNU_SOURCE     /* vasprintf */

#include <stdarg.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>

#include "base/err.h"
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

uint32_t dr_random_seed(void)
{
    uint32_t seed;
    FILE *f = fopen("/dev/urandom", "re");

    if (f) {
        size_t n = fread(&seed, 1, sizeof(seed), f);
        fclose(f);
        if (n == sizeof(seed))
            return seed;
    }
    return (uint32_t)time(NULL) ^ (uint32_t)clock();
}

size_t dr_strlcatf(char *dst, size_t size, const char *fmt, ...)
{
    size_t used = strlen(dst);
    va_list vl;
    int n;

    va_start(vl, fmt);
    n = vsnprintf(dst + (used < size ? used : size), used < size ? size - used : 0, fmt, vl);
    va_end(vl);
    return used + (n < 0 ? 0 : (size_t)n);
}

char *dr_asprintf_str(const char *fmt, ...)
{
    char *out;
    va_list vl;
    int n;

    va_start(vl, fmt);
    n = vasprintf(&out, fmt, vl);
    va_end(vl);
    return n < 0 ? NULL : out;
}

char *dr_strndup(const char *s, size_t n)
{
    size_t len = 0;
    char *p;

    if (!s)
        return NULL;
    while (len < n && s[len])
        len++;
    if (!(p = dr_malloc(len + 1)))
        return NULL;
    memcpy(p, s, len);
    p[len] = 0;
    return p;
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

/* The arrays grow geometrically: the capacity is the next power of two
 * above the count (at least 4), so a realloc happens only when the count
 * reaches one. */
static void *array_grow(void *array, int nb, size_t elem_size)
{
    void **arr = array;

    if (nb >= 4 && (nb & (nb - 1)))
        return *arr;                      /* inside the current capacity */
    return dr_realloc_array(*arr, nb < 4 ? 4 : (size_t)nb * 2, elem_size);
}

int dr_array_add_ptr(void *array, int *nb, void *item)
{
    void ***arr = array;
    void **p = array_grow(array, *nb, sizeof(void *));

    if (!p)
        return DR_ERR_NOMEM;
    *arr = p;
    p[(*nb)++] = item;
    return 0;
}

void *dr_array_add(void *array, int *nb, size_t elem_size, const void *data)
{
    char **arr = array;
    char *p = array_grow(array, *nb, elem_size);

    if (!p)
        return NULL;
    *arr = p;
    p += (size_t)(*nb)++ * elem_size;
    if (data)
        memcpy(p, data, elem_size);
    return p;
}
