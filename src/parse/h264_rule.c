/*
 * H.264 (ISO/IEC 14496-10): the access-unit cut rule and the picture values.
 *
 * Moved out of our FFmpeg's h264_parser.c disc mode. The cut rule is the
 * one the generic disc engine runs; dr_h264_picture() reads the unit just
 * cut. The header walks (SPS, PPS, SEI, the slice header through the
 * decoded reference picture marking) are our own, written from the spec to
 * read bit for bit what the processing library's parser read, so every
 * value and every finding stays identical across the move.
 *
 * The picture order count (init_poc() below) follows FFmpeg's
 * libavcodec/h264_parse.c (ff_h264_init_poc), part of FFmpeg,
 * LGPL-2.1-or-later.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "base/err.h"
#include "base/mem.h"
#include "base/vlog.h"

#include "parse/bits.h"
#include "parse/h264.h"
#include "parse/rational.h"
#include "parse/rules.h"

/* nal_unit_type (7.4.1) */
enum {
    NAL_SLICE     = 1,
    NAL_DPA       = 2,
    NAL_DPB       = 3,
    NAL_DPC       = 4,
    NAL_IDR_SLICE = 5,
    NAL_SEI       = 6,
    NAL_SPS       = 7,
    NAL_PPS       = 8,
    NAL_AUD       = 9,
};

/* SEI payload types (D.1) */
enum {
    SEI_PIC_TIMING     = 1,
    SEI_RECOVERY_POINT = 6,
};

/* the slice's picture (7.4.3) */
enum { PICT_TOP = 1, PICT_BOTTOM = 2, PICT_FRAME = 3 };

#define MAX_SPS_COUNT  32
#define MAX_PPS_COUNT  256
#define MAX_MMCO_COUNT 66

/* The sequence parameter set, the fields the rules read (7.3.2.1.1). */
typedef struct DrH264Sps {
    int present;
    int chroma_format_idc;
    int separate_colour_plane;
    int log2_max_frame_num;
    int poc_type;
    int log2_max_poc_lsb;
    int delta_pic_order_always_zero_flag;
    int offset_for_non_ref_pic;
    int offset_for_top_to_bottom_field;
    int poc_cycle_length;
    int offset_for_ref_frame[256];
    int frame_mbs_only_flag;
    /* VUI (E.1) */
    int timing_info_present_flag;
    uint32_t num_units_in_tick, time_scale;
    int fixed_frame_rate_flag;
    int nal_hrd_parameters_present_flag, vcl_hrd_parameters_present_flag;
    int cpb_removal_delay_length, dpb_output_delay_length;
    int pic_struct_present_flag;
} DrH264Sps;

/* The picture parameter set (7.3.2.2), with the SPS as it was when the set
 * was read (a later SPS of the same id does not change it, as the stored
 * sets the processing library kept did not). */
typedef struct DrH264Pps {
    int present;
    int pic_order_present;          /* bottom_field_pic_order_in_frame_present_flag */
    int redundant_pic_cnt_present;
    int weighted_pred;
    int weighted_bipred_idc;
    int ref_count[2];               /* num_ref_idx_l0/l1_default_active */
    DrH264Sps sps;
} DrH264Pps;

struct DrH264 {
    DrH264Sps sps_list[MAX_SPS_COUNT];
    DrH264Pps *pps_list[MAX_PPS_COUNT];
    const DrH264Pps *active;        /**< the last picture's PPS */
    /* the picture order count state (8.2.1) */
    int poc_lsb, poc_msb, delta_poc_bottom, delta_poc[2];
    int frame_num, frame_num_offset;
    int prev_poc_lsb, prev_poc_msb, prev_frame_num, prev_frame_num_offset;
    int picture_structure;          /**< of the last slice parsed (PICT_*) */
    /* display order across memory_management_control_operation 5 */
    int reset;                      /**< the picture just parsed has one */
    int order_base, order_max;
    int64_t resets;
    /* pic_struct */
    uint8_t ps_hist[128];           /**< the latest 128 pictures */
    int ps_n, ps_start;
    int64_t guessed;                /**< frames whose pic_struct was guessed */
    int64_t ps_field;               /**< pic_struct that does not fit field_pic_flag (Table D-1) */
    int64_t ps_fixed;               /**< pic_struct 7 / 8 without fixed_frame_rate_flag (D.2.3) */
    char note[256];                 /**< the note of the chunk given out */
    /* the picture pass's scratch */
    uint8_t *rbsp;
    int rbsp_cap;
};

DrH264 *dr_h264_new(void)
{
    return dr_mallocz(sizeof(DrH264));
}

void dr_h264_free(DrH264 **ph)
{
    DrH264 *h = *ph;

    if (!h)
        return;
    for (int i = 0; i < MAX_PPS_COUNT; i++)
        dr_free(h->pps_list[i]);
    dr_free(h->rbsp);
    dr_freep(ph);
}

/* The NAL unit's bytes after its header (nal, size bytes from the header)
 * without emulation prevention, into h->rbsp, read with b. In a legal
 * stream 00 00 03 appears only as emulation (7.4.1.1), so every 3 after two
 * zeros is dropped. */
static int rbsp_get(DrH264 *h, const uint8_t *nal, int size, DrBits *b)
{
    int n = 0, zeros = 0;

    if (size > h->rbsp_cap) {
        uint8_t *r = dr_realloc(h->rbsp, size);
        if (!r)
            return DR_ERR(ENOMEM);
        h->rbsp     = r;
        h->rbsp_cap = size;
    }
    for (int i = 1; i < size; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        h->rbsp[n++] = nal[i];
    }
    return dr_bits_init8(b, h->rbsp, n) < 0 ? DR_ERR_INVALID : 0;
}

/* ---- the parameter sets ---- */

/* hrd_parameters (E.1.2): walked; the delay lengths kept. */
static int parse_hrd(DrBits *b, DrH264Sps *sps)
{
    unsigned cpb_count = dr_bits_ue(b) + 1;

    if (cpb_count > 32)
        return DR_ERR_INVALID;
    dr_bits_skip(b, 4 + 4);                 /* bit_rate_scale, cpb_size_scale */
    for (unsigned i = 0; i < cpb_count; i++) {
        dr_bits_ue(b);                      /* bit_rate_value_minus1 */
        dr_bits_ue(b);                      /* cpb_size_value_minus1 */
        dr_bits_get1(b);                    /* cbr_flag */
    }
    dr_bits_skip(b, 5);                     /* initial_cpb_removal_delay_length_minus1 */
    sps->cpb_removal_delay_length = dr_bits_get(b, 5) + 1;
    sps->dpb_output_delay_length  = dr_bits_get(b, 5) + 1;
    dr_bits_skip(b, 5);                     /* time_offset_length */
    return 0;
}

