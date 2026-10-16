/*
 * The splitter: one engine that runs a codec's rule (parse/rules.h) over a
 * stream of bytes and gives out chunks — units, and the runs of bytes that
 * are no unit, each with why. It replaces the two engines that lived in the
 * processing library's parser host: feed bytes in with dr_split_write(),
 * take chunks out with dr_split_next(), say the stream ended with
 * dr_split_end() and drain. For H.264 and HEVC a unit chunk also carries
 * the picture's values (parse/h264.h, parse/hevc.h).
 */

#ifndef DISC_PARSE_SPLIT_H
#define DISC_PARSE_SPLIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The codecs the splitter cuts. */
enum DrSplitCodec {
    DR_SPLIT_NONE = 0,
    DR_SPLIT_VC1,
    DR_SPLIT_H264,
    DR_SPLIT_HEVC,
    DR_SPLIT_MLP,               /* TrueHD / MLP */
    DR_SPLIT_DVDSUB,            /* DVD / HD DVD sub-pictures */
    DR_SPLIT_PGS,               /* PGS display sets */
    DR_SPLIT_TEXTST,            /* Blu-ray text subtitle segments */
    DR_SPLIT_PCM_BLURAY,
    DR_SPLIT_PCM_DVD,           /* DVD / HD DVD LPCM (dr_split_pcm_format) */
    DR_SPLIT_AC3,               /* AC-3 (dr_split_take_any: E-AC-3 frames too) */
    DR_SPLIT_DTS,               /* DTS core (dr_split_take_any: with the extension substream) */
    DR_SPLIT_MPA,               /* MPEG audio (dr_split_dvd: the DVD-Video rule) */
    DR_SPLIT_ADTS,              /* AAC ADTS */
    DR_SPLIT_LATM,              /* AAC LATM / LOAS */
};

/* What a chunk is. */
enum {
    DR_SPLIT_UNIT     = 0,
    DR_SPLIT_SKIP     = 1,      /* bytes that do not start a frame (skipped to the next start) */
    DR_SPLIT_REFUSED  = 2,      /* a frame that fails its check */
    DR_SPLIT_NO_START = 3,      /* bytes in which no frame starts (given up) */
    DR_SPLIT_END      = 4,      /* at the end, the start of a frame the stream does not finish */
};

typedef struct DrSplitPicture {
    int type;                   /* DR_H26X_PIC_* (parse/h264.h) */
    int key, order, fields;
    int rate_num, rate_den;
} DrSplitPicture;

typedef struct DrSplitChunk {
    const uint8_t *data;        /* the splitter's; valid until the next call */
    int size;
    int64_t offset;             /* its first byte's offset in the stream */
    int status;                 /* DR_SPLIT_UNIT, or why the bytes are no unit */
    const char *note;           /* a finding worth the chunk's place, or NULL */
    const DrSplitPicture *pic;  /* a video unit's picture values, or NULL */
} DrSplitChunk;

typedef struct DrSplit DrSplit;

/** A splitter for the codec, or NULL (no memory). logctx goes to the rules'
 *  log lines (base/vlog.h). */
DrSplit *dr_split_new(enum DrSplitCodec codec, void *logctx);
void     dr_split_free(DrSplit **ps);

/* Options, before the first write: */
void dr_split_take_any(DrSplit *s);     /**< AC-3: both kinds; DTS: with the extension substream */
void dr_split_ext_only(DrSplit *s);     /**< DTS: a stream without a core: extension substreams only */
void dr_split_dvd(DrSplit *s);          /**< MPEG audio: a DVD-Video disc's stream */
/** DVD / HD DVD LPCM: the frame format (any time; until set the bytes are kept). */
void dr_split_pcm_format(DrSplit *s, int channels, int bits, int samples_per_frame);

/** More bytes of the stream: 0, or < 0 (base/err.h). */
int  dr_split_write(DrSplit *s, const uint8_t *data, int size);
/** The stream ends here (then drain with dr_split_next()). */
void dr_split_end(DrSplit *s);
/** The next chunk: 1 and *c filled, 0 none (yet), < 0 the stream cannot be
 *  cut (base/err.h; the track is over). */
int  dr_split_next(DrSplit *s, DrSplitChunk *c);
/** The stream offset the splitter stands at (its first undecided byte). */
int64_t dr_split_pos(const DrSplit *s);

#ifdef __cplusplus
}
#endif

#endif /* DISC_PARSE_SPLIT_H */
