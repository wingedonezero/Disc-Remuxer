/*
 * disc-remuxer: settings (see settings.h).
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tomlc17.h"

#include "log.h"
#include "msg.h"
#include "settings.h"


static const char *const udf_readers[]  = { "netbsd", "linux", NULL };
static const char *const dv_layers[]    = { "merged", "separate", NULL };
static const char *const cell_modes[]   = { "auto", "walk", "trim", "walk_trim", NULL };
static const char *const cell_checks[]  = { "all", "spread", "random", NULL };

static const Setting registry[] = {
    { "read", "attempts", SET_INT, "5", 1, 99, NULL,
      "How many times a read from a disc is tried before it counts as failed; after the first retry "
      "the rest of the read goes one sector at a time.", 0, SET_AT_OPEN },
    { "read", "udf_reader", SET_CHOICE, "netbsd", 0, 0, udf_readers,
      "Which UDF reader opens disc images: netbsd (based on NetBSD's UDF code) or linux (based on Linux's).",
      0, SET_AT_OPEN },
    { "read", "prefer_iso_for_old_udf102", SET_BOOL, "true", 0, 0, NULL,
      "Read a DVD image whose UDF 1.02 file system was recorded before 2006 through its ISO 9660 file "
      "system, when that holds a valid DVD-Video structure.", 0, SET_AT_OPEN },
    { "aacs", "key_files", SET_LIST, "", 0, 0, NULL,
      "AACS key files (KEYDB.cfg format), read in this order; the first one holding a key for the disc "
      "is used. Empty: every .cfg file in the folder keydb next to the program (made on the first start), "
      "in name order.", 0, SET_AT_OPEN },
    { "titles", "min_length", SET_INT, "0", 0, 86400, NULL,
      "Titles shorter than this (seconds) are not ripped with demux all: a DVD or HD DVD lists them as not "
      "selected (a title number still rips them), a Blu-ray leaves them out of its title list. 0: every title.",
      0, SET_AT_OPEN },
    { "output", "folder", SET_TEXT, "", 0, 0, NULL,
      "The main output folder: a disc's files go into a folder of the disc's name in it (its image's or "
      "folder's name; _001, _002, ... when that exists), when demux is given no output folder and in the GUI. "
      "Empty: the folder output next to the program.", 0, SET_AT_RIP },
    { "output", "file_name_template", SET_TEXT, "", 0, 0, NULL,
      "How a title's output files are named. {NAME1} the title's name, {CMNT1} its comment, {DT} its "
      "date, {N2} its number with two digits; {prefix:VAR} writes the prefix only after other text. "
      "Empty: {NAME1}{-:CMNT1}{-:DT}{title:+DFLT}{_t:N2}.", 0, SET_AT_RIP },
    { "output", "video_language", SET_TEXT, "eng", 0, 0, NULL,
      "The language given to a title's video, which the disc formats never name (an ISO 639 code, e.g. eng, "
      "fra, jpn): shown in the title list and written into the video file's name. Empty: none (und).",
      0, SET_AT_OPEN },
    { "output", "split_joined_titles", SET_BOOL, "false", 0, 0, NULL,
      "A title that plays whole stream files back to back (the episodes of a series one after another in one "
      "Blu-ray title) is written as parts, one per file: <name>_p1, _p2, ..., each as a title of its own (in a folder "
      "of its name, as every title), from its own start with its own chapters and its audio as the "
      "file holds it. Off: such a title is one timeline (the audio's overlaps "
      "at the joins are evened out by dropping whole frames). The demux option --split <titles> turns it on "
      "for chosen titles of one run.", 0, SET_AT_RIP },
    { "bluray", "second_view", SET_BOOL, "false", 0, 0, NULL,
      "3-D titles: also write the second view (the MVC dependent view, a .mvc file next to the base view's "
      ".h264; each file names its eye, from the playlist). Off: a 3-D title is written in 2-D. The demux "
      "option --second-view <titles> turns it on for chosen titles of one run.", 0, SET_AT_OPEN },
    { "bluray", "dolby_vision", SET_CHOICE, "merged", 0, 0, dv_layers,
      "Dolby Vision titles (UHD Blu-ray): merged writes one video file, each frame the base layer's picture "
      "followed by its enhancement layer's picture and metadata (the single-track form dovi_tool, mkvmerge and "
      "players read); separate writes the base layer and the enhancement layer (with the metadata) as two files, "
      "as they are on the disc.", 0, SET_AT_OPEN },
    { "dvd", "cell_mode", SET_CHOICE, "auto", 0, 0, cell_modes,
      "Which cells of a DVD title's program chain are played: walk (the cells the disc's navigation plays, found "
      "by playing the disc), trim (the program chain's cells without the decoy cells at its start and end), "
      "walk_trim (walk; trim for the titles the navigation does not reach, which are then listed too), auto "
      "(walk, or trim when playing the disc fails).", 0, SET_AT_OPEN },
    { "dvd", "cell_check", SET_CHOICE, "all", 0, 0, cell_checks,
      "How many VOBUs of a DVD cell are read when a cell is checked in full (in the decoy checks, mostly when the "
      "cells are trimmed): all (every VOBU; one broken VOBU rejects the cell), spread (50 evenly spread over the "
      "cell) or random (50 at random, a different choice on each run).", 0, SET_AT_OPEN },
    { "log", "debug_file", SET_BOOL, "false", 0, 0, NULL,
      "Also write a debug log with every detail into the output folder, next to the log.", 0, SET_AT_RIP },
    { "gui", "keep_folder_structure", SET_BOOL, "true", 0, 0, NULL,
      "GUI: a disc found in an added folder keeps the folders from that folder down to it under the main output "
      "folder (<main output folder>/<added folder>/<folders>/<disc name>); off: <main output folder>/<disc name>.",
      0, SET_IN_GUI },
    { "gui", "window", SET_TEXT, "", 0, 0, NULL, "Kept by the GUI: its window's place and size.", 1, SET_IN_GUI },
    { "gui", "columns", SET_LIST, "", 0, 0, NULL, "Kept by the GUI: the widths of the queue's columns.",
      1, SET_IN_GUI },
    { "gui", "panes", SET_LIST, "", 0, 0, NULL, "Kept by the GUI: the sizes of the window's panes.", 1, SET_IN_GUI },
    { "gui", "last_folder", SET_TEXT, "", 0, 0, NULL,
      "Kept by the GUI: the folder discs were last added from.", 1, SET_IN_GUI },
};
#define NB_SETTINGS (int)(sizeof(registry) / sizeof(registry[0]))

enum Source { FROM_DEFAULT, FROM_FILE, FROM_COMMAND_LINE };
static const char *const source_name[] = { "default", "settings file", "command line" };

typedef struct Value {
    char        *file;               /* value in the file (text form), NULL = not there */
    char        *run;                /* value of this run (--set), NULL = none */
    const char **items;              /* SET_LIST: items of the effective value */
    int          nb_items;
    int          stored;             /* changed by settings_store() in this run (written by settings_save()) */
} Value;

