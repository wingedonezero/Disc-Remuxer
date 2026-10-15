/*
 * HDMV text subtitle (Blu-ray TextST) segments
 */

/**
 * @file
 * A text subtitle stream is a run of segments: a type byte, a 16-bit
 * length and the body. The dialog style segment (0x81) comes first; on the
 * disc 2 more bytes follow it (the number of dialog presentation segments),
 * not counted in its length. Each dialog presentation segment (0x82) is one
 * subtitle: after the segment header its start and end PTS (5 bytes each:
 * 7 reserved bits and the 33-bit time), a byte with palette_update_flag
 * (bit 7), when set a palette update (16-bit length and that many bytes),
 * then number_of_regions (1 or 2) and each region: a flags byte
 * (continuous_present_flag 0x80, forced_on_flag 0x40), region_style_id_ref,
 * a 16-bit length and the region's text data.
 */

#ifndef DISC_PARSE_HDMV_TEXTST_H
#define DISC_PARSE_HDMV_TEXTST_H

#include <stdint.h>

#include "base/bytes.h"

#define HDMV_TEXTST_DSS 0x81            /**< dialog style segment */
#define HDMV_TEXTST_DPS 0x82            /**< dialog presentation segment */

#define HDMV_TEXTST_FORCED_ON 0x40      /**< a region's forced_on_flag */

typedef struct HDMVTextstDialog {
    int      size;                      /**< the segment's bytes (3 + its length) */
    int64_t  start, end;                /**< its start and end PTS (33 bits, 90 kHz) */
    int      regions_at;                /**< the offset of number_of_regions */
    int      nb_regions;
    int      region_at[2];              /**< each region's offset (its flags byte) */
    int      region_size[2];            /**< and its bytes (4 + its length) */
    int      forced;                    /**< a region has forced_on_flag */
    const char *error;                  /**< why it does not parse */
} HDMVTextstDialog;

/** The size of the segment at p (avail bytes): 3 + its length (+ 2 for the
 *  dialog style segment), -1 when its header is not all there. */
static inline int ff_hdmv_textst_segment_size(const uint8_t *p, int avail)
{
    if (avail < 3)
        return -1;
    return dr_rb16(p + 1) + 3 + (p[0] == HDMV_TEXTST_DSS ? 2 : 0);
}

/** The dialog presentation segment at p (avail bytes): 1 it parses (*d
 *  filled), 0 it does not (d->error says why). */
static inline int ff_hdmv_textst_dialog(const uint8_t *p, int avail, HDMVTextstDialog *d)
{
    int size, off, i;

    d->error = NULL;
    d->forced = 0;
    if (avail < 3 || p[0] != HDMV_TEXTST_DPS) {
        d->error = "no dialog presentation segment";
        return 0;
    }
    size = dr_rb16(p + 1) + 3;
    if (size < 15 || size > avail) {
        d->error = size < 15 ? "shorter than its fixed fields" : "longer than the bytes it comes in";
        return 0;
    }
    d->size  = size;
    d->start = (int64_t)(p[3] & 1) << 32 | dr_rb32(p + 4);
    d->end   = (int64_t)(p[8] & 1) << 32 | dr_rb32(p + 9);
    if (p[13] & 0x80) {
        if (size < 16) {
            d->error = "its palette update has no length";
            return 0;
        }
        off = dr_rb16(p + 14) + 16;
        if (off >= size) {
            d->error = "its palette update runs to its end";
            return 0;
        }
    } else
        off = 14;
    d->regions_at = off;
    d->nb_regions = p[off];
    if (d->nb_regions < 1 || d->nb_regions > 2) {
        d->error = "its number of regions is not 1 or 2";
        return 0;
    }
    i = off + 1;
    for (int r = 0; r < d->nb_regions; r++) {
        if (i + 4 > size) {
            d->error = "a region runs past its end";
            return 0;
        }
        d->region_at[r]   = i;
        d->region_size[r] = 4 + dr_rb16(p + i + 2);
        d->forced        |= !!(p[i] & HDMV_TEXTST_FORCED_ON);
        i += d->region_size[r];
    }
    if (i > size) {
        d->error = "a region runs past its end";
        return 0;
    }
    return 1;
}

#endif /* DISC_PARSE_HDMV_TEXTST_H */
