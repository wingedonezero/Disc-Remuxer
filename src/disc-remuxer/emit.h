/*
 * The engine's own side of its events (events.h): how its parts hand an
 * event on, and the info records engine.c renders for the terminal.
 */

#ifndef DISC_REMUXER_EMIT_H
#define DISC_REMUXER_EMIT_H

#include <stdint.h>

#include "disc/info.h"
#include "events.h"

/* An event to the front end's sink, else rendered for the terminal (in
 * JSON mode a record; lines and progress are the log's own). */
void engine_event(const DrEvent *ev);
/* demux --details: the queue's states of a rip as "Queue:" lines at the
 * moments the GUI shows them, the disc's under name (NULL: none). */
void engine_queue_details(const char *name);

/* The disc and one title (keep: its tracks ripped) as JSON records. */
void engine_disc_json(const DiscInfo *d);
void engine_title_json(const DiscTitleInfo *t, const uint8_t *keep);

#endif
