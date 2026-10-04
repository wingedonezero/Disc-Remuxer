/*
 * DVD-Video demuxer, powered by libdvdnav and libdvdread
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

/*
 * DVD-Video demuxer: video, audio and subpicture streams from the IFO attributes
 */

#include "dvdvideo_internal.h"

static const char dvdvideo_subp_viewport_labels[4][13] = {
    "Fullscreen", "Widescreen", "Letterbox", "Pan and Scan"
};

static int dvdvideo_video_stream_analyze(AVFormatContext *s, video_attr_t video_attr,
                                         DVDVideoVTSVideoStreamEntry *entry)
{
    AVRational framerate;
    int height = 0;
    int width = 0;
    int is_pal = video_attr.video_format == 1;

    framerate = is_pal ? (AVRational) { 25, 1 } : (AVRational) { 30000, 1001 };
    height = is_pal ? 576 : 480;

    if (height > 0) {
        switch (video_attr.picture_size) {
            case 0: /* D1 */
                width = 720;
                break;
            case 1: /* 4CIF */
                width = 704;
                break;
            case 2: /* Half D1 */
                width = 352;
                break;
            case 3: /* CIF */
                width = 352;
                height /= 2;
                break;
        }
    }

    if (!width || !height) {
        av_log(s, AV_LOG_ERROR, "Invalid video stream parameters in the IFO headers, "
                                "this could be an authoring error or empty title "
                                "(video_format=%d picture_size=%d)\n",
                                video_attr.video_format, video_attr.picture_size);

        return AVERROR_INVALIDDATA;
    }

    entry->startcode = 0x1E0;
    entry->codec_id = !video_attr.mpeg_version ? AV_CODEC_ID_MPEG1VIDEO : AV_CODEC_ID_MPEG2VIDEO;
    entry->width = width;
    entry->height = height;
    entry->dar = video_attr.display_aspect_ratio ? (AVRational) { 16, 9 } : (AVRational) { 4, 3 };
    entry->framerate = framerate;

    return 0;
}

static int dvdvideo_video_stream_add(AVFormatContext *s, DVDVideoVTSVideoStreamEntry *entry)
{
    AVStream *st;
    FFStream *sti;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->id = entry->startcode;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = entry->codec_id;
    st->codecpar->width = entry->width;
    st->codecpar->height = entry->height;
    st->codecpar->format = AV_PIX_FMT_YUV420P;
    st->codecpar->color_range = AVCOL_RANGE_MPEG;

#if FF_API_R_FRAME_RATE
    st->r_frame_rate = entry->framerate;
#endif
    st->avg_frame_rate = entry->framerate;

    sti = ffstream(st);
    sti->request_probe = 0;
    sti->need_parsing = AVSTREAM_PARSE_HEADERS;
    sti->display_aspect_ratio = entry->dar;

    avpriv_set_pts_info(st, DVDVIDEO_PTS_WRAP_BITS,
                        DVDVIDEO_TIME_BASE_Q.num, DVDVIDEO_TIME_BASE_Q.den);

    return 0;
}

int ff_dvdvideo_video_stream_setup(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    int ret;
    DVDVideoVTSVideoStreamEntry entry = {0};
    video_attr_t video_attr;

    if (c->opt_menu)
        video_attr = !c->opt_menu_vts ? c->vmg_ifo->vmgi_mat->vmgm_video_attr :
                                        c->vts_ifo->vtsi_mat->vtsm_video_attr;
    else
        video_attr = c->vts_ifo->vtsi_mat->vts_video_attr;

    if ((ret = dvdvideo_video_stream_analyze(s, video_attr, &entry)) < 0 ||
        (ret = dvdvideo_video_stream_add(s, &entry)) < 0) {

        av_log(s, AV_LOG_ERROR, "Unable to add video stream\n");
        return ret;
    }

    return 0;
}

