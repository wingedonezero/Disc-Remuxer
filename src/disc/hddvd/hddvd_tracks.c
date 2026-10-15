/*
 * HD DVD (Advanced Content): the tracks of a title. They come from the
 * attribute record of one EVOB of the title (the one with the most bytes,
 * the first of them on a tie): a video stream, up to 8 audio streams and up
 * to 32 sub-picture streams. Their languages come from the playlists; a
 * Dolby Digital Plus stream is probed (its first frames) to tell E-AC-3 from
 * plain AC-3.
 *
 * Attribute record (big-endian; offsets from the record's start): byte 0x02
 * video attribute (bits 7..5 coding: 0 / 1 MPEG-2, 2 AVC, 3 VC-1, bit 7 set
 * = none; byte 0x02 bits 4 and 2 and byte 0x04 bits 7..4 give the frame
 * size), 0x0e u16 audio stream count with 4-byte entries from 0x10 (byte 0
 * below 0x20: bits 4..2 = coding: AC-3, MLP, MPEG audio, (not known), LPCM,
 * LPCM, DTS, Dolby Digital Plus; 0x20 and up: none), 0xe4 u16 sub-picture stream
 * count with 5-byte entries from 0xe6 (byte 0 bits 7..5 = coding, bytes
 * 1..4 one stream number per display mode with bit 5 = present), 0x186 32
 * palette entries (two palettes of 16).
 *
 * The probe reads the EVOB's stream from its first block: the stream needs a
 * PES packet with a PTS as the first packet of one of its first 2000 packs;
 * then the stream's frames come through FFmpeg's MPEG-PS demuxer as disc
 * sectors and the AC-3 / DTS rules (src/parse; the disc reader's
 * frame rules, every variant of the codec); its first frame must be a valid
 * header;
 * 4 frames all E-AC-3 -> E-AC-3, all AC-3 -> AC-3, anything else (also fewer
 * than 4 frames) -> the stream is not supported and left out. A probe that
 * fails leaves the whole title out.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/err.h"
#include "base/bytes.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "parse/rules.h"
#include "parse/split.h"

#include "disc/lang.h"
#include "timeline/disctl.h"
#include "hddvd_internal.h"
#include "disc/clip_demux.h"

#define PACK            2048
#define PTS_SCAN_PACKS  2000    /* packs searched for the stream's first PTS */
#define PROBE_FRAMES    4

/* ---- one pack of an EVOB: the PS clip demuxer's disc sectors ---- */

typedef MpegPsDiscPacket Pkt;

static int pack_packet(const uint8_t *s, int *off, Pkt *p, int lpcm5)
{
    return ff_mpegps_disc_packet(s, off, p, lpcm5);
}

/* ---- the stream probe ----
 * The clip is read from its start through FFmpeg's MPEG-PS demuxer as disc
 * sectors (src/mux/ps_disc.c: the disc reader's sector rules) and the
 * stream's payloads go to the splitter taking every variant of the codec
 * (dr_split_take_any: the disc reader's frame rules, AC-3 and E-AC-3 frames
 * alike, DTS frames with or without a DTS-HD extension). */

typedef struct Probe {
    void          *log;
    DiscIOFS      *fs;
    HDDVDAACS     *aacs;
    HDDVDClip     *clip;
    int            key;                         /**< the stream's packet key */
    int            sub;                         /**< its sub-stream id: the demuxer's stream id */
    uint32_t       pos, end;                    /**< blocks */
    int            read_failed;                 /**< a block could not be read */
    int            nb_units;
    int            bsid[PROBE_FRAMES];          /**< AC-3: the first frames' bsid */
    int            dts_hd;                      /**< DTS: an extension substream follows the core frames */
} Probe;

/* A block of the stream as the probe sees it: made usable where it can be
 * (a pack that cannot be is passed on as read: the pack rules decide);
 * 0 only when it cannot be read. */
static int probe_block(Probe *p, uint32_t block, uint8_t *sec)
{
    int ret = ff_hddvd_clip_block(p->log, p->aacs, p->fs, p->clip, block, sec);

    if (ret < 0)
        dr_vlog(p->log, DR_VLOG_WARNING, "EVOB %s: block %"PRIu32" cannot be read for the stream probe: %s\n",
               p->clip->evob->name, block, dr_err_text(ret));
    else if (!ret)
        dr_vlog(p->log, DR_VLOG_DEBUG, "EVOB %s: block %"PRIu32" is probed as read\n", p->clip->evob->name, block);
    return ret >= 0;
}

