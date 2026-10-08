/*
 * HEVC Annex B format parser
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 *
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

#include <string.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"

#include "libavcodec/golomb.h"
#include "libavcodec/parser_internal.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/parser.h"

#include "hevc.h"
#include "parse.h"
#include "ps.h"
#include "sei.h"

#define START_CODE 0x000001 ///< start_code_prefix_one_3bytes

#define IS_IRAP_NAL(nal) (nal->type >= 16 && nal->type <= 23)
#define IS_IDR_NAL(nal) (nal->type == HEVC_NAL_IDR_W_RADL || nal->type == HEVC_NAL_IDR_N_LP)

typedef struct HEVCParserContext {
    ParseContext pc;

    H2645Packet pkt;
    HEVCParamSets ps;
    HEVCSEI sei;

    int is_avc;
    int nal_length_size;
    int parsed_extradata;

    int poc;
    int pocTid0;

    /* disc mode (PARSER_FLAG_DISC) */
    FFDiscCut disc;
    int disc_said;                          ///< the untested notes already logged (bits)
} HEVCParserContext;

static int hevc_parse_slice_header(AVCodecParserContext *s, H2645NAL *nal,
                                   AVCodecContext *avctx)
{
    HEVCParserContext *ctx = s->priv_data;
    HEVCParamSets *ps = &ctx->ps;
    HEVCSEI *sei = &ctx->sei;
    GetBitContext *gb = &nal->gb;
    const HEVCPPS *pps;
    const HEVCSPS *sps;
    const HEVCWindow *ow;
    int i, num = 0, den = 0;

    unsigned int pps_id, first_slice_in_pic_flag, dependent_slice_segment_flag;
    enum HEVCSliceType slice_type;

    first_slice_in_pic_flag = get_bits1(gb);
    s->picture_structure = sei->picture_timing.picture_struct;
    s->field_order = sei->picture_timing.picture_struct;

    if (IS_IRAP_NAL(nal)) {
        s->key_frame = 1;
        skip_bits1(gb); // no_output_of_prior_pics_flag
    }

    pps_id = get_ue_golomb(gb);
    if (pps_id >= HEVC_MAX_PPS_COUNT || !ps->pps_list[pps_id]) {
        av_log(avctx, AV_LOG_ERROR, "PPS id out of range: %d\n", pps_id);
        return AVERROR_INVALIDDATA;
    }
    pps = ps->pps_list[pps_id];
    sps = pps->sps;

    ow  = &sps->output_window;

    s->coded_width  = sps->width;
    s->coded_height = sps->height;
    s->width        = sps->width  - ow->left_offset - ow->right_offset;
    s->height       = sps->height - ow->top_offset  - ow->bottom_offset;
    s->format       = sps->pix_fmt;
    avctx->profile  = sps->ptl.general_ptl.profile_idc;
    avctx->level    = sps->ptl.general_ptl.level_idc;

    if (sps->vps->vps_timing_info_present_flag) {
        num = sps->vps->vps_num_units_in_tick;
        den = sps->vps->vps_time_scale;
    } else if (sps->vui.vui_timing_info_present_flag) {
        num = sps->vui.vui_num_units_in_tick;
        den = sps->vui.vui_time_scale;
    }

    if (num > 0 && den > 0)
        av_reduce(&avctx->framerate.den, &avctx->framerate.num,
                  num, den, 1 << 30);

    if (!first_slice_in_pic_flag) {
        unsigned int slice_segment_addr;
        int slice_address_length;

        if (pps->dependent_slice_segments_enabled_flag)
            dependent_slice_segment_flag = get_bits1(gb);
        else
            dependent_slice_segment_flag = 0;

        slice_address_length = av_ceil_log2_c(sps->ctb_width *
                                              sps->ctb_height);
        slice_segment_addr = get_bitsz(gb, slice_address_length);
        if (slice_segment_addr >= sps->ctb_width * sps->ctb_height) {
            av_log(avctx, AV_LOG_ERROR, "Invalid slice segment address: %u.\n",
                   slice_segment_addr);
            return AVERROR_INVALIDDATA;
        }
    } else
        dependent_slice_segment_flag = 0;

    if (dependent_slice_segment_flag)
        return 0; /* break; */

    for (i = 0; i < pps->num_extra_slice_header_bits; i++)
        skip_bits(gb, 1); // slice_reserved_undetermined_flag[]

    slice_type = get_ue_golomb_31(gb);
    if (!(slice_type == HEVC_SLICE_I || slice_type == HEVC_SLICE_P ||
          slice_type == HEVC_SLICE_B)) {
        av_log(avctx, AV_LOG_ERROR, "Unknown slice type: %d.\n",
               slice_type);
        return AVERROR_INVALIDDATA;
    }
    s->pict_type = slice_type == HEVC_SLICE_B ? AV_PICTURE_TYPE_B :
                   slice_type == HEVC_SLICE_P ? AV_PICTURE_TYPE_P :
                                                AV_PICTURE_TYPE_I;

    if (pps->output_flag_present_flag)
        skip_bits1(gb); // pic_output_flag

    if (sps->separate_colour_plane)
        skip_bits(gb, 2);   // colour_plane_id

    if (!IS_IDR_NAL(nal)) {
        int pic_order_cnt_lsb = get_bits(gb, sps->log2_max_poc_lsb);
        s->output_picture_number = ctx->poc =
            ff_hevc_compute_poc(sps, ctx->pocTid0, pic_order_cnt_lsb, nal->type);
    } else
        s->output_picture_number = ctx->poc = 0;

    if (nal->temporal_id == 0 &&
        nal->type != HEVC_NAL_TRAIL_N &&
        nal->type != HEVC_NAL_TSA_N &&
        nal->type != HEVC_NAL_STSA_N &&
        nal->type != HEVC_NAL_RADL_N &&
        nal->type != HEVC_NAL_RASL_N &&
        nal->type != HEVC_NAL_RADL_R &&
        nal->type != HEVC_NAL_RASL_R)
        ctx->pocTid0 = ctx->poc;

    return 1; /* no need to evaluate the rest */
}