/* scaling_list (7.3.2.1.1.1): walked. */
static void skip_scaling_lists(DrBits *b, int chroma_format_idc)
{
    int lists = chroma_format_idc == 3 ? 12 : 8;

    for (int i = 0; i < lists; i++) {
        if (!dr_bits_get1(b))               /* seq_scaling_list_present_flag */
            continue;
        int size = i < 6 ? 16 : 64, last = 8, next = 8;
        for (int j = 0; j < size; j++) {
            if (next)
                next = (last + dr_bits_se(b) + 256) & 255;
            last = next ? next : last;
        }
    }
}

/* A seq parameter set of the access unit into the sets (7.3.2.1.1): the
 * fields the rules read, with the checks whose failure keeps a set out. */
static int parse_sps(void *logctx, DrH264 *h, DrBits *b)
{
    DrH264Sps sps = { 0 };
    int profile_idc = dr_bits_get(b, 8);
    unsigned sps_id, v;

    dr_bits_skip(b, 8 + 8);                 /* constraint flags + level_idc */
    sps_id = dr_bits_ue31(b);
    if (sps_id >= MAX_SPS_COUNT) {
        dr_vlog(logctx, DR_VLOG_ERROR, "sps_id %u out of range\n", sps_id);
        return DR_ERR_INVALID;
    }
    sps.chroma_format_idc = 1;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc ==  44 || profile_idc ==  83 ||
        profile_idc ==  86 || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135) {
        sps.chroma_format_idc = dr_bits_ue31(b);
        if (sps.chroma_format_idc > 3) {
            dr_vlog(logctx, DR_VLOG_ERROR, "chroma_format_idc %d is illegal\n", sps.chroma_format_idc);
            return DR_ERR_INVALID;
        }
        if (sps.chroma_format_idc == 3)
            sps.separate_colour_plane = dr_bits_get1(b);
        v = dr_bits_ue(b);                  /* bit_depth_luma_minus8 */
        if (v > 6)
            return DR_ERR_INVALID;
        v = dr_bits_ue(b);                  /* bit_depth_chroma_minus8 */
        if (v > 6)
            return DR_ERR_INVALID;
        dr_bits_get1(b);                    /* qpprime_y_zero_transform_bypass_flag */
        if (dr_bits_get1(b))                /* seq_scaling_matrix_present_flag */
            skip_scaling_lists(b, sps.chroma_format_idc);
    }
    v = dr_bits_ue(b);                      /* log2_max_frame_num_minus4 */
    if (v > 12) {
        dr_vlog(logctx, DR_VLOG_ERROR, "log2_max_frame_num_minus4 out of range (0-12): %u\n", v);
        return DR_ERR_INVALID;
    }
    sps.log2_max_frame_num = v + 4;
    sps.poc_type = dr_bits_ue31(b);
    if (sps.poc_type == 0) {
        v = dr_bits_ue(b);                  /* log2_max_pic_order_cnt_lsb_minus4 */
        if (v > 12) {
            dr_vlog(logctx, DR_VLOG_ERROR, "log2_max_poc_lsb (%u) is out of range\n", v + 4);
            return DR_ERR_INVALID;
        }
        sps.log2_max_poc_lsb = v + 4;
    } else if (sps.poc_type == 1) {
        sps.delta_pic_order_always_zero_flag = dr_bits_get1(b);
        sps.offset_for_non_ref_pic           = dr_bits_se(b);
        sps.offset_for_top_to_bottom_field   = dr_bits_se(b);
        v = dr_bits_ue(b);
        if (v >= 256) {
            dr_vlog(logctx, DR_VLOG_ERROR, "poc_cycle_length overflow %u\n", v);
            return DR_ERR_INVALID;
        }
        sps.poc_cycle_length = v;
        for (int i = 0; i < sps.poc_cycle_length; i++)
            sps.offset_for_ref_frame[i] = dr_bits_se(b);
    } else if (sps.poc_type != 2) {
        dr_vlog(logctx, DR_VLOG_ERROR, "illegal POC type %d\n", sps.poc_type);
        return DR_ERR_INVALID;
    }
    v = dr_bits_ue31(b);                    /* max_num_ref_frames */
    if (v > 16)
        return DR_ERR_INVALID;
    dr_bits_get1(b);                        /* gaps_in_frame_num_value_allowed_flag */
    dr_bits_ue(b);                          /* pic_width_in_mbs_minus1 */
    dr_bits_ue(b);                          /* pic_height_in_map_units_minus1 */
    sps.frame_mbs_only_flag = dr_bits_get1(b);
    if (!sps.frame_mbs_only_flag)
        dr_bits_get1(b);                    /* mb_adaptive_frame_field_flag */
    dr_bits_get1(b);                        /* direct_8x8_inference_flag */
    if (dr_bits_get1(b)) {                  /* frame_cropping_flag */
        dr_bits_ue(b);
        dr_bits_ue(b);
        dr_bits_ue(b);
        dr_bits_ue(b);
    }
    if (dr_bits_get1(b)) {                  /* vui_parameters_present_flag (E.1.1) */
        if (dr_bits_get1(b)) {              /* aspect_ratio_info_present_flag */
            if (dr_bits_get(b, 8) == 255)   /* aspect_ratio_idc: Extended_SAR */
                dr_bits_skip(b, 16 + 16);
        }
        if (dr_bits_get1(b))                /* overscan_info_present_flag */
            dr_bits_get1(b);
        if (dr_bits_get1(b)) {              /* video_signal_type_present_flag */
            dr_bits_skip(b, 3 + 1);
            if (dr_bits_get1(b))            /* colour_description_present_flag */
                dr_bits_skip(b, 24);
        }
        if (dr_bits_get1(b)) {              /* chroma_loc_info_present_flag */
            dr_bits_ue(b);
            dr_bits_ue(b);
        }
        sps.timing_info_present_flag = dr_bits_get1(b);
        if (sps.timing_info_present_flag) {
            sps.num_units_in_tick      = dr_bits_get(b, 32);
            sps.time_scale             = dr_bits_get(b, 32);
            sps.fixed_frame_rate_flag  = dr_bits_get1(b);
            if (!sps.num_units_in_tick || !sps.time_scale) {
                dr_vlog(logctx, DR_VLOG_DEBUG, "time_scale/num_units_in_tick invalid or unsupported (%u/%u)\n",
                        sps.time_scale, sps.num_units_in_tick);
                sps.timing_info_present_flag = 0;
            }
        }
        sps.nal_hrd_parameters_present_flag = dr_bits_get1(b);
        if (sps.nal_hrd_parameters_present_flag && parse_hrd(b, &sps) < 0)
            return DR_ERR_INVALID;
        sps.vcl_hrd_parameters_present_flag = dr_bits_get1(b);
        if (sps.vcl_hrd_parameters_present_flag && parse_hrd(b, &sps) < 0)
            return DR_ERR_INVALID;
        if (sps.nal_hrd_parameters_present_flag || sps.vcl_hrd_parameters_present_flag)
            dr_bits_get1(b);                /* low_delay_hrd_flag */
        sps.pic_struct_present_flag = dr_bits_get1(b);
        /* the rest of the VUI is not read */
    }
    sps.present = 1;
    h->sps_list[sps_id] = sps;
    return 0;
}