/* The stream's first packet with a PTS that is the first packet of its pack. */
static int probe_first_pts(Probe *p)
{
    uint32_t n = p->end < PTS_SCAN_PACKS ? p->end : PTS_SCAN_PACKS;
    uint8_t sec[PACK];

    for (uint32_t i = 0; i < n; i++) {
        Pkt k;
        int off = 0;

        if (!probe_block(p, i, sec))
            return 0;
        if (!pack_packet(sec, &off, &k, 1)) {
            dr_vlog(p->log, DR_VLOG_WARNING, "EVOB %s: block %"PRIu32" is not a valid pack\n", p->clip->evob->name, i);
            return 0;
        }
        if (k.key == p->key && k.pts_present)
            return 1;
    }
    dr_vlog(p->log, DR_VLOG_WARNING, "EVOB %s: stream 0x%04x: no PES packet with a PTS starts one of the first "
           "%"PRIu32" packs\n", p->clip->evob->name, p->key, n);
    return 0;
}

/* The next block of the clip for the clip demuxer; DR_ERR_EOF at its end. */
static int probe_read(void *opaque, uint8_t *buf, int size)
{
    Probe *p = opaque;

    if (size < PACK)
        return DR_ERR(EINVAL);
    if (p->pos >= p->end)
        return DR_ERR_EOF;
    if (!probe_block(p, p->pos++, buf)) {
        p->read_failed = 1;
        return DR_ERR(EIO);
    }
    return PACK;
}

/* A frame the parser found: the first one must be a valid stream header
 * (AC-3: its E-AC-3 bitstream information through the channel map; DTS: a
 * core frame, with an extension substream whose header, frame and assets
 * fit). 1 taken, 0 the probe fails. */
static int probe_frame(Probe *p, DRCodecID codec, const uint8_t *b, int len)
{
    if (codec == DR_CODEC_DTS) {
        if (!p->nb_units) {
            int kind = dr_dca_first_frame(b, len);
            if (!kind)
                goto bad;
            p->dts_hd = kind == 2;
        }
    } else {
        if (!p->nb_units && !dr_ac3_stream_header(b, len))
            goto bad;
        if (p->nb_units < PROBE_FRAMES)
            p->bsid[p->nb_units] = len > 5 ? b[5] >> 3 : 0;
    }
    p->nb_units++;
    return 1;
bad:
    dr_vlog(p->log, DR_VLOG_WARNING, "EVOB %s: stream 0x%04x: its first frame is not a valid header\n",
           p->clip->evob->name, p->key);
    return 0;
}

/* Read the clip from its start until want frames are found or the clip
 * ends. 1 done, 0 failed, < 0 error. */
static int probe_run(Probe *p, DRCodecID codec, int want)
{
    DrSplit *split = NULL;
    DrClipDemux *ps = NULL;
    int ret, done = 0;

    if (!probe_first_pts(p))
        return 0;
    p->pos = 0;
    if ((ret = dr_clip_ps_open(&ps, p->log, 5, probe_read, p)) < 0)
        return ret == DR_ERR_NOMEM ? DR_ERR(ENOMEM) : ret;
    if (!(split = dr_split_new(codec == DR_CODEC_DTS ? DR_SPLIT_DTS : DR_SPLIT_AC3, NULL))) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    dr_split_take_any(split);
    ret = 1;
    while (!done && p->nb_units < want) {
        DrClipPacket pkt;
        int r = dr_clip_read(ps, &pkt);

        if (r == DR_ERR_EOF)
            break;
        if (r < 0) {
            /* a block that cannot be read, or a sector the disc reader's
             * rules refuse (logged by the clip demuxer) */
            ret = r == DR_ERR_NOMEM || r == DR_ERR(ENOMEM) ? DR_ERR(ENOMEM) : 0;
            break;
        }
        if (pkt.id == p->sub && (r = dr_split_write(split, pkt.data, pkt.size)) < 0) {
            ret  = r == DR_ERR(ENOMEM) ? r : 0;
            done = 1;
        }
        while (!done && p->nb_units < want) {
            DrSplitChunk ch;

            r = dr_split_next(split, &ch);
            if (!r)
                break;
            if (r < 0) {
                ret  = r == DR_ERR(ENOMEM) ? r : 0;
                done = 1;
                break;
            }
            if (!ch.status && !probe_frame(p, codec, ch.data, ch.size)) {
                ret  = 0;
                done = 1;
                break;
            }
        }
    }
end:
    dr_split_free(&split);
    dr_clip_close(&ps);
    return ret;
}

