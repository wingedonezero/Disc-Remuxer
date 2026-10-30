/*
 * The cut editor's picture decoder: a title's video track's units (as the
 * title timeline gives them, in decoding order) decoded with FFmpeg and
 * turned into BGRA pictures (Qt's RGB32 on little-endian machines) with
 * swscale, at a size asked for.
 */

#ifndef DISC_ANALYZE_PREVIEW_H
#define DISC_ANALYZE_PREVIEW_H

#include <stdint.h>

struct DrDiscTrack;

typedef struct DrPreview DrPreview;

/* A decoded picture. */
typedef struct DrPreviewImage {
    int      width, height;     /* as converted */
    int      stride;            /* bytes per row of bgra */
    uint8_t *bgra;              /* owned: dr_preview_image_free() */
    int64_t  time;              /* the time its unit was sent with */
    char     md5[33];           /* of the decoded picture as decoded (its planes packed, as FFmpeg's framemd5) */
} DrPreviewImage;

/* A decoder for the track's codec. 0, or a negative error. */
int  dr_preview_open(DrPreview **p, const struct DrDiscTrack *t);
/* A unit, in decoding order, with its time (any clock: given back with the
 * picture). NULL data: the end (the decoder gives out what it holds). 0, or
 * a negative error. */
int  dr_preview_send(DrPreview *p, const uint8_t *data, int size, int64_t time);
/* The next decoded picture, converted to width x height (0 x 0: its own
 * size): 1 and img filled, 0 none yet, a negative error. */
int  dr_preview_receive(DrPreview *p, int width, int height, DrPreviewImage *img);
/* Forgets what was sent (a new reading starts). */
void dr_preview_reset(DrPreview *p);
void dr_preview_close(DrPreview **p);
void dr_preview_image_free(DrPreviewImage *img);

#endif /* DISC_ANALYZE_PREVIEW_H */
