/*
 * disc-remuxer: the stream files of a title (see writer.h). Each track's
 * packets back to back in a file named <name>_<NN>_[<lang>]_<codec>
 * [_<channel layout>][ (forced)][ (commentary)][ (visually impaired)]
 * [ DELAY <ms>ms].<ext> (the language between brackets and the words in
 * parentheses as the MKVToolNix GUI reads a file name: it splits it at
 * [ ] ( ) { } . + - = #, and finds the delay as "delay <number>");
 * PCM as RIFF WAVE; sub-pictures
 * as a VobSub pair (.sub packs + .idx); PGS as .sup; text subtitles as
 * .textst (MKVToolNix's layout); the chapters as Matroska XML.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>

#include "base/err.h"
#include "base/log.h"
#include "io/discio.h"
#include "base/msg.h"
#include "names.h"
#include "analyze/details.h"
#include "disc/disc.h"
#include "emit.h"
#include "engine.h"
#include "events.h"
#include "base/settings.h"
#include "writer.h"

/* Times on the title timeline (DR_TICKS_PER_SECOND ticks) in other units,
 * rounded to nearest as before. */
#define TICKS_TB   (AVRational){ 1, DR_TICKS_PER_SECOND }
#define MS_TB      (AVRational){ 1, 1000 }
#define PTS90_TB   (AVRational){ 1, 90000 }

/* One stream's result (times in ms on the title timeline). */
typedef struct StreamResult {
    int         index;
    const char *kind, *codec, *lang, *file;     /* file "" = not written */
    int64_t     packets, bytes;
    int64_t     first_ms, end_ms;
    int64_t     delay_ms;                       /* audio: the start delay in the file name */
} StreamResult;

/* The file extension and the codec's short name in file names. */
static const char *es_extension(int id)
{
    switch (id) {
    case AV_CODEC_ID_MPEG1VIDEO:   return "m1v";
    case AV_CODEC_ID_MPEG2VIDEO:   return "m2v";
    case AV_CODEC_ID_VC1:          return "vc1";
    case AV_CODEC_ID_H264:         return "h264";
    case AV_CODEC_ID_HEVC:         return "hevc";
    case AV_CODEC_ID_AC3:          return "ac3";
    case AV_CODEC_ID_EAC3:         return "eac3";
    case AV_CODEC_ID_TRUEHD:       return "thd";
    case AV_CODEC_ID_MLP:          return "mlp";
    case AV_CODEC_ID_DTS:          return "dts";
    case AV_CODEC_ID_MP1:          return "mp1";
    case AV_CODEC_ID_MP2:          return "mp2";
    case AV_CODEC_ID_MP3:          return "mp3";
    case AV_CODEC_ID_AAC:          return "aac";
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:    return "wav";
    case AV_CODEC_ID_DVD_SUBTITLE: return "sub";
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE: return "sup";
    case AV_CODEC_ID_HDMV_TEXT_SUBTITLE: return "textst";
    case AV_CODEC_ID_SUBRIP:       return "srt";
    default:                       return "bin";
    }
}

const char *track_extension(const DiscStreamInfo *k)
{
    if (k->second_view)
        return "mvc";
    switch (k->codec) {
    case AV_CODEC_ID_PCM_DVD:
    case AV_CODEC_ID_PCM_BLURAY:   return "wav";   /* LPCM is written as PCM in a WAVE file */
    case AV_CODEC_ID_EIA_608:      return "srt";   /* closed captions made into text */
    default:                       return es_extension(k->codec);
    }
}

const char *codec_label(int id, int profile)
{
    switch (id) {
    case AV_CODEC_ID_MPEG1VIDEO:   return "Mpeg1";
    case AV_CODEC_ID_MPEG2VIDEO:   return "Mpeg2";
    case AV_CODEC_ID_VC1:          return "VC-1";
    case AV_CODEC_ID_H264:         return "Mpeg4";
    case AV_CODEC_ID_HEVC:         return "MpegH";
    case AV_CODEC_ID_AC3:          return "DD";
    case AV_CODEC_ID_EAC3:         return "DDplus";
    case AV_CODEC_ID_TRUEHD:       return profile == AV_PROFILE_TRUEHD_ATMOS ? "TrueHD Atmos" : "TrueHD";
    case AV_CODEC_ID_MLP:          return "MLP";
    case AV_CODEC_ID_DTS:          return profile == AV_PROFILE_DTS_HD_MA || profile == AV_PROFILE_DTS_HD_MA_X ||
                                          profile == AV_PROFILE_DTS_HD_MA_X_IMAX ? "DTS-HD MA" :
                                          profile == AV_PROFILE_DTS_HD_HRA ? "DTS-HD HR" : "DTS";
    case AV_CODEC_ID_MP1:
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:          return "MPEG audio";
    case AV_CODEC_ID_AAC:          return "AAC";
    case AV_CODEC_ID_PCM_DVD:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_BLURAY:   return "LPCM";
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:  return "PGS";
    case AV_CODEC_ID_HDMV_TEXT_SUBTITLE: return "TextST";
    case AV_CODEC_ID_DVD_SUBTITLE: return "VobSub";
    case AV_CODEC_ID_SUBRIP:       return "CC";      /* closed captions made into text */
    default:                       return avcodec_get_name(id);
    }
}

/* A 3-D video's second view (its pictures are decoded with the base
 * view's): MVC in names and logs, no read-back check of its own. */
static const char *stream_label(const DrDiscTrack *t, int profile)
{
    return t->second_view ? "MVC" : codec_label(t->codec, profile);
}

#define HOLD_MAX 64   /* audio packets kept while their channel layout is not known yet */

typedef struct EsOut {
    StreamResult st;
    char         *name;
    FILE         *f;
    FILE         *idx;              /* sub-pictures: the VobSub index next to the .sub */
    int           textst;           /* text subtitles: the number of dialogs to fill in at the end */
    int64_t       dialogs;          /* text subtitles: dialogs written */
    int           no_forced;        /* a forced-only track that showed nothing: its file removed */
    int64_t       sub_pos;          /* bytes written to the .sub */
    int64_t       cues;             /* captions: SRT cues written */
    int           wav;              /* a WAV header to complete at the end */
    int           opened;           /* the file name is chosen */
    int           profile;          /* the codec profile the name was chosen with (e.g. TrueHD + Atmos) */
    char            layout[64];     /* audio: the channel layout in the name ("" unknown) */
    int             layout_done;    /* a frame was decoded (or decoding gave up) */
    DrLayoutProbe  *lp;             /* the decoder that gives the layout (analyze/details.h) */
    DrDiscFrame  *hold;             /* frames held (their references ours) */
    int           nb_hold;
    DetailsProbe *probe;            /* video / audio: the disc stream's details for the end check */
    int           probe_late;       /* LPCM: the probe opens with the first packet (the stream's format is
                                       the converted PCM's from then on) */
    int           checked;          /* the end check ran: 1 pass, -1 fail, 0 not run */
    int           write_err;        /* a write to its file failed: why (errno), for the title's failure line */
    char          problems[1024];   /* its findings */
} EsOut;

