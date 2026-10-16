/*
 * The engine's events: what it reports while it works, as data. The GUI and
 * the command line are two front ends of the one engine and get the same
 * events: the GUI the structs themselves (engine_set_events()), the command
 * line their rendering — text lines on the terminal, or with --json one JSON
 * record per event (the record's fields named below). JSON is a rendering
 * for tests and scripts, never the way the engine talks to a front end.
 */

#ifndef DISC_REMUXER_EVENTS_H
#define DISC_REMUXER_EVENTS_H

#include <stdint.h>

#include "disc/info.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DrEventKind {
    DR_EVENT_LINE,          /* one log line                         (JSON "message") */
    DR_EVENT_PROGRESS,      /* how far the open title is written    (JSON "progress") */
    DR_EVENT_DISC,          /* the open disc, as info lists it       (JSON "disc") */
    DR_EVENT_TITLE,         /* one of its titles with its tracks     (JSON "title") */
    DR_EVENT_TITLE_START,   /* a title's rip begins                  (JSON "title_start") */
    DR_EVENT_TRACK_RESULT,  /* one track of a title written          (JSON "track") */
    DR_EVENT_TITLE_RESULT,  /* a title's rip ended                   (JSON "title_result") */
    DR_EVENT_RUN_RESULT,    /* the run ended                         (JSON "result") */
} DrEventKind;

typedef enum DrStatus {
    DR_STATUS_OK,
    DR_STATUS_FAILED,
    DR_STATUS_STOPPED,
} DrStatus;

typedef struct DrEventLine {
    int         level;          /* enum LogLevel (base/log.h) */
    int         code;           /* msg.h number; 0 = none */
    const char *source;         /* the library a line comes from; NULL = ours */
    const char *text;
    int         heading;        /* a step heading */
} DrEventLine;

typedef struct DrEventProgress {
    int    title;
    double done;                /* 0..1 */
} DrEventProgress;

typedef struct DrEventDisc {
    const DiscInfo *disc;
} DrEventDisc;

typedef struct DrEventTitle {
    const DiscTitleInfo *title;
    const uint8_t       *keep;  /* per track: 1 ripped ("selected") */
} DrEventTitle;

typedef struct DrEventTitleStart {
    int         title;          /* the title's index on the disc */
    int         part;           /* -1: the whole title; else the part written (from 0) */
    const char *name;           /* the name its files take */
    int         ordinal, count; /* "title 2 of 5" of this run */
} DrEventTitleStart;

/* What a subtitle track's units were (DrEventTrackResult.sub). */
enum { DR_SUB_NONE, DR_SUB_CAPTIONS, DR_SUB_PICTURES };

typedef struct DrEventTrackResult {
    int         title;
    int         part;           /* -1: the whole title */
    int         index;          /* the track's index in the title's streams */
    const char *codec;          /* the short label the file names use */
    const char *language;       /* ISO 639-2, "" none */
    const char *layout;         /* audio: "5.1(side)", ... ; "" none */
    const char *file;           /* the written file's name, "" none written */
    int64_t     frames;
    int64_t     warnings;
    int64_t     delay_ms;
    /* video: line-21 caption data taken out of the video (has_line21) */
    int         has_line21;
    const char *line21;         /* where the disc marks them, "" none */
    int64_t     cc_blocks, cc_text;
    /* subtitles: DR_SUB_CAPTIONS (caption, cc_blocks, cues, cc_lost when
     * has_cc_lost) or DR_SUB_PICTURES (picture, width, height, no_stop,
     * outside) */
    int         sub;
    const char *caption;
    int64_t     cues;
    int         has_cc_lost;
    int64_t     cc_lost;
    const char *picture;
    int         width, height;
    int64_t     no_stop, outside;
    /* the end check: 0 not checked, 1 passed, -1 failed (details: what
     * differs) */
    int         details_check;
    const char *details;
} DrEventTrackResult;

typedef struct DrEventTitleResult {
    int         title;
    int         part;           /* -1: the whole title */
    DrStatus    status;
    int64_t     warnings;
    int64_t     damaged_ranges; /* the source's damaged ranges left out */
    int64_t     damaged_bytes;
    const char *folder;         /* where the title's files went */
} DrEventTitleResult;

typedef struct DrEventRunResult {
    int         written, failed;
    int         warnings, errors;
    int         stopped;
    const char *log;            /* the run's log file */
} DrEventRunResult;

typedef struct DrEvent {
    DrEventKind kind;
    union {
        DrEventLine        line;
        DrEventProgress    progress;
        DrEventDisc        disc;
        DrEventTitle       title;
        DrEventTitleStart  title_start;
        DrEventTrackResult track;
        DrEventTitleResult title_result;
        DrEventRunResult   run;
    } u;
} DrEvent;

/* A front end's sink: one callback, every event, on the thread that works
 * (a GUI hands them to its own thread). Strings, arrays and the disc model
 * pointed to are valid only during the call (copy what is kept). */
typedef struct DrSink {
    void (*event)(void *opaque, const DrEvent *ev);
    void  *opaque;
} DrSink;

/* Every event to sink (NULL: back to the terminal: text, or JSON records). */
void engine_set_events(const DrSink *sink);

#ifdef __cplusplus
}
#endif

#endif
