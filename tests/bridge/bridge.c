/*
 * The C side of the test bridge: the headers the tests call into, and a few
 * helpers for what is awkward to do from Python (FFmpeg's varargs log
 * callback, running a parser the way libavformat does).
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/codec_desc.h"
#include "libavformat/avformat.h"
#include "libavformat/demux.h"
#include "libavutil/aes.h"
#include "libavutil/bprint.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "libavformat/bluray_internal.h"
#include "libavformat/disclang.h"
#include "libavformat/discio.h"
#include "libavformat/disctl.h"
#include "libavformat/dvd_internal.h"
#include "libavformat/hddvd_internal.h"

#include <dvdread/dvd_udf.h>
#include <dvdnav/dvdnav.h>

#include "dvdnav_selftest.h"
#include <expat.h>
#include <gcrypt.h>
#include <libaacs/aacs.h>
#include <libaacs/aacs-version.h>

typedef void (*tb_log_sink)(int level, const char *line);

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static tb_log_sink log_sink;
static int log_max_level = AV_LOG_INFO;
static char log_pending[4096];
static size_t log_pending_len;
static int log_print_prefix = 1;

/* FFmpeg writes a line in several calls: pieces are collected until the
 * newline, then the whole line goes to the sink with the level of its last
 * piece. */
static void log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    char part[2048];
    size_t len;

    if (level > log_max_level)
        return;
    pthread_mutex_lock(&log_lock);
    av_log_format_line2(avcl, level, fmt, vl, part, sizeof(part), &log_print_prefix);
    len = strlen(part);
    if (len > sizeof(log_pending) - 1 - log_pending_len)
        len = sizeof(log_pending) - 1 - log_pending_len;
    memcpy(log_pending + log_pending_len, part, len);
    log_pending_len += len;
    log_pending[log_pending_len] = 0;
    if (log_pending_len > 0 && log_pending[log_pending_len - 1] == '\n') {
        log_pending[--log_pending_len] = 0;
        if (log_sink)
            log_sink(level, log_pending);
        log_pending_len = 0;
    }
    pthread_mutex_unlock(&log_lock);
}

/* Route FFmpeg's log (and the libraries' through it) to sink, up to
 * max_level. */
static void tb_log_install(tb_log_sink sink, int max_level)
{
    pthread_mutex_lock(&log_lock);
    log_sink = sink;
    log_max_level = max_level;
    pthread_mutex_unlock(&log_lock);
    av_log_set_level(max_level);
    av_log_set_callback(log_callback);
}

typedef void (*tb_parser_frame_cb)(void *opaque, int size, int64_t pts, int64_t dts);

/* Runs FFmpeg's parser for the codec named codec_name (a codec descriptor
 * name such as "ac3") over nb packets the way libavformat does: each
 * packet's timestamp is passed on its first parser call only, the rest of the
 * packet follows without one, and the parser is flushed at the end. cb gets
 * every frame the parser returns with the timestamps the parser gave it.
 * Returns 0 or a negative AVERROR. */
static int tb_parser_run(const char *codec_name, const uint8_t *const *data,
                         const int *sizes, const int64_t *pts, int nb,
                         tb_parser_frame_cb cb, void *opaque)
{
    const AVCodecDescriptor *desc = avcodec_descriptor_get_by_name(codec_name);
    AVCodecParserContext *parser;
    AVCodecContext *avctx;
    int64_t pos = 0;
    uint8_t *out;
    int out_size;

    if (!desc)
        return AVERROR_DECODER_NOT_FOUND;
    parser = av_parser_init(desc->id);
    if (!parser)
        return AVERROR(ENOSYS);
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        av_parser_close(parser);
        return AVERROR(ENOMEM);
    }
    avctx->codec_id   = desc->id;
    avctx->codec_type = desc->type;

    for (int i = 0; i < nb; i++) {
        const uint8_t *p = data[i];
        int left = sizes[i];
        int64_t t = pts[i], packet_pos = pos;

        pos += sizes[i];
        while (left > 0) {
            int used = av_parser_parse2(parser, avctx, &out, &out_size, p, left,
                                        t, t, packet_pos);
            t          = AV_NOPTS_VALUE;
            packet_pos = -1;
            p    += used;
            left -= used;
            if (out_size)
                cb(opaque, out_size, parser->pts, parser->dts);
        }
    }
    do {
        av_parser_parse2(parser, avctx, &out, &out_size, NULL, 0,
                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);
        if (out_size)
            cb(opaque, out_size, parser->pts, parser->dts);
    } while (out_size);

    av_parser_close(parser);
    avcodec_free_context(&avctx);
    return 0;
}

/* The versions our library headers give (the linked libraries must match). */
static const char *tb_gcrypt_header_version(void) { return GCRYPT_VERSION; }
static const char *tb_aacs_header_version(void) { return AACS_VERSION_STRING; }

/* Runs FFmpeg's MPEG-PS demuxer (mpeg.c; `which`: 0 as it is, 1 as disc sectors with the
 * LPCM header length) over the bytes in memory, the way the disc demuxers do (no
 * parsing, no filled-in times); each packet goes to cb with its stream id,
 * the codec the demuxer gave the stream, its bytes and pts. Returns the
 * number of packets, or an AVERROR. */
