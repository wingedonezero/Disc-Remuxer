/*
 * disc-remuxer: the cut editor's pictures. A title's pictures counted in
 * display order (its cut index, cuts.h), each shown by reading the title
 * from the place of the key frame its decoding starts at (the disc opened a
 * second time, for this alone) through the same title timeline, every
 * picture read checked against the index, decoded with FFmpeg
 * (analyze/preview.h); the pictures decoded last are kept up to a memory
 * limit.
 */

#ifndef DISC_REMUXER_CUT_PREVIEW_H
#define DISC_REMUXER_CUT_PREVIEW_H

#include <stdint.h>

#include "cuts.h"

typedef struct CutPreview CutPreview;

/* A picture as shown. */
typedef struct CutFrame {
    int            number;       /* in display order, from 0 */
    int64_t        time;         /* ticks, title timeline */
    int            key, start;   /* a key frame, its enum DrDiscStart */
    int            width, height, stride;
    const uint8_t *bgra;         /* valid until the next call or the close */
    char           md5[33];      /* of the picture as decoded (its planes packed, as FFmpeg's framemd5) */
    int            decoded;      /* pictures decoded for it (0: it was kept) */
    int            from;         /* display number of the key frame the decoding started at (-1: kept) */
} CutFrame;

/* The pictures of title `title` of source, by its index ix (kept by the
 * caller while the preview lives); cache_bytes: what the decoded pictures
 * kept may take. 0, or a negative base error (logged). */
int     cut_preview_open(CutPreview **p, const char *source, int title, const CutIndex *ix, int64_t cache_bytes);
int     cut_preview_count(const CutPreview *p);
/* Picture n's time; the picture shown at time t (the last one starting at
 * or before it, the first one before the first). */
int64_t cut_preview_time(const CutPreview *p, int n);
int     cut_preview_at(const CutPreview *p, int64_t t);
/* Picture n converted to width x height (0 x 0: its own size). 0, or a
 * negative base error (logged: the place does not hold the index's
 * pictures, the picture cannot be decoded). */
int     cut_preview_picture(CutPreview *p, int n, int width, int height, CutFrame *out);
/* Everything it holds freed (the disc closed). */
void    cut_preview_close(CutPreview **p);

#endif /* DISC_REMUXER_CUT_PREVIEW_H */
