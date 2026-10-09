/*
 * disc-remuxer: settings. Settings decide how output is made and stay until
 * changed (the settings file next to the program, disc-remuxer.toml);
 * commands decide what one run does. The file is written by the program:
 * every setting with its help, new settings added at their default, values
 * of settings it no longer has removed (both logged); a value given there
 * is kept. --set group.key=value changes a setting for one run only.
 */

#ifndef DISC_REMUXER_SETTINGS_H
#define DISC_REMUXER_SETTINGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum SettingKind { SET_BOOL, SET_INT, SET_CHOICE, SET_TEXT, SET_LIST };

/* A setting as the settings menu shows it. */
typedef struct Setting {
    const char        *group, *key;
    enum SettingKind   kind;
    const char        *def;          /* default, as text (a list: items separated by commas) */
    int64_t            min, max;     /* SET_INT */
    const char *const *choices;      /* SET_CHOICE, NULL-terminated */
    const char        *help;
    int                kept;         /* a value the GUI keeps for itself (window sizes): not in the settings
                                        menu, not in the run's log */
} Setting;

/* Reads the settings file (path NULL: disc-remuxer.toml next to the
 * program) and brings it up to date. Returns 0 or -1 (logged). */
int settings_load(const char *path);

/* "group.key=value" for this run only. Returns 0 or -1 (logged). */
int settings_set(const char *assignment);

int64_t     setting_int(const char *name);
int         setting_bool(const char *name);
const char *setting_text(const char *name);
/* A list setting's items (count returned; *items valid until the next set). */
int         setting_list(const char *name, const char *const **items);

const char *settings_path(void);
/* The folder of the program (where the settings file and .config live). */
const char *program_dir(void);

/* Every setting, in the file's order: the count, setting i. */
int            settings_count(void);
const Setting *settings_get(int i);
/* Setting i's value in effect, as text (a list: items joined by commas). */
const char    *settings_value(int i);
/* Keeps a value in the settings file ("group.key", value as text): checked,
 * in effect at once, written with settings_save(). 0, or -1 with why in
 * err. */
int  settings_store(const char *name, const char *value, char *err, int size);
/* Writes the settings file with the values stored in this run; every other
 * value as the file holds it then (a change made by hand meanwhile is kept).
 * The old file kept as .bak. 0, or -1 (logged). */
int  settings_save(void);

/* Logs the settings that differ from their default (one line, or none). */
void settings_log_changed(void);
/* Prints every setting, its value and where it came from. */
void settings_show(void);

#ifdef __cplusplus
}
#endif

#endif