typedef void (*tb_ps_packet_cb)(void *opaque, int id, int codec, const uint8_t *data, int size, int64_t pts);

typedef struct TbMem { const uint8_t *data; int size, pos; } TbMem;

static int tb_mem_read(void *opaque, uint8_t *buf, int n)
{
    TbMem *m = opaque;
    int k = FFMIN(n, m->size - m->pos);
    if (k <= 0)
        return AVERROR_EOF;
    memcpy(buf, m->data + m->pos, k);
    m->pos += k;
    return k;
}

static int tb_ps_read(int which, int lpcm_header, const uint8_t *data, int size, tb_ps_packet_cb cb, void *opaque)
{
    extern const FFInputFormat ff_mpegps_demuxer;
    const FFInputFormat *f = &ff_mpegps_demuxer;
    TbMem m = { data, size, 0 };
    AVFormatContext *s = avformat_alloc_context();
    uint8_t *buf = av_malloc(4096);
    AVPacket *pkt = av_packet_alloc();
    AVDictionary *opts = NULL;
    int n = 0, ret;

    if (!s || !buf || !pkt || !(s->pb = avio_alloc_context(buf, 4096, 0, &m, tb_mem_read, NULL, NULL))) {
        av_free(buf);
        ret = AVERROR(ENOMEM);
        goto end;
    }
    s->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
    s->probesize = 0;
    if (which) {                /* disc sectors */
        av_dict_set_int(&opts, "disc", 1, 0);
        av_dict_set_int(&opts, "lpcm_header", lpcm_header, 0);
    }
    ret = avformat_open_input(&s, "", &f->p, &opts);
    av_dict_free(&opts);
    if (ret < 0)
        goto end;
    while ((ret = av_read_frame(s, pkt)) >= 0) {
        AVStream *st = s->streams[pkt->stream_index];
        cb(opaque, st->id, st->codecpar->codec_id, pkt->data, pkt->size, pkt->pts);
        av_packet_unref(pkt);
        n++;
    }
    ret = ret == AVERROR_EOF ? n : ret;
end:
    if (s && s->pb) {
        av_freep(&s->pb->buffer);
        avio_context_free(&s->pb);
    }
    avformat_close_input(&s);
    av_packet_free(&pkt);
    return ret;
}

/* FFmpeg's MPEG-TS demuxer (mpegts.c) in disc mode over the bytes in
 * memory, or over the first max_bytes of the file at path (data NULL), for
 * the PIDs given; each payload piece goes to cb with its stream id (PID |
 * PES key << 16), bytes, pts and byte position. Returns the number of
 * pieces, or an AVERROR. */
typedef void (*tb_ts_packet_cb)(void *opaque, int id, const uint8_t *data, int size, int64_t pts, int64_t pos);

typedef struct TbFile { FILE *f; int64_t left; } TbFile;

static int tb_file_read(void *opaque, uint8_t *buf, int n)
{
    TbFile *t = opaque;
    size_t k;
    if (t->left <= 0)
        return AVERROR_EOF;
    k = fread(buf, 1, FFMIN((int64_t)n, t->left), t->f);
    if (!k)
        return AVERROR_EOF;
    t->left -= k;
    return k;
}

static int tb_ts_read(const uint8_t *data, int size, const char *path, int64_t max_bytes, const int *pids, int npids,
                      tb_ts_packet_cb cb, void *opaque)
{
    extern const FFInputFormat ff_mpegts_demuxer;
    TbMem m = { data, size, 0 };
    TbFile tf = { NULL, max_bytes };
    AVFormatContext *s = avformat_alloc_context();
    uint8_t *buf = av_malloc(4096);
    AVPacket *pkt = av_packet_alloc();
    AVDictionary *opts = NULL;
    AVBPrint bp;
    int n = 0, ret;

    if (!data && !(tf.f = fopen(path, "rb"))) {
        av_free(buf);
        ret = AVERROR(errno);
        goto end;
    }
    if (!s || !buf || !pkt ||
        !(s->pb = avio_alloc_context(buf, 4096, 0, data ? (void *)&m : (void *)&tf, data ? tb_mem_read : tb_file_read,
                                     NULL, NULL))) {
        av_free(buf);
        ret = AVERROR(ENOMEM);
        goto end;
    }
    s->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
    s->correct_ts_overflow = 0;
    s->probesize = 0;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (int i = 0; i < npids; i++)
        av_bprintf(&bp, "%s%d", i ? "," : "", pids[i]);
    av_dict_set_int(&opts, "disc", 1, 0);
    if (npids)
        av_dict_set(&opts, "disc_pids", bp.str, 0);
    av_bprint_finalize(&bp, NULL);
    ret = avformat_open_input(&s, "", &ff_mpegts_demuxer.p, &opts);
    av_dict_free(&opts);
    if (ret < 0)
        goto end;
    while ((ret = av_read_frame(s, pkt)) >= 0) {
        cb(opaque, s->streams[pkt->stream_index]->id, pkt->data, pkt->size, pkt->pts, pkt->pos);
        av_packet_unref(pkt);
        n++;
    }
    ret = ret == AVERROR_EOF ? n : ret;
end:
    if (s && s->pb) {
        av_freep(&s->pb->buffer);
        avio_context_free(&s->pb);
    }
    avformat_close_input(&s);
    av_packet_free(&pkt);
    if (tf.f)
        fclose(tf.f);
    return ret;
}

