/*
 * A finished Matroska file read back (mkv_readback.h).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>

#include "analyze/analyze.h"
#include "analyze/mkv_readback.h"
#include "base/err.h"

/* A block of H.264 in Matroska: NAL units, each after its length (ISO/IEC
 * 14496-15: lengthSizeMinusOne of the track's configuration record). */
static int nal_units(DrContentSum *s, const uint8_t *p, int size, int len_size)
{
    int at = 0;

    while (at + len_size <= size) {
        uint32_t n = 0;
        for (int i = 0; i < len_size; i++)
            n = n << 8 | p[at + i];
        at += len_size;
        if (n > (uint32_t)(size - at))
            return -1;
        dr_content_sum_nal(s, p + at, n);
        at += (int)n;
    }
    return at == size ? 0 : -1;
}

int dr_mkv_read_back(const char *path, DrMkvTrack *tracks, int nb, char *why, int why_size)
{
    const AVInputFormat *fmt = av_find_input_format("matroska");
    AVFormatContext *ic = NULL;
    AVPacket *pkt = av_packet_alloc();
    DrContentSum **sums = av_calloc(nb ? nb : 1, sizeof(*sums));
    int *len_size = av_calloc(nb ? nb : 1, sizeof(*len_size));
    char *url = av_asprintf("file:%s", path);
    int ret;

    if (why_size > 0)
        why[0] = 0;
    if (!pkt || !sums || !len_size || !url) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    if (!fmt) {
        snprintf(why, why_size, "the processing library has no Matroska reader");
        ret = DR_ERR(ENOSYS);
        goto end;
    }
    /* (what the library says while it reads the file is about reading it, not about the disc) */
    dr_analyze_log_detail(1);
    if ((ret = avformat_open_input(&ic, url, fmt, NULL)) < 0) {
        snprintf(why, why_size, "the file cannot be opened as Matroska");
        ret = DR_ERR_EXTERNAL;
        goto end;
    }
    if ((int)ic->nb_streams != nb) {
        snprintf(why, why_size, "the file holds %u track(s), %d were given to the muxer", ic->nb_streams, nb);
        ret = DR_ERR_INVALID;
        goto end;
    }
    for (int i = 0; i < nb; i++) {
        const AVCodecParameters *par = ic->streams[i]->codecpar;

        tracks[i].codec = par->codec_id;
        memset(&tracks[i].got, 0, sizeof(tracks[i].got));
        if (tracks[i].kind == DR_CONTENT_NONE) {
            ic->streams[i]->discard = AVDISCARD_ALL;
            continue;
        }
        if ((ret = dr_content_sum_open(&sums[i], tracks[i].kind)) < 0)
            goto end;
        if (tracks[i].kind == DR_CONTENT_NAL) {
            if (par->extradata_size < 7 || par->extradata[0] != 1) {
                snprintf(why, why_size, "track %d has no H.264 configuration record", i);
                ret = DR_ERR_INVALID;
                goto end;
            }
            len_size[i] = (par->extradata[4] & 3) + 1;
        }
    }
    for (;;) {
        int i;

        ret = av_read_frame(ic, pkt);
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            snprintf(why, why_size, "the file cannot be read to its end");
            ret = DR_ERR_EXTERNAL;
            goto end;
        }
        i = pkt->stream_index;
        if (i >= 0 && i < nb && sums[i]) {
            if (tracks[i].kind == DR_CONTENT_NAL) {
                if (nal_units(sums[i], pkt->data, pkt->size, len_size[i]) < 0)
                    tracks[i].got.odd = 1;      /* a block that is not whole NAL units */
            } else
                dr_content_sum_data(sums[i], pkt->data, pkt->size);
        }
        av_packet_unref(pkt);
    }
    for (int i = 0; i < nb; i++)
        if (sums[i]) {
            int odd = tracks[i].got.odd;
            dr_content_sum_finish(sums[i], &tracks[i].got);
            tracks[i].got.odd |= odd;
        }
    ret = 0;
end:
    dr_analyze_log_detail(0);
    if (ret == DR_ERR(ENOMEM) && why_size > 0 && !why[0])
        snprintf(why, why_size, "out of memory");
    for (int i = 0; sums && i < nb; i++)
        dr_content_sum_close(&sums[i]);
    av_free(sums);
    av_free(len_size);
    av_packet_free(&pkt);
    avformat_close_input(&ic);
    av_free(url);
    return ret;
}
