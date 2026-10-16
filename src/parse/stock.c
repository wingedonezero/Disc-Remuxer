/*
 * The stock frame cutters (stock.h): FFmpeg's parsers for MPEG-1/2 video
 * and AC-3 / E-AC-3, as far as they decide the cut, ported from FFmpeg
 * 9.0.2 — libavcodec/parser.c (av_parser_parse2's offsets,
 * ff_combine_frame), mpegvideo_parser.c (mpeg1_find_frame_end), utils.c
 * (avpriv_find_start_code), aac_ac3_parser.c (ff_aac_ac3_parse),
 * ac3_parser.c (ac3_sync, ff_ac3_parse_header with the bitstream
 * information it checks, ff_ac3_find_syncword) and ac3tab.c's tables.
 * Part of FFmpeg, LGPL-2.1-or-later; copyright (c) 2000-2001 Fabrice
 * Bellard, (c) 2002-2004 Michael Niedermayer and the FFmpeg developers.
 * Each function keeps the shape of the one it comes from, so the two can
 * be read side by side.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/bytes.h"
#include "base/crc16.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"
#include "parse/stock.h"

#define END_NOT_FOUND (-100)
#define NOT_COMBINED  (-1)      /* ff_combine_frame's "no frame yet" (and its EINVAL) */

/* ---- the parser host (parser.c) ---- */

typedef struct ParseContext {
    uint8_t *buffer;
    int index;
    int last_index;
    unsigned buffer_size;
    uint32_t state;             /* the last few bytes in MSB order */
    int frame_start_found;
    int overread;               /* bytes irreversibly read from the next frame */
    int overread_index;         /* where in buffer the overread bytes are */
    uint64_t state64;           /* the last 8 bytes in MSB order */
} ParseContext;

struct DrStock {
    enum DrStockCodec codec;
    ParseContext pc;
    /* av_parser_parse2's offsets */
    int fetched;
    int64_t cur_offset, next_frame_offset, frame_offset;
    int duration, rate;
    /* the AC-3 parser's (AACAC3ParseContext) */
    int remaining_size;
    uint64_t state;
    int need_next_header;
};

/* av_fast_realloc: the buffer grown to at least min_size, its bytes kept. */
static int grow(ParseContext *pc, size_t min_size)
{
    uint8_t *b;
    size_t size;

    if (min_size <= pc->buffer_size)
        return 0;
    size = DR_MAX(min_size + min_size / 16 + 32, min_size);
    if (!(b = dr_realloc(pc->buffer, size)))
        return DR_ERR_NOMEM;
    pc->buffer      = b;
    pc->buffer_size = size;
    return 0;
}

/* ff_combine_frame: 0 a whole frame in *buf / *buf_size; NOT_COMBINED the
 * bytes are kept for later; DR_ERR_NOMEM. */
static int combine_frame(ParseContext *pc, int next, const uint8_t **buf, int *buf_size)
{
    /* the overread bytes of the last frame into the buffer */
    for (; pc->overread > 0; pc->overread--)
        pc->buffer[pc->index++] = pc->buffer[pc->overread_index++];

    if (next > *buf_size)
        return NOT_COMBINED;

    /* flush what remains at the end */
    if (!*buf_size && next == END_NOT_FOUND)
        next = 0;

    pc->last_index = pc->index;

    /* keep the bytes */
    if (next == END_NOT_FOUND) {
        if (grow(pc, (size_t)*buf_size + pc->index + DR_BUF_PADDING) < 0) {
            pc->index = 0;
            return DR_ERR_NOMEM;
        }
        memcpy(&pc->buffer[pc->index], *buf, *buf_size);
        memset(&pc->buffer[pc->index + *buf_size], 0, DR_BUF_PADDING);
        pc->index += *buf_size;
        return NOT_COMBINED;
    }

    *buf_size          =
    pc->overread_index = pc->index + next;

    /* append to the kept bytes */
    if (pc->index) {
        if (grow(pc, (size_t)(next + pc->index) + DR_BUF_PADDING) < 0) {
            *buf_size          =
            pc->overread_index =
            pc->index          = 0;
            return DR_ERR_NOMEM;
        }
        if (next > -DR_BUF_PADDING)
            memcpy(&pc->buffer[pc->index], *buf, next + DR_BUF_PADDING);
        pc->index = 0;
        *buf      = pc->buffer;
    }

    if (next < -8) {
        pc->overread += -8 - next;
        next = -8;
    }
    /* keep the overread bytes */
    for (; next < 0; next++) {
        pc->state   = pc->state   << 8 | pc->buffer[pc->last_index + next];
        pc->state64 = pc->state64 << 8 | pc->buffer[pc->last_index + next];
        pc->overread++;
    }
    return 0;
}

