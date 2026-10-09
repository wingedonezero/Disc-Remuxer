/*
 * disc-remuxer's engine: what the command line (main.c) and the GUI call.
 * Settings, the log, opening a disc (read once), the choice of titles and
 * tracks, ripping titles into stream files. The command line's commands
 * are engine_info() and engine_demux(); a GUI opens a disc itself
 * (engine_disc_open()), changes the choice and rips from the open disc
 * (engine_disc_rip()).
 */

#ifndef DISC_REMUXER_ENGINE_H
#define DISC_REMUXER_ENGINE_H

#include <stdint.h>

#include <libavformat/discinfo.h>

#ifndef DISC_REMUXER_VERSION
#define DISC_REMUXER_VERSION "0.2.0"
#endif

/* Where the log goes besides the run's log files: NULL members print to the
 * terminal (text, or JSON records with json). level: LOG_* (log.h); code:
 * the message number (msg.h), 0 none; source: the library a line comes
 * from, NULL ours; heading: a step's heading. record: one JSON record. */
typedef struct EngineSink {
    void (*line)(void *opaque, int level, int code, const char *source, const char *text, int heading);
    void (*progress)(void *opaque, int title, double done);
    void (*record)(void *opaque, const char *json);
    void  *opaque;
} EngineSink;

/* The log (json: JSON records on stdout; verbose 0-2), the settings
 * (config: the settings file, NULL the one next to the program; sets:
 * "group.key=value" for this run), the folders next to the program
 * (.config: key caches, key files). 0, 1 (settings unreadable), 2 (a bad
 * --set). */
int  engine_init(const char *config, const char *const *sets, int nb_sets, int json, int verbose);
/* Log lines, progress and records to sink (NULL: the terminal again). */
void engine_set_sink(const EngineSink *sink);

/* What the caller chose of the disc's tracks: --tracks values (select.h),
 * the 3-D titles whose second view is read (--second-view: "all", "1,3"). */
typedef struct EngineChoice {
    const char *const *tracks;
    int                nb_tracks;
    const char        *second;
} EngineChoice;

/* Checks the --tracks values; 0, or -1 (logged). */
int  engine_check_choice(const EngineChoice *c);

/* The info command: the disc and every title with its tracks. 0, 1. */
int  engine_info(const char *source, const EngineChoice *c);
/* The demux command: titles ("all": the titles not shorter than the
 * minimum length; "3", "1,3-5") into out_dir, NULL: a folder named after
 * the disc in the main output folder (engine_output_folder()); split: the
 * titles written as parts, or NULL. 0, 1 (a title failed), 2 (bad
 * arguments). */
int  engine_demux(const char *source, const char *titles, const char *out_dir, const char *split,
                  const EngineChoice *c);

/* The folder a disc's output goes to by default: <main output folder (setting
 * output.folder; empty: "output" next to the program)>/<the disc's name>,
 * with _001, _002, ... when it exists already. */
void engine_output_folder(const char *source, char *out, size_t size);

/* An open disc: read once, its titles ripped from it. */
typedef struct EngineDisc EngineDisc;

/* Opens source (errors logged): its disc info and each title's tracks as
 * c chose them. 0, or -1. */
int  engine_disc_open(const char *source, const EngineChoice *c, EngineDisc **d);
const DiscInfo *engine_disc_info(const EngineDisc *d);
/* Title t's tracks to rip, one per stream of its disc info (1 rip, 0 not;
 * the caller may change them; track 0 is always ripped). */
uint8_t *engine_disc_tracks(EngineDisc *d, int t);
/* The disc line and its titles in the log (detail: each title's files and
 * tracks too) with the JSON disc and title records. */
void engine_disc_overview(const EngineDisc *d, int detail);
/* Rips the titles with sel[t] set (split[t]: as parts; split may be NULL)
 * into out_dir (NULL: engine_output_folder()), with the run's log files
 * there. 0, 1 (a title failed). */
int  engine_disc_rip(EngineDisc *d, const char *sel, const char *split, const char *out_dir);
void engine_disc_close(EngineDisc **d);

#endif
