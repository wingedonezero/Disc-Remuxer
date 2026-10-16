/*
 * disc-remuxer: opening a title and writing its stream files.
 */

#ifndef DISC_REMUXER_WRITER_H
#define DISC_REMUXER_WRITER_H

#include "disc/disc.h"
#include "disc/info.h"

/* The codec's short name in file names and logs (DD, DDplus, TrueHD, VC-1,
 * Mpeg2, VobSub, ...). */
const char *codec_label(int codec, int profile);

/* The extension of the stream file a disc track becomes. */
const char *track_extension(const DiscStreamInfo *k);

/* Opens the disc source with the disc layer (disc/disc.h) and the settings
 * (read attempts, UDF reader, AACS key files, ...): the disc is read once
 * (its disc info: the title list with each title's streams), no title
 * opened. Returns 0 or a negative base error. */
int open_disc(const char *source, DrDisc **disc);

typedef struct TitleJob {
    DrDisc     *disc;          /* the open disc (open_disc()); the title is opened on it and closed again */
    int         title;         /* index in the disc's title list */
    int         part, nb_parts;/* part >= 0: only that part of the title's nb_parts (files _p1, _p2, ...) */
    int         second_view;   /* a 3-D title's second view as a track of its own (with keep: when kept) */
    const uint8_t *keep;       /* the title's tracks to rip (dr_disc_title_open()), NULL every track */
    int         ordinal, count;/* this is title <ordinal> of <count> selected */
    const char *out_dir;
    int         sub_folder;    /* the files in a folder of their name: the title's, a part's <title>_p<n> */
} TitleJob;

typedef struct TitleOutcome {
    int     failed;            /* no usable output (or untested features met) */
    int     stopped;           /* stopped on request (engine_cancel()): its files are not complete */
    int     disk_full;         /* failed because the disk is full: the titles after it would fail too */
    int64_t warnings;          /* warnings about its tracks */
    int64_t damaged_ranges;    /* damaged data left out: its ranges (each a warning) and bytes */
    int64_t damaged_bytes;
    char  **damage;            /* each range in words (nb_damage of them; title_outcome_free()) */
    int     nb_damage;
    char    name[512];         /* the name its files take */
} TitleOutcome;

/* Frees what an outcome holds (its damaged ranges). */
void title_outcome_free(TitleOutcome *out);

/* A size in GB / MB of 1024^3 / 1024^2 bytes, one decimal, cut off (not
 * rounded), into buf; returns buf. */
const char *size_text(char *buf, size_t size, int64_t bytes);

/* A WAV header (into out, at most WAV_HEADER_MAX bytes; returns its size):
 * WAVE_FORMAT_PCM for up to 2 channels of 16 bits, else
 * WAVE_FORMAT_EXTENSIBLE with the channel mask and the bits that carry the
 * sound (valid: 20-bit samples ride in 24-bit words); data: the sound's
 * bytes (0 while they are written: the header is written again at the
 * end). room: the header keeps a 36-byte chunk after "WAVE" for 64-bit
 * sizes (a file that may pass 4 GiB): JUNK while the file fits the 32-bit
 * sizes; when it does not, the file is RF64 (EBU Tech 3306): "RF64", the
 * ds64 chunk there with the RIFF size, the data size and the sample frames,
 * the 32-bit sizes 0xFFFFFFFF. Without room a file past 4 GiB keeps
 * 0xFFFFFFFF in the sizes it cannot hold. */
#define WAV_HEADER_MAX 120
#define WAV_DS64       28
/* The expected sound bytes from which a WAV header keeps room (3.5 GiB:
 * the title's length can be a little short of what is written). */
#define WAV_ROOM_FROM  0xE0000000LL
int writer_wav_header(uint8_t *out, int channels, int rate, int bits, int valid, uint64_t mask, int64_t data,
                      int room);

/* Writes one title's stream files and chapters (logged as a step of its
 * own; out: free with title_outcome_free()); part >= 0: only that part of the title (one of the stream files a
 * title plays back to back), -1 the whole title; second_view: a 3-D title's
 * second view is read too. Returns 0, or a negative base error when it
 * failed (logged). */
int demux_title(const TitleJob *job, TitleOutcome *out);

#endif