/* The end check of a written file: its details (read back with FFmpeg's
 * decoders) against the disc stream's, read while ripping. */
static void end_check(const DrDiscTrack *t, EsOut *o)
{
    Details disc, file;
    int n, ret_file;

    if (!o->probe || !o->opened)
        return;
    details_probe_get(o->probe, &disc);
    if (!disc.valid) {
        o->checked = -1;
        snprintf(o->problems, sizeof(o->problems), "the disc stream's first frame could not be decoded");
        return;
    }
    ret_file = details_of_file(o->name, t->type, &file);
    if (ret_file < 0 || !file.valid) {
        o->checked = -1;
        snprintf(o->problems, sizeof(o->problems), "the written file cannot be read back");
        return;
    }
    n = details_compare(&disc, &file, o->problems, sizeof(o->problems));
    o->checked = n ? -1 : 1;
    if (!n)
        snprintf(o->problems, sizeof(o->problems), "%d fields", disc.n);
}

/* A write to o's file failed: the reason (errno) kept, so the title's
 * failure names the file and why (a full disk stops the rip). */
static int write_failed(EsOut *o)
{
    o->write_err = errno ? errno : EIO;
    return DR_ERR(o->write_err);
}

/* Closes a stream file: a write that failed before (fputs, fprintf and
 * buffered writes report only here) or the close itself counts. */
static int out_close(EsOut *o, FILE **f)
{
    int bad = ferror(*f), err = bad ? errno : 0;

    if (fclose(*f) && !bad) {
        bad = 1;
        err = errno;
    }
    *f = NULL;
    if (!bad)
        return 0;
    o->write_err = err ? err : EIO;
    return DR_ERR(o->write_err);
}

static void put_le(FILE *f, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        fputc((v >> (8 * i)) & 0xFF, f);
}

/* RIFF WAVE header: WAVE_FORMAT_PCM for up to 2 channels of 16 bits, else
 * WAVE_FORMAT_EXTENSIBLE with the channel mask and the bits that carry the
 * sound (20-bit samples ride in 24-bit words: 20 of 24 valid); sizes filled
 * in at the end. */
static void wav_header(FILE *f, const DrDiscTrack *t, int64_t data)
{
    int ch = t->channels, bits = t->codec == AV_CODEC_ID_PCM_S24LE ? 24 : 16;
    int valid = t->bits > 0 && t->bits < bits ? t->bits : bits;
    int ext = ch > 2 || bits > 16, fmt = ext ? 40 : 16;
    uint64_t riff = 4 + 8 + fmt + 8 + data;

    fputs("RIFF", f); put_le(f, riff > 0xFFFFFFFFu ? 0xFFFFFFFFu : riff, 4); fputs("WAVE", f);
    fputs("fmt ", f); put_le(f, fmt, 4);
    put_le(f, ext ? 0xFFFE : 1, 2); put_le(f, ch, 2); put_le(f, t->sample_rate, 4);
    put_le(f, (uint64_t)t->sample_rate * ch * bits / 8, 4); put_le(f, ch * bits / 8, 2); put_le(f, bits, 2);
    if (ext) {
        static const uint8_t pcm_guid[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                              0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        put_le(f, 22, 2); put_le(f, valid, 2);
        put_le(f, dr_layout_of(t->layout, t->channels, NULL, 0), 4);
        put_le(f, 1, 2); fwrite(pcm_guid, 1, sizeof(pcm_guid), f);
    }
    fputs("data", f); put_le(f, data > 0xFFFFFFFFu ? 0xFFFFFFFFu : data, 4);
}

/* Matroska chapter XML (mkvmerge --chapters). */
static int write_chapters(const DrDisc *d, const char *path)
{
    int nb = dr_disc_nb_chapters(d);
    FILE *f;

    if (!nb)
        return 0;
    if (!(f = fopen(path, "w")))
        return DR_ERR(errno);
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE Chapters SYSTEM \"matroskachapters.dtd\">\n"
               "<Chapters>\n  <EditionEntry>\n");
    for (int i = 0; i < nb; i++) {
        const DrDiscChapter *c = dr_disc_chapter(d, i);
        int64_t ns = av_rescale_q(c->start, TICKS_TB, (AVRational){ 1, 1000000000 });
        fprintf(f, "    <ChapterAtom>\n      <ChapterTimeStart>%02"PRId64":%02"PRId64":%02"PRId64".%09"PRId64"</ChapterTimeStart>\n",
                ns / INT64_C(3600000000000), ns / INT64_C(60000000000) % 60, ns / INT64_C(1000000000) % 60, ns % INT64_C(1000000000));
        fprintf(f, "      <ChapterDisplay>\n        <ChapterString>");
        if (c->name && *c->name) {
            for (const char *p = c->name; *p; p++)
                switch (*p) {
                case '<': fputs("&lt;", f); break;
                case '>': fputs("&gt;", f); break;
                case '&': fputs("&amp;", f); break;
                default:  fputc(*p, f);
                }
        } else {
            fprintf(f, "Chapter %02u", (unsigned)(i + 1));
        }
        fprintf(f, "</ChapterString>\n      </ChapterDisplay>\n    </ChapterAtom>\n");
    }
    fprintf(f, "  </EditionEntry>\n</Chapters>\n");
    {
        /* a write that failed before (reported only here) or the close */
        int bad = ferror(f), err = bad ? errno : 0;
        if (fclose(f) && !bad) {
            bad = 1;
            err = errno;
        }
        return bad ? DR_ERR(err ? err : EIO) : 0;
    }
}

/* ---- VobSub (.sub: the sub-picture units in MPEG-2 program stream packs
 * of 2048 bytes, private stream 1 sub-stream 0x20; .idx: the index header
 * and one "timestamp: ..., filepos: ..." line per unit) ---- */

