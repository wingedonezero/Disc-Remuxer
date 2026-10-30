/*
 * disc-remuxer: cuts, the places a title is split at that are not clean
 * breaks of the disc (episodes inside one continuous stream). A cut is a
 * key frame of the title's video on the title timeline; the part it starts
 * begins with the first picture of that key frame's group that can be
 * decoded. This file: the title's pictures (read the way a rip reads the
 * title), the places a part can start, and the rule that turns what is
 * asked (a chapter, a time) into cuts.
 */

#ifndef DISC_REMUXER_CUTS_H
#define DISC_REMUXER_CUTS_H

#include <stdint.h>

#include "disc/disc.h"

/* A picture of the title's video, in decoding order. */
typedef struct CutPicture {
    int64_t time;            /* ticks (DR_TICKS_PER_SECOND), title timeline */
    int64_t duration;        /* ticks */
    uint8_t key;             /* a key frame */
    uint8_t start;           /* key frames: enum DrDiscStart */
    uint8_t rasl;            /* a leading picture that cannot be decoded from its key frame alone */
} CutPicture;

/* A place a part can start: a key frame and its group. */
typedef struct CutPoint {
    int64_t zero;            /* the part's 0: the earliest picture of the group that can be decoded */
    int64_t key_time;        /* the key frame's time */
    int     start;           /* enum DrDiscStart of the key frame */
    int     kept;            /* pictures after the key frame in decoding order shown before it, decodable */
    int     left_out;        /* those that cannot be decoded without the group before (left out of the part) */
} CutPoint;

/* The places a part can start, from the pictures in decoding order: one
 * per key frame after the first picture. The pictures that follow a key
 * frame in decoding order and are shown before it (its leading pictures)
 * can be decoded from it when it starts cleanly (an IDR picture, a closed
 * GOP, a closed entry point); after any other key frame they need the group
 * before it, except where the codec marks its leading pictures one by one
 * (marked: HEVC, whose RADL pictures decode and RASL pictures do not). 0, or
 * a negative base error. */
int cut_points(const CutPicture *pics, int nb, int marked, CutPoint **points, int *nb_points);

/* What is asked: a chapter (from 1, as the rip names it: 'Chapter 05') or
 * a time on the title timeline. */
typedef struct CutRequest {
    int     chapter;         /* > 0: that chapter; 0: time */
    int64_t time;            /* ticks */
    char    text[40];        /* as given */
} CutRequest;

/* Reads "c5,45:42.322,1:23:45.6" (comma separated; c<n> a chapter, else a
 * time h:mm:ss[.f] or m:ss[.f], minutes may pass 59 when no hours are
 * given, up to 9 decimals). *reqs freed with free(). 0, or -1 with *bad
 * pointing at the item that cannot be read (in spec). */
int cut_parse(const char *spec, CutRequest **reqs, int *nb, const char **bad);

/* Which key frame a time that is not on one takes. */
enum CutRule {
    CUT_NEAREST = 0,         /* the nearest one (the earlier one on a tie) */
    CUT_NEXT,                /* the first one after it (mkvmerge's way) */
};

/* A request's outcome. */
typedef struct CutResult {
    int64_t     target;      /* the time asked for (a chapter: its start), ticks */
    int         point;       /* the CutPoint used, -1: refused */
    int         on_point;    /* the target is the point's first picture or its key frame (within 1 ms) */
    const char *why;         /* refused: why */
    int         same_as;     /* refused as a second request for the same point: that request's index */
} CutResult;

/* Turns requests into cuts: a time within 1 ms of a point's first picture
 * or key frame is on that point; any other takes the point the rule picks.
 * chapters: the chapters' starts (ticks), the title's video ends at end.
 * Refused: a chapter that does not exist, a time not inside the title, a
 * place at the title's start (no part before it), no key frame after it
 * (CUT_NEXT), a second request for a point already taken. res: nb_reqs
 * outcomes. Returns the cuts made. */
int cut_resolve(const CutPoint *points, int nb_points, const int64_t *chapters, int nb_chapters, int64_t end,
                const CutRequest *reqs, int nb_reqs, int rule, CutResult *res);

/* A title's pictures read as a rip reads the title (its video only), the
 * title's chapters and where its video ends. */
typedef struct CutIndex {
    CutPicture *pics;
    int         nb_pics;
    int         marked;      /* the codec marks its leading pictures (cut_points()) */
    int64_t    *chapters;    /* ticks; read only when the title was read to its end */
    int         nb_chapters;
    int64_t     end;         /* ticks: the end of the last picture read */
    int         complete;    /* read to the title's end */
} CutIndex;

/* Reads title `title` of the open disc (its video track only) into ix;
 * stop_after >= 0: the reading stops once two key frames after that time
 * have been read (what the requests need); < 0: to the title's end (the
 * chapters need it). 0, or a negative base error (logged). */
int  cut_index_read(DrDisc *d, int title, int64_t stop_after, CutIndex *ix);
void cut_index_free(CutIndex *ix);

/* A time on the title timeline in words: 00:22:52.204.166.667 (h, min, s,
 * ms, us, ns). */
const char *cut_time_text(char *buf, int size, int64_t ticks);

#endif /* DISC_REMUXER_CUTS_H */
