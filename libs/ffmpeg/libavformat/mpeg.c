/*
 * MPEG-1/2 demuxer
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config_components.h"

#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"
#include "mpeg.h"

/*********************************************/
/* demux code */

#define MAX_SYNC_SIZE 100000

static int check_pes(const uint8_t *p, const uint8_t *end)
{
    int pes1;
    int pes2 = (p[3] & 0xC0) == 0x80 &&
               (p[4] & 0xC0) != 0x40 &&
               ((p[4] & 0xC0) == 0x00 ||
                (p[4] & 0xC0) >> 2 == (p[6] & 0xF0));

    for (p += 3; p < end && *p == 0xFF; p++) ;
    if ((*p & 0xC0) == 0x40)
        p += 2;

    if ((*p & 0xF0) == 0x20)
        pes1 = p[0] & p[2] & p[4] & 1;
    else if ((*p & 0xF0) == 0x30)
        pes1 = p[0] & p[2] & p[4] & p[5] & p[7] & p[9] & 1;
    else
        pes1 = *p == 0x0F;

    return pes1 || pes2;
}

static int check_pack_header(const uint8_t *buf)
{
    return (buf[1] & 0xC0) == 0x40 || (buf[1] & 0xF0) == 0x20;
}

static int mpegps_probe(const AVProbeData *p)
{
    uint32_t code = -1;
    int i;
    int sys = 0, pspack = 0, priv1 = 0, vid = 0;
    int audio = 0, invalid = 0, score = 0;
    int endpes = 0;

    for (i = 0; i < p->buf_size; i++) {
        code = (code << 8) + p->buf[i];
        if ((code & 0xffffff00) == 0x100) {
            int len  = p->buf[i + 1] << 8 | p->buf[i + 2];
            int pes  = endpes <= i && check_pes(p->buf + i, p->buf + p->buf_size);
            int pack = check_pack_header(p->buf + i);

            if (len > INT_MAX - i)
                break;

            if (code == SYSTEM_HEADER_START_CODE)
                sys++;
            else if (code == PACK_START_CODE && pack)
                pspack++;
            else if ((code & 0xf0) == VIDEO_ID && pes) {
                endpes = i + len;
                vid++;
            }
            // skip pes payload to avoid start code emulation for private
            // and audio streams
            else if ((code & 0xe0) == AUDIO_ID &&  pes) {audio++; i+=len;}
            else if (code == PRIVATE_STREAM_1  &&  pes) {priv1++; i+=len;}
            else if (code == 0x1fd             &&  pes) vid++; //VC1

            else if ((code & 0xf0) == VIDEO_ID && !pes) invalid++;
            else if ((code & 0xe0) == AUDIO_ID && !pes) invalid++;
            else if (code == PRIVATE_STREAM_1  && !pes) invalid++;
        }
    }

    if (vid + audio > invalid + 1) /* invalid VDR files nd short PES streams */
        score = AVPROBE_SCORE_EXTENSION / 2;

//     av_log(NULL, AV_LOG_ERROR, "vid:%d aud:%d sys:%d pspack:%d invalid:%d size:%d \n",
//            vid, audio, sys, pspack, invalid, p->buf_size);

    if (sys > invalid && sys * 9 <= pspack * 10)
        return (audio > 12 || vid > 3 || pspack > 2) ? AVPROBE_SCORE_EXTENSION + 2
                                                     : AVPROBE_SCORE_EXTENSION / 2 + (audio + vid + pspack > 1); // 1 more than mp3
    if (pspack > invalid && (priv1 + vid + audio) * 10 >= pspack * 9)
        return pspack > 2 ? AVPROBE_SCORE_EXTENSION + 2
                          : AVPROBE_SCORE_EXTENSION / 2; // 1 more than .mpg
    if ((!!vid ^ !!audio) && (audio > 4 || vid > 1) && !sys &&
        !pspack && p->buf_size > 2048 && vid + audio > invalid) /* PES stream */
        return (audio > 12 || vid > 6 + 2 * invalid) ? AVPROBE_SCORE_EXTENSION + 1
                                                     : AVPROBE_SCORE_EXTENSION / 2;

    // 02-Penguin.flac has sys:0 priv1:0 pspack:0 vid:0 audio:1
    // mp3_misidentified_2.mp3 has sys:0 priv1:0 pspack:0 vid:0 audio:6
    // Have\ Yourself\ a\ Merry\ Little\ Christmas.mp3 0 0 0 5 0 1 len:21618
    return score;
}

#define DISC_QUEUE 8             /* packets of one disc sector (two, or more with MPEG audio) */

typedef struct MpegDemuxContext {
    const AVClass *class;
    int32_t header_state;
    unsigned char psm_es_type[256];
    int sofdec;
    int dvd;
    int imkh_cctv;
    int raw_ac3;

    /* disc sectors (option disc) */
    int        disc;             /**< option: read one 2048-byte disc sector at a time, the disc reader's rules */
    int        lpcm_header;      /**< option: the LPCM audio frame header's length, 3 or 5 */
    int64_t    sector;           /**< sectors read */
    AVPacket  *q[DISC_QUEUE];    /**< the current sector's packets, not given out yet */
    int        q_head, q_n;
    int        sticky;           /**< a failed sector: every later read returns this */
} MpegDemuxContext;

static int mpegps_read_header(AVFormatContext *s)
{
    MpegDemuxContext *m = s->priv_data;
    char buffer[7] = { 0 };
    int64_t last_pos = avio_tell(s->pb);

    m->header_state = 0xff;
    s->ctx_flags   |= AVFMTCTX_NOHEADER;
    if (m->disc)
        return 0;

    avio_get_str(s->pb, 6, buffer, sizeof(buffer));
    if (!memcmp("IMKH", buffer, 4)) {
        m->imkh_cctv = 1;
    } else if (!memcmp("Sofdec", buffer, 6)) {
        m->sofdec = 1;
    } else
       avio_seek(s->pb, last_pos, SEEK_SET);

    /* no need to do more */
    return 0;
}

static int64_t get_pts(AVIOContext *pb, int c)
{
    uint8_t buf[5];
    int ret;

    buf[0] = c < 0 ? avio_r8(pb) : c;
    ret = avio_read(pb, buf + 1, 4);
    if (ret < 4)
        return AV_NOPTS_VALUE;

    return ff_parse_pes_pts(buf);
}

static int find_next_start_code(AVIOContext *pb, int *size_ptr,
                                int32_t *header_state)
{
    unsigned int state, v;
    int val, n;

    state = *header_state;
    n     = *size_ptr;
    while (n > 0) {
        if (avio_feof(pb))
            break;
        v = avio_r8(pb);
        n--;
        if (state == 0x000001) {
            state = ((state << 8) | v) & 0xffffff;
            val   = state;
            goto found;
        }
        state = ((state << 8) | v) & 0xffffff;
    }
    val = -1;

found:
    *header_state = state;
    *size_ptr     = n;
    return val;
}

/**
 * Extract stream types from a program stream map
 * According to ISO/IEC 13818-1 ('MPEG-2 Systems') table 2-35
 *
 * @return number of bytes occupied by PSM in the bitstream
 */
