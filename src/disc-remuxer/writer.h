/*
 * disc-remuxer: opening a title and writing its stream files.
 */

#ifndef DISC_REMUXER_WRITER_H
#define DISC_REMUXER_WRITER_H

#include <libavformat/avformat.h>

/* The codec's short name in file names and logs (DD, DDplus, TrueHD, VC-1,
 * Mpeg2, VobSub, ...). */
const char *codec_label(enum AVCodecID id, int profile);

/* Opens title `title` of source with demuxer `format` and the settings
 * (read attempts, UDF reader, AACS key files). Returns 0 or an AVERROR. */
int open_title(const char *format, const char *source, int title, AVFormatContext **ctx);

typedef struct TitleJob {
    const char *format;        /* demuxer */
    const char *source;
    int         title;         /* index in the disc's title list */
    int         ordinal, count;/* this is title <ordinal> of <count> selected */
    const char *out_dir;
    int         sub_folder;    /* the title's files in a folder of its name */
} TitleJob;

typedef struct TitleOutcome {
    int     failed;            /* no usable output (or untested features met) */
    int64_t warnings;          /* warnings about its tracks */
    char    name[512];         /* the name its files take */
} TitleOutcome;

/* Writes one title's stream files and chapters (logged as a step of its
 * own). Returns 0, or an AVERROR when it failed (logged). */
int demux_title(const TitleJob *job, TitleOutcome *out);

#endif