static void probe_init(Probe *p, void *logctx, DiscIOFS *fs, HDDVDAACS *aacs, HDDVDClip *clip, int sub)
{
    *p = (Probe){ .log = logctx, .fs = fs, .aacs = aacs, .clip = clip, .sub = sub, .key = sub << 8 | 0xbd,
                  .end = clip->size / PACK };
}

/* Dolby Digital Plus stream `number` of clip: *codec = E-AC-3, AC-3 or
 * DR_CODEC_NONE (not supported). 1 done, 0 failed. */
static int probe_ddplus(void *logctx, DiscIOFS *fs, HDDVDAACS *aacs, HDDVDClip *clip, int number,
                        DRCodecID *codec)
{
    Probe p;
    unsigned mask;
    int ret, k;

    probe_init(&p, logctx, fs, aacs, clip, 0xc0 + number);
    if ((ret = probe_run(&p, DR_CODEC_AC3, PROBE_FRAMES)) <= 0)
        return ret;
    k = DR_MIN(p.nb_units, PROBE_FRAMES);
    mask = k << 8;
    for (int i = 0; i < k; i++)
        if (p.bsid[i] >= 10 && p.bsid[i] <= 16)
            mask |= 1 << i;
    *codec = mask == 0x40f ? DR_CODEC_EAC3 : mask == 0x400 ? DR_CODEC_AC3 : DR_CODEC_NONE;
    dr_vlog(logctx, *codec == DR_CODEC_NONE ? DR_VLOG_WARNING : DR_VLOG_VERBOSE,
           "EVOB %s: Dolby Digital Plus stream %d: %d frame(s) found, %s\n", clip->evob->name, number, k,
           *codec == DR_CODEC_EAC3 ? "E-AC-3" : *codec == DR_CODEC_AC3 ? "AC-3 only" :
           "a mix of AC-3 and E-AC-3 or fewer than 4 frames: not supported, the stream is left out "
           "[untested on real discs]");
    return 1;
}

/* DTS stream `number` of clip: *hd = 1 when an extension substream follows
 * its first core frame (DTS-HD: a track, plus the DTS stream inside it as a
 * track of its own). 1 done, 0 failed (also when no frame is found). */
static int probe_dts(void *logctx, DiscIOFS *fs, HDDVDAACS *aacs, HDDVDClip *clip, int number, int *hd)
{
    Probe p;
    int ret;

    probe_init(&p, logctx, fs, aacs, clip, 0x88 + number);
    if ((ret = probe_run(&p, DR_CODEC_DTS, 1)) <= 0)
        return ret;
    if (!p.nb_units) {
        dr_vlog(logctx, DR_VLOG_WARNING, "EVOB %s: DTS stream %d: no frame found\n", clip->evob->name, number);
        return 0;
    }
    *hd = p.dts_hd;
    dr_vlog(logctx, DR_VLOG_WARNING, "EVOB %s: DTS stream %d: %s [untested on real discs]\n", clip->evob->name,
           number, *hd ? "DTS-HD (a DTS stream with an extension): listed, and the DTS stream inside it as a second track"
                       : "DTS core");
    return 1;
}

/* ---- playlist metadata of a stream ---- */

/* The language of audio (audio = 1) or sub-picture stream `number` (1-based)
 * of EVOB evo: the first Title (playlist, TitleSet, Title order) one of whose
 * clips names the EVOB and has an Audio / Subtitle element with that
 * streamNumber, and whose only TrackNavigationList has an AudioTrack /
 * SubtitleTrack with the same track: its langcode "LLL:N", LLL as an ISO
 * 639-2 code when known. 1 found (lang set), 0 not. */
