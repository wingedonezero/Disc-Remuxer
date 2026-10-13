/*
 * H.264 (ISO/IEC 14496-10): the access-unit cut rule and the picture values.
 *
 * One DrH264 per track holds what carries across access units: the parameter
 * sets, the picture order count state (H.264 8.2.1), the display-order
 * bookkeeping across memory_management_control_operation 5, and the
 * pic_struct history the guess reads. The cut rule (the generic disc engine
 * runs it) cuts the stream into access units; dr_h264_picture() then reads
 * the unit just cut and gives its picture values.
 */

#ifndef DISC_PARSE_H264_H
#define DISC_PARSE_H264_H

#include <stdint.h>

#include "parse/rules.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DrH264 DrH264;

DrH264 *dr_h264_new(void);
void    dr_h264_free(DrH264 **ph);

/* The picture's class, by its first slice's slice_type (values shared with
 * H.265: I, P, B; H.264 alone has SI / SP). The numbering keeps I/P/B at
 * 1/2/3 so (type & 3) folds SI onto I and SP onto P, as the slice rules do. */
enum {
    DR_H26X_PIC_NONE = 0,
    DR_H26X_PIC_I    = 1,
    DR_H26X_PIC_P    = 2,
    DR_H26X_PIC_B    = 3,
    DR_H26X_PIC_SI   = 5,
    DR_H26X_PIC_SP   = 6,
};

/* ---- the cut rule ---- */

typedef struct DrH264CutNal {
    int zr;                 /**< first byte of the zero run in front of its start code */
    int sc;                 /**< its start code 00 00 01 */
    int type;               /**< nal_unit_type */
} DrH264CutNal;

typedef struct DrH264CutSlice {
    int slice_type, pps_id, frame_num, field, bottom, idr, idr_id, poc_lsb;
} DrH264CutSlice;

#define DR_H264_SUFFIX_MAX 100

/** Where the cut of the access unit at the start of the kept bytes stands.
 *  The caller owns one per track: dr_h264_cut_init() to start (and to bind
 *  the track's DrH264), dr_h264_cut_end() when the track closes. */
typedef struct DrH264Cut {
    DrH264 *h;
    DrH264CutNal *nal;      /**< the NAL units found so far */
    int nb_nal, nal_cap;
    int scan;               /**< next offset to look for a start code at */
    int done;               /**< NAL units decided */
    int phase;
    DrH264CutSlice first;   /**< the picture's first slice */
    int suffix[DR_H264_SUFFIX_MAX + 1]; /**< the units the trailing-unit search went over */
    int nb_suffix, suffix_k;
    int no_unit;            /**< the chunk given out is no frame */
    const char *why;        /**< and why (NULL: not said) */
    int look;               /**< the slices after the first one still to be checked for the repair */
    int forced;             /**< the last NAL unit the repair puts into the picture (-1 none) */
} DrH264Cut;

void dr_h264_cut_init(DrH264Cut *st, DrH264 *h);
void dr_h264_cut_end(DrH264Cut *st);

/** The rule; its state is the caller's DrH264Cut (state_size 0). */
extern const DrCutRule dr_h264_cut;

/* ---- the picture values ---- */

typedef struct DrH264Picture {
    int type;               /**< DR_H26X_PIC_* */
    int key;                /**< an IDR picture, or a recovery point SEI in front of it */
    int order;              /**< the display order */
    int fields;             /**< field periods, from pic_struct */
    int rate_num, rate_den; /**< the frame rate; 0 / 0 when the stream has not said */
    const char *note;       /**< a finding worth the chunk's place, or NULL */
} DrH264Picture;

/** The values of the access unit just cut (buf, size bytes): 0 and *pic
 *  filled, or < 0 (base/err.h) when the picture's headers cannot be read. */
int dr_h264_picture(DrH264 *h, void *logctx, const uint8_t *buf, int size, DrH264Picture *pic);

/** The end-of-track counts (guessed pic_struct, D-1 / D.2.3 findings),
 *  logged. */
void dr_h264_close(DrH264 *h, void *logctx);

#ifdef __cplusplus
}
#endif

#endif /* DISC_PARSE_H264_H */
