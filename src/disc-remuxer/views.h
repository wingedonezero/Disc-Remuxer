/*
 * What the front ends show of a disc, in the engine's words: the details
 * (rows of a heading, a sub-heading or "label: value", each at a depth) of
 * the disc, a title, a track and a title's chapters, and the one-line texts
 * of the queue (a disc, a title and its state, a track, a chapter). The GUI
 * lays them out; the command line prints the same rows (info --details), so
 * what one shows the other can, word for word.
 */

#ifndef DISC_REMUXER_VIEWS_H
#define DISC_REMUXER_VIEWS_H

#include <stddef.h>
#include <stdint.h>

#include "disc/info.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum EngineRowKind {
    ENGINE_ROW_HEAD,        /* a block's heading */
    ENGINE_ROW_SUB,         /* a sub-heading (a track's) at its level */
    ENGINE_ROW_FIELD,       /* "label: value" at its level */
} EngineRowKind;

typedef struct EngineRow {
    EngineRowKind kind;
    int           level;
    char         *label;    /* the heading's text for HEAD / SUB */
    char         *value;    /* FIELD only, else NULL */
} EngineRow;

typedef struct EngineRows {
    EngineRow *rows;
    int        nb, alloc;
} EngineRows;

/* The details, appended to out (0, < 0 no memory); free with
 * engine_rows_free(). source: the disc's place as the user gave it. keep:
 * per track, 1 ripped. */
int  engine_disc_rows(const DiscInfo *d, const char *source, EngineRows *out);
int  engine_title_rows(const DiscTitleInfo *t, const uint8_t *keep, EngineRows *out);
int  engine_track_rows(const DiscTitleInfo *t, int i, const uint8_t *keep, EngineRows *out);
int  engine_chapters_rows(const DiscTitleInfo *t, EngineRows *out);
void engine_rows_free(EngineRows *r);

/* The queue's one-line texts. A disc: what it is ("Blu-ray  Name"), its
 * state ("3 titles – BD+, 2 fake playlists") and why (the notes in full,
 * one per line); returns 1 when its checks found something. */
int  engine_disc_line(const DiscInfo *d, char *desc, size_t dsize, char *state, size_t ssize, char *why,
                      size_t wsize);
/* A title: "Title #3" and "Name – 12 chapters, 1:42:10, 2 segments, ...". */
void engine_title_line(const DiscTitleInfo *t, char *name, size_t nsize, char *desc, size_t dsize);
/* A title's state in the queue (split: written as parts): "likely fake; not
 * the main angle", and why its mark (the fake playlist check's words, ""
 * none); returns its mark (DiscPlaylistCheck). */
int  engine_title_state(const DiscTitleInfo *t, int split, char *state, size_t ssize, char *why, size_t wsize);
/* A track: "Audio #2" and "DTS-HD Master Audio  Surround 5.1  ...  English". */
void engine_track_line(const DiscTitleInfo *t, int i, char *name, size_t nsize, char *desc, size_t dsize);
/* A chapter: "0:12:01.250  Name". */
void engine_chapter_line(const DiscTitleInfo *t, int c, char *desc, size_t dsize);

/* The queue in one line: "Queue: 2 discs, 5 titles checked (40.1 GB)", with
 * " — scanning 1 disc…" while discs are scanned. */
void engine_queue_line(int discs, int titles, int64_t bytes, int scanning, char *buf, size_t size);
/* A title being written: "Writing 45%", with its speed and the time left
 * once that can be told (a whole title of size bytes, secs into it):
 * "Writing 45% · 12.3 MB/s · 0:10:00 left". */
void engine_progress_line(double done, int64_t size, double secs, int whole, char *buf, size_t bsize);

/* Times and sizes as the front ends show them: "1:42:10" (to the second),
 * "0:12:01.250", "2.3 GB" (cut, not rounded). */
const char *engine_hms(int64_t us, char *buf, size_t size);
const char *engine_hms_ms(int64_t us, char *buf, size_t size);
const char *engine_size(int64_t bytes, char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