/* A picture parameter set of the access unit into the sets (7.3.2.2). */
static int parse_pps(void *logctx, DrH264 *h, DrBits *b)
{
    DrH264Pps pps = { 0 };
    unsigned pps_id = dr_bits_ue(b), sps_id;

    if (pps_id >= MAX_PPS_COUNT) {
        dr_vlog(logctx, DR_VLOG_ERROR, "pps_id %u out of range\n", pps_id);
        return DR_ERR_INVALID;
    }
    sps_id = dr_bits_ue31(b);
    if (sps_id >= MAX_SPS_COUNT || !h->sps_list[sps_id].present) {
        dr_vlog(logctx, DR_VLOG_ERROR, "sps_id %u out of range\n", sps_id);
        return DR_ERR_INVALID;
    }
    pps.sps = h->sps_list[sps_id];
    dr_bits_get1(b);                        /* entropy_coding_mode_flag */
    pps.pic_order_present = dr_bits_get1(b);
    if (dr_bits_ue(b) + 1 > 1) {            /* num_slice_groups_minus1 */
        dr_vlog(logctx, DR_VLOG_ERROR, "FMO not supported\n");
        return DR_ERR_INVALID;
    }
    pps.ref_count[0] = dr_bits_ue(b) + 1;
    pps.ref_count[1] = dr_bits_ue(b) + 1;
    if (pps.ref_count[0] - 1 > 31 || pps.ref_count[1] - 1 > 31) {
        dr_vlog(logctx, DR_VLOG_ERROR, "reference overflow (pps)\n");
        return DR_ERR_INVALID;
    }
    pps.weighted_pred       = dr_bits_get1(b);
    pps.weighted_bipred_idc = dr_bits_get(b, 2);
    dr_bits_se(b);                          /* pic_init_qp_minus26 */
    dr_bits_se(b);                          /* pic_init_qs_minus26 */
    dr_bits_se(b);                          /* chroma_qp_index_offset */
    dr_bits_get1(b);                        /* deblocking_filter_control_present_flag */
    dr_bits_get1(b);                        /* constrained_intra_pred_flag */
    pps.redundant_pic_cnt_present = dr_bits_get1(b);
    pps.present = 1;
    if (!h->pps_list[pps_id] && !(h->pps_list[pps_id] = dr_malloc(sizeof(DrH264Pps))))
        return DR_ERR(ENOMEM);
    *h->pps_list[pps_id] = pps;
    return 0;
}

/* ---- the SEI messages in front of the picture ---- */

typedef struct DrH264Sei {
    int recovery;                           /**< a recovery point is there */
    int pt_present;                         /**< a picture timing message is there */
    uint8_t pt[256];                        /**< its payload */
    int pt_size;
} DrH264Sei;

/* The messages of one SEI NAL unit (D.1), from its RBSP. */
static void parse_sei(void *logctx, DrH264 *h, DrH264Sei *sei, int rbsp_size)
{
    const uint8_t *p = h->rbsp;
    int n = rbsp_size, i = 0;

    while (n > 0 && !p[n - 1])              /* the zeros after the stop bit */
        n--;
    if (n < 1 || p[n - 1] != 0x80)          /* rbsp_trailing_bits */
        return;
    n--;
    while (i < n) {
        int type = 0, len = 0;
        while (i < n && p[i] == 0xff)
            type += p[i++];
        if (i >= n)
            break;
        type += p[i++];
        while (i < n && p[i] == 0xff)
            len += p[i++];
        if (i >= n)
            break;
        len += p[i++];
        if (len > n - i)
            break;
        if (type == SEI_PIC_TIMING) {
            if (len > (int)sizeof(sei->pt)) {
                dr_vlog(logctx, DR_VLOG_ERROR, "Picture timing SEI payload too large\n");
                return;
            }
            memcpy(sei->pt, p + i, len);
            sei->pt_size    = len;
            sei->pt_present = 1;
        } else if (type == SEI_RECOVERY_POINT && len) {
            DrBits b;
            if (dr_bits_init8(&b, p + i, len) == 0) {
                unsigned cnt = dr_bits_ue(&b);  /* recovery_frame_cnt */
                if (cnt >= (1u << 16)) {
                    dr_vlog(logctx, DR_VLOG_ERROR, "recovery_frame_cnt %u is out of range\n", cnt);
                    return;
                }
                sei->recovery = 1;
            }
        }
        i += len;
    }
}

/* The picture timing message's pic_struct (D.2.3), with the active SPS:
 * >= 0, or < 0 when the message is broken (taken as absent). */
static int sei_pic_struct(const DrH264Sei *sei, const DrH264Sps *sps)
{
    DrBits b;
    int ps;

    dr_bits_init8(&b, sei->pt, sei->pt_size);
    if (sps->nal_hrd_parameters_present_flag || sps->vcl_hrd_parameters_present_flag)
        dr_bits_skip(&b, sps->cpb_removal_delay_length + sps->dpb_output_delay_length);
    if (!sps->pic_struct_present_flag)
        return -1;                          /* not read without the flag */
    ps = dr_bits_get(&b, 4);
    return ps > 8 ? -1 : ps;
}

/* ---- the slice header, through the decoded reference picture marking ---- */