static long mpegps_psm_parse(MpegDemuxContext *m, AVIOContext *pb)
{
    int psm_length, ps_info_length, es_map_length;

    psm_length = avio_rb16(pb);
    avio_r8(pb);
    avio_r8(pb);
    ps_info_length = avio_rb16(pb);

    /* skip program_stream_info */
    avio_skip(pb, ps_info_length);
    /*es_map_length = */avio_rb16(pb);
    /* Ignore es_map_length, trust psm_length */
    es_map_length = psm_length - ps_info_length - 10;

    /* at least one es available? */
    while (es_map_length >= 4) {
        unsigned char type      = avio_r8(pb);
        unsigned char es_id     = avio_r8(pb);
        uint16_t es_info_length = avio_rb16(pb);

        /* remember mapping from stream id to stream type */
        m->psm_es_type[es_id] = type;
        /* skip program_stream_info */
        avio_skip(pb, es_info_length);
        es_map_length -= 4 + es_info_length;
    }
    avio_rb32(pb); /* crc32 */
    return 2 + psm_length;
}

/* read the next PES header. Return its position in ppos
 * (if not NULL), and its start code, pts and dts.
 */
static int mpegps_read_pes_header(AVFormatContext *s,
                                  int64_t *ppos, int *pstart_code,
                                  int64_t *ppts, int64_t *pdts)
{
    MpegDemuxContext *m = s->priv_data;
    int len, size, startcode, c, flags, header_len;
    int pes_ext, ext2_len, id_ext, skip;
    int64_t pts, dts;
    int64_t last_sync = avio_tell(s->pb);

error_redo:
    avio_seek(s->pb, last_sync, SEEK_SET);
redo:
    /* next start code (should be immediately after) */
    m->header_state = 0xff;
    size      = MAX_SYNC_SIZE;
    startcode = find_next_start_code(s->pb, &size, &m->header_state);
    last_sync = avio_tell(s->pb);
    if (startcode < 0) {
        if (avio_feof(s->pb))
            return AVERROR_EOF;
        // FIXME we should remember header_state
        return FFERROR_REDO;
    }

    if (startcode == PACK_START_CODE)
        goto redo;
    if (startcode == SYSTEM_HEADER_START_CODE)
        goto redo;
    if (startcode == PADDING_STREAM) {
        avio_skip(s->pb, avio_rb16(s->pb));
        goto redo;
    }
    if (startcode == PRIVATE_STREAM_2) {
        if (!m->sofdec) {
            /* Need to detect whether this from a DVD or a 'Sofdec' stream */
            int len = avio_rb16(s->pb);
            int bytesread = 0;
            uint8_t *ps2buf = av_malloc(len);

            if (ps2buf) {
                bytesread = avio_read(s->pb, ps2buf, len);

                if (bytesread != len) {
                    avio_skip(s->pb, len - bytesread);
                } else {
                    uint8_t *p = 0;
                    if (len >= 6)
                        p = memchr(ps2buf, 'S', len - 5);

                    if (p)
                        m->sofdec = !memcmp(p+1, "ofdec", 5);

                    m->sofdec -= !m->sofdec;

                    if (m->sofdec < 0) {
                        if (len == 980  && ps2buf[0] == 0) {
                            /* PCI structure? */
                            uint32_t startpts = AV_RB32(ps2buf + 0x0d);
                            uint32_t endpts = AV_RB32(ps2buf + 0x11);
                            uint8_t hours = ((ps2buf[0x19] >> 4) * 10) + (ps2buf[0x19] & 0x0f);
                            uint8_t mins  = ((ps2buf[0x1a] >> 4) * 10) + (ps2buf[0x1a] & 0x0f);
                            uint8_t secs  = ((ps2buf[0x1b] >> 4) * 10) + (ps2buf[0x1b] & 0x0f);

                            m->dvd = (hours <= 23 &&
                                      mins  <= 59 &&
                                      secs  <= 59 &&
                                      (ps2buf[0x19] & 0x0f) < 10 &&
                                      (ps2buf[0x1a] & 0x0f) < 10 &&
                                      (ps2buf[0x1b] & 0x0f) < 10 &&
                                      endpts >= startpts);
                        } else if (len == 1018 && ps2buf[0] == 1) {
                            /* DSI structure? */
                            uint8_t hours = ((ps2buf[0x1d] >> 4) * 10) + (ps2buf[0x1d] & 0x0f);
                            uint8_t mins  = ((ps2buf[0x1e] >> 4) * 10) + (ps2buf[0x1e] & 0x0f);
                            uint8_t secs  = ((ps2buf[0x1f] >> 4) * 10) + (ps2buf[0x1f] & 0x0f);

                            m->dvd = (hours <= 23 &&
                                      mins  <= 59 &&
                                      secs  <= 59 &&
                                      (ps2buf[0x1d] & 0x0f) < 10 &&
                                      (ps2buf[0x1e] & 0x0f) < 10 &&
                                      (ps2buf[0x1f] & 0x0f) < 10);
                        }
                    }
                }

                av_free(ps2buf);

                /* If this isn't a DVD packet or no memory
                 * could be allocated, just ignore it.
                 * If we did, move back to the start of the
                 * packet (plus 'length' field) */
                if (!m->dvd || avio_skip(s->pb, -(len + 2)) < 0) {
                    /* Skip back failed.
                     * This packet will be lost but that can't be helped
                     * if we can't skip back
                     */
                    goto redo;
                }
            } else {
                /* No memory */
                avio_skip(s->pb, len);
                goto redo;
            }
        } else if (!m->dvd) {
            int len = avio_rb16(s->pb);
            avio_skip(s->pb, len);
            goto redo;
        }
    }
    if (startcode == PROGRAM_STREAM_MAP) {
        mpegps_psm_parse(m, s->pb);
        goto redo;
    }

    /* find matching stream */
    if (!((startcode >= 0x1c0 && startcode <= 0x1df) ||
          (startcode >= 0x1e0 && startcode <= 0x1ef) ||
          (startcode == 0x1bd) ||
          (startcode == PRIVATE_STREAM_2) ||
          (startcode == 0x1fd)))
        goto redo;
    if (ppos) {
        *ppos = avio_tell(s->pb) - 4;
    }
    len = avio_rb16(s->pb);
    pts =
    dts = AV_NOPTS_VALUE;
    if (startcode != PRIVATE_STREAM_2)
    {
    /* stuffing */
    for (;;) {
        if (len < 1)
            goto error_redo;
        c = avio_r8(s->pb);
        len--;
        /* XXX: for MPEG-1, should test only bit 7 */
        if (c != 0xff)
            break;
    }
    if ((c & 0xc0) == 0x40) {
        /* buffer scale & size */
        avio_r8(s->pb);
        c    = avio_r8(s->pb);
        len -= 2;
    }
    if ((c & 0xe0) == 0x20) {
        dts  =
        pts  = get_pts(s->pb, c);
        len -= 4;
        if (c & 0x10) {
            dts  = get_pts(s->pb, -1);
            len -= 5;
        }
    } else if ((c & 0xc0) == 0x80) {
        /* mpeg 2 PES */
        flags      = avio_r8(s->pb);
        header_len = avio_r8(s->pb);
        len       -= 2;
        if (header_len > len)
            goto error_redo;
        len -= header_len;
        if (flags & 0x80) {
            dts         = pts = get_pts(s->pb, -1);
            header_len -= 5;
            if (flags & 0x40) {
                dts         = get_pts(s->pb, -1);
                header_len -= 5;
            }
        }
        if (flags & 0x3f && header_len == 0) {
            flags &= 0xC0;
            av_log(s, AV_LOG_WARNING, "Further flags set but no bytes left\n");
        }
        if (flags & 0x01) { /* PES extension */
            pes_ext = avio_r8(s->pb);
            header_len--;
            /* Skip PES private data, program packet sequence counter
             * and P-STD buffer */
            skip  = (pes_ext >> 4) & 0xb;
            skip += skip & 0x9;
            if (pes_ext & 0x40 || skip > header_len) {
                av_log(s, AV_LOG_WARNING, "pes_ext %X is invalid\n", pes_ext);
                pes_ext = skip = 0;
            }
            avio_skip(s->pb, skip);
            header_len -= skip;

            if (pes_ext & 0x01) { /* PES extension 2 */
                ext2_len = avio_r8(s->pb);
                header_len--;
                if ((ext2_len & 0x7f) > 0) {
                    id_ext = avio_r8(s->pb);
                    if ((id_ext & 0x80) == 0)
                        startcode = ((startcode & 0xff) << 8) | id_ext;
                    header_len--;
                }
            }
        }
        if (header_len < 0)
            goto error_redo;
        avio_skip(s->pb, header_len);
    } else if (c != 0xf)
        goto redo;
    }

    if (startcode == PRIVATE_STREAM_1) {
        int ret = ffio_ensure_seekback(s->pb, 2);

        if (ret < 0)
            return ret;

        startcode = avio_r8(s->pb);
        m->raw_ac3 = 0;
        if (startcode == 0x0b) {
            if (avio_r8(s->pb) == 0x77) {
                startcode = 0x80;
                m->raw_ac3 = 1;
                avio_skip(s->pb, -2);
            } else {
                avio_skip(s->pb, -1);
            }
        } else {
            len--;
        }
    }
    if (len < 0)
        goto error_redo;
    if (dts != AV_NOPTS_VALUE && ppos) {
        int i;
        for (i = 0; i < s->nb_streams; i++) {
            if (startcode == s->streams[i]->id &&
                (s->pb->seekable & AVIO_SEEKABLE_NORMAL) /* index useless on streams anyway */) {
                ff_reduce_index(s, i);
                av_add_index_entry(s->streams[i], *ppos, dts, 0, 0,
                                   AVINDEX_KEYFRAME /* FIXME keyframe? */);
            }
        }
    }

    *pstart_code = startcode;
    *ppts        = pts;
    *pdts        = dts;
    return len;
}

