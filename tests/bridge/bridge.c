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

#include "base/buf.h"
#include "base/vlog.h"
#include "io/discio.h"
#include "base/err.h"
#include "base/log.h"
#include "disc-remuxer/names.h"
#include "base/mem.h"
#include "base/crc32.h"
#include "base/sha.h"
#include "base/str.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/codec_desc.h"
#include "libavformat/avformat.h"
#include "libavformat/demux.h"
#include "libavutil/aes.h"
#include "libavutil/bprint.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "disc/bluray/bluray_internal.h"
#include "analyze/analyze.h"
#include "timeline/disctl.h"
#include "disc/dvd/dvd_internal.h"
#include "disc/dvd/dvd_stream_probe.h"
#include "disc/hddvd/hddvd_internal.h"
#include "disc/clip_demux.h"
#include "parse/stock.h"
#include "mkv/mkv.h"
#include "parse/rules.h"
#include "disc-remuxer/writer.h"
#include "base/crc16.h"
#include "disc/lang.h"

#include <dvdcss/dvdcss.h>
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

/* Route the modules' lines (base/vlog.h; logctx is a source-name string,
 * or NULL) through FFmpeg's log to the sink, up to max_level. */
static void tb_module_vlog(const void *logctx, int level, const char *fmt, va_list vl)
{
    static const int map[] = { AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_INFO,
                               AV_LOG_VERBOSE, AV_LOG_DEBUG, AV_LOG_TRACE };
    (void)logctx;               /* the tests' lines carry no source prefix */
    av_vlog(NULL, map[level], fmt, vl);
}

static void tb_log_install(tb_log_sink sink, int max_level)
{
    dr_vlog_set(tb_module_vlog);
    dr_analyze_use();
    pthread_mutex_lock(&log_lock);
    log_sink = sink;
    log_max_level = max_level;
    pthread_mutex_unlock(&log_lock);
    av_log_set_level(max_level);
    av_log_set_callback(log_callback);
}

/* The versions our library headers give (the linked libraries must match). */
static const char *tb_gcrypt_header_version(void) { return GCRYPT_VERSION; }
static const char *tb_aacs_header_version(void) { return AACS_VERSION_STRING; }

/* Runs the MPEG-PS reading (`which`: 0 FFmpeg's demuxer as it is, 1 the
 * clip demuxer's disc sectors with the LPCM header length) over the bytes
 * in memory, the way the disc layer does (no parsing, no filled-in times);
 * each packet goes to cb with its stream id, the codec the reading gave the
 * stream, its bytes and pts. Returns the number of packets, or an
 * AVERROR. */
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

/* tb_mem_read for the clip demuxer (DR_ERR_EOF at the end). */
static int tb_mem_read_clip(void *opaque, uint8_t *buf, int n)
{
    int ret = tb_mem_read(opaque, buf, n);
    return ret == AVERROR_EOF ? DR_ERR_EOF : ret;
}

/* The clip demuxer's errors as FFmpeg's values (what the wrapped demuxers
 * returned before the reading moved out). */
static int tb_clip_err(int ret)
{
    switch (ret) {
    case DR_ERR_EOF:      return AVERROR_EOF;
    case DR_ERR_INVALID:  return AVERROR_INVALIDDATA;
    case DR_ERR_NOMEM:    return AVERROR(ENOMEM);
    case DR_ERR_EXTERNAL:
    case DR_ERR_BUG:      return AVERROR_BUG;
    default:              return ret;
    }
}