/* The picture order count (8.2.1), from the slice fields in h. */
static int init_poc(DrH264 *h, const DrH264Sps *sps, int nal_ref_idc,
                    int64_t field_poc_out[2], int *pic_poc)
{
    const int max_frame_num = 1 << sps->log2_max_frame_num;
    int64_t field_poc[2];

    h->frame_num_offset = h->prev_frame_num_offset;
    if (h->frame_num < h->prev_frame_num)
        h->frame_num_offset += max_frame_num;

    if (sps->poc_type == 0) {
        const int max_poc_lsb = 1 << sps->log2_max_poc_lsb;
        if (h->prev_poc_lsb < 0)
            h->prev_poc_lsb = h->poc_lsb;
        if (h->poc_lsb < h->prev_poc_lsb &&
            h->prev_poc_lsb - h->poc_lsb >= max_poc_lsb / 2)
            h->poc_msb = h->prev_poc_msb + max_poc_lsb;
        else if (h->poc_lsb > h->prev_poc_lsb &&
                 h->prev_poc_lsb - h->poc_lsb < -max_poc_lsb / 2)
            h->poc_msb = h->prev_poc_msb - max_poc_lsb;
        else
            h->poc_msb = h->prev_poc_msb;
        field_poc[0] =
        field_poc[1] = h->poc_msb + h->poc_lsb;
        if (h->picture_structure == PICT_FRAME)
            field_poc[1] += h->delta_poc_bottom;
    } else if (sps->poc_type == 1) {
        int abs_frame_num;
        int64_t expected_delta_per_poc_cycle, expectedpoc;

        if (sps->poc_cycle_length != 0)
            abs_frame_num = h->frame_num_offset + h->frame_num;
        else
            abs_frame_num = 0;
        if (nal_ref_idc == 0 && abs_frame_num > 0)
            abs_frame_num--;
        expected_delta_per_poc_cycle = 0;
        for (int i = 0; i < sps->poc_cycle_length; i++)
            expected_delta_per_poc_cycle += sps->offset_for_ref_frame[i];
        if (abs_frame_num > 0) {
            int poc_cycle_cnt          = (abs_frame_num - 1) / sps->poc_cycle_length;
            int frame_num_in_poc_cycle = (abs_frame_num - 1) % sps->poc_cycle_length;
            expectedpoc = (int64_t)poc_cycle_cnt * expected_delta_per_poc_cycle;
            for (int i = 0; i <= frame_num_in_poc_cycle; i++)
                expectedpoc += sps->offset_for_ref_frame[i];
        } else
            expectedpoc = 0;
        if (nal_ref_idc == 0)
            expectedpoc += sps->offset_for_non_ref_pic;
        field_poc[0] = expectedpoc + h->delta_poc[0];
        field_poc[1] = field_poc[0] + sps->offset_for_top_to_bottom_field;
        if (h->picture_structure == PICT_FRAME)
            field_poc[1] += h->delta_poc[1];
    } else {
        int poc = 2 * (h->frame_num_offset + h->frame_num);
        if (!nal_ref_idc)
            poc--;
        field_poc[0] = poc;
        field_poc[1] = poc;
    }

    if (field_poc[0] != (int)field_poc[0] || field_poc[1] != (int)field_poc[1])
        return DR_ERR_INVALID;
    if (h->picture_structure != PICT_BOTTOM)
        field_poc_out[0] = field_poc[0];
    if (h->picture_structure != PICT_TOP)
        field_poc_out[1] = field_poc[1];
    *pic_poc = (int)(field_poc_out[0] < field_poc_out[1] ? field_poc_out[0] : field_poc_out[1]);
    return 0;
}

/* pred_weight_table (7.3.3.2): walked; a weight outside its 8 bits stops
 * the walk where the reference walk stopped. */
static void skip_pred_weights(DrBits *b, const DrH264Pps *pps, int slice_type_nos,
                              const int ref_count[2])
{
    int lists = slice_type_nos == DR_H26X_PIC_B ? 2 : 1;

    dr_bits_ue31(b);                        /* luma_log2_weight_denom */
    if (pps->sps.chroma_format_idc)
        dr_bits_ue31(b);                    /* chroma_log2_weight_denom */
    for (int list = 0; list < lists; list++)
        for (int i = 0; i < ref_count[list]; i++) {
            if (dr_bits_get1(b)) {          /* luma_weight_flag */
                int w = dr_bits_se(b), o = dr_bits_se(b);
                if ((int8_t)w != w || (int8_t)o != o)
                    return;
            }
            if (pps->sps.chroma_format_idc && dr_bits_get1(b))
                for (int j = 0; j < 2; j++) {
                    int w = dr_bits_se(b), o = dr_bits_se(b);
                    if ((int8_t)w != w || (int8_t)o != o)
                        return;
                }
        }
}

/* Whether the slice carries memory_management_control_operation 5 (7.3.3.3):
 * 1 / 0, or < 0 when the walk to it fails. */
static int scan_mmco_reset(void *logctx, DrH264 *h, DrBits *b, const DrH264Pps *pps,
                           int slice_type_nos)
{
    int list_count, ref_count[2];

    if (pps->redundant_pic_cnt_present)
        dr_bits_ue(b);                      /* redundant_pic_count */
    if (slice_type_nos == DR_H26X_PIC_B)
        dr_bits_get1(b);                    /* direct_spatial_mv_pred */

    ref_count[0] = pps->ref_count[0];
    ref_count[1] = pps->ref_count[1];
    if (slice_type_nos != DR_H26X_PIC_I) {
        unsigned max = h->picture_structure == PICT_FRAME ? 15 : 31;
        if (dr_bits_get1(b)) {              /* num_ref_idx_active_override_flag */
            ref_count[0] = dr_bits_ue(b) + 1;
            if (slice_type_nos == DR_H26X_PIC_B)
                ref_count[1] = dr_bits_ue(b) + 1;
            else
                ref_count[1] = 1;
        }
        list_count = slice_type_nos == DR_H26X_PIC_B ? 2 : 1;
        if ((unsigned)ref_count[0] - 1 > max ||
            (list_count == 2 && (unsigned)ref_count[1] - 1 > max)) {
            dr_vlog(logctx, DR_VLOG_ERROR, "reference overflow %u > %u or %u > %u\n",
                    ref_count[0] - 1, max, ref_count[1] - 1, max);
            return DR_ERR_INVALID;
        } else if ((unsigned)ref_count[1] - 1 > max) {
            ref_count[1] = 0;
        }
    } else {
        list_count   = 0;
        ref_count[0] = ref_count[1] = 0;
    }

    if (slice_type_nos != DR_H26X_PIC_I)
        for (int list = 0; list < list_count; list++) {
            if (!dr_bits_get1(b))           /* ref_pic_list_modification_flag */
                continue;
            for (int index = 0; ; index++) {
                unsigned idc = dr_bits_ue31(b);
                if (idc < 3)
                    dr_bits_ue(b);
                else if (idc > 3) {
                    dr_vlog(logctx, DR_VLOG_ERROR, "illegal reordering_of_pic_nums_idc %u\n", idc);
                    return DR_ERR_INVALID;
                } else
                    break;
                if (index >= ref_count[list]) {
                    dr_vlog(logctx, DR_VLOG_ERROR, "reference count %d overflow\n", index);
                    return DR_ERR_INVALID;
                }
            }
        }

    if ((pps->weighted_pred && slice_type_nos == DR_H26X_PIC_P) ||
        (pps->weighted_bipred_idc == 1 && slice_type_nos == DR_H26X_PIC_B))
        skip_pred_weights(b, pps, slice_type_nos, ref_count);

    if (dr_bits_get1(b)) {                  /* adaptive_ref_pic_marking_mode_flag */
        for (int i = 0; i < MAX_MMCO_COUNT; i++) {
            unsigned opcode;
            if (dr_bits_left(b) < 1)
                return DR_ERR_INVALID;
            opcode = dr_bits_ue31(b);       /* memory_management_control_operation */
            if (opcode > 6) {
                dr_vlog(logctx, DR_VLOG_ERROR, "illegal memory management control operation %u\n", opcode);
                return DR_ERR_INVALID;
            }
            if (opcode == 0)
                return 0;
            if (opcode == 5)
                return 1;
            if (opcode == 1 || opcode == 3)
                dr_bits_ue(b);              /* difference_of_pic_nums_minus1 */
            if (opcode == 3 || opcode == 2 || opcode == 6 || opcode == 4)
                dr_bits_ue31(b);
        }
    }
    return 0;
}