static int disc_read_packet(AVFormatContext *s, AVPacket *pkt);

static int mpegps_read_packet(AVFormatContext *s,
                              AVPacket *pkt)
{
    MpegDemuxContext *m = s->priv_data;
    AVStream *st;
    FFStream *sti;
    int len, startcode, i, es_type, ret;
    int pcm_dvd = 0;
    int request_probe= 0;
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    enum AVMediaType type;
    int64_t pts, dts, dummy_pos; // dummy_pos is needed for the index building to work

    if (m->disc)
        return disc_read_packet(s, pkt);

redo:
    len = mpegps_read_pes_header(s, &dummy_pos, &startcode, &pts, &dts);
    if (len < 0)
        return len;

    if (startcode >= 0x80 && startcode <= 0xcf) {
        if (len < 4)
            goto skip;

        if (!m->raw_ac3) {
            /* audio: skip header */
            avio_skip(s->pb, 3);
            len -= 3;
            if (startcode >= 0xb0 && startcode <= 0xbf) {
                /* MLP/TrueHD audio has a 4-byte header */
                avio_r8(s->pb);
                len--;
            } else if (startcode >= 0xa0 && startcode <= 0xaf) {
                ret = ffio_ensure_seekback(s->pb, 3);
                if (ret < 0)
                    return ret;
                pcm_dvd = (avio_rb24(s->pb) & 0xFF) == 0x80;
                avio_skip(s->pb, -3);
            }
        }
    }

    /* now find stream */
    for (i = 0; i < s->nb_streams; i++) {
        st = s->streams[i];
        if (st->id == startcode)
            goto found;
    }

    es_type = m->psm_es_type[startcode & 0xff];
    if (es_type == STREAM_TYPE_VIDEO_MPEG1) {
        codec_id = AV_CODEC_ID_MPEG2VIDEO;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_VIDEO_MPEG2) {
        codec_id = AV_CODEC_ID_MPEG2VIDEO;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_AUDIO_MPEG1 ||
               es_type == STREAM_TYPE_AUDIO_MPEG2) {
        codec_id = AV_CODEC_ID_MP3;
        type     = AVMEDIA_TYPE_AUDIO;
    } else if (es_type == STREAM_TYPE_AUDIO_AAC) {
        codec_id = AV_CODEC_ID_AAC;
        type     = AVMEDIA_TYPE_AUDIO;
    } else if (es_type == STREAM_TYPE_VIDEO_MPEG4) {
        codec_id = AV_CODEC_ID_MPEG4;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_VIDEO_H264) {
        codec_id = AV_CODEC_ID_H264;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_VIDEO_HEVC) {
        codec_id = AV_CODEC_ID_HEVC;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_VIDEO_VVC) {
        codec_id = AV_CODEC_ID_VVC;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_AUDIO_AC3) {
        codec_id = AV_CODEC_ID_AC3;
        type     = AVMEDIA_TYPE_AUDIO;
    } else if (es_type == 0x90) {
        codec_id = AV_CODEC_ID_PCM_ALAW;
        type     = AVMEDIA_TYPE_AUDIO;
    } else if (m->imkh_cctv && es_type == 0x91) {
        codec_id = AV_CODEC_ID_PCM_MULAW;
        type     = AVMEDIA_TYPE_AUDIO;
    } else if (startcode >= 0x1e0 && startcode <= 0x1ef) {
        static const unsigned char avs_seqh[4] = { 0, 0, 1, 0xb0 };
        unsigned char buf[8];

        ret = ffio_read_size(s->pb, buf, 8);
        if (ret < 0)
            return ret;
        avio_seek(s->pb, -8, SEEK_CUR);
        if (!memcmp(buf, avs_seqh, 4) && (buf[6] != 0 || buf[7] != 1))
            codec_id = AV_CODEC_ID_CAVS;
        else
            request_probe= 1;
        type = AVMEDIA_TYPE_VIDEO;
    } else if (startcode == PRIVATE_STREAM_2) {
        type = AVMEDIA_TYPE_DATA;
        codec_id = AV_CODEC_ID_DVD_NAV;
    } else if (startcode >= 0x1c0 && startcode <= 0x1df) {
        type     = AVMEDIA_TYPE_AUDIO;
        if (m->sofdec > 0) {
            codec_id = AV_CODEC_ID_ADPCM_ADX;
            // Auto-detect AC-3
            request_probe = 50;
        } else if (m->imkh_cctv && startcode == 0x1c0 && len > 80) {
            codec_id = AV_CODEC_ID_PCM_ALAW;
            request_probe = 50;
        } else {
            codec_id = AV_CODEC_ID_MP2;
            if (m->imkh_cctv)
                request_probe = 25;
        }
    } else if (startcode >= 0x80 && startcode <= 0x87) {
        type     = AVMEDIA_TYPE_AUDIO;
        codec_id = AV_CODEC_ID_AC3;
    } else if ((startcode >= 0x88 && startcode <= 0x8f) ||
               (startcode >= 0x98 && startcode <= 0x9f)) {
        /* 0x90 - 0x97 is reserved for SDDS in DVD specs */
        type     = AVMEDIA_TYPE_AUDIO;
        codec_id = AV_CODEC_ID_DTS;
    } else if (startcode >= 0xa0 && startcode <= 0xaf) {
        type     = AVMEDIA_TYPE_AUDIO;
        if (!pcm_dvd) {
            codec_id = AV_CODEC_ID_MLP;
        } else {
            codec_id = AV_CODEC_ID_PCM_DVD;
        }
    } else if (startcode >= 0xb0 && startcode <= 0xbf) {
        type     = AVMEDIA_TYPE_AUDIO;
        codec_id = AV_CODEC_ID_TRUEHD;
    } else if (startcode >= 0xc0 && startcode <= 0xcf) {
        /* Used for both AC-3 and E-AC-3 in EVOB files */
        type     = AVMEDIA_TYPE_AUDIO;
        codec_id = AV_CODEC_ID_AC3;
    } else if (startcode >= 0x20 && startcode <= 0x3f) {
        type     = AVMEDIA_TYPE_SUBTITLE;
        codec_id = AV_CODEC_ID_DVD_SUBTITLE;
    } else if (startcode >= 0xfd55 && startcode <= 0xfd5f) {
        type     = AVMEDIA_TYPE_VIDEO;
        codec_id = AV_CODEC_ID_VC1;
    } else if (startcode == 0x69 || startcode == 0x49) {
        type     = AVMEDIA_TYPE_SUBTITLE;
        codec_id = AV_CODEC_ID_IVTV_VBI;
    } else {
skip:
        /* skip packet */
        avio_skip(s->pb, len);
        goto redo;
    }
    /* no stream found: add a new stream */
    st = avformat_new_stream(s, NULL);
    if (!st)
        goto skip;
    sti = ffstream(st);
    st->id                = startcode;
    st->codecpar->codec_type = type;
    st->codecpar->codec_id   = codec_id;
    if (   st->codecpar->codec_id == AV_CODEC_ID_PCM_MULAW
        || st->codecpar->codec_id == AV_CODEC_ID_PCM_ALAW) {
        st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        st->codecpar->sample_rate = 8000;
    }
    sti->request_probe = request_probe;
    sti->need_parsing  = AVSTREAM_PARSE_FULL;

found:
    if (st->discard >= AVDISCARD_ALL)
        goto skip;
    if (startcode >= 0xa0 && startcode <= 0xaf) {
      if (st->codecpar->codec_id == AV_CODEC_ID_MLP) {
            if (len < 6)
                goto skip;
            avio_skip(s->pb, 6);
            len -=6;
      }
    }
    ret = av_get_packet(s->pb, pkt, len);

    pkt->pts          = pts;
    pkt->dts          = dts;
    pkt->pos          = dummy_pos;
    pkt->stream_index = st->index;

    if (s->debug & AV_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG, "%d: pts=%0.3f dts=%0.3f size=%d\n",
            pkt->stream_index, pkt->pts / 90000.0, pkt->dts / 90000.0,
            pkt->size);

    return (ret < 0) ? ret : 0;
}