/**
 * Parse NAL units of found picture and decode some basic information.
 *
 * @param s parser context.
 * @param avctx codec context.
 * @param buf buffer with field/frame data.
 * @param buf_size size of the buffer.
 */
static int parse_nal_units(AVCodecParserContext *s, const uint8_t *buf,
                           int buf_size, AVCodecContext *avctx)
{
    HEVCParserContext *ctx = s->priv_data;
    HEVCParamSets *ps = &ctx->ps;
    HEVCSEI *sei = &ctx->sei;
    int flags = (H2645_FLAG_IS_NALFF * !!ctx->is_avc) | H2645_FLAG_SMALL_PADDING;
    int ret, i;

    /* set some sane default values */
    s->pict_type         = AV_PICTURE_TYPE_I;
    s->key_frame         = 0;
    s->picture_structure = AV_PICTURE_STRUCTURE_UNKNOWN;

    ff_hevc_reset_sei(sei);

    ret = ff_h2645_packet_split(&ctx->pkt, buf, buf_size, avctx,
                                ctx->nal_length_size, AV_CODEC_ID_HEVC, flags);
    if (ret < 0)
        return ret;

    for (i = 0; i < ctx->pkt.nb_nals; i++) {
        H2645NAL *nal = &ctx->pkt.nals[i];
        GetBitContext *gb = &nal->gb;

        if (nal->nuh_layer_id > 0)
            continue;

        switch (nal->type) {
        case HEVC_NAL_VPS:
            ff_hevc_decode_nal_vps(gb, avctx, ps);
            break;
        case HEVC_NAL_SPS:
            ff_hevc_decode_nal_sps(gb, avctx, ps, nal->nuh_layer_id, 1);
            break;
        case HEVC_NAL_PPS:
            ff_hevc_decode_nal_pps(gb, avctx, ps);
            break;
        case HEVC_NAL_SEI_PREFIX:
        case HEVC_NAL_SEI_SUFFIX:
            ff_hevc_decode_nal_sei(gb, avctx, sei, ps, nal->type);
            break;
        case HEVC_NAL_TRAIL_N:
        case HEVC_NAL_TRAIL_R:
        case HEVC_NAL_TSA_N:
        case HEVC_NAL_TSA_R:
        case HEVC_NAL_STSA_N:
        case HEVC_NAL_STSA_R:
        case HEVC_NAL_BLA_W_LP:
        case HEVC_NAL_BLA_W_RADL:
        case HEVC_NAL_BLA_N_LP:
        case HEVC_NAL_IDR_W_RADL:
        case HEVC_NAL_IDR_N_LP:
        case HEVC_NAL_CRA_NUT:
        case HEVC_NAL_RADL_N:
        case HEVC_NAL_RADL_R:
        case HEVC_NAL_RASL_N:
        case HEVC_NAL_RASL_R:
            if (ctx->sei.picture_timing.picture_struct == HEVC_SEI_PIC_STRUCT_FRAME_DOUBLING) {
                s->repeat_pict = 1;
            } else if (ctx->sei.picture_timing.picture_struct == HEVC_SEI_PIC_STRUCT_FRAME_TRIPLING) {
                s->repeat_pict = 2;
            }
            ret = hevc_parse_slice_header(s, nal, avctx);
            if (ret)
                return ret;
            break;
        }
    }
    /* didn't find a picture! */
    av_log(avctx, AV_LOG_ERROR, "missing picture in access unit with size %d\n", buf_size);
    return -1;
}

