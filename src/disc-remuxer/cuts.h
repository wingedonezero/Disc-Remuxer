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
    uint8_t ref;             /* H.264: a reference picture */
    int     seg;             /* its place: the title's segment (from 0) */
    int64_t block;           /* and the block of it a reading can start at to reach it, -1 not known */
} CutPicture;

/* A place a part can start: a key frame and its group. */
typedef struct CutPoint {
    int64_t zero;            /* the part's 0: the earliest picture of the group that can be decoded */
    int64_t key_time;        /* the key frame's time */
    int     start;           /* enum DrDiscStart of the key frame */
    int     kept;            /* pictures after the key frame in decoding order shown before it, decodable */
    int     undecodable;     /* those that cannot be decoded without the group before, kept all the same */
    int     left_out;        /* those that cannot be decoded, left out of the part */
    int     seg;             /* the key frame's place (CutPicture) */
    int64_t block;
} CutPoint;

/* The places a part can start, from the pictures in decoding order: one
 * per key frame after the first picture. The pictures that follow a key
 * frame in decoding order and are shown before it (its leading pictures)
 * can be decoded from it when it starts cleanly (an IDR picture, a closed
 * GOP, a closed entry point); after any other key frame they need the group
 * before it, except HEVC's RADL pictures. One that cannot be decoded is left
 * out where the codec allows the stream without it (an HEVC RASL picture,
 * an H.264 picture that is no reference picture), else kept (timeline
 * ff_disctl_leading_kept()). The part's 0 is the earliest picture kept.
 * codec: DRCutCodec. 0, or a negative base error. */
int cut_points(const CutPicture *pics, int nb, int codec, CutPoint **points, int *nb_points);

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

/* A unit of an audio track that starts only at some units (TrueHD, a DTS-HD
 * stream: dr_disc_cut_start_units()). */
typedef struct CutUnit {
    int64_t time;            /* ticks, title timeline */
    uint8_t entry;           /* a stream can start at it (DrDiscFrame.entry) */
} CutUnit;

/* Where such a track's part starts at a cut. */
typedef struct CutStart {
    int64_t time;            /* the unit's time (ticks); INT64_MAX: the track has no unit after the cut */
    int     entry;           /* 1 an entry unit within the limit, 0 none: the first unit at or after the cut */
    int     carried;         /* units from the cut's 0 up to it (they stay in the part before) */
    int     to_entry;        /* entry 0: units from it to the next entry unit, -1 none in sight */
} CutStart;

/* The limit an entry unit may lie after a cut's 0: one TrueHD restart
 * interval (128 units of 1/1200 s). */
#define CUT_START_LIMIT (DR_TICKS_PER_SECOND * 128 / 1200)

/* Where a start-unit track's part starts at a cut whose 0 is zero: its
 * first entry unit at or after zero when it is within limit of it, else
 * its first unit at or after zero (none: INT64_MAX, the track ends before
 * the cut). u: the track's units in time order around the cut. 0. */
int cut_track_start(const CutUnit *u, int nb, int64_t zero, int64_t limit, CutStart *s);

/* A start-unit track's units as the cuts pass read them. */
typedef struct CutAudio {
    int      track;          /* its number in the title's tracks (disc info) */
    int      codec;          /* DRCodecID */
    CutUnit *u;
    int      nb, cap;
    int64_t  last;           /* the last unit's time read (ticks), INT64_MIN none */
} CutAudio;

/* A title's pictures read as a rip reads the title (its video only), the
 * title's chapters and where its video ends. */
typedef struct CutIndex {
    CutPicture *pics;
    int         nb_pics;
    int         codec;       /* DRCutCodec of the video (cut_points()) */
    int64_t    *chapters;    /* ticks; read only when the title was read to its end */
    char      **chapter_names; /* each as the rip names it in a Matroska file (the disc's, else "Chapter 05") */
    int         nb_chapters;
    int64_t     end;         /* ticks: the end of the last picture read */
    int         complete;    /* read to the title's end */
    CutAudio   *audio;       /* the start-unit tracks: their units inside the windows */
    int         nb_audio;
} CutIndex;

/* Reads title `title` of the open disc into ix: its video and its tracks
 * that start only at some units (their units only inside the windows: nb
 * pairs of from / to times, ticks); stop_after >= 0: the reading stops once
 * two key frames after that time and those tracks' units past it by the
 * start limit have been read (what the requests need); < 0: to the title's
 * end (the chapters need it). 0, or a negative base error (logged). */
int  cut_index_read(DrDisc *d, int title, int64_t stop_after, const int64_t *windows, int nb_windows, CutIndex *ix);
void cut_index_free(CutIndex *ix);

/* A time on the title timeline in words: 00:22:52.204.166.667 (h, min, s,
 * ms, us, ns). */
const char *cut_time_text(char *buf, int size, int64_t ticks);

#endif /* DISC_REMUXER_CUTS_H */
