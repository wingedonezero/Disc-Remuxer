/*
 * Refcounted byte buffers (see buf.h).
 */

#include <stdatomic.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"

struct DrBuf {
    atomic_int refs;
    size_t     size;
    uint8_t   *data;        /* into the same allocation, after the struct */
};

DrBuf *dr_buf_alloc(size_t size)
{
    DrBuf *b;

    if (size > SIZE_MAX - sizeof(*b) - DR_BUF_PADDING)
        return NULL;
    b = dr_malloc(sizeof(*b) + size + DR_BUF_PADDING);
    if (!b)
        return NULL;
    atomic_init(&b->refs, 1);
    b->size = size;
    b->data = (uint8_t *)(b + 1);
    memset(b->data + size, 0, DR_BUF_PADDING);
    return b;
}

DrBuf *dr_buf_from(const void *data, size_t size)
{
    DrBuf *b = dr_buf_alloc(size);

    if (b && size)
        memcpy(b->data, data, size);
    return b;
}

DrBuf *dr_buf_ref(DrBuf *b)
{
    if (b)
        atomic_fetch_add_explicit(&b->refs, 1, memory_order_relaxed);
    return b;
}

void dr_buf_unref(DrBuf **bp)
{
    DrBuf *b = *bp;

    *bp = NULL;
    if (!b)
        return;
    if (atomic_fetch_sub_explicit(&b->refs, 1, memory_order_acq_rel) == 1)
        dr_free(b);
}

uint8_t *dr_buf_data(const DrBuf *b)
{
    return b->data;
}

size_t dr_buf_size(const DrBuf *b)
{
    return b->size;
}

int dr_buf_writable(const DrBuf *b)
{
    return atomic_load_explicit(&b->refs, memory_order_acquire) == 1;
}

void dr_buf_trim(DrBuf *b, size_t size)
{
    if (size < b->size) {
        b->size = size;
        memset(b->data + size, 0, DR_BUF_PADDING);
    }
}