static Value values[NB_SETTINGS];
static char  path_buf[PATH_MAX], dir_buf[PATH_MAX];

const char *program_dir(void)
{
    if (!*dir_buf) {
        ssize_t n = readlink("/proc/self/exe", dir_buf, sizeof(dir_buf) - 1);
        char *slash;
        if (n <= 0) {
            strcpy(dir_buf, ".");
            return dir_buf;
        }
        dir_buf[n] = 0;
        if ((slash = strrchr(dir_buf, '/')))
            *slash = 0;
    }
    return dir_buf;
}

const char *settings_path(void)
{
    return path_buf;
}

static int find(const char *name)
{
    const char *dot = strchr(name, '.');
    if (!dot)
        return -1;
    for (int i = 0; i < NB_SETTINGS; i++)
        if (strlen(registry[i].group) == (size_t)(dot - name) && !strncmp(registry[i].group, name, dot - name) &&
            !strcmp(registry[i].key, dot + 1))
            return i;
    return -1;
}

static const char *effective(int i, enum Source *src)
{
    if (values[i].run) {
        if (src) *src = FROM_COMMAND_LINE;
        return values[i].run;
    }
    if (values[i].file) {
        if (src) *src = FROM_FILE;
        return values[i].file;
    }
    if (src) *src = FROM_DEFAULT;
    return registry[i].def;
}

/* Checks a value in text form; returns 0, or -1 with why in err. */
static int check(const Setting *s, const char *v, char *err, size_t size)
{
    char *end;
    long long n;

    switch (s->kind) {
    case SET_BOOL:
        if (strcmp(v, "true") && strcmp(v, "false")) {
            snprintf(err, size, "true or false");
            return -1;
        }
        return 0;
    case SET_INT:
        errno = 0;
        n = strtoll(v, &end, 10);
        if (errno || !*v || *end || n < s->min || n > s->max) {
            snprintf(err, size, "a number from %lld to %lld", (long long)s->min, (long long)s->max);
            return -1;
        }
        return 0;
    case SET_CHOICE:
        for (const char *const *c = s->choices; *c; c++)
            if (!strcmp(*c, v))
                return 0;
        snprintf(err, size, "one of:");
        for (const char *const *c = s->choices; *c; c++)
            snprintf(err + strlen(err), size - strlen(err), " %s", *c);
        return -1;
    default:
        return 0;
    }
}

