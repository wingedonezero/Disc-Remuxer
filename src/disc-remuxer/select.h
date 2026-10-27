/*
 * disc-remuxer: which tracks of a title are ripped (--tracks).
 *
 * Every track is selected to start with (a 3-D title's second view only
 * when it is asked for); steps taken left to right then take tracks out or
 * put them back:
 *
 *   --tracks "-subtitle,+eng"   every title: no subtitles but the English ones
 *   --tracks "3:-2,-5"          title 3: not tracks 2 and 5
 *
 * A step is + (select) or - (leave out) and a word: all, video, audio,
 * subtitle, forced, a language code (2 or 3 letters) or a track number
 * (as info lists them, from 0). Track 0, the video, is always ripped; a
 * 3-D second view is put back only by its number (or --second-view). A
 * track number takes the tracks taken out of that track along (its forced
 * subtitles, its core).
 */

#ifndef DISC_REMUXER_SELECT_H
#define DISC_REMUXER_SELECT_H

#include <stdint.h>

#include "disc/info.h"

/* Checks one --tracks value; 0, or -1 (logged). */
int tracks_check(const char *spec);

/* The tracks of title t (its index in the title list) to rip into keep
 * (one per stream of t): every track, the second view only with
 * second_view, then every spec (--tracks values, in order) for every title
 * or for this one. Returns the count selected. */
int tracks_select(const DiscTitleInfo *t, int second_view, const char *const *specs, int nb_specs, uint8_t *keep);

/* Track i of title t kept (on) or not in keep, with the tracks taken out of
 * it (forced subtitles, a core; not a 3-D second view). */
void tracks_set(const DiscTitleInfo *t, uint8_t *keep, int i, int on);

/* The steps of one --tracks value (without a title) applied to title t's
 * keep, as the GUI's menu does for one title; 0, or -1 (logged, keep as it
 * was). */
int tracks_apply(const DiscTitleInfo *t, const char *steps, uint8_t *keep);

/* A --tracks value names a title the disc does not have: the first such
 * title, or -1 none. */
int tracks_unknown_title(const char *const *specs, int nb_specs, int nb_titles);

#endif
