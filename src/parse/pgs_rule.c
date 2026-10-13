/*
 * HDMV presentation graphics (Blu-ray PGS) display-set cut rule, and the
 * text subtitle (TextST) one-segment frame rule.
 *
 * The rules the generic disc engine runs (today FFmpeg's parser.c disc
 * engine; the native splitter later). Moved out of our FFmpeg's
 * hdmv_pgs_parser.c disc mode; the cutting rules are described there.
 */

#include <stddef.h>
#include <stdint.h>

#include "base/bytes.h"
#include "base/vlog.h"

#include "parse/hdmv_textst.h"
#include "parse/rules.h"

#define SEG_PCS 0x16
#define SEG_END 0x80
#define SEG_DSS 0x81
#define SEG_DPS 0x82

static int disc_seg_size(const uint8_t *b, int avail)
{
    if (avail < 3)
        return -1;
    return dr_rb16(b + 1) + 3 + (b[0] == SEG_DSS ? 2 : 0);
}

static int disc_is_start(int t)
{
    return t == SEG_PCS || t == SEG_DPS;
}

static int disc_unit_size(void *logctx, const uint8_t *buf, int avail, int final, void *state)
{
    PGSDiscCut *st = state;
    int off = 0, size;

    st->no_unit = 0;
    if ((size = disc_seg_size(buf, avail)) < 0)
        return final && avail ? (st->no_unit = 1, avail) : 0;
    if (!disc_is_start(buf[0])) {
        dr_vlog(logctx, DR_VLOG_DEBUG, "Title timeline: PGS: a segment of type 0x%02x that starts no display set: "
               "left out\n", buf[0]);
        st->no_unit = 1;
        return size <= avail ? size : final ? avail : 0;
    }
    for (;;) {
        if ((size = disc_seg_size(buf + off, avail - off)) < 0)
            break;
        if (off && disc_is_start(buf[off])) {
            dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: PGS: a display set without its end segment, cut short "
                   "by the next one: left out\n");
            st->no_unit = 1;
            return off;
        }
        if (size > avail - off)
            break;
        off += size;
        if (buf[off - size] == SEG_END)
            return off;
    }
    if (!final)
        return 0;
    dr_vlog(logctx, DR_VLOG_WARNING, "Title timeline: PGS: a display set the stream ends in: left out\n");
    st->no_unit = 1;
    return avail;
}

static int disc_resync(const uint8_t *buf, int avail)
{
    return avail;
}

const DrCutRule dr_pgs_cut = { disc_unit_size, disc_resync, sizeof(PGSDiscCut) };

/* ---- text subtitles: one segment per unit ---- */

static int text_sync(void *ctx, const uint8_t *buf, int avail)
{
    return 1;                       /* every segment starts where the one before ends */
}

static int text_size(void *ctx, const uint8_t *buf, int avail)
{
    int size = ff_hdmv_textst_segment_size(buf, avail);
    return size < 0 ? 0 : size;
}

static int text_check(void *ctx, const uint8_t *buf, int size)
{
    HDMVTextstDialog d;

    return buf[0] != HDMV_TEXTST_DPS || ff_hdmv_textst_dialog(buf, size, &d);
}

static int text_resync(void *ctx, const uint8_t *buf, int avail)
{
    return -1;
}

/* a segment is at most 3 + 65535 + 2 bytes */
const DrFrameRule dr_textst_frames = { text_sync, text_size, text_check, text_resync, 0x20000 };