/* ---- the picture pass ---- */

/* pic_struct guessed from the pictures before: < 0 none. */
static int guess_ps(const DrH264 *h)
{
    int n = h->ps_n, l[128];

    if (n >= 9) {
        int v = h->ps_hist[h->ps_start], k;
        for (k = 1; k < n && h->ps_hist[(h->ps_start + k) & 127] == v; k++)
            ;
        if (k == n)
            return v;
    }
    if (n <= 10)
        return -1;
    for (int k = 0; k < n; k++)             /* the newest first */
        l[k] = h->ps_hist[(h->ps_start + n - 1 - k) & 127];
    for (int k = 2; k != (n & ~1); k += 2)
        if (l[k] != l[0] || l[k + 1] != l[1])
            return -1;
    return l[1];
}

/* slice_type % 5 -> the picture's class (7.4.3) */
static const int slice_pict[5] = {
    DR_H26X_PIC_P, DR_H26X_PIC_B, DR_H26X_PIC_I, DR_H26X_PIC_SP, DR_H26X_PIC_SI,
};

int dr_h264_picture(DrH264 *h, void *logctx, const uint8_t *buf, int size, DrH264Picture *pic)
{
    static const uint8_t fields[9] = { 2, 1, 1, 2, 2, 3, 3, 4, 6 };
    DrH264Sei sei = { 0 };
    const DrH264Sps *sps = NULL;
    DrBits b;
    int64_t field_poc[2];
    int i = 0, got_slice = 0, idr = 0, poc = 0, ps, ret;

    memset(pic, 0, sizeof(*pic));
    pic->type = DR_H26X_PIC_I;              /* the default the parser set */

    while (i + 2 < size && !got_slice) {
        int s0, e, type, ref_idc, src;

        if (buf[i] || buf[i + 1] || buf[i + 2] != 1) {
            i++;
            continue;
        }
        s0 = i + 3;
        if (s0 >= size)
            break;
        for (e = s0; e + 2 < size && (buf[e] || buf[e + 1] || buf[e + 2] > 1); e++)
            ;
        if (e + 2 >= size)
            e = size;
        type    = buf[s0] & 0x1f;
        ref_idc = (buf[s0] >> 5) & 3;

        switch (type) {
        case NAL_SPS:
            if ((ret = rbsp_get(h, buf + s0, e - s0, &b)) < 0)
                return ret;
            parse_sps(logctx, h, &b);
            break;
        case NAL_PPS:
            if ((ret = rbsp_get(h, buf + s0, e - s0, &b)) < 0)
                return ret;
            if ((ret = parse_pps(logctx, h, &b)) == DR_ERR(ENOMEM))
                return ret;
            break;
        case NAL_SEI:
            if ((ret = rbsp_get(h, buf + s0, e - s0, &b)) < 0)
                return ret;
            parse_sei(logctx, h, &sei, dr_bits_left(&b) / 8);
            break;
        case NAL_IDR_SLICE:
            idr = 1;
            h->prev_frame_num        = 0;
            h->prev_frame_num_offset = 0;
            h->prev_poc_msb          =
            h->prev_poc_lsb          = 0;
            /* fall through */
        case NAL_SLICE: {
            unsigned slice_type, pps_id;
            const DrH264Pps *pps;
            int got_reset = 0;

            /* only the header is walked (the reference parse read at most
             * 60 bytes of an IDR / disposable slice, 1000 otherwise) */
            src = e - s0;
            if (src > (type == NAL_IDR_SLICE || ref_idc == 0 ? 60 : 1000))
                src = type == NAL_IDR_SLICE || ref_idc == 0 ? 60 : 1000;
            if ((ret = rbsp_get(h, buf + s0, src, &b)) < 0)
                return ret;
            dr_bits_ue(&b);                 /* first_mb_in_slice */
            slice_type = dr_bits_ue31(&b);
            pic->type  = slice_pict[slice_type % 5];
            pps_id     = dr_bits_ue(&b);
            if (pps_id >= MAX_PPS_COUNT) {
                dr_vlog(logctx, DR_VLOG_ERROR, "pps_id %u out of range\n", pps_id);
                goto fail;
            }
            if (!h->pps_list[pps_id] || !h->pps_list[pps_id]->present) {
                dr_vlog(logctx, DR_VLOG_ERROR, "non-existing PPS %u referenced\n", pps_id);
                goto fail;
            }
            pps = h->pps_list[pps_id];
            sps = &pps->sps;
            h->active = pps;

            h->frame_num = dr_bits_get(&b, sps->log2_max_frame_num);
            if (sps->frame_mbs_only_flag) {
                h->picture_structure = PICT_FRAME;
            } else if (dr_bits_get1(&b)) {  /* field_pic_flag */
                h->picture_structure = PICT_TOP + dr_bits_get1(&b);
            } else {
                h->picture_structure = PICT_FRAME;
            }
            if (type == NAL_IDR_SLICE)
                dr_bits_ue(&b);             /* idr_pic_id */
            if (sps->poc_type == 0) {
                h->poc_lsb = dr_bits_get(&b, sps->log2_max_poc_lsb);
                if (pps->pic_order_present == 1 && h->picture_structure == PICT_FRAME)
                    h->delta_poc_bottom = dr_bits_se(&b);
            }
            if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero_flag) {
                h->delta_poc[0] = dr_bits_se(&b);
                if (pps->pic_order_present == 1 && h->picture_structure == PICT_FRAME)
                    h->delta_poc[1] = dr_bits_se(&b);
            }
            field_poc[0] = field_poc[1] = INT32_MAX;
            if (init_poc(h, sps, ref_idc, field_poc, &poc) < 0)
                goto fail;
            if (ref_idc && type != NAL_IDR_SLICE) {
                got_reset = scan_mmco_reset(logctx, h, &b, pps, pic->type & 3);
                if (got_reset < 0)
                    goto fail;
            }
            /* the prev_ values for the next picture; after a reset the next
             * picture counts from TopFieldOrderCnt less tempPicOrderCnt of
             * this one (8.2.1): the display order goes on above it */
            h->prev_frame_num        = got_reset ? 0 : h->frame_num;
            h->prev_frame_num_offset = got_reset ? 0 : h->frame_num_offset;
            if (ref_idc != 0) {
                if (!got_reset) {
                    h->prev_poc_msb = h->poc_msb;
                    h->prev_poc_lsb = h->poc_lsb;
                } else {
                    h->prev_poc_msb = 0;
                    h->prev_poc_lsb = h->picture_structure == PICT_BOTTOM ? 0
                                      : (int)field_poc[0] - poc;
                }
            }
            h->reset  = got_reset > 0;
            got_slice = 1;
            break;
        }
        }
        i = e;
    }

    if (!got_slice) {
        dr_vlog(logctx, DR_VLOG_ERROR, "missing picture in access unit with size %d\n", size);
        goto fail;
    }

    /* the values of the frame just cut */
    pic->key = sei.recovery || idr;
    /* the display order across a memory_management_control_operation 5:
     * the picture itself counts 0 after it (8.2.1) and is shown after
     * every picture before it */
    if (idr)
        h->order_base = 0;
    if (h->reset) {
        h->order_base = h->order_max + 2;
        poc = 0;
        if (!h->resets++)
            dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: H.264: a picture with memory_management_control_operation "
                    "5 (the picture order count starts again after it): display order continued as H.264 8.2.1 "
                    "says [untested on real discs]\n");
        dr_vlog(logctx, DR_VLOG_VERBOSE, "Title timeline: H.264: memory_management_control_operation 5 in picture %"
                PRId64" of the reset count\n", h->resets);
    }
    poc += h->order_base;
    if (idr || h->reset || poc > h->order_max)
        h->order_max = poc;
    pic->order = poc;

    if (!sps->nal_hrd_parameters_present_flag && !sps->vcl_hrd_parameters_present_flag &&
        !sps->pic_struct_present_flag)
        ps = 0;
    else if (sps->pic_struct_present_flag && sei.pt_present &&
             (ps = sei_pic_struct(&sei, sps)) >= 0) {
        int field = h->picture_structure != PICT_FRAME;
        /* checks of H.264 Table D-1 and D.2.3 (said for the first one, then
         * counted; the stream is kept as it is) */
        if ((field && ps != 1 && ps != 2) || (!field && (ps == 1 || ps == 2))) {
            if (!h->ps_field++) {
                snprintf(h->note, sizeof(h->note), "pic_struct %d on a %s picture (H.264 Table D-1: a field "
                         "takes 1 or 2, a frame the others): timed as the pic_struct says (the first one; all are "
                         "counted)", ps, field ? "field" : "frame");
                pic->note = h->note;
            }
        } else if ((ps == 7 || ps == 8) && !sps->fixed_frame_rate_flag && !h->ps_fixed++) {
            snprintf(h->note, sizeof(h->note), "pic_struct %d (frame doubling / tripling) without "
                     "fixed_frame_rate_flag (H.264 D.2.3): timed as it says (the first one; all are counted)", ps);
            pic->note = h->note;
        }
    }
    else if (sps->frame_mbs_only_flag)
        ps = 0;
    else if (h->picture_structure != PICT_FRAME)
        ps = h->picture_structure == PICT_BOTTOM ? 2 : 1;
    else if ((ps = guess_ps(h)) < 0) {
        dr_vlog(logctx, DR_VLOG_ERROR, "Title timeline: H.264: a frame of an interlaced stream without its "
                "pic_struct, and none can be guessed from the pictures before\n");
        return DR_ERR_INVALID;
    } else if (!h->guessed++) {
        snprintf(h->note, sizeof(h->note), "a frame of an interlaced H.264 stream without its pic_struct: "
                 "%d taken from the pictures before (the first one; all are counted) [untested on real discs]", ps);
        pic->note = h->note;
    }
    if (ps < 0 || ps > 8) {
        dr_vlog(logctx, DR_VLOG_ERROR, "Title timeline: H.264: pic_struct %d\n", ps);
        return DR_ERR_INVALID;
    }
    if (h->ps_n == 128) {
        h->ps_start = (h->ps_start + 1) & 127;
        h->ps_n--;
    }
    h->ps_hist[(h->ps_start + h->ps_n++) & 127] = ps;
    pic->fields = fields[ps];
    if (sps->timing_info_present_flag && sps->num_units_in_tick && sps->time_scale)
        dr_reduce(&pic->rate_num, &pic->rate_den, sps->time_scale,
                  2LL * sps->num_units_in_tick, 1 << 30);
    return 0;