#define PACK 2048

static void put_ts(uint8_t *p, int marker, int64_t ts)
{
    p[0] = marker << 4 | ((ts >> 29) & 0x0E) | 1;
    p[1] = ts >> 22;
    p[2] = ((ts >> 14) & 0xFE) | 1;
    p[3] = ts >> 7;
    p[4] = (ts << 1) | 1;
}

/* One unit (PTS in 90 kHz) as packs: the PTS in the first packet; the last
 * pack is filled with stuffing bytes in its PES header (fewer than 6
 * bytes left) or a padding packet. */
static int vobsub_unit(EsOut *o, const uint8_t *data, int size, int64_t pts)
{
    int first = 1;

    while (size > 0 || first) {
        uint8_t pk[PACK];
        int64_t scr = FFMAX(pts, 0);
        int hdr = first ? 5 : 0, room, n, left, stuff = 0, i = 0;

        memset(pk, 0xFF, sizeof(pk));
        pk[i++] = 0; pk[i++] = 0; pk[i++] = 1; pk[i++] = 0xBA;
        pk[i++] = 0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 0x03);
        pk[i++] = scr >> 20;
        pk[i++] = ((scr >> 12) & 0xF8) | 0x04 | ((scr >> 13) & 0x03);
        pk[i++] = scr >> 5;
        pk[i++] = ((scr << 3) & 0xF8) | 0x04;
        pk[i++] = 0x01;                                  /* SCR extension 0, marker */
        pk[i++] = 0x01; pk[i++] = 0x89; pk[i++] = 0xC3;  /* program mux rate 25200 (x 50 bytes/s) */
        pk[i++] = 0xF8;                                  /* no pack stuffing */
        room = PACK - i - 6 - 3 - hdr - 1;
        n    = FFMIN(size, room);
        left = room - n;
        if (left > 0 && left < 6)                        /* too little for a padding packet */
            stuff = left;
        pk[i++] = 0; pk[i++] = 0; pk[i++] = 1; pk[i++] = 0xBD;
        AV_WB16(pk + i, 3 + hdr + stuff + 1 + n); i += 2;
        pk[i++] = 0x81;
        pk[i++] = first ? 0x80 : 0x00;
        pk[i++] = hdr + stuff;
        if (first) {
            put_ts(pk + i, 2, pts);
            i += 5;
        }
        i += stuff;                                      /* 0xFF stuffing bytes */
        pk[i++] = 0x20;
        memcpy(pk + i, data, n);
        i += n;
        if (PACK - i >= 6) {                             /* padding packet */
            pk[i] = 0; pk[i + 1] = 0; pk[i + 2] = 1; pk[i + 3] = 0xBE;
            AV_WB16(pk + i + 4, PACK - i - 6);
        }
        if (fwrite(pk, 1, PACK, o->f) != PACK)
            return write_failed(o);
        o->sub_pos += PACK;
        data  += n;
        size  -= n;
        first  = 0;
    }
    return 0;
}

static int vobsub_write(EsOut *o, const DrDiscFrame *fr)
{
    int64_t pts = fr->time == DR_NOPTS_VALUE ? 0 : av_rescale_q(fr->time, TICKS_TB, PTS90_TB);
    int64_t ms  = fr->time == DR_NOPTS_VALUE ? 0 : av_rescale_q(fr->time, TICKS_TB, MS_TB);

    if (ms < 0)
        ms = 0;
    fprintf(o->idx, "timestamp: %02"PRId64":%02"PRId64":%02"PRId64":%03"PRId64", filepos: %09"PRIx64"\n",
            ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000, (uint64_t)o->sub_pos);
    o->st.bytes += fr->size;
    return vobsub_unit(o, fr->data, fr->size, pts);
}

/* Chooses the stream's file name and opens it: <prefix>_<NN>_[<lang>]_<codec>
 * [_<channel layout>][ (forced)][ (commentary)][ (visually impaired)]
 * [ DELAY <ms>ms].<ext>; audio carries its start delay (its first time
 * against the title start, which is the video's). */
static int es_open(const DrDiscTrack *t, EsOut *o, const char *prefix, int64_t first)
{
    char layout[64] = "", delay[32] = "", roles[64] = "";
    int audio = t->type == DISC_MEDIA_AUDIO;
    int profile = dr_layout_probe_profile(o->lp, t->profile);

    o->opened  = 1;
    o->profile = profile;
    if (t->eye) {
        snprintf(layout, sizeof(layout), "_%s eye", t->eye);
        snprintf(o->layout, sizeof(o->layout), "%s eye", t->eye);
    }
    /* Dolby Vision: DV (both layers and the metadata in one file), or the
     * layer the file holds (BL, EL) */
    if (t->dolby_vision) {
        const char *what = !strcmp(t->dolby_vision, "BL+EL+RPU") ? "DV" :
                           !strcmp(t->dolby_vision, "EL") ? "DV EL" : "DV BL";
        snprintf(layout, sizeof(layout), "_%s", what);
        snprintf(o->layout, sizeof(o->layout), "%s", what);
    }
    if (audio) {
        int known = 1;
        if (t->channels)                /* LPCM: the first frame's header said */
            dr_layout_of(t->layout, t->channels, layout + 1, sizeof(layout) - 1);
        else
            known = dr_layout_probe_name(o->lp, layout + 1, sizeof(layout) - 1);
        if (known) {
            layout[0] = '_';
            snprintf(o->layout, sizeof(o->layout), "%s", layout + 1);
        } else {
            layout[1] = 0;
            log_text(LOG_WARNING, "Track %d: channel layout not known: not in the file name", t->number);
        }
        o->st.delay_ms = av_rescale_q_rnd(first, TICKS_TB, MS_TB, AV_ROUND_NEAR_INF);
        snprintf(delay, sizeof(delay), " DELAY %"PRId64"ms", o->st.delay_ms);
    }
    snprintf(roles, sizeof(roles), "%s%s%s", t->forced ? " (forced)" : "",
             t->commentary ? " (commentary)" : "",
             t->visually_impaired ? " (visually impaired)" : "");
    if (!(o->name = av_asprintf("%s_%02d_[%s]_%s%s%s%s.%s", prefix, t->number, *o->st.lang ? o->st.lang : "und",
                                stream_label(t, profile), layout, roles, delay,
                                t->second_view ? "mvc" : es_extension(t->codec))))
        return DR_ERR(ENOMEM);
    o->st.file = o->name;
    if (!(o->f = fopen(o->name, "wb")))
        return DR_ERR(errno);
    if (t->codec == AV_CODEC_ID_PCM_S16LE || t->codec == AV_CODEC_ID_PCM_S24LE) {
        o->wav = 1;
        wav_header(o->f, t, 0);
        if (t->bits > 16 && t->bits < 24)
            log_text(LOG_INFO, "Track %d: %d-bit samples in 24-bit words, as on the disc (the WAV header says %d "
                     "valid bits) [untested on real discs]", t->number, t->bits, t->bits);
    }
    /* text subtitles: "TextST", the dialog style segment (the stream's
     * header), the number of dialogs (filled in at the end) */
    if (t->codec == AV_CODEC_ID_HDMV_TEXT_SUBTITLE) {
        if (!t->extradata_size)
            return DR_ERR_BUG;         /* the title reader gives it with the first dialog */
        if (fwrite("TextST", 1, 6, o->f) != 6 ||
            fwrite(t->extradata, 1, t->extradata_size, o->f) != (size_t)t->extradata_size ||
            fwrite("\0\0", 1, 2, o->f) != 2)
            return write_failed(o);
        o->textst    = 1;
        o->st.bytes += 8 + t->extradata_size;
    }
    if (t->codec == AV_CODEC_ID_DVD_SUBTITLE) {
        char *idx = av_strdup(o->name);
        if (!idx)
            return DR_ERR(ENOMEM);
        memcpy(idx + strlen(idx) - 3, "idx", 3);
        o->idx = fopen(idx, "w");
        av_free(idx);
        if (!o->idx)
            return DR_ERR(errno);
        /* a VobSub index starts with its format line (readers check it) */
        if (t->extradata_size < 22 || memcmp(t->extradata, "# VobSub index file, v", 22))
            fputs("# VobSub index file, v7 (do not modify this line!)\n", o->idx);
        if (t->extradata_size)
            fwrite(t->extradata, 1, t->extradata_size, o->idx);
        fprintf(o->idx, "\n# %s\nid: %s, index: 0\n", *o->st.lang ? o->st.lang : "und", *o->st.lang ? o->st.lang : "und");
    }
    return 0;
}