static int64_t mpegps_read_dts(AVFormatContext *s, int stream_index,
                               int64_t *ppos, int64_t pos_limit)
{
    int len, startcode;
    int64_t pos, pts, dts;

    pos = *ppos;
    if (avio_seek(s->pb, pos, SEEK_SET) < 0)
        return AV_NOPTS_VALUE;

    for (;;) {
        len = mpegps_read_pes_header(s, &pos, &startcode, &pts, &dts);
        if (len < 0) {
            if (s->debug & AV_FDEBUG_TS)
                av_log(s, AV_LOG_DEBUG, "none (ret=%d)\n", len);
            return AV_NOPTS_VALUE;
        }
        if (startcode == s->streams[stream_index]->id &&
            dts != AV_NOPTS_VALUE) {
            break;
        }
        avio_skip(s->pb, len);
    }
    if (s->debug & AV_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG, "pos=0x%"PRIx64" dts=0x%"PRIx64" %0.3f\n",
            pos, dts, dts / 90000.0);
    *ppos = pos;
    return dts;
}

/* ---- Disc sectors (option disc): DVD-Video VOBs, HD DVD EVOBs ----
 * The stream is read one 2048-byte sector (one pack) at a time and its
 * packets with the rules of the reference's disc reader: an empty sector has
 * none; a pack holds at most two packets unless one is MPEG audio (else:
 * fail); bytes that are no packet start after a video or private stream 1
 * packet drop the rest of the sector (elsewhere: fail); a first packet
 * running past the sector leaves the sector out (short header) or fails; a
 * PTS whose marker bits are wrong is no time; an LPCM packet shorter than its
 * audio frame header, or whose first access unit starts inside it, fails.
 *
 * Each packet's payload is a packet of the stream with the ids of the
 * reading above: video and MPEG audio 0x100 | stream_id, VC-1 0xFD00 |
 * stream_id_extension, private stream 1 the sub-stream id, after the
 * sub-stream header (sub-pictures: the id byte; audio: id, frame count,
 * first access unit pointer; TrueHD / MLP: 5 bytes). A sub-stream 0xA0-0xAF
 * is always linear PCM, its audio frame header kept in the payload (3 bytes
 * on DVD-Video, 5 on HD DVD: option lpcm_header) for the title timeline; the
 * reading above guesses MLP from that header's dynamic range byte and cuts
 * 6 more bytes, which ruins LPCM whose dynamic range is not 0x80. */

/* PES header. Packets of other streams than video, private stream 1 and
 * MPEG audio are not checked against the bytes available. A PTS is present
 * when PTS_DTS_flags say so; it is valid only when its three marker bits
 * are set. */
static int disc_parse_pes(MpegPsDiscPacket *p, const uint8_t *b, int avail)
{
    int sid, f;

    if (avail < 6 || b[0] || b[1] || b[2] != 1)
        return 0;
    sid = b[3];
    if ((sid & 0xfe) != 0xbe && avail < 9)
        return 0;
    p->len         = AV_RB16(b + 4) ? AV_RB16(b + 4) + 6 : 0;
    p->key         = sid;
    p->pts_present = 0;
    p->pts_valid   = 0;
    p->pts         = 0;
    p->first_au    = -1;
    p->extra       = 0;
    if ((sid & 0xfe) == 0xbe) {     /* padding, private stream 2 */
        p->payload = 6;
        return 1;
    }
    if (sid == 0xe0 || sid == 0xe1 || sid == 0xe2 || sid == 0xfd || sid == 0xbd || (sid & 0xe0) == 0xc0) {
        f = b[7];
        if ((f & 0xc0) == 0xc0 || (f & 0xc0) == 0x80) {
            if (avail < 14)
                return 0;
            p->pts_present = 1;
            if ((b[9] & 1) && (b[11] & 1) && (b[13] & 1)) {
                p->pts_valid = 1;
                p->pts = (uint64_t)(b[9] & 0x0e) << 29 | b[10] << 22 | (b[11] & 0xfe) << 14 |
                         b[12] << 7 | b[13] >> 1;
            }
        }
        if ((f & 1) && !(f & 0x3e)) {   /* PES extension, no other optional field */
            int e = 9 + 5 * ((f >> 6) & 1) + 5 * (f >> 7), q;

            if (avail < e + 1)
                return 0;
            if (b[e] < 0x40) {
                q = e + ((b[e] & 0x10) ? 3 : 1);
                if (b[e] & 1) {
                    if (avail < q + 2)
                        return 0;
                    if (b[q] == 0x81 && b[q + 1] < 0x80)
                        p->key = b[q + 1] << 8 | sid;
                }
            }
        }
        if (avail < b[8] + 9)
            return 0;
    }
    p->payload = b[8] + 9;
    return 1;
}