static int dvdvideo_audio_stream_analyze(AVFormatContext *s, audio_attr_t audio_attr,
                                         uint16_t audio_control, DVDVideoPGCAudioStreamEntry *entry)
{
    int startcode = 0;
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    int sample_fmt = AV_SAMPLE_FMT_NONE;
    int sample_rate = 0;
    int bit_depth = 0;
    int nb_channels = 0;
    AVChannelLayout ch_layout = (AVChannelLayout) {0};
    char lang_dvd[3] = {0};

    int position = (audio_control & 0x7F00) >> 8;

    /* XXX(PATCHWELCOME): SDDS is not supported due to lack of sample material */
    switch (audio_attr.audio_format) {
        case 0: /* AC3 */
            codec_id = AV_CODEC_ID_AC3;
            sample_fmt = AV_SAMPLE_FMT_FLTP;
            sample_rate = 48000;
            startcode = 0x80 + position;
            break;
        case 2: /* MP1 */
            codec_id = AV_CODEC_ID_MP1;
            sample_fmt = audio_attr.quantization ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
            sample_rate = 48000;
            bit_depth = audio_attr.quantization ? 20 : 16;
            startcode = 0x1C0 + position;
            break;
        case 3: /* MP2 */
            codec_id = AV_CODEC_ID_MP2;
            sample_fmt = audio_attr.quantization ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
            sample_rate = 48000;
            bit_depth = audio_attr.quantization ? 20 : 16;
            startcode = 0x1C0 + position;
            break;
        case 4: /* DVD PCM */
            codec_id = AV_CODEC_ID_PCM_DVD;
            sample_fmt = audio_attr.quantization ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
            sample_rate = audio_attr.sample_frequency ? 96000 : 48000;
            bit_depth = audio_attr.quantization == 2 ? 24 : (audio_attr.quantization ? 20 : 16);
            startcode = 0xA0 + position;
            break;
        case 6: /* DCA */
            codec_id = AV_CODEC_ID_DTS;
            sample_fmt = AV_SAMPLE_FMT_FLTP;
            sample_rate = 48000;
            bit_depth = audio_attr.quantization == 2 ? 24 : (audio_attr.quantization ? 20 : 16);
            startcode = 0x88 + position;
            break;
    }

    nb_channels = audio_attr.channels + 1;

    if (codec_id == AV_CODEC_ID_NONE     ||
        startcode == 0                   ||
        sample_fmt == AV_SAMPLE_FMT_NONE ||
        sample_rate == 0                 ||
        nb_channels == 0) {

        av_log(s, AV_LOG_ERROR, "Invalid audio stream parameters in the IFO headers, "
                                "this could be an authoring error or dummy title "
                                "(stream position %d in IFO)\n", position);
        return AVERROR_INVALIDDATA;
    }

    if (nb_channels == 1)
        ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_MONO;
    else if (nb_channels == 2)
        ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
    else if (nb_channels == 6)
        ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_5POINT1;
    else if (nb_channels == 7)
        ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_6POINT1;
    else if (nb_channels == 8)
        ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_7POINT1;

    /* XXX(PATCHWELCOME): IFO structures have metadata on karaoke tracks for additional features */
    if (audio_attr.application_mode == 1) {
        entry->disposition |= AV_DISPOSITION_KARAOKE;

        av_log(s, AV_LOG_WARNING, "Extended karaoke metadata is not supported at this time "
                                  "(stream id=%d)\n", startcode);
    }

    if (audio_attr.code_extension == 2)
        entry->disposition |= AV_DISPOSITION_VISUAL_IMPAIRED;
    if (audio_attr.code_extension == 3 || audio_attr.code_extension == 4)
        entry->disposition |= AV_DISPOSITION_COMMENT;

    AV_WB16(lang_dvd, audio_attr.lang_code);

    entry->startcode = startcode;
    entry->codec_id = codec_id;
    entry->sample_rate = sample_rate;
    entry->bit_depth = bit_depth;
    entry->nb_channels = nb_channels;
    entry->ch_layout = ch_layout;
    entry->lang_iso = ff_convert_lang_to(lang_dvd, AV_LANG_ISO639_2_BIBL);

    return 0;
}