/* HH:MM:SS,mmm of a time in ms. */
static void srt_time(FILE *f, int64_t ms)
{
    fprintf(f, "%02"PRId64":%02"PRId64":%02"PRId64",%03"PRId64, ms / 3600000, ms / 60000 % 60, ms / 1000 % 60,
            ms % 1000);
}

/* A caption cue as SRT: its number, start --> end, its text lines, an
 * empty line. */
static int srt_write(EsOut *o, const DrDiscFrame *fr)
{
    int64_t start = av_rescale_q(fr->time, TICKS_TB, MS_TB);
    int64_t end   = av_rescale_q(fr->time + fr->duration, TICKS_TB, MS_TB);

    fprintf(o->f, "%"PRId64"\n", ++o->cues);
    srt_time(o->f, start);
    fputs(" --> ", o->f);
    srt_time(o->f, end);
    fputc('\n', o->f);
    if (fwrite(fr->data, 1, fr->size, o->f) != (size_t)fr->size || fputs("\n\n", o->f) < 0)
        return write_failed(o);
    o->st.bytes += fr->size;
    return 0;
}

/* A PGS display set as .sup: each segment after a "PG" header with the
 * set's time (90 kHz, 32 bits) and a decode time of 0 (a reader shows a set
 * at its composition segment's time; the decode times only schedule a
 * player's decoder). */
static int sup_write(EsOut *o, const DrDiscFrame *fr)
{
    uint32_t pts = (uint32_t)av_rescale_q(fr->time, TICKS_TB, PTS90_TB);
    int off = 0;

    while (off + 3 <= fr->size) {
        int len = 3 + AV_RB16(fr->data + off + 1);
        uint8_t h[10] = { 'P', 'G' };
        if (off + len > fr->size)
            return DR_ERR_INVALID;
        AV_WB32(h + 2, pts);
        if (fwrite(h, 1, sizeof(h), o->f) != sizeof(h) || fwrite(fr->data + off, 1, len, o->f) != (size_t)len)
            return write_failed(o);
        o->st.bytes += sizeof(h) + len;
        off += len;
    }
    return off == fr->size ? 0 : DR_ERR_INVALID;
}

/* A text subtitle dialog as MKVToolNix writes it in a .textst file: the
 * segment as on the disc with its start and end PTS (5 bytes each: the low
 * bit of the first byte and 32 bits) set to its time on the title timeline
 * (90 kHz) and that time plus its duration. */
static void put_textst_pts(uint8_t *p, int64_t pts)
{
    p[0] = (p[0] & 0xfe) | ((pts >> 32) & 1);
    AV_WB32(p + 1, (uint32_t)pts);
}

static int textst_write(EsOut *o, const DrDiscFrame *fr)
{
    int64_t start = av_rescale_q(fr->time, TICKS_TB, PTS90_TB);
    int64_t end   = start + av_rescale_q(fr->duration, TICKS_TB, PTS90_TB);
    uint8_t *d;

    if (fr->size < 13 || !(d = av_memdup(fr->data, fr->size)))
        return fr->size < 13 ? DR_ERR_INVALID : DR_ERR(ENOMEM);
    put_textst_pts(d + 3, start);
    put_textst_pts(d + 8, end);
    if (fwrite(d, 1, fr->size, o->f) != (size_t)fr->size) {
        av_free(d);
        return write_failed(o);
    }
    av_free(d);
    o->st.bytes += fr->size;
    o->dialogs++;
    return 0;
}

static int es_write(EsOut *o, const uint8_t *data, int size)
{
    if (o->idx)
        return DR_ERR_BUG;   /* sub-pictures go through vobsub_write() */
    if (fwrite(data, 1, size, o->f) != (size_t)size)
        return write_failed(o);
    o->st.bytes += size;
    return 0;
}

static int es_flush_hold(const DrDiscTrack *t, EsOut *o, const char *prefix)
{
    int ret = 0;

    if (!o->opened && o->nb_hold && (ret = es_open(t, o, prefix, o->hold[0].time)) < 0)
        return ret;
    for (int i = 0; i < o->nb_hold; i++) {
        if (ret >= 0)
            ret = es_write(o, o->hold[i].data, o->hold[i].size);
        dr_disc_frame_unref(&o->hold[i]);
    }
    o->nb_hold = 0;
    return ret;
}