/**
 * Find the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or END_NOT_FOUND
 */
static int hevc_find_frame_end(AVCodecParserContext *s, const uint8_t *buf,
                               int buf_size)
{
    HEVCParserContext *ctx = s->priv_data;
    ParseContext       *pc = &ctx->pc;
    int i;

    for (i = 0; i < buf_size; i++) {
        int nut, layer_id;

        pc->state64 = (pc->state64 << 8) | buf[i];

        if (((pc->state64 >> 3 * 8) & 0xFFFFFF) != START_CODE)
            continue;

        nut = (pc->state64 >> 2 * 8 + 1) & 0x3F;

        layer_id = (pc->state64 >> 11) & 0x3F;
        if (layer_id > 0)
            continue;

        // Beginning of access unit
        if ((nut >= HEVC_NAL_VPS && nut <= HEVC_NAL_EOB_NUT) || nut == HEVC_NAL_SEI_PREFIX ||
            (nut >= 41 && nut <= 44) || (nut >= 48 && nut <= 55)) {
            if (pc->frame_start_found) {
                pc->frame_start_found = 0;
                if (!((pc->state64 >> 6 * 8) & 0xFF))
                    return i - 6;
                return i - 5;
            }
        } else if (nut <= HEVC_NAL_RASL_R ||
                   (nut >= HEVC_NAL_BLA_W_LP && nut <= HEVC_NAL_CRA_NUT)) {
            int first_slice_segment_in_pic_flag = buf[i] >> 7;
            if (first_slice_segment_in_pic_flag) {
                if (!pc->frame_start_found) {
                    pc->frame_start_found = 1;
                } else { // First slice of next frame found
                    pc->frame_start_found = 0;
                    if (!((pc->state64 >> 6 * 8) & 0xFF))
                        return i - 6;
                    return i - 5;
                }
            }
        }
    }

    return END_NOT_FOUND;
}

/* ---- Disc mode (PARSER_FLAG_DISC): the disc reader's access-unit rule ----
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
 * follows. SPS, slice segments and delimiters end the search.
 * A picture whose first slice segment does not start a picture, or that has
 * no parameter sets yet, is left out with the units in front of it (no
 * frame).
 *
 * Each frame then gives its picture type (its first slice segment),
 * key_frame (an IRAP picture, or a recovery point SEI in front of it),
 * output_picture_number (the display order: the picture order count of
 * H.265 8.3.1) and disc_fields from its pic_struct: 0 -> 2, 3 / 4 -> 2,
 * 5 / 6 -> 3, 7 -> 4, 8 -> 6 field periods (H.265 Table E.7). pic_struct is
 * the picture timing SEI's (in front of the picture) when
 * frame_field_info_present_flag is set (without VUI: inferred from the
 * profile's source flags, E.3.1), else 0; a missing SEI: 0 (logged). Ultra
 * HD Blu-ray video is progressive: a stream of field pictures
 * (field_seq_flag, pic_struct 1, 2, 9-12) is not a Blu-ray stream and
 * cannot be cut.
 */

