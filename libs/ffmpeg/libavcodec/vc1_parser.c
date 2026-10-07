/*
 * VC-1 and WMV3 parser
 * Copyright (c) 2006-2007 Konstantin Shishkov
 * Partly based on vc9.c (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer
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
 * VC-1 and WMV3 parser
 */

#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/log.h"
#include "parser.h"
#include "parser_internal.h"
#include "vc1.h"
#include "get_bits.h"
#include "vc1dsp.h"

/** The maximum number of bytes of a sequence, entry point or
 *  frame header whose values we pay any attention to */
#define UNESCAPED_THRESHOLD 37

/** The maximum number of bytes of a sequence, entry point or
 *  frame header which must be valid memory (because they are
 *  used to update the bitstream cache in skip_bits() calls)
 */
#define UNESCAPED_LIMIT 144

typedef enum {
    NO_MATCH,
    ONE_ZERO,
    TWO_ZEROS,
    ONE
} VC1ParseSearchState;

typedef struct VC1ParseContext {
    ParseContext pc;
    VC1Context v;
    uint8_t prev_start_code;
    size_t bytes_to_skip;
    uint8_t unesc_buffer[UNESCAPED_LIMIT];
    size_t unesc_index;
    VC1ParseSearchState search_state;
    FFDiscCut disc;
} VC1ParseContext;

static void vc1_extract_header(AVCodecParserContext *s, AVCodecContext *avctx,
                               const uint8_t *buf, int buf_size)
{
    /* Parse the header we just finished unescaping */
    VC1ParseContext *vpc = s->priv_data;
    GetBitContext gb;
    int ret;
    vpc->v.s.avctx = avctx;
    ret = init_get_bits8(&gb, buf, buf_size);
    av_assert1(ret >= 0); // buf_size is bounded by UNESCAPED_THRESHOLD

    switch (vpc->prev_start_code) {
    case VC1_CODE_SEQHDR & 0xFF:
        ff_vc1_decode_sequence_header(avctx, &vpc->v, &gb);
        break;
    case VC1_CODE_ENTRYPOINT & 0xFF:
        ff_vc1_decode_entry_point(avctx, &vpc->v, &gb);
        break;
    case VC1_CODE_FRAME & 0xFF:
        if(vpc->v.profile < PROFILE_ADVANCED)
            ret = ff_vc1_parse_frame_header    (&vpc->v, &gb);
        else
            ret = ff_vc1_parse_frame_header_adv(&vpc->v, &gb);

        if (ret < 0)
            break;

        /* keep AV_PICTURE_TYPE_BI internal to VC1 */
        if (vpc->v.s.pict_type == AV_PICTURE_TYPE_BI)
            s->pict_type = AV_PICTURE_TYPE_B;
        else
            s->pict_type = vpc->v.s.pict_type;

        if (vpc->v.broadcast){
            // process pulldown flags
            s->repeat_pict = 1;
            // Pulldown flags are only valid when 'broadcast' has been set.
            if (vpc->v.rff){
                // repeat field
                s->repeat_pict = 2;
            }else if (vpc->v.rptfrm){
                // repeat frames
                s->repeat_pict = vpc->v.rptfrm * 2 + 1;
            }
        }else{
            s->repeat_pict = 0;
        }

        if (vpc->v.broadcast && vpc->v.interlace && !vpc->v.psf)
            s->field_order = vpc->v.tff ? AV_FIELD_TT : AV_FIELD_BB;
        else
            s->field_order = AV_FIELD_PROGRESSIVE;

        break;
    }
    s->format = vpc->v.chromaformat == 1 ? AV_PIX_FMT_YUV420P
                                         : AV_PIX_FMT_NONE;
    if (avctx->width && avctx->height) {
        s->width        = avctx->width;
        s->height       = avctx->height;
        s->coded_width  = FFALIGN(avctx->coded_width,  16);
        s->coded_height = FFALIGN(avctx->coded_height, 16);
    }
}

