/*
 * disc-remuxer: the Matroska writer.
 *
 * One title into one Matroska file, written the way mkvmerge (MKVToolNix)
 * writes one: the same file layout, track header fields, clusters, blocks,
 * lacing, cues, chapters and statistics tags, on libebml + libmatroska. The
 * times are the title's own (the disc's), rounded to the file's timestamp
 * scale (1 ms) as mkvmerge rounds.
 *
 * The caller gives each track's description, then the title's frames in the
 * order they come (each track's frames in its own order: video in decode
 * order with display times); the writer packs them per codec (H.264: NAL
 * units with 4-byte sizes, its configuration record; PCM: 40 ms blocks; PGS:
 * a display set per block) and writes them in time order across the tracks. Written by C++ code behind
 * this C interface (libmatroska is C++).
 */

#ifndef DISC_MKV_MKV_H
#define DISC_MKV_MKV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MkvWriter MkvWriter;

enum MkvCodec {
    MKV_CODEC_H264 = 1,     /* frames: access units (Annex B), display times */
    MKV_CODEC_PCM,          /* frames: little-endian samples, interleaved */
    MKV_CODEC_AC3,          /* frames: AC-3 / E-AC-3 sync frames (A_AC3, or A_EAC3 from the stream) */
    MKV_CODEC_DTS,          /* frames: DTS frames (a core, a core + extension substream, or an extension
                               substream alone) */
    MKV_CODEC_PGS,          /* frames: PGS display sets (their segments one after another), each at its
                               presentation time */
};

typedef struct MkvTrackSpec {
    int         codec;              /* enum MkvCodec */
    const char *language;           /* ISO 639-2 (bibliographic), NULL or "": und */
    const char *language_ietf;      /* BCP 47, NULL or "": from language */
    const char *name;               /* the track's name, NULL none */
    int         forced;             /* subtitles: forced only (FlagForced) */
    int         zlib;               /* subtitles: each block compressed with zlib (a ContentEncoding) */
    int         remove_dialnorm;    /* AC-3 / E-AC-3 / DTS: each frame's dialogue normalization set to "none"
                                       (as mkvmerge's --remove-dialog-normalization-gain) */
    /* video */
    int         width, height;      /* the picture (the stream's, cropped) */
    /* audio */
    int         sample_rate, channels, bits;
} MkvTrackSpec;

typedef struct MkvChapter {
    int64_t     start;              /* ns from the title's start */
    const char *name;               /* NULL: "Chapter NN" */
} MkvChapter;

typedef struct MkvFileSpec {
    const char *muxing_app, *writing_app;
    int64_t     date;               /* seconds since 1970-01-01 UTC */
    int         fixed_ids;          /* track / chapter UIDs 1, 2, ... and a zero segment UID (as mkvmerge's
                                       no_variable_data), for byte comparisons */
    /* the chapters as the disc's title list has them (their room is kept
     * near the file's start, as mkvmerge keeps it; the chapters written are
     * those given to mkv_close()) */
    const MkvChapter *chapters;
    int         nb_chapters;
} MkvFileSpec;

/* Creates the file (written once every track's first frame is known);
 * 0 or a negative errno. */
int mkv_open(MkvWriter **w, const char *path, const MkvFileSpec *file, const MkvTrackSpec *tracks, int nb_tracks);

/* One frame of track `track`: time and duration in ns (duration -1: not
 * known), key: a frame decoding can start at. 0 or a negative errno. */
int mkv_frame(MkvWriter *w, int track, const uint8_t *data, int size, int64_t time, int64_t duration, int key);

/* What a finished file holds, for the log. */
typedef struct MkvStats {
    int64_t clusters, cues, blocks, bytes;
    int64_t late;                   /* subtitle display sets that came after a later packet was written */
    int     nb_removed;             /* tracks that got no frames: taken out of the track list at the end */
    int     removed[128];           /* the first of them (their index in the tracks given to mkv_open) */
} MkvStats;

/* Finishes the file (the frames still held, the cues, the chapters, the
 * tags, the seek head and the sizes; a track that got no frame is taken out
 * of the track list, which is written again in its place, the others keeping
 * their numbers) and frees the writer; 0 or a negative
 * errno. With abort set the file is only closed. st: what it holds (NULL:
 * not wanted). */
int mkv_close(MkvWriter **w, const MkvChapter *chapters, int nb_chapters, int abort, MkvStats *st);

/* The pictures of an H.264 access unit (Annex B), as the writer splits it
 * into blocks: 2 for a frame of two field pictures, else 1 (0: no slice). */
int mkv_h264_pictures(const uint8_t *data, int size);

/* The packets mkvmerge makes of audio data (MKV_CODEC_AC3: an independent
 * frame with its dependent ones; MKV_CODEC_DTS: a frame): their count. */
int mkv_audio_packets(int codec, const uint8_t *data, int size);

#ifdef __cplusplus
}
#endif

#endif /* DISC_MKV_MKV_H */