/* PES header and, for private stream 1, the sub-stream header (DVD layout:
 * audio 4 bytes {id, frame count, first access unit pointer}, MLP 5 bytes,
 * sub-pictures 1 byte). lpcm5: the LPCM audio header is 5 bytes, not 3. */
static int disc_parse_packet(MpegPsDiscPacket *p, const uint8_t *b, int avail, int lpcm5)
{
    int off, b0, cls;

    if (!disc_parse_pes(p, b, avail))
        return 0;
    if (p->key != 0xbd)
        return 1;
    off = p->payload;
    if (avail - off < 2)
        return 0;
    b0  = b[off];
    cls = b0 & 0xf8;
    if (cls >= 0x20 && cls <= 0x38) {
        p->first_au = -2;
        off += 1;
    } else if (cls == 0x80 || cls == 0x88 || cls == 0xc0 || cls == 0xa0) {
        int nf, ptr, f;

        if (cls == 0xa0)
            p->extra = lpcm5 ? 5 : 3;
        if (avail - off < 5)
            return 0;
        nf  = b[off + 1];
        ptr = AV_RB16(b + off + 2);
        f   = ptr ? ptr - 1 : -1;
        p->first_au = (nf || cls == 0x88) ? f : -1;
        off += 4;
    } else if (cls == 0xb0) {
        int v;

        if (avail - off < 6)
            return 0;
        v = AV_RB16(b + off + 1);
        p->first_au = v >= 3 ? v - 3 : -1;
        off += 5;
    } else {
        off += 1;
    }
    p->key     = b0 << 8 | 0xbd;
    p->payload = off;
    return 1;
}

/* Pack header: its length (14 + stuffing; the stuffing only counts when its
 * first byte is not 0). */
static int disc_pack_header_len(const uint8_t *s)
{
    if (AV_RB32(s) != 0x1ba || (s[4] & 0xc0) != 0x40)
        return 0;
    return (s[13] & 7) && s[14] ? 14 + (s[13] & 7) : 14;
}

int ff_mpegps_disc_packet(const uint8_t *s, int *off, MpegPsDiscPacket *p, int lpcm5)
{
    int at = *off, pos, lim, n = 0;

    p->why = "not a valid pack";
    if (!at && !(at = disc_pack_header_len(s)))
        return 0;
    if (!disc_parse_packet(p, s + at, MPEGPS_DISC_SECTOR - at, lpcm5))
        return 0;
    p->at = at;
    if (p->extra) {
        int d = p->len - p->payload;
        if (d == p->extra) {        /* nothing but the LPCM header: like padding */
            p->extra = 0;
            p->key   = 0xbe;
        } else if (d < p->extra) {
            p->why = "an LPCM packet is shorter than its audio frame header";
            return 0;
        } else {
            if (p->first_au >= 0 && p->first_au < p->extra) {
                p->why = "an LPCM packet's first audio frame starts inside its audio frame header";
                return 0;
            }
            if (p->first_au >= 0)
                p->first_au -= p->extra;
            p->payload += p->extra;
        }
    }
    if (at >= 0x7f7 && !p->len && p->key == 0xbe) {
        *off = MPEGPS_DISC_SECTOR;
        return 1;
    }
    pos = at + p->len;
    lim = p->key == 0xbf ? MPEGPS_DISC_SECTOR : pos;
    if (lim >= MPEGPS_DISC_SECTOR) {
        *off = lim;
        return 1;
    }
    for (;;) {
        MpegPsDiscPacket pad;
        int k;

        for (k = pos + 1; k < MPEGPS_DISC_SECTOR && !s[k]; k++)
            ;
        if (!s[pos] && k == MPEGPS_DISC_SECTOR)
            break;
        for (k = pos; k < MPEGPS_DISC_SECTOR && s[k] == 0xff; k++)
            ;
        if (k == MPEGPS_DISC_SECTOR || pos > 0x7f9)
            break;
        if (AV_RB32(s + pos) != 0x1be)
            goto next;
        if (!disc_parse_pes(&pad, s + pos, MPEGPS_DISC_SECTOR - pos))
            goto next;
        if (pos >= 0x7f7 && !pad.len) {
            if (pad.key == 0xbe)
                pos = MPEGPS_DISC_SECTOR;
            goto next;
        }
        if (!pad.len)
            goto next;
        pos += pad.len;
        if (pos > MPEGPS_DISC_SECTOR)
            break;
        n++;
        if (pos == MPEGPS_DISC_SECTOR)
            break;
    }
    *off = MPEGPS_DISC_SECTOR;
    return 1;
next:
    if (n && pos >= 0x601 && pos <= 0x7ff) {   /* after padding near the end: is it a packet? */
        MpegPsDiscPacket t;
        int q = pos;
        if (!disc_parse_packet(&t, s + pos, MPEGPS_DISC_SECTOR - pos, 0) || !ff_mpegps_disc_packet(s, &q, &t, lpcm5))
            pos = MPEGPS_DISC_SECTOR;
    }
    *off = pos;
    return 1;
}

int ff_mpegps_disc_sector(const uint8_t *sec, int lpcm5, MpegPsDiscPacketCb cb, void *opaque, const char **why,
                           int *at)
{
    MpegPsDiscPacket k;
    int off = 0, count = 1, mpa, ret;

    *why = NULL;
    *at  = 0;
    if (!sec[4]) {
        int all = sec[0] == 0x00 || sec[0] == 0xff;
        for (int i = 1; all && i < MPEGPS_DISC_SECTOR; i++)
            all = sec[i] == sec[0];
        if (all)
            return MPEGPS_DISC_EMPTY;
    }
    if (!ff_mpegps_disc_packet(sec, &off, &k, lpcm5)) {
        *why = k.why;
        return MPEGPS_DISC_FAIL;
    }
    if (off > MPEGPS_DISC_SECTOR) {
        *at = k.at;
        if (k.payload + k.at > 0x3f) {
            *why = "its first packet runs past the sector's end";
            return MPEGPS_DISC_FAIL;
        }
        *why = "its first packet runs past the sector's end";
        return MPEGPS_DISC_BAD;
    }
    mpa = (k.key & 0xfff0) == 0xd0;
    if ((ret = cb(opaque, sec, &k)) < 0)
        return ret;
    while (off != MPEGPS_DISC_SECTOR) {
        int rem = MPEGPS_DISC_SECTOR - off;

        if (!mpa && count >= 2) {
            *at  = off;
            *why = "it holds more than two packets";
            return MPEGPS_DISC_FAIL;
        }
        if ((rem < 6 || AV_RB24(sec + off) != 1 || ((sec[off + 3] & 0xfe) != 0xbe && rem < 9)) &&
            off >= 1 && off <= 0x7fc && (k.key == 0xe0 || (k.key & 0xff) == 0xbd)) {
            *at  = off;
            *why = "no packet starts after its video / private stream 1 packet";
            return MPEGPS_DISC_REST_DROPPED;
        }
        if (!ff_mpegps_disc_packet(sec, &off, &k, lpcm5)) {
            *at  = off;
            *why = k.why;
            return MPEGPS_DISC_FAIL;
        }
        mpa |= (k.key & 0xfff0) == 0xd0;
        if ((ret = cb(opaque, sec, &k)) < 0)
            return ret;
        count++;
    }
    return MPEGPS_DISC_OK;
}