enum { DISC_PREFIX, DISC_SLICES, DISC_SUFFIX };

#define DISC_SUFFIX_MAX 100

typedef struct HEVCDiscNal {
    int zr;                 ///< first byte of the zero run in front of its start code
    int sc;                 ///< its start code 00 00 01
    int type;               ///< nal_unit_type
    int layer;              ///< nuh_layer_id
} HEVCDiscNal;

typedef struct HEVCDiscSlice {
    int first, pps_id;
} HEVCDiscSlice;

/** Where the cut of the access unit at the start of the kept bytes stands. */
typedef struct HEVCDiscCut {
    HEVCParserContext *p;
    AVCodecContext *avctx;
    HEVCDiscNal *nal;       ///< the NAL units found so far
    int nb_nal, nal_cap;
    int scan;               ///< next offset to look for a start code at
    int done;               ///< NAL units decided
    int phase;
    HEVCDiscSlice first;    ///< the picture's first slice segment
    int suffix[DISC_SUFFIX_MAX + 1];    ///< the units the trailing-unit search went over
    int nb_suffix, suffix_k;
    int no_unit;            ///< the chunk given out is no frame
    int64_t pictures;       ///< pictures cut so far (where a warning points)
} HEVCDiscCut;

static int disc_vcl(int t)
{
    return t <= HEVC_NAL_RASL_R || (t >= HEVC_NAL_BLA_W_LP && t <= HEVC_NAL_CRA_NUT);
}

/* Whether the unit (its type, layer, bytes after the start code) is one
 * the disc reader does not see. */
static int disc_hidden(int t, int layer, int size)
{
    return size < 2 || layer || (t >= 10 && t <= 15) || (t >= 22 && t <= 31) ||
           (t >= HEVC_NAL_EOS_NUT && t <= HEVC_NAL_FD_NUT) || (t >= 41 && t <= 61);
}

static int disc_push(HEVCDiscCut *st, int zr, int sc, const uint8_t *hdr)
{
    if (st->nb_nal == st->nal_cap) {
        int cap = st->nal_cap ? 2 * st->nal_cap : 64;
        HEVCDiscNal *n = av_realloc_array(st->nal, cap, sizeof(*n));
        if (!n)
            return AVERROR(ENOMEM);
        st->nal     = n;
        st->nal_cap = cap;
    }
    st->nal[st->nb_nal++] = (HEVCDiscNal){ zr, sc, (hdr[0] >> 1) & 0x3f, (hdr[0] & 1) << 5 | hdr[1] >> 3 };
    return 0;
}

/* A parameter set of the access unit into the parser's sets. */
static int disc_param_set(HEVCDiscCut *st, const uint8_t *nal, int size)
{
    HEVCParserContext *p = st->p;
    H2645RBSP rbsp = { NULL };
    H2645NAL n = { NULL };
    int t = (nal[0] >> 1) & 0x3f, ret;

    av_fast_padded_malloc(&rbsp.rbsp_buffer, &rbsp.rbsp_buffer_alloc_size, size);
    if (!rbsp.rbsp_buffer)
        return AVERROR(ENOMEM);
    ret = ff_h2645_extract_rbsp(nal, size, &rbsp, &n, 1);
    if (ret >= 0 && (ret = init_get_bits8(&n.gb, n.data, n.size)) >= 0) {
        skip_bits(&n.gb, 16);
        if (t == HEVC_NAL_VPS)
            ff_hevc_decode_nal_vps(&n.gb, st->avctx, &p->ps);
        else if (t == HEVC_NAL_SPS)
            ff_hevc_decode_nal_sps(&n.gb, st->avctx, &p->ps, 0, 1);
        else
            ff_hevc_decode_nal_pps(&n.gb, st->avctx, &p->ps);
    }
    av_freep(&rbsp.rbsp_buffer);
    return ret == AVERROR(ENOMEM) ? ret : 0;
}

/* A slice segment header as far as the picture comparison reads it:
 * first_slice_segment_in_pic_flag and slice_pic_parameter_set_id; 0 when cut
 * short. */
