/*
 * The engine's events (events.h): to the front end's sink when one is
 * installed (the GUI), else rendered for the terminal — in JSON mode one
 * record per event, built here from the event's fields (the disc and its
 * titles by engine.c, which describes them for info).
 */

#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "base/log.h"
#include "emit.h"
#include "events.h"
#include "views.h"

static pthread_mutex_t sink_lock = PTHREAD_MUTEX_INITIALIZER;
static DrSink sink;
static char queue_name[1024];   /* engine_queue_details(): the disc's name, "" off */

void engine_queue_details(const char *name)
{
    snprintf(queue_name, sizeof(queue_name), "%s", name ? name : "");
}


/* The states the GUI's queue shows for an event, as "Queue:" lines. */
static void queue_lines(const DrEvent *ev)
{
    char tstate[128], dstate[128];

    switch (ev->kind) {
    case DR_EVENT_TITLE_START: {
        const DrEventTitleStart *e = &ev->u.title_start;
        engine_title_start_state(e->title, e->part, e->ordinal, e->count, tstate, sizeof(tstate), dstate,
                                 sizeof(dstate));
        log_text(LOG_INFO, "Queue: %s | %s", queue_name, dstate);
        log_text(LOG_INFO, "Queue:   Title #%d | %s", e->title, tstate);
        break;
    }
    case DR_EVENT_TITLE_RESULT: {
        const DrEventTitleResult *e = &ev->u.title_result;
        engine_title_result_state(e->part, e->status, e->warnings, tstate, sizeof(tstate));
        log_text(LOG_INFO, "Queue:   Title #%d | %s", e->title, tstate);
        break;
    }
    case DR_EVENT_RUN_RESULT:
        log_text(LOG_INFO, "Queue: %s | %s", queue_name, engine_disc_result_state(ev->u.run.failed > 0,
                                                                                    ev->u.run.stopped));
        break;
    default:
        break;
    }
}

/* The log's lines and progress as events, to the front end. */
static void line_event(void *opaque, int level, int code, const char *source, const char *text, int heading)
{
    DrEvent ev = { .kind = DR_EVENT_LINE };

    (void)opaque;
    ev.u.line = (DrEventLine){ level, code, source, text, heading };
    engine_event(&ev);
}

static void progress_event(void *opaque, int title, double done)
{
    DrEvent ev = { .kind = DR_EVENT_PROGRESS };

    (void)opaque;
    ev.u.progress = (DrEventProgress){ title, done };
    engine_event(&ev);
}

void engine_set_events(const DrSink *s)
{
    LogSink ls = { line_event, progress_event, NULL };

    pthread_mutex_lock(&sink_lock);
    if (s)
        sink = *s;
    else
        memset(&sink, 0, sizeof(sink));
    pthread_mutex_unlock(&sink_lock);
    log_set_sink(s ? &ls : NULL);
}

/* ",\"part\":<n>" (from 1) for a part, "" for the whole title */
static const char *part_json(char *buf, size_t size, int part)
{
    buf[0] = 0;
    if (part >= 0)
        snprintf(buf, size, ",\"part\":%d", part + 1);
    return buf;
}

static void title_start_json(const DrEventTitleStart *e)
{
    char jpart[32], js[4300];

    log_json("title_start", "\"title\":%d%s,\"name\":%s,\"ordinal\":%d,\"count\":%d", e->title,
             part_json(jpart, sizeof(jpart), e->part), json_str(js, sizeof(js), e->name), e->ordinal, e->count);
}

