/*
 * disc-remuxer: subtitles tied to their pictures in the MKV that mkvmerge
 * makes from the stream files (see subtie.h).
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/bytes.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"
#include "timeline/disctl.h"
#include "subtie.h"

/* a subtitle this close to a picture's field (one 90 kHz tick) is on it */
#define TOLERANCE (DR_TICKS_PER_SECOND / 90000)

typedef struct Pic {
    int64_t time, len;               /* ticks; len 0: not known */
    int     field_picture;
} Pic;

struct SubTie {
    Pic     *pic;
    size_t   n, cap;
    /* subtie_clock() */
    int64_t  field;                  /* one field, ticks */
    int64_t  num, den;
    int64_t *ns;                     /* each picture's time on mkvmerge's clock */
    int     *fields;                 /* each picture's fields on the disc */
};

/* mkvmerge (MKVToolNix 103) times the pictures of a stream file without
 * times by their lengths, added up in display order:
 * - MPEG-1/2: the picture's fields (2; 3 with repeat_first_field; 4 or 6
 *   when a progressive sequence repeats the frame) x 1e9 / (2 x the frame
 *   rate of frame_rate_code), cut; a field pair is one picture;
 * - H.264: 1e9 x num_units_in_tick / time_scale (one field) cut, twice for
 *   a frame picture; pic_struct is not read, so a picture is only counted
 *   right when it lasts its two fields (a field picture its one);
 * - HEVC: 1e9 x num_units_in_tick / time_scale (one picture) cut, whatever
 *   the picture's pic_struct.
 * VC-1 is timed in stream order and a skipped frame's repeated field is not
 * read: not covered. */
int64_t subtie_picture_ns(int codec, int num, int den, int field_picture, int fields)
{
    if (num <= 0 || den <= 0)
        return -1;
    switch (codec) {
    case DR_CODEC_MPEG1VIDEO:
    case DR_CODEC_MPEG2VIDEO:
        if (field_picture || (fields != 2 && fields != 3 && fields != 4 && fields != 6))
            return -1;
        return 1000000000LL * fields * den / (2 * (int64_t)num);
    case DR_CODEC_H264:
        if (fields != (field_picture ? 1 : 2))
            return -1;
        return 1000000000LL * den / (2 * (int64_t)num) * fields;
    case DR_CODEC_HEVC:
        if (field_picture || fields != 2)
            return -1;
        return 1000000000LL * den / num;
    default:
        return -1;
    }
}

static int64_t ms_of(int64_t ns)
{
    return (ns + 500000) / 1000000;
}

int64_t subtie_sup_ms(int64_t tick)
{
    return ms_of(tick * 100000 / 9);
}

SubTie *subtie_alloc(void)
{
    return dr_calloc(1, sizeof(SubTie));
}

void subtie_free(SubTie **sp)
{
    SubTie *s = *sp;

    if (!s)
        return;
    dr_free(s->pic);
    dr_free(s->ns);
    dr_free(s->fields);
    dr_free(s);
    *sp = NULL;
}

int subtie_video_frame(SubTie *s, int64_t time, int64_t duration, int pictures)
{
    if (pictures < 1)
        pictures = 1;
    if (s->n + pictures > s->cap) {
        size_t cap = s->cap ? 2 * s->cap : 65536;
        Pic *p = dr_realloc_array(s->pic, cap, sizeof(*p));
        if (!p)
            return DR_ERR(ENOMEM);
        s->pic = p;
        s->cap = cap;
    }
    for (int k = 0; k < pictures; k++)
        s->pic[s->n++] = (Pic){ time + duration * k / pictures, duration > 0 ? duration / pictures : 0,
                                pictures > 1 };
    return 0;
}

static int cmp_pic(const void *a, const void *b)
{
    int64_t x = ((const Pic *)a)->time, y = ((const Pic *)b)->time;
    return x < y ? -1 : x > y;
}

static const char *codec_words(int codec)
{
    return codec == DR_CODEC_MPEG1VIDEO ? "MPEG-1" : codec == DR_CODEC_MPEG2VIDEO ? "MPEG-2" :
           codec == DR_CODEC_H264 ? "H.264" : codec == DR_CODEC_HEVC ? "HEVC" : codec == DR_CODEC_VC1 ? "VC-1" :
           "this codec";
}