static int stream_lang(void *logctx, HDDVDXpl *const *xpls, int nb_xpls, const char *evo, int audio,
                       uint32_t number, char lang[4])
{
    int ecls = audio ? HDDVD_XPL_AUDIO : HDDVD_XPL_SUBTITLE;
    int tcls = audio ? HDDVD_XPL_AUDIO_TRACK : HDDVD_XPL_SUBTITLE_TRACK;

    for (int q = 0; q < nb_xpls; q++) {
        const HDDVDXplNode *root = &xpls[q]->root;
        for (int a = 0; a < ff_hddvd_xpl_count(root, HDDVD_XPL_TITLE_SET); a++) {
            const HDDVDXplNode *ts = ff_hddvd_xpl_child(root, HDDVD_XPL_TITLE_SET, a);
            for (int t = 0; t < ff_hddvd_xpl_count(ts, HDDVD_XPL_TITLE); t++) {
                const HDDVDXplNode *title = ff_hddvd_xpl_child(ts, HDDVD_XPL_TITLE, t), *el = NULL, *nav;
                const char *track, *lc, *colon;
                char code[4] = { 0 };

                for (int c = 0; !el && c < ff_hddvd_xpl_count(title, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP); c++) {
                    const HDDVDXplNode *clip = ff_hddvd_xpl_child(title, HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, c);
                    if (!ff_hddvd_src_names_evob(logctx, ff_hddvd_xpl_str(clip, "src"), evo))
                        continue;
                    for (int s = 0; !el && s < ff_hddvd_xpl_count(clip, ecls); s++) {
                        const HDDVDXplNode *x = ff_hddvd_xpl_child(clip, ecls, s);
                        const char *sn = ff_hddvd_xpl_str(x, "streamNumber");
                        if (sn && strtoul(sn, NULL, 10) == number)
                            el = x;
                    }
                }
                if (!el || ff_hddvd_xpl_count(title, HDDVD_XPL_TRACK_NAVIGATION_LIST) != 1)
                    continue;
                nav   = ff_hddvd_xpl_child(title, HDDVD_XPL_TRACK_NAVIGATION_LIST, 0);
                track = ff_hddvd_xpl_str(el, "track");
                lc    = NULL;
                for (int s = 0; !lc && track && s < ff_hddvd_xpl_count(nav, tcls); s++) {
                    const HDDVDXplNode *x = ff_hddvd_xpl_child(nav, tcls, s);
                    const char *tr = ff_hddvd_xpl_str(x, "track");
                    if (tr && !strcmp(tr, track)) {
                        lc = ff_hddvd_xpl_str(x, "langcode");
                        if (!lc)
                            break;
                    }
                }
                if (!lc || !(colon = strchr(lc, ':')) || colon - lc > 3)
                    continue;
                memcpy(code, lc, colon - lc);
                if (code[0]) {
                    const char *iso = dr_disc_lang_code(code);
                    if (iso)
                        dr_strlcpy(code, iso, sizeof(code));
                }
                memcpy(lang, code, 4);
                return 1;
            }
        }
    }
    return 0;
}

/* ---- the tracks of a title ---- */

static const DRCodecID video_codecs[4] = {
    DR_CODEC_MPEG2VIDEO, DR_CODEC_MPEG2VIDEO, DR_CODEC_H264, DR_CODEC_VC1,
};

static const DRCodecID audio_codecs[8] = {   /* coding 3: no codec */
    DR_CODEC_AC3, DR_CODEC_TRUEHD, DR_CODEC_MP2, DR_CODEC_NONE,
    DR_CODEC_PCM_DVD, DR_CODEC_PCM_DVD, DR_CODEC_DTS, DR_CODEC_EAC3,
};

/* Dolby Digital Plus (coding 7) and DTS (coding 6) are probed; coding 3 has
 * no stream handler: a title with such a stream cannot be built. */
#define CODING_UNKNOWN  3
#define CODING_DTS      6
#define CODING_DDPLUS   7

typedef struct Builder {
    void            *log;
    DiscIOFS        *fs;
    HDDVDAACS       *aacs;
    const HDDVDVTI  *vti;
    HDDVDXpl *const *xpls;
    int              nb_xpls;
    unsigned         untested;                  /**< features already reported as untested */
} Builder;                                      /* Report once per disc that a feature met has not been seen on a real disc. */
static void untested(Builder *b, unsigned bit, const char *what)
{
    if (b->untested & bit)
        return;
    b->untested |= bit;
    dr_vlog(b->log, DR_VLOG_WARNING, "HD DVD: %s [untested on real discs]\n", what);
}

/* Content probe of every audio stream of clip that needs one (once per
 * clip). 1 done, 0 a probe failed. */