/* ---- MPEG-1 / MPEG-2 video (mpegvideo_parser.c, utils.c) ---- */

#define SEQ_END_CODE         0x000001b7
#define SEQ_START_CODE       0x000001b3
#define PICTURE_START_CODE   0x00000100
#define SLICE_MIN_START_CODE 0x00000101
#define SLICE_MAX_START_CODE 0x000001af
#define EXT_START_CODE       0x000001b5

/* avpriv_find_start_code */
static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end, uint32_t *state)
{
    if (p >= end)
        return end;

    for (int i = 0; i < 3; i++) {
        uint32_t tmp = *state << 8;
        *state = tmp + *(p++);
        if (tmp == 0x100 || p == end)
            return p;
    }

    while (p < end) {
        if      (p[-1] > 1             ) p += 3;
        else if (p[-2]                 ) p += 2;
        else if (p[-3] | (p[-1] - 1)   ) p++;
        else {
            p++;
            break;
        }
    }

    p = DR_MIN(p, end) - 4;
    *state = dr_rb32(p);

    return p + 4;
}

/* mpeg1_find_frame_end: where the picture being read ends in buf, or
 * END_NOT_FOUND. States: 0 frame start -> 1/4; 1 first sequence extension
 * -> 0/2; 2 first field start -> 3/0; 3 second sequence extension -> 2/0;
 * 4 searching the end. */
static int mpeg1_find_frame_end(ParseContext *pc, const uint8_t *buf, int buf_size)
{
    uint32_t state = pc->state;
    int i;

    /* the end of the stream ends a frame */
    if (buf_size == 0)
        return 0;

    for (i = 0; i < buf_size; i++) {
        if (pc->frame_start_found & 1) {
            if (state == EXT_START_CODE && (buf[i] & 0xF0) != 0x80)
                pc->frame_start_found--;
            else if (state == EXT_START_CODE + 2) {
                if ((buf[i] & 3) == 3)
                    pc->frame_start_found = 0;
                else
                    pc->frame_start_found = (pc->frame_start_found + 1) & 3;
            }
            state++;
        } else {
            i = find_start_code(buf + i, buf + buf_size, &state) - buf - 1;
            if (pc->frame_start_found == 0 && state >= SLICE_MIN_START_CODE && state <= SLICE_MAX_START_CODE) {
                i++;
                pc->frame_start_found = 4;
            }
            if (state == SEQ_END_CODE) {
                pc->frame_start_found = 0;
                pc->state = -1;
                return i + 1;
            }
            if (pc->frame_start_found == 2 && state == SEQ_START_CODE)
                pc->frame_start_found = 0;
            if (pc->frame_start_found  < 4 && state == EXT_START_CODE)
                pc->frame_start_found++;
            if (pc->frame_start_found == 4 && (state & 0xFFFFFF00) == 0x100) {
                if (state < SLICE_MIN_START_CODE || state > SLICE_MAX_START_CODE) {
                    pc->frame_start_found = 0;
                    pc->state             = -1;
                    return i - 3;
                }
            }
        }
    }
    pc->state = state;
    return END_NOT_FOUND;
}

