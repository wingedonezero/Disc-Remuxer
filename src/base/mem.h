/*
 * Memory: thin wrappers over libc that keep one convention — NULL on
 * failure, size overflow checked, dr_freep() clears the pointer. No
 * aborting: callers check.
 */

#ifndef DISC_BASE_MEM_H
#define DISC_BASE_MEM_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

void *dr_malloc(size_t size);
void *dr_mallocz(size_t size);                  /* zeroed */
void *dr_calloc(size_t n, size_t size);         /* zeroed, n*size checked */
void *dr_realloc(void *p, size_t size);
void *dr_realloc_array(void *p, size_t n, size_t size);
void *dr_malloc_array(size_t n, size_t size);   /* not zeroed, n*size checked */

static inline void dr_free(void *p) { free(p); }

/* Frees *p and sets it NULL (pass the address of the pointer). */
void dr_freep(void *p);

char *dr_strdup(const char *s);                 /* NULL in: NULL out */
char *dr_strndup(const char *s, size_t n);      /* at most n bytes, 0-terminated */
void *dr_memdup(const void *p, size_t size);

/* The classic strlcpy/strlcat: always NUL-terminated when size > 0; the
 * length the full string would have is returned. */
size_t dr_strlcpy(char *dst, const char *src, size_t size);
size_t dr_strlcat(char *dst, const char *src, size_t size);

/* asprintf: *out set (caller frees), the length returned; negative =
 * failure, *out NULL. */
int dr_asprintf(char **out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Append item to the pointer array (pass the address of a T* array): the
 * array grows (geometrically); 0, or DR_ERR_NOMEM with the array as it
 * was. */
int dr_array_add_ptr(void *array, int *nb, void *item);

/* Append one element of elem_size bytes (copied from data) to the value
 * array (pass the address of a T* array): the new element, NULL on
 * failure with the array as it was. */
void *dr_array_add(void *array, int *nb, size_t elem_size, const void *data);

#ifdef __cplusplus
}
#endif

#endif
