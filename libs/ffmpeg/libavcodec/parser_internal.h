/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_PARSER_INTERNAL_H
#define AVCODEC_PARSER_INTERNAL_H

#include <stdint.h>

#include "libavutil/macros.h"
#include "avcodec.h"

typedef struct FFCodecParser {
    AVCodecParser p;
    unsigned priv_data_size;
    int (*init)(AVCodecParserContext *s);
    int (*parse)(AVCodecParserContext *s,
                 AVCodecContext *avctx,
                 const uint8_t **poutbuf, int *poutbuf_size,
                 const uint8_t *buf, int buf_size);
    void (*close)(AVCodecParserContext *s);
} FFCodecParser;

static inline const FFCodecParser *ffcodecparser(const AVCodecParser *parser)
{
    return (const FFCodecParser*)parser;
}

#define EIGTH_ARG(a,b,c,d,e,f,g,h,...) h
#define NO_FAIL
// Expands to nothing if <= 7 args; induces compilation failure if not.
#define CHECK_FOR_TOO_MANY_IDS(...) AV_JOIN(EIGTH_ARG(__VA_ARGS__, NO, NO, NO, NO, NO, NO, NO, NO), _FAIL)

// For compatibility with MSVC's old, spec-incompliant preprocessor.
#define FF_MSVC_EXPAND(...) __VA_ARGS__
#define FIRST_SEVEN2(a,b,c,d,e,f,g,...) a,b,c,d,e,f,g
#define FIRST_SEVEN(...) FF_MSVC_EXPAND(FIRST_SEVEN2(__VA_ARGS__))
#define TIMES_SEVEN(a) a,a,a,a,a,a,a

#define PARSER_CODEC_LIST(...) CHECK_FOR_TOO_MANY_IDS(__VA_ARGS__) \
    .p.codec_ids = { FIRST_SEVEN(__VA_ARGS__, TIMES_SEVEN(AV_CODEC_ID_NONE)) }

/**
 * A disc cut rule (PARSER_FLAG_DISC): how a parser cuts a disc's stream.
 */
typedef struct FFDiscCutRule {
    /** Size of the unit at the start of buf (avail bytes; final: no more
     *  bytes will come): > 0 its size, 0 more bytes needed, DISC_CUT_NONE no
     *  unit starts there, another < 0 the stream cannot be cut (logged). */
    int (*unit_size)(void *logctx, const uint8_t *buf, int avail, int final, void *state);
    /** Where the next unit may start in buf (avail bytes), < 0 when not
     *  within them. */
    int (*resync)(const uint8_t *buf, int avail);
    int state_size;                     /**< bytes of the rule's own state */
} FFDiscCutRule;

#define DISC_CUT_NONE (-1)

/** The bytes of a disc cut: kept, not yet given out. */
typedef struct FFDiscCut {
    uint8_t *win;                       /**< bytes taken in and not yet given out */
    int      len, cap;
    uint8_t *out;                       /**< the chunk given out last */
    int      out_cap;
    void    *state;
} FFDiscCut;

/**
 * Cut a disc's stream with rule, keeping to the parser contract: the
 * return value is the number of buf's bytes the chunk given out took (bytes
 * after them come again; < 0 when it ended in bytes kept from earlier
 * calls), so the frame offsets and timestamps of av_parser_parse2() stay
 * right. One chunk per call: a unit (disc_status 0)
 * or bytes that are no unit (disc_status 1); disc_status < 0: the stream
 * cannot be cut. buf_size 0: the end of the stream (the bytes left are cut
 * as the last ones).
 */
int ff_parser_disc_cut(AVCodecParserContext *s, void *logctx, const FFDiscCutRule *rule, FFDiscCut *dc,
                       const uint8_t **poutbuf, int *poutbuf_size, const uint8_t *buf, int buf_size);
void ff_parser_disc_cut_close(FFDiscCut *dc);

/**
 * A disc frame rule (PARSER_FLAG_DISC) in the disc reader's four steps, for
 * codecs whose frames are found by a sync word: sync (0 no frame starts
 * here, 1 one does, 2 too few bytes to tell), size (> 0 the frame's size,
 * <= 0 more bytes needed), check (1 the frame is valid), resync (bytes to
 * skip to the next frame start, -2 more bytes needed, -1 give the kept
 * bytes up), and limit: the bytes kept while looking for a frame (more
 * coming in: the kept bytes and the new ones are given up).
 */
typedef struct FFDiscFrameRule {
    int (*sync)(void *ctx, const uint8_t *buf, int avail);
    int (*size)(void *ctx, const uint8_t *buf, int avail);
    int (*check)(void *ctx, const uint8_t *buf, int size);
    int (*resync)(void *ctx, const uint8_t *buf, int avail);
    int limit;
} FFDiscFrameRule;

/** The state of a disc frame search: the kept bytes and the sync. */
typedef struct FFDiscFrames {
    FFDiscCut cut;
    int       synced;                   /**< a frame starts at the kept bytes' start */
} FFDiscFrames;

/**
 * Find a disc's frames with rule (ctx: the rule's own context), under the
 * same parser contract as ff_parser_disc_cut(): one chunk per call, a frame
 * that is valid (disc_status 0) or bytes that are no frame (1: skipped to
 * the next frame start, given up, or a frame the check refuses).
 */
int ff_parser_disc_frames(AVCodecParserContext *s, void *ctx, const FFDiscFrameRule *rule, FFDiscFrames *df,
                          const uint8_t **poutbuf, int *poutbuf_size, const uint8_t *buf, int buf_size);

#endif /* AVCODEC_PARSER_INTERNAL_H */
