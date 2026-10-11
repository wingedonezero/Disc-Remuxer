/*
 * disc-remuxer: opening a title and writing its stream files.
 */

#ifndef DISC_REMUXER_WRITER_H
#define DISC_REMUXER_WRITER_H

#include <libavformat/avformat.h>
#include <libavformat/discinfo.h>

/* The codec's short name in file names and logs (DD, DDplus, TrueHD, VC-1,
 * Mpeg2, VobSub, ...). */
const char *codec_label(enum AVCodecID id, int profile);

/* The extension of the stream file a disc track becomes. */
const char *track_extension(const DiscStreamInfo *k);

/* Opens the disc source with demuxer `format` and the settings (read
 * attempts, UDF reader, AACS key files, ...): the disc is read once (its
 * disc info: the title list with each title's streams), no title opened.
 * Returns 0 or an AVERROR. */
int open_disc(const char *format, const char *source, AVFormatContext **ctx);

typedef struct TitleJob {
    AVFormatContext *disc;     /* the open disc (open_disc()); the title is opened on it and closed again */
    int         title;         /* index in the disc's title list */
    int         part, nb_parts;/* part >= 0: only that part of the title's nb_parts (files _p1, _p2, ...) */
    int         second_view;   /* a 3-D title's second view as a track of its own (with keep: when kept) */
    const uint8_t *keep;       /* the title's tracks to rip (disc_title_open()), NULL every track */
    int         ordinal, count;/* this is title <ordinal> of <count> selected */
    const char *out_dir;
    int         sub_folder;    /* the files in a folder of their name: the title's, a part's <title>_p<n> */
} TitleJob;

typedef struct TitleOutcome {
    int     failed;            /* no usable output (or untested features met) */
    int     stopped;           /* stopped on request (engine_cancel()): its files are not complete */
    int64_t warnings;          /* warnings about its tracks */
    int64_t damaged_ranges;    /* damaged data left out: its ranges (each a warning) and bytes */
    int64_t damaged_bytes;
    char    damage[4096];      /* the ranges, one per line (cut at the end when longer) */
    char    name[512];         /* the name its files take */
} TitleOutcome;

/* A size in GB / MB of 1024^3 / 1024^2 bytes, one decimal, cut off (not
 * rounded), into buf; returns buf. */
const char *size_text(char *buf, size_t size, int64_t bytes);

/* Writes one title's stream files and chapters (logged as a step of its
 * own); part >= 0: only that part of the title (one of the stream files a
 * title plays back to back), -1 the whole title; second_view: a 3-D title's
 * second view is read too. Returns 0, or an AVERROR when it failed
 * (logged). */
int demux_title(const TitleJob *job, TitleOutcome *out);

#endif