/* mpegvideo_parse (the headers it reads after the cut decide nothing here) */
static int mpegvideo_parse(DrStock *s, const uint8_t **out, int *out_size, const uint8_t *buf, int buf_size)
{
    ParseContext *pc = &s->pc;
    int next = mpeg1_find_frame_end(pc, buf, buf_size), ret;

    if ((ret = combine_frame(pc, next, &buf, &buf_size)) < 0) {
        *out      = NULL;
        *out_size = 0;
        return ret == DR_ERR_NOMEM ? ret : buf_size;
    }
    *out      = buf;
    *out_size = buf_size;
    return next;
}

/* ---- AC-3 / E-AC-3 (aac_ac3_parser.c, ac3_parser.c, ac3tab.c) ---- */

#define AC3_HEADER_SIZE 7
#define EAC3_MAX_CHANNELS 16

enum {
    EAC3_FRAME_TYPE_INDEPENDENT = 0,
    EAC3_FRAME_TYPE_DEPENDENT,
    EAC3_FRAME_TYPE_AC3_CONVERT,
    EAC3_FRAME_TYPE_RESERVED,
};

/* ff_ac3_frame_size_tab: frame sizes in 16-bit words by frame size code and
 * sample rate code */
static const uint16_t frame_size_tab[38][3] = {
    { 64,   69,   96   }, { 64,   70,   96   }, { 80,   87,   120  }, { 80,   88,   120  },
    { 96,   104,  144  }, { 96,   105,  144  }, { 112,  121,  168  }, { 112,  122,  168  },
    { 128,  139,  192  }, { 128,  140,  192  }, { 160,  174,  240  }, { 160,  175,  240  },
    { 192,  208,  288  }, { 192,  209,  288  }, { 224,  243,  336  }, { 224,  244,  336  },
    { 256,  278,  384  }, { 256,  279,  384  }, { 320,  348,  480  }, { 320,  349,  480  },
    { 384,  417,  576  }, { 384,  418,  576  }, { 448,  487,  672  }, { 448,  488,  672  },
    { 512,  557,  768  }, { 512,  558,  768  }, { 640,  696,  960  }, { 640,  697,  960  },
    { 768,  835,  1152 }, { 768,  836,  1152 }, { 896,  975,  1344 }, { 896,  976,  1344 },
    { 1024, 1114, 1536 }, { 1024, 1115, 1536 }, { 1152, 1253, 1728 }, { 1152, 1254, 1728 },
    { 1280, 1393, 1920 }, { 1280, 1394, 1920 },
};

static const int sample_rate_tab[] = { 48000, 44100, 32000, 0 };
static const uint8_t eac3_blocks[4] = { 1, 2, 3, 6 };

/* ff_eac3_custom_channel_map_locations: the channels each bit of a
 * dependent frame's channel map stands for */
static const uint8_t channel_map_channels[16] = { 1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1, 2, 1, 1 };

/* FFmpeg's bit reader with its safe-reader rule: reads past the end come
 * from the bytes after it (the padding), the position stops 8 bits past
 * the end. */
typedef struct Bits {
    const uint8_t *b;
    int index, limit;
} Bits;

static unsigned get_bits(Bits *g, int n)
{
    unsigned v = (dr_rb32(g->b + (g->index >> 3)) << (g->index & 7)) >> (32 - n);
    g->index = DR_MIN(g->limit, g->index + n);
    return v;
}

static void skip_bits(Bits *g, int n)
{
    g->index = DR_MIN(g->limit, g->index + n);
}

typedef struct Header {
    int bitstream_id, frame_type, substreamid, frame_size;
    int sr_code, sample_rate, num_blocks, channel_mode, lfe_on;
} Header;

/* ff_ac3_parse_header over bits bits of b (the bitstream information it
 * reads as far as it can fail): 0, or < 0 not a header. */