static int tb_ps_read(int which, int lpcm_header, const uint8_t *data, int size, tb_ps_packet_cb cb, void *opaque)
{
    TbMem m = { data, size, 0 };
    int n = 0, ret;

    if (which) {                /* disc sectors: the clip demuxer */
        DrClipDemux *c = NULL;
        DrClipPacket p;

        if ((ret = tb_clip_err(dr_clip_ps_open(&c, NULL, lpcm_header, tb_mem_read_clip, &m))) < 0)
            return ret;
        while (!(ret = dr_clip_read(c, &p))) {
            cb(opaque, p.id, p.codec, p.data, p.size, p.pts);
            n++;
        }
        dr_clip_close(&c);
        return ret == DR_ERR_EOF ? n : tb_clip_err(ret);
    }
    {
        extern const FFInputFormat ff_mpegps_demuxer;
        const FFInputFormat *f = &ff_mpegps_demuxer;
        AVFormatContext *s = avformat_alloc_context();
        uint8_t *buf = av_malloc(4096);
        AVPacket *pkt = av_packet_alloc();

        if (!s || !buf || !pkt || !(s->pb = avio_alloc_context(buf, 4096, 0, &m, tb_mem_read, NULL, NULL))) {
            av_free(buf);
            ret = AVERROR(ENOMEM);
            goto end;
        }
        s->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
        s->probesize = 0;
        ret = avformat_open_input(&s, "", &f->p, NULL);
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
}

/* The clip demuxer's transport-stream reading over the bytes in memory, or
 * over the first max_bytes of the file at path (data NULL), for the PIDs
 * given; each payload piece goes to cb with its stream id (PID | PES key
 * << 16), bytes, pts and byte position. Returns the number of pieces, or an
 * AVERROR. */
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

static int tb_file_read_clip(void *opaque, uint8_t *buf, int n)
{
    int ret = tb_file_read(opaque, buf, n);
    return ret == AVERROR_EOF ? DR_ERR_EOF : ret;
}

static int tb_ts_read(const uint8_t *data, int size, const char *path, int64_t max_bytes, const int *pids, int npids,
                      tb_ts_packet_cb cb, void *opaque)
{
    TbMem m = { data, size, 0 };
    TbFile tf = { NULL, max_bytes };
    DrClipDemux *c = NULL;
    DrClipPacket p;
    int n = 0, ret;

    if (!data && !(tf.f = fopen(path, "rb")))
        return AVERROR(errno);
    ret = tb_clip_err(dr_clip_ts_open(&c, NULL, pids, npids, 0, 0,
                                      data ? tb_mem_read_clip : tb_file_read_clip, data ? (void *)&m : (void *)&tf));
    if (ret < 0) {
        if (tf.f)
            fclose(tf.f);
        return ret;
    }
    while (!(ret = dr_clip_read(c, &p))) {
        cb(opaque, p.id, p.data, p.size, p.pts, p.pos);
        n++;
    }
    dr_clip_close(&c);
    if (tf.f)
        fclose(tf.f);
    return ret == DR_ERR_EOF ? n : tb_clip_err(ret);
}

/* Blu-ray: a disc folder's structure as ff_bluray_disc_dump() writes it
 * (free()), or NULL with *err set when the disc cannot be read. */
static char *tb_bluray_dump(const char *path, const char *lang, int min_length, int *err)
{
    DiscIOFS *fs = NULL;
    BlurayDisc d;
    DrStr bp;
    char *out = NULL;

    *err = ff_discio_mount_folder(NULL, path, 1, &fs);
    if (*err < 0)
        return NULL;
    *err = ff_bluray_disc_open(NULL, fs, path, lang, &d);
    if (*err >= 0 && min_length >= 0)
        *err = ff_bluray_menu_read(NULL, &d);
    if (*err >= 0 && min_length >= 0)
        *err = ff_bluray_titles_build(NULL, &d, min_length);
    if (*err >= 0 && min_length >= 0)
        *err = ff_bluray_decoys_check(NULL, &d);
    if (*err >= 0) {
        dr_str_init(&bp);
        ff_bluray_disc_dump(&d, &bp);
        if (dr_str_ok(&bp))
            out = strdup(bp.str);
        dr_str_free(&bp);
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

/* src/disc/dvd/dvd_nav.c: a navigator on the disc as the disc layer reads it,
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

/* disc/dvd/dvd_scan.c: the navigation scan (trace: NULL, or the Python
 * callback receiving its trace lines). */
static int tb_dvd_scan(DVDDisc *d, void (*trace)(void *, const char *), dvdnav_scan_results_t *out)
{
    return ff_dvd_scan(d, trace, NULL, out);
}

/* The scan with dvd.menu_revisit: the normal scan, then the titles only a
 * menu visited again with other register values reaches added (as
 * dvd_load() does). */
static int tb_dvd_scan_revisit(DVDDisc *d, dvdnav_scan_results_t *out)
{
    int ret = ff_dvd_scan(d, NULL, NULL, out);

    return ret < 0 ? ret : ff_dvd_scan_revisit(d, out);
}

/* disc/dvd/dvd_title_list.c: the navigation scan, then the title list built
 * from it, as ff_dvd_titles_dump() text (*dump, av_free() it); scan_ret:
 * the scan's return value. */
static int tb_dvd_titles(DVDDisc *d, int cell_mode, int cell_check, int min_length, int *scan_ret, char **dump)
{
    dvdnav_scan_results_t scan = { 0 };
    dvd_title_list_t list = { 0 };
    DVDTitleOptions o = { cell_mode, cell_check, min_length };
    int ret;

    *dump = NULL;
    *scan_ret = cell_mode == DVD_CELL_MODE_TRIM ? 0 : ff_dvd_scan(d, NULL, NULL, &scan);
    ret = ff_dvd_titles_build(d, &scan, &o, &list);
    if (ret >= 0 && !(*dump = ff_dvd_titles_dump(&list)))
        ret = AVERROR(ENOMEM);
    DVDFreeTitleList_rec(&list);
    dvdnav_scan_free_results(&scan);
    return ret;
}

/* src/disc/dvd/dvd_read.c: the title list (scan as tb_dvd_titles), then
 * segment seg of title t (both from 0) read sector by sector through the
 * checks into path (NULL: not written; sectors left out are not written);
 * *read / *left: sectors read and left out; the first read error ends it. */
static int tb_dvd_segment_dump(DVDDisc *d, int cell_mode, int t, int seg, const char *path, int64_t *nread,
                               int64_t *left)
{
    dvdnav_scan_results_t scan = { 0 };
    dvd_title_list_t list = { 0 };
    DVDTitleOptions o = { cell_mode, DVD_CELL_CHECK_ALL, 0 };
    FILE *out = NULL;
    uint8_t buf[DVD_SECTOR];
    int ret;

    *nread = *left = 0;
    if (cell_mode != DVD_CELL_MODE_TRIM)
        ff_dvd_scan(d, NULL, NULL, &scan);
    if ((ret = ff_dvd_titles_build(d, &scan, &o, &list)) < 0)
        goto end;
    if (t < 0 || t >= list.nb_titles || seg < 0 || seg >= list.titles[t]->segs.nb_segments) {
        ret = AVERROR(EINVAL);
        goto end;
    }
    if (path && !(out = fopen(path, "wb"))) {
        ret = AVERROR(errno);
        goto end;
    }
    for (uint32_t b = 0, n = ff_dvd_segment_blocks(&list.titles[t]->segs.segments[seg]); b < n; b++) {
        if ((ret = ff_dvd_segment_read(d, list.titles[t], seg, b, buf)) < 0)
            goto end;
        if (!ret) {
            (*left)++;
            continue;
        }
        (*nread)++;
        if (out && fwrite(buf, DVD_SECTOR, 1, out) != 1) {
            ret = AVERROR(EIO);
            goto end;
        }
    }
    ret = 0;
end:
    if (out)
        fclose(out);
    DVDFreeTitleList_rec(&list);
    dvdnav_scan_free_results(&scan);
    return ret;
}

/* The title list from given scan results (no scan run); walk_all_runs:
 * dvd.walk_all_runs. */
static int tb_dvd_titles_from(DVDDisc *d, dvd_nav_cells_t *res, int nb, int cell_mode, int cell_check,
                              int min_length, int walk_all_runs, char **dump)
{
    dvdnav_scan_results_t scan = { res, nb };
    dvd_title_list_t list = { 0 };
    DVDTitleOptions o = { cell_mode, cell_check, min_length, walk_all_runs };
    int ret = ff_dvd_titles_build(d, &scan, &o, &list);

    *dump = NULL;
    if (ret >= 0 && !(*dump = ff_dvd_titles_dump(&list)))
        ret = AVERROR(ENOMEM);
    DVDFreeTitleList_rec(&list);
    return ret;
}

/* ---- the stock cutters against the processing library's parsers ----
 * Both are fed the same bytes in the same pieces (each piece copied into a
 * padded buffer, as the frame step feeds it), then flushed; every call's
 * result is compared: bytes taken, the unit's bytes, its stream offset, the
 * samples and rate. Returns -1 all equal, else the first differing call
 * (tb_stock_why() says what). */
static char tb_stock_why_buf[512];

static const char *tb_stock_why(void)
{
    return tb_stock_why_buf;
}

static int tb_stock_one(AVCodecParserContext *pc, AVCodecContext *avctx, DrStock *st, const uint8_t *p,
                        int left, int64_t pos, int call, int *used, int *got)
{
    uint8_t *of = NULL;
    const uint8_t *os = NULL;
    int nf = 0, ns = 0, ds = 0, rs = 0;
    int64_t offs = 0;
    int uf = av_parser_parse2(pc, avctx, &of, &nf, p, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, pos);
    int us = dr_stock_parse(st, p, left, pos, &os, &ns, &offs, &ds, &rs);

    *used = uf;
    *got  = nf;
    if (uf != us || nf != ns || (nf && memcmp(of, os, nf)) || pc->frame_offset != offs ||
        pc->duration != ds || avctx->sample_rate != rs) {
        snprintf(tb_stock_why_buf, sizeof(tb_stock_why_buf), "call %d (pos %lld, %d bytes in): used %d/%d, unit "
                 "%d/%d bytes%s, offset %lld/%lld, samples %d/%d, rate %d/%d", call, (long long)pos, left, uf, us,
                 nf, ns, nf == ns && nf && memcmp(of, os, nf) ? " (bytes differ)" : "",
                 (long long)pc->frame_offset, (long long)offs, pc->duration, ds, avctx->sample_rate, rs);
        return 1;
    }
    return 0;
}

static int tb_stock_compare(int ffmpeg_codec, int stock_codec, const uint8_t *data, int size,
                            const int *chunks, int nb_chunks, int *calls)
{
    AVCodecParserContext *pc = av_parser_init(ffmpeg_codec);
    AVCodecContext *avctx = avcodec_alloc_context3(NULL);
    DrStock *st = dr_stock_new(stock_codec);
    uint8_t *feed = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    int64_t pos = 0;
    int at = 0, call = 0, bad = -1;

    tb_stock_why_buf[0] = 0;
    if (!pc || !avctx || !st || !feed) {
        snprintf(tb_stock_why_buf, sizeof(tb_stock_why_buf), "no memory");
        bad = 0;
        goto end;
    }
    avctx->codec_id   = ffmpeg_codec;
    avctx->codec_type = avcodec_get_type(ffmpeg_codec);
    for (int c = 0; c < nb_chunks && at < size && bad < 0; c++) {
        int len = chunks[c] < size - at ? chunks[c] : size - at, left = len;
        const uint8_t *p = feed;
        memcpy(feed, data + at, len);
        memset(feed + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        at += len;
        while (left > 0 && bad < 0) {
            int used, got;
            if (tb_stock_one(pc, avctx, st, p, left, pos, call, &used, &got))
                bad = call;
            call++;
            pos  += used;
            p    += used;
            left -= used;
        }
    }
    while (bad < 0) {
        int used, got;
        if (call > 4 * size + 1000) {
            snprintf(tb_stock_why_buf, sizeof(tb_stock_why_buf), "no end after %d calls", call);
            bad = call;
            break;
        }
        if (tb_stock_one(pc, avctx, st, NULL, 0, pos, call, &used, &got))
            bad = call;
        call++;
        if (!got)
            break;
    }
end:
    *calls = call;
    av_parser_close(pc);
    avcodec_free_context(&avctx);
    dr_stock_free(&st);
    av_free(feed);
    return bad;
}
