/*
 * H.26L/H.264/AVC/JVT/14496-10/... parser
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
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

/**
 * @file
 * H.264 / AVC / MPEG-4 part10 parser.
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#define UNCHECKED_BITSTREAM_READER 1

#include <stdint.h>
#include <string.h>

#include "libavutil/attributes.h"
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixfmt.h"

#include "avcodec.h"
#include "get_bits.h"
#include "golomb.h"
#include "h264.h"
#include "h264dsp.h"
#include "h264_parse.h"
#include "h264_sei.h"
#include "h264_ps.h"
#include "h2645_parse.h"
#include "h264data.h"
#include "mpegutils.h"
#include "parser.h"
#include "libavutil/refstruct.h"
#include "parser_internal.h"
#include "startcode.h"

typedef struct H264ParseContext {
    ParseContext pc;
    H264ParamSets ps;
    H264DSPContext h264dsp;
    H264POCContext poc;
    H264SEIContext sei;
    int is_avc;
    int nal_length_size;
    int got_first;
    int picture_structure;
    uint8_t parse_history[6];
    int parse_history_count;
    int parse_last_mb;
    int64_t reference_dts;
    int last_frame_num, last_picture_structure;
    /* disc mode (PARSER_FLAG_DISC) */
    FFDiscCut disc;
    uint8_t disc_ps[128];                   ///< pic_struct history (the latest 128 pictures)
    int disc_ps_n, disc_ps_start;
    int disc_reset;                         ///< the picture just parsed has memory_management_control_operation 5
    int disc_order_base, disc_order_max;    ///< display order: added to the POC since the last reset / the highest
    int64_t disc_resets;                    ///< pictures with memory_management_control_operation 5 so far
} H264ParseContext;

static int find_start_code(const uint8_t *buf, int buf_size,
                                  int buf_index, int next_avc)
{
    uint32_t state = -1;

    buf_index = avpriv_find_start_code(buf + buf_index, buf + next_avc + 1, &state) - buf - 1;

    return FFMIN(buf_index, buf_size);
}

static int h264_find_frame_end(H264ParseContext *p, const uint8_t *buf,
                               int buf_size, void *logctx)
{
    int i, j;
    uint32_t state;
    ParseContext *pc = &p->pc;

    int next_avc = p->is_avc ? 0 : buf_size;
//    mb_addr= pc->mb_addr - 1;
    state = pc->state;
    if (state > 13)
        state = 7;

    if (p->is_avc && !p->nal_length_size)
        av_log(logctx, AV_LOG_ERROR, "AVC-parser: nal length size invalid\n");

    for (i = 0; i < buf_size; i++) {
        if (i >= next_avc) {
            int64_t nalsize = 0;
            i = next_avc;
            for (j = 0; j < p->nal_length_size; j++)
                nalsize = (nalsize << 8) | buf[i++];
            if (!nalsize || nalsize > buf_size - i) {
                av_log(logctx, AV_LOG_ERROR, "AVC-parser: nal size %"PRId64" "
                       "remaining %d\n", nalsize, buf_size - i);
                return buf_size;
            }
            next_avc = i + nalsize;
            state    = 5;
        }

        if (state == 7) {
            i += p->h264dsp.startcode_find_candidate(buf + i, next_avc - i);
            if (i < next_avc)
                state = 2;
        } else if (state <= 2) {
            if (buf[i] == 1)
                state ^= 5;            // 2->7, 1->4, 0->5
            else if (buf[i])
                state = 7;
            else
                state >>= 1;           // 2->1, 1->0, 0->0
        } else if (state <= 5) {
            int nalu_type = buf[i] & 0x1F;
            if (nalu_type == H264_NAL_SEI || nalu_type == H264_NAL_SPS ||
                nalu_type == H264_NAL_PPS || nalu_type == H264_NAL_AUD) {
                if (pc->frame_start_found) {
                    i++;
                    goto found;
                }
            } else if (nalu_type == H264_NAL_SLICE || nalu_type == H264_NAL_DPA ||
                       nalu_type == H264_NAL_IDR_SLICE) {
                state += 8;
                continue;
            }
            state = 7;
        } else {
            unsigned int mb, last_mb = p->parse_last_mb;
            GetBitContext gb;
            p->parse_history[p->parse_history_count++] = buf[i];

            init_get_bits(&gb, p->parse_history, 8*p->parse_history_count);
            mb= get_ue_golomb_long(&gb);
            if (get_bits_left(&gb) > 0 || p->parse_history_count > 5) {
                p->parse_last_mb = mb;
                if (pc->frame_start_found) {
                    if (mb <= last_mb) {
                        i -= p->parse_history_count - 1;
                        p->parse_history_count = 0;
                        goto found;
                    }
                } else
                    pc->frame_start_found = 1;
                p->parse_history_count = 0;
                state = 7;
            }
        }
    }
    pc->state = state;
    if (p->is_avc)
        return next_avc;
    return END_NOT_FOUND;

found:
    pc->state             = 7;
    pc->frame_start_found = 0;
    if (p->is_avc)
        return next_avc;
    return i - (state & 5);
}