/* Blu-ray: a disc folder's structure as ff_bluray_disc_dump() writes it
 * (free()), or NULL with *err set when the disc cannot be read. */
static char *tb_bluray_dump(const char *path, const char *lang, int min_length, int *err)
{
    DiscIOFS *fs = NULL;
    BlurayDisc d;
    AVBPrint bp;
    char *out = NULL;

    *err = ff_discio_mount_folder(NULL, path, 1, &fs);
    if (*err < 0)
        return NULL;
    *err = ff_bluray_disc_open(NULL, fs, path, lang, &d);
    if (*err >= 0 && min_length >= 0)
        *err = ff_bluray_titles_build(NULL, &d, min_length);
    if (*err >= 0) {
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        ff_bluray_disc_dump(&d, &bp);
        if (av_bprint_is_complete(&bp))
            out = strdup(bp.str);
        av_bprint_finalize(&bp, NULL);
    }
    ff_bluray_disc_free(&d);
    ff_discio_fs_close(&fs);
    return out;
}

/* Blu-ray AACS: a disc folder opened with the AACS step (key files in
 * order), its clips' keys and their aligned units. */
typedef struct TBBluray {
    DiscIOFS   *fs;
    BlurayDisc  d;
    BlurayAACS *aacs;
} TBBluray;

static void tb_bluray_close(TBBluray *b)
{
    if (!b)
        return;
    ff_bluray_aacs_close(&b->aacs);
    ff_bluray_disc_free(&b->d);
    ff_discio_fs_close(&b->fs);
    free(b);
}

static TBBluray *tb_bluray_open(const char *path, const char *const *key_files, int nb_key_files, int *err)
{
    TBBluray *b = calloc(1, sizeof(*b));

    if (!b) {
        *err = AVERROR(ENOMEM);
        return NULL;
    }
    if ((*err = ff_discio_mount_folder(NULL, path, 1, &b->fs)) >= 0 &&
        (*err = ff_bluray_disc_open(NULL, b->fs, path, NULL, &b->d)) >= 0)
        *err = ff_bluray_aacs_open(NULL, &b->d, key_files, nb_key_files, &b->aacs);
    if (*err < 0) {
        tb_bluray_close(b);
        return NULL;
    }
    return b;
}

static BlurayClip *tb_bluray_clip(TBBluray *b, uint32_t number)
{
    for (int i = 0; i < b->d.nb_clips; i++)
        if (b->d.clips[i].number == number)
            return &b->d.clips[i];
    return NULL;
}

/* The clip's key unit, or -100 when there is no such clip. */
static int tb_bluray_key_unit(TBBluray *b, uint32_t clip)
{
    BlurayClip *c = tb_bluray_clip(b, clip);
    return c ? c->key_unit : -100;
}

static int tb_bluray_unit(TBBluray *b, uint32_t clip, uint8_t *unit)
{
    BlurayClip *c = tb_bluray_clip(b, clip);
    return c ? ff_bluray_aacs_unit(NULL, b->aacs, c, unit) : AVERROR(EINVAL);
}

/* 1 and the disc ID, or 0 when the disc has none. */
static int tb_bluray_disc_id(TBBluray *b, uint8_t *id)
{
    if (!b->d.has_disc_id)
        return 0;
    memcpy(id, b->d.disc_id, 20);
    return 1;
}

static int tb_bluray_has_keys(TBBluray *b)
{
    return b->aacs != NULL;
}

static char *tb_bluray_text(uint8_t charset, const uint8_t *text, uint32_t len)
{
    char *t = ff_bluray_text(NULL, charset, text, len), *out = t ? strdup(t) : NULL;
    av_free(t);
    return out;
}

static char *tb_bluray_clean_title(const char *s)
{
    char *t = ff_bluray_clean_title(s), *out = t ? strdup(t) : NULL;
    av_free(t);
    return out;
}

/* libavformat/dvd_nav.c: a navigator on the disc as the disc layer reads it,
 * one title played (dr_nav_play). */
static int tb_dvd_nav_trace(DVDDisc *d, int title, unsigned opts, int max_steps,
                            dr_nav_event *ev, int max_events, dr_nav_result *res)
{
    dvdnav_t *nav;
    int ret = ff_dvd_nav_open(d, NULL, &nav);

    memset(res, 0, sizeof(*res));
    if (ret < 0)
        return res->status = -1;
    return dr_nav_play(nav, title, opts, max_steps, ev, max_events, res);
}

/* libavformat/dvd_scan.c: the navigation scan (trace: NULL, or the Python
 * callback receiving its trace lines). */
static int tb_dvd_scan(DVDDisc *d, void (*trace)(void *, const char *), DVDScan *out)
{
    return ff_dvd_scan(d, trace, NULL, out);
}