static void track_json(const DrEventTrackResult *e)
{
    char jpart[32], jc[64], jl[32], jy[96], jf[4300], extra[2048] = "";

    if (e->has_line21) {
        char jl21[64];
        snprintf(extra, sizeof(extra), ",\"line21\":%s,\"cc_blocks\":%"PRId64",\"cc_text\":%"PRId64,
                 json_str(jl21, sizeof(jl21), e->line21), e->cc_blocks, e->cc_text);
    } else if (e->sub == DR_SUB_CAPTIONS) {
        char jp[256], jlost[96] = "";
        if (e->has_cc_lost)
            snprintf(jlost, sizeof(jlost), ",\"cc_lost\":%"PRId64, e->cc_lost);
        snprintf(extra, sizeof(extra), ",\"caption\":%s,\"cc_blocks\":%"PRId64",\"cues\":%"PRId64"%s",
                 json_str(jp, sizeof(jp), e->caption), e->cc_blocks, e->cues, jlost);
    } else if (e->sub == DR_SUB_PICTURES) {
        char jp[256];
        snprintf(extra, sizeof(extra), ",\"picture\":%s,\"width\":%d,\"height\":%d,\"no_stop\":%"PRId64
                 ",\"outside\":%"PRId64, json_str(jp, sizeof(jp), e->picture), e->width, e->height, e->no_stop,
                 e->outside);
    }
    if (e->details_check) {
        char jp[1100];
        size_t l = strlen(extra);
        snprintf(extra + l, sizeof(extra) - l, ",\"details_check\":\"%s\",\"details\":%s",
                 e->details_check > 0 ? "pass" : "fail", json_str(jp, sizeof(jp), e->details));
    }
    log_json("track", "\"title\":%d%s,\"index\":%d,\"codec\":%s,\"language\":%s,\"layout\":%s,"
             "\"frames\":%"PRId64",\"warnings\":%"PRId64",\"delay_ms\":%"PRId64",\"file\":%s%s", e->title,
             part_json(jpart, sizeof(jpart), e->part), e->index, json_str(jc, sizeof(jc), e->codec),
             json_str(jl, sizeof(jl), e->language), json_str(jy, sizeof(jy), e->layout), e->frames, e->warnings,
             e->delay_ms, json_str(jf, sizeof(jf), e->file), extra);
}

static void title_result_json(const DrEventTitleResult *e)
{
    static const char *const status[] = { "ok", "failed", "stopped" };
    char jpart[32], js[4300];

    log_json("title_result", "\"title\":%d%s,\"status\":\"%s\",\"warnings\":%"PRId64",\"damaged_ranges\":%"PRId64
             ",\"damaged_bytes\":%"PRId64",\"folder\":%s", e->title, part_json(jpart, sizeof(jpart), e->part),
             status[e->status], e->warnings, e->damaged_ranges, e->damaged_bytes, json_str(js, sizeof(js), e->folder));
}

static void run_json(const DrEventRunResult *e)
{
    char jl[PATH_MAX + 700];

    log_json("result", "\"written\":%d,\"failed\":%d,\"warnings\":%d,\"errors\":%d,\"stopped\":%s,\"log\":%s",
             e->written, e->failed, e->warnings, e->errors, e->stopped ? "true" : "false",
             json_str(jl, sizeof(jl), e->log));
}

void engine_event(const DrEvent *ev)
{
    DrSink s;

    pthread_mutex_lock(&sink_lock);
    s = sink;
    pthread_mutex_unlock(&sink_lock);
    if (s.event) {
        s.event(s.opaque, ev);
        return;
    }
    /* the terminal: lines and progress are the log's own; the queue's states
     * with --details; the rest are records in JSON mode only */
    if (queue_name[0])
        queue_lines(ev);
    if (!log_json_mode())
        return;
    switch (ev->kind) {
    case DR_EVENT_DISC:         engine_disc_json(ev->u.disc.disc);                     break;
    case DR_EVENT_TITLE:        engine_title_json(ev->u.title.title, ev->u.title.keep); break;
    case DR_EVENT_TITLE_START:  title_start_json(&ev->u.title_start);                  break;
    case DR_EVENT_TRACK_RESULT: track_json(&ev->u.track);                              break;
    case DR_EVENT_TITLE_RESULT: title_result_json(&ev->u.title_result);                break;
    case DR_EVENT_RUN_RESULT:   run_json(&ev->u.run);                                  break;
    default:                                                                           break;
    }
}