/* A clock that cannot be: nothing is placed on it. */
static int refused(SubTie *s)
{
    dr_free(s->ns);
    dr_free(s->fields);
    s->ns     = NULL;
    s->fields = NULL;
    return 0;
}

int subtie_clock(SubTie *s, int codec, int num, int den, char *why, size_t why_size)
{
    int64_t t = 0;

    *why = 0;
    if (codec == DR_CODEC_VC1) {
        snprintf(why, why_size, "mkvmerge times VC-1 in stream order and misses the repeated field of a skipped "
                 "frame, so its picture times are not counted on here");
        return 0;
    }
    if (codec != DR_CODEC_MPEG1VIDEO && codec != DR_CODEC_MPEG2VIDEO && codec != DR_CODEC_H264 &&
        codec != DR_CODEC_HEVC) {
        snprintf(why, why_size, "how mkvmerge times %s video is not built in", codec_words(codec));
        return 0;
    }
    if (!s->n) {
        snprintf(why, why_size, "the title has no video picture");
        return 0;
    }
    if (num <= 0 || den <= 0 || (DR_TICKS_PER_SECOND * den) % (2 * (int64_t)num)) {
        snprintf(why, why_size, "the video's frame rate (%d/%d) is not known as whole fields", num, den);
        return 0;
    }
    s->num   = num;
    s->den   = den;
    s->field = DR_TICKS_PER_SECOND * den / (2 * (int64_t)num);
    /* the pictures in display order (the frames came in decode order) */
    qsort(s->pic, s->n, sizeof(*s->pic), cmp_pic);
    dr_free(s->ns);
    dr_free(s->fields);
    s->ns     = dr_malloc_array(s->n, sizeof(*s->ns));
    s->fields = dr_malloc_array(s->n, sizeof(*s->fields));
    if (!s->ns || !s->fields)
        return DR_ERR(ENOMEM);
    for (size_t k = 0; k < s->n; k++) {
        /* a picture lasts until the next one; the last one its own length
         * (not known: as long as the one before it) */
        int64_t len = k + 1 < s->n ? s->pic[k + 1].time - s->pic[k].time : s->pic[k].len, ns;
        int fields;

        s->ns[k] = t;
        if (k + 1 == s->n && len <= 0) {
            s->fields[k] = k ? s->fields[k - 1] : 2;
            break;
        }
        if (len <= 0 || len % s->field) {
            snprintf(why, why_size, "the picture at %.3f s lasts %.3f ms, not a whole number of fields at %d/%d fps",
                     s->pic[k].time / (double)DR_TICKS_PER_SECOND, len * 1000.0 / DR_TICKS_PER_SECOND, num, den);
            return refused(s);
        }
        fields = (int)(len / s->field);
        if ((ns = subtie_picture_ns(codec, num, den, s->pic[k].field_picture, fields)) < 0) {
            snprintf(why, why_size, "the %s at %.3f s lasts %d field%s, which mkvmerge does not count as the disc "
                     "does for %s video", s->pic[k].field_picture ? "field picture" : "picture",
                     s->pic[k].time / (double)DR_TICKS_PER_SECOND, fields, fields == 1 ? "" : "s", codec_words(codec));
            return refused(s);
        }
        s->fields[k] = fields;
        t += ns;
    }
    return 1;
}

int subtie_place(const SubTie *s, int64_t t, int64_t *ms)
{
    size_t lo = 0, hi = s->n;
    int64_t off, j;

    if (!s->ns || !s->n)
        return -1;
    /* the last picture starting at or before t */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->pic[mid].time <= t + TOLERANCE)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return -1;
    lo--;
    off = t - s->pic[lo].time;
    j   = (off + s->field / 2) / s->field;
    if (off < -TOLERANCE || DR_ABS(off - j * s->field) > TOLERANCE || j >= s->fields[lo])
        return -1;
    /* its fields into the picture: their length on the same clock (cut) */
    *ms = ms_of(s->ns[lo] + j * 1000000000LL * s->den / (2 * s->num));
    return (int)j;
}

