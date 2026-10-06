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

typedef struct Setting {
    const char        *group, *key;
    enum SettingKind   kind;
    const char        *def;          /* default, as text (a list: items separated by commas) */
    int64_t            min, max;     /* SET_INT */
    const char *const *choices;      /* SET_CHOICE, NULL-terminated */
    const char        *help;
} Setting;

static const char *const udf_readers[]  = { "netbsd", "linux", NULL };
static const char *const cell_modes[]   = { "auto", "walk", "trim", "walk_trim", NULL };
static const char *const title_orders[] = { "auto", "scan_first", "table", NULL };

static const Setting registry[] = {
    { "read", "attempts", SET_INT, "5", 1, 99, NULL,
      "How many times a read from a disc is tried before it counts as failed; after the first retry "
      "the rest of the read goes one sector at a time." },
    { "read", "udf_reader", SET_CHOICE, "netbsd", 0, 0, udf_readers,
      "Which UDF reader opens disc images: netbsd (based on NetBSD's UDF code) or linux (based on Linux's)." },
    { "read", "prefer_iso_for_old_udf102", SET_BOOL, "true", 0, 0, NULL,
      "Read a DVD image whose UDF 1.02 file system was recorded before 2006 through its ISO 9660 file "
      "system, when that holds a valid DVD-Video structure." },
    { "dvd", "cell_mode", SET_CHOICE, "auto", 0, 0, cell_modes,
      "How a DVD title's cells are chosen: walk = the cells the disc's navigation plays, trim = cells "
      "that do not look like content are trimmed off the program chain's ends, walk_trim = walk, trim "
      "where walk finds nothing, auto = walk when the navigation found titles, else trim." },
    { "dvd", "title_order", SET_CHOICE, "auto", 0, 0, title_orders,
      "Order of a DVD's titles: scan_first = the titles the disc's navigation leads to first, then the "
      "others; table = the disc's title table order; auto = scan_first with the cell modes auto and "
      "walk, table with trim and walk_trim." },
    { "dvd", "min_title_length", SET_INT, "120", 0, 86400, NULL,
      "DVD titles shorter than this many seconds are listed but not selected." },
    { "aacs", "key_files", SET_LIST, "", 0, 0, NULL,
      "AACS key files (KEYDB.cfg format), read in this order; the first one holding a key for the disc "
      "is used. Empty: .config/aacs/KEYDB.cfg next to the program." },
    { "output", "file_name_template", SET_TEXT, "", 0, 0, NULL,
      "How a title's output files are named. {NAME1} the title's name, {CMNT1} its comment, {DT} its "
      "date, {N2} its number with two digits; {prefix:VAR} writes the prefix only after other text. "
      "Empty: {NAME1}{-:CMNT1}{-:DT}{title:+DFLT}{_t:N2}." },
    { "log", "debug_file", SET_BOOL, "false", 0, 0, NULL,
      "Also write a debug log with every detail into the output folder, next to the log." },
};
#define NB_SETTINGS (int)(sizeof(registry) / sizeof(registry[0]))

enum Source { FROM_DEFAULT, FROM_FILE, FROM_COMMAND_LINE };
static const char *const source_name[] = { "default", "settings file", "command line" };

typedef struct Value {
    char        *file;               /* value in the file (text form), NULL = not there */
    char        *run;                /* value of this run (--set), NULL = none */
    const char **items;              /* SET_LIST: items of the effective value */
    int          nb_items;
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
        fprintf(f, "# default: ");
        put_value(f, s, s->def);
        fprintf(f, "\n%s = ", s->key);
        put_value(f, s, values[i].file ? values[i].file : s->def);
        fputc('\n', f);
    }
    fclose(f);
    return buf;
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
    if (changed || !old || now_len != old_len || memcmp(now, old, now_len)) {
        char tmp[PATH_MAX + 8];
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
            free(now);
            free(old);
            return -1;
        }
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
        if (strcmp(v, registry[i].def))
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