static int scan_mmco_reset(AVCodecParserContext *s, GetBitContext *gb,
                           void *logctx)
{
    H264PredWeightTable pwt;
    int slice_type_nos = s->pict_type & 3;
    H264ParseContext *p = s->priv_data;
    int list_count, ref_count[2];


    if (p->ps.pps->redundant_pic_cnt_present)
        get_ue_golomb(gb); // redundant_pic_count

    if (slice_type_nos == AV_PICTURE_TYPE_B)
        get_bits1(gb); // direct_spatial_mv_pred

    if (ff_h264_parse_ref_count(&list_count, ref_count, gb, p->ps.pps,
                                slice_type_nos, p->picture_structure, logctx) < 0)
        return AVERROR_INVALIDDATA;

    if (slice_type_nos != AV_PICTURE_TYPE_I) {
        int list;
        for (list = 0; list < list_count; list++) {
            if (get_bits1(gb)) {
                int index;
                for (index = 0; ; index++) {
                    unsigned int reordering_of_pic_nums_idc = get_ue_golomb_31(gb);

                    if (reordering_of_pic_nums_idc < 3)
                        get_ue_golomb_long(gb);
                    else if (reordering_of_pic_nums_idc > 3) {
                        av_log(logctx, AV_LOG_ERROR,
                               "illegal reordering_of_pic_nums_idc %d\n",
                               reordering_of_pic_nums_idc);
                        return AVERROR_INVALIDDATA;
                    } else
                        break;

                    if (index >= ref_count[list]) {
                        av_log(logctx, AV_LOG_ERROR,
                               "reference count %d overflow\n", index);
                        return AVERROR_INVALIDDATA;
                    }
                }
            }
        }
    }

    if ((p->ps.pps->weighted_pred && slice_type_nos == AV_PICTURE_TYPE_P) ||
        (p->ps.pps->weighted_bipred_idc == 1 && slice_type_nos == AV_PICTURE_TYPE_B))
        ff_h264_pred_weight_table(gb, p->ps.sps, ref_count, slice_type_nos,
                                  &pwt, p->picture_structure, logctx);

    if (get_bits1(gb)) { // adaptive_ref_pic_marking_mode_flag
        int i;
        for (i = 0; i < H264_MAX_MMCO_COUNT; i++) {
            if (get_bits_left(gb) < 1)
                return AVERROR_INVALIDDATA;

            MMCOOpcode opcode = get_ue_golomb_31(gb);
            if (opcode > (unsigned) MMCO_LONG) {
                av_log(logctx, AV_LOG_ERROR,
                       "illegal memory management control operation %d\n",
                       opcode);
                return AVERROR_INVALIDDATA;
            }
            if (opcode == MMCO_END)
               return 0;
            else if (opcode == MMCO_RESET)
                return 1;

            if (opcode == MMCO_SHORT2UNUSED || opcode == MMCO_SHORT2LONG)
                get_ue_golomb_long(gb); // difference_of_pic_nums_minus1
            if (opcode == MMCO_SHORT2LONG || opcode == MMCO_LONG2UNUSED ||
                opcode == MMCO_LONG || opcode == MMCO_SET_MAX_LONG)
                get_ue_golomb_31(gb);
        }
    }

    return 0;
}

/**
 * Parse NAL units of found picture and decode some basic information.
 *
 * @param s parser context.
 * @param avctx codec context.
 * @param buf buffer with field/frame data.
 * @param buf_size size of the buffer.
 */
