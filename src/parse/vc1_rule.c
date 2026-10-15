/*
 * VC-1 (SMPTE 421M Advanced Profile): the frame-unit cut rule
 *
 * The rule the splitter engine runs (parse/split.h); it was once a disc
 * mode of ours in FFmpeg's vc1_parser.c.
 */

#include <stdint.h>

#include "base/err.h"
#include "base/vlog.h"

#include "parse/rules.h"

/* ---- The cut rule of the disc reader ----
 * A unit is one frame with what belongs to it, in start-code units (BDUs,
 * Annex E):
 *   [sequence header]* [sequence user data]* [entry point]*
 *   [sequence / entry-point user data]* frame [field / frame user data]*
 *   [field [field user data]* [end of sequence]*]
 * User data after the frame header stays with that frame (FFmpeg's own
 * parser would start the next unit there), so does an end of sequence after a second
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
    dr_vlog(log, DR_VLOG_ERROR, "Title timeline: VC-1: start code 0x%02X %s: the track cannot be ripped\n", code, where);
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
    return DR_ERR_INVALID;
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
                return DR_ERR_INVALID;
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
        return DR_ERR_INVALID;
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
            return final ? DR_CUT_NONE : 0;
        if (!disc_is_sc(buf))
            return DR_CUT_NONE;
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

const DrCutRule dr_vc1_cut = { disc_unit_size, disc_resync, sizeof(VC1DiscCut) };
