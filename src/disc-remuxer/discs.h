/*
 * Finding discs in what the user names (an image, a disc folder, or a folder
 * holding any number of them) and the folder a disc's output goes to: one
 * rule for the GUI's queue and the command line (find, demux).
 */

#ifndef DISC_REMUXER_DISCS_H
#define DISC_REMUXER_DISCS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EngineFound {
    char *path;         /* what the engine opens: the image, or the disc's folder (absolute) */
    char *name;         /* the disc's name: the image's without its extension, or the folder's (a VIDEO_TS /
                           BDMV folder: its parent's) */
    char *relative;     /* where it lies below the named folder ("." : the named path itself) */
    char *root;         /* the named folder (an image named itself: its folder) */
} EngineFound;

typedef struct EngineFoundList {
    EngineFound *discs;
    int          nb, alloc;
} EngineFoundList;

/* The discs at or below path (searched max_depth folders deep, 5 in the
 * GUI), in name order (case ignored; hidden entries skipped): an image is a
 * .iso / .img file; a disc folder holds VIDEO_TS, HVDVD_TS / HDDVD_TS or
 * BDMV, or is a VIDEO_TS / BDMV folder itself (VIDEO_TS.IFO or index.bdmv
 * in it). 0, < 0 no memory; free with engine_found_free(). */
int  engine_find_discs(const char *path, int max_depth, EngineFoundList *out);
void engine_found_free(EngineFoundList *l);
/* path is an image (.iso / .img file). */
int  engine_is_disc_image(const char *path);

/* The folder a found disc's output goes to, before _001, _002, ...
 * (engine_unique_folder()): <root>/<the named folder>/<folders down to the
 * disc>/<disc name> with setting output.keep_folder_structure, else
 * <root>/<disc name>; root: the main output folder (NULL: setting
 * output.folder, engine_output_root()). */
void engine_found_folder(const EngineFound *f, const char *root, char *out, size_t size);

/* The name a disc's output and log files take, from what was opened: the
 * finder's name when source is a disc it recognises, else the source's
 * last part (".iso" left out); cleaned for a file name. */
void engine_disc_name(const char *source, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