fail:
    dr_vlog(logctx, DR_VLOG_ERROR, "Title timeline: H.264: a picture's headers cannot be read\n");
    return DR_ERR_INVALID;
}

void dr_h264_close(DrH264 *h, void *logctx)
{
    if (h->guessed)
        dr_vlog(logctx, DR_VLOG_INFO, "Title timeline: H.264: %"PRId64" frame(s) without their pic_struct: taken from "
                "the pictures before them [untested on real discs]\n", h->guessed);
    if (h->ps_field)
        dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: H.264: %"PRId64" picture(s) whose pic_struct does not fit "
                "their field_pic_flag (H.264 Table D-1)\n", h->ps_field);
    if (h->ps_fixed)
        dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: H.264: %"PRId64" picture(s) with pic_struct 7 / 8 without "
                "fixed_frame_rate_flag (H.264 D.2.3)\n", h->ps_fixed);
}

/* ---- the cut rule: the disc reader's access-unit rule ----
 * The stream is NAL units with start codes (Annex B). A NAL unit runs from
 * its 00 00 01 to the zero run in front of the next one; zero bytes go with
 * the start code after them.
 * Units the disc reader does not see stay in the chunk they fall in, for the
 * title timeline to take out: filler data, end of sequence and end of stream
 * (nal_unit_type 10-12) and the types that start no unit (0, 16-18, 21-23,
 * 25-27, 29, 31).
 * An access unit is every other unit before a picture's first slice
 * (parameter sets, SEI, delimiter, ...), the picture's slices, then its
 * trailing units. A slice belongs to the picture of the slice before it
 * while pic_parameter_set_id, frame_num, pic_order_cnt_lsb, field_pic_flag,
 * bottom_field_flag, IdrPicFlag and idr_pic_id stay the same (data
 * partitions B and C always do). Trailing units: after the slices, a run of
 * auxiliary slices (19), slice extensions (20) and types 28 / 30 (28 ends
 * the run), looked for across at most 100 units; SEI, PPS and the other
 * types that are neither in the run nor end it are passed over, and go with
 * the run when one follows. SPS, slices, delimiters and data partitions end
 * the search.
 * A picture whose first slice has a slice_type of 10 or more, or no
 * parameter sets yet, is left out with the units in front of it (no frame).
 * The reference program's repair of slices that disagree on frame_num /
 * pic_order_cnt_lsb: when the first slice of a picture is followed by
 * exactly 3 more slices that match it but for those two fields, and then a
 * delimiter (units the reader does not see not counted), the 4 slices are
 * one picture.
 * A data partition where a picture starts: the stream cannot be cut. */