static int disc_slice(const uint8_t *nal, int size, HEVCDiscSlice *h)
{
    uint8_t tmp[16 + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    int t = (nal[0] >> 1) & 0x3f, n = 0, zeros = 0;
    GetBitContext gb;

    /* the first bytes after the NAL unit header, without emulation prevention */
    for (int i = 2; i < size && n < 16; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        tmp[n++] = nal[i];
    }
    if (!n || init_get_bits8(&gb, tmp, n) < 0)
        return 0;
    h->first = get_bits1(&gb);
    if (t >= HEVC_NAL_BLA_W_LP && t <= 23)
        skip_bits1(&gb);                    /* no_output_of_prior_pics_flag */
    h->pps_id = get_ue_golomb_long(&gb);
    return get_bits_left(&gb) >= 0;
}

/* The picture's parameter sets are there. */
static int disc_has_sets(const HEVCParserContext *p, int pps_id)
{
    const HEVCPPS *pps = pps_id < HEVC_MAX_PPS_COUNT ? p->ps.pps_list[pps_id] : NULL;
    return pps && pps->sps && pps->sps->vps;
}

/* The chunk of the kept bytes up to end is decided: the cut starts anew. */
static int disc_end(HEVCDiscCut *st, int end, int no_unit)
{
    st->nb_nal = st->scan = st->done = st->nb_suffix = st->suffix_k = 0;
    st->phase   = DISC_PREFIX;
    st->no_unit = no_unit;
    st->pictures += !no_unit;
    return end;
}

static int disc_unit_size(void *log, const uint8_t *buf, int avail, int final, void *state)
{
    HEVCDiscCut *st = state;
    int i, ret;

    if (!st->nb_nal) {                      /* the unit's own start code */
        int z = 0;
        while (z < avail && !buf[z])
            z++;
        if (z + 2 >= avail)
            return final ? DISC_CUT_NONE : 0;
        if (z < 2 || buf[z] != 1)
            return DISC_CUT_NONE;
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
        HEVCDiscSlice h;

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
                if (!disc_slice(nal, size, &h) || !h.first || !disc_has_sets(st->p, h.pps_id)) {
                    /* no decoder can decode it (H.265 7.4.2.4.2): damaged
                     * data, or a stream read from the middle */
                    av_log(log, AV_LOG_WARNING, "Title timeline: HEVC: a picture %s after picture %"PRId64
                           " of the segment: it cannot be decoded and is left out with the units in front of it (damaged data?) "
                           "[untested on real discs]\n", !h.first ? "that does not start with its first slice "
                           "segment" : "without its parameter sets", st->pictures);
                    return disc_end(st, end, 1);
                }
                st->first = h;
                st->phase = DISC_SLICES;
            } else if (t >= HEVC_NAL_VPS && t <= HEVC_NAL_PPS) {
                if (!complete)
                    return 0;
                if ((ret = disc_param_set(st, nal, size)) < 0)
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
                ok = disc_slice(nal, size, &h);
                if (ok && h.first)
                    return disc_end(st, st->nal[j].zr, 0);
                if (!ok || h.pps_id != st->first.pps_id)
                    av_log(log, AV_LOG_WARNING, "Title timeline: HEVC: a slice segment %s in picture %"PRId64
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
            int suffix = t == HEVC_NAL_SEI_SUFFIX || t == 62 || t == 63;
            int stop   = t == HEVC_NAL_SPS || disc_vcl(t) || t == HEVC_NAL_AUD || t == 62;
            int e      = suffix ? st->nb_suffix + 1 : 0;

            st->suffix[st->nb_suffix++] = j;
            if (!e && st->suffix_k)
                return disc_end(st, st->nal[st->suffix[st->suffix_k]].zr, 0);
            st->suffix_k = e;
            if (stop || st->nb_suffix == DISC_SUFFIX_MAX) {
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

static const FFDiscCutRule disc_rule = { disc_unit_size, disc_resync, 0 };

/* The SEI messages of a prefix SEI unit (from its NAL unit header, size
 * bytes) in front of the picture: the picture timing's pic_struct (the
 * first 4 bits of its payload; *ps stays < 0 without one) and whether a
 * recovery point is there. */
static void disc_sei(const uint8_t *nal, int size, int *ps, int *recovery)
{
    uint8_t *b = av_malloc(size);
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
        if (type == SEI_TYPE_PIC_TIMING && len && *ps < 0)
            *ps = b[i] >> 4;
        if (type == SEI_TYPE_RECOVERY_POINT && len)
            *recovery = 1;
        i += len;
    }
end:
    av_free(b);
}

/* The values of the frame just cut. */
static int disc_picture(AVCodecParserContext *s, AVCodecContext *avctx, const uint8_t *buf, int size)
{
    static const uint8_t fields[9] = { 2, 1, 1, 2, 2, 3, 3, 4, 6 };
    HEVCParserContext *p = s->priv_data;
    const HEVCPPS *pps = NULL;
    const HEVCSPS *sps;
    int ps = -1, recovery = 0, num = 0, den = 0, ffi, pps_id = -1;

    /* the SEI in front of the picture; its first slice segment's PPS */
    for (int i = 0; i + 4 < size; i++) {
        int t, e;
        if (buf[i] || buf[i + 1] || buf[i + 2] != 1)
            continue;
        t = (buf[i + 3] >> 1) & 0x3f;
        if ((buf[i + 3] & 1) || buf[i + 4] >> 3) {
            i += 2;
            continue;
        }
        for (e = i + 3; e + 2 < size && (buf[e] || buf[e + 1] || buf[e + 2] > 1); e++)
            ;
        if (e + 2 >= size)
            e = size;
        if (t == HEVC_NAL_SEI_PREFIX)
            disc_sei(buf + i + 3, e - i - 3, &ps, &recovery);
        if (disc_vcl(t)) {
            HEVCDiscSlice h;
            if (disc_slice(buf + i + 3, e - i - 3, &h) && h.pps_id < HEVC_MAX_PPS_COUNT)
                pps_id = h.pps_id;
            break;
        }
        i = e - 1;
    }
    /* the PPS looked up after FFmpeg's parse, which reads the parameter sets
     * of the access unit again and replaces the stored ones */
    if (parse_nal_units(s, buf, size, avctx) < 0 || pps_id < 0 || !(pps = p->ps.pps_list[pps_id]) ||
        !(sps = pps->sps)) {
        av_log(avctx, AV_LOG_ERROR, "Title timeline: HEVC: a picture's headers cannot be read\n");
        return AVERROR_INVALIDDATA;
    }
    s->key_frame = s->key_frame > 0 || recovery;
    /* Ultra HD Blu-ray video is progressive: a stream of field pictures is
     * none of it (and is not read) */
    if (sps->vui_present && sps->vui.field_seq_flag) {
        av_log(avctx, AV_LOG_ERROR, "Title timeline: HEVC: a stream of field pictures (field_seq_flag): not a "
               "Blu-ray HEVC stream (Ultra HD Blu-ray video is progressive)\n");
        return AVERROR_INVALIDDATA;
    }
    if (!sps->vui_present && !(p->disc_said & 1)) {
        p->disc_said |= 1;
        av_log(avctx, AV_LOG_WARNING, "Title timeline: HEVC: a sequence without VUI (a Blu-ray stream states its "
               "colours and timing there): its pictures taken as frames [untested on real discs]\n");
    }
    /* frame_field_info_present_flag; without VUI inferred from the profile's
     * source flags (H.265 E.3.1) */
    ffi = sps->vui_present ? sps->vui.frame_field_info_present_flag :
          sps->ptl.general_ptl.progressive_source_flag && sps->ptl.general_ptl.interlaced_source_flag;
    if (!ffi) {
        ps = 0;
    } else if (ps < 0) {
        if (!(p->disc_said & 2)) {
            p->disc_said |= 2;
            av_log(avctx, AV_LOG_WARNING, "Title timeline: HEVC: a picture without the picture timing SEI its "
                   "frame_field_info_present_flag asks for (a stream fault, H.265 E.3.1): taken as a frame "
                   "[untested on real discs]\n");
        }
        ps = 0;
    }
    if (ps == 1 || ps == 2 || ps > 8) {
        av_log(avctx, AV_LOG_ERROR, "Title timeline: HEVC: pic_struct %d (%s): not a Blu-ray HEVC stream (Ultra HD "
               "Blu-ray video is progressive)\n", ps, ps <= 12 ? "a field picture" : "a reserved value");
        return AVERROR_INVALIDDATA;
    }
    if (ps && !(p->disc_said & 4)) {
        p->disc_said |= 4;
        av_log(avctx, AV_LOG_WARNING, "Title timeline: HEVC: pictures with pic_struct %d (a frame shown as fields "
               "or repeated) [untested on real discs]\n", ps);
    }
    s->disc_fields = fields[ps];
    /* the rate: the VUI's timing, else the VPS's */
    if (sps->vui_present && sps->vui.vui_timing_info_present_flag) {
        num = sps->vui.vui_num_units_in_tick;
        den = sps->vui.vui_time_scale;
    } else if (sps->vps && sps->vps->vps_timing_info_present_flag) {
        num = sps->vps->vps_num_units_in_tick;
        den = sps->vps->vps_time_scale;
    }
    avctx->framerate = (AVRational){ 0, 1 };
    if (num > 0 && den > 0)
        av_reduce(&avctx->framerate.num, &avctx->framerate.den, den, num, 1 << 30);
    return 0;
}

static int disc_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size, const uint8_t *buf, int buf_size)
{
    HEVCParserContext *p = s->priv_data;
    HEVCDiscCut *st;
    int ret, r;

    if (!p->disc.state && !(p->disc.state = av_mallocz(sizeof(HEVCDiscCut)))) {
        s->disc_status = AVERROR(ENOMEM);
        *poutbuf       = NULL;
        *poutbuf_size  = 0;
        return 0;
    }
    st          = p->disc.state;
    st->p       = p;
    st->avctx   = avctx;
    st->no_unit = 0;
    s->disc_fields = 0;
    ret = ff_parser_disc_cut(s, avctx, &disc_rule, &p->disc, poutbuf, poutbuf_size, buf, buf_size);
    if (*poutbuf_size && !s->disc_status) {
        if (st->no_unit)
            s->disc_status = 1;
        else if ((r = disc_picture(s, avctx, *poutbuf, *poutbuf_size)) < 0)
            s->disc_status = r;
    }
    return ret;
}

static int hevc_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size,
                      const uint8_t *buf, int buf_size)
{
    int next;
    HEVCParserContext *ctx = s->priv_data;
    ParseContext *pc = &ctx->pc;
    int is_dummy_buf = !buf_size;
    const uint8_t *dummy_buf = buf;

    if (s->flags & PARSER_FLAG_DISC)
        return disc_parse(s, avctx, poutbuf, poutbuf_size, buf, buf_size);

    if (avctx->extradata && !ctx->parsed_extradata) {
        ff_hevc_decode_extradata(avctx->extradata, avctx->extradata_size, &ctx->ps, &ctx->sei,
                                 &ctx->is_avc, &ctx->nal_length_size, avctx->err_recognition,
                                 1, avctx);
        ctx->parsed_extradata = 1;
    }

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        next = hevc_find_frame_end(s, buf, buf_size);
        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }
    }

    is_dummy_buf &= (dummy_buf == buf);

    if (!is_dummy_buf)
        parse_nal_units(s, buf, buf_size, avctx);

    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return next;
}

static void hevc_parser_close(AVCodecParserContext *s)
{
    HEVCParserContext *ctx = s->priv_data;

    ff_hevc_ps_uninit(&ctx->ps);
    ff_h2645_packet_uninit(&ctx->pkt);
    ff_hevc_reset_sei(&ctx->sei);

    av_freep(&ctx->pc.buffer);
    if (ctx->disc.state)
        av_freep(&((HEVCDiscCut *)ctx->disc.state)->nal);
    ff_parser_disc_cut_close(&ctx->disc);
}

const FFCodecParser ff_hevc_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_HEVC),
    .priv_data_size = sizeof(HEVCParserContext),
    .parse          = hevc_parse,
    .close          = hevc_parser_close,
};