/* ---- Disc mode (PARSER_FLAG_DISC): the cut rule of the disc reader ----
 * A unit is one frame with what belongs to it, in start-code units (BDUs,
 * Annex E):
 *   [sequence header]* [sequence user data]* [entry point]*
 *   [sequence / entry-point user data]* frame [field / frame user data]*
 *   [field [field user data]* [end of sequence]*]
 * User data after the frame header stays with that frame (FFmpeg's parser
 * starts the next unit there), so does an end of sequence after a second
 * field. Units without a frame take no time and are left out by the video
 * stage: an end of sequence without a second field before it, and headers
 * followed by another sequence header or an end of sequence instead of a
 * frame. A slice, a field or user data where none can be (or a reserved
 * start code) stops the track: the stream cannot be cut. */

#define DISC_SC_END    0x0A   /* end of sequence */
#define DISC_SC_FIELD  0x0C
#define DISC_SC_FRAME  0x0D
#define DISC_SC_ENTRY  0x0E
#define DISC_SC_SEQ    0x0F
#define DISC_SC_SLICE  0x0B
#define DISC_SC_UD_SLICE 0x1B
#define DISC_SC_UD_FIELD 0x1C
#define DISC_SC_UD_FRAME 0x1D
#define DISC_SC_UD_ENTRY 0x1E
#define DISC_SC_UD_SEQ   0x1F

/** Where the scan of the unit at the start of the kept bytes stands. */
typedef struct VC1DiscCut {
    int scan;              /**< next offset to look for a start code at */
    int part;              /**< the part of the unit the last start code began */
    int stage;             /**< the header stage reached (part 0) */
} VC1DiscCut;

enum { PART_HEADERS, PART_FRAME, PART_FIELD, PART_END_AFTER_FIELD, PART_END };

static int disc_is_sc(const uint8_t *p)
{
    return !p[0] && !p[1] && p[2] == 1;
}

static void disc_cut_fail(void *log, int code, const char *where)
{
    av_log(log, AV_LOG_ERROR, "Title timeline: VC-1: start code 0x%02X %s: the track cannot be ripped\n", code, where);
}

/* The part a unit's first BDU begins, or < 0. */
static int disc_first_part(void *log, int code, VC1DiscCut *st)
{
    switch (code) {
    case DISC_SC_SEQ:     st->stage = 0; return PART_HEADERS;
    case DISC_SC_UD_SEQ:  st->stage = 1; return PART_HEADERS;
    case DISC_SC_ENTRY:   st->stage = 2; return PART_HEADERS;
    case DISC_SC_UD_ENTRY: st->stage = 3; return PART_HEADERS;
    case DISC_SC_FRAME:   return PART_FRAME;
    case DISC_SC_END:     return PART_END;
    }
    disc_cut_fail(log, code, "where a frame or its headers must start");
    return AVERROR_INVALIDDATA;
}

/* 1 when a BDU of code continues the unit, 0 when it starts the next one,
 * < 0 when it cannot be there. */
static int disc_continues(void *log, int code, VC1DiscCut *st)
{
    switch (st->part) {
    case PART_HEADERS:
        switch (code) {
        case DISC_SC_SEQ:
            if (st->stage > 0)
                return 0;       /* the headers before it have no frame */
            return 1;
        case DISC_SC_UD_SEQ:
            st->stage = st->stage <= 1 ? 1 : 3;
            return 1;
        case DISC_SC_ENTRY:
            if (st->stage > 2) {
                disc_cut_fail(log, code, "(entry point) after the entry-point user data");
                return AVERROR_INVALIDDATA;
            }
            st->stage = 2;
            return 1;
        case DISC_SC_UD_ENTRY:
            st->stage = 3;
            return 1;
        case DISC_SC_FRAME:
            st->part = PART_FRAME;
            return 1;
        case DISC_SC_END:
            st->part = PART_END;  /* the headers and the end are left out together */
            return 1;
        }
        disc_cut_fail(log, code, "before a frame start code");
        return AVERROR_INVALIDDATA;
    case PART_FRAME:
        if (code == DISC_SC_UD_FIELD || code == DISC_SC_UD_FRAME)
            return 1;
        if (code == DISC_SC_FIELD) {
            st->part = PART_FIELD;
            return 1;
        }
        return 0;
    case PART_FIELD:
        if (code == DISC_SC_UD_FIELD)
            return 1;
        if (code == DISC_SC_END) {
            st->part = PART_END_AFTER_FIELD;
            return 1;
        }
        return 0;
    default:                    /* PART_END_AFTER_FIELD, PART_END */
        return code == DISC_SC_END;
    }
}

