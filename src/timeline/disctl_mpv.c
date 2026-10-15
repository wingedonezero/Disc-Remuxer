/*
 * The title timeline: the rules for MPEG-1 / MPEG-2 video (ISO/IEC 11172-2,
 * 13818-2) pictures.
 */

#include <string.h>


#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/vlog.h"

#include "timeline/disctl.h"

/* frame_rate_code 1..8 (ISO/IEC 13818-2 Table 6-4) */
static const int rates[8][2] = {
    { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 },
};

typedef struct MpvState {
    int have_rate;        /* the first sequence header gave the frame rate */
    int mpeg2;            /* a sequence extension follows the sequence header */
    int progressive_seq;  /* sequence_extension progressive_sequence */
} MpvState;

/* The next start code 00 00 01 xx at or after i, or -1. */
static int next_sc(const uint8_t *p, int n, int i)
{
    for (; i + 3 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1)
            return i;
    return -1;
}

/* User data right after a GOP header (ISO/IEC 13818-2 6.2.2.6: user_data
 * at the GOP level; DVD-Video line-21 captions are carried there) leaves
 * the video: each user_data from its start code up to the next start code
 * (its trailing zero bytes with it) becomes a side unit. The GOP header is
 * 8 bytes; zero stuffing bytes after it, before the next start code, leave
 * the video too (they are no part of a unit: the reader keeps a GOP header
 * as its 8 bytes and drops a run of zero bytes as an empty unit). User data
 * after a sequence header or a picture header stays in the video. */
static int split_mpv(DRVideo *v, DRFrame *f)
{
    uint8_t *p = f->data;
    int n = f->size, i = 0, ret, z;

    /* Zero stuffing after an end of sequence code (a unit of its own 4
     * bytes; FFmpeg's parser ends a unit right after it): the zeros before
     * the next start code's two leave the video, as after a GOP header. */
    for (z = 0; z < n && !p[z]; z++)
        ;
    if (z > 2 && z < n && p[z] == 1) {
        memmove(p, p + z - 2, n - (z - 2));
        n -= z - 2;
        ff_disctl_video_stuffing(v, z - 2);
    }

    while ((i = next_sc(p, n, i)) >= 0) {
        int e;

        if (p[i + 3] != 0xB8) {
            i += 4;
            continue;
        }
        i += 8;                                  /* group_of_pictures_header: 8 bytes */
        for (z = i; z < n && !p[z]; z++)
            ;
        if (z - i > 2 && z < n && p[z] == 1) {   /* zeros, then 00 00 01: the zeros before those two go */
            memmove(p + i, p + z - 2, n - (z - 2));
            n -= z - 2 - i;
            ff_disctl_video_stuffing(v, z - 2 - i);
        }
        while ((i = next_sc(p, n, i)) >= 0 && p[i + 3] == 0xB2) {
            e = next_sc(p, n, i + 4);
            if (e < 0)
                e = n;
            if ((ret = ff_disctl_video_side(v, p + i, e - i, f->pos + i)) < 0)
                return ret;
            memmove(p + i, p + e, n - e);
            n -= e - i;
        }
        if (i < 0)
            break;
    }
    f->size = n;
    return 0;
}

/* A unit as FFmpeg's parser cuts it: sequence header (with its extension and
 * user data), GOP header (and its user data), picture header (with its
 * coding extension and user data) and slices; FFmpeg keeps both field
 * pictures of a frame in one unit.
 * How decoding can start at an I picture (ISO/IEC 13818-2 6.3.8): its GOP
 * header's closed_gop (the B pictures right after it use only backward
 * prediction: clean) or broken_link (they cannot be decoded); an I picture
 * of an open GOP, or without a GOP header, may have B pictures after it
 * that use the picture before it. The scan: MPEG-1 frames are progressive;
 * MPEG-2 frames as the picture coding extension states (progressive_frame,
 * top_field_first, repeat_first_field). */
