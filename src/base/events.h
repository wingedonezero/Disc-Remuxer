/*
 * Typed events: what the engine reports while it works, as data. This is
 * the contract of engine API v2 (docs/REWRITE.md §3): the GUI consumes the
 * structs; the command line renders them as text or JSON — JSON is a
 * rendering, never the transport.
 *
 * Until the engine moves onto this (plan step 9), the EngineSink in log.h
 * carries lines, progress and JSON records as before; the fields here
 * mirror those records one to one so nothing is lost in the move.
 */

#ifndef DISC_BASE_EVENTS_H
#define DISC_BASE_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DrEventKind {
    DR_EVENT_LINE,          /* one log line */
    DR_EVENT_PROGRESS,      /* how far the open title is read */
    DR_EVENT_TITLE_START,   /* a title's rip begins */
    DR_EVENT_TRACK_RESULT,  /* one written track of a title */
    DR_EVENT_TITLE_RESULT,  /* a title's rip ended */
    DR_EVENT_RUN_RESULT,    /* the run ended */
} DrEventKind;

typedef enum DrStatus {
    DR_STATUS_OK,
    DR_STATUS_FAILED,
    DR_STATUS_STOPPED,
} DrStatus;

/* One damaged byte range of the source a title read through. */
typedef struct DrDamageRange {
    uint64_t start, bytes;
} DrDamageRange;

typedef struct DrEventLine {
    int         level;          /* enum LogLevel (log.h) */
    int         code;           /* msg.h number; 0 = none */
    const char *source;         /* the library a line comes from; NULL = ours */
    const char *text;
    int         heading;        /* a step heading */
} DrEventLine;

typedef struct DrEventProgress {
    int    title;
    double done;                /* 0..1 */
} DrEventProgress;

typedef struct DrEventTitleStart {
    int         title;          /* the title's number on the disc */
    int         part;           /* 0: the whole title; else the part written */
    const char *name;
    int         ordinal, count; /* "title 2 of 5" of this run */
} DrEventTitleStart;

typedef struct DrEventTrackResult {
    int         title;
    int         index;          /* the track's index in the title's streams */
    const char *codec;          /* the short label the file names use */
    const char *language;       /* ISO 639-2, "" none */
    const char *layout;         /* audio: "5.1(side)", ... ; "" none */
    const char *file;           /* the written file's name */
    int64_t     frames;
    int         warnings;
    int64_t     delay_ms;
    DrStatus    details_check;  /* the end check of this track */
} DrEventTrackResult;

typedef struct DrEventTitleResult {
    int                  title;
    DrStatus             status;
    int                  warnings;
    const DrDamageRange *damage;    /* the source's damaged ranges, counted below */
    int                  nb_damage;
    uint64_t             damaged_bytes;
    const char          *folder;    /* where the title's files went */
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
        DrEventTitleStart  title_start;
        DrEventTrackResult track;
        DrEventTitleResult title_result;
        DrEventRunResult   run;
    } u;
} DrEvent;

/* The sink of engine API v2: one callback, every event. Strings and arrays
 * are valid only during the call (a GUI copies what it keeps). */
typedef struct DrSink {
    void (*event)(void *opaque, const DrEvent *ev);
    void  *opaque;
} DrSink;

#ifdef __cplusplus
}
#endif

#endif
