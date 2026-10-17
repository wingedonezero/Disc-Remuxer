/*
 * The title timeline's host (timeline/disctl.h DRTimelineHost): the
 * TrueHD / MLP end-of-stream decode — the processing library answering a
 * question on copies of data. The program and the test bridge register it with
 * dr_analyze_use(). Here too: the build-time check that the timeline's
 * codec ids are the library's values.
 */

#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"

#include "base/err.h"
#include "base/vlog.h"

#include "analyze/analyze.h"
#include "disc/info.h"
#include "timeline/disctl.h"

/* The timeline's codec ids are the library's values (disctl.h). */
#define SAME(a, b) _Static_assert((int)(a) == (int)(b), #a)
SAME(DR_CODEC_NONE, AV_CODEC_ID_NONE);
SAME(DR_CODEC_MPEG1VIDEO, AV_CODEC_ID_MPEG1VIDEO);
SAME(DR_CODEC_MPEG2VIDEO, AV_CODEC_ID_MPEG2VIDEO);
SAME(DR_CODEC_H264, AV_CODEC_ID_H264);
SAME(DR_CODEC_VC1, AV_CODEC_ID_VC1);
SAME(DR_CODEC_HEVC, AV_CODEC_ID_HEVC);
SAME(DR_CODEC_PCM_DVD, AV_CODEC_ID_PCM_DVD);
SAME(DR_CODEC_PCM_BLURAY, AV_CODEC_ID_PCM_BLURAY);
SAME(DR_CODEC_PCM_S16LE, AV_CODEC_ID_PCM_S16LE);
SAME(DR_CODEC_PCM_S24LE, AV_CODEC_ID_PCM_S24LE);
SAME(DR_CODEC_EIA_608, AV_CODEC_ID_EIA_608);
SAME(DR_CODEC_MP1, AV_CODEC_ID_MP1);
SAME(DR_CODEC_MP2, AV_CODEC_ID_MP2);
SAME(DR_CODEC_MP3, AV_CODEC_ID_MP3);
SAME(DR_CODEC_AAC, AV_CODEC_ID_AAC);
SAME(DR_CODEC_AAC_LATM, AV_CODEC_ID_AAC_LATM);
SAME(DR_CODEC_AC3, AV_CODEC_ID_AC3);
SAME(DR_CODEC_EAC3, AV_CODEC_ID_EAC3);
SAME(DR_CODEC_DTS, AV_CODEC_ID_DTS);
SAME(DR_CODEC_TRUEHD, AV_CODEC_ID_TRUEHD);
SAME(DR_CODEC_MLP, AV_CODEC_ID_MLP);
SAME(DR_CODEC_DVD_SUBTITLE, AV_CODEC_ID_DVD_SUBTITLE);
SAME(DR_CODEC_HDMV_PGS_SUBTITLE, AV_CODEC_ID_HDMV_PGS_SUBTITLE);
SAME(DR_CODEC_HDMV_TEXT_SUBTITLE, AV_CODEC_ID_HDMV_TEXT_SUBTITLE);
SAME(DR_CODEC_SUBRIP, AV_CODEC_ID_SUBRIP);
SAME(DR_NOPTS_VALUE, AV_NOPTS_VALUE);