static inline int parse_nal_units(AVCodecParserContext *s,
                                  AVCodecContext *avctx,
                                  const uint8_t * const buf, int buf_size)
{
    H264ParseContext *p = s->priv_data;
    H2645RBSP rbsp = { NULL };
    H2645NAL nal = { NULL };
    int buf_index, next_avc;
    unsigned int pps_id;
    unsigned int slice_type;
    int state = -1, got_reset = 0;
    int q264 = buf_size >=4 && !memcmp("Q264", buf, 4);
    int field_poc[2];
    int ret;

    /* set some sane default values */
    s->pict_type         = AV_PICTURE_TYPE_I;
    s->key_frame         = 0;
    s->picture_structure = AV_PICTURE_STRUCTURE_UNKNOWN;

    ff_h264_sei_uninit(&p->sei);
    p->sei.common.frame_packing.arrangement_cancel_flag = -1;
    p->sei.common.unregistered.x264_build = -1;

    if (!buf_size)
        return 0;

    av_fast_padded_malloc(&rbsp.rbsp_buffer, &rbsp.rbsp_buffer_alloc_size, buf_size);
    if (!rbsp.rbsp_buffer)
        return AVERROR(ENOMEM);

    buf_index     = 0;
    next_avc      = p->is_avc ? 0 : buf_size;
    for (;;) {
        const SPS *sps;
        int src_length, consumed, nalsize = 0;

        if (buf_index >= next_avc) {
            nalsize = get_nalsize(p->nal_length_size, buf, buf_size, &buf_index, avctx);
            if (nalsize < 0)
                break;
            next_avc = buf_index + nalsize;
        } else {
            buf_index = find_start_code(buf, buf_size, buf_index, next_avc);
            if (buf_index >= buf_size)
                break;
            if (buf_index >= next_avc)
                continue;
        }
        src_length = next_avc - buf_index;

        state = buf[buf_index];
        switch (state & 0x1f) {
        case H264_NAL_SLICE:
        case H264_NAL_IDR_SLICE:
            // Do not walk the whole buffer just to decode slice header
            if ((state & 0x1f) == H264_NAL_IDR_SLICE || ((state >> 5) & 0x3) == 0) {
                /* IDR or disposable slice
                 * No need to decode many bytes because MMCOs shall not be present. */
                if (src_length > 60)
                    src_length = 60;
            } else {
                /* To decode up to MMCOs */
                if (src_length > 1000)
                    src_length = 1000;
            }
            break;
        }
        consumed = ff_h2645_extract_rbsp(buf + buf_index, src_length, &rbsp, &nal, 1);
        if (consumed < 0)
            break;

        buf_index += consumed;

        ret = init_get_bits8(&nal.gb, nal.data, nal.size);
        if (ret < 0)
            goto fail;
        get_bits1(&nal.gb);
        nal.ref_idc = get_bits(&nal.gb, 2);
        nal.type    = get_bits(&nal.gb, 5);

        switch (nal.type) {
        case H264_NAL_SPS:
            ff_h264_decode_seq_parameter_set(&nal.gb, avctx, &p->ps, 0);
            break;
        case H264_NAL_PPS:
            ff_h264_decode_picture_parameter_set(&nal.gb, avctx, &p->ps,
                                                 nal.size_bits);
            break;
        case H264_NAL_SEI:
            ff_h264_sei_decode(&p->sei, &nal.gb, &p->ps, avctx);
            break;
        case H264_NAL_IDR_SLICE:
            s->key_frame = 1;

            p->poc.prev_frame_num        = 0;
            p->poc.prev_frame_num_offset = 0;
            p->poc.prev_poc_msb          =
            p->poc.prev_poc_lsb          = 0;
            av_fallthrough;
        case H264_NAL_SLICE:
            get_ue_golomb_long(&nal.gb);  // skip first_mb_in_slice
            slice_type   = get_ue_golomb_31(&nal.gb);
            s->pict_type = ff_h264_golomb_to_pict_type[slice_type % 5];
            if (p->sei.recovery_point.recovery_frame_cnt >= 0) {
                /* key frame, since recovery_frame_cnt is set */
                s->key_frame = 1;
            }
            pps_id = get_ue_golomb(&nal.gb);
            if (pps_id >= MAX_PPS_COUNT) {
                av_log(avctx, AV_LOG_ERROR,
                       "pps_id %u out of range\n", pps_id);
                goto fail;
            }
            if (!p->ps.pps_list[pps_id]) {
                av_log(avctx, AV_LOG_ERROR,
                       "non-existing PPS %u referenced\n", pps_id);
                goto fail;
            }

            av_refstruct_replace(&p->ps.pps, p->ps.pps_list[pps_id]);
            p->ps.sps = p->ps.pps->sps;
            sps       = p->ps.sps;

            // heuristic to detect non marked keyframes
            if (p->ps.sps->ref_frame_count <= 1 && p->ps.pps->ref_count[0] <= 1 && s->pict_type == AV_PICTURE_TYPE_I)
                s->key_frame = 1;

            p->poc.frame_num = get_bits(&nal.gb, sps->log2_max_frame_num);

            s->coded_width  = 16 * sps->mb_width;
            s->coded_height = 16 * sps->mb_height;
            s->width        = s->coded_width  - (sps->crop_right + sps->crop_left);
            s->height       = s->coded_height - (sps->crop_top   + sps->crop_bottom);
            if (s->width <= 0 || s->height <= 0) {
                s->width  = s->coded_width;
                s->height = s->coded_height;
            }

            switch (sps->bit_depth_luma) {
            case 9:
                if (sps->chroma_format_idc == 3)      s->format = AV_PIX_FMT_YUV444P9;
                else if (sps->chroma_format_idc == 2) s->format = AV_PIX_FMT_YUV422P9;
                else                                  s->format = AV_PIX_FMT_YUV420P9;
                break;
            case 10:
                if (sps->chroma_format_idc == 3)      s->format = AV_PIX_FMT_YUV444P10;
                else if (sps->chroma_format_idc == 2) s->format = AV_PIX_FMT_YUV422P10;
                else                                  s->format = AV_PIX_FMT_YUV420P10;
                break;
            case 8:
                if (sps->chroma_format_idc == 3)      s->format = AV_PIX_FMT_YUV444P;
                else if (sps->chroma_format_idc == 2) s->format = AV_PIX_FMT_YUV422P;
                else                                  s->format = AV_PIX_FMT_YUV420P;
                break;
            default:
                s->format = AV_PIX_FMT_NONE;
            }

            avctx->profile = ff_h264_get_profile(sps);
            avctx->level   = sps->level_idc;

            if (sps->frame_mbs_only_flag) {
                p->picture_structure = PICT_FRAME;
            } else {
                if (get_bits1(&nal.gb)) { // field_pic_flag
                    p->picture_structure = PICT_TOP_FIELD + get_bits1(&nal.gb); // bottom_field_flag
                } else {
                    p->picture_structure = PICT_FRAME;
                }
            }

            if (nal.type == H264_NAL_IDR_SLICE)
                get_ue_golomb_long(&nal.gb); /* idr_pic_id */
            if (sps->poc_type == 0) {
                p->poc.poc_lsb = get_bits(&nal.gb, sps->log2_max_poc_lsb);

                if (p->ps.pps->pic_order_present == 1 &&
                    p->picture_structure == PICT_FRAME)
                    p->poc.delta_poc_bottom = get_se_golomb(&nal.gb);
            }

            if (sps->poc_type == 1 &&
                !sps->delta_pic_order_always_zero_flag) {
                p->poc.delta_poc[0] = get_se_golomb(&nal.gb);

                if (p->ps.pps->pic_order_present == 1 &&
                    p->picture_structure == PICT_FRAME)
                    p->poc.delta_poc[1] = get_se_golomb(&nal.gb);
            }

            /* Decode POC of this picture.
             * The prev_ values needed for decoding POC of the next picture are not set here. */
            field_poc[0] = field_poc[1] = INT_MAX;
            ret = ff_h264_init_poc(field_poc, &s->output_picture_number, sps,
                             &p->poc, p->picture_structure, nal.ref_idc);
            if (ret < 0)
                goto fail;

            /* Continue parsing to check if MMCO_RESET is present.
             * FIXME: MMCO_RESET could appear in non-first slice.
             *        Maybe, we should parse all undisposable non-IDR slice of this
             *        picture until encountering MMCO_RESET in a slice of it. */
            if (nal.ref_idc && nal.type != H264_NAL_IDR_SLICE) {
                got_reset = scan_mmco_reset(s, &nal.gb, avctx);
                if (got_reset < 0)
                    goto fail;
            }

            /* Set up the prev_ values for decoding POC of the next picture. */
            p->poc.prev_frame_num        = got_reset ? 0 : p->poc.frame_num;
            p->poc.prev_frame_num_offset = got_reset ? 0 : p->poc.frame_num_offset;
            if (nal.ref_idc != 0) {
                if (!got_reset) {
                    p->poc.prev_poc_msb = p->poc.poc_msb;
                    p->poc.prev_poc_lsb = p->poc.poc_lsb;
                } else {
                    p->poc.prev_poc_msb = 0;
                    p->poc.prev_poc_lsb =
                        p->picture_structure == PICT_BOTTOM_FIELD ? 0 : field_poc[0];
                    /* disc mode: H.264 8.2.1, after the picture its
                     * TopFieldOrderCnt is less tempPicOrderCnt (its own
                     * PicOrderCnt): the next picture counts from there */
                    if ((s->flags & PARSER_FLAG_DISC) && p->picture_structure != PICT_BOTTOM_FIELD)
                        p->poc.prev_poc_lsb = field_poc[0] - s->output_picture_number;
                }
            }
            p->disc_reset = got_reset > 0;

            if (p->sei.picture_timing.present) {
                ret = ff_h264_sei_process_picture_timing(&p->sei.picture_timing,
                                                         sps, avctx);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Error processing the picture timing SEI\n");
                    p->sei.picture_timing.present = 0;
                }
            }

            if (sps->pic_struct_present_flag && p->sei.picture_timing.present) {
                switch (p->sei.picture_timing.pic_struct) {
                case H264_SEI_PIC_STRUCT_TOP_FIELD:
                case H264_SEI_PIC_STRUCT_BOTTOM_FIELD:
                    s->repeat_pict = 0;
                    break;
                case H264_SEI_PIC_STRUCT_FRAME:
                case H264_SEI_PIC_STRUCT_TOP_BOTTOM:
                case H264_SEI_PIC_STRUCT_BOTTOM_TOP:
                    s->repeat_pict = 1;
                    break;
                case H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP:
                case H264_SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM:
                    s->repeat_pict = 2;
                    break;
                case H264_SEI_PIC_STRUCT_FRAME_DOUBLING:
                    s->repeat_pict = 3;
                    break;
                case H264_SEI_PIC_STRUCT_FRAME_TRIPLING:
                    s->repeat_pict = 5;
                    break;
                default:
                    s->repeat_pict = p->picture_structure == PICT_FRAME ? 1 : 0;
                    break;
                }
            } else {
                s->repeat_pict = p->picture_structure == PICT_FRAME ? 1 : 0;
            }

            if (p->picture_structure == PICT_FRAME) {
                s->picture_structure = AV_PICTURE_STRUCTURE_FRAME;
                if (sps->pic_struct_present_flag && p->sei.picture_timing.present) {
                    switch (p->sei.picture_timing.pic_struct) {
                    case H264_SEI_PIC_STRUCT_TOP_BOTTOM:
                    case H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP:
                        s->field_order = AV_FIELD_TT;
                        break;
                    case H264_SEI_PIC_STRUCT_BOTTOM_TOP:
                    case H264_SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM:
                        s->field_order = AV_FIELD_BB;
                        break;
                    default:
                        s->field_order = AV_FIELD_PROGRESSIVE;
                        break;
                    }
                } else {
                    if (field_poc[0] < field_poc[1])
                        s->field_order = AV_FIELD_TT;
                    else if (field_poc[0] > field_poc[1])
                        s->field_order = AV_FIELD_BB;
                    else
                        s->field_order = AV_FIELD_PROGRESSIVE;
                }
            } else {
                if (p->picture_structure == PICT_TOP_FIELD)
                    s->picture_structure = AV_PICTURE_STRUCTURE_TOP_FIELD;
                else
                    s->picture_structure = AV_PICTURE_STRUCTURE_BOTTOM_FIELD;
                if (p->poc.frame_num == p->last_frame_num &&
                    p->last_picture_structure != AV_PICTURE_STRUCTURE_UNKNOWN &&
                    p->last_picture_structure != AV_PICTURE_STRUCTURE_FRAME &&
                    p->last_picture_structure != s->picture_structure) {
                    if (p->last_picture_structure == AV_PICTURE_STRUCTURE_TOP_FIELD)
                        s->field_order = AV_FIELD_TT;
                    else
                        s->field_order = AV_FIELD_BB;
                } else {
                    s->field_order = AV_FIELD_UNKNOWN;
                }
                p->last_picture_structure = s->picture_structure;
                p->last_frame_num = p->poc.frame_num;
            }
            if (sps->timing_info_present_flag) {
                int64_t den = sps->time_scale;
                if (p->sei.common.unregistered.x264_build < 44U)
                    den *= 2;
                av_reduce(&avctx->framerate.den, &avctx->framerate.num,
                          sps->num_units_in_tick * 2, den, 1 << 30);
            }

            av_freep(&rbsp.rbsp_buffer);
            return 0; /* no need to evaluate the rest */
        }
    }
    if (q264) {
        av_freep(&rbsp.rbsp_buffer);
        return 0;
    }
    /* didn't find a picture! */
    av_log(avctx, AV_LOG_ERROR, "missing picture in access unit with size %d\n", buf_size);
