/*
 * A finished Matroska file read back: each track's content as one checksum
 * (content_sum.h), to compare with the checksum taken when the track's
 * stream was written. The file is read with the processing library's
 * Matroska reader; nothing of it is changed.
 */

#ifndef DISC_ANALYZE_MKV_READBACK_H
#define DISC_ANALYZE_MKV_READBACK_H

#include "analyze/content_sum.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DrMkvTrack {
    int             kind;    /* in: DrContentKind of the file's track (in the file's track order);
                                DR_CONTENT_NONE: not read */
    DrContentResult got;     /* out */
    int             codec;   /* out: the library's codec id of the track */
} DrMkvTrack;

/* Reads the file at path to its end and fills each track's sum. The file
 * must hold exactly nb tracks. 0, or a negative base error (why, why_size:
 * in words). */
int dr_mkv_read_back(const char *path, DrMkvTrack *tracks, int nb, char *why, int why_size);

#ifdef __cplusplus
}
#endif

#endif /* DISC_ANALYZE_MKV_READBACK_H */
