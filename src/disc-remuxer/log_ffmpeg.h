/*
 * FFmpeg's log, routed into ours: lines without FFmpeg's "[name @ 0x...]"
 * prefix, the source named from the AVClass, levels mapped. The plan
 * (docs/REWRITE.md) moves this hook into the analyze module when the engine
 * stops demuxing through FFmpeg.
 */

#ifndef DISC_REMUXER_LOG_FFMPEG_H
#define DISC_REMUXER_LOG_FFMPEG_H

/* Installs FFmpeg's log callback (log_init() first). */
void log_ffmpeg_install(void);

/* While on, FFmpeg's own lines below error level are detail lines: what it
 * says while the written files are read back (the end check) is about
 * reading those files, not about the disc. */
void log_ffmpeg_detail(int on);

#endif
