/*
 * HEVC / H.265 (ISO/IEC 23008-2): the access-unit cut rule and the picture
 * values.
 *
 * It was once a disc mode of ours in FFmpeg's hevc/parser.c. The cut rule is
 * the one the splitter engine runs; dr_hevc_picture() reads the unit just cut.
 * The header walks (VPS, SPS, PPS, the slice segment header through
 * slice_pic_order_cnt_lsb) are our own, written from the spec to read bit
 * for bit what the processing library's parser read, so every value and
 * every finding stays identical across the move.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "base/err.h"
#include "base/mem.h"
#include "base/vlog.h"

#include "parse/bits.h"
#include "parse/hevc.h"
#include "parse/rational.h"
#include "parse/rules.h"

/* nal_unit_type (7.4.2.2) */
enum {
    NAL_TRAIL_N    = 0,
    NAL_TSA_N      = 2,
    NAL_STSA_N     = 4,
    NAL_RADL_N     = 6,
    NAL_RADL_R     = 7,
    NAL_RASL_N     = 8,
    NAL_RASL_R     = 9,
    NAL_BLA_W_LP   = 16,
    NAL_BLA_W_RADL = 17,
    NAL_BLA_N_LP   = 18,
    NAL_IDR_W_RADL = 19,
    NAL_IDR_N_LP   = 20,
    NAL_CRA_NUT    = 21,
    NAL_VPS        = 32,
    NAL_SPS        = 33,
    NAL_PPS        = 34,
    NAL_AUD        = 35,
    NAL_EOS_NUT    = 36,
    NAL_EOB_NUT    = 37,
    NAL_FD_NUT     = 38,
    NAL_SEI_PREFIX = 39,
    NAL_SEI_SUFFIX = 40,
};

/* SEI payload types (D.2.1) */
enum {
    SEI_PIC_TIMING     = 1,
    SEI_RECOVERY_POINT = 6,
};

#define MAX_VPS_COUNT 16
#define MAX_SPS_COUNT 16
#define MAX_PPS_COUNT 64
#define MAX_SHORT_TERM_RPS 64

typedef struct DrHevcVps {
    int present;
    int timing_info_present;
    uint32_t num_units_in_tick, time_scale;
} DrHevcVps;

/* The sequence parameter set, the fields the rules read (7.3.2.2.1), with
 * the VPS as it was when the set was read. */
typedef struct DrHevcSps {
    int present;
    int separate_colour_plane;
    int log2_max_poc_lsb;
    int ptl_progressive_source, ptl_interlaced_source;
    int vui_present;
    int field_seq_flag;
    int frame_field_info_present;
    int vui_timing_info_present;
    uint32_t vui_num_units_in_tick, vui_time_scale;
    DrHevcVps vps;
} DrHevcSps;

typedef struct DrHevcPps {
    int present;
    int output_flag_present;
    int num_extra_slice_header_bits;
    DrHevcSps sps;
} DrHevcPps;

struct DrHevc {
    DrHevcVps vps_list[MAX_VPS_COUNT];
    DrHevcSps sps_list[MAX_SPS_COUNT];
    DrHevcPps *pps_list[MAX_PPS_COUNT];
    /* the picture order count state (8.3.1) */
    int poc, poc_tid0;
    /* one-time notes already said (bits) */
    int said;
    /* pic_struct history (the latest 128 pictures) */
    uint8_t ps_hist[128];
    int ps_n, ps_start;
    /* the picture pass's scratch */
    uint8_t *rbsp;
    int rbsp_cap;
};

DrHevc *dr_hevc_new(void)
{
    return dr_mallocz(sizeof(DrHevc));
}

void dr_hevc_free(DrHevc **ph)
{
    DrHevc *h = *ph;

    if (!h)
        return;
    for (int i = 0; i < MAX_PPS_COUNT; i++)
        dr_free(h->pps_list[i]);
    dr_free(h->rbsp);
    dr_freep(ph);
}

/* The NAL unit's bytes after its 2-byte header without emulation prevention,
 * into h->rbsp, read with b. */
