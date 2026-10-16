/*
 * disc-remuxer: a title into one Matroska file (output.format = mkv).
 */

#ifndef DISC_REMUXER_MKVOUT_H
#define DISC_REMUXER_MKVOUT_H

#include <stdint.h>

#include "disc/disc.h"

typedef struct MkvOut MkvOut;

/* Prepares path for the opened title (part >= 0: that part of it): the
 * tracks the writer takes go in, each other one is said in the log. 0 or a
 * negative base error. */
int mkvout_open(MkvOut **m, DrDisc *d, const char *path, int title, int part);

/* A frame of the title (its reference is taken). 0 or a negative base
 * error. */
int mkvout_frame(MkvOut *m, DrDisc *d, DrDiscFrame *fr);

/* Finishes the file (after the title's last frame: its chapters are known)
 * and frees m; abort: the file is only closed. bytes: the file's size. */
int mkvout_close(MkvOut **m, DrDisc *d, int abort, int64_t *bytes);

/* Whether title track `track` goes into the file. */
int mkvout_has_track(const MkvOut *m, int track);

#endif