/* The stream of a packet: FFmpeg's MPEG-PS ids and codecs; 0 when the
 * packet carries no stream (padding, private stream 2, system header,
 * sub-streams and stream ids no disc format uses). */
static int disc_stream_of(const MpegPsDiscPacket *p, int *id, enum AVMediaType *type, enum AVCodecID *codec)
{
    int sid = p->key & 0xff;

    if (sid == 0xbd) {
        int sub = p->key >> 8;
        *id   = sub;
        *type = AVMEDIA_TYPE_AUDIO;
        if (sub >= 0x20 && sub <= 0x3f) {
            *type  = AVMEDIA_TYPE_SUBTITLE;
            *codec = AV_CODEC_ID_DVD_SUBTITLE;
        } else if (sub >= 0x80 && sub <= 0x87) {
            *codec = AV_CODEC_ID_AC3;
        } else if ((sub >= 0x88 && sub <= 0x8f) || (sub >= 0x98 && sub <= 0x9f)) {
            *codec = AV_CODEC_ID_DTS;
        } else if (sub >= 0xa0 && sub <= 0xaf) {
            *codec = AV_CODEC_ID_PCM_DVD;
        } else if (sub >= 0xb0 && sub <= 0xbf) {
            *codec = AV_CODEC_ID_TRUEHD;
        } else if (sub >= 0xc0 && sub <= 0xcf) {
            *codec = AV_CODEC_ID_AC3;        /* AC-3 and E-AC-3 */
        } else {
            return 0;
        }
        return 1;
    }
    if (sid == 0xfd) {
        int ext = p->key >> 8;
        if (ext < 0x55 || ext > 0x5f)
            return 0;
        *id    = 0xfd00 | ext;
        *type  = AVMEDIA_TYPE_VIDEO;
        *codec = AV_CODEC_ID_VC1;
        return 1;
    }
    if (p->key > 0xff)
        return 0;
    if (sid >= 0xe0 && sid <= 0xef) {
        *id    = 0x100 | sid;
        *type  = AVMEDIA_TYPE_VIDEO;
        *codec = sid == 0xe2 ? AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG2VIDEO;
        return 1;
    }
    if (sid >= 0xc0 && sid <= 0xdf) {
        *id    = 0x100 | sid;
        *type  = AVMEDIA_TYPE_AUDIO;
        *codec = AV_CODEC_ID_MP2;
        return 1;
    }
    return 0;
}

typedef struct DiscSectorRead {
    AVFormatContext *s;
    int64_t          pos;        /* the sector's byte position */
} DiscSectorRead;

static int disc_on_packet(void *opaque, const uint8_t *sec, const MpegPsDiscPacket *p)
{
    DiscSectorRead *r = opaque;
    AVFormatContext *s = r->s;
    MpegDemuxContext *c = s->priv_data;
    enum AVMediaType type;
    enum AVCodecID codec;
    AVStream *st = NULL;
    AVPacket *pkt;
    int id, start, end, ret;

    if (!disc_stream_of(p, &id, &type, &codec))
        return 0;
    start = p->at + p->payload - p->extra;          /* LPCM: its audio frame header stays */
    end   = p->len ? FFMIN(p->at + p->len, MPEGPS_DISC_SECTOR) : MPEGPS_DISC_SECTOR;
    if (end <= start)
        return 0;
    for (int i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->id == id)
            st = s->streams[i];
    if (!st) {
        if (!(st = avformat_new_stream(s, NULL)))
            return AVERROR(ENOMEM);
        st->id                   = id;
        st->codecpar->codec_type = type;
        st->codecpar->codec_id   = codec;
        avpriv_set_pts_info(st, 33, 1, 90000);
    }
    if (st->discard >= AVDISCARD_ALL)
        return 0;
    if (p->pts_present && !p->pts_valid)
        av_log(s, AV_LOG_WARNING, "Disc pack reader: sector %"PRId64", stream 0x%x: the PTS's marker bits are "
               "wrong: the packet has no time\n", r->pos / MPEGPS_DISC_SECTOR, id);
    if (c->q_n == DISC_QUEUE)
        return AVERROR_BUG;
    if (!(pkt = av_packet_alloc()))
        return AVERROR(ENOMEM);
    if ((ret = av_new_packet(pkt, end - start)) < 0) {
        av_packet_free(&pkt);
        return ret;
    }
    memcpy(pkt->data, sec + start, end - start);
    pkt->pts          = p->pts_valid ? (int64_t)p->pts : AV_NOPTS_VALUE;
    pkt->dts          = AV_NOPTS_VALUE;
    pkt->pos          = r->pos + p->at;
    pkt->stream_index = st->index;
    c->q[(c->q_head + c->q_n++) % DISC_QUEUE] = pkt;
    return 0;
}

static int disc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MpegDemuxContext *c = s->priv_data;
    uint8_t sec[MPEGPS_DISC_SECTOR];

    for (;;) {
        DiscSectorRead r = { s, avio_tell(s->pb) };
        const char *why;
        int n, at, ret;

        if (c->q_n) {
            AVPacket *q = c->q[c->q_head];
            c->q_head = (c->q_head + 1) % DISC_QUEUE;
            c->q_n--;
            av_packet_move_ref(pkt, q);
            av_packet_free(&q);
            return 0;
        }
        if (c->sticky)
            return c->sticky;
        n = avio_read(s->pb, sec, MPEGPS_DISC_SECTOR);
        if (n == AVERROR_EOF || n == 0)
            return AVERROR_EOF;
        if (n < 0)
            return n;
        if (n < MPEGPS_DISC_SECTOR) {
            av_log(s, AV_LOG_ERROR, "Disc pack reader: the stream ends %d bytes into sector %"PRId64"\n", n,
                   c->sector);
            return c->sticky = AVERROR_INVALIDDATA;
        }
        ret = ff_mpegps_disc_sector(sec, c->lpcm_header == 5, disc_on_packet, &r, &why, &at);
        c->sector++;
        switch (ret) {
        case MPEGPS_DISC_OK:
        case MPEGPS_DISC_EMPTY:
            break;
        case MPEGPS_DISC_BAD:
            av_log(s, AV_LOG_WARNING, "Disc pack reader: sector %"PRId64": %s: left out\n", c->sector - 1, why);
            break;
        case MPEGPS_DISC_REST_DROPPED:
            av_log(s, AV_LOG_WARNING, "Disc pack reader: sector %"PRId64": %s (byte %d): the rest of the sector "
                   "is left out\n", c->sector - 1, why, at);
            break;
        case MPEGPS_DISC_FAIL:
            av_log(s, AV_LOG_ERROR, "Disc pack reader: sector %"PRId64" (byte %d): %s\n", c->sector - 1, at, why);
            return c->sticky = AVERROR_INVALIDDATA;
        default:
            if (ret < 0)
                return c->sticky = ret;
        }
    }
}