enum { DISC_PREFIX, DISC_SLICES, DISC_SUFFIX };

#define DISC_REPAIR_SLICES 4    /* slices of a picture the repair takes together */

static int disc_hidden(int t)
{
    /* filler, ends of sequence / stream; the types that start no unit */
    return (t >= 10 && t <= 12) || !t || (t >= 16 && t <= 18) || (t >= 21 && t <= 23) ||
           (t >= 25 && t <= 27) || t == 29 || t == 31;
}

static int disc_push(DrH264Cut *st, int zr, int sc, int type)
{
    if (st->nb_nal == st->nal_cap) {
        int cap = st->nal_cap ? 2 * st->nal_cap : 64;
        DrH264CutNal *n = dr_realloc_array(st->nal, cap, sizeof(*n));
        if (!n)
            return DR_ERR(ENOMEM);
        st->nal     = n;
        st->nal_cap = cap;
    }
    st->nal[st->nb_nal++] = (DrH264CutNal){ zr, sc, type };
    return 0;
}

/* A parameter set of the access unit into the sets. */
static int disc_param_set(void *logctx, DrH264 *h, const uint8_t *nal, int size)
{
    DrBits b;
    int ret = rbsp_get(h, nal, size, &b);

    if (ret < 0)
        return ret;
    if ((nal[0] & 0x1f) == NAL_SPS)
        ret = parse_sps(logctx, h, &b);
    else
        ret = parse_pps(logctx, h, &b);
    return ret == DR_ERR(ENOMEM) ? ret : 0;
}

/* A slice header as far as the picture comparison reads it: 1 read, 0 no
 * parameter sets for it or cut short, 2 a slice_type of 10 or more. */
static int disc_slice(DrH264 *h, const uint8_t *nal, int size, DrH264CutSlice *s)
{
    uint8_t tmp[64] = { 0 };
    const DrH264Pps *pps;
    const DrH264Sps *sps;
    DrBits b;
    int n = 0, zeros = 0;

    /* the first bytes after the NAL header, without emulation prevention */
    for (int i = 1; i < size && n < 64; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        tmp[n++] = nal[i];
    }
    memset(s, 0, sizeof(*s));
    s->idr = (nal[0] & 0x1f) == NAL_IDR_SLICE;
    if (dr_bits_init8(&b, tmp, n) < 0)
        return 0;
    dr_bits_ue(&b);                         /* first_mb_in_slice */
    s->slice_type = dr_bits_ue31(&b);
    if (dr_bits_left(&b) < 0)
        return 0;
    if (s->slice_type >= 10)
        return 2;
    s->pps_id = (int)dr_bits_ue(&b);
    if (s->pps_id < 0 || s->pps_id >= MAX_PPS_COUNT || !(pps = h->pps_list[s->pps_id]) ||
        !pps->present || !(sps = &pps->sps)->present)
        return 0;
    if (sps->separate_colour_plane)
        dr_bits_skip(&b, 2);                /* colour_plane_id */
    s->frame_num = dr_bits_get(&b, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only_flag && (s->field = dr_bits_get1(&b)))
        s->bottom = dr_bits_get1(&b);
    if (s->idr)
        s->idr_id = dr_bits_ue(&b);
    if (!sps->poc_type)
        s->poc_lsb = dr_bits_get(&b, sps->log2_max_poc_lsb);
    return dr_bits_left(&b) >= 0;
}

/* The same picture but for frame_num and pic_order_cnt_lsb (the repair). */
static int disc_same_but_numbers(const DrH264CutSlice *a, const DrH264CutSlice *b)
{
    return a->pps_id == b->pps_id && a->field == b->field && (!a->field || a->bottom == b->bottom) &&
           a->idr == b->idr && (!a->idr || a->idr_id == b->idr_id);
}

static int disc_same(const DrH264CutSlice *a, const DrH264CutSlice *b)
{
    return a->pps_id == b->pps_id && a->frame_num == b->frame_num && a->poc_lsb == b->poc_lsb &&
           a->field == b->field && (!a->field || a->bottom == b->bottom) && a->idr == b->idr &&
           (!a->idr || a->idr_id == b->idr_id);
}

/* The chunk of the kept bytes up to end is decided: the cut starts anew. */
static int disc_end(DrH264Cut *st, int end, int no_unit)
{
    st->nb_nal = st->scan = st->done = st->nb_suffix = st->suffix_k = 0;
    st->phase   = DISC_PREFIX;
    st->no_unit = no_unit;
    st->look    = 0;
    st->forced  = -1;
    return end;
}

/* The repair after a picture's first slice (unit j is the first unit after
 * it): 1 the next units are the slices it takes in (st->forced set), 0 not
 * (also when they agree on their numbers: the picture is whole without it),
 * -1 more bytes are needed to tell. */
static int disc_repair(DrH264Cut *st, const uint8_t *buf, int avail, int final, int j)
{
    int idx[DISC_REPAIR_SLICES], n = 0, differ = 0;

    for (int i = j; i < st->nb_nal && n < DISC_REPAIR_SLICES; i++) {
        if (disc_hidden(st->nal[i].type))
            continue;
        if (i + 1 >= st->nb_nal && !final)
            return -1;                      /* this unit is not complete yet */
        idx[n++] = i;
    }
    if (n < DISC_REPAIR_SLICES)
        return final ? 0 : -1;
    if (st->nal[idx[DISC_REPAIR_SLICES - 1]].type != NAL_AUD)
        return 0;
    for (int k = 0; k < DISC_REPAIR_SLICES - 1; k++) {
        const DrH264CutNal *u = &st->nal[idx[k]];
        int end = idx[k] + 1 < st->nb_nal ? st->nal[idx[k] + 1].zr : avail;
        DrH264CutSlice s;
        if ((u->type != NAL_SLICE && u->type != NAL_IDR_SLICE) ||
            disc_slice(st->h, buf + u->sc + 3, end - u->sc - 3, &s) != 1 ||
            !disc_same_but_numbers(&s, &st->first))
            return 0;
        differ |= !disc_same(&s, &st->first);
    }
    if (!differ)
        return 0;
    st->forced = idx[DISC_REPAIR_SLICES - 2];
    return 1;
}

