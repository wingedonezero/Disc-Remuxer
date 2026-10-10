/*
 * disc-remuxer: the stream files of a title (see writer.h). Each track's
 * packets back to back in a file named <name>_<NN>_[<lang>]_<codec>
 * [_<channel layout>][ (forced)][ (commentary)][ (visually impaired)]
 * [ DELAY <ms>ms].<ext> (the language between brackets and the words in
 * parentheses as the MKVToolNix GUI reads a file name: it splits it at
 * [ ] ( ) { } . + - = #, and finds the delay as "delay <number>");
 * PCM as RIFF WAVE; sub-pictures
 * as a VobSub pair (.sub packs + .idx); the chapters as Matroska XML.
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
#include <libavformat/avformat.h>
#include <libavformat/discinfo.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "log.h"
#include "msg.h"
#include "names.h"
#include "check.h"
#include "engine.h"
#include "settings.h"
#include "writer.h"

static int64_t meta_int(AVDictionary *m, const char *key)
{
    const AVDictionaryEntry *e = av_dict_get(m, key, NULL, 0);
    return e ? strtoll(e->value, NULL, 10) : 0;
}

/* A stream's number as the disc info lists its track (files and logs keep
 * the disc's numbers whichever tracks are left out). */
static int track_no(const AVStream *st)
{
    const AVDictionaryEntry *e = av_dict_get(st->metadata, "disc_track", NULL, 0);
    return e ? atoi(e->value) : st->index;
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
static const char *es_extension(enum AVCodecID id)
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

const char *codec_label(enum AVCodecID id, int profile)
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
    case AV_CODEC_ID_DTS:          return profile == AV_PROFILE_DTS_HD_MA ? "DTS-HD MA" :
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
static int second_view(const AVStream *st)
{
    return st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && (st->disposition & AV_DISPOSITION_DEPENDENT);
}

static const char *stream_label(const AVStream *st, int profile)
{
    return second_view(st) ? "MVC" : codec_label(st->codecpar->codec_id, profile);
}

#define HOLD_MAX 64   /* audio packets kept while their channel layout is not known yet */

typedef struct EsOut {
    StreamResult st;
    char         *name;
    FILE         *f;
    FILE         *idx;              /* sub-pictures: the VobSub index next to the .sub */
    int           no_forced;        /* a forced-only track that showed nothing: its file removed */
    int64_t       sub_pos;          /* bytes written to the .sub */
    int64_t       cues;             /* captions: SRT cues written */
    int           wav;              /* a WAV header to complete at the end */
    int           opened;           /* the file name is chosen */
    int           profile;          /* the codec profile the name was chosen with (e.g. TrueHD + Atmos) */
    char            layout[64];     /* audio: the channel layout in the name ("" unknown) */
    AVCodecContext *avctx;          /* audio: FFmpeg's decoder, for the channel layout */
    AVFrame        *frame;
    int             layout_done;    /* a frame was decoded (or decoding gave up) */
    AVPacket    **hold;
    int           nb_hold;
    DetailsProbe *probe;            /* video / audio: the disc stream's details for the end check */
    int           probe_late;       /* LPCM: the probe opens with the first packet (the stream's format is
                                       the converted PCM's from then on) */
    int           checked;          /* the end check ran: 1 pass, -1 fail, 0 not run */
    char          problems[1024];   /* its findings */
} EsOut;

/* The end check of a written file: its details (read back with FFmpeg's
 * decoders) against the disc stream's, read while ripping. */
static void end_check(const AVStream *st, EsOut *o)
{
    Details disc, file;
    int n;

    if (!o->probe || !o->opened)
        return;
    details_probe_get(o->probe, &disc);
    if (!disc.valid) {
        o->checked = -1;
        snprintf(o->problems, sizeof(o->problems), "the disc stream's first frame could not be decoded");
        return;
    }
    if (details_of_file(o->name, st->codecpar->codec_type, &file) < 0 || !file.valid) {
        o->checked = -1;
        snprintf(o->problems, sizeof(o->problems), "the written file cannot be read back");
        return;
    }
    n = details_compare(&disc, &file, o->problems, sizeof(o->problems));
    o->checked = n ? -1 : 1;
    if (!n)
        snprintf(o->problems, sizeof(o->problems), "%d fields", disc.n);
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
static void wav_header(FILE *f, const AVCodecParameters *par, int64_t data)
{
    int ch = par->ch_layout.nb_channels, bits = par->codec_id == AV_CODEC_ID_PCM_S24LE ? 24 : 16;
    int valid = par->bits_per_coded_sample > 0 && par->bits_per_coded_sample < bits ? par->bits_per_coded_sample : bits;
    int ext = ch > 2 || bits > 16, fmt = ext ? 40 : 16;
    uint64_t riff = 4 + 8 + fmt + 8 + data;

    fputs("RIFF", f); put_le(f, riff > 0xFFFFFFFFu ? 0xFFFFFFFFu : riff, 4); fputs("WAVE", f);
    fputs("fmt ", f); put_le(f, fmt, 4);
    put_le(f, ext ? 0xFFFE : 1, 2); put_le(f, ch, 2); put_le(f, par->sample_rate, 4);
    put_le(f, (uint64_t)par->sample_rate * ch * bits / 8, 4); put_le(f, ch * bits / 8, 2); put_le(f, bits, 2);
    if (ext) {
        static const uint8_t pcm_guid[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                              0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        put_le(f, 22, 2); put_le(f, valid, 2);
        put_le(f, par->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? par->ch_layout.u.mask : 0, 4);
        put_le(f, 1, 2); fwrite(pcm_guid, 1, sizeof(pcm_guid), f);
    }
    fputs("data", f); put_le(f, data > 0xFFFFFFFFu ? 0xFFFFFFFFu : data, 4);
}

/* Matroska chapter XML (mkvmerge --chapters). */
static int write_chapters(const AVFormatContext *ctx, const char *path)
{
    FILE *f;

    if (!ctx->nb_chapters)
        return 0;
    if (!(f = fopen(path, "w")))
        return AVERROR(errno);
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE Chapters SYSTEM \"matroskachapters.dtd\">\n"
               "<Chapters>\n  <EditionEntry>\n");
    for (unsigned i = 0; i < ctx->nb_chapters; i++) {
        const AVChapter *c = ctx->chapters[i];
        const AVDictionaryEntry *t = av_dict_get(c->metadata, "title", NULL, 0);
        int64_t ns = av_rescale_q(c->start, c->time_base, (AVRational){ 1, 1000000000 });
        fprintf(f, "    <ChapterAtom>\n      <ChapterTimeStart>%02"PRId64":%02"PRId64":%02"PRId64".%09"PRId64"</ChapterTimeStart>\n",
                ns / INT64_C(3600000000000), ns / INT64_C(60000000000) % 60, ns / INT64_C(1000000000) % 60, ns % INT64_C(1000000000));
        fprintf(f, "      <ChapterDisplay>\n        <ChapterString>");
        if (t && *t->value) {
            for (const char *p = t->value; *p; p++)
                switch (*p) {
                case '<': fputs("&lt;", f); break;
                case '>': fputs("&gt;", f); break;
                case '&': fputs("&amp;", f); break;
                default:  fputc(*p, f);
                }
        } else {
            fprintf(f, "Chapter %02u", i + 1);
        }
        fprintf(f, "</ChapterString>\n      </ChapterDisplay>\n    </ChapterAtom>\n");
    }
    fprintf(f, "  </EditionEntry>\n</Chapters>\n");
    return fclose(f) ? AVERROR(errno) : 0;
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
            return AVERROR(EIO);
        o->sub_pos += PACK;
        data  += n;
        size  -= n;
        first  = 0;
    }
    return 0;
}

static int vobsub_write(EsOut *o, const AVStream *st, const AVPacket *pkt)
{
    int64_t pts = pkt->pts == AV_NOPTS_VALUE ? 0 : av_rescale_q(pkt->pts, st->time_base, (AVRational){ 1, 90000 });
    int64_t ms  = pkt->pts == AV_NOPTS_VALUE ? 0 : av_rescale_q(pkt->pts, st->time_base, (AVRational){ 1, 1000 });

    if (ms < 0)
        ms = 0;
    fprintf(o->idx, "timestamp: %02"PRId64":%02"PRId64":%02"PRId64":%03"PRId64", filepos: %09"PRIx64"\n",
            ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000, (uint64_t)o->sub_pos);
    o->st.bytes += pkt->size;
    return vobsub_unit(o, pkt->data, pkt->size, pts);
}

/* Chooses the stream's file name and opens it: <prefix>_<NN>_[<lang>]_<codec>
 * [_<channel layout>][ (forced)][ (commentary)][ (visually impaired)]
 * [ DELAY <ms>ms].<ext>; audio carries its start delay (its first time
 * against the title start, which is the video's). */
static int es_open(const AVFormatContext *ctx, const AVStream *st, EsOut *o, const char *prefix, int64_t first)
{
    const AVCodecParameters *par = st->codecpar;
    char layout[64] = "", delay[32] = "", roles[64] = "";
    int audio = par->codec_type == AVMEDIA_TYPE_AUDIO;
    int profile = o->avctx ? o->avctx->profile : par->profile;

    const AVDictionaryEntry *eye = av_dict_get(st->metadata, "eye", NULL, 0);
    const AVDictionaryEntry *dv = av_dict_get(st->metadata, "dolby_vision", NULL, 0);
    int second = second_view(st);

    o->opened  = 1;
    o->profile = profile;
    if (eye) {
        snprintf(layout, sizeof(layout), "_%s eye", eye->value);
        snprintf(o->layout, sizeof(o->layout), "%s eye", eye->value);
    }
    /* Dolby Vision: DV (both layers and the metadata in one file), or the
     * layer the file holds (BL, EL) */
    if (dv) {
        const char *what = !strcmp(dv->value, "BL+EL+RPU") ? "DV" : !strcmp(dv->value, "EL") ? "DV EL" : "DV BL";
        snprintf(layout, sizeof(layout), "_%s", what);
        snprintf(o->layout, sizeof(o->layout), "%s", what);
    }
    if (audio) {
        const AVChannelLayout *cl = par->ch_layout.nb_channels ? &par->ch_layout :
                                    o->frame && o->frame->ch_layout.nb_channels ? &o->frame->ch_layout : NULL;
        if (cl) {
            layout[0] = '_';
            av_channel_layout_describe(cl, layout + 1, sizeof(layout) - 1);
            snprintf(o->layout, sizeof(o->layout), "%s", layout + 1);
        } else {
            log_text(LOG_WARNING, "Track %d: channel layout not known: not in the file name", track_no(st));
        }
        o->st.delay_ms = av_rescale_q_rnd(first, st->time_base, (AVRational){ 1, 1000 }, AV_ROUND_NEAR_INF);
        snprintf(delay, sizeof(delay), " DELAY %"PRId64"ms", o->st.delay_ms);
    }
    snprintf(roles, sizeof(roles), "%s%s%s", st->disposition & AV_DISPOSITION_FORCED ? " (forced)" : "",
             st->disposition & AV_DISPOSITION_COMMENT ? " (commentary)" : "",
             st->disposition & AV_DISPOSITION_VISUAL_IMPAIRED ? " (visually impaired)" : "");
    if (!(o->name = av_asprintf("%s_%02d_[%s]_%s%s%s%s.%s", prefix, track_no(st), *o->st.lang ? o->st.lang : "und",
                                stream_label(st, profile), layout, roles, delay,
                                second ? "mvc" : es_extension(par->codec_id))))
        return AVERROR(ENOMEM);
    o->st.file = o->name;
    if (!(o->f = fopen(o->name, "wb")))
        return AVERROR(errno);
    if (par->codec_id == AV_CODEC_ID_PCM_S16LE || par->codec_id == AV_CODEC_ID_PCM_S24LE) {
        o->wav = 1;
        wav_header(o->f, par, 0);
        if (par->bits_per_coded_sample > 16 && par->bits_per_coded_sample < 24)
            log_text(LOG_INFO, "Track %d: %d-bit samples in 24-bit words, as on the disc (the WAV header says %d "
                     "valid bits) [untested on real discs]", track_no(st), par->bits_per_coded_sample,
                     par->bits_per_coded_sample);
    }
    if (par->codec_id == AV_CODEC_ID_DVD_SUBTITLE) {
        char *idx = av_strdup(o->name);
        if (!idx)
            return AVERROR(ENOMEM);
        memcpy(idx + strlen(idx) - 3, "idx", 3);
        o->idx = fopen(idx, "w");
        av_free(idx);
        if (!o->idx)
            return AVERROR(errno);
        /* a VobSub index starts with its format line (readers check it) */
        if (par->extradata_size < 22 || memcmp(par->extradata, "# VobSub index file, v", 22))
            fputs("# VobSub index file, v7 (do not modify this line!)\n", o->idx);
        if (par->extradata_size)
            fwrite(par->extradata, 1, par->extradata_size, o->idx);
        fprintf(o->idx, "\n# %s\nid: %s, index: 0\n", *o->st.lang ? o->st.lang : "und", *o->st.lang ? o->st.lang : "und");
    }
    (void)ctx;
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
static int srt_write(EsOut *o, const AVStream *st, const AVPacket *pkt)
{
    int64_t start = av_rescale_q(pkt->pts, st->time_base, (AVRational){ 1, 1000 });
    int64_t end   = av_rescale_q(pkt->pts + pkt->duration, st->time_base, (AVRational){ 1, 1000 });

    fprintf(o->f, "%"PRId64"\n", ++o->cues);
    srt_time(o->f, start);
    fputs(" --> ", o->f);
    srt_time(o->f, end);
    fputc('\n', o->f);
    if (fwrite(pkt->data, 1, pkt->size, o->f) != (size_t)pkt->size || fputs("\n\n", o->f) < 0)
        return AVERROR(EIO);
    o->st.bytes += pkt->size;
    return 0;
}

/* A PGS display set as .sup: each segment after a "PG" header with the
 * set's time (90 kHz, 32 bits) and a decode time of 0 (a reader shows a set
 * at its composition segment's time; the decode times only schedule a
 * player's decoder). */
static int sup_write(EsOut *o, const AVStream *st, const AVPacket *pkt)
{
    uint32_t pts = (uint32_t)av_rescale_q(pkt->pts, st->time_base, (AVRational){ 1, 90000 });
    int off = 0;

    while (off + 3 <= pkt->size) {
        int len = 3 + AV_RB16(pkt->data + off + 1);
        uint8_t h[10] = { 'P', 'G' };
        if (off + len > pkt->size)
            return AVERROR_INVALIDDATA;
        AV_WB32(h + 2, pts);
        if (fwrite(h, 1, sizeof(h), o->f) != sizeof(h) || fwrite(pkt->data + off, 1, len, o->f) != (size_t)len)
            return AVERROR(EIO);
        o->st.bytes += sizeof(h) + len;
        off += len;
    }
    return off == pkt->size ? 0 : AVERROR_INVALIDDATA;
}

static int es_write(EsOut *o, const AVPacket *pkt)
{
    if (o->idx)
        return AVERROR_BUG;   /* sub-pictures go through vobsub_write() */
    if (fwrite(pkt->data, 1, pkt->size, o->f) != (size_t)pkt->size)
        return AVERROR(EIO);
    o->st.bytes += pkt->size;
    return 0;
}

static int es_flush_hold(const AVFormatContext *ctx, const AVStream *st, EsOut *o, const char *prefix)
{
    int ret = 0;

    if (!o->opened && o->nb_hold && (ret = es_open(ctx, st, o, prefix, o->hold[0]->pts)) < 0)
        return ret;
    for (int i = 0; i < o->nb_hold; i++) {
        if (ret >= 0)
            ret = es_write(o, o->hold[i]);
        av_packet_free(&o->hold[i]);
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
 * the keydb folder next to the program in name order; joined by commas into
 * bp. */
static void key_files(AVBPrint *bp)
{
    const char *const *keys;
    int nb = setting_list("aacs.key_files", &keys);

    if (nb) {
        for (int i = 0; i < nb; i++)
            av_bprintf(bp, "%s%s", i ? "," : "", keys[i]);
    } else {
        char dir[PATH_MAX];
        struct dirent **list;
        snprintf(dir, sizeof(dir), "%s/keydb", program_dir());
        if ((nb = scandir(dir, &list, keydb_file, alphasort)) < 0)
            return;
        for (int i = 0; i < nb; i++) {
            av_bprintf(bp, "%s%s/%s", bp->len ? "," : "", dir, list[i]->d_name);
            free(list[i]);
        }
        free(list);
    }
}

int open_disc(const char *format, const char *source, AVFormatContext **ctx)
{
    const AVInputFormat *fmt = av_find_input_format(format);
    AVDictionary *opts = NULL;
    AVBPrint bp;
    int ret;

    *ctx = NULL;
    if (!fmt)
        return AVERROR_DEMUXER_NOT_FOUND;
    /* the disc info only: the titles are opened on it one by one */
    av_dict_set_int(&opts, "title", -1, 0);
    av_dict_set(&opts, "dolby_vision", setting_text("bluray.dolby_vision"), 0);
    av_dict_set_int(&opts, "read_attempts", setting_int("read.attempts"), 0);
    av_dict_set_int(&opts, "min_length", setting_int("titles.min_length"), 0);
    av_dict_set(&opts, "udf_reader", setting_text("read.udf_reader"), 0);
    av_dict_set_int(&opts, "prefer_iso_for_old_udf102", setting_bool("read.prefer_iso_for_old_udf102"), 0);
    av_dict_set(&opts, "dvd_cell_mode", setting_text("dvd.cell_mode"), 0);
    av_dict_set(&opts, "dvd_cell_check", setting_text("dvd.cell_check"), 0);
    av_dict_set(&opts, "video_language", setting_text("output.video_language"), 0);
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    key_files(&bp);
    if (bp.len)
        av_dict_set(&opts, "keydb", bp.str, 0);
    av_bprint_finalize(&bp, NULL);
    ret = avformat_open_input(ctx, source, fmt, &opts);
    av_dict_free(&opts);
    return ret;
}


/* A video track's details from the demuxer: line-21 caption data taken
 * out of the video and not kept (a format without caption tracks), with
 * the disc's caption marks. A warning when the data carried caption text. */
static void video_detail(const AVStream *st, char *json, size_t size)
{
    const AVDictionaryEntry *l21 = av_dict_get(st->metadata, "line21", NULL, 0);
    /* the caption track left out as asked: its data is not lost by mistake */
    int asked = !!av_dict_get(st->metadata, "caption_track", NULL, 0);
    int64_t blocks = meta_int(st->metadata, "cc_blocks"), text = meta_int(st->metadata, "cc_text");
    char jl[64];

    if (blocks)
        log_text(text && !asked ? LOG_WARNING : LOG_INFO, "      line-21 caption data taken out of the video, not "
                 "kept%s: %"PRId64" caption blocks, %s%s%s%s", asked ? " (the caption track is not selected)" : "",
                 blocks, text ? "WITH caption text" : "no caption text", l21 ? " (the disc marks captions on " : "",
                 l21 ? l21->value : "", l21 ? ")" : "");
    else if (l21)
        log_text(LOG_INFO, "      the disc marks line-21 captions on %s, but the video carries none", l21->value);
    if (blocks || l21)
        snprintf(json, size, ",\"line21\":%s,\"cc_blocks\":%"PRId64",\"cc_text\":%"PRId64,
                 json_str(jl, sizeof(jl), l21 ? l21->value : ""), blocks, text);
}

/* A subtitle track's details from the demuxer: what the units are, the
 * canvas they are drawn on and whether every unit fits it, and how many had
 * no stop time (shown until the next one). One line in the log; the same
 * as JSON fields appended to the track's record (json, starting with a comma). */
static void subtitle_detail(const AVStream *st, int opened, char *json, size_t size)
{
    const AVDictionaryEntry *cap = av_dict_get(st->metadata, "caption", NULL, 0);
    const AVDictionaryEntry *pic = av_dict_get(st->metadata, "picture", NULL, 0);
    if (cap) {
        int64_t blocks = meta_int(st->metadata, "cc_blocks"), cues = meta_int(st->metadata, "cues");
        int64_t lost = meta_int(st->metadata, "cc_lost");
        char jp[256], jl[96] = "";
        log_text(LOG_INFO, "      %s; %"PRId64" caption blocks -> %"PRId64" cues%s", cap->value, blocks, cues,
                 blocks && !cues ? " (the caption data carries no caption text)" : "");
        if (lost) {
            log_text(LOG_INFO, "      %"PRId64" caption(s) still shown at the end of a segment left out", lost);
            snprintf(jl, sizeof(jl), ",\"cc_lost\":%"PRId64, lost);
        }
        snprintf(json, size, ",\"caption\":%s,\"cc_blocks\":%"PRId64",\"cues\":%"PRId64"%s",
                 json_str(jp, sizeof(jp), cap->value), blocks, cues, jl);
        return;
    }
    int64_t no_stop = meta_int(st->metadata, "no_stop"), outside = meta_int(st->metadata, "outside");
    int w = st->codecpar->width, h = st->codecpar->height;
    char jp[256];
    AVBPrint b;

    av_bprint_init(&b, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&b, "%s", pic ? pic->value : "sub-pictures");
    if (w > 0 && h > 0) {
        av_bprintf(&b, "; drawn on %dx%d", w, h);
        if (opened && outside)
            av_bprintf(&b, ", %"PRId64" outside it", outside);
        else if (opened)
            av_bprintf(&b, ", all inside it");
    }
    if (no_stop)
        av_bprintf(&b, "; %"PRId64" without a stop time, each shown until the next one", no_stop);
    if (av_dict_get(st->metadata, "forced_objects", NULL, 0))
        av_bprintf(&b, "; %"PRId64" forced object%s", meta_int(st->metadata, "forced_objects"),
                   meta_int(st->metadata, "forced_objects") == 1 ? "" : "s");
    log_text(outside ? LOG_WARNING : LOG_INFO, "      %s", b.str);
    av_bprint_finalize(&b, NULL);
    snprintf(json, size, ",\"picture\":%s,\"width\":%d,\"height\":%d,\"no_stop\":%"PRId64",\"outside\":%"PRId64,
             json_str(jp, sizeof(jp), pic ? pic->value : ""), w, h, no_stop, outside);
}

static void hms(char *buf, size_t size, double s)
{
    int64_t t = (int64_t)(s + 0.5);
    snprintf(buf, size, "%d:%02d:%02d", (int)(t / 3600), (int)(t / 60 % 60), (int)(t % 60));
}

int demux_title(const TitleJob *job, TitleOutcome *res)
{
    AVFormatContext *ctx = job->disc;
    const DiscInfo *info;
    const char *tname;
    EsOut *out = NULL;
    AVPacket *pkt = NULL;
    char dir[4096], prefix[4200], dur[32], js[4300], who[64], base[512], jpart[32] = "";
    int ret;

    memset(res, 0, sizeof(*res));
    /* "Title #8", or "Title #8 part 2/7" for a part of a title */
    if (job->part >= 0) {
        snprintf(who, sizeof(who), "Title #%d part %d/%d", job->title, job->part + 1, job->nb_parts);
        snprintf(jpart, sizeof(jpart), ",\"part\":%d", job->part + 1);
    } else
        snprintf(who, sizeof(who), "Title #%d", job->title);
    if ((ret = disc_title_open(ctx, job->title, job->part, job->second_view, job->keep)) < 0) {
        log_step("=== Title %d/%d: %s ===", job->ordinal, job->count, who + 6);
        log_msg(MSG_TITLE_OPEN_FAILED, LOG_ERROR, "%s cannot be opened: %s", who, av_err2str(ret));
        res->failed = 1;
        return ret;
    }
    info = disc_info(ctx);
    tname = info && job->title < info->nb_titles ? info->titles[job->title].name : NULL;
    title_name(setting_text("output.file_name_template"), tname, NULL, NULL, job->title, base, sizeof(base));
    /* a part's files: the title's name + _p<n>, in the title's folder */
    if (job->part >= 0)
        snprintf(res->name, sizeof(res->name), "%.500s_p%d", base, job->part + 1);
    else
        snprintf(res->name, sizeof(res->name), "%s", base);
    hms(dur, sizeof(dur), ctx->duration > 0 ? ctx->duration / (double)AV_TIME_BASE : 0);
    log_step("=== Title %d/%d: %s %s (%s) ===", job->ordinal, job->count, who + 6, tname ? tname : "", dur);
    log_json("title_start", "\"title\":%d%s,\"name\":%s,\"ordinal\":%d,\"count\":%d", job->title, jpart,
             json_str(js, sizeof(js), res->name), job->ordinal, job->count);

    /* a part is written as a title of its own: its own folder, like every title's */
    snprintf(dir, sizeof(dir), "%s%s%s", job->out_dir, job->sub_folder ? "/" : "", job->sub_folder ? res->name : "");
    if (mkdir(dir, 0777) < 0 && errno != EEXIST) {
        ret = AVERROR(errno);
        log_msg(MSG_FOLDER_FAILED, LOG_ERROR, "The folder %s cannot be created: %s", dir, strerror(errno));
        goto end;
    }
    snprintf(prefix, sizeof(prefix), "%s/%s", dir, res->name);
    log_text(LOG_DETAIL, "Files: %s_*", prefix);

    out = av_calloc(ctx->nb_streams, sizeof(*out));
    pkt = av_packet_alloc();
    if (!out || !pkt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (unsigned i = 0; i < ctx->nb_streams; i++) {
        const AVStream *st = ctx->streams[i];
        const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        EsOut *o = &out[i];

        o->st.index    = track_no(st);
        o->st.kind     = av_get_media_type_string(st->codecpar->codec_type);
        o->st.codec    = avcodec_get_name(st->codecpar->codec_id);
        o->st.lang     = lang ? lang->value : "";
        o->st.file     = "";
        o->st.first_ms = AV_NOPTS_VALUE;
        /* LPCM leaves the demuxer as little-endian PCM: its format (with the
         * channel layout) is set when its first packet comes */
        o->probe_late  = st->codecpar->codec_id == AV_CODEC_ID_PCM_DVD ||
                         st->codecpar->codec_id == AV_CODEC_ID_PCM_BLURAY;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !st->codecpar->ch_layout.nb_channels &&
            !o->probe_late) {
            const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
            if (!(o->hold = av_calloc(HOLD_MAX, sizeof(*o->hold)))) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            if (!dec || !(o->avctx = avcodec_alloc_context3(dec)) || !(o->frame = av_frame_alloc()) ||
                avcodec_open2(o->avctx, dec, NULL) < 0) {
                log_text(LOG_DETAIL, "Track %d: no decoder for %s: its channel layout is not known", track_no(st),
                         avcodec_get_name(st->codecpar->codec_id));
                avcodec_free_context(&o->avctx);
                o->layout_done = 1;
            }
        }
    }

    for (unsigned i = 0; i < ctx->nb_streams; i++) {
        enum AVMediaType t = ctx->streams[i]->codecpar->codec_type;
        if (second_view(ctx->streams[i])) {
            log_text(LOG_DETAIL, "Track %d: the second view of a 3-D video: its details are not checked (its "
                     "pictures are decoded with the base view's)", track_no(ctx->streams[i]));
            continue;
        }
        if ((t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO) && !out[i].probe_late &&
            details_probe_open(&out[i].probe, ctx->streams[i]->codecpar) < 0)
            log_text(LOG_DETAIL, "Track %d: no decoder for %s: its details are not checked", track_no(ctx->streams[i]),
                     avcodec_get_name(ctx->streams[i]->codecpar->codec_id));
    }
    while ((ret = av_read_frame(ctx, pkt)) >= 0) {
        /* a stop asked for (engine_cancel()): the title ends here */
        if (engine_cancelled()) {
            av_packet_unref(pkt);
            ret = AVERROR_EXIT;
            res->stopped = 1;
            goto end;
        }
        const AVStream *st = ctx->streams[pkt->stream_index];
        EsOut *o = &out[pkt->stream_index];
        int64_t progress;

        if (o->probe_late) {
            o->probe_late = 0;
            if (details_probe_open(&o->probe, st->codecpar) < 0)
                log_text(LOG_DETAIL, "Track %d: no decoder for %s: its details are not checked", track_no(st),
                         avcodec_get_name(st->codecpar->codec_id));
        }
        if (o->probe)
            details_probe_packet(o->probe, pkt);

        if (av_opt_get_int(ctx, "progress", AV_OPT_SEARCH_CHILDREN, &progress) >= 0)
            log_progress(job->title, progress / 10000.0);
        o->st.packets++;
        if (pkt->pts != AV_NOPTS_VALUE) {
            int64_t ms  = av_rescale_q(pkt->pts, st->time_base, (AVRational){ 1, 1000 });
            int64_t end = av_rescale_q(pkt->pts + pkt->duration, st->time_base, (AVRational){ 1, 1000 });
            if (o->st.first_ms == AV_NOPTS_VALUE)
                o->st.first_ms = ms;
            o->st.end_ms = FFMAX(o->st.end_ms, end);
        }
        if (!o->opened && o->hold) {
            if (!o->layout_done && avcodec_send_packet(o->avctx, pkt) >= 0 &&
                avcodec_receive_frame(o->avctx, o->frame) >= 0)
                o->layout_done = 1;
            if (!(o->hold[o->nb_hold++] = av_packet_clone(pkt))) {
                ret = AVERROR(ENOMEM);
                av_packet_unref(pkt);
                goto end;
            }
            if ((o->layout_done || o->nb_hold == HOLD_MAX) && (ret = es_flush_hold(ctx, st, o, prefix)) < 0) {
                av_packet_unref(pkt);
                goto end;
            }
            av_packet_unref(pkt);
            continue;
        }
        if (!o->opened && (ret = es_open(ctx, st, o, prefix, pkt->pts)) < 0) {
            av_packet_unref(pkt);
            goto end;
        }
        ret = o->idx ? vobsub_write(o, st, pkt) :
              st->codecpar->codec_id == AV_CODEC_ID_SUBRIP ? srt_write(o, st, pkt) :
              st->codecpar->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE ? sup_write(o, st, pkt) : es_write(o, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    if (ret == AVERROR_EOF)
        ret = 0;
    for (unsigned i = 0; ret >= 0 && i < ctx->nb_streams; i++)
        ret = es_flush_hold(ctx, ctx->streams[i], &out[i], prefix);
    if (ret >= 0) {
        char *chap = av_asprintf("%s_chapters.xml", prefix);
        ret = chap ? write_chapters(ctx, chap) : AVERROR(ENOMEM);
        av_free(chap);
    }

end:
    for (unsigned i = 0; ctx && out && i < ctx->nb_streams; i++) {
        EsOut *o = &out[i];
        if (o->f && o->wav && ret >= 0) {
            if (fseek(o->f, 0, SEEK_SET) == 0)
                wav_header(o->f, ctx->streams[i]->codecpar, o->st.bytes);
            else
                ret = AVERROR(errno);
        }
        if (o->f && fclose(o->f) && ret >= 0)
            ret = AVERROR(errno);
        if (o->idx && fclose(o->idx) && ret >= 0)
            ret = AVERROR(errno);
        o->f = NULL;
        /* a forced-only subtitle track that shows nothing is empty: no file
         * (as an MKV leaves such a track out) */
        if (ret >= 0 && o->opened && (ctx->streams[i]->disposition & AV_DISPOSITION_FORCED) &&
            av_dict_get(ctx->streams[i]->metadata, "forced_objects", NULL, 0) &&
            !meta_int(ctx->streams[i]->metadata, "forced_objects")) {
            if (remove(o->name) < 0)
                ret = AVERROR(errno);
            o->opened = 0;
            o->no_forced = 1;
        }
    }
    if (res->stopped) {
        log_msg(MSG_TITLE_FAILED, LOG_WARNING, "%s stopped as asked: its files are not complete", who);
        res->failed = 1;
    } else if (ret < 0) {
        log_msg(MSG_TITLE_FAILED, LOG_ERROR, "%s failed: %s", who, av_err2str(ret));
        res->failed = 1;
    } else {
        int64_t untested = meta_int(ctx->metadata, "untested");
        int checked = 0, check_failed = 0, unchecked = 0;

        for (unsigned i = 0; i < ctx->nb_streams; i++) {
            enum AVMediaType t = ctx->streams[i]->codecpar->codec_type;
            end_check(ctx->streams[i], &out[i]);
            checked      += out[i].checked != 0;
            check_failed += out[i].checked < 0;
            unchecked    += out[i].opened && !out[i].probe && (t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO) &&
                            !second_view(ctx->streams[i]);
        }
        log_text(LOG_INFO, "Tracks:");
        for (unsigned i = 0; i < ctx->nb_streams; i++) {
            const AVStream *st = ctx->streams[i];
            EsOut *o = &out[i];
            int64_t frames = meta_int(st->metadata, "frames"), warn = meta_int(st->metadata, "warnings");
            char what[160], state[96], delay[48] = "", jc[64], jl[32], jy[96], jf[4300], extra[2048] = "";
            const char *unit = st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE ? "subtitles" : "frames";
            int empty_cc = !o->opened && av_dict_get(st->metadata, "caption", NULL, 0);

            snprintf(what, sizeof(what), "%s %s%s%s", *o->st.lang ? o->st.lang : "   ",
                     stream_label(st, o->opened ? o->profile : st->codecpar->profile), *o->layout ? " " : "", o->layout);
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
            if (o->opened && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                snprintf(delay, sizeof(delay), ", delay %"PRId64" ms", o->st.delay_ms);
            log_text(!o->opened && !empty_cc && !o->no_forced ? LOG_WARNING : warn || empty_cc || o->no_forced ?
                     LOG_INFO : LOG_OK,
                     "  %02d  %-30s %8"PRId64" %-9s  %s%s",
                     track_no(st), what, frames, unit, state, delay);
            if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
                subtitle_detail(st, o->opened, extra, sizeof(extra));
            else if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                video_detail(st, extra, sizeof(extra));
            if (o->checked < 0)
                log_text(LOG_ERROR, "      the file's stream details differ from the disc stream's: %s", o->problems);
            if (o->checked) {
                char jp[1100];
                size_t l = strlen(extra);
                snprintf(extra + l, sizeof(extra) - l, ",\"details_check\":\"%s\",\"details\":%s",
                         o->checked > 0 ? "pass" : "fail", json_str(jp, sizeof(jp), o->problems));
            }
            if (o->opened)
                log_text(LOG_DETAIL, "      %s", o->name);
            res->warnings += warn;
            log_json("track", "\"title\":%d%s,\"index\":%d,\"codec\":%s,\"language\":%s,\"layout\":%s,"
                     "\"frames\":%"PRId64",\"warnings\":%"PRId64",\"delay_ms\":%"PRId64",\"file\":%s%s", job->title,
                     jpart, track_no(st), json_str(jc, sizeof(jc), stream_label(st, o->opened ? o->profile : st->codecpar->profile)),
                     json_str(jl, sizeof(jl), o->st.lang), json_str(jy, sizeof(jy), o->layout), frames, warn,
                     o->st.delay_ms, json_str(jf, sizeof(jf), o->opened ? o->name : ""), extra);
        }
        if (checked)
            log_text(check_failed ? LOG_ERROR : LOG_OK, "Stream details (read back from the files): %d of %d "
                     "video / audio track%s match the disc streams%s", checked - check_failed, checked,
                     checked == 1 ? "" : "s", check_failed ? "" : ": PASS");
        if (unchecked)
            log_text(LOG_WARNING, "Stream details: %d video / audio track%s not checked (no decoder)", unchecked,
                     unchecked == 1 ? "" : "s");
        res->warnings += unchecked;
        log_msg(MSG_CHAPTERS, LOG_INFO, "Chapters: %u", ctx->nb_chapters);
        if (check_failed) {
            log_msg(MSG_DETAILS_DIFFER, LOG_ERROR, "%s: its files are written, but %d of them do not keep "
                    "the disc stream's details (see above)", who, check_failed);
            res->failed = 1;
        } else if (untested) {
            /* written: the untested paths are said in the log ("[untested on
             * real discs]"), a warning each */
            res->warnings += untested;
            log_msg(MSG_UNTESTED, LOG_WARNING, "%s: done with %"PRId64" warning%s, %"PRId64" of them feature(s) no "
                    "real disc has tested yet (see the log's \"[untested on real discs]\" lines)", who,
                    res->warnings, res->warnings == 1 ? "" : "s", untested);
        } else if (res->warnings) {
            log_msg(MSG_TITLE_DONE, LOG_INFO, "%s: done with %"PRId64" warning%s", who, res->warnings,
                    res->warnings == 1 ? "" : "s");
        } else {
            log_msg(MSG_TITLE_DONE, LOG_OK, "%s: done", who);
        }
    }
    log_json("title_result", "\"title\":%d%s,\"status\":\"%s\",\"warnings\":%"PRId64",\"folder\":%s", job->title, jpart,
             res->stopped ? "stopped" : res->failed ? "failed" : "ok", res->warnings, json_str(js, sizeof(js), dir));
    for (unsigned i = 0; ctx && out && i < ctx->nb_streams; i++) {
        EsOut *o = &out[i];
        for (int k = 0; k < o->nb_hold; k++)
            av_packet_free(&o->hold[k]);
        av_free(o->hold);
        av_frame_free(&o->frame);
        avcodec_free_context(&o->avctx);
        details_probe_close(&o->probe);
        av_free(o->name);
    }
    av_free(out);
    av_packet_free(&pkt);
    disc_title_close(ctx);
    return ret;
}