fail:
    av_freep(&rbsp.rbsp_buffer);
    return -1;
}

/* ---- Disc mode (PARSER_FLAG_DISC): the disc reader's access-unit rule ----
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
 * A data partition where a picture starts: the stream cannot be cut.
 *
 * Each frame then gives its picture type (its first slice), key_frame (an
 * IDR picture, or a recovery point SEI in front of it), output_picture_number
 * (the display order: the picture order count of H.264 8.2.1; a picture
 * with memory_management_control_operation 5 resets it for the pictures
 * after it (its own TopFieldOrderCnt less its PicOrderCnt), and as every
 * picture before it is shown before it, the count after it goes on above
 * the highest one before it until the next IDR picture) and disc_fields
 * from its pic_struct: 0 -> 2, 1 / 2 -> 1,
 * 3 / 4 -> 2, 5 / 6 -> 3, 7 -> 4, 8 -> 6 field periods. pic_struct is 0 when
 * the SPS has neither HRD parameters nor pic_struct_present_flag; else
 * the picture timing SEI's; without one 0 for a frame-only stream, 1 / 2 for
 * a top / bottom field, and for a frame of an interlaced stream it is guessed
 * from the pictures before (all of the last 9 or more alike: that value; an
 * A, B, A, B alternation over 11 or more: B), or the stream cannot be cut.
 */