static int dvdvideo_audio_stream_add(AVFormatContext *s, DVDVideoPGCAudioStreamEntry *entry)
{
    AVStream *st;
    FFStream *sti;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->id = entry->startcode;
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = entry->codec_id;
    st->codecpar->format = entry->sample_fmt;
    st->codecpar->sample_rate = entry->sample_rate;
    st->codecpar->bits_per_coded_sample = entry->bit_depth;
    st->codecpar->bits_per_raw_sample = entry->bit_depth;
    st->codecpar->ch_layout = entry->ch_layout;
    st->codecpar->ch_layout.nb_channels = entry->nb_channels;
    st->disposition = entry->disposition;

    if (entry->lang_iso)
        av_dict_set(&st->metadata, "language", entry->lang_iso, 0);

    sti = ffstream(st);
    sti->request_probe = 0;
    sti->need_parsing = AVSTREAM_PARSE_HEADERS;

    avpriv_set_pts_info(st, DVDVIDEO_PTS_WRAP_BITS,
                        DVDVIDEO_TIME_BASE_Q.num, DVDVIDEO_TIME_BASE_Q.den);

    return 0;
}

int ff_dvdvideo_audio_stream_add_all(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    int ret;
    int nb_streams;

    if (c->opt_menu)
        nb_streams = !c->opt_menu_vts ? c->vmg_ifo->vmgi_mat->nr_of_vmgm_audio_streams :
                                        c->vts_ifo->vtsi_mat->nr_of_vtsm_audio_streams;
    else
        nb_streams = c->vts_ifo->vtsi_mat->nr_of_vts_audio_streams;

    for (int i = 0; i < nb_streams; i++) {
        DVDVideoPGCAudioStreamEntry entry = {0};
        audio_attr_t audio_attr;

        if (c->opt_menu)
            audio_attr = !c->opt_menu_vts ? c->vmg_ifo->vmgi_mat->vmgm_audio_attr :
                                            c->vts_ifo->vtsi_mat->vtsm_audio_attr;
        else
            audio_attr = c->vts_ifo->vtsi_mat->vts_audio_attr[i];

        if (!(c->play_state.pgc->audio_control[i] & 0x8000))
            continue;

        if ((ret = dvdvideo_audio_stream_analyze(s, audio_attr, c->play_state.pgc->audio_control[i],
                                                 &entry)) < 0)
            goto break_error;

        /* IFO structures can declare duplicate entries for the same startcode */
        for (int j = 0; j < s->nb_streams; j++)
            if (s->streams[j]->id == entry.startcode)
                continue;

        if ((ret = dvdvideo_audio_stream_add(s, &entry)) < 0)
            goto break_error;

        continue;

break_error:
        av_log(s, AV_LOG_ERROR, "Unable to add audio stream at position %d\n", i);
        return ret;
    }

    return 0;
}

static int dvdvideo_subp_stream_analyze(AVFormatContext *s, uint32_t offset, subp_attr_t subp_attr,
                                        DVDVideoPGCSubtitleStreamEntry *entry)
{
    DVDVideoDemuxContext *c = s->priv_data;

    int ret;
    char lang_dvd[3] = {0};

    entry->startcode = 0x20 + (offset & 0x1F);

    if (subp_attr.lang_extension == 9)
        entry->disposition |= AV_DISPOSITION_FORCED;

    memcpy(&entry->clut, c->play_state.pgc->palette, FF_DVDCLUT_CLUT_SIZE);

    /* dvdsub palettes currently have no colorspace tagging and all muxers only support RGB */
    /* this is not a lossless conversion, but no use cases are supported for the original YUV */
    ret = ff_dvdclut_yuv_to_rgb(entry->clut, FF_DVDCLUT_CLUT_SIZE);
    if (ret < 0)
        return ret;

    AV_WB16(lang_dvd, subp_attr.lang_code);
    entry->lang_iso = ff_convert_lang_to(lang_dvd, AV_LANG_ISO639_2_BIBL);

    return 0;
}

static int dvdvideo_subp_stream_add(AVFormatContext *s, DVDVideoPGCSubtitleStreamEntry *entry)
{
    AVStream *st;
    FFStream *sti;
    int ret;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->id = entry->startcode;
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id = AV_CODEC_ID_DVD_SUBTITLE;

    if ((ret = ff_dvdclut_palette_extradata_cat(entry->clut, FF_DVDCLUT_CLUT_SIZE, st->codecpar)) < 0)
        return ret;

    if (entry->lang_iso)
        av_dict_set(&st->metadata, "language", entry->lang_iso, 0);

    av_dict_set(&st->metadata, "VIEWPORT", dvdvideo_subp_viewport_labels[entry->viewport], 0);

    st->disposition = entry->disposition;

    sti = ffstream(st);
    sti->request_probe = 0;
    sti->need_parsing = AVSTREAM_PARSE_HEADERS;

    avpriv_set_pts_info(st, DVDVIDEO_PTS_WRAP_BITS,
                        DVDVIDEO_TIME_BASE_Q.num, DVDVIDEO_TIME_BASE_Q.den);

    return 0;
}

