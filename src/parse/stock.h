/*
 * The stock frame cutters: MPEG-1/2 video and AC-3 / E-AC-3 cut exactly as
 * FFmpeg's own parsers cut them (no disc rule of ours ever applied to these
 * two), ported so that no stream byte passes through the processing
 * library. Bytes go in as they come; whole units come out with the stream
 * offset of their first byte. The cut is the library's to the byte, damaged
 * stretches included (tests/unit/test_stock_parse.py runs both side by
 * side).
 */

#ifndef DISC_PARSE_STOCK_H
#define DISC_PARSE_STOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum DrStockCodec {
    DR_STOCK_MPEGVIDEO,     /**< MPEG-1 / MPEG-2 video: one picture (with the headers before it) a unit */
    DR_STOCK_AC3,           /**< AC-3 / E-AC-3: a frame with the dependent frames after it a unit */
};

typedef struct DrStock DrStock;

/** A cutter for the codec, or NULL (no memory). */
DrStock *dr_stock_new(enum DrStockCodec codec);
void     dr_stock_free(DrStock **ps);

/** Bytes in (data with DR_BUF_PADDING bytes readable after size; size 0:
 *  the stream ends), at most one unit out: *out / *out_size (0 none),
 *  *offset its first byte's stream offset; AC-3: *samples / *rate of the
 *  unit's last frame when its header and CRC are good (else as before;
 *  video: 0). pos: the stream offset of data. Returns the bytes taken, or a
 *  base/err.h value (no memory). Call again with the rest. */
int dr_stock_parse(DrStock *s, const uint8_t *data, int size, int64_t pos,
                   const uint8_t **out, int *out_size, int64_t *offset, int *samples, int *rate);

#ifdef __cplusplus
}
#endif

#endif