static void split_items(int i)
{
    const char *v = effective(i, NULL);
    char *copy, *tok, *save = NULL;

    free(values[i].items);
    values[i].items    = NULL;
    values[i].nb_items = 0;
    if (!*v)
        return;
    copy = strdup(v);
    values[i].items = calloc(strlen(v) / 2 + 2, sizeof(char *));
    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
        values[i].items[values[i].nb_items++] = tok;
}

/* A file value in text form (a list: items joined by commas), or NULL when
 * its type does not fit. */
static char *from_toml(const Setting *s, toml_datum_t d)
{
    char buf[64];

    switch (s->kind) {
    case SET_BOOL:
        return d.type == TOML_BOOLEAN ? strdup(d.u.boolean ? "true" : "false") : NULL;
    case SET_INT:
        if (d.type != TOML_INT64)
            return NULL;
        snprintf(buf, sizeof(buf), "%lld", (long long)d.u.int64);
        return strdup(buf);
    case SET_CHOICE:
    case SET_TEXT:
        return d.type == TOML_STRING ? strdup(d.u.s) : NULL;
    case SET_LIST: {
        size_t len = 1;
        char *out;
        if (d.type != TOML_ARRAY)
            return NULL;
        for (int k = 0; k < d.u.arr.size; k++) {
            if (d.u.arr.elem[k].type != TOML_STRING || strchr(d.u.arr.elem[k].u.s, ','))
                return NULL;
            len += strlen(d.u.arr.elem[k].u.s) + 1;
        }
        out = calloc(1, len);
        for (int k = 0; k < d.u.arr.size; k++) {
            if (k)
                strcat(out, ",");
            strcat(out, d.u.arr.elem[k].u.s);
        }
        return out;
    }
    }
    return NULL;
}

static void put_toml_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void put_value(FILE *f, const Setting *s, const char *v)
{
    if (s->kind == SET_BOOL || s->kind == SET_INT) {
        fputs(v, f);
    } else if (s->kind == SET_LIST) {
        char *copy = strdup(v), *tok, *save = NULL;
        int first = 1;
        fputc('[', f);
        for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            fputs(first ? "" : ", ", f);
            put_toml_string(f, tok);
            first = 0;
        }
        fputc(']', f);
        free(copy);
    } else {
        put_toml_string(f, v);
    }
}

/* The help text as comment lines of at most 78 characters. */
static void put_help(FILE *f, const char *help)
{
    const char *p = help;

    while (*p) {
        const char *end = p, *cut = NULL;
        while (*end && end - p < 76) {
            if (*end == ' ')
                cut = end;
            end++;
        }
        if (!*end)
            cut = end;
        else if (!cut)
            cut = end;
        fprintf(f, "# %.*s\n", (int)(cut - p), p);
        p = *cut ? cut + 1 : cut;
    }
}

/* The settings file as the program writes it, from the file values. */
static char *render(size_t *len)
{
    char *buf = NULL;
    FILE *f = open_memstream(&buf, len);
    const char *group = NULL;

    fputs("# disc-remuxer settings. Change a value here to keep it; the program adds new\n"
          "# settings at their default and removes settings it no longer has.\n", f);
    for (int i = 0; i < NB_SETTINGS; i++) {
        const Setting *s = &registry[i];
        if (!group || strcmp(group, s->group)) {
            fprintf(f, "\n[%s]\n", s->group);
            group = s->group;
        }
        fputc('\n', f);
        put_help(f, s->help);
        fprintf(f, "# read: %s\n", s->use == SET_AT_OPEN ? "as a disc is opened" : s->use == SET_AT_RIP ?
                "as a rip runs" : "by the GUI");
        fprintf(f, "# default: ");
        put_value(f, s, s->def);
        fprintf(f, "\n%s = ", s->key);
        put_value(f, s, values[i].file ? values[i].file : s->def);
        fputc('\n', f);
    }
    fclose(f);
    return buf;
}

/* The settings file written (through a .new file), the old one kept as
 * .bak. 0, or -1 (logged). */
