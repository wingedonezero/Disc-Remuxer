/*
 * Refcounted byte buffers: one allocation shared by readers (frames on the
 * title timeline reference slices of what was read). The data of every
 * buffer ends with DR_BUF_PADDING zeroed bytes beyond its size, so parsers
 * that read a fixed word past the end stay inside the allocation.
 *
 * Thread rule: dr_buf_ref()/dr_buf_unref() are safe from any thread (the
 * count is atomic); the DATA is the caller's to guard — write only while
 * dr_buf_writable().
 */

#ifndef DISC_BASE_BUF_H
#define DISC_BASE_BUF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DR_BUF_PADDING 64

typedef struct DrBuf DrBuf;

/* A new buffer of size bytes (uninitialised, padding zeroed), count 1;
 * NULL on failure. */
DrBuf *dr_buf_alloc(size_t size);

/* A new buffer holding a copy of data. */
DrBuf *dr_buf_from(const void *data, size_t size);

/* One more owner; returns b. */
DrBuf *dr_buf_ref(DrBuf *b);

/* One owner less; the last unref frees. *bp (may be NULL) is set NULL. */
void dr_buf_unref(DrBuf **bp);

uint8_t *dr_buf_data(const DrBuf *b);
size_t   dr_buf_size(const DrBuf *b);

/* 1 while this is the only owner (the data may be written). */
int dr_buf_writable(const DrBuf *b);

/* Shrinks what the buffer counts as its size (never grows; the padding
 * rule holds at the new size). */
void dr_buf_trim(DrBuf *b, size_t size);

#ifdef __cplusplus
}
#endif

#endif
