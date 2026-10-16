/*
 * HEVC / H.265 (ISO/IEC 23008-2): the access-unit cut rule and the picture
 * values.
 *
 * One DrHevc per track holds the parameter sets and the picture order count
 * state (H.265 8.3.1), the pic_struct history the guess reads, and which
 * one-time notes were already said. The cut rule (the generic disc engine
 * runs it) cuts the stream into access units; dr_hevc_picture() then reads
 * the unit just cut and gives its picture values. Picture classes are the
 * DR_H26X_PIC_* of parse/h264.h.
 */

#ifndef DISC_PARSE_HEVC_H
#define DISC_PARSE_HEVC_H

#include <stdint.h>

#include "parse/h264.h"
#include "parse/rules.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DrHevc DrHevc;

DrHevc *dr_hevc_new(void);
void    dr_hevc_free(DrHevc **ph);

/* ---- the cut rule ---- */

typedef struct DrHevcCutNal {
    int zr;                 /**< first byte of the zero run in front of its start code */
    int sc;                 /**< its start code 00 00 01 */
    int type;               /**< nal_unit_type */
    int layer;              /**< nuh_layer_id */
} DrHevcCutNal;

typedef struct DrHevcCutSlice {
    int first, pps_id;
} DrHevcCutSlice;

#define DR_HEVC_SUFFIX_MAX 100

/** Where the cut of the access unit at the start of the kept bytes stands.
 *  The caller owns one per track: dr_hevc_cut_init() to start, then
 *  dr_hevc_cut_end() when the track closes. */
typedef struct DrHevcCut {
    DrHevc *h;
    DrHevcCutNal *nal;      /**< the NAL units found so far */
    int nb_nal, nal_cap;
    int scan;               /**< next offset to look for a start code at */
    int done;               /**< NAL units decided */
    int phase;
    DrHevcCutSlice first;   /**< the picture's first slice segment */
    int suffix[DR_HEVC_SUFFIX_MAX + 1]; /**< the units the trailing-unit search went over */
    int nb_suffix, suffix_k;
    int no_unit;            /**< the chunk given out is no frame */
    int64_t pictures;       /**< pictures cut so far (where a warning points) */
} DrHevcCut;

void dr_hevc_cut_init(DrHevcCut *st, DrHevc *h);
void dr_hevc_cut_end(DrHevcCut *st);

/** The rule; its state is the caller's DrHevcCut (state_size 0). */
extern const DrCutRule dr_hevc_cut;

/* ---- the picture values ---- */

typedef struct DrHevcPicture {
    int type;               /**< DR_H26X_PIC_* */
    int key;                /**< an IRAP picture, or a recovery point SEI in front of it */
    int order;              /**< the display order */
    int fields;             /**< field periods, from pic_struct */
    int rate_num, rate_den; /**< the frame rate; 0 / 1 when the stream has not said */
} DrHevcPicture;

/** The values of the access unit just cut (buf, size bytes): 0 and *pic
 *  filled, or < 0 (base/err.h) when the picture's headers cannot be read or
 *  its pic_struct cannot be handled. */
int dr_hevc_picture(DrHevc *h, void *logctx, const uint8_t *buf, int size, DrHevcPicture *pic);

#ifdef __cplusplus
}
#endif

#endif /* DISC_PARSE_HEVC_H */
