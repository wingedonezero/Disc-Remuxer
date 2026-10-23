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
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/bytes.h"
#include "base/err.h"
#include "base/log.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/str.h"
#include "io/discio.h"
#include "base/msg.h"
#include "names.h"
#include "analyze/details.h"
#include "disc/disc.h"
#include "emit.h"
#include "engine.h"
#include "events.h"
#include "base/settings.h"
#include "codecs.h"
#include "timeline/disctl.h"
#include "writer.h"
#include "mkvout.h"
#include "mkv/mkv.h"

/* A time on the title timeline (DR_TICKS_PER_SECOND ticks) in units of
 * 1 / per_second, rounded to the nearest (halves away from 0). */
static int64_t ticks_in(int64_t t, int64_t per_second)
{
    int64_t a = per_second, b = DR_TICKS_PER_SECOND;

    while (b) {
        int64_t r = a % b;
        a = b;
        b = r;
    }
    return dr_rescale(t, per_second / a, DR_TICKS_PER_SECOND / a);
}

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
    case DR_CODEC_MPEG1VIDEO:   return "m1v";
    case DR_CODEC_MPEG2VIDEO:   return "m2v";
    case DR_CODEC_VC1:          return "vc1";
    case DR_CODEC_H264:         return "h264";
    case DR_CODEC_HEVC:         return "hevc";
    case DR_CODEC_AC3:          return "ac3";
    case DR_CODEC_EAC3:         return "eac3";
    case DR_CODEC_TRUEHD:       return "thd";
    case DR_CODEC_MLP:          return "mlp";
    case DR_CODEC_DTS:          return "dts";
    case DR_CODEC_MP1:          return "mp1";
    case DR_CODEC_MP2:          return "mp2";
    case DR_CODEC_MP3:          return "mp3";
    case DR_CODEC_AAC:          return "aac";
    case DR_CODEC_PCM_S16LE:
    case DR_CODEC_PCM_S24LE:    return "wav";
    case DR_CODEC_DVD_SUBTITLE: return "sub";
    case DR_CODEC_HDMV_PGS_SUBTITLE: return "sup";
    case DR_CODEC_HDMV_TEXT_SUBTITLE: return "textst";
    case DR_CODEC_SUBRIP:       return "srt";
    default:                       return "bin";
    }
}

const char *track_extension(const DiscStreamInfo *k)
{
    if (k->second_view)
        return "mvc";
    switch (k->codec) {
    case DR_CODEC_PCM_DVD:
    case DR_CODEC_PCM_BLURAY:   return "wav";   /* LPCM is written as PCM in a WAVE file */
    case DR_CODEC_EIA_608:      return "srt";   /* closed captions made into text */
    default:                       return es_extension(k->codec);
    }
}