/* A key file of the keydb folder: a regular file named *.cfg. */
static int keydb_file(const struct dirent *e)
{
    size_t n = strlen(e->d_name);
    return e->d_name[0] != '.' && n > 4 && !strcasecmp(e->d_name + n - 4, ".cfg");
}

/* The AACS key files: setting aacs.key_files, or (empty) every .cfg file of
 * the keydb folder next to the program in name order. *list (free it and
 * every own entry) and the count; the setting's entries are not copied. */
static int key_files(const char ***list, int *own)
{
    const char *const *keys;
    int nb = setting_list("aacs.key_files", &keys);
    char dir[PATH_MAX];
    struct dirent **names;
    const char **out;

    *own = 0;
    if (nb) {
        if (!(out = calloc(nb, sizeof(*out))))
            return 0;
        for (int i = 0; i < nb; i++)
            out[i] = keys[i];
        *list = out;
        return nb;
    }
    snprintf(dir, sizeof(dir), "%s/keydb", program_dir());
    if ((nb = scandir(dir, &names, keydb_file, alphasort)) <= 0)
        return 0;
    *own = 1;
    out = calloc(nb, sizeof(*out));
    for (int i = 0; i < nb; i++) {
        char *p = out ? malloc(strlen(dir) + strlen(names[i]->d_name) + 2) : NULL;
        if (p)
            sprintf(p, "%s/%s", dir, names[i]->d_name);
        if (out)
            out[i] = p;
        free(names[i]);
    }
    free(names);
    if (!out)
        return 0;
    *list = out;
    return nb;
}

/* DVD: the cell mode / cell check settings' words as the disc layer's
 * values (the settings registry allows only these). */
static int cell_mode_of(const char *text)
{
    return !strcmp(text, "walk") ? 1 : !strcmp(text, "trim") ? 2 : !strcmp(text, "walk_trim") ? 3 : 0;
}

static int cell_check_of(const char *text)
{
    return !strcmp(text, "spread") ? 1 : !strcmp(text, "random") ? 2 : 0;
}

int open_disc(const char *source, DrDisc **disc)
{
    DrDiscOptions o = { 0 };
    const char **keydb = NULL;
    int own = 0, ret;

    o.title          = -1;              /* the disc info only: the titles are opened on it one by one */
    o.part           = -1;              /* a part of a title is a run's choice (dr_disc_title_open()) */
    o.details        = 1;               /* the title list reads each track's details: always on */
    o.dolby_vision   = !strcmp(setting_text("bluray.dolby_vision"), "separate");
    o.read_attempts  = setting_int("read.attempts");
    o.min_length     = setting_int("titles.min_length");
    o.udf_reader     = !strcmp(setting_text("read.udf_reader"), "linux") ? DISCIO_UDF_LINUX : DISCIO_UDF_NETBSD;
    o.prefer_iso     = setting_bool("read.prefer_iso_for_old_udf102");
    o.dvd_cell_mode  = cell_mode_of(setting_text("dvd.cell_mode"));
    o.dvd_cell_check = cell_check_of(setting_text("dvd.cell_check"));
    o.video_language = setting_text("output.video_language");
    o.nb_keydb       = key_files(&keydb, &own);     /* setting aacs.key_files, or the keydb folder */
    o.keydb          = keydb;
    ret = dr_disc_open(disc, source, &o, NULL);
    for (int i = 0; own && i < o.nb_keydb; i++)
        free((void *)keydb[i]);
    free(keydb);
    return ret;
}


/* A video track's details from the disc layer: line-21 caption data taken
 * out of the video and not kept (a format without caption tracks), with
 * the disc's caption marks. A warning when the data carried caption text. */
static void video_detail(const DrDiscTrack *t, DrEventTrackResult *e)
{
    const char *l21 = t->line21;
    /* the caption track left out as asked: its data is not lost by mistake */
    int asked = t->caption_left;
    int64_t blocks = t->cc_blocks, text = t->cc_text;

    if (blocks)
        log_text(text && !asked ? LOG_WARNING : LOG_INFO, "      line-21 caption data taken out of the video, not "
                 "kept%s: %"PRId64" caption blocks, %s%s%s%s", asked ? " (the caption track is not selected)" : "",
                 blocks, text ? "WITH caption text" : "no caption text", l21 ? " (the disc marks captions on " : "",
                 l21 ? l21 : "", l21 ? ")" : "");
    else if (l21)
        log_text(LOG_INFO, "      the disc marks line-21 captions on %s, but the video carries none", l21);
    if (blocks || l21) {
        e->has_line21 = 1;
        e->line21     = l21 ? l21 : "";
        e->cc_blocks  = blocks;
        e->cc_text    = text;
    }
}

/* A subtitle track's details from the demuxer: what the units are, the
 * canvas they are drawn on and whether every unit fits it, and how many had
 * no stop time (shown until the next one). One line in the log; the same
 * in the track's result (e). */
static void subtitle_detail(const DrDiscTrack *t, int opened, DrEventTrackResult *e)
{
    if (t->caption) {
        int64_t blocks = t->cc_blocks, cues = t->cues, lost = t->cc_lost;
        log_text(blocks && !cues ? LOG_INFO : LOG_DETAIL, "      %s; %"PRId64" caption blocks -> %"PRId64" cues%s",
                 t->caption, blocks, cues,
                 blocks && !cues ? " (the caption data carries no caption text)" : "");
        if (lost) {
            log_text(LOG_INFO, "      %"PRId64" caption(s) still shown at the end of a segment left out", lost);
            e->has_cc_lost = 1;
            e->cc_lost     = lost;
        }
        e->sub       = DR_SUB_CAPTIONS;
        e->caption   = t->caption;
        e->cc_blocks = blocks;
        e->cues      = cues;
        return;
    }
    int64_t no_stop = t->no_stop, outside = t->outside;
    int w = t->width, h = t->height;
    AVBPrint b;

    av_bprint_init(&b, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&b, "%s", t->picture ? t->picture : "sub-pictures");
    if (w > 0 && h > 0) {
        av_bprintf(&b, "; drawn on %dx%d", w, h);
        if (opened && outside)
            av_bprintf(&b, ", %"PRId64" outside it", outside);
        else if (opened)
            av_bprintf(&b, ", all inside it");
    }
    if (no_stop)
        av_bprintf(&b, "; %"PRId64" without a stop time, each shown until the next one", no_stop);
    if (t->has_forced_objects)
        av_bprintf(&b, "; %"PRId64" forced object%s", t->forced_objects, t->forced_objects == 1 ? "" : "s");
    log_text(outside ? LOG_WARNING : LOG_DETAIL, "      %s", b.str);
    av_bprint_finalize(&b, NULL);
    e->sub     = DR_SUB_PICTURES;
    e->picture = t->picture ? t->picture : "";
    e->width   = w;
    e->height  = h;
    e->no_stop = no_stop;
    e->outside = outside;
}

