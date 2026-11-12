/*
 * A track's content as one checksum, in a form that does not depend on the
 * file it sits in: taken once from the track's bytes as they are written
 * (in memory) and once from the finished Matroska file read back, the two
 * must be equal. The forms are those a Matroska muxer was measured to keep
 * byte for byte:
 * - bytes: the stream's bytes one after another (TrueHD, AC-3, PCM samples,
 *   FLAC frames, PGS segments);
 * - NAL units: H.264, each unit's bytes and its length, without what
 *   separates the units (start codes in a stream file, length fields in
 *   Matroska). Its parameter sets count once each, wherever and however
 *   often they stand: a muxer repeats them in front of a key picture that
 *   comes without them;
 * - AC-3 frames whose dialogue normalization the muxer was told to remove:
 *   each frame without the fields that change with it (ETSI TS 102 366: the
 *   dialnorm values, crc1, crc2 and the bit reserved next to crc2).
 * A codec without a measured form has none (DR_CONTENT_NONE): its track is
 * not compared, and that is said.
 */

#ifndef DISC_ANALYZE_CONTENT_SUM_H
#define DISC_ANALYZE_CONTENT_SUM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum DrContentKind {
    DR_CONTENT_NONE = 0,     /* no form for the codec yet */
    DR_CONTENT_BYTES,
    DR_CONTENT_NAL,
    DR_CONTENT_AC3_NO_DIALNORM,
};

typedef struct DrContentSum DrContentSum;

typedef struct DrContentResult {
    char    digest[33];      /* 128 bits, hex */
    int64_t bytes;           /* content bytes summed (NAL units: a parameter set's once) */
    int64_t units;           /* NAL units other than parameter sets / AC-3 frames; bytes: the calls */
    int     odd;             /* the data did not have the form (bytes before a first start code, an AC-3 frame
                                that cannot be read): the sum cannot be compared */
} DrContentResult;

/* The form for a codec (the library's codec id); dialnorm_removed: the
 * muxer is told to remove its dialogue normalization. */
int  dr_content_kind(int codec, int dialnorm_removed);
/* The form's name in a log line. */
const char *dr_content_kind_name(int kind);

/* 0, or a negative base error. */
int  dr_content_sum_open(DrContentSum **s, int kind);
/* The stream's next bytes as a stream file holds them (NAL units: with
 * their start codes; AC-3 frames: whole frames, any number per call or
 * across calls). */
void dr_content_sum_data(DrContentSum *s, const uint8_t *data, size_t size);
/* One NAL unit as a Matroska block holds it (without its length field). */
void dr_content_sum_nal(DrContentSum *s, const uint8_t *nal, size_t size);
/* The end of the stream: the result (the sum takes no more data). */
void dr_content_sum_finish(DrContentSum *s, DrContentResult *r);
void dr_content_sum_close(DrContentSum **s);

#ifdef __cplusplus
}
#endif

#endif /* DISC_ANALYZE_CONTENT_SUM_H */
