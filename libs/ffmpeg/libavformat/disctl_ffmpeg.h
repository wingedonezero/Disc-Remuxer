/*
 * Registers FFmpeg as the title timeline's host (disctl.h DRTimelineHost):
 * the stock frame parser, the LATM stream header and the TrueHD / MLP
 * end-of-stream decode. The program and the test bridge call it once at
 * start-up; src/analyze replaces it at step 6 of the rewrite.
 *
 * This file is part of FFmpeg (see disctl_ffmpeg.c).
 */

#ifndef AVFORMAT_DISCTL_FFMPEG_H
#define AVFORMAT_DISCTL_FFMPEG_H

void ff_disctl_use_ffmpeg_host(void);

#endif