static int probe_clip(Builder *b, HDDVDClip *clip, const HDDVDEvobAttr *a)
{
    for (int j = 0; j < a->nb_audio; j++) {
        uint8_t c0 = a->raw[0x10 + 4 * j];
        int coding = c0 < 0x20 ? c0 >> 2 : -1, ret, hd = 0;
        DRCodecID codec = coding >= 0 ? audio_codecs[coding] : DR_CODEC_NONE;

        if (clip->probed[j])
            continue;
        if (coding == CODING_DDPLUS) {
            if ((ret = probe_ddplus(b->log, b->fs, b->aacs, clip, j, &codec)) <= 0)
                return ret;
        } else if (coding == CODING_DTS) {
            if ((ret = probe_dts(b->log, b->fs, b->aacs, clip, j, &hd)) <= 0)
                return ret;
        }
        clip->probed[j]      = 1;
        clip->probe_codec[j] = codec;
        clip->probe_dtshd[j] = hd;
    }
    return 1;
}

/* Frame size of sub-pictures from the video attribute (bytes 0x02..0x04). */
static void subpic_size(const uint8_t *v, int *w, int *h)
{
    *h = (v[0] & 0x14) == 4 ? 576 : 480;
    *w = 352;
    switch (v[2] >> 4) {
    case 0:  *h /= 2;                   break;
    case 1:                             break;
    case 2:  *w = 480;                  break;
    case 3:  *w = 544;                  break;
    case 4:  *w = 704;                  break;
    case 5:  *w = 720;                  break;
    case 8:  *w = 1280; *h = 720;       break;
    case 9:  *w = 960;  *h = 1080;      break;
    case 10: *w = 1280; *h = 1080;      break;
    case 11: *w = 1440; *h = 1080;      break;
    case 12: *w = 1920; *h = 1080;      break;
    default: *w = 0;    *h = 0;         break;
    }
}

/* The track of stream index i of clip: 1 described (codec NONE: no track),
 * 0 no such stream. */
static int describe(Builder *b, const HDDVDClip *clip, const HDDVDEvobAttr *a, int i, HDDVDTrack *t)
{
    memset(t, 0, sizeof(*t));
    t->index       = i;
    t->inner_track = -1;
    t->from_track  = -1;
    if (!i) {
        uint8_t v = a->raw[2];
        t->type  = DISC_MEDIA_VIDEO;
        t->codec = v < 0x80 ? video_codecs[v >> 5] : DR_CODEC_NONE;
        return 1;
    }
    if (i <= a->nb_audio) {
        int j = i - 1;
        uint8_t c0 = a->raw[0x10 + 4 * j];

        t->type   = DISC_MEDIA_AUDIO;
        t->number = j;
        t->coding = c0 < 0x20 ? c0 >> 2 : -1;
        t->codec  = clip->probed[j] ? clip->probe_codec[j] : t->coding >= 0 ? audio_codecs[t->coding] : DR_CODEC_NONE;
        if (t->codec != DR_CODEC_NONE)
            stream_lang(b->log, b->xpls, b->nb_xpls, clip->evob->name, 1, j + 1, t->lang);
        return 1;
    }
    if (i <= a->nb_audio + a->nb_subpic) {
        const uint8_t *p = a->raw + 0xe6 + 5 * (i - 1 - a->nb_audio);
        int k;

        t->type  = DISC_MEDIA_SUBTITLE;
        t->codec = DR_CODEC_NONE;
        for (k = 1; k <= 4 && !(p[k] & 0x20); k++)
            ;
        if (k <= 4) {
            t->number = p[k] & 0x1f;
            t->coding = p[0] >> 5;
            if (t->coding == 0 || t->coding == 1 || t->coding == 4) {
                t->codec   = DR_CODEC_DVD_SUBTITLE;
                t->palette = a->words + ((p[1] & 0x20) ? 16 : 0);
                if (t->coding == HDDVD_SUBPIC_CODING_HD) {
                    t->width  = 1920;
                    t->height = 1080;
                } else {
                    subpic_size(a->raw + 2, &t->width, &t->height);
                    untested(b, 16, "sub-picture stream of coding 0 or 1: drawn on the video's frame size");
                }
            }
        }
        stream_lang(b->log, b->xpls, b->nb_xpls, clip->evob->name, 0, t->number + 1, t->lang);
        return 1;
    }
    return 0;
}

static int add_track(HDDVDTitle *t, const HDDVDTrack *k)
{
    return dr_array_add(&t->tracks, &t->nb_tracks, sizeof(*k), k) ? 0 : DR_ERR(ENOMEM);
}