const char *size_text(char *buf, size_t size, int64_t bytes)
{
    int gb = bytes >= (1LL << 30);
    int64_t tenths = bytes * 10 / (gb ? 1LL << 30 : 1LL << 20);

    snprintf(buf, size, "%"PRId64".%d %s", tenths / 10, (int)(tenths % 10), gb ? "GB" : "MB");
    return buf;
}

static void hms(char *buf, size_t size, double s)
{
    int64_t t = (int64_t)(s + 0.5);
    snprintf(buf, size, "%d:%02d:%02d", (int)(t / 3600), (int)(t / 60 % 60), (int)(t % 60));
}

int demux_title(const TitleJob *job, TitleOutcome *res)
{
    DrDisc *d = job->disc;
    const DiscInfo *info;
    const char *tname;
    EsOut *out = NULL;
    int nb_tracks = 0;
    char dir[4096], prefix[4200], dur[32], who[64], base[512];
    char *chap = NULL;          /* the chapter file, and why it could not be written */
    int ret, r, chap_err = 0, errors0, warnings0;

    memset(res, 0, sizeof(*res));
    /* the title's warnings: every warning line from here to its last line */
    log_counts(&errors0, &warnings0);
    /* "Title #8", or "Title #8 part 2/7" for a part of a title */
    if (job->part >= 0) {
        snprintf(who, sizeof(who), "Title #%d part %d/%d", job->title, job->part + 1, job->nb_parts);
    } else
        snprintf(who, sizeof(who), "Title #%d", job->title);
    /* the heading first: what opening the title says belongs under it */
    info = dr_disc_info(d);
    tname = info && job->title < info->nb_titles ? info->titles[job->title].name : NULL;
    {
        const DiscTitleInfo *ti = info && job->title < info->nb_titles ? &info->titles[job->title] : NULL;
        int64_t us = !ti ? 0 : job->part >= 0 && job->part < ti->nb_parts ? ti->parts[job->part].duration : ti->duration;
        hms(dur, sizeof(dur), us > 0 ? us / 1000000.0 : 0);
    }
    log_step("=== Title %d/%d: %s %s (%s) ===", job->ordinal, job->count, who + 6, tname ? tname : "", dur);
    if ((ret = dr_disc_title_open(d, job->title, job->part, job->second_view, job->keep)) < 0) {
        log_msg(MSG_TITLE_OPEN_FAILED, LOG_ERROR, "%s cannot be opened: %s", who, dr_err_text(ret));
        res->failed = 1;
        return ret;
    }
    title_name(setting_text("output.file_name_template"), tname, NULL, NULL, job->title, base, sizeof(base));
    /* a part's files: the title's name + _p<n>, in the title's folder */
    if (job->part >= 0)
        snprintf(res->name, sizeof(res->name), "%.500s_p%d", base, job->part + 1);
    else
        snprintf(res->name, sizeof(res->name), "%s", base);
    {
        DrEvent ev = { .kind = DR_EVENT_TITLE_START };
        ev.u.title_start = (DrEventTitleStart){ job->title, job->part, res->name, job->ordinal, job->count };
        engine_event(&ev);
    }

    /* a part is written as a title of its own: its own folder, like every title's */
    snprintf(dir, sizeof(dir), "%s%s%s", job->out_dir, job->sub_folder ? "/" : "", job->sub_folder ? res->name : "");
    if (mkdir(dir, 0777) < 0 && errno != EEXIST) {
        ret = DR_ERR(errno);
        log_msg(MSG_FOLDER_FAILED, LOG_ERROR, "The folder %s cannot be created: %s", dir, strerror(errno));
        goto end;
    }
    snprintf(prefix, sizeof(prefix), "%s/%s", dir, res->name);
    log_text(LOG_DETAIL, "Files: %s_*", prefix);

    nb_tracks = dr_disc_nb_tracks(d);
    out = av_calloc(nb_tracks ? nb_tracks : 1, sizeof(*out));
    if (!out) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        EsOut *o = &out[i];

        o->st.index    = t->number;
        o->st.kind     = disc_media_name(t->type);
        o->st.codec    = avcodec_get_name(t->codec);
        o->st.lang     = t->language;
        o->st.file     = "";
        o->st.first_ms = DR_NOPTS_VALUE;
        /* LPCM leaves the disc layer as little-endian PCM: its format (with
         * the channel layout) is set when its first frame comes */
        o->probe_late  = t->codec == AV_CODEC_ID_PCM_DVD || t->codec == AV_CODEC_ID_PCM_BLURAY;
        if (t->type == DISC_MEDIA_AUDIO && !t->channels && !o->probe_late) {
            if (!(o->hold = av_calloc(HOLD_MAX, sizeof(*o->hold)))) {
                ret = DR_ERR(ENOMEM);
                goto end;
            }
            if (dr_layout_probe_open_track(&o->lp, t) < 0) {
                log_text(LOG_DETAIL, "Track %d: no decoder for %s: its channel layout is not known", t->number,
                         avcodec_get_name(t->codec));
                o->layout_done = 1;
            }
        }
    }

    for (int i = 0; i < nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        if (t->second_view) {
            log_text(LOG_DETAIL, "Track %d: the second view of a 3-D video: its details are not checked (its "
                     "pictures are decoded with the base view's)", t->number);
            continue;
        }
        if ((t->type == DISC_MEDIA_VIDEO || t->type == DISC_MEDIA_AUDIO) && !out[i].probe_late &&
            details_probe_open_track(&out[i].probe, t) < 0)
            log_text(LOG_DETAIL, "Track %d: no decoder for %s: its details are not checked", t->number,
                     avcodec_get_name(t->codec));
    }
    for (;;) {
        DrDiscFrame fr;

        if ((ret = dr_disc_read(d, &fr)) < 0)
            break;
        /* a stop asked for (engine_cancel()): the title ends here */
        if (engine_cancelled()) {
            dr_disc_frame_unref(&fr);
            ret = DR_ERR_CANCELLED;
            res->stopped = 1;
            goto end;
        }
        const DrDiscTrack *t = dr_disc_track(d, fr.track);
        EsOut *o = &out[fr.track];

        if (o->probe_late) {
            o->probe_late = 0;
            if (details_probe_open_track(&o->probe, t) < 0)
                log_text(LOG_DETAIL, "Track %d: no decoder for %s: its details are not checked", t->number,
                         avcodec_get_name(t->codec));
        }
        if (o->probe)
            details_probe_data(o->probe, fr.data, fr.size);

        log_progress(job->title, dr_disc_progress(d) / 10000.0);
        o->st.packets++;
        if (fr.time != DR_NOPTS_VALUE) {
            int64_t ms  = av_rescale_q(fr.time, TICKS_TB, MS_TB);
            int64_t end = av_rescale_q(fr.time + fr.duration, TICKS_TB, MS_TB);
            if (o->st.first_ms == DR_NOPTS_VALUE)
                o->st.first_ms = ms;
            o->st.end_ms = FFMAX(o->st.end_ms, end);
        }
        if (!o->opened && o->hold) {
            if (!o->layout_done && dr_layout_probe_data(o->lp, fr.data, fr.size))
                o->layout_done = 1;
            o->hold[o->nb_hold++] = fr;         /* the frame's reference is held */
            if ((o->layout_done || o->nb_hold == HOLD_MAX) && (ret = es_flush_hold(t, o, prefix)) < 0)
                goto end;
            continue;
        }
        if (!o->opened && (ret = es_open(t, o, prefix, fr.time)) < 0) {
            dr_disc_frame_unref(&fr);
            goto end;
        }
        ret = o->idx ? vobsub_write(o, &fr) :
              t->codec == AV_CODEC_ID_SUBRIP ? srt_write(o, &fr) :
              t->codec == AV_CODEC_ID_HDMV_PGS_SUBTITLE ? sup_write(o, &fr) :
              o->textst ? textst_write(o, &fr) : es_write(o, fr.data, fr.size);
        dr_disc_frame_unref(&fr);
        if (ret < 0)
            goto end;
    }
    if (ret == DR_ERR_EOF)
        ret = 0;
    for (int i = 0; ret >= 0 && i < nb_tracks; i++)
        ret = es_flush_hold(dr_disc_track(d, i), &out[i], prefix);
    if (ret >= 0) {
        if (!(chap = av_asprintf("%s_chapters.xml", prefix)))
            ret = DR_ERR(ENOMEM);
        else if ((ret = write_chapters(d, chap)) < 0 && ret != DR_ERR(ENOMEM))
            chap_err = -ret;
    }