enum { DISC_PREFIX, DISC_SLICES, DISC_SUFFIX };

#define DISC_SUFFIX_MAX 100

typedef struct H264DiscNal {
    int zr;                 ///< first byte of the zero run in front of its start code
    int sc;                 ///< its start code 00 00 01
    int type;               ///< nal_unit_type
} H264DiscNal;

typedef struct H264DiscSlice {
    int slice_type, pps_id, frame_num, field, bottom, idr, idr_id, poc_lsb;
} H264DiscSlice;

/** Where the cut of the access unit at the start of the kept bytes stands. */
typedef struct H264DiscCut {
    H264ParseContext *p;
    H264DiscNal *nal;       ///< the NAL units found so far
    int nb_nal, nal_cap;
    int scan;               ///< next offset to look for a start code at
    int done;               ///< NAL units decided
    int phase;
    H264DiscSlice first;    ///< the picture's first slice
    int suffix[DISC_SUFFIX_MAX + 1];    ///< the units the trailing-unit search went over
    int nb_suffix, suffix_k;
    int no_unit;            ///< the chunk given out is no frame
} H264DiscCut;

static int disc_hidden(int t)
{
    /* filler, ends of sequence / stream; the types that start no unit */
    return (t >= 10 && t <= 12) || !t || (t >= 16 && t <= 18) || (t >= 21 && t <= 23) ||
           (t >= 25 && t <= 27) || t == 29 || t == 31;
}

static int disc_push(H264DiscCut *st, int zr, int sc, int type)
{
    if (st->nb_nal == st->nal_cap) {
        int cap = st->nal_cap ? 2 * st->nal_cap : 64;
        H264DiscNal *n = av_realloc_array(st->nal, cap, sizeof(*n));
        if (!n)
            return AVERROR(ENOMEM);
        st->nal     = n;
        st->nal_cap = cap;
    }
    st->nal[st->nb_nal++] = (H264DiscNal){ zr, sc, type };
    return 0;
}

/* The unit (from its NAL header, size bytes) as RBSP read with gb; the
 * caller frees *buf. */