static int rbsp_get(DrHevc *h, const uint8_t *nal, int size, DrBits *b)
{
    int n = 0, zeros = 0;

    if (size > h->rbsp_cap) {
        uint8_t *r = dr_realloc(h->rbsp, size > 0 ? size : 1);
        if (!r)
            return DR_ERR(ENOMEM);
        h->rbsp     = r;
        h->rbsp_cap = size > 0 ? size : 1;
    }
    for (int i = 2; i < size; i++) {
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

/* profile_tier_level (7.3.3): walked; the general source flags kept. */
static int parse_ptl(DrBits *b, int max_sub_layers, int *progressive, int *interlaced)
{
    int sub_profile[8], sub_level[8];

    dr_bits_skip(b, 2 + 1 + 5 + 32);        /* space, tier, idc, compatibility */
    *progressive = dr_bits_get1(b);
    *interlaced  = dr_bits_get1(b);
    dr_bits_skip(b, 1 + 1);                 /* non_packed, frame_only */
    dr_bits_skip(b, 43 + 1);                /* reserved / extension flags */
    dr_bits_skip(b, 8);                     /* general_level_idc */
    for (int i = 0; i < max_sub_layers - 1; i++) {
        sub_profile[i] = dr_bits_get1(b);
        sub_level[i]   = dr_bits_get1(b);
    }
    if (max_sub_layers - 1 > 0)
        for (int i = max_sub_layers - 1; i < 8; i++)
            dr_bits_skip(b, 2);             /* reserved */
    for (int i = 0; i < max_sub_layers - 1; i++) {
        if (sub_profile[i])
            dr_bits_skip(b, 88);
        if (sub_level[i])
            dr_bits_skip(b, 8);
    }
    return dr_bits_left(b) < 0 ? DR_ERR_INVALID : 0;
}

/* A video parameter set of the access unit into the sets (7.3.2.1). */
static int parse_vps(void *logctx, DrHevc *h, DrBits *b)
{
    DrHevcVps vps = { 0 };
    int vps_id = dr_bits_get(b, 4), max_sub_layers, prog, il;
    unsigned num_layer_sets, max_layer_id;

    dr_bits_skip(b, 2 + 6);                 /* base layer flags, max_layers_minus1 */
    max_sub_layers = dr_bits_get(b, 3) + 1;
    dr_bits_get1(b);                        /* temporal_id_nesting */
    if (dr_bits_get(b, 16) != 0xffff) {
        dr_vlog(logctx, DR_VLOG_ERROR, "vps_reserved_ffff_16bits is not 0xffff\n");
        return DR_ERR_INVALID;
    }
    if (max_sub_layers > 7)
        return DR_ERR_INVALID;
    if (parse_ptl(b, max_sub_layers, &prog, &il) < 0)
        return DR_ERR_INVALID;
    {
        int present = dr_bits_get1(b);      /* sub_layer_ordering_info_present */
        for (int i = present ? 0 : max_sub_layers - 1; i < max_sub_layers; i++) {
            dr_bits_ue(b);
            dr_bits_ue(b);
            dr_bits_ue(b);
        }
    }
    max_layer_id   = dr_bits_get(b, 6);
    num_layer_sets = dr_bits_ue(b) + 1;
    if (num_layer_sets > 1024)
        return DR_ERR_INVALID;
    for (unsigned i = 1; i < num_layer_sets; i++)
        dr_bits_skip(b, max_layer_id + 1);  /* layer_id_included_flag[] */
    vps.timing_info_present = dr_bits_get1(b);
    if (vps.timing_info_present) {
        vps.num_units_in_tick = dr_bits_get(b, 32);
        vps.time_scale        = dr_bits_get(b, 32);
    }
    if (dr_bits_left(b) < 0)
        return DR_ERR_INVALID;
    vps.present = 1;
    h->vps_list[vps_id] = vps;
    return 0;
}

/* short_term_ref_pic_set (7.3.7): walked; returns the set's NumDeltaPocs,
 * or < 0. num_delta holds the sets before it. */
static int skip_st_rps(DrBits *b, int idx, const int *num_delta)
{
    if (idx && dr_bits_get1(b)) {           /* inter_ref_pic_set_prediction_flag */
        int ref = num_delta[idx - 1], n = 0;
        dr_bits_get1(b);                    /* delta_rps_sign */
        dr_bits_ue(b);                      /* abs_delta_rps_minus1 */
        for (int j = 0; j <= ref; j++) {
            int used = dr_bits_get1(b);     /* used_by_curr_pic_flag */
            int use_delta = used ? 1 : dr_bits_get1(b);
            n += used || use_delta;
        }
        return n;
    } else {
        unsigned neg = dr_bits_ue(b), pos = dr_bits_ue(b);
        if (neg > 16 || pos > 16 - neg)
            return DR_ERR_INVALID;
        for (unsigned i = 0; i < neg; i++) {
            dr_bits_ue(b);                  /* delta_poc_s0_minus1 */
            dr_bits_get1(b);                /* used_by_curr_pic_s0_flag */
        }
        for (unsigned i = 0; i < pos; i++) {
            dr_bits_ue(b);
            dr_bits_get1(b);
        }
        return neg + pos;
    }
}

/* scaling_list_data (7.3.4): walked. */
static void skip_scaling_list(DrBits *b)
{
    for (int size_id = 0; size_id < 4; size_id++)
        for (int matrix_id = 0; matrix_id < 6; matrix_id += size_id == 3 ? 3 : 1) {
            if (!dr_bits_get1(b)) {         /* scaling_list_pred_mode_flag */
                dr_bits_ue(b);              /* scaling_list_pred_matrix_id_delta */
            } else {
                int n = size_id > 0 ? 64 : 16;
                if (n > (1 << (4 + (size_id << 1))))
                    n = 1 << (4 + (size_id << 1));
                if (size_id > 1)
                    dr_bits_se(b);          /* scaling_list_dc_coef_minus8 */
                for (int i = 0; i < n; i++)
                    dr_bits_se(b);          /* scaling_list_delta_coef */
            }
        }
}

/* A seq parameter set of the access unit into the sets (7.3.2.2.1): the
 * fields the rules read, with the checks whose failure keeps a set out. */
static int parse_sps(void *logctx, DrHevc *h, DrBits *b)
{
    DrHevcSps sps = { 0 };
    int num_delta[MAX_SHORT_TERM_RPS];
    int vps_id = dr_bits_get(b, 4), max_sub_layers, chroma;
    unsigned sps_id, v, num_rps;

    if (!h->vps_list[vps_id].present) {
        dr_vlog(logctx, DR_VLOG_ERROR, "VPS %d does not exist\n", vps_id);
        return DR_ERR_INVALID;
    }
    sps.vps = h->vps_list[vps_id];
    max_sub_layers = dr_bits_get(b, 3) + 1;
    if (max_sub_layers > 7)
        return DR_ERR_INVALID;
    dr_bits_get1(b);                        /* temporal_id_nesting */
    if (parse_ptl(b, max_sub_layers, &sps.ptl_progressive_source, &sps.ptl_interlaced_source) < 0)
        return DR_ERR_INVALID;
    sps_id = dr_bits_ue(b);
    if (sps_id >= MAX_SPS_COUNT) {
        dr_vlog(logctx, DR_VLOG_ERROR, "SPS id out of range: %u\n", sps_id);
        return DR_ERR_INVALID;
    }
    chroma = dr_bits_ue(b);
    if (chroma > 3)
        return DR_ERR_INVALID;
    if (chroma == 3)
        sps.separate_colour_plane = dr_bits_get1(b);
    dr_bits_ue(b);                          /* pic_width_in_luma_samples */
    dr_bits_ue(b);                          /* pic_height_in_luma_samples */
    if (dr_bits_get1(b)) {                  /* conformance_window_flag */
        dr_bits_ue(b);
        dr_bits_ue(b);
        dr_bits_ue(b);
        dr_bits_ue(b);
    }
    dr_bits_ue(b);                          /* bit_depth_luma_minus8 */
    dr_bits_ue(b);                          /* bit_depth_chroma_minus8 */
    v = dr_bits_ue(b);                      /* log2_max_pic_order_cnt_lsb_minus4 */
    if (v > 12) {
        dr_vlog(logctx, DR_VLOG_ERROR, "log2_max_pic_order_cnt_lsb_minus4 out of range: %u\n", v);
        return DR_ERR_INVALID;
    }
    sps.log2_max_poc_lsb = v + 4;
    {
        int present = dr_bits_get1(b);      /* sub_layer_ordering_info_present */
        for (int i = present ? 0 : max_sub_layers - 1; i < max_sub_layers; i++) {
            dr_bits_ue(b);
            dr_bits_ue(b);
            dr_bits_ue(b);
        }
    }
    dr_bits_ue(b);                          /* log2_min_luma_coding_block_size_minus3 */
    dr_bits_ue(b);                          /* log2_diff_max_min_luma_coding_block_size */
    dr_bits_ue(b);                          /* log2_min_luma_transform_block_size_minus2 */
    dr_bits_ue(b);                          /* log2_diff_max_min_luma_transform_block_size */
    dr_bits_ue(b);                          /* max_transform_hierarchy_depth_inter */
    dr_bits_ue(b);                          /* max_transform_hierarchy_depth_intra */
    if (dr_bits_get1(b) && dr_bits_get1(b)) /* scaling_list_enabled, data present */
        skip_scaling_list(b);
    dr_bits_get1(b);                        /* amp_enabled_flag */
    dr_bits_get1(b);                        /* sample_adaptive_offset_enabled_flag */
    if (dr_bits_get1(b)) {                  /* pcm_enabled_flag */
        dr_bits_skip(b, 4 + 4);
        dr_bits_ue(b);
        dr_bits_ue(b);
        dr_bits_get1(b);
    }
    num_rps = dr_bits_ue(b);                /* num_short_term_ref_pic_sets */
    if (num_rps > MAX_SHORT_TERM_RPS)
        return DR_ERR_INVALID;
    for (unsigned i = 0; i < num_rps; i++) {
        int n = skip_st_rps(b, i, num_delta);
        if (n < 0)
            return DR_ERR_INVALID;
        num_delta[i] = n;
    }
    if (dr_bits_get1(b)) {                  /* long_term_ref_pics_present_flag */
        unsigned n = dr_bits_ue(b);         /* num_long_term_ref_pics_sps */
        if (n > 32)
            return DR_ERR_INVALID;
        for (unsigned i = 0; i < n; i++) {
            dr_bits_skip(b, sps.log2_max_poc_lsb);
            dr_bits_get1(b);                /* used_by_curr_pic_lt_sps_flag */
        }
    }
    dr_bits_get1(b);                        /* sps_temporal_mvp_enabled_flag */
    dr_bits_get1(b);                        /* strong_intra_smoothing_enabled_flag */
    sps.vui_present = dr_bits_get1(b);
    if (sps.vui_present) {                  /* vui_parameters (E.2.1) */
        if (dr_bits_get1(b)) {              /* aspect_ratio_info_present_flag */
            if (dr_bits_get(b, 8) == 255)
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
        dr_bits_get1(b);                    /* neutral_chroma_indication_flag */
        sps.field_seq_flag            = dr_bits_get1(b);
        sps.frame_field_info_present  = dr_bits_get1(b);
        if (dr_bits_get1(b)) {              /* default_display_window_flag */
            dr_bits_ue(b);
            dr_bits_ue(b);
            dr_bits_ue(b);
            dr_bits_ue(b);
        }
        sps.vui_timing_info_present = dr_bits_get1(b);
        if (sps.vui_timing_info_present) {
            sps.vui_num_units_in_tick = dr_bits_get(b, 32);
            sps.vui_time_scale        = dr_bits_get(b, 32);
        }
        /* the rest of the VUI is not read */
    }
    if (dr_bits_left(b) < 0)
        return DR_ERR_INVALID;
    sps.present = 1;
    h->sps_list[sps_id] = sps;
    return 0;
}

/* A picture parameter set of the access unit into the sets (7.3.2.3). */
static int parse_pps(void *logctx, DrHevc *h, DrBits *b)
{
    DrHevcPps pps = { 0 };
    unsigned pps_id = dr_bits_ue(b), sps_id;

    if (pps_id >= MAX_PPS_COUNT) {
        dr_vlog(logctx, DR_VLOG_ERROR, "PPS id out of range: %u\n", pps_id);
        return DR_ERR_INVALID;
    }
    sps_id = dr_bits_ue(b);
    if (sps_id >= MAX_SPS_COUNT || !h->sps_list[sps_id].present) {
        dr_vlog(logctx, DR_VLOG_ERROR, "SPS %u does not exist\n", sps_id);
        return DR_ERR_INVALID;
    }
    pps.sps = h->sps_list[sps_id];
    dr_bits_get1(b);                        /* dependent_slice_segments_enabled_flag */
    pps.output_flag_present          = dr_bits_get1(b);
    pps.num_extra_slice_header_bits  = dr_bits_get(b, 3);
    if (dr_bits_left(b) < 0)
        return DR_ERR_INVALID;
    pps.present = 1;
    if (!h->pps_list[pps_id] && !(h->pps_list[pps_id] = dr_malloc(sizeof(DrHevcPps))))
        return DR_ERR(ENOMEM);
    *h->pps_list[pps_id] = pps;
    return 0;
}

/* The picture order count (8.3.1). */
static int compute_poc(const DrHevcSps *sps, int poc_tid0, int poc_lsb, int nal_type)
{
    int max_poc_lsb  = 1 << sps->log2_max_poc_lsb;
    int prev_poc_lsb = poc_tid0 % max_poc_lsb;
    int prev_poc_msb = poc_tid0 - prev_poc_lsb;
    int poc_msb;

    if (poc_lsb < prev_poc_lsb && prev_poc_lsb - poc_lsb >= max_poc_lsb / 2)
        poc_msb = prev_poc_msb + max_poc_lsb;
    else if (poc_lsb > prev_poc_lsb && poc_lsb - prev_poc_lsb > max_poc_lsb / 2)
        poc_msb = prev_poc_msb - max_poc_lsb;
    else
        poc_msb = prev_poc_msb;

    /* for BLA picture types, POCmsb is 0 */
    if (nal_type == NAL_BLA_W_LP || nal_type == NAL_BLA_W_RADL || nal_type == NAL_BLA_N_LP)
        poc_msb = 0;

    return poc_msb + poc_lsb;
}

/* ---- the cut rule: the disc reader's access-unit rule ----
 * The stream is NAL units with start codes (Annex B), each with its 2-byte
 * NAL unit header. A NAL unit runs from its 00 00 01 to the zero run in front
 * of the next one; zero bytes go with the start code after them.
 * Units the disc reader does not see stay in the chunk they fall in, for the
 * title timeline to take out: end of sequence, end of bitstream, filler data
 * (nal_unit_type 36-38), the reserved types (10-15, 22-31, 41-61), units of
 * another layer (nuh_layer_id other than 0: on a disc the second layer is a
 * stream of its own) and units too short for their header.
 * An access unit is every other unit before a picture's first slice segment
 * (parameter sets, SEI, delimiter, ...), the picture's slice segments, then
 * its trailing units. A slice segment belongs to the picture of the one
 * before it while it is not the first slice segment of a picture
 * (first_slice_segment_in_pic_flag 0) and its slice_pic_parameter_set_id is
 * the picture's. Trailing units: after the slice segments, a run of suffix
 * SEI (40) and types 62 / 63 (62 ends the run), looked for across at most
 * 100 units; prefix SEI, parameter sets and the other types that are neither
 * in the run nor end it are passed over, and go with the run when one
 * follows. SPS, slice segments and delimiters end the search; so does a
 * type 62 unit, which is part of the run (end of sequence / end of
 * bitstream units right after it stay with the access unit: they are its
 * last units, 7.4.2.4.4).
 * A picture whose first slice segment does not start a picture, or that has
 * no parameter sets yet, is left out with the units in front of it (no
 * frame). */

enum { DISC_PREFIX, DISC_SLICES, DISC_SUFFIX };

static int disc_vcl(int t)
{
    return t <= NAL_RASL_R || (t >= NAL_BLA_W_LP && t <= NAL_CRA_NUT);
}

/* Whether the unit (its type, layer, bytes after the start code) is one
 * the disc reader does not see. */
static int disc_hidden(int t, int layer, int size)
{
    return size < 2 || layer || (t >= 10 && t <= 15) || (t >= 22 && t <= 31) ||
           (t >= NAL_EOS_NUT && t <= NAL_FD_NUT) || (t >= 41 && t <= 61);
}

static int disc_push(DrHevcCut *st, int zr, int sc, const uint8_t *hdr)
{
    if (st->nb_nal == st->nal_cap) {
        int cap = st->nal_cap ? 2 * st->nal_cap : 64;
        DrHevcCutNal *n = dr_realloc_array(st->nal, cap, sizeof(*n));
        if (!n)
            return DR_ERR(ENOMEM);
        st->nal     = n;
        st->nal_cap = cap;
    }
    st->nal[st->nb_nal++] = (DrHevcCutNal){ zr, sc, (hdr[0] >> 1) & 0x3f, (hdr[0] & 1) << 5 | hdr[1] >> 3 };
    return 0;
}

/* A parameter set of the access unit into the sets. */
static int disc_param_set(void *logctx, DrHevc *h, const uint8_t *nal, int size)
{
    DrBits b;
    int t = (nal[0] >> 1) & 0x3f;
    int ret = rbsp_get(h, nal, size, &b);

    if (ret < 0)
        return ret;
    if (t == NAL_VPS)
        ret = parse_vps(logctx, h, &b);
    else if (t == NAL_SPS)
        ret = parse_sps(logctx, h, &b);
    else
        ret = parse_pps(logctx, h, &b);
    return ret == DR_ERR(ENOMEM) ? ret : 0;
}

/* A slice segment header as far as the picture comparison reads it:
 * first_slice_segment_in_pic_flag and slice_pic_parameter_set_id; 0 when cut
 * short. */
static int disc_slice(const uint8_t *nal, int size, DrHevcCutSlice *s)
{
    uint8_t tmp[16] = { 0 };
    int t = (nal[0] >> 1) & 0x3f, n = 0, zeros = 0;
    DrBits b;

    /* the first bytes after the NAL unit header, without emulation prevention */
    for (int i = 2; i < size && n < 16; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        tmp[n++] = nal[i];
    }
    if (!n || dr_bits_init8(&b, tmp, n) < 0)
        return 0;
    s->first = dr_bits_get1(&b);
    if (t >= NAL_BLA_W_LP && t <= 23)
        dr_bits_get1(&b);                   /* no_output_of_prior_pics_flag */
    s->pps_id = (int)dr_bits_ue(&b);
    return dr_bits_left(&b) >= 0;
}

/* The picture's parameter sets are there (a stored PPS always carries its
 * SPS, and that its VPS). */
static int disc_has_sets(const DrHevc *h, int pps_id)
{
    return pps_id >= 0 && pps_id < MAX_PPS_COUNT && h->pps_list[pps_id] &&
           h->pps_list[pps_id]->present;
}

/* The chunk of the kept bytes up to end is decided: the cut starts anew. */
static int disc_end(DrHevcCut *st, int end, int no_unit)
{
    st->nb_nal = st->scan = st->done = st->nb_suffix = st->suffix_k = 0;
    st->phase   = DISC_PREFIX;
    st->no_unit = no_unit;
    st->pictures += !no_unit;
    return end;
}

static int disc_unit_size(void *log, const uint8_t *buf, int avail, int final, void *state)
{
    DrHevcCut *st = state;
    int i, ret;

    if (!st->nb_nal) {                      /* the unit's own start code */
        int z = 0;
        while (z < avail && !buf[z])
            z++;
        if (z + 2 >= avail)
            return final ? DR_CUT_NONE : 0;
        if (z < 2 || buf[z] != 1)
            return DR_CUT_NONE;
        if ((ret = disc_push(st, 0, z - 2, buf + z + 1)) < 0)
            return ret;
        st->scan = z + 2;
    }
    for (i = st->scan; i + 4 < avail; i++) {
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
        if ((ret = disc_push(st, zr, i, buf + i + 3)) < 0)
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
        DrHevcCutSlice s;

        if (!complete && size < 2)
            return 0;
        if (disc_hidden(t, st->nal[j].layer, size)) {
            st->done++;
            continue;
        }
        if (st->phase == DISC_PREFIX) {
            if (disc_vcl(t)) {
                if (!complete)
                    return 0;
                if (!disc_slice(nal, size, &s) || !s.first || !disc_has_sets(st->h, s.pps_id)) {
                    /* no decoder can decode it (H.265 7.4.2.4.2): damaged
                     * data, or a stream read from the middle */
                    dr_vlog(log, DR_VLOG_WARNING, "Title timeline: HEVC: a picture %s after picture %"PRId64
                            " of the segment: it cannot be decoded and is left out with the units in front of it (damaged data?) "
                            "[untested on real discs]\n", !s.first ? "that does not start with its first slice "
                            "segment" : "without its parameter sets", st->pictures);
                    return disc_end(st, end, 1);
                }
                st->first = s;
                st->phase = DISC_SLICES;
            } else if (t >= NAL_VPS && t <= NAL_PPS) {
                if (!complete)
                    return 0;
                if ((ret = disc_param_set(log, st->h, nal, size)) < 0)
                    return ret;
            }
            st->done++;
            continue;
        }
        if (st->phase == DISC_SLICES) {
            if (disc_vcl(t)) {
                int ok;
                if (!complete)
                    return 0;
                /* only first_slice_segment_in_pic_flag starts a picture
                 * (H.265 7.4.7.1); a slice segment of another PPS or one
                 * whose header cannot be read is damaged data of this
                 * picture: kept with it (a decoder skips that slice
                 * segment and decodes the rest of the picture) */
                ok = disc_slice(nal, size, &s);
                if (ok && s.first)
                    return disc_end(st, st->nal[j].zr, 0);
                if (!ok || s.pps_id != st->first.pps_id)
                    dr_vlog(log, DR_VLOG_WARNING, "Title timeline: HEVC: a slice segment %s in picture %"PRId64
                            " of the segment: kept in it (damaged data?) [untested on real discs]\n", ok ?
                            "of another picture parameter set than its picture's" : "whose header cannot be read",
                            st->pictures + 1);
                st->done++;
                continue;
            }
            st->phase = DISC_SUFFIX;
        }
        /* trailing units: the run after the slice segments */
        {
            int suffix = t == NAL_SEI_SUFFIX || t == 62 || t == 63;
            int stop   = t == NAL_SPS || disc_vcl(t) || t == NAL_AUD || t == 62;
            int e      = suffix ? st->nb_suffix + 1 : 0;

            st->suffix[st->nb_suffix++] = j;
            if (!e && st->suffix_k)
                return disc_end(st, st->nal[st->suffix[st->suffix_k]].zr, 0);
            st->suffix_k = e;
            if (stop || st->nb_suffix == DR_HEVC_SUFFIX_MAX) {
                if (st->suffix_k < st->nb_suffix)
                    return disc_end(st, st->nal[st->suffix[st->suffix_k]].zr, 0);
                /* the run ended with this unit (type 62): end of sequence /
                 * end of bitstream units right after it are the access
                 * unit's last units (H.265 7.4.2.4.4) */
                while (j + 1 < st->nb_nal && (st->nal[j + 1].type == NAL_EOS_NUT ||
                                              st->nal[j + 1].type == NAL_EOB_NUT) && !st->nal[j + 1].layer)
                    j++;
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

const DrCutRule dr_hevc_cut = { disc_unit_size, disc_resync, 0 };

void dr_hevc_cut_init(DrHevcCut *st, DrHevc *h)
{
    memset(st, 0, sizeof(*st));
    st->h = h;
}

void dr_hevc_cut_end(DrHevcCut *st)
{
    dr_freep(&st->nal);
}

/* ---- the picture values ---- */

/* The SEI messages of a prefix SEI unit (from its NAL unit header, size
 * bytes) in front of the picture: the picture timing's pic_struct (the
 * first 4 bits of its payload; *ps stays < 0 without one) and whether a
 * recovery point is there. */
static void disc_sei(const uint8_t *nal, int size, int *ps, int *recovery)
{
    uint8_t *b = dr_malloc(size > 0 ? size : 1);
    int n = 0, zeros = 0, i = 0;

    if (!b)
        return;
    for (int k = 2; k < size; k++) {        /* the RBSP after the header */
        if (zeros >= 2 && nal[k] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[k] ? 0 : zeros + 1;
        b[n++] = nal[k];
    }
    while (n > 0 && !b[n - 1])
        n--;
    if (n < 1 || b[n - 1] != 0x80)          /* rbsp_trailing_bits */
        goto end;
    n--;
    while (i < n) {
        int type = 0, len = 0;
        while (i < n && b[i] == 0xff)
            type += b[i++];
        if (i >= n)
            break;
        type += b[i++];
        while (i < n && b[i] == 0xff)
            len += b[i++];
        if (i >= n)
            break;
        len += b[i++];
        if (len > n - i)
            break;
        if (type == SEI_PIC_TIMING && len && *ps < 0)
            *ps = b[i] >> 4;
        if (type == SEI_RECOVERY_POINT && len)
            *recovery = 1;
        i += len;
    }
end:
    dr_free(b);
}

/* pic_struct guessed from the pictures before: < 0 none. */
static int guess_ps(const DrHevc *h)
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

int dr_hevc_picture(DrHevc *h, void *logctx, const uint8_t *buf, int size, DrHevcPicture *pic)
{
    static const uint8_t fields[9] = { 2, 1, 1, 2, 2, 3, 3, 4, 6 };
    const DrHevcPps *pps = NULL;
    const DrHevcSps *sps;
    DrBits b;
    uint32_t num = 0, den = 0;
    int ps = -1, recovery = 0, ffi, field_coded;
    int i = 0, got_slice = 0, ret;

    memset(pic, 0, sizeof(*pic));
    pic->type     = DR_H26X_PIC_I;          /* the default the parser set */
    pic->rate_den = 1;

    while (i + 4 < size && !got_slice) {
        int s0, e, t, layer, tid;

        if (buf[i] || buf[i + 1] || buf[i + 2] != 1) {
            i++;
            continue;
        }
        s0    = i + 3;
        t     = (buf[s0] >> 1) & 0x3f;
        layer = (buf[s0] & 1) << 5 | buf[s0 + 1] >> 3;
        tid   = (buf[s0 + 1] & 7) - 1;
        if (layer) {
            i += 2;
            continue;
        }
        for (e = s0; e + 2 < size && (buf[e] || buf[e + 1] || buf[e + 2] > 1); e++)
            ;
        if (e + 2 >= size)
            e = size;

        if (t == NAL_SEI_PREFIX)
            disc_sei(buf + s0, e - s0, &ps, &recovery);
        else if (t >= NAL_VPS && t <= NAL_PPS) {
            if ((ret = disc_param_set(logctx, h, buf + s0, e - s0)) < 0)
                return ret;
        } else if (disc_vcl(t)) {
            unsigned pps_id, slice_type;
            int irap = t >= NAL_BLA_W_LP && t <= 23;

            if ((ret = rbsp_get(h, buf + s0, e - s0, &b)) < 0)
                return ret;
            dr_bits_get1(&b);               /* first_slice_segment_in_pic_flag */
            if (irap)
                dr_bits_get1(&b);           /* no_output_of_prior_pics_flag */
            pps_id = dr_bits_ue(&b);
            if (pps_id >= MAX_PPS_COUNT || !h->pps_list[pps_id] || !h->pps_list[pps_id]->present) {
                dr_vlog(logctx, DR_VLOG_ERROR, "PPS id out of range: %u\n", pps_id);
                goto fail;
            }
            pps = h->pps_list[pps_id];
            sps = &pps->sps;
            for (int k = 0; k < pps->num_extra_slice_header_bits; k++)
                dr_bits_get1(&b);           /* slice_reserved_flag[] */
            slice_type = dr_bits_ue31(&b);
            if (slice_type > 2) {           /* B 0, P 1, I 2 (7.4.7.1) */
                dr_vlog(logctx, DR_VLOG_ERROR, "Unknown slice type: %u.\n", slice_type);
                goto fail;
            }
            pic->type = slice_type == 0 ? DR_H26X_PIC_B :
                        slice_type == 1 ? DR_H26X_PIC_P : DR_H26X_PIC_I;
            pic->key  = irap;
            if (pps->output_flag_present)
                dr_bits_get1(&b);           /* pic_output_flag */
            if (sps->separate_colour_plane)
                dr_bits_skip(&b, 2);        /* colour_plane_id */
            if (t == NAL_IDR_W_RADL || t == NAL_IDR_N_LP) {
                h->poc = 0;
            } else {
                int poc_lsb = dr_bits_get(&b, sps->log2_max_poc_lsb);
                h->poc = compute_poc(sps, h->poc_tid0, poc_lsb, t);
            }
            pic->order = h->poc;
            if (tid == 0 && t != NAL_TRAIL_N && t != NAL_TSA_N && t != NAL_STSA_N &&
                t != NAL_RADL_N && t != NAL_RASL_N && t != NAL_RADL_R && t != NAL_RASL_R)
                h->poc_tid0 = h->poc;
            got_slice = 1;
        }
        i = e;
    }

    if (!got_slice) {
        dr_vlog(logctx, DR_VLOG_ERROR, "missing picture in access unit with size %d\n", size);
        goto fail;
    }

    sps = &pps->sps;
    pic->key = pic->key || recovery;
    /* a stream coded as fields: Ultra HD Blu-ray video is progressive */
    field_coded = sps->vui_present && sps->field_seq_flag;
    if (field_coded && !(h->said & 8)) {
        h->said |= 8;
        dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: HEVC: a stream coded as fields (field_seq_flag; Ultra HD "
                "Blu-ray video is progressive): each field picture lasts one field period [untested on real "
                "discs]\n");
    }
    if (!sps->vui_present && !(h->said & 1)) {
        h->said |= 1;
        dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: HEVC: a sequence without VUI (a Blu-ray stream states its "
                "colours and timing there): its pictures taken as frames [untested on real discs]\n");
    }
    /* frame_field_info_present_flag; without VUI inferred from the profile's
     * source flags (H.265 E.3.1) */
    ffi = sps->vui_present ? sps->frame_field_info_present :
          sps->ptl_progressive_source && sps->ptl_interlaced_source;
    if (!ffi) {
        ps = 0;
    } else if (ps < 0 && field_coded) {
        /* the reference program's way: guessed from the pictures before */
        if ((ps = guess_ps(h)) < 0) {
            dr_vlog(logctx, DR_VLOG_ERROR, "Title timeline: HEVC: a field picture without its pic_struct, and none "
                    "can be guessed from the pictures before\n");
            return DR_ERR_INVALID;
        }
        if (!(h->said & 16)) {
            h->said |= 16;
            dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: HEVC: a picture of a field stream without its "
                    "picture timing SEI: pic_struct %d taken from the pictures before [untested on real discs]\n",
                    ps);
        }
    } else if (ps < 0) {
        if (!(h->said & 2)) {
            h->said |= 2;
            dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: HEVC: a picture without the picture timing SEI its "
                    "frame_field_info_present_flag asks for (a stream fault, H.265 E.3.1): taken as a frame "
                    "[untested on real discs]\n");
        }
        ps = 0;
    }
    if (ps > 8) {
        dr_vlog(logctx, DR_VLOG_ERROR, "Title timeline: HEVC: pic_struct %d (%s): the stream cannot be cut\n", ps,
                ps <= 12 ? "a field paired with the picture before or after" : "a reserved value");
        return DR_ERR_INVALID;
    }
    /* checks only (H.265 D.3.3): a field's pic_struct on a frame, a frame's on a field */
    if ((ps == 1 || ps == 2) != field_coded && !(h->said & 32)) {
        h->said |= 32;
        dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: HEVC: pic_struct %d on a %s (field_seq_flag %d): timed as "
                "pic_struct says\n", ps, field_coded ? "field picture" : "frame", field_coded);
    }
    if (h->ps_n == 128) {
        h->ps_start = (h->ps_start + 1) & 127;
        h->ps_n--;
    }
    h->ps_hist[(h->ps_start + h->ps_n++) & 127] = ps;
    if (ps >= 3 && !(h->said & 4)) {
        h->said |= 4;
        dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: HEVC: pictures with pic_struct %d (a frame shown as fields "
                "or repeated) [untested on real discs]\n", ps);
    }
    pic->fields = fields[ps];
    /* the rate: the VUI's timing, else the VPS's */
    if (sps->vui_present && sps->vui_timing_info_present) {
        num = sps->vui_num_units_in_tick;
        den = sps->vui_time_scale;
    } else if (sps->vps.present && sps->vps.timing_info_present) {
        num = sps->vps.num_units_in_tick;
        den = sps->vps.time_scale;
    }
    pic->rate_num = 0;
    pic->rate_den = 1;
    /* a tick is a picture period: in a stream coded as fields, a field's
     * (H.265 E.3.1), so a frame is two of them */
    if (num > 0 && den > 0)
        dr_reduce(&pic->rate_num, &pic->rate_den, den, (int64_t)num * (field_coded ? 2 : 1),
                  1 << 30);
    return 0;

fail:
    dr_vlog(logctx, DR_VLOG_ERROR, "Title timeline: HEVC: a picture's headers cannot be read\n");
    return DR_ERR_INVALID;
}