/* The disc info's pinned values (disc/info.h). */
SAME(DISC_MEDIA_VIDEO, AVMEDIA_TYPE_VIDEO);
SAME(DISC_MEDIA_AUDIO, AVMEDIA_TYPE_AUDIO);
SAME(DISC_MEDIA_DATA, AVMEDIA_TYPE_DATA);
SAME(DISC_MEDIA_SUBTITLE, AVMEDIA_TYPE_SUBTITLE);
SAME(DISC_PROFILE_UNKNOWN, AV_PROFILE_UNKNOWN);
SAME(DISC_LEVEL_UNKNOWN, AV_LEVEL_UNKNOWN);
SAME(DISC_PROFILE_DTS_HD_HRA, AV_PROFILE_DTS_HD_HRA);
SAME(DISC_PROFILE_DTS_HD_MA, AV_PROFILE_DTS_HD_MA);
SAME(DISC_PROFILE_DTS_HD_MA_X, AV_PROFILE_DTS_HD_MA_X);
SAME(DISC_PROFILE_DTS_HD_MA_X_IMAX, AV_PROFILE_DTS_HD_MA_X_IMAX);
SAME(DISC_PROFILE_DTS_EXPRESS, AV_PROFILE_DTS_EXPRESS);
SAME(DISC_PROFILE_TRUEHD_ATMOS, AV_PROFILE_TRUEHD_ATMOS);
SAME(DISC_PROFILE_EAC3_DDP_ATMOS, AV_PROFILE_EAC3_DDP_ATMOS);
SAME(DISC_PROFILE_H264_CONSTRAINED, AV_PROFILE_H264_CONSTRAINED);
SAME(DISC_PROFILE_H264_INTRA, AV_PROFILE_H264_INTRA);
SAME(DISC_PROFILE_H264_BASELINE, AV_PROFILE_H264_BASELINE);
SAME(DISC_PROFILE_H264_MAIN, AV_PROFILE_H264_MAIN);
SAME(DISC_PROFILE_H264_EXTENDED, AV_PROFILE_H264_EXTENDED);
SAME(DISC_PROFILE_H264_HIGH, AV_PROFILE_H264_HIGH);
SAME(DISC_PROFILE_H264_HIGH_10, AV_PROFILE_H264_HIGH_10);
SAME(DISC_PROFILE_H264_MULTIVIEW_HIGH, AV_PROFILE_H264_MULTIVIEW_HIGH);
SAME(DISC_PROFILE_H264_HIGH_422, AV_PROFILE_H264_HIGH_422);
SAME(DISC_PROFILE_H264_STEREO_HIGH, AV_PROFILE_H264_STEREO_HIGH);
SAME(DISC_PROFILE_H264_HIGH_444, AV_PROFILE_H264_HIGH_444);
SAME(DISC_PROFILE_H264_HIGH_444_PREDICTIVE, AV_PROFILE_H264_HIGH_444_PREDICTIVE);
SAME(DISC_PROFILE_H264_CAVLC_444, AV_PROFILE_H264_CAVLC_444);
SAME(DISC_PROFILE_HEVC_MAIN, AV_PROFILE_HEVC_MAIN);
SAME(DISC_PROFILE_HEVC_MAIN_10, AV_PROFILE_HEVC_MAIN_10);
SAME(DISC_PROFILE_HEVC_MAIN_STILL_PICTURE, AV_PROFILE_HEVC_MAIN_STILL_PICTURE);
SAME(DISC_PROFILE_HEVC_REXT, AV_PROFILE_HEVC_REXT);
SAME(DISC_PROFILE_HEVC_MULTIVIEW_MAIN, AV_PROFILE_HEVC_MULTIVIEW_MAIN);
SAME(DISC_PROFILE_HEVC_SCC, AV_PROFILE_HEVC_SCC);
_Static_assert(DISC_CH_FRONT_LEFT == AV_CH_FRONT_LEFT && DISC_CH_FRONT_RIGHT == AV_CH_FRONT_RIGHT &&
               DISC_CH_FRONT_CENTER == AV_CH_FRONT_CENTER && DISC_CH_LOW_FREQUENCY == AV_CH_LOW_FREQUENCY &&
               DISC_CH_LOW_FREQUENCY_2 == AV_CH_LOW_FREQUENCY_2, "channel bits");

/* ---- TrueHD / MLP: the access units decoded in order ---- */

static int host_mlp_last_au_samples(void *logctx, const uint8_t *const *aus, const int *sizes,
                                    int n, int *count)
{
    const AVCodec *dec;
    AVCodecContext *ctx = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int ret = DR_ERR(ENOMEM);

    if (!pkt || !frame || n < 1)
        goto end;
    dec = avcodec_find_decoder(aus[0][7] == 0xBA ? AV_CODEC_ID_TRUEHD : AV_CODEC_ID_MLP);
    if (!dec || !(ctx = avcodec_alloc_context3(dec))) {
        ret = DR_ERR(ENOSYS);
        goto end;
    }
    if ((ret = avcodec_open2(ctx, dec, NULL)) < 0)
        goto end;
    for (int i = 0; i < n; i++) {
        av_packet_unref(pkt);
        if ((ret = av_new_packet(pkt, sizes[i])) < 0)
            goto end;
        memcpy(pkt->data, aus[i], sizes[i]);
        if ((ret = avcodec_send_packet(ctx, pkt)) < 0 ||
            (ret = avcodec_receive_frame(ctx, frame)) < 0)
            goto end;
        *count = frame->nb_samples;
        av_frame_unref(frame);
    }
    ret = 0;
end:
    if (ret < 0 && ret != DR_ERR(ENOMEM))
        ret = ret == DR_ERR(ENOSYS) ? ret : DR_ERR_EXTERNAL;
    avcodec_free_context(&ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    return ret;
}

static const DRTimelineHost host = {
    .mlp_last_au_samples = host_mlp_last_au_samples,
};

const char *dr_analyze_version(char *buf, size_t size)
{
    unsigned v = avformat_version();

    snprintf(buf, size, "FFmpeg %s (libavformat %u.%u.%u)", av_version_info(), v >> 16, v >> 8 & 0xff, v & 0xff);
    return buf;
}

void dr_analyze_use(void)
{
    ff_disctl_set_host(&host);
    dr_analyze_use_probe();
}
