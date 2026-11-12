/*
 * src/convert: the one conversion the program makes, and the second module
 * that links the processing library (analyze/ only answers questions; this
 * one writes a file). A lossless audio track (Dolby TrueHD, DTS-HD Master
 * Audio) of a title with cuts is decoded from the title's first unit and its
 * samples are written as FLAC, each part cut between two samples: a part of
 * such a track can otherwise start only at some of its units, up to about a
 * tenth of a second after the cut.
 *
 * The decoder runs strictly (integrity failures and frames of a lossless
 * stream that do not come out lossless are errors) and never starts inside
 * the stream: a DTS-HD Master Audio frame builds on what the frames before
 * it left in the decoder, so only a decode from the first frame is exact.
 */

#ifndef DISC_CONVERT_FLAC_OUT_H
#define DISC_CONVERT_FLAC_OUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DrFlacOut DrFlacOut;

/* The samples between two bounds (dr_flac_out_open()). */
typedef struct DrFlacRange {
    int64_t samples;
    char    md5[33];             /* of its samples as FLAC counts them (its STREAMINFO MD5's form) */
} DrFlacRange;

typedef struct DrFlacResult {
    int      rate, channels;
    int      bits;               /* the bits per sample the decoder states */
    int      flac_bits;          /* the bits per sample of the FLAC file (16 or 24) */
    uint64_t layout;             /* channel mask, 0 when the decoder gives none */
    int      native_layout;      /* FLAC has this layout for its channel count (else the file carries the mask as a
                                    tag) */
    int64_t  units;              /* units decoded */
    int64_t  decoded;            /* samples decoded */
    int64_t  before, kept, after;/* samples before the part's range, in it (the file's), after it */
    int64_t  first;              /* the first kept sample's time (ticks, as the units' times), INT64_MIN none */
    int64_t  odd_units;          /* units after the first whose time is not where the samples before it end */
    int      pieces;             /* units given as the start of a new piece of the stream (dr_flac_out_unit()) */
    int      restarts;           /* those the decoder started afresh at */
    int      block;              /* the FLAC block size (samples) */
    /* each range between the bounds, in order (nb_bounds + 1 of them); those
     * up to the part's are complete when the units reached the part's end */
    const DrFlacRange *ranges;
    int      nb_ranges;
    char     md5[33];            /* the kept samples' (the part's range) */
} DrFlacResult;

/* A decoder for codec (the library's codec id: TrueHD, DTS) and, from its
 * first kept sample on, the FLAC file at path. bounds (nb_bounds, rising):
 * the places the samples are cut at, as times on the clock of the units'
 * times (ticks per second: ticks); range k lies between bound k - 1 and
 * bound k (the first from the stream's start, the last to its end). part:
 * the range the file gets. A sample belongs to the range whose bounds its
 * time lies in (from a bound on, up to the next one: the first sample at or
 * after a bound starts its range). 0, or a negative base error. */
int  dr_flac_out_open(DrFlacOut **f, int codec, const char *path, const int64_t *bounds, int nb_bounds, int part,
                      int64_t ticks);
/* The stream's next unit and its time (DR_NOPTS_VALUE: where the samples
 * before it end). piece: the unit is the first of a new piece of the stream
 * (the next of the stream files a title plays back to back). A TrueHD
 * decoder starts afresh there when the unit carries a major sync, as it
 * restarts a stream: its check of the samples since the restart before
 * (one value per restart) would otherwise span two pieces and fail. A
 * piece that goes on without a major sync, and every DTS piece, is decoded
 * on (counted in the result). 0, or a negative base error
 * (dr_flac_out_error() says what). */
int  dr_flac_out_unit(DrFlacOut *f, const uint8_t *data, int size, int64_t time, int piece);
/* The end of the units: the decoder and the encoder are emptied and the
 * file is finished. 0 and *res filled (valid until the close), or a
 * negative base error. No kept sample: no file is left. */
int  dr_flac_out_finish(DrFlacOut *f, DrFlacResult *res);
/* What went wrong, in words ("" nothing). */
const char *dr_flac_out_error(const DrFlacOut *f);
/* errno of a failed write to the file, 0 none. */
int  dr_flac_out_write_error(const DrFlacOut *f);
void dr_flac_out_close(DrFlacOut **f);

/* A FLAC file read back and decoded: its samples (*samples), their MD5 as
 * FLAC counts it (md5, 33 bytes), and what its header states: the samples
 * (*header_samples, 0 not stated) and their MD5 (header_md5, 33 bytes; all
 * zeros not stated). 0, or a negative base error (why, why_size: in
 * words). */
int  dr_flac_read_back(const char *path, int64_t *samples, char *md5, int64_t *header_samples, char *header_md5,
                       char *why, int why_size);

#ifdef __cplusplus
}
#endif

#endif /* DISC_CONVERT_FLAC_OUT_H */