static int disc_rbsp(const uint8_t *nal, int size, H2645RBSP *rbsp, H2645NAL *n)
{
    av_fast_padded_malloc(&rbsp->rbsp_buffer, &rbsp->rbsp_buffer_alloc_size, size);
    if (!rbsp->rbsp_buffer)
        return AVERROR(ENOMEM);
    if (ff_h2645_extract_rbsp(nal, size, rbsp, n, 1) < 0)
        return AVERROR_INVALIDDATA;
    return init_get_bits8(&n->gb, n->data, n->size);
}

/* A parameter set of the access unit into the parser's sets. */
static int disc_param_set(H264ParseContext *p, AVCodecContext *avctx, const uint8_t *nal, int size)
{
    H2645RBSP rbsp = { NULL };
    H2645NAL n = { NULL };
    int ret = disc_rbsp(nal, size, &rbsp, &n);

    if (ret >= 0) {
        skip_bits(&n.gb, 8);
        if ((nal[0] & 0x1f) == H264_NAL_SPS)
            ff_h264_decode_seq_parameter_set(&n.gb, avctx, &p->ps, 0);
        else
            ff_h264_decode_picture_parameter_set(&n.gb, avctx, &p->ps, n.size_bits);
    }
    av_freep(&rbsp.rbsp_buffer);
    return ret == AVERROR(ENOMEM) ? ret : 0;
}

/* A slice header as far as the picture comparison reads it: 1 read, 0 no
 * parameter sets for it or cut short, 2 a slice_type of 10 or more. */