static int write_file(const char *now, size_t now_len, const char *old, size_t old_len)
{
    char tmp[PATH_MAX + 8];
    FILE *f;

    if (old) {
        char bak[PATH_MAX + 8];
        snprintf(bak, sizeof(bak), "%s.bak", path_buf);
        if ((f = fopen(bak, "wb"))) {
            fwrite(old, 1, old_len, f);
            fclose(f);
        }
    }
    snprintf(tmp, sizeof(tmp), "%s.new", path_buf);
    if (!(f = fopen(tmp, "wb")) || fwrite(now, 1, now_len, f) != now_len || fclose(f) || rename(tmp, path_buf)) {
        log_msg(MSG_SETTINGS_UNWRITABLE, LOG_ERROR, "The settings file %s cannot be written: %s", path_buf,
                strerror(errno));
        return -1;
    }
    return 0;
}

int settings_load(const char *path)
{
    toml_result_t r = { 0 };
    char *old = NULL, *now;
    size_t old_len = 0, now_len;
    int changed = 0;
    FILE *f;

    if (path)
        snprintf(path_buf, sizeof(path_buf), "%s", path);
    else
        snprintf(path_buf, sizeof(path_buf), "%s/disc-remuxer.toml", program_dir());

    if ((f = fopen(path_buf, "rb"))) {
        fseek(f, 0, SEEK_END);
        old_len = ftell(f);
        rewind(f);
        old = malloc(old_len + 1);
        if (fread(old, 1, old_len, f) != old_len) {
            fclose(f);
            log_msg(MSG_SETTINGS_UNREADABLE, LOG_ERROR, "The settings file %s cannot be read", path_buf);
            return -1;
        }
        old[old_len] = 0;
        fclose(f);
        r = toml_parse(old, (int)old_len);
        if (!r.ok) {
            log_msg(MSG_SETTINGS_INVALID, LOG_ERROR, "The settings file %s is not valid TOML: %s", path_buf, r.errmsg);
            toml_free(r);
            free(old);
            return -1;
        }
        for (int i = 0; i < NB_SETTINGS; i++) {
            const Setting *s = &registry[i];
            toml_datum_t g = toml_get(r.toptab, s->group), d;
            char err[256];
            if (g.type != TOML_TABLE || (d = toml_get(g, s->key)).type == TOML_UNKNOWN) {
                log_msg(MSG_SETTING_ADDED, LOG_INFO, "Settings file: %s.%s added (default %s)", s->group, s->key,
                        *s->def ? s->def : "empty");
                changed = 1;
                continue;
            }
            if (!(values[i].file = from_toml(s, d)) || check(s, values[i].file, err, sizeof(err))) {
                log_msg(MSG_SETTING_BAD, LOG_WARNING, "Settings file: %s.%s has a value of the wrong kind "
                        "(line %d): the default is used and written back", s->group, s->key, d.lineno);
                free(values[i].file);
                values[i].file = NULL;
                changed = 1;
            }
        }
        /* settings the program no longer has */
        for (int g = 0; g < r.toptab.u.tab.size; g++) {
            toml_datum_t t = r.toptab.u.tab.value[g];
            const char *gname = r.toptab.u.tab.key[g];
            if (t.type != TOML_TABLE) {
                log_msg(MSG_SETTING_REMOVED, LOG_WARNING, "Settings file: %s is no setting: removed", gname);
                changed = 1;
                continue;
            }
            for (int k = 0; k < t.u.tab.size; k++) {
                char name[256];
                snprintf(name, sizeof(name), "%s.%s", gname, t.u.tab.key[k]);
                if (find(name) < 0) {
                    log_msg(MSG_SETTING_REMOVED, LOG_WARNING, "Settings file: %s is no setting of this program: "
                            "removed", name);
                    changed = 1;
                }
            }
        }
        toml_free(r);
    } else {
        log_msg(MSG_SETTINGS_CREATED, LOG_INFO, "Settings file created with every setting at its default: %s",
                path_buf);
        changed = 1;
    }

    now = render(&now_len);
    if ((changed || !old || now_len != old_len || memcmp(now, old, now_len)) &&
        write_file(now, now_len, old, old_len) < 0) {
        free(now);
        free(old);
        return -1;
    }
    free(now);
    free(old);
    for (int i = 0; i < NB_SETTINGS; i++)
        if (registry[i].kind == SET_LIST)
            split_items(i);
    return 0;
}

int settings_set(const char *a)
{
    const char *eq = strchr(a, '=');
    char name[256], err[256];
    int i;

    if (!eq || eq - a >= (int)sizeof(name)) {
        log_msg(MSG_SET_SYNTAX, LOG_ERROR, "--set wants group.key=value, not '%s'", a);
        return -1;
    }
    snprintf(name, sizeof(name), "%.*s", (int)(eq - a), a);
    if ((i = find(name)) < 0) {
        log_msg(MSG_SET_UNKNOWN, LOG_ERROR, "--set: there is no setting %s (disc-remuxer settings lists them)", name);
        return -1;
    }
    if (check(&registry[i], eq + 1, err, sizeof(err))) {
        log_msg(MSG_SET_BAD, LOG_ERROR, "--set %s: the value must be %s", name, err);
        return -1;
    }
    free(values[i].run);
    values[i].run = strdup(eq + 1);
    if (registry[i].kind == SET_LIST)
        split_items(i);
    return 0;
}