static int picture_mpv(DRVideo *v, const DRFrame *f, DRPicture *pic)
{
    MpvState *s = ff_disctl_video_priv(v);
    const uint8_t *p = f->data;
    int n = f->size, i = 0, pictures = 0, structure = 3, tff = 0, rff = 0, prog_frame = 0, have_ext = 0;
    int pct = 0, tr = 0, gop = 0, closed = 0, broken = 0, seq_end = 0, seq_start = 0;

    while ((i = next_sc(p, n, i)) >= 0) {
        int code = p[i + 3];

        if (code == 0xB3 && i + 12 <= n) {
            int frc = p[i + 7] & 0xF;
            int e   = next_sc(p, n, i + 12);
            int num = 0, den = 0, mpeg2 = 0;

            if (frc >= 1 && frc <= 8) {
                num = rates[frc - 1][0];
                den = rates[frc - 1][1];
            }
            /* the sequence extension (id 1) right after it, past any quantiser matrices */
            while (e >= 0 && e + 10 <= n && p[e + 3] == 0xB5 && (p[e + 4] & 0xF0) != 0x10)
                e = next_sc(p, n, e + 4);
            if (e >= 0 && e + 10 <= n && p[e + 3] == 0xB5 && (p[e + 4] & 0xF0) == 0x10) {
                mpeg2 = 1;
                num *= ((p[e + 9] >> 5) & 3) + 1;
                den *= (p[e + 9] & 0x1F) + 1;
                if (!s->have_rate)
                    s->progressive_seq = (p[e + 5] >> 3) & 1;
            }
            seq_start |= !pictures;
            if (num && den) {
                if (!s->have_rate) {
                    s->have_rate = 1;
                    s->mpeg2     = mpeg2;
                }
                ff_disctl_video_set_rate(v, num, den);
            }
        } else if (code == 0xB8 && i + 8 <= n && !pictures) {
            gop    = 1;
            closed = (p[i + 7] >> 6) & 1;
            broken = (p[i + 7] >> 5) & 1;
        } else if (code == 0xB7) {
            seq_end = 1;
        } else if (code == 0x00 && i + 6 <= n) {
            if (!pictures++) {
                pct = (p[i + 5] >> 3) & 7;
                tr  = (p[i + 4] << 2) | (p[i + 5] >> 6);
            }
        } else if (code == 0xB5 && i + 9 <= n && (p[i + 4] & 0xF0) == 0x80 && pictures == 1 && !have_ext) {
            have_ext   = 1;
            structure  = p[i + 6] & 3;
            tff        = p[i + 7] >> 7;
            rff        = (p[i + 7] >> 1) & 1;
            prog_frame = p[i + 8] >> 7;
        }
        i += 4;
    }
    pic->seq_end   = seq_end;
    pic->seq_start = seq_start;
    if (!pictures) {
        pic->type = DR_PIC_OTHER;
        return 0;
    }
    if (pct < 1 || pct > 3 || !structure) {
        dr_vlog(NULL, DR_VLOG_WARNING, "Title timeline: MPEG video: a picture of coding type %d (structure %d) is not a "
               "picture of the stream: left out\n", pct, structure);
        pic->type = DR_PIC_OTHER;
        return 0;
    }
    pic->type  = pct + 1;
    pic->order = tr;
    if (pic->type == DR_PIC_I)
        pic->start = !gop ? DR_START_OPEN : broken ? DR_START_BROKEN : closed ? DR_START_CLEAN : DR_START_OPEN;
    if (!s->mpeg2)
        pic->scan = DR_SCAN_KNOWN | DR_SCAN_PROGRESSIVE;
    else if (have_ext)
        pic->scan = DR_SCAN_KNOWN | (prog_frame ? DR_SCAN_PROGRESSIVE : 0) | (tff ? DR_SCAN_TFF : 0) |
                    (rff ? DR_SCAN_RFF : 0);
    if (!s->mpeg2)
        pic->fields = 2;
    else if (structure != 3)
        pic->fields = pictures >= 2 ? 2 : 1;           /* a field pair, or a lone field */
    else if (s->progressive_seq) {
        static const int prog[4] = { 2, 2, 4, 6 };     /* repeated frames of a progressive sequence */
        pic->fields = prog[rff * 2 + tff];
    } else
        pic->fields = (prog_frame && rff) ? 3 : 2;
    return 0;
}

const DRVideoRules ff_disctl_video_mpv = {
    .picture      = picture_mpv,
    .split        = split_mpv,
    .order_period = 0x400,
    .priv_size    = sizeof(MpvState),
};