static int parse_header(const uint8_t *b, int bits, Header *h)
{
    Bits g = { b, 0, bits + 8 };

    memset(h, 0, sizeof(*h));
    if (get_bits(&g, 16) != 0x0B77)
        return -1;
    /* read ahead to bsid to tell AC-3 from E-AC-3 */
    {
        Bits t = g;
        unsigned v = get_bits(&t, 16) << 13;
        v |= get_bits(&t, 13);
        h->bitstream_id = v & 0x1F;
    }
    if (h->bitstream_id > 16)
        return -1;
    h->num_blocks = 6;

    if (h->bitstream_id <= 10) {
        int frame_size_code;
        skip_bits(&g, 16);                  /* crc1 */
        h->sr_code = get_bits(&g, 2);
        if (h->sr_code == 3)
            return -1;
        frame_size_code = get_bits(&g, 6);
        if (frame_size_code > 37)
            return -1;
        h->sample_rate = sample_rate_tab[h->sr_code] >> (DR_MAX(h->bitstream_id, 8) - 8);
        h->frame_size  = frame_size_tab[frame_size_code][h->sr_code] * 2;
        h->frame_type  = EAC3_FRAME_TYPE_AC3_CONVERT;
        /* the rest of the AC-3 bitstream information fails nothing */
        return 0;
    }

    /* E-AC-3 */
    h->frame_type = get_bits(&g, 2);
    if (h->frame_type == EAC3_FRAME_TYPE_RESERVED)
        return -1;
    h->substreamid = get_bits(&g, 3);
    h->frame_size  = (get_bits(&g, 11) + 1) << 1;
    if (h->frame_size < AC3_HEADER_SIZE)
        return -1;
    h->sr_code = get_bits(&g, 2);
    if (h->sr_code == 3) {
        int sr_code2 = get_bits(&g, 2);
        if (sr_code2 == 3)
            return -1;
        h->sample_rate = sample_rate_tab[sr_code2] / 2;
    } else {
        h->num_blocks  = eac3_blocks[get_bits(&g, 2)];
        h->sample_rate = sample_rate_tab[h->sr_code];
    }
    h->channel_mode = get_bits(&g, 3);
    h->lfe_on       = get_bits(&g, 1);

    /* eac3_parse_header, as far as it can fail */
    if (h->substreamid)
        return -1;
    skip_bits(&g, 5);                       /* bsid */
    for (int i = 0; i < (h->channel_mode ? 1 : 2); i++) {
        skip_bits(&g, 5);                   /* dialnorm */
        if (get_bits(&g, 1))                /* compre */
            skip_bits(&g, 8);
    }
    if (h->frame_type == EAC3_FRAME_TYPE_DEPENDENT && get_bits(&g, 1)) {
        unsigned channel_map = get_bits(&g, 16);
        int channels = 0;
        for (int i = 0; i < 16; i++)
            if (channel_map & (1 << (EAC3_MAX_CHANNELS - i - 1)))
                channels += channel_map_channels[i];
        if (channels > EAC3_MAX_CHANNELS)
            return -1;
    }
    return 0;
}

/* ac3_sync: the frame size when the 8 bytes in state end a valid header
 * (0 none); a dependent frame starts no new unit. */
static int ac3_sync(uint64_t state, int *need_next_header, int *new_frame_start)
{
    uint8_t tmp[8 + DR_BUF_PADDING] = { 0 };
    Header h;

    for (int k = 0; k < 8; k++)
        tmp[k] = state >> (56 - 8 * k);
    if (tmp[1] == 0x77 && tmp[2] == 0x0b) {     /* byte-swapped */
        uint8_t t;
        t = tmp[1]; tmp[1] = tmp[2]; tmp[2] = t;
        t = tmp[3]; tmp[3] = tmp[4]; tmp[4] = t;
        t = tmp[5]; tmp[5] = tmp[6]; tmp[6] = t;
    }
    if (parse_header(tmp + 8 - AC3_HEADER_SIZE, 54, &h) < 0)
        return 0;
    *new_frame_start  = h.frame_type != EAC3_FRAME_TYPE_DEPENDENT;
    *need_next_header = *new_frame_start || h.frame_type != EAC3_FRAME_TYPE_AC3_CONVERT;
    return h.frame_size;
}

/* ff_ac3_find_syncword */
static int find_syncword(const uint8_t *buf, int buf_size)
{
    int i;

    for (i = 1; i < buf_size; i += 2) {
        if (buf[i] == 0x77 || buf[i] == 0x0B) {
            if ((buf[i] ^ buf[i - 1]) == (0x77 ^ 0x0B)) {
                i--;
                break;
            } else if ((buf[i] ^ buf[i + 1]) == (0x77 ^ 0x0B)) {
                break;
            }
        }
    }
    return i >= buf_size ? -1 : i;
}