static int disc_unit_size(void *log, const uint8_t *buf, int avail, int final, void *state)
{
    VC1DiscCut *st = state;
    int i = st->scan;

    if (!i) {
        int part;

        if (avail < 4)
            return final ? DISC_CUT_NONE : 0;
        if (!disc_is_sc(buf))
            return DISC_CUT_NONE;
        if ((part = disc_first_part(log, buf[3], st)) < 0) {
            *st = (VC1DiscCut){ 0 };
            return part;
        }
        st->part = part;
        i = 4;
    }
    for (; i + 3 < avail; i++) {
        int r;

        if (buf[i + 2] > 1) {
            i += 2;
            continue;
        }
        if (!disc_is_sc(buf + i))
            continue;
        if ((r = disc_continues(log, buf[i + 3], st)) <= 0) {
            *st = (VC1DiscCut){ 0 };
            return r < 0 ? r : i;
        }
        i += 3;
    }
    if (final) {
        *st = (VC1DiscCut){ 0 };
        return avail;
    }
    st->scan = i;
    return 0;
}

/* The next start code; the last two bytes may begin one. */
static int disc_resync(const uint8_t *buf, int avail)
{
    for (int i = 0; i + 2 < avail; i++)
        if (disc_is_sc(buf + i))
            return i;
    return avail > 2 ? avail - 2 : -1;
}

static const FFDiscCutRule disc_rule = { disc_unit_size, disc_resync, sizeof(VC1DiscCut) };