end:
    for (int i = 0; out && i < nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        EsOut *o = &out[i];
        if (o->f && o->wav && ret >= 0) {
            if (fseek(o->f, 0, SEEK_SET) == 0)
                wav_header(o->f, t, o->st.bytes);
            else
                ret = DR_ERR(errno);
        }
        /* text subtitles: the number of dialogs after the style segment */
        if (o->f && o->textst && ret >= 0) {
            uint8_t n[2];
            if (o->dialogs > 0xffff)
                log_text(LOG_WARNING, "Track %d: %"PRId64" text subtitle dialogs: the file's 16-bit count says %d",
                         t->number, o->dialogs, (int)(o->dialogs & 0xffff));
            AV_WB16(n, o->dialogs & 0xffff);
            if (fseek(o->f, 6 + t->extradata_size, SEEK_SET) ||
                fwrite(n, 1, 2, o->f) != 2)
                ret = write_failed(o);
        }
        if (o->f && (r = out_close(o, &o->f)) < 0 && ret >= 0)
            ret = r;
        if (o->idx && (r = out_close(o, &o->idx)) < 0 && ret >= 0)
            ret = r;
        /* a forced-only subtitle track that shows nothing is empty: no file
         * (as an MKV leaves such a track out) */
        if (ret >= 0 && o->opened && t->forced && t->has_forced_objects && !t->forced_objects) {
            if (remove(o->name) < 0)
                ret = DR_ERR(errno);
            o->opened = 0;
            o->no_forced = 1;
        }
    }
    if (res->stopped) {
        log_msg(MSG_TITLE_FAILED, LOG_WARNING, "%s stopped as asked: its files are not complete", who);
        res->failed = 1;
    } else if (ret < 0) {
        /* a file that could not be written: which one and why */
        const char *wfile = chap_err ? chap : NULL;
        int werr = chap_err;
        for (int i = 0; !werr && out && i < nb_tracks; i++)
            if (out[i].write_err) {
                werr  = out[i].write_err;
                wfile = out[i].name;
            }
        if (werr == ENOSPC || werr == EDQUOT) {
            log_msg(MSG_DISK_FULL, LOG_ERROR, "%s failed: the disk is full (no space left writing %s)", who,
                    wfile ? wfile : "its files");
            res->disk_full = 1;
        } else if (werr) {
            log_msg(MSG_TITLE_FAILED, LOG_ERROR, "%s failed: %s cannot be written: %s", who,
                    wfile ? wfile : "a file", strerror(werr));
        } else {
            log_msg(MSG_TITLE_FAILED, LOG_ERROR, "%s failed: %s", who, dr_err_text(ret));
        }
        res->failed = 1;
    } else {
        int64_t untested = dr_disc_untested(d);
        int checked = 0, check_failed = 0, unchecked = 0;
        int64_t written = 0;
        int nb_chapters = dr_disc_nb_chapters(d);
        char made[96], sz[32];

        for (int i = 0; i < nb_tracks; i++) {
            const DrDiscTrack *t = dr_disc_track(d, i);
            end_check(t, &out[i]);
            checked      += out[i].checked != 0;
            check_failed += out[i].checked < 0;
            unchecked    += out[i].opened && !out[i].probe &&
                            (t->type == DISC_MEDIA_VIDEO || t->type == DISC_MEDIA_AUDIO) && !t->second_view;
        }
        log_text(LOG_INFO, "Tracks:");
        for (int i = 0; i < nb_tracks; i++) {
            const DrDiscTrack *t = dr_disc_track(d, i);
            EsOut *o = &out[i];
            int64_t frames = t->frames, warn = t->warnings;
            char what[160], state[96], delay[48] = "";
            DrEvent ev = { .kind = DR_EVENT_TRACK_RESULT };
            DrEventTrackResult *e = &ev.u.track;
            const char *unit = t->type == DISC_MEDIA_SUBTITLE ? "subtitles" : "frames";
            int empty_cc = !o->opened && t->caption;

            snprintf(what, sizeof(what), "%s %s%s%s", *o->st.lang ? o->st.lang : "   ",
                     stream_label(t, o->opened ? o->profile : t->profile), *o->layout ? " " : "", o->layout);
            if (empty_cc)
                snprintf(state, sizeof(state), "no caption text: no file");
            else if (o->no_forced)
                snprintf(state, sizeof(state), "no forced subtitles: no file");
            else if (!o->opened)
                snprintf(state, sizeof(state), "no data: no file");
            else if (warn)
                snprintf(state, sizeof(state), "%"PRId64" warning%s", warn, warn == 1 ? "" : "s");
            else
                snprintf(state, sizeof(state), "OK");
            if (o->opened && t->type == DISC_MEDIA_AUDIO)
                snprintf(delay, sizeof(delay), ", delay %"PRId64" ms", o->st.delay_ms);
            log_text(!o->opened && !empty_cc && !o->no_forced ? LOG_WARNING : warn || empty_cc || o->no_forced ?
                     LOG_INFO : LOG_OK,
                     "  %02d  %-30s %8"PRId64" %-9s  %s%s",
                     t->number, what, frames, unit, state, delay);
            if (t->type == DISC_MEDIA_SUBTITLE)
                subtitle_detail(t, o->opened, e);
            else if (t->type == DISC_MEDIA_VIDEO)
                video_detail(t, e);
            if (o->checked < 0)
                log_text(LOG_ERROR, "      the file's stream details differ from the disc stream's: %s", o->problems);
            if (o->checked) {
                e->details_check = o->checked > 0 ? 1 : -1;
                e->details       = o->problems;
            }
            if (o->opened)
                log_text(LOG_DETAIL, "      %s", o->name);
            e->title    = job->title;
            e->part     = job->part;
            e->index    = t->number;
            e->codec    = stream_label(t, o->opened ? o->profile : t->profile);
            e->language = o->st.lang;
            e->layout   = o->layout;
            e->frames   = frames;
            e->warnings = warn;
            e->delay_ms = o->st.delay_ms;
            e->file     = o->opened ? o->name : "";
            engine_event(&ev);
        }
        if (checked)
            log_text(check_failed ? LOG_ERROR : LOG_DETAIL, "Stream details (read back from the files): %d of %d "
                     "video / audio track%s match the disc streams%s", checked - check_failed, checked,
                     checked == 1 ? "" : "s", check_failed ? "" : ": PASS");
        if (unchecked)
            log_text(LOG_WARNING, "Stream details: %d video / audio track%s not checked (no decoder)", unchecked,
                     unchecked == 1 ? "" : "s");
        /* damaged data the reader left out (each range a warning above) */
        res->damaged_ranges = dr_disc_damaged_ranges(d);
        res->damaged_bytes  = dr_disc_damaged_bytes(d);
        snprintf(res->damage, sizeof(res->damage), "%s", dr_disc_damage(d));
        log_msg(MSG_CHAPTERS, LOG_DETAIL, "Chapters: %u", (unsigned)nb_chapters);
        /* the warning lines the title gave, as the run's summary counts them */
        {
            int errors1, warnings1;
            log_counts(&errors1, &warnings1);
            res->warnings = warnings1 - warnings0;
        }
        /* what was written, for the title's last line */
        for (int i = 0; i < nb_tracks; i++)
            written += out[i].opened ? out[i].st.bytes : 0;
        snprintf(made, sizeof(made), "(%u chapter%s, %s)", (unsigned)nb_chapters, nb_chapters == 1 ? "" : "s",
                 size_text(sz, sizeof(sz), written));
        if (check_failed) {
            log_msg(MSG_DETAILS_DIFFER, LOG_ERROR, "%s: its files are written, but %d of them do not keep "
                    "the disc stream's details (see above)", who, check_failed);
            res->failed = 1;
        } else if (untested) {
            /* written: the untested paths are said in the log ("[untested on
             * real discs]"), a warning each */
            log_msg(MSG_UNTESTED, LOG_WARNING, "%s: done with %"PRId64" warning%s, %"PRId64" of them feature(s) no "
                    "real disc has tested yet (see the log's \"[untested on real discs]\" lines) %s", who,
                    res->warnings, res->warnings == 1 ? "" : "s", untested, made);
        } else if (res->damaged_ranges) {
            char dz[32];
            log_msg(MSG_TITLE_DONE, LOG_INFO, "%s: done with %"PRId64" warning%s (see above), %"PRId64" of them "
                    "damaged data (%s left out) %s", who, res->warnings, res->warnings == 1 ? "" : "s",
                    res->damaged_ranges, size_text(dz, sizeof(dz), res->damaged_bytes), made);
        } else if (res->warnings) {
            log_msg(MSG_TITLE_DONE, LOG_INFO, "%s: done with %"PRId64" warning%s (see above) %s", who, res->warnings,
                    res->warnings == 1 ? "" : "s", made);
        } else {
            log_msg(MSG_TITLE_DONE, LOG_OK, "%s: done, all checks passed %s", who, made);
        }
    }
    {
        DrEvent ev = { .kind = DR_EVENT_TITLE_RESULT };
        ev.u.title_result = (DrEventTitleResult){ job->title, job->part,
                                                  res->stopped ? DR_STATUS_STOPPED : res->failed ? DR_STATUS_FAILED :
                                                  DR_STATUS_OK, res->warnings, res->damaged_ranges,
                                                  res->damaged_bytes, dir };
        engine_event(&ev);
    }
    for (int i = 0; out && i < nb_tracks; i++) {
        EsOut *o = &out[i];
        for (int k = 0; k < o->nb_hold; k++)
            dr_disc_frame_unref(&o->hold[k]);
        av_free(o->hold);
        dr_layout_probe_close(&o->lp);
        details_probe_close(&o->probe);
        av_free(o->name);
    }
    av_free(out);
    av_free(chap);
    dr_disc_title_close(d);
    return ret;
}