static int disc_slice(H264ParseContext *p, const uint8_t *nal, int size, H264DiscSlice *h)
{
    uint8_t tmp[64 + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    const PPS *pps;
    const SPS *sps;
    GetBitContext gb;
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
    memset(h, 0, sizeof(*h));
    h->idr = (nal[0] & 0x1f) == H264_NAL_IDR_SLICE;
    if (init_get_bits8(&gb, tmp, n) < 0)
        return 0;
    get_ue_golomb_long(&gb);                /* first_mb_in_slice */
    h->slice_type = get_ue_golomb_31(&gb);
    if (get_bits_left(&gb) < 0)
        return 0;
    if (h->slice_type >= 10)
        return 2;
    h->pps_id = get_ue_golomb(&gb);
    if (h->pps_id < 0 || h->pps_id >= MAX_PPS_COUNT || !(pps = p->ps.pps_list[h->pps_id]) || !(sps = pps->sps))
        return 0;
    if (sps->residual_color_transform_flag)
        skip_bits(&gb, 2);                  /* colour_plane_id */
    h->frame_num = get_bits_long(&gb, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only_flag && (h->field = get_bits1(&gb)))
        h->bottom = get_bits1(&gb);
    if (h->idr)
        h->idr_id = get_ue_golomb_long(&gb);
    if (!sps->poc_type)
        h->poc_lsb = get_bits_long(&gb, sps->log2_max_poc_lsb);
    return get_bits_left(&gb) >= 0;
}

static int disc_same(const H264DiscSlice *a, const H264DiscSlice *b)
{
    return a->pps_id == b->pps_id && a->frame_num == b->frame_num && a->poc_lsb == b->poc_lsb &&
           a->field == b->field && (!a->field || a->bottom == b->bottom) && a->idr == b->idr &&
           (!a->idr || a->idr_id == b->idr_id);
}

/* The chunk of the kept bytes up to end is decided: the cut starts anew. */
static int disc_end(H264DiscCut *st, int end, int no_unit)
{
    st->nb_nal = st->scan = st->done = st->nb_suffix = st->suffix_k = 0;
    st->phase   = DISC_PREFIX;
    st->no_unit = no_unit;
    return end;
}

static int disc_fail(H264DiscCut *st, void *log, int type)
{
    av_log(log, AV_LOG_ERROR, "Title timeline: H.264: a data partition (NAL unit type %d) where a picture "
           "starts: the track cannot be ripped\n", type);
    disc_end(st, 0, 0);
    return AVERROR_PATCHWELCOME;
}

static int disc_unit_size(void *log, const uint8_t *buf, int avail, int final, void *state)
{
    H264DiscCut *st = state;
    H264ParseContext *p = st->p;
    int i, ret;

    if (!st->nb_nal) {                      /* the unit's own start code */
        int z = 0;
        while (z < avail && !buf[z])
            z++;
        if (z + 1 >= avail)
            return final ? DISC_CUT_NONE : 0;
        if (z < 2 || buf[z] != 1)
            return DISC_CUT_NONE;
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
        H264DiscSlice h;

        if (disc_hidden(t)) {
            st->done++;
            continue;
        }
        if (st->phase == DISC_PREFIX) {
            if (t >= H264_NAL_DPA && t <= H264_NAL_DPC)
                return disc_fail(st, log, t);
            if (t == H264_NAL_SLICE || t == H264_NAL_IDR_SLICE) {
                if (!complete)
                    return 0;
                if ((ret = disc_slice(p, nal, size, &h)) != 1) {
                    av_log(log, AV_LOG_DEBUG, "Title timeline: H.264: a picture %s: left out with the units in "
                           "front of it\n", ret == 2 ? "with a slice_type of 10 or more" :
                           "without its parameter sets");
                    return disc_end(st, end, 1);
                }
                st->first = h;
                st->phase = DISC_SLICES;
            } else if (t == H264_NAL_SPS || t == H264_NAL_PPS) {
                if (!complete)
                    return 0;
                if ((ret = disc_param_set(p, log, nal, size)) < 0)
                    return ret;
            }
            st->done++;
            continue;
        }
        if (st->phase == DISC_SLICES) {
            if (t == H264_NAL_DPB || t == H264_NAL_DPC) {
                st->done++;
                continue;
            }
            if (t == H264_NAL_SLICE || t == H264_NAL_IDR_SLICE || t == H264_NAL_DPA) {
                if (!complete)
                    return 0;
                ret = disc_slice(p, nal, size, &h);
                if (ret == 1 && h.pps_id != st->first.pps_id)
                    av_log(log, AV_LOG_WARNING, "Title timeline: H.264: slices of another picture parameter set "
                           "follow without a unit between them: taken as the next picture "
                           "[untested on real discs]\n");
                if (ret == 1 && disc_same(&h, &st->first)) {
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
            int stop   = t == H264_NAL_SPS || t == H264_NAL_SLICE || t == H264_NAL_IDR_SLICE ||
                         t == H264_NAL_AUD || (t >= H264_NAL_DPA && t <= H264_NAL_DPC) || t == 28;
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

/* pic_struct guessed from the pictures before: < 0 none. */
static int disc_guess_ps(const H264ParseContext *p)
{
    int n = p->disc_ps_n, l[128];

    if (n >= 9) {
        int v = p->disc_ps[p->disc_ps_start], k;
        for (k = 1; k < n && p->disc_ps[(p->disc_ps_start + k) & 127] == v; k++)
            ;
        if (k == n)
            return v;
    }
    if (n <= 10)
        return -1;
    for (int k = 0; k < n; k++)             /* the newest first */
        l[k] = p->disc_ps[(p->disc_ps_start + n - 1 - k) & 127];
    for (int k = 2; k != (n & ~1); k += 2)
        if (l[k] != l[0] || l[k + 1] != l[1])
            return -1;
    return l[1];
}

/* The values of the frame just cut. */
static int disc_picture(AVCodecParserContext *s, AVCodecContext *avctx, const uint8_t *buf, int size)
{
    static const uint8_t fields[9] = { 2, 1, 1, 2, 2, 3, 3, 4, 6 };
    H264ParseContext *p = s->priv_data;
    const SPS *sps;
    int ps, idr = 0;

    if (parse_nal_units(s, avctx, buf, size) < 0 || !(sps = p->ps.sps)) {
        av_log(avctx, AV_LOG_ERROR, "Title timeline: H.264: a picture's headers cannot be read\n");
        return AVERROR_INVALIDDATA;
    }
    s->key_frame = p->sei.recovery_point.recovery_frame_cnt >= 0;
    for (int i = 0; i + 3 < size; i++)      /* the first slice: an IDR picture's */
        if (!buf[i] && !buf[i + 1] && buf[i + 2] == 1 &&
            ((buf[i + 3] & 0x1f) == H264_NAL_SLICE || (buf[i + 3] & 0x1f) == H264_NAL_IDR_SLICE)) {
            idr = (buf[i + 3] & 0x1f) == H264_NAL_IDR_SLICE;
            break;
        }
    s->key_frame |= idr;
    /* the display order across a memory_management_control_operation 5:
     * the picture itself counts 0 after it (H.264 8.2.1) and is shown after
     * every picture before it */
    if (idr)
        p->disc_order_base = 0;
    if (p->disc_reset) {
        p->disc_order_base = p->disc_order_max + 2;
        s->output_picture_number = 0;
        if (!p->disc_resets++)
            av_log(avctx, AV_LOG_WARNING, "Title timeline: H.264: a picture with memory_management_control_operation "
                   "5 (the picture order count starts again after it): display order continued as H.264 8.2.1 "
                   "says [untested on real discs]\n");
        av_log(avctx, AV_LOG_VERBOSE, "Title timeline: H.264: memory_management_control_operation 5 in picture %"
               PRId64" of the reset count\n", p->disc_resets);
    }
    s->output_picture_number += p->disc_order_base;
    if (idr || p->disc_reset || s->output_picture_number > p->disc_order_max)
        p->disc_order_max = s->output_picture_number;
    if (!sps->nal_hrd_parameters_present_flag && !sps->vcl_hrd_parameters_present_flag &&
        !sps->pic_struct_present_flag)
        ps = 0;
    else if (sps->pic_struct_present_flag && p->sei.picture_timing.present)
        ps = p->sei.picture_timing.pic_struct;
    else if (sps->frame_mbs_only_flag)
        ps = 0;
    else if (p->picture_structure != PICT_FRAME)
        ps = p->picture_structure == PICT_BOTTOM_FIELD ? 2 : 1;
    else if ((ps = disc_guess_ps(p)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Title timeline: H.264: a frame of an interlaced stream without its "
               "pic_struct, and none can be guessed from the pictures before\n");
        return AVERROR_INVALIDDATA;
    } else
        av_log(avctx, AV_LOG_DEBUG, "Title timeline: H.264: a frame without its pic_struct: %d guessed from the "
               "pictures before [untested on real discs]\n", ps);
    if (ps < 0 || ps > 8) {
        av_log(avctx, AV_LOG_ERROR, "Title timeline: H.264: pic_struct %d\n", ps);
        return AVERROR_INVALIDDATA;
    }
    if (p->disc_ps_n == 128) {
        p->disc_ps_start = (p->disc_ps_start + 1) & 127;
        p->disc_ps_n--;
    }
    p->disc_ps[(p->disc_ps_start + p->disc_ps_n++) & 127] = ps;
    s->disc_fields = fields[ps];
    if (sps->timing_info_present_flag && sps->num_units_in_tick && sps->time_scale)
        av_reduce(&avctx->framerate.num, &avctx->framerate.den, sps->time_scale,
                  2LL * sps->num_units_in_tick, 1 << 30);
    return 0;
}

static int disc_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size, const uint8_t *buf, int buf_size)
{
    H264ParseContext *p = s->priv_data;
    H264DiscCut *st;
    int ret, r;

    if (!p->disc.state && !(p->disc.state = av_mallocz(sizeof(H264DiscCut)))) {
        s->disc_status = AVERROR(ENOMEM);
        *poutbuf       = NULL;
        *poutbuf_size  = 0;
        return 0;
    }
    st          = p->disc.state;
    st->p       = p;
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

static int h264_parse(AVCodecParserContext *s,
                      AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size,
                      const uint8_t *buf, int buf_size)
{
    H264ParseContext *p = s->priv_data;
    ParseContext *pc = &p->pc;
    AVRational time_base = { 0, 1 };
    int next;

    if (s->flags & PARSER_FLAG_DISC)
        return disc_parse(s, avctx, poutbuf, poutbuf_size, buf, buf_size);

    if (!p->got_first) {
        p->got_first = 1;
        if (avctx->extradata_size) {
            ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size,
                                     &p->ps, &p->is_avc, &p->nal_length_size,
                                     avctx->err_recognition, avctx);
        }
    }

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        next = h264_find_frame_end(p, buf, buf_size, avctx);

        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }

        if (next < 0 && next != END_NOT_FOUND) {
            av_assert1(pc->last_index + next >= 0);
            h264_find_frame_end(p, &pc->buffer[pc->last_index + next], -next, avctx); // update state
        }
    }

    parse_nal_units(s, avctx, buf, buf_size);

    if (avctx->framerate.num)
        time_base = av_inv_q(av_mul_q(avctx->framerate, (AVRational){2, 1}));
    if (p->sei.picture_timing.cpb_removal_delay >= 0) {
        s->dts_sync_point    = p->sei.buffering_period.present;
        s->dts_ref_dts_delta = p->sei.picture_timing.cpb_removal_delay;
        s->pts_dts_delta     = p->sei.picture_timing.dpb_output_delay;
    } else {
        s->dts_sync_point    = INT_MIN;
        s->dts_ref_dts_delta = INT_MIN;
        s->pts_dts_delta     = INT_MIN;
    }

    if (s->flags & PARSER_FLAG_ONCE) {
        s->flags &= PARSER_FLAG_COMPLETE_FRAMES;
    }

    if (s->dts_sync_point >= 0) {
        int64_t den = time_base.den * (int64_t)avctx->pkt_timebase.num;
        if (den > 0) {
            int64_t num = time_base.num * (int64_t)avctx->pkt_timebase.den;
            if (s->dts != AV_NOPTS_VALUE) {
                // got DTS from the stream, update reference timestamp
                p->reference_dts = av_sat_sub64(s->dts, av_rescale(s->dts_ref_dts_delta, num, den));
            } else if (p->reference_dts != AV_NOPTS_VALUE) {
                // compute DTS based on reference timestamp
                s->dts = av_sat_add64(p->reference_dts, av_rescale(s->dts_ref_dts_delta, num, den));
            }

            if (p->reference_dts != AV_NOPTS_VALUE && s->pts == AV_NOPTS_VALUE) {
                int64_t pts_dts_delta = av_rescale(s->pts_dts_delta, num, den);
                uint64_t pts = (uint64_t)s->dts + pts_dts_delta;
                if (pts == av_sat_add64(s->dts, pts_dts_delta))
                    s->pts = pts;
            }

            if (s->dts_sync_point > 0)
                p->reference_dts = s->dts; // new reference
        }
    }

    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return next;
}

static av_cold void h264_close(AVCodecParserContext *s)
{
    H264ParseContext *p = s->priv_data;
    ParseContext *pc = &p->pc;

    av_freep(&pc->buffer);
    if (p->disc.state)
        av_freep(&((H264DiscCut *)p->disc.state)->nal);
    ff_parser_disc_cut_close(&p->disc);

    ff_h264_sei_uninit(&p->sei);
    ff_h264_ps_uninit(&p->ps);
}

static av_cold int init(AVCodecParserContext *s)
{
    H264ParseContext *p = s->priv_data;

    p->reference_dts = AV_NOPTS_VALUE;
    p->last_frame_num = INT_MAX;
    ff_h264dsp_init(&p->h264dsp, 8, 1);
    return 0;
}

const FFCodecParser ff_h264_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_H264),
    .priv_data_size = sizeof(H264ParseContext),
    .init           = init,
    .parse          = h264_parse,
    .close          = h264_close,
};