static int mpegps_read_close(AVFormatContext *s)
{
    MpegDemuxContext *c = s->priv_data;

    while (c->q_n) {
        av_packet_free(&c->q[c->q_head]);
        c->q_head = (c->q_head + 1) % DISC_QUEUE;
        c->q_n--;
    }
    return 0;
}

#define OFFSET(x) offsetof(MpegDemuxContext, x)
static const AVOption mpegps_options[] = {
    { "disc", "read the stream as disc sectors (DVD-Video VOBs, HD DVD EVOBs): one 2048-byte pack at a time with "
      "the disc reader's rules, raw payloads", OFFSET(disc), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1,
      AV_OPT_FLAG_DECODING_PARAM },
    { "lpcm_header", "disc sectors: length of the LPCM audio frame header (3 DVD-Video, 5 HD DVD)", OFFSET(lpcm_header),
      AV_OPT_TYPE_INT, { .i64 = 3 }, 3, 5, AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};

static const AVClass mpegps_class = {
    .class_name = "mpeg demuxer",
    .item_name  = av_default_item_name,
    .option     = mpegps_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_mpegps_demuxer = {
    .p.name         = "mpeg",
    .p.long_name    = NULL_IF_CONFIG_SMALL("MPEG-PS (MPEG-2 Program Stream)"),
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_TS_DISCONT,
    .p.priv_class   = &mpegps_class,
    .priv_data_size = sizeof(MpegDemuxContext),
    .read_probe     = mpegps_probe,
    .read_header    = mpegps_read_header,
    .read_packet    = mpegps_read_packet,
    .read_close     = mpegps_read_close,
    .read_timestamp = mpegps_read_dts,
};

#if CONFIG_VOBSUB_DEMUXER

#include "subtitles.h"
#include "libavutil/avassert.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"

#define REF_STRING "# VobSub index file,"
#define MAX_LINE_SIZE 2048

typedef struct VobSubDemuxContext {
    const AVClass *class;
    AVFormatContext *sub_ctx;
    FFDemuxSubtitlesQueue q[32];
    char *sub_name;
} VobSubDemuxContext;

static int vobsub_probe(const AVProbeData *p)
{
    if (!strncmp(p->buf, REF_STRING, sizeof(REF_STRING) - 1))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int vobsub_read_close(AVFormatContext *s)
{
    VobSubDemuxContext *vobsub = s->priv_data;
    int i;

    for (i = 0; i < s->nb_streams; i++)
        ff_subtitles_queue_clean(&vobsub->q[i]);
    avformat_close_input(&vobsub->sub_ctx);
    return 0;
}

static int vobsub_read_header(AVFormatContext *s)
{
    int i, ret = 0, header_parsed = 0, langidx = 0;
    VobSubDemuxContext *vobsub = s->priv_data;
    const AVInputFormat *iformat;
    size_t fname_len;
    AVBPrint header;
    int64_t delay = 0;
    AVStream *st = NULL;
    int stream_id = -1;
    char id[64] = {0};
    char alt[MAX_LINE_SIZE] = {0};

    if (!vobsub->sub_name) {
        char *ext;
        vobsub->sub_name = av_strdup(s->url);
        if (!vobsub->sub_name) {
            return AVERROR(ENOMEM);
        }

        fname_len = strlen(vobsub->sub_name);
        ext = vobsub->sub_name - 3 + fname_len;
        if (fname_len < 4 || *(ext - 1) != '.') {
            av_log(s, AV_LOG_ERROR, "The input index filename is too short "
                   "to guess the associated .SUB file\n");
            return AVERROR_INVALIDDATA;
        }
        memcpy(ext, !strncmp(ext, "IDX", 3) ? "SUB" : "sub", 3);
        av_log(s, AV_LOG_VERBOSE, "IDX/SUB: %s -> %s\n", s->url, vobsub->sub_name);
    }

    if (!(iformat = av_find_input_format("mpeg"))) {
        return AVERROR_DEMUXER_NOT_FOUND;
    }

    vobsub->sub_ctx = avformat_alloc_context();
    if (!vobsub->sub_ctx) {
        return AVERROR(ENOMEM);
    }

    if ((ret = ff_copy_whiteblacklists(vobsub->sub_ctx, s)) < 0)
        return ret;

    ret = avformat_open_input(&vobsub->sub_ctx, vobsub->sub_name, iformat, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Unable to open %s as MPEG subtitles\n", vobsub->sub_name);
        return ret;
    }

    av_bprint_init(&header, 0, INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE);

    while (!avio_feof(s->pb)) {
        char line[MAX_LINE_SIZE];
        int len = ff_get_line(s->pb, line, sizeof(line));

        if (!len)
            break;

        line[strcspn(line, "\r\n")] = 0;

        if (!strncmp(line, "id:", 3)) {
            if (sscanf(line, "id: %63[^,], index: %u", id, &stream_id) != 2) {
                av_log(s, AV_LOG_WARNING, "Unable to parse index line '%s', "
                       "assuming 'id: und, index: 0'\n", line);
                strcpy(id, "und");
                stream_id = 0;
            }

            if (stream_id >= FF_ARRAY_ELEMS(vobsub->q)) {
                av_log(s, AV_LOG_ERROR, "Maximum number of subtitles streams reached\n");
                ret = AVERROR(EINVAL);
                goto end;
            }

            header_parsed = 1;
            alt[0] = '\0';
            /* We do not create the stream immediately to avoid adding empty
             * streams. See the following timestamp entry. */

            av_log(s, AV_LOG_DEBUG, "IDX stream[%d] id=%s\n", stream_id, id);

        } else if (!strncmp(line, "timestamp:", 10)) {
            AVPacket *sub;
            int hh, mm, ss, ms;
            int64_t pos, timestamp;
            const char *p = line + 10;

            if (stream_id == -1) {
                av_log(s, AV_LOG_ERROR, "Timestamp declared before any stream\n");
                ret = AVERROR_INVALIDDATA;
                goto end;
            }

            if (!st || st->id != stream_id) {
                st = NULL;
                for (i = 0; i < s->nb_streams; i++) {
                    if (s->streams[i]->id == stream_id) {
                        st = s->streams[i];
                        break;
                    }
                }
            }
            if (!st) {
                if (s->nb_streams >= FF_ARRAY_ELEMS(vobsub->q)) {
                    av_log(s, AV_LOG_ERROR, "Maximum number of subtitle streams reached\n");
                    ret = AVERROR_INVALIDDATA;
                    goto end;
                }
                st = avformat_new_stream(s, NULL);
                if (!st) {
                    ret = AVERROR(ENOMEM);
                    goto end;
                }
                st->id = stream_id;
                st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
                st->codecpar->codec_id   = AV_CODEC_ID_DVD_SUBTITLE;
                avpriv_set_pts_info(st, 64, 1, 1000);
                av_dict_set(&st->metadata, "language", id, 0);
                if (alt[0])
                    av_dict_set(&st->metadata, "title", alt, 0);
            }

            if (sscanf(p, "%02d:%02d:%02d:%03d, filepos: %"SCNx64,
                       &hh, &mm, &ss, &ms, &pos) != 5) {
                av_log(s, AV_LOG_ERROR, "Unable to parse timestamp line '%s', "
                       "abort parsing\n", line);
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
            timestamp = (hh*3600LL + mm*60LL + ss) * 1000LL + ms + delay;
            timestamp = av_rescale_q(timestamp, av_make_q(1, 1000), st->time_base);

            sub = ff_subtitles_queue_insert(&vobsub->q[st->index], "", 0, 0);
            if (!sub) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            sub->pos = pos;
            sub->pts = timestamp;
            sub->stream_index = st->index;

        } else if (!strncmp(line, "alt:", 4)) {
            const char *p = line + 4;

            while (*p == ' ')
                p++;
            av_log(s, AV_LOG_DEBUG, "IDX stream[%d] name=%s\n", stream_id, p);
            av_strlcpy(alt, p, sizeof(alt));
            header_parsed = 1;

        } else if (!strncmp(line, "delay:", 6)) {
            int sign = 1, hh = 0, mm = 0, ss = 0, ms = 0;
            const char *p = line + 6;

            while (*p == ' ')
                p++;
            if (*p == '-' || *p == '+') {
                sign = *p == '-' ? -1 : 1;
                p++;
            }
            sscanf(p, "%d:%d:%d:%d", &hh, &mm, &ss, &ms);
            delay = ((hh*3600LL + mm*60LL + ss) * 1000LL + ms) * sign;

        } else if (!strncmp(line, "langidx:", 8)) {
            const char *p = line + 8;

            if (sscanf(p, "%d", &langidx) != 1)
                av_log(s, AV_LOG_ERROR, "Invalid langidx specified\n");

        } else if (!header_parsed) {
            if (line[0] && line[0] != '#')
                av_bprintf(&header, "%s\n", line);
        }
    }

    if (langidx < s->nb_streams)
        s->streams[langidx]->disposition |= AV_DISPOSITION_DEFAULT;

    for (i = 0; i < s->nb_streams; i++) {
        vobsub->q[i].sort = SUB_SORT_POS_TS;
        vobsub->q[i].keep_duplicates = 1;
        ff_subtitles_queue_finalize(s, &vobsub->q[i]);
    }

    if (!av_bprint_is_complete(&header)) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        ret = ff_alloc_extradata(par, header.len);
        if (ret < 0) {
            goto end;
        }
        memcpy(par->extradata, header.str, header.len);
    }
end:
    av_bprint_finalize(&header, NULL);
    return ret;
}

static int vobsub_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    VobSubDemuxContext *vobsub = s->priv_data;
    FFDemuxSubtitlesQueue *q;
    AVIOContext *pb = vobsub->sub_ctx->pb;
    int ret, psize, total_read = 0, i;

    int64_t min_ts = INT64_MAX;
    int sid = 0;
    for (i = 0; i < s->nb_streams; i++) {
        FFDemuxSubtitlesQueue *tmpq = &vobsub->q[i];
        int64_t ts;
        av_assert0(tmpq->nb_subs);

        if (tmpq->current_sub_idx >= tmpq->nb_subs)
            continue;

        ts = tmpq->subs[tmpq->current_sub_idx]->pts;
        if (ts < min_ts) {
            min_ts = ts;
            sid = i;
        }
    }
    q = &vobsub->q[sid];
    /* The returned packet will have size zero,
     * so that it can be directly used with av_grow_packet. */
    ret = ff_subtitles_queue_read_packet(q, pkt);
    if (ret < 0)
        return ret;

    /* compute maximum packet size using the next packet position. This is
     * useful when the len in the header is non-sense */
    if (q->current_sub_idx < q->nb_subs) {
        psize = q->subs[q->current_sub_idx]->pos - pkt->pos;
    } else {
        int64_t fsize = avio_size(pb);
        psize = fsize < 0 ? 0xffff : fsize - pkt->pos;
    }

    avio_seek(pb, pkt->pos, SEEK_SET);

    do {
        int n, to_read, startcode;
        int64_t pts, dts;
        int64_t old_pos = avio_tell(pb), new_pos;
        int pkt_size;

        ret = mpegps_read_pes_header(vobsub->sub_ctx, NULL, &startcode, &pts, &dts);
        if (ret < 0) {
            if (pkt->size) // raise packet even if incomplete
                break;
            return ret;
        }
        to_read = ret & 0xffff;
        new_pos = avio_tell(pb);
        pkt_size = ret + (new_pos - old_pos);

        /* this prevents reads above the current packet */
        if (total_read + pkt_size > psize)
            break;
        total_read += pkt_size;

        /* the current chunk doesn't match the stream index (unlikely) */
        if ((startcode & 0x1f) != s->streams[pkt->stream_index]->id)
            break;

        ret = av_grow_packet(pkt, to_read);
        if (ret < 0)
            return ret;

        n = avio_read(pb, pkt->data + (pkt->size - to_read), to_read);
        if (n < to_read)
            pkt->size -= to_read - n;
    } while (total_read < psize);

    return 0;
}

static int vobsub_read_seek(AVFormatContext *s, int stream_index,
                            int64_t min_ts, int64_t ts, int64_t max_ts, int flags)
{
    VobSubDemuxContext *vobsub = s->priv_data;

    /* Rescale requested timestamps based on the first stream (timebase is the
     * same for all subtitles stream within a .idx/.sub). Rescaling is done just
     * like in avformat_seek_file(). */
    if (stream_index == -1 && s->nb_streams != 1) {
        int i, ret = 0;
        AVRational time_base = s->streams[0]->time_base;
        ts = av_rescale_q(ts, AV_TIME_BASE_Q, time_base);
        min_ts = av_rescale_rnd(min_ts, time_base.den,
                                time_base.num * (int64_t)AV_TIME_BASE,
                                AV_ROUND_UP   | AV_ROUND_PASS_MINMAX);
        max_ts = av_rescale_rnd(max_ts, time_base.den,
                                time_base.num * (int64_t)AV_TIME_BASE,
                                AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX);
        for (i = 0; i < s->nb_streams; i++) {
            int r = ff_subtitles_queue_seek(&vobsub->q[i], s, stream_index,
                                            min_ts, ts, max_ts, flags);
            if (r < 0)
                ret = r;
        }
        return ret;
    }

    if (stream_index == -1) // only 1 stream
        stream_index = 0;
    return ff_subtitles_queue_seek(&vobsub->q[stream_index], s, stream_index,
                                   min_ts, ts, max_ts, flags);
}

static const AVOption options[] = {
    { "sub_name", "URI for .sub file", offsetof(VobSubDemuxContext, sub_name), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};

static const AVClass vobsub_demuxer_class = {
    .class_name = "vobsub",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_vobsub_demuxer = {
    .p.name         = "vobsub",
    .p.long_name    = NULL_IF_CONFIG_SMALL("VobSub subtitle format"),
    .p.flags        = AVFMT_SHOW_IDS,
    .p.extensions   = "idx",
    .p.priv_class   = &vobsub_demuxer_class,
    .priv_data_size = sizeof(VobSubDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = vobsub_probe,
    .read_header    = vobsub_read_header,
    .read_packet    = vobsub_read_packet,
    .read_seek2     = vobsub_read_seek,
    .read_close     = vobsub_read_close,
};
#endif