/* ff_aac_ac3_parse, AC-3 */
static int ac3_parse(DrStock *s, const uint8_t **out, int *out_size, const uint8_t *buf, int buf_size)
{
    ParseContext *pc = &s->pc;
    int len, i, new_frame_start = 0, got_frame = 0, ret;

get_next:
    i = END_NOT_FOUND;
    if (s->remaining_size <= buf_size) {
        if (s->remaining_size && !s->need_next_header) {
            i = s->remaining_size;
            s->remaining_size = 0;
        } else {                            /* a header first */
            len = 0;
            for (i = s->remaining_size; i < buf_size; i++) {
                s->state = (s->state << 8) + buf[i];
                if ((len = ac3_sync(s->state, &s->need_next_header, &new_frame_start)))
                    break;
            }
            if (len <= 0) {
                i = END_NOT_FOUND;
            } else {
                got_frame = 1;
                s->state = 0;
                i -= AC3_HEADER_SIZE - 1;
                s->remaining_size = len;
                if (!new_frame_start || pc->index + i <= 0) {
                    s->remaining_size += i;
                    goto get_next;
                } else if (i < 0) {
                    s->remaining_size += i;
                }
            }
        }
    }

    if ((ret = combine_frame(pc, i, &buf, &buf_size)) < 0) {
        if (ret == DR_ERR_NOMEM)
            return ret;
        s->remaining_size -= DR_MIN(s->remaining_size, buf_size);
        *out      = NULL;
        *out_size = 0;
        return buf_size;
    }

    *out      = buf;
    *out_size = buf_size;

    if (got_frame) {
        /* the unit's last frame: its rate and samples when its CRC is good */
        Header h = { 0 };
        int offset = find_syncword(buf, buf_size);

        if (offset < 0)
            return i;
        buf      += offset;
        buf_size -= offset;
        while (buf_size > 0) {
            if (parse_header(buf, buf_size * 8, &h) < 0 || h.frame_size > buf_size)
                return i;
            if (buf_size > h.frame_size) {
                buf      += h.frame_size;
                buf_size -= h.frame_size;
                continue;
            }
            if (dr_crc16_ansi(0, buf + 2, h.frame_size - 2))
                return i;
            break;
        }
        s->rate     = h.sample_rate;
        s->duration = h.num_blocks * 256;
    }
    return i;
}

/* ---- the host (av_parser_parse2) ---- */

DrStock *dr_stock_new(enum DrStockCodec codec)
{
    DrStock *s = dr_mallocz(sizeof(*s));

    if (s)
        s->codec = codec;
    return s;
}

void dr_stock_free(DrStock **ps)
{
    if (!*ps)
        return;
    dr_free((*ps)->pc.buffer);
    dr_freep(ps);
}

int dr_stock_parse(DrStock *s, const uint8_t *data, int size, int64_t pos,
                   const uint8_t **out, int *out_size, int64_t *offset, int *samples, int *rate)
{
    static const uint8_t dummy[DR_BUF_PADDING];
    int index;

    if (!s->fetched) {
        s->next_frame_offset =
        s->cur_offset        = pos;
        s->fetched           = 1;
    }
    if (size == 0)
        data = dummy;       /* padding is needed even at the end */

    *out      = NULL;
    *out_size = 0;
    index = s->codec == DR_STOCK_MPEGVIDEO ? mpegvideo_parse(s, out, out_size, data, size)
                                           : ac3_parse(s, out, out_size, data, size);
    if (index == DR_ERR_NOMEM)
        return index;

    if (*out_size) {
        s->frame_offset      = s->next_frame_offset;
        s->next_frame_offset = s->cur_offset + index;
    } else {
        *out = NULL;
    }
    if (index < 0)
        index = 0;
    s->cur_offset += index;

    *offset  = s->frame_offset;
    *samples = s->duration;
    *rate    = s->rate;
    return index;
}