/* The tracks of title t: 1 built, 0 the title cannot be used, < 0 error. */
static int build_title(Builder *b, HDDVDTitle *t)
{
    HDDVDClip *clip;
    const HDDVDEvobAttr *a;
    HDDVDTrack k;
    int seg = 0, n, ret;

    for (int i = 1; i < t->nb_clips; i++)
        if (t->clips[i]->size > t->clips[seg]->size)
            seg = i;
    clip = t->clips[seg];
    if (clip->evob->attr < 1 || clip->evob->attr > b->vti->nb_attrs)
        return 0;
    a = &b->vti->attrs[clip->evob->attr - 1];
    n = 1 + a->nb_audio + a->nb_subpic;
    t->segment = seg;

    if ((ret = probe_clip(b, clip, a)) <= 0) {
        if (!ret)
            dr_vlog(b->log, DR_VLOG_WARNING, "Title %s: the streams of EVOB %s cannot be probed: "
                   "the title is left out\n", t->name, clip->evob->name);
        return ret;
    }
    /* video: the first stream with a video codec */
    for (int i = 0; i < n; i++) {
        describe(b, clip, a, i, &k);
        if (k.type == DISC_MEDIA_VIDEO && k.codec != DR_CODEC_NONE) {
            if ((ret = add_track(t, &k)) < 0)
                return ret;
            break;
        }
    }
    if (!t->nb_tracks) {
        dr_vlog(b->log, DR_VLOG_WARNING, "Title %s: EVOB %s has no video stream: the title is left out\n",
               t->name, clip->evob->name);
        return 0;
    }
    /* audio, then sub-pictures (a stream like an earlier one is left out by
     * the disc layer's stream pass) */
    for (int pass = 0; pass < 2; pass++) {
        int type = pass ? DISC_MEDIA_SUBTITLE : DISC_MEDIA_AUDIO;
        for (int i = 0; i < n; i++) {
            describe(b, clip, a, i, &k);
            if (k.type == DISC_MEDIA_AUDIO && type == k.type && k.coding == CODING_UNKNOWN) {
                untested(b, 1, "audio stream with coding mode 3: a title with one cannot be used");
                dr_vlog(b->log, DR_VLOG_WARNING, "Title %s: audio stream %d has coding mode 3, which has no "
                       "stream handler: the title is left out\n", t->name, i);
                return 0;
            }
            if (k.type != type || k.codec == DR_CODEC_NONE)
                continue;
            if (k.type == DISC_MEDIA_AUDIO && clip->probe_dtshd[k.number]) {
                /* DTS-HD, then the DTS stream inside it as a track of its own */
                k.inner_track = t->nb_tracks + 1;
                if ((ret = add_track(t, &k)) < 0)
                    return ret;
                k.inner_track = -1;
                k.from_track  = t->nb_tracks - 1;
            }
            if (k.codec == DR_CODEC_PCM_DVD)
                untested(b, 2, "LPCM audio");
            else if (k.codec == DR_CODEC_MP2)
                untested(b, 4, "MPEG audio");
            if ((ret = add_track(t, &k)) < 0)
                return ret;
        }
    }
    if (t->tracks[0].codec == DR_CODEC_H264)
        untested(b, 8, "AVC video");
    return 1;
}

int ff_hddvd_tracks_build(void *logctx, DiscIOFS *fs, HDDVDAACS *aacs, const HDDVDVTI *vti,
                          HDDVDXpl *const *xpls, int nb_xpls, HDDVDTitlePlan *plan)
{
    Builder b = { .log = logctx, .fs = fs, .aacs = aacs, .vti = vti, .xpls = xpls, .nb_xpls = nb_xpls };
    int i = 0;

    while (i < plan->nb_titles) {
        HDDVDTitle *t = &plan->titles[i];
        int ret = build_title(&b, t);

        if (ret < 0)
            return ret;
        if (ret) {
            dr_vlog(logctx, DR_VLOG_VERBOSE, "Title %d (%s): %d track(s) from EVOB %s\n", i, t->name,
                   t->nb_tracks, t->clips[t->segment]->evob->name);
            i++;
            continue;
        }
        dr_free(t->clips);
        dr_free(t->marks);
        dr_free(t->tracks);
        memmove(t, t + 1, (plan->nb_titles - i - 1) * sizeof(*t));
        plan->nb_titles--;
    }
    return 0;
}