int64_t setting_int(const char *name)
{
    int i = find(name);
    return i < 0 ? 0 : strtoll(effective(i, NULL), NULL, 10);
}

int setting_bool(const char *name)
{
    int i = find(name);
    return i >= 0 && !strcmp(effective(i, NULL), "true");
}

const char *setting_text(const char *name)
{
    int i = find(name);
    return i < 0 ? "" : effective(i, NULL);
}

int setting_list(const char *name, const char *const **items)
{
    int i = find(name);
    if (i < 0) {
        *items = NULL;
        return 0;
    }
    *items = values[i].items;
    return values[i].nb_items;
}

void settings_log_changed(void)
{
    char line[2048] = "";

    for (int i = 0; i < NB_SETTINGS; i++) {
        enum Source src;
        const char *v = effective(i, &src);
        if (strcmp(v, registry[i].def) && !registry[i].kept)
            snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s.%s = %s%s", *line ? ", " : "",
                     registry[i].group, registry[i].key, *v ? v : "(empty)",
                     src == FROM_COMMAND_LINE ? " (this run)" : "");
    }
    if (*line)
        log_msg(MSG_SETTINGS_CHANGED, LOG_INFO, "Settings changed from default: %s", line);
}

void settings_show(void)
{
    printf("Settings file: %s\n", path_buf);
    for (int i = 0; i < NB_SETTINGS; i++) {
        enum Source src;
        const char *v = effective(i, &src);
        char name[128];
        snprintf(name, sizeof(name), "%s.%s", registry[i].group, registry[i].key);
        int changed = strcmp(v, registry[i].def) != 0;
        printf("  %-34s %s%s%s%s\n", name, *v ? v : "(empty)", changed ? "   (" : "",
               changed ? source_name[src] : "", changed ? ")" : "");
    }
}

int settings_count(void)
{
    return NB_SETTINGS;
}

const Setting *settings_get(int i)
{
    return i >= 0 && i < NB_SETTINGS ? &registry[i] : NULL;
}

const char *settings_value(int i)
{
    return i >= 0 && i < NB_SETTINGS ? effective(i, NULL) : "";
}

int settings_store(const char *name, const char *value, char *err, int size)
{
    int i = find(name);

    if (i < 0) {
        snprintf(err, size, "there is no setting %s", name);
        return -1;
    }
    if (check(&registry[i], value, err, size))
        return -1;
    free(values[i].file);
    values[i].file = strdup(value);
    values[i].stored = 1;
    free(values[i].run);
    values[i].run = NULL;
    if (registry[i].kind == SET_LIST)
        split_items(i);
    return 0;
}

int settings_save(void)
{
    char *now, *old = NULL;
    size_t now_len, old_len = 0;
    FILE *f;
    int ret;

    if ((f = fopen(path_buf, "rb"))) {
        fseek(f, 0, SEEK_END);
        old_len = ftell(f);
        rewind(f);
        old = malloc(old_len + 1);
        if (fread(old, 1, old_len, f) != old_len) {
            free(old);
            old = NULL;
        }
        fclose(f);
    }
    /* the file as it is now (changed by hand, or by another run, since it was
     * read): its values are kept but for those this run stored */
    if (old) {
        toml_result_t r;
        old[old_len] = 0;
        r = toml_parse(old, (int)old_len);
        if (r.ok) {
            for (int i = 0; i < NB_SETTINGS; i++) {
                const Setting *s = &registry[i];
                toml_datum_t g = toml_get(r.toptab, s->group), d;
                char err[256], *v;
                if (values[i].stored || g.type != TOML_TABLE || (d = toml_get(g, s->key)).type == TOML_UNKNOWN ||
                    !(v = from_toml(s, d)))
                    continue;
                if (check(s, v, err, sizeof(err))) {
                    free(v);
                    continue;
                }
                free(values[i].file);
                values[i].file = v;
                if (s->kind == SET_LIST)
                    split_items(i);
            }
        }
        toml_free(r);
    }
    now = render(&now_len);
    ret = old && now_len == old_len && !memcmp(now, old, now_len) ? 0 : write_file(now, now_len, old, old_len);
    free(now);
    free(old);
    return ret;
}