static int dvdvideo_subp_stream_add_internal(AVFormatContext *s, uint32_t offset,
                                             subp_attr_t subp_attr,
                                             enum DVDVideoSubpictureViewport viewport)
{
    int ret;
    DVDVideoPGCSubtitleStreamEntry entry = {0};

    entry.viewport = viewport;

    if ((ret = dvdvideo_subp_stream_analyze(s, offset, subp_attr, &entry)) < 0)
        goto end_error;

    /* IFO structures can declare duplicate entries for the same startcode */
    for (int i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->id == entry.startcode)
            return 0;

    if ((ret = dvdvideo_subp_stream_add(s, &entry)) < 0)
        goto end_error;

    return 0;

end_error:
    av_log(s, AV_LOG_ERROR, "Unable to add subtitle stream\n");
    return ret;
}

int ff_dvdvideo_subp_stream_add_all(AVFormatContext *s)
{
    DVDVideoDemuxContext *c = s->priv_data;

    int nb_streams;

    if (c->opt_menu)
        nb_streams = !c->opt_menu_vts ? c->vmg_ifo->vmgi_mat->nr_of_vmgm_subp_streams :
                                        c->vts_ifo->vtsi_mat->nr_of_vtsm_subp_streams;
    else
        nb_streams = c->vts_ifo->vtsi_mat->nr_of_vts_subp_streams;


    for (int i = 0; i < nb_streams; i++) {
        int ret;
        uint32_t subp_control;
        subp_attr_t subp_attr;
        video_attr_t video_attr;

        subp_control = c->play_state.pgc->subp_control[i];
        if (!(subp_control & 0x80000000))
            continue;

        /* there can be several presentations for one SPU */
        /* the DAR check is flexible in order to support weird authoring */
        if (c->opt_menu) {
            video_attr = !c->opt_menu_vts ? c->vmg_ifo->vmgi_mat->vmgm_video_attr :
                                            c->vts_ifo->vtsi_mat->vtsm_video_attr;

            subp_attr  = !c->opt_menu_vts ? c->vmg_ifo->vmgi_mat->vmgm_subp_attr :
                                            c->vts_ifo->vtsi_mat->vtsm_subp_attr;
        } else {
            video_attr = c->vts_ifo->vtsi_mat->vts_video_attr;
            subp_attr = c->vts_ifo->vtsi_mat->vts_subp_attr[i];
        }

        /* 4:3 */
        if (!video_attr.display_aspect_ratio) {
            if ((ret = dvdvideo_subp_stream_add_internal(s, subp_control >> 24, subp_attr,
                                                         DVDVIDEO_SUBP_VIEWPORT_FULLSCREEN)) < 0)
                return ret;

            continue;
        }

        /* 16:9 */
        if ((    ret = dvdvideo_subp_stream_add_internal(s, subp_control >> 16, subp_attr,
                                                         DVDVIDEO_SUBP_VIEWPORT_WIDESCREEN)) < 0)
            return ret;

        /* 16:9 letterbox */
        if (video_attr.permitted_df == 2 || video_attr.permitted_df == 0)
            if ((ret = dvdvideo_subp_stream_add_internal(s, subp_control >> 8, subp_attr,
                                                         DVDVIDEO_SUBP_VIEWPORT_LETTERBOX)) < 0)
                return ret;

        /* 16:9 pan-and-scan */
        if (video_attr.permitted_df == 1 || video_attr.permitted_df == 0)
            if ((ret = dvdvideo_subp_stream_add_internal(s, subp_control, subp_attr,
                                                         DVDVIDEO_SUBP_VIEWPORT_PANSCAN)) < 0)
                return ret;
    }

    return 0;
}