const char *codec_label(int id, int profile)
{
    switch (id) {
    case DR_CODEC_MPEG1VIDEO:   return "Mpeg1";
    case DR_CODEC_MPEG2VIDEO:   return "Mpeg2";
    case DR_CODEC_VC1:          return "VC-1";
    case DR_CODEC_H264:         return "Mpeg4";
    case DR_CODEC_HEVC:         return "MpegH";
    case DR_CODEC_AC3:          return "DD";
    case DR_CODEC_EAC3:         return profile == DISC_PROFILE_EAC3_DDP_ATMOS ? "DD+ Atmos" : "DDplus";
    case DR_CODEC_TRUEHD:       return profile == DISC_PROFILE_TRUEHD_ATMOS ? "TrueHD Atmos" : "TrueHD";
    case DR_CODEC_MLP:          return "MLP";
    case DR_CODEC_DTS:          return profile == DISC_PROFILE_DTS_HD_MA_X || profile == DISC_PROFILE_DTS_HD_MA_X_IMAX ?
                                          "DTS:X" : profile == DISC_PROFILE_DTS_HD_MA ? "DTS-HD MA" :
                                          profile == DISC_PROFILE_DTS_HD_HRA ? "DTS-HD HR" : "DTS";
    case DR_CODEC_MP1:
    case DR_CODEC_MP2:
    case DR_CODEC_MP3:          return "MPEG audio";
    case DR_CODEC_AAC:          return "AAC";
    case DR_CODEC_PCM_DVD:
    case DR_CODEC_PCM_S16LE:
    case DR_CODEC_PCM_S24LE:
    case DR_CODEC_PCM_BLURAY:   return "LPCM";
    case DR_CODEC_HDMV_PGS_SUBTITLE:  return "PGS";
    case DR_CODEC_HDMV_TEXT_SUBTITLE: return "TextST";
    case DR_CODEC_DVD_SUBTITLE: return "VobSub";
    case DR_CODEC_SUBRIP:       return "CC";      /* closed captions made into text */
    default:                       return codec_id_name(id);
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
    int           mkv_skip;         /* MKV: a track whose codec the MKV writer does not take yet */
    int64_t      *ts;               /* video: each picture's time (ns), for the timestamp file */
    size_t        nb_ts, cap_ts;
    char         *ts_path;          /* the timestamp file written */
    int64_t       sub_pos;          /* bytes written to the .sub */
    int64_t       sup_wrapped;      /* PGS: display sets past the .sup's 32-bit time (writer_sup_time()) */
    int64_t       sup_wrap_first;   /* the first one's time */
    int64_t       cues;             /* captions: SRT cues written */
    int           wav;              /* a WAV header to complete at the end */
    int           wav_room;         /* its header keeps room for the RF64 sizes (it may pass 4 GiB) */
    int64_t       title_us;         /* the title's (or part's) length, microseconds: what a file may reach */
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

static uint8_t *put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        *p++ = (v >> (8 * i)) & 0xFF;
    return p;
}

static uint8_t *put_tag(uint8_t *p, const char *tag)
{
    memcpy(p, tag, 4);
    return p + 4;
}

int writer_wav_header(uint8_t *out, int channels, int rate, int bits, int valid, uint64_t mask, int64_t data,
                      int room)
{
    static const uint8_t pcm_guid[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                          0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
    int ext = channels > 2 || bits > 16, fmt = ext ? 40 : 16, block = channels * bits / 8;
    uint64_t riff = 4 + (room ? 8 + WAV_DS64 : 0) + 8 + fmt + 8 + data;
    int rf64 = room && riff > 0xFFFFFFFFu;
    uint8_t *p = out;

    p = put_tag(p, rf64 ? "RF64" : "RIFF");
    p = put_le(p, riff > 0xFFFFFFFFu ? 0xFFFFFFFFu : riff, 4);
    p = put_tag(p, "WAVE");
    if (room) {
        p = put_tag(p, rf64 ? "ds64" : "JUNK");
        p = put_le(p, WAV_DS64, 4);
        p = put_le(p, rf64 ? riff : 0, 8);                       /* RIFF size */
        p = put_le(p, rf64 ? (uint64_t)data : 0, 8);             /* data size */
        p = put_le(p, rf64 && block ? (uint64_t)data / block : 0, 8);   /* sample frames */
        p = put_le(p, 0, 4);                                     /* no table */
    }
    p = put_tag(p, "fmt ");
    p = put_le(p, fmt, 4);
    p = put_le(p, ext ? 0xFFFE : 1, 2);
    p = put_le(p, channels, 2);
    p = put_le(p, rate, 4);
    p = put_le(p, (uint64_t)rate * block, 4);
    p = put_le(p, block, 2);
    p = put_le(p, bits, 2);
    if (ext) {
        p = put_le(p, 22, 2);
        p = put_le(p, valid, 2);
        p = put_le(p, mask, 4);
        p = put_le(p, 1, 2);
        memcpy(p, pcm_guid, sizeof(pcm_guid));
        p += sizeof(pcm_guid);
    }
    p = put_tag(p, "data");
    p = put_le(p, rf64 || (uint64_t)data > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint64_t)data, 4);
    return p - out;
}

/* The track's WAV header for data bytes of sound (see writer.h): its size,
 * or a base error. */
static int wav_header(FILE *f, const DrDiscTrack *t, int64_t data, int room)
{
    uint8_t h[WAV_HEADER_MAX];
    int bits = t->codec == DR_CODEC_PCM_S24LE ? 24 : 16;
    int n = writer_wav_header(h, t->channels, t->sample_rate, bits, t->bits > 0 && t->bits < bits ? t->bits : bits,
                              dr_layout_of(t->layout, t->channels, NULL, 0), data, room);

    return fwrite(h, 1, n, f) == (size_t)n ? n : DR_ERR(errno);
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
        int64_t ns = ticks_in(c->start, 1000000000);
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
        int64_t scr = DR_MAX(pts, 0);
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
        n    = DR_MIN(size, room);
        left = room - n;
        if (left > 0 && left < 6)                        /* too little for a padding packet */
            stuff = left;
        pk[i++] = 0; pk[i++] = 0; pk[i++] = 1; pk[i++] = 0xBD;
        dr_wb16(pk + i, 3 + hdr + stuff + 1 + n); i += 2;
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
            dr_wb16(pk + i + 4, PACK - i - 6);
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
    int64_t pts = fr->time == DR_NOPTS_VALUE ? 0 : ticks_in(fr->time, 90000);
    int64_t ms  = fr->time == DR_NOPTS_VALUE ? 0 : ticks_in(fr->time, 1000);

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
    char layout[64] = "", delay[32] = "", roles[64] = "", label[32];
    int audio = t->type == DISC_MEDIA_AUDIO;
    int profile = dr_layout_probe_profile(o->lp, t->profile);

    /* DTS-HD MA: DTS:X as the title's details found it (our walk of the
     * lossless frames' marker), not the decoder's profile */
    if (t->codec == DR_CODEC_DTS && (t->profile == DISC_PROFILE_DTS_HD_MA || t->profile == DISC_PROFILE_DTS_HD_MA_X ||
                                     t->profile == DISC_PROFILE_DTS_HD_MA_X_IMAX))
        profile = t->profile;

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
        o->st.delay_ms = ticks_in(first, 1000);
        snprintf(delay, sizeof(delay), " DELAY %"PRId64"ms", o->st.delay_ms);
    }
    snprintf(roles, sizeof(roles), "%s%s%s", t->forced ? " (forced)" : "",
             t->commentary ? " (commentary)" : "",
             t->visually_impaired ? " (visually impaired)" : "");
    /* no ':' in a file name (DTS:X -> DTS-X), as in every name we write */
    snprintf(label, sizeof(label), "%s", stream_label(t, profile));
    for (char *c = label; (c = strchr(c, ':')); )
        *c = '-';
    if (!(o->name = dr_asprintf_str("%s_%02d_[%s]_%s%s%s%s.%s", prefix, t->number, *o->st.lang ? o->st.lang : "und",
                                label, layout, roles, delay,
                                t->second_view ? "mvc" : es_extension(t->codec))))
        return DR_ERR(ENOMEM);
    o->st.file = o->name;
    if (!(o->f = fopen(o->name, "wb")))
        return DR_ERR(errno);
    if (t->codec == DR_CODEC_PCM_S16LE || t->codec == DR_CODEC_PCM_S24LE) {
        /* room for the RF64 sizes when the title's length lets the file
         * come near 4 GiB (a file that cannot stays as it always was) */
        int64_t expect = o->title_us / 1000000 * t->sample_rate * t->channels *
                         (t->codec == DR_CODEC_PCM_S24LE ? 3 : 2);
        int ret;

        o->wav      = 1;
        o->wav_room = expect >= WAV_ROOM_FROM;
        if ((ret = wav_header(o->f, t, 0, o->wav_room)) < 0)
            return ret;
        if (t->bits > 16 && t->bits < 24)
            log_text(LOG_INFO, "Track %d: %d-bit samples in 24-bit words, as on the disc (the WAV header says %d "
                     "valid bits) [untested on real discs]", t->number, t->bits, t->bits);
    }
    /* text subtitles: "TextST", the dialog style segment (the stream's
     * header), the number of dialogs (filled in at the end) */
    if (t->codec == DR_CODEC_HDMV_TEXT_SUBTITLE) {
        if (!t->extradata_size)
            return DR_ERR_BUG;         /* the title reader gives it with the first dialog */
        if (fwrite("TextST", 1, 6, o->f) != 6 ||
            fwrite(t->extradata, 1, t->extradata_size, o->f) != (size_t)t->extradata_size ||
            fwrite("\0\0", 1, 2, o->f) != 2)
            return write_failed(o);
        o->textst    = 1;
        o->st.bytes += 8 + t->extradata_size;
    }
    if (t->codec == DR_CODEC_DVD_SUBTITLE) {
        char *idx = dr_strdup(o->name);
        if (!idx)
            return DR_ERR(ENOMEM);
        memcpy(idx + strlen(idx) - 3, "idx", 3);
        o->idx = fopen(idx, "w");
        dr_free(idx);
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
    int64_t start = ticks_in(fr->time, 1000);
    int64_t end   = ticks_in(fr->time + fr->duration, 1000);

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

/* The timestamp file: a video frame's pictures (H.264: two field pictures
 * are two, as a muxer writes them), an audio frame's packets as mkvmerge
 * cuts them (AC-3 / E-AC-3: an independent frame with its dependent ones;
 * DTS: a frame; TrueHD: an access unit), each at its share of the frame's
 * time. */
static int ts_codec(const DrDiscTrack *t)
{
    if (t->type == DISC_MEDIA_VIDEO)
        return !t->second_view;
    return t->type == DISC_MEDIA_AUDIO &&
           (t->codec == DR_CODEC_AC3 || t->codec == DR_CODEC_EAC3 || t->codec == DR_CODEC_DTS ||
            t->codec == DR_CODEC_TRUEHD);
}

static int is_lpcm(const DrDiscTrack *t)
{
    return t->codec == DR_CODEC_PCM_S16LE || t->codec == DR_CODEC_PCM_S24LE || t->codec == DR_CODEC_PCM_BLURAY ||
           t->codec == DR_CODEC_PCM_DVD;
}

static int ts_add(EsOut *o, const DrDiscTrack *t, const DrDiscFrame *fr)
{
    int n = t->codec == DR_CODEC_H264 ? mkv_h264_pictures(fr->data, fr->size) :
            t->codec == DR_CODEC_AC3 || t->codec == DR_CODEC_EAC3 ? mkv_audio_packets(MKV_CODEC_AC3, fr->data, fr->size) :
            t->codec == DR_CODEC_DTS ? mkv_audio_packets(MKV_CODEC_DTS, fr->data, fr->size) :
            t->codec == DR_CODEC_TRUEHD ? mkv_audio_packets(MKV_CODEC_TRUEHD, fr->data, fr->size) : 1;
    if (n == 0 && t->type == DISC_MEDIA_AUDIO)
        return 0;                       /* only dependent frames: they go with the frame before them */
    int64_t ns = ticks_in(fr->time, 1000000000), dur = fr->duration > 0 ? ticks_in(fr->duration, 1000000000) : 0;

    if (n < 1)
        n = 1;
    if (o->nb_ts + n > o->cap_ts) {
        size_t cap = o->cap_ts ? 2 * o->cap_ts : 65536;
        int64_t *p = dr_realloc_array(o->ts, cap, sizeof(*p));
        if (!p)
            return DR_ERR(ENOMEM);
        o->ts = p;
        o->cap_ts = cap;
    }
    for (int k = 0; k < n; k++)
        o->ts[o->nb_ts++] = ns + dur * k / n;
    return 0;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* mkvmerge's timestamp format v4: the pictures' times in display order, in
 * ms with six decimals and half a ns more (away from 0: mkvmerge cuts the ns
 * it reads toward 0, so each line reads back as its exact ns). */
static int ts_write(const char *path, EsOut *o)
{
    FILE *f = fopen(path, "w");
    int ok;

    if (!f)
        return DR_ERR(errno);
    qsort(o->ts, o->nb_ts, sizeof(*o->ts), cmp_i64);
    ok = fputs("# timestamp format v4\n", f) >= 0;
    for (size_t i = 0; ok && i < o->nb_ts; i++) {
        int64_t t = o->ts[i];
        ok = (t < 0 ? fprintf(f, "-%"PRId64".%06"PRId64"5\n", -t / 1000000, -t % 1000000) :
                      fprintf(f, "%"PRId64".%06"PRId64"5\n", t / 1000000, t % 1000000)) > 0;
    }
    if (fclose(f) != 0 || !ok)
        return DR_ERR(errno ? errno : EIO);
    return 0;
}

/* A PGS display set as .sup: each segment after a "PG" header with the
 * set's time (90 kHz, 32 bits) and a decode time of 0 (a reader shows a set
 * at its composition segment's time; the decode times only schedule a
 * player's decoder). */
int writer_sup_time(int64_t time, uint32_t *pts)
{
    int64_t tick = ticks_in(time, 90000);

    *pts = (uint32_t)tick;
    return tick > UINT32_MAX;
}

static int sup_write(EsOut *o, const DrDiscFrame *fr)
{
    uint32_t pts;
    int off = 0;

    /* past what the .sup's 32 bits hold: counted, said at the title's end */
    if (writer_sup_time(fr->time, &pts) && !o->sup_wrapped++)
        o->sup_wrap_first = fr->time;

    while (off + 3 <= fr->size) {
        int len = 3 + dr_rb16(fr->data + off + 1);
        uint8_t h[10] = { 'P', 'G' };
        if (off + len > fr->size)
            return DR_ERR_INVALID;
        dr_wb32(h + 2, pts);
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
    dr_wb32(p + 1, (uint32_t)pts);
}

static int textst_write(EsOut *o, const DrDiscFrame *fr)
{
    int64_t start = ticks_in(fr->time, 90000);
    int64_t end   = start + ticks_in(fr->duration, 90000);
    uint8_t *d;

    if (fr->size < 13 || !(d = dr_memdup(fr->data, fr->size)))
        return fr->size < 13 ? DR_ERR_INVALID : DR_ERR(ENOMEM);
    put_textst_pts(d + 3, start);
    put_textst_pts(d + 8, end);
    if (fwrite(d, 1, fr->size, o->f) != (size_t)fr->size) {
        dr_free(d);
        return write_failed(o);
    }
    dr_free(d);
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
    DrStr b;

    dr_str_init(&b);
    dr_str_catf(&b, "%s", t->picture ? t->picture : "sub-pictures");
    if (w > 0 && h > 0) {
        dr_str_catf(&b, "; drawn on %dx%d", w, h);
        if (opened && outside)
            dr_str_catf(&b, ", %"PRId64" outside it", outside);
        else if (opened)
            dr_str_catf(&b, ", all inside it");
    }
    if (no_stop)
        dr_str_catf(&b, "; %"PRId64" without a stop time, each shown until the next one", no_stop);
    if (t->has_forced_objects)
        dr_str_catf(&b, "; %"PRId64" forced object%s", t->forced_objects, t->forced_objects == 1 ? "" : "s");
    log_text(outside ? LOG_WARNING : LOG_DETAIL, "      %s", b.str ? b.str : "");
    dr_str_free(&b);
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

    /* below 1 MB in bytes or KB (a few damaged bytes are not "0.0 MB") */
    if (bytes < 1024)
        snprintf(buf, size, "%"PRId64" byte%s", bytes, bytes == 1 ? "" : "s");
    else if (bytes < (1LL << 20))
        snprintf(buf, size, "%.1f KB", bytes / 1024.0);
    else
        snprintf(buf, size, "%"PRId64".%d %s", tenths / 10, (int)(tenths % 10), gb ? "GB" : "MB");
    return buf;
}

static void hms(char *buf, size_t size, double s)
{
    int64_t t = (int64_t)(s + 0.5);
    snprintf(buf, size, "%d:%02d:%02d", (int)(t / 3600), (int)(t / 60 % 60), (int)(t % 60));
}

void title_outcome_free(TitleOutcome *out)
{
    for (int i = 0; i < out->nb_damage; i++)
        dr_freep(&out->damage[i]);
    dr_freep(&out->damage);
    out->nb_damage = 0;
}

/* ---- output.format = mkvmerge: the stream files muxed by mkvmerge ----
 * The options go into an options file (mkvmerge @file: one JSON array of
 * strings, UTF-8): --output, then each file's options before "( file )",
 * then the chapters and the track order. */

typedef struct Opts {
    char  *buf;
    size_t len, cap;
    int    n;
} Opts;

static int opts_add(Opts *o, const char *arg)
{
    for (const unsigned char *p = (const unsigned char *)arg; ; p++) {
        char esc[8];
        const char *add = NULL;
        size_t need;
        if (o->len + 16 > o->cap) {
            size_t cap = o->cap ? 2 * o->cap : 4096;
            char *b = dr_realloc(o->buf, cap);
            if (!b)
                return DR_ERR(ENOMEM);
            o->buf = b;
            o->cap = cap;
        }
        if (p == (const unsigned char *)arg) {
            if (o->n++)
                o->buf[o->len++] = ',';
            o->buf[o->len++] = '"';
        }
        if (!*p) {
            o->buf[o->len++] = '"';
            o->buf[o->len] = 0;
            return 0;
        }
        if (*p == '"' || *p == '\\')
            snprintf(esc, sizeof(esc), "\\%c", *p), add = esc;
        else if (*p < 0x20)
            snprintf(esc, sizeof(esc), "\\u%04x", *p), add = esc;
        if (add) {
            need = strlen(add);
            memcpy(o->buf + o->len, add, need);
            o->len += need;
        } else
            o->buf[o->len++] = (char)*p;
    }
}

static int opts_addf(Opts *o, const char *fmt, ...)
{
    char b[4200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    return opts_add(o, b);
}

/* One line of mkvmerge's output into the log (its warnings and errors as
 * such; mkvmerge separates its progress updates with \r inside one line, so
 * each \r-piece is a line of its own). */
static void mkvmerge_line(const char *line)
{
    log_text(!strncmp(line, "Error:", 6) ? LOG_ERROR : !strncmp(line, "Warning:", 8) ? LOG_WARNING :
             LOG_DETAIL, "mkvmerge: %s", line);
}

/* Runs mkvmerge with the options file; its lines go into the log. A stop
 * asked for while it runs (engine_cancel()) ends it (SIGTERM). Its exit
 * status: 0 done, 1 done with warnings, 2 failed (also a stop; the caller
 * tells them apart by engine_cancelled()); negative: it could not be run. */
static int run_mkvmerge(const char *program, const char *opts_path)
{
    char at[4200];
    char *argv[] = { (char *)program, at, NULL };
    posix_spawn_file_actions_t fa;
    int pfd[2], status = 0, ret, killed = 0;
    pid_t pid;
    char buf[4096], line[4096];
    size_t have = 0;
    extern char **environ;

    snprintf(at, sizeof(at), "@%s", opts_path);
    if (pipe(pfd) < 0)
        return DR_ERR(errno);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    ret = posix_spawnp(&pid, program, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    if (ret) {
        close(pfd[0]);
        return DR_ERR(ret);
    }
    for (;;) {
        struct pollfd pl = { .fd = pfd[0], .events = POLLIN };
        int pr = poll(&pl, 1, 200), n;
        if (engine_cancelled() && !killed) {
            kill(pid, SIGTERM);
            killed = 1;
        }
        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        if ((n = (int)read(pfd[0], buf, sizeof(buf))) <= 0)
            break;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\r' || buf[i] == '\n') {
                line[have] = 0;
                if (have)
                    mkvmerge_line(line);
                have = 0;
            } else if (have < sizeof(line) - 1)
                line[have++] = (char)buf[i];
        }
    }
    line[have] = 0;
    if (have)
        mkvmerge_line(line);
    close(pfd[0]);
    if (waitpid(pid, &status, 0) < 0)
        return DR_ERR(errno);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
}

/* The title's stream files into <out folder>/<title>.mkv with mkvmerge;
 * on success the files written for it are deleted (the title's folder too
 * when it has one). 0, or a negative error (the stream files are kept). */
/* Does this title have a real hole — pictures missing where the times jump,
 * an audio gap left open (damaged data left out makes one when it took
 * pictures or audio with it)? The stream files close such a hole; with its
 * timestamp file every track keeps it. why: what was found. */
static int title_holes(DrDisc *d, EsOut *out, int nb_tracks, char *why, size_t why_size)
{
    char ms[32];

    *why = 0;
    for (int i = 0; i < nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        if (!out[i].opened || !t->holes || (t->type != DISC_MEDIA_VIDEO && t->type != DISC_MEDIA_AUDIO))
            continue;
        snprintf(ms, sizeof(ms), "%.3f", t->hole_ticks / (DR_TICKS_PER_SECOND / 1000.0));
        snprintf(why + strlen(why), why_size - strlen(why), "%strack %d: %"PRId64" %s (%s ms)",
                 *why ? "; " : "", t->number, t->holes,
                 t->type == DISC_MEDIA_VIDEO ? "hole(s) in the pictures" : "audio gap(s) left open", ms);
    }
    return *why != 0;
}

static int mkvmerge_title(DrDisc *d, EsOut *out, int nb_tracks, const char *out_dir, const char *dir, int own_dir,
                          const char *name, const char *chap, int nb_chapters, int use_ts, char **mkv_out)
{
    const char *program = *setting_text("mkv.mkvmerge_path") ? setting_text("mkv.mkvmerge_path") : "mkvmerge";
    int none = !strcmp(setting_text("mkv.compression"), "none");
    int dialnorm = setting_bool("mkv.remove_dialog_normalization");
    Opts o = { 0 };
    char *mkv = dr_asprintf_str("%s/%s.mkv", out_dir, name), *opts_path = dr_asprintf_str("%s/%s_mkvmerge.json", dir,
                                                                                          name);
    char order[8192] = "";
    int nfile = 0, ret = 0, st;
    FILE *f;

    *mkv_out = NULL;
    if (!mkv || !opts_path) {
        ret = DR_ERR(ENOMEM);
        goto done;
    }
    if ((ret = opts_add(&o, "--output")) < 0 || (ret = opts_add(&o, mkv)) < 0)
        goto done;
    if (getenv("DR_MKV_NO_VARIABLE_DATA") &&
        ((ret = opts_add(&o, "--engage")) < 0 || (ret = opts_add(&o, "no_variable_data")) < 0))
        goto done;
    /* the timestamp files only for a title with a real hole (title_holes(),
     * decided and said when they were written); any other title: the delays
     * as --sync, as with stream files muxed by hand */
    for (int i = 0; use_ts && i < nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        /* LPCM keeps the gaps as silence; another codec without a timestamp file closes them */
        if (out[i].opened && t->type == DISC_MEDIA_AUDIO && !out[i].ts_path && !is_lpcm(t))
            log_text(LOG_WARNING, "Track %d (%s): no timestamp file (how mkvmerge cuts this codec into packets "
                     "is not built in yet): in the MKV file it closes the title's gaps, so after a hole it "
                     "runs ahead of the picture", t->number, codec_id_name(t->codec));
        if (out[i].opened && t->type == DISC_MEDIA_AUDIO && out[i].ts_path && out[i].st.delay_ms < 0)
            log_text(LOG_WARNING, "Track %d starts %"PRId64" ms before the video and the title has holes: its "
                     "timestamp file starts below 0, so mkvmerge moves the whole file later (the video does "
                     "not start at 0) [not handled yet]", t->number, -out[i].st.delay_ms);
    }
    for (int i = 0; i < nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        EsOut *e = &out[i];
        char file[4200];
        if (!e->opened || !e->name)
            continue;
        if (t->second_view) {
            log_text(LOG_WARNING, "Track %d: the second view of a 3-D video is not given to mkvmerge (kept out of the "
                     "MKV file); its file is kept: %s", t->number, e->name);
            continue;
        }
        snprintf(file, sizeof(file), "%s", e->name);
        if (t->codec == DR_CODEC_DVD_SUBTITLE && strlen(file) > 3)
            memcpy(file + strlen(file) - 3, "idx", 3);        /* mkvmerge reads a VobSub pair by its .idx */
        if ((ret = opts_add(&o, "--language")) < 0 || (ret = opts_addf(&o, "0:%s", *e->st.lang ? e->st.lang : "und")) < 0)
            goto done;
        if (t->forced && t->type == DISC_MEDIA_SUBTITLE &&
            ((ret = opts_add(&o, "--forced-display-flag")) < 0 || (ret = opts_add(&o, "0:yes")) < 0))
            goto done;
        /* audio: its timestamp file (when used) holds its start too, else the delay */
        if (t->type == DISC_MEDIA_AUDIO && !(use_ts && e->ts_path) && e->st.delay_ms &&
            ((ret = opts_add(&o, "--sync")) < 0 || (ret = opts_addf(&o, "0:%"PRId64, e->st.delay_ms)) < 0))
            goto done;
        if (use_ts && e->ts_path &&
            ((ret = opts_add(&o, "--timestamps")) < 0 || (ret = opts_addf(&o, "0:%s", e->ts_path)) < 0))
            goto done;
        if (t->type == DISC_MEDIA_SUBTITLE && none &&
            ((ret = opts_add(&o, "--compression")) < 0 || (ret = opts_add(&o, "0:none")) < 0))
            goto done;
        if (dialnorm && t->type == DISC_MEDIA_AUDIO &&
            (t->codec == DR_CODEC_AC3 || t->codec == DR_CODEC_EAC3 || t->codec == DR_CODEC_DTS ||
             t->codec == DR_CODEC_TRUEHD) &&
            ((ret = opts_add(&o, "--remove-dialog-normalization-gain")) < 0 || (ret = opts_add(&o, "0")) < 0))
            goto done;
        if ((ret = opts_add(&o, "(")) < 0 || (ret = opts_add(&o, file)) < 0 || (ret = opts_add(&o, ")")) < 0)
            goto done;
        snprintf(order + strlen(order), sizeof(order) - strlen(order), "%s%d:0", nfile ? "," : "", nfile);
        nfile++;
    }
    if (chap && nb_chapters && ((ret = opts_add(&o, "--chapters")) < 0 || (ret = opts_add(&o, chap)) < 0))
        goto done;
    if (nfile && ((ret = opts_add(&o, "--track-order")) < 0 || (ret = opts_add(&o, order)) < 0))
        goto done;
    if (!(f = fopen(opts_path, "w"))) {
        ret = DR_ERR(errno);
        log_text(LOG_ERROR, "mkvmerge: the options file %s cannot be written: %s", opts_path, strerror(errno));
        goto done;
    }
    if (fprintf(f, "[%s]\n", o.buf ? o.buf : "") < 0 || fclose(f) != 0) {
        ret = DR_ERR(EIO);
        log_text(LOG_ERROR, "mkvmerge: the options file %s cannot be written", opts_path);
        goto done;
    }
    log_text(LOG_INFO, "mkvmerge: %d file%s into %s (options file %s)", nfile, nfile == 1 ? "" : "s", mkv, opts_path);
    st = run_mkvmerge(program, opts_path);
    /* a stop asked for while mkvmerge ran: no MKV file, the stream files are kept */
    if (engine_cancelled()) {
        struct stat sb;
        log_text(LOG_WARNING, "mkvmerge stopped as asked; the stream files are kept");
        if (stat(mkv, &sb) == 0 && unlink(mkv) == 0)
            log_text(LOG_INFO, "mkvmerge: its unfinished file %s is removed", mkv);
        ret = DR_ERR_CANCELLED;
        goto done;
    }
    if (st < 0) {
        log_text(LOG_ERROR, "mkvmerge (%s) cannot be run: %s; the stream files are kept", program, strerror(-st));
        ret = DR_ERR_INVALID;
        goto done;
    }
    if (st >= 2) {
        struct stat sb;
        log_text(LOG_ERROR, "mkvmerge failed (exit status %d, see its lines above); the stream files are kept", st);
        /* mkvmerge leaves what it wrote of its output: no unfinished MKV
         * file stays next to finished ones */
        if (stat(mkv, &sb) == 0 && unlink(mkv) == 0)
            log_text(LOG_INFO, "mkvmerge: its unfinished file %s is removed", mkv);
        ret = DR_ERR_INVALID;
        goto done;
    }
    if (st == 1)
        log_text(LOG_WARNING, "mkvmerge finished with warnings (see its lines above)");
    /* done: the files written for the title go (not the second view's, which
     * mkvmerge was not given: the MKV file does not hold it) */
    {
        int left = 0, kept = 0;
        for (int i = 0; i < nb_tracks; i++) {
            EsOut *e = &out[i];
            if (dr_disc_track(d, i)->second_view) {
                kept += e->opened && e->name;
                continue;
            }
            if (e->opened && e->name) {
                left += unlink(e->name) < 0;
                if (dr_disc_track(d, i)->codec == DR_CODEC_DVD_SUBTITLE && strlen(e->name) > 3) {
                    char idx[4200];
                    snprintf(idx, sizeof(idx), "%s", e->name);
                    memcpy(idx + strlen(idx) - 3, "idx", 3);
                    left += unlink(idx) < 0;
                }
            }
            if (e->ts_path)
                left += unlink(e->ts_path) < 0;
        }
        if (chap)
            unlink(chap);
        left += unlink(opts_path) < 0;
        if (own_dir && !kept && rmdir(dir) < 0)
            log_text(LOG_WARNING, "The folder %s is not empty after the mux: kept", dir);
        log_text(LOG_INFO, "MKV file: %s (mkvmerge); the stream files are deleted%s%s", mkv,
                 left ? " (some could not be)" : "", kept ? " (the second view's file is kept)" : "");
    }
    *mkv_out = mkv;
    mkv = NULL;
done:
    dr_free(o.buf);
    dr_free(mkv);
    dr_free(opts_path);
    return ret;
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
    int64_t title_us = 0;
    /* output.format = mkv: the title goes into one Matroska file (mkvout.c) */
    int mkv = !strcmp(setting_text("output.format"), "mkv");
    /* output.format = mkvmerge: the stream files (with the timestamp files), then mkvmerge muxes them */
    int via = !strcmp(setting_text("output.format"), "mkvmerge");
    int want_ts_setting = setting_bool("output.timestamps_file");
    int want_ts = want_ts_setting || via, ts_track = -1, use_ts = 0;
    MkvOut *mo = NULL;
    char *mkv_path = NULL;
    int64_t mkv_bytes = 0;
    int via_done = 0;

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
        title_us = !ti ? 0 : job->part >= 0 && job->part < ti->nb_parts ? ti->parts[job->part].duration : ti->duration;
        hms(dur, sizeof(dur), title_us > 0 ? title_us / 1000000.0 : 0);
    }
    log_step("=== Title %d/%d: %s %s (%s) ===", job->ordinal, job->count, who + 6, tname ? tname : "", dur);
    dr_disc_set_chapter_rule(d, !setting_bool("chapters.snap"), (int)setting_int("chapters.before_ms"),
                             (int)setting_int("chapters.after_ms"));
    if ((ret = dr_disc_title_open(d, job->title, job->part, job->second_view, job->keep)) < 0) {
        log_msg(MSG_TITLE_OPEN_FAILED, LOG_ERROR, "%s cannot be opened: %s", who, dr_err_text(ret));
        res->failed = 1;
        return ret;
    }
    /* from here every line is this title's own (no repeat hidden) */
    log_title_reading(1);
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
    out = dr_calloc(nb_tracks ? nb_tracks : 1, sizeof(*out));
    if (!out) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        EsOut *o = &out[i];

        o->st.index    = t->number;
        o->title_us    = title_us;
        o->st.kind     = disc_media_name(t->type);
        o->st.codec    = codec_id_name(t->codec);
        o->st.lang     = t->language;
        o->st.file     = "";
        o->st.first_ms = DR_NOPTS_VALUE;
        /* LPCM leaves the disc layer as little-endian PCM: its format (with
         * the channel layout) is set when its first frame comes */
        o->probe_late  = t->codec == DR_CODEC_PCM_DVD || t->codec == DR_CODEC_PCM_BLURAY;
        if (t->type == DISC_MEDIA_AUDIO && !t->channels && !o->probe_late) {
            if (!(o->hold = dr_calloc(HOLD_MAX, sizeof(*o->hold)))) {
                ret = DR_ERR(ENOMEM);
                goto end;
            }
            if (dr_layout_probe_open_track(&o->lp, t) < 0) {
                log_text(LOG_DETAIL, "Track %d: no decoder for %s: its channel layout is not known", t->number,
                         codec_id_name(t->codec));
                o->layout_done = 1;
            }
        }
    }

    if (mkv) {
        if (!(mkv_path = dr_asprintf_str("%s.mkv", prefix))) {
            ret = DR_ERR(ENOMEM);
            goto end;
        }
        if ((ret = mkvout_open(&mo, d, mkv_path, job->title, job->part)) < 0)
            goto end;
        log_text(LOG_DETAIL, "File: %s", mkv_path);
    }
    for (int i = 0; i < nb_tracks && !mkv; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        if (t->second_view) {
            log_text(LOG_DETAIL, "Track %d: the second view of a 3-D video: its details are not checked (its "
                     "pictures are decoded with the base view's)", t->number);
            continue;
        }
        if ((t->type == DISC_MEDIA_VIDEO || t->type == DISC_MEDIA_AUDIO) && !out[i].probe_late &&
            details_probe_open_track(&out[i].probe, t) < 0)
            log_text(LOG_DETAIL, "Track %d: no decoder for %s: its details are not checked", t->number,
                     codec_id_name(t->codec));
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

        if (o->probe_late && !mkv) {
            o->probe_late = 0;
            if (details_probe_open_track(&o->probe, t) < 0)
                log_text(LOG_DETAIL, "Track %d: no decoder for %s: its details are not checked", t->number,
                         codec_id_name(t->codec));
        }
        if (o->probe)
            details_probe_data(o->probe, fr.data, fr.size);

        log_progress(job->title, dr_disc_progress(d) / 10000.0);
        o->st.packets++;
        if (fr.time != DR_NOPTS_VALUE) {
            int64_t ms  = ticks_in(fr.time, 1000);
            int64_t end = ticks_in(fr.time + fr.duration, 1000);
            if (o->st.first_ms == DR_NOPTS_VALUE)
                o->st.first_ms = ms;
            o->st.end_ms = DR_MAX(o->st.end_ms, end);
        }
        /* the timestamp files: each video and audio track's (MKV: the video track in the file) */
        if (want_ts && ts_codec(t) && fr.time != DR_NOPTS_VALUE &&
            (!mkv || (t->type == DISC_MEDIA_VIDEO && (ts_track < 0 || ts_track == fr.track) &&
                      mkvout_has_track(mo, fr.track)))) {
            ts_track = fr.track;
            if ((ret = ts_add(o, t, &fr)) < 0) {
                dr_disc_frame_unref(&fr);
                goto end;
            }
        }
        if (mkv) {
            if ((ret = mkvout_frame(mo, d, &fr)) < 0)
                goto end;
            continue;
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
              t->codec == DR_CODEC_SUBRIP ? srt_write(o, &fr) :
              t->codec == DR_CODEC_HDMV_PGS_SUBTITLE ? sup_write(o, &fr) :
              o->textst ? textst_write(o, &fr) : es_write(o, fr.data, fr.size);
        dr_disc_frame_unref(&fr);
        if (ret < 0)
            goto end;
    }
    if (ret == DR_ERR_EOF)
        ret = 0;
    if (mkv && ret >= 0) {
        /* the file is finished (its chapters are known now); its tracks count as written */
        for (int i = 0; i < nb_tracks; i++) {
            const DrDiscTrack *t = dr_disc_track(d, i);
            out[i].mkv_skip = !mkvout_has_track(mo, i);
            if (mkvout_has_track(mo, i) && out[i].st.packets) {
                out[i].opened = 1;
                out[i].name   = dr_strdup(mkv_path);
                if (t->type == DISC_MEDIA_AUDIO)
                    out[i].st.delay_ms = ticks_in(t->delay_ticks, 1000);
            }
        }
        ret = mkvout_close(&mo, d, 0, &mkv_bytes);
    }
    for (int i = 0; ret >= 0 && !mkv && i < nb_tracks; i++)
        ret = es_flush_hold(dr_disc_track(d, i), &out[i], prefix);
    /* the mkvmerge mode gives mkvmerge the timestamp files only for a title
     * with a real hole: the decision (and why) first, so the files written
     * below are the ones used */
    if (ret >= 0 && via) {
        char why[1024];
        use_ts = title_holes(d, out, nb_tracks, why, sizeof(why));
        if (use_ts)
            log_text(LOG_INFO, "mkvmerge: timestamp files used for this title: it has holes the stream files close "
                     "(%s); with them every track keeps its gaps", why);
        else if (dr_disc_damaged_ranges(d))
            log_text(LOG_INFO, "mkvmerge: damaged data was left out, but no picture is missing and no audio gap is "
                     "left open: no hole, no timestamp files, the delays as --sync");
        else
            log_text(LOG_INFO, "mkvmerge: no hole in this title: no timestamp files, the delays as --sync");
    }
    /* the timestamp files: next to each video and audio stream file (MKV: the video's, next to the MKV file;
     * the mkvmerge mode: only when they are used, unless output.timestamps_file asks for them) */
    for (int i = 0; ret >= 0 && want_ts && !(via && !use_ts && !want_ts_setting) && i < nb_tracks; i++) {
        EsOut *o = &out[i];
        const DrDiscTrack *t = dr_disc_track(d, i);
        char *path;
        int r2;
        /* LPCM fills a gap with silence (its WAV file keeps the title's times): it needs none */
        if (!mkv && !via && o->opened && t->type == DISC_MEDIA_AUDIO && !ts_codec(t) && !is_lpcm(t))
            log_text(LOG_INFO, "Track %d (%s): no timestamp file (how mkvmerge cuts this codec into packets is not "
                     "built in yet): a later mux closes any gap in it", t->number, codec_id_name(t->codec));
        if (!o->nb_ts)
            continue;
        if (mkv)
            path = dr_asprintf_str("%s_timestamps.txt", prefix);
        else {
            const char *dot = o->name ? strrchr(o->name, '.') : NULL;
            path = o->name ? dr_asprintf_str("%.*s_timestamps.txt", (int)(dot ? dot - o->name : (int)strlen(o->name)),
                                             o->name) : NULL;
        }
        if (!path) {
            ret = DR_ERR(ENOMEM);
            break;
        }
        if ((r2 = ts_write(path, o)) < 0)
            log_text(LOG_ERROR, "Track %d: the timestamp file %s could not be written: %s", dr_disc_track(d, i)->number,
                     path, strerror(-r2));
        else
            log_text(LOG_INFO, "Track %d: timestamp file %s (%zu %s)", dr_disc_track(d, i)->number, path,
                     o->nb_ts, t->type == DISC_MEDIA_VIDEO ? "pictures" : "packets");
        if (r2 < 0)
            dr_free(path);
        else
            o->ts_path = path;
    }
    if (ret >= 0 && !mkv) {
        if (!(chap = dr_asprintf_str("%s_chapters.xml", prefix)))
            ret = DR_ERR(ENOMEM);
        else if ((ret = write_chapters(d, chap)) < 0 && ret != DR_ERR(ENOMEM))
            chap_err = -ret;
    }

end:
    if (mo)
        mkvout_close(&mo, d, 1, NULL);
    for (int i = 0; out && i < nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        EsOut *o = &out[i];
        if (o->f && o->wav && ret >= 0) {
            int n = fseek(o->f, 0, SEEK_SET) == 0 ? wav_header(o->f, t, o->st.bytes, o->wav_room) : DR_ERR(errno);
            if (n < 0)
                ret = n;
            else if (n - 8 + o->st.bytes > 0xFFFFFFFFLL) {   /* the RIFF size the 32 bits cannot hold */
                if (o->wav_room)
                    log_text(LOG_INFO, "Track %d: its WAV file is larger than 4 GiB: written as RF64 (the WAV form "
                             "with 64-bit sizes, EBU Tech 3306, which FFmpeg, mkvmerge and VLC read)", t->number);
                else
                    log_text(LOG_WARNING, "Track %d: its WAV file passed 4 GiB, more than the title's length let "
                             "expect: the header's 32-bit sizes are capped (FFmpeg and mkvmerge read to the end; "
                             "a reader that trusts the sizes stops at 4 GiB)", t->number);
            }
        }
        /* text subtitles: the number of dialogs after the style segment */
        if (o->f && o->textst && ret >= 0) {
            uint8_t n[2];
            if (o->dialogs > 0xffff)
                log_text(LOG_WARNING, "Track %d: %"PRId64" text subtitle dialogs: the file's 16-bit count says %d",
                         t->number, o->dialogs, (int)(o->dialogs & 0xffff));
            dr_wb16(n, o->dialogs & 0xffff);
            if (fseek(o->f, 6 + t->extradata_size, SEEK_SET) ||
                fwrite(n, 1, 2, o->f) != 2)
                ret = write_failed(o);
        }
        /* PGS past 13 h 15 min: the .sup cannot hold those times (a 32-bit
         * 90 kHz field in every program that writes or reads one) */
        if (o->sup_wrapped && ret >= 0) {
            char at[32];
            hms(at, sizeof(at), o->sup_wrap_first / (double)DR_TICKS_PER_SECOND);
            log_text(LOG_WARNING, "Track %d: %"PRId64" PGS display set%s from %s on %s past 13:15:21, the most a .sup "
                     "file's 32-bit time can hold: in the .sup their times start again from 0 (every program's .sup "
                     "has this limit)%s; rip the title in parts (--split) or as output.format = mkv to keep them right",
                     t->number, o->sup_wrapped, o->sup_wrapped == 1 ? "" : "s", at, o->sup_wrapped == 1 ? "is" : "are",
                     via ? ", and mkvmerge puts them at those wrong times in the MKV" : "");
        }
        if (o->f && (r = out_close(o, &o->f)) < 0 && ret >= 0)
            ret = r;
        if (o->idx && (r = out_close(o, &o->idx)) < 0 && ret >= 0)
            ret = r;
        /* a forced-only subtitle track that shows nothing is empty: no file
         * (as an MKV leaves such a track out); it may have given no set at
         * all (none shows a forced subtitle) */
        if (ret >= 0 && t->forced && t->has_forced_objects && !t->forced_objects) {
            if (o->opened && remove(o->name) < 0)
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
        /* the MKV file: the writer's error is the file's */
        if (!werr && mkv && mkv_path && (ret == DR_ERR(ENOSPC) || ret == DR_ERR(EDQUOT) || ret == DR_ERR(EIO))) {
            werr  = -ret;
            wfile = mkv_path;
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
        char made[160], sz[32];

        for (int i = 0; i < nb_tracks; i++) {
            const DrDiscTrack *t = dr_disc_track(d, i);
            end_check(t, &out[i]);
            checked      += out[i].checked != 0;
            check_failed += out[i].checked < 0;
            unchecked    += !mkv && out[i].opened && !out[i].probe &&
                            (t->type == DISC_MEDIA_VIDEO || t->type == DISC_MEDIA_AUDIO) && !t->second_view;
        }
        /* mkvmerge: the checked stream files into the MKV file (not when a check failed: they are kept) */
        if (via && !check_failed) {
            char *made_mkv = NULL;
            struct stat sb;
            int mret = mkvmerge_title(d, out, nb_tracks, job->out_dir, dir, job->sub_folder, res->name, chap,
                                      nb_chapters, use_ts, &made_mkv);
            if (mret == DR_ERR_CANCELLED) {
                res->stopped = 1;
                res->failed  = 1;
            } else if (mret < 0) {
                res->failed = 1;
            } else {
                for (int i = 0; i < nb_tracks; i++)
                    if (out[i].opened && !dr_disc_track(d, i)->second_view) {
                        dr_free(out[i].name);
                        out[i].name = dr_strdup(made_mkv);
                    }
                mkv_bytes = stat(made_mkv, &sb) == 0 ? (int64_t)sb.st_size : 0;
                dr_free(made_mkv);
                via_done = 1;
            }
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
                snprintf(state, sizeof(state), "no caption text: %s", mkv ? "not in the MKV" : "no file");
            else if (o->no_forced)
                snprintf(state, sizeof(state), "no forced subtitles: %s", mkv ? "not in the MKV" : "no file");
            else if (mkv && o->mkv_skip)
                snprintf(state, sizeof(state), "not in the MKV: the MKV writer does not take this codec yet");
            else if (!o->opened)
                snprintf(state, sizeof(state), "no data: %s", mkv ? "not in the MKV" : "no file");
            else if (warn)
                snprintf(state, sizeof(state), "%"PRId64" warning%s", warn, warn == 1 ? "" : "s");
            else
                snprintf(state, sizeof(state), "OK");
            if (o->opened && t->type == DISC_MEDIA_AUDIO)
                snprintf(delay, sizeof(delay), mkv ? ", starts at %"PRId64" ms" : ", delay %"PRId64" ms",
                         o->st.delay_ms);
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
        for (int i = 0; i < res->damaged_ranges; i++) {
            char *text = dr_strdup(dr_disc_damage(d, i));
            if (!text || dr_array_add_ptr(&res->damage, &res->nb_damage, text) < 0) {
                dr_free(text);
                break;
            }
        }
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
        if (mkv || via_done)
            written = mkv_bytes;
        snprintf(made, sizeof(made), "(%u chapter%s, %s%s)", (unsigned)nb_chapters, nb_chapters == 1 ? "" : "s",
                 size_text(sz, sizeof(sz), written),
                 mkv || via_done ? "; the MKV file itself is not read back yet" : "");
        if (via && res->stopped) {
            log_msg(MSG_TITLE_FAILED, LOG_WARNING, "%s stopped as asked during the mux: its stream files are kept, "
                    "the MKV file is not made", who);
        } else if (via && !via_done && !check_failed) {
            log_msg(MSG_TITLE_DONE, LOG_ERROR, "%s: its stream files are written, but mkvmerge did not make the MKV "
                    "file (see above)", who);
        } else if (check_failed) {
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
        dr_free(o->hold);
        dr_layout_probe_close(&o->lp);
        details_probe_close(&o->probe);
        dr_free(o->name);
        dr_free(o->ts);
        dr_free(o->ts_path);
    }
    dr_free(out);
    dr_free(chap);
    dr_free(mkv_path);
    dr_disc_title_close(d);
    log_title_reading(0);
    return ret;
}