int subtie_sup(const char *in, const char *out, const int64_t *pos, const int64_t *size, const int64_t *ms,
               size_t n)
{
    FILE *fi = fopen(in, "rb"), *fo = fi ? fopen(out, "wb") : NULL;
    uint8_t *buf = NULL;
    int64_t at = 0, cap = 0;
    int ret = 0;

    if (!fi || !fo) {
        ret = DR_ERR(errno);
        goto end;
    }
    for (size_t i = 0; i < n && ret >= 0; i++) {
        int64_t off = 0;

        if (pos[i] != at || size[i] <= 0) {
            ret = DR_ERR_INVALID;
            break;
        }
        if (size[i] > cap) {
            uint8_t *b = realloc(buf, size[i]);
            if (!b) {
                ret = DR_ERR(ENOMEM);
                break;
            }
            buf = b;
            cap = size[i];
        }
        if (fread(buf, 1, size[i], fi) != (size_t)size[i]) {
            ret = ferror(fi) ? DR_ERR(errno) : DR_ERR_INVALID;
            break;
        }
        /* each segment: "PG", its time (32 bits), a decode time, then the
         * segment (type, 16-bit length, data) */
        while (off < size[i]) {
            if (off + 13 > size[i] || buf[off] != 'P' || buf[off + 1] != 'G') {
                ret = DR_ERR_INVALID;
                break;
            }
            if (ms[i] >= 0)
                dr_wb32(buf + off + 2, (uint32_t)(ms[i] * 90));
            off += 13 + dr_rb16(buf + off + 11);
        }
        if (ret >= 0 && off != size[i])
            ret = DR_ERR_INVALID;
        if (ret >= 0 && fwrite(buf, 1, size[i], fo) != (size_t)size[i])
            ret = DR_ERR(errno);
        at += size[i];
    }
    if (ret >= 0 && fgetc(fi) != EOF)
        ret = DR_ERR_INVALID;               /* the sets do not cover the file */
end:
    free(buf);
    if (fi)
        fclose(fi);
    if (fo && fclose(fo) != 0 && ret >= 0)
        ret = DR_ERR(errno);
    if (ret < 0 && fo)
        unlink(out);
    return ret;
}

int subtie_idx(const char *in, const char *out, const int64_t *ms, size_t n)
{
    static const char key[] = "timestamp: ", pos[] = ", filepos:";
    FILE *fi = fopen(in, "rb"), *fo = fi ? fopen(out, "wb") : NULL;
    char line[4096];
    size_t i = 0;
    int ret = 0;

    if (!fi || !fo) {
        ret = DR_ERR(errno);
        goto end;
    }
    while (ret >= 0 && fgets(line, sizeof(line), fi)) {
        const char *rest;
        int64_t m;

        if (strncmp(line, key, sizeof(key) - 1)) {
            if (fputs(line, fo) < 0)
                ret = DR_ERR(errno);
            continue;
        }
        if (i == n || !(rest = strstr(line, pos))) {
            ret = DR_ERR_INVALID;
            break;
        }
        m = ms[i++];
        if (m < 0) {
            if (fputs(line, fo) < 0)
                ret = DR_ERR(errno);
            continue;
        }
        if (fprintf(fo, "%s%02"PRId64":%02"PRId64":%02"PRId64":%03"PRId64"%s", key, m / 3600000, m / 60000 % 60,
                    m / 1000 % 60, m % 1000, rest) < 0)
            ret = DR_ERR(errno);
    }
    if (ret >= 0 && ferror(fi))
        ret = DR_ERR(EIO);
    if (ret >= 0 && i != n)
        ret = DR_ERR_INVALID;               /* not one line per unit */
end:
    if (fi)
        fclose(fi);
    if (fo && fclose(fo) != 0 && ret >= 0)
        ret = DR_ERR(errno);
    if (ret < 0 && fo)
        unlink(out);
    return ret;
}

int subtie_link_or_copy(const char *in, const char *out)
{
    FILE *fi, *fo;
    char buf[1 << 16];
    size_t got;
    int ret = 0;

    if (unlink(out) < 0 && errno != ENOENT)
        return DR_ERR(errno);
    if (link(in, out) == 0)
        return 0;
    if (!(fi = fopen(in, "rb")))
        return DR_ERR(errno);
    if (!(fo = fopen(out, "wb"))) {
        ret = DR_ERR(errno);
        fclose(fi);
        return ret;
    }
    while ((got = fread(buf, 1, sizeof(buf), fi)) > 0)
        if (fwrite(buf, 1, got, fo) != got) {
            ret = DR_ERR(errno);
            break;
        }
    if (ret >= 0 && ferror(fi))
        ret = DR_ERR(EIO);
    fclose(fi);
    if (fclose(fo) != 0 && ret >= 0)
        ret = DR_ERR(errno);
    if (ret < 0)
        unlink(out);
    return ret;
}