static int vc1_parse(AVCodecParserContext *s,
                           AVCodecContext *avctx,
                           const uint8_t **poutbuf, int *poutbuf_size,
                           const uint8_t *buf, int buf_size)
{
    /* Here we do the searching for frame boundaries and headers at
     * the same time. Only a minimal amount at the start of each
     * header is unescaped. */
    VC1ParseContext *vpc = s->priv_data;
    int pic_found = vpc->pc.frame_start_found;
    uint8_t *unesc_buffer = vpc->unesc_buffer;
    size_t unesc_index = vpc->unesc_index;
    VC1ParseSearchState search_state = vpc->search_state;
    int start_code_found = 0;
    int next = END_NOT_FOUND;
    int i = vpc->bytes_to_skip;

    if (s->flags & PARSER_FLAG_DISC)
        return ff_parser_disc_cut(s, avctx, &disc_rule, &vpc->disc, poutbuf, poutbuf_size, buf, buf_size);

    if (pic_found && buf_size == 0) {
        /* EOF considered as end of frame */
        memset(unesc_buffer + unesc_index, 0, UNESCAPED_THRESHOLD - unesc_index);
        vc1_extract_header(s, avctx, unesc_buffer, unesc_index);
        next = 0;
    }
    while (i < buf_size) {
        uint8_t b;
        start_code_found = 0;
        while (i < buf_size && unesc_index < UNESCAPED_THRESHOLD) {
            b = buf[i++];
            unesc_buffer[unesc_index++] = b;
            if (search_state <= ONE_ZERO)
                search_state = b ? NO_MATCH : search_state + 1;
            else if (search_state == TWO_ZEROS) {
                if (b == 1)
                    search_state = ONE;
                else if (b > 1) {
                    if (b == 3)
                        unesc_index--; // swallow emulation prevention byte
                    search_state = NO_MATCH;
                }
            }
            else { // search_state == ONE
                // Header unescaping terminates early due to detection of next start code
                search_state = NO_MATCH;
                start_code_found = 1;
                break;
            }
        }
        if ((s->flags & PARSER_FLAG_COMPLETE_FRAMES) &&
                unesc_index >= UNESCAPED_THRESHOLD &&
                vpc->prev_start_code == (VC1_CODE_FRAME & 0xFF))
        {
            // No need to keep scanning the rest of the buffer for
            // start codes if we know it contains a complete frame and
            // we've already unescaped all we need of the frame header
            vc1_extract_header(s, avctx, unesc_buffer, unesc_index);
            unesc_index = 0;
            break;
        }
        if (unesc_index >= UNESCAPED_THRESHOLD && !start_code_found) {
            while (i < buf_size) {
                if (search_state == NO_MATCH) {
                    i += vpc->v.vc1dsp.startcode_find_candidate(buf + i, buf_size - i);
                    if (i < buf_size) {
                        search_state = ONE_ZERO;
                    }
                    i++;
                } else {
                    b = buf[i++];
                    if (search_state == ONE_ZERO)
                        search_state = b ? NO_MATCH : TWO_ZEROS;
                    else if (search_state == TWO_ZEROS) {
                        if (b >= 1)
                            search_state = b == 1 ? ONE : NO_MATCH;
                    }
                    else { // search_state == ONE
                        search_state = NO_MATCH;
                        start_code_found = 1;
                        break;
                    }
                }
            }
        }
        if (start_code_found) {
            vc1_extract_header(s, avctx, unesc_buffer, unesc_index);

            vpc->prev_start_code = b;
            unesc_index = 0;

            if (!(s->flags & PARSER_FLAG_COMPLETE_FRAMES)) {
                if (!pic_found && (b == (VC1_CODE_FRAME & 0xFF) || b == (VC1_CODE_FIELD & 0xFF))) {
                    pic_found = 1;
                }
                else if (pic_found && b != (VC1_CODE_FIELD & 0xFF) && b != (VC1_CODE_SLICE & 0xFF)
                                   && b != (VC1_CODE_ENDOFSEQ & 0xFF)) {
                    next = i - 4;
                    pic_found = b == (VC1_CODE_FRAME & 0xFF);
                    break;
                }
            }
        }
    }

    vpc->pc.frame_start_found = pic_found;
    vpc->unesc_index = unesc_index;
    vpc->search_state = search_state;

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        if (ff_combine_frame(&vpc->pc, next, &buf, &buf_size) < 0) {
            vpc->bytes_to_skip = 0;
            *poutbuf = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }
    }

    /* If we return with a valid pointer to a combined frame buffer
     * then on the next call then we'll have been unhelpfully rewound
     * by up to 4 bytes (depending upon whether the start code
     * overlapped the input buffer, and if so by how much). We don't
     * want this: it will either cause spurious second detections of
     * the start code we've already seen, or cause extra bytes to be
     * inserted at the start of the unescaped buffer. */
    vpc->bytes_to_skip = 4;
    if (next < 0 && next != END_NOT_FOUND)
        vpc->bytes_to_skip += next;

    *poutbuf = buf;
    *poutbuf_size = buf_size;
    return next;
}

static av_cold int vc1_parse_init(AVCodecParserContext *s)
{
    VC1ParseContext *vpc = s->priv_data;
    vpc->v.s.slice_context_count = 1;
    vpc->v.first_pic_header_flag = 1;
    vpc->v.parse_only = 1;
    vpc->prev_start_code = 0;
    vpc->bytes_to_skip = 0;
    vpc->unesc_index = 0;
    vpc->search_state = NO_MATCH;
    ff_vc1dsp_init(&vpc->v.vc1dsp); /* startcode_find_candidate */
    return 0;
}

static av_cold void vc1_parse_close(AVCodecParserContext *s)
{
    VC1ParseContext *vpc = s->priv_data;

    ff_parser_disc_cut_close(&vpc->disc);
    ff_parse_close(s);
}

const FFCodecParser ff_vc1_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_VC1),
    .priv_data_size = sizeof(VC1ParseContext),
    .init           = vc1_parse_init,
    .parse          = vc1_parse,
    .close          = vc1_parse_close,
};
