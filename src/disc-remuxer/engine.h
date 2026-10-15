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

#include <stddef.h>
#include <stdint.h>

#include "base/log.h"
#include "disc/info.h"
#include "views.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DISC_REMUXER_VERSION
#define DISC_REMUXER_VERSION "0.2.0"
#endif

/* The log (json: JSON records on stdout; verbose 0-2), the settings
 * (config: the settings file, NULL the one next to the program; sets:
 * "group.key=value" for this run), the folders next to the program
 * (.config: key caches, key files). 0, 1 (settings unreadable), 2 (a bad
 * --set). */
int  engine_init(const char *config, const char *const *sets, int nb_sets, int json, int verbose);

/* What the caller chose of the disc's tracks: --tracks values (select.h),
 * the 3-D titles whose second view is read (--second-view: "all", "1,3"). */
typedef struct EngineChoice {
    const char *const *tracks;
    int                nb_tracks;
    const char        *second;
} EngineChoice;

/* Checks the --tracks values; 0, or -1 (logged). */
int  engine_check_choice(const EngineChoice *c);

/* The info command: the disc and every title with its tracks; details:
 * then everything the GUI shows of them (engine_details()). 0, 1. */
int  engine_info(const char *source, const EngineChoice *c, int details);
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
/* The main output folder (engine_output_folder()'s first part). */
void engine_output_root(char *out, size_t size);
/* A name as a file or folder name (characters a file system refuses
 * replaced). */
void engine_clean_name(const char *name, char *out, size_t size);
/* base, or base_001, base_002, ... : the first that does not exist. */
void engine_unique_folder(const char *base, char *out, size_t size);

/* Stops a run (any thread; Ctrl+C on the command line): the title being
 * written ends at once (its files are not complete, said in the log), the
 * titles after it are not written. A run clears it when it starts. */
void engine_cancel(void);
int  engine_cancelled(void);

/* Track i of title t in words, as info lists it (its codec in full, its
 * details, its links to other tracks; "not selected" when !kept). */
const char *engine_track_text(const DiscTitleInfo *t, int i, int kept, char *buf, size_t size);
/* Track i of title t as info lists it, in two pieces: its codec in full
 * (codec), then its details, links and roles (rest, may be ""). */
void engine_track_parts(const DiscTitleInfo *t, int i, char *codec, size_t csize, char *rest, size_t rsize);
/* The same in three pieces: its codec in full, its details and roles
 * (body), and how it is linked to another track (link, "" none). */
void engine_track_pieces(const DiscTitleInfo *t, int i, char *codec, size_t csize, char *body, size_t bsize,
                         char *link, size_t lsize);
/* The extension of the stream file a track becomes (m2v, ac3, sub, wav, ...). */
const char *engine_track_extension(const DiscStreamInfo *k);
/* The name title t's files take (setting output.file_name_template): its
 * files are <name>_<track>_..., in a folder <name> when a run writes more
 * than one title. */
void engine_title_file_name(const DiscTitleInfo *t, char *out, size_t size);
/* The short codec name the file names use (Mpeg2, DD, DTS-HD MA, ...). */
const char *engine_codec_label(int codec, int profile);

/* The ISO 639-2 bibliographic code the disc info uses for a language code
 * (2 or 3 letters, either ISO 639-2 form), NULL when not known. */
const char *disc_language(const char *code);
/* The English name of a language code ("eng": "English"), NULL when not
 * known. */
const char *disc_language_name(const char *code);
/* The Matroska codec ID a stream is stored with in a Matroska file
 * ("V_MPEG2", "A_AC3", "S_HDMV/PGS", ...): LPCM as PCM ("A_PCM/INT/LIT"),
 * closed captions as text ("S_TEXT/UTF8"), a 3-D second view as
 * "V_MPEG4/ISO/MVC"; NULL when Matroska has none. */
const char *disc_mkv_codec_id(const DiscStreamInfo *k);

/* An open disc: read once, its titles ripped from it. */
typedef struct EngineDisc EngineDisc;

/* Opens source (errors logged): its disc info and each title's tracks as
 * c chose them. 0, or -1. */
int  engine_disc_open(const char *source, const EngineChoice *c, EngineDisc **d);
const DiscInfo *engine_disc_info(const EngineDisc *d);
/* The selection: what a rip of the open disc writes. Both front ends change
 * it only through these calls, so its rules are the engine's.
 * - A title ripped (to start with: every title but the angles other than
 *   the main one); engine_select_titles(): exactly the titles of spec
 *   ("all": every title but those angles; "3", "1,3-5"), their count, < 0
 *   (a title that does not exist: logged).
 * - A title written as parts, one per stream file it plays (to start with:
 *   none; setting output.split_joined_titles adds every title at the rip);
 *   engine_split_titles(): the titles of spec, < 0 as above.
 * - A track kept (1 ripped; to start with: as --tracks / the settings chose;
 *   a 3-D title's second view is a track of its own); track 0, the video,
 *   is always ripped: turning it off is refused (-1).
 * Setters: 0, < 0 no such title / track. */
int  engine_title_rip(const EngineDisc *d, int t);
int  engine_set_title_rip(EngineDisc *d, int t, int on);
int  engine_titles_ripped(const EngineDisc *d);
int  engine_select_titles(EngineDisc *d, const char *spec);
int  engine_title_split(const EngineDisc *d, int t);
int  engine_set_title_split(EngineDisc *d, int t, int on);
int  engine_split_titles(EngineDisc *d, const char *spec);
int  engine_track_kept(const EngineDisc *d, int t, int i);
int  engine_set_track_kept(EngineDisc *d, int t, int i, int on);
/* Title t's tracks kept, one per stream of its disc info (1 rip, 0 not). */
const uint8_t *engine_disc_tracks(const EngineDisc *d, int t);
/* The disc line and its titles in the log (detail: each title's files and
 * tracks too) with the JSON disc and title records. */
void engine_disc_overview(const EngineDisc *d, int detail);
/* Everything the GUI shows of the open disc (source: as the user named it),
 * in the log: its queue lines (the disc, each title with its state, its
 * chapters and tracks) and its details panel's rows (views.h). */
void engine_details(const EngineDisc *d, const char *source);
/* Rips the selection (the titles ripped, those as parts, their tracks) into
 * out_dir (NULL: engine_output_folder()), with the run's log files there.
 * 0, 1 (a title failed). */
int  engine_disc_rip(EngineDisc *d, const char *out_dir);
void engine_disc_close(EngineDisc **d);

#ifdef __cplusplus
}
#endif

#endif
