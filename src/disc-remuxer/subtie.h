/*
 * disc-remuxer: subtitles tied to their pictures in the MKV that mkvmerge
 * makes from the stream files (output.format = mkvmerge, setting
 * mkv.tie_subtitles).
 *
 * On a disc every subtitle starts on a picture, or on a later field of one
 * (a 29.97 frame of the disc's signal inside a picture of film). mkvmerge
 * gives the pictures of a stream file (which carries no times) times of its
 * own: each picture's length cut to whole ns, added up, every time then
 * rounded to the ms (halves up). Its clock runs a little early, so where a
 * picture falls on a half ms the picture rounds down and its subtitle, from
 * the disc's exact time, up: 1 ms after its picture, shown one picture late.
 * The copy of a subtitle file mkvmerge is given carries instead the ms its
 * clock gives the subtitle's picture (plus the fields into the picture on
 * the same clock). The subtitle data is not touched, the stream files keep
 * the disc's times, and an MKV rebuilt from what is extracted from it
 * counts the same.
 */

#ifndef DISC_REMUXER_SUBTIE_H
#define DISC_REMUXER_SUBTIE_H

#include <stddef.h>
#include <stdint.h>

/* The length mkvmerge gives a picture of a stream file (ns): codec
 * DR_CODEC_*, the stream's frame rate num / den, a field picture (H.264:
 * one of two in a frame) or a frame, lasting `fields` fields on the disc.
 * -1 when mkvmerge's count of such a picture is not the disc's or the codec
 * is not covered (VC-1: counted in stream order, a skipped frame's repeated
 * field missed; H.264 / HEVC pictures lasting other than their fields:
 * pic_struct is not read). */
int64_t subtie_picture_ns(int codec, int num, int den, int field_picture, int fields);

/* The video's pictures, collected as the title is read (times in
 * DR_TICKS_PER_SECOND ticks), then put on mkvmerge's clock. */
typedef struct SubTie SubTie;

SubTie *subtie_alloc(void);
void    subtie_free(SubTie **s);

/* A frame of the video given to mkvmerge: its time and length, and the
 * pictures it holds (H.264: two field pictures, each half of it). */
int subtie_video_frame(SubTie *s, int64_t time, int64_t duration, int pictures);

/* After the title's last frame: every picture on mkvmerge's clock. 1: done;
 * 0: not for this video (why says why); a negative error. */
int subtie_clock(SubTie *s, int codec, int num, int den, char *why, size_t why_size);

/* The ms mkvmerge's clock gives a subtitle at time t (ticks): its picture's
 * time, plus the fields it starts into the picture. Returns those fields
 * (0: on the picture), or -1 when t is on no field of a picture (*ms
 * untouched). */
int subtie_place(const SubTie *s, int64_t t, int64_t *ms);

/* The ms mkvmerge gives a PGS display set whose .sup time is tick (90 kHz):
 * tick x 100000 / 9 cut to whole ns, rounded to the ms (halves up). */
int64_t subtie_sup_ms(int64_t tick);

/* A tied copy of a .sup: n display sets in file order, set i at pos[i]
 * (size[i] bytes, each segment after a "PG" header) gets ms[i] x 90 as its
 * time; ms[i] < 0 keeps its time. The sets must cover the file exactly. 0
 * or a negative error (out removed). */
int subtie_sup(const char *in, const char *out, const int64_t *pos, const int64_t *size, const int64_t *ms,
               size_t n);

/* A tied copy of a VobSub index: its n "timestamp:" lines in order get
 * ms[i] (ms[i] < 0 keeps the line), every other line as it is. 0 or a
 * negative error (out removed). */
int subtie_idx(const char *in, const char *out, const int64_t *ms, size_t n);

/* The .sub next to a tied .idx: a hard link to the stream file's, else a
 * copy. 0 or a negative error. */
int subtie_link_or_copy(const char *in, const char *out);

#endif