static int disc_fail(DrH264Cut *st, void *log, int type)
{
    dr_vlog(log, DR_VLOG_ERROR, "Title timeline: H.264: a data partition (NAL unit type %d) where a picture "
            "starts: the track cannot be ripped\n", type);
    disc_end(st, 0, 0);
    return DR_ERR_UNSUPPORTED;
}

static int disc_unit_size(void *log, const uint8_t *buf, int avail, int final, void *state)
{
    DrH264Cut *st = state;
    DrH264 *h = st->h;
    int i, ret;

    if (!st->nb_nal) {                      /* the unit's own start code */
        int z = 0;
        while (z < avail && !buf[z])
            z++;
        if (z + 1 >= avail)
            return final ? DR_CUT_NONE : 0;
        if (z < 2 || buf[z] != 1)
            return DR_CUT_NONE;
        if ((ret = disc_push(st, 0, z - 2, buf[z + 1] & 0x1f)) < 0)
            return ret;
        st->scan = z + 2;
    }
    for (i = st->scan; i + 3 < avail; i++) {
        int zr, lo;
        if (buf[i + 2] > 1) {
            i += 2;
            continue;
        }
        if (buf[i] || buf[i + 1] || buf[i + 2] != 1)
            continue;
        lo = st->nal[st->nb_nal - 1].sc + 4;
        for (zr = i; zr > lo && !buf[zr - 1]; zr--)
            ;
        if ((ret = disc_push(st, zr, i, buf[i + 3] & 0x1f)) < 0)
            return ret;
        i += 3;
    }
    st->scan = i;

    while (st->done < st->nb_nal) {
        int j = st->done, t = st->nal[j].type;
        int complete = j + 1 < st->nb_nal || final;
        int end = j + 1 < st->nb_nal ? st->nal[j + 1].zr : avail;
        const uint8_t *nal = buf + st->nal[j].sc + 3;
        int size = end - st->nal[j].sc - 3;
        DrH264CutSlice s;

        if (disc_hidden(t)) {
            st->done++;
            continue;
        }
        if (st->phase == DISC_PREFIX) {
            if (t >= NAL_DPA && t <= NAL_DPC)
                return disc_fail(st, log, t);
            if (t == NAL_SLICE || t == NAL_IDR_SLICE) {
                if (!complete)
                    return 0;
                if ((ret = disc_slice(h, nal, size, &s)) != 1) {
                    int slice_type = ret == 2;
                    ret = disc_end(st, end, 1);
                    st->why = slice_type ? "an H.264 picture with a slice_type of 10 or more (no slice type of "
                                         "H.264 7.4.3): left out with the units in front of it" :
                                         "an H.264 picture without its parameter sets (its SPS / PPS not "
                                         "seen): left out with the units in front of it";
                    return ret;
                }
                st->first = s;
                st->phase = DISC_SLICES;
                st->look  = 1;
            } else if (t == NAL_SPS || t == NAL_PPS) {
                if (!complete)
                    return 0;
                if ((ret = disc_param_set(log, h, nal, size)) < 0)
                    return ret;
            }
            st->done++;
            continue;
        }
        if (st->phase == DISC_SLICES) {
            if (st->look) {
                if ((ret = disc_repair(st, buf, avail, final, j)) < 0)
                    return 0;
                st->look = 0;
                if (ret)
                    dr_vlog(log, DR_VLOG_WARNING, "Title timeline: H.264: a picture's %d slices disagree on frame_num / "
                            "pic_order_cnt_lsb, a delimiter after them: taken as one picture (the reference "
                            "program's repair) [untested on real discs]\n", DISC_REPAIR_SLICES);
            }
            if (j <= st->forced) {
                st->done++;
                continue;
            }
            if (t == NAL_DPB || t == NAL_DPC) {
                st->done++;
                continue;
            }
            if (t == NAL_SLICE || t == NAL_IDR_SLICE || t == NAL_DPA) {
                if (!complete)
                    return 0;
                ret = disc_slice(h, nal, size, &s);
                if (ret == 1 && s.pps_id != st->first.pps_id)
                    dr_vlog(log, DR_VLOG_WARNING, "Title timeline: H.264: slices of another picture parameter set "
                            "follow without a unit between them: taken as the next picture "
                            "[untested on real discs]\n");
                if (ret == 1 && disc_same(&s, &st->first)) {
                    st->done++;
                    continue;
                }
                return disc_end(st, st->nal[j].zr, 0);
            }
            st->phase = DISC_SUFFIX;
        }
        /* trailing units: the run after the slices */
        {
            int suffix = t == 19 || t == 20 || t == 28 || t == 30;
            int stop   = t == NAL_SPS || t == NAL_SLICE || t == NAL_IDR_SLICE ||
                         t == NAL_AUD || (t >= NAL_DPA && t <= NAL_DPC) || t == 28;
            int e      = suffix ? st->nb_suffix + 1 : 0;

            st->suffix[st->nb_suffix++] = j;
            if (!e && st->suffix_k)
                return disc_end(st, st->nal[st->suffix[st->suffix_k]].zr, 0);
            st->suffix_k = e;
            if (stop || st->nb_suffix == DR_H264_SUFFIX_MAX) {
                if (st->suffix_k < st->nb_suffix)
                    return disc_end(st, st->nal[st->suffix[st->suffix_k]].zr, 0);
                if (j + 1 < st->nb_nal)
                    return disc_end(st, st->nal[j + 1].zr, 0);
                if (!final)
                    return 0;
                return disc_end(st, avail, 0);
            }
            st->done++;
        }
    }
    if (!final)
        return 0;
    /* the end of the stream */
    if (st->phase == DISC_PREFIX)
        return disc_end(st, avail, 1);      /* units without a picture */
    if (st->phase == DISC_SUFFIX && st->suffix_k < st->nb_suffix)
        return disc_end(st, st->nal[st->suffix[st->suffix_k]].zr, 0);
    return disc_end(st, avail, 0);
}

/* The next start code with the zero run in front of it; the last bytes
 * may begin one. */
static int disc_resync(const uint8_t *buf, int avail)
{
    for (int i = 0; i + 2 < avail; i++)
        if (!buf[i] && !buf[i + 1] && buf[i + 2] == 1) {
            while (i > 0 && !buf[i - 1])
                i--;
            return i;
        }
    return avail > 3 ? avail - 3 : -1;
}

const DrCutRule dr_h264_cut = { disc_unit_size, disc_resync, 0 };

void dr_h264_cut_init(DrH264Cut *st, DrH264 *h)
{
    memset(st, 0, sizeof(*st));
    st->h      = h;
    st->forced = -1;
}

void dr_h264_cut_end(DrH264Cut *st)
{
    dr_freep(&st->nal);
}
