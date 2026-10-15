/*
 * Finding discs and the folder a disc's output goes to (discs.h).
 */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/mem.h"
#include "base/settings.h"
#include "discs.h"
#include "engine.h"
#include "names.h"

static const char *const disc_dirs[] = { "VIDEO_TS", "BDMV", "HVDVD_TS", "HDDVD_TS" };

/* ---- paths ---- */

/* path absolute (from the working folder) and clean: no "." parts, ".."
 * parts taken back, no doubled or trailing "/" */
static void absolute(const char *path, char *out, size_t size)
{
    char tmp[PATH_MAX * 2], *parts[PATH_MAX / 2], *save = NULL, *p;
    int n = 0;

    if (path[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", path);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    }
    for (p = strtok_r(tmp, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (!strcmp(p, "."))
            continue;
        if (!strcmp(p, "..")) {
            n -= n > 0;
            continue;
        }
        parts[n++] = p;
    }
    out[0] = 0;
    for (int i = 0; i < n; i++)
        snprintf(out + strlen(out), size - strlen(out), "/%s", parts[i]);
    if (!n)
        snprintf(out, size, "/");
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : s ? s : path;
}

/* the folder holding path */
static void parent(const char *path, char *out, size_t size)
{
    const char *s = strrchr(path, '/');

    if (!s || s == path)
        snprintf(out, size, "/");
    else
        snprintf(out, size, "%.*s", (int)(s - path), path);
}

/* path below root, "." for root itself */
static const char *relative(const char *root, const char *path)
{
    size_t n = strlen(root);

    if (!strcmp(root, path))
        return ".";
    if (!strcmp(root, "/"))
        return path + 1;
    return !strncmp(root, path, n) && path[n] == '/' ? path + n + 1 : path;
}

static int is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode);
}

static int exists(const char *dir, const char *name)
{
    char p[PATH_MAX * 2];
    struct stat st;

    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return !lstat(p, &st);
}

/* ---- what is a disc ---- */

int engine_is_disc_image(const char *path)
{
    const char *b = base_name(path), *dot = strrchr(b, '.');

    return is_file(path) && dot && (!strcasecmp(dot + 1, "iso") || !strcasecmp(dot + 1, "img"));
}

/* A folder holding a disc's structure, or being one (VIDEO_TS.IFO or
 * index.bdmv directly in it). */
static int is_disc_folder(const char *dir)
{
    for (size_t i = 0; i < sizeof(disc_dirs) / sizeof(*disc_dirs); i++) {
        char p[PATH_MAX * 2];
        snprintf(p, sizeof(p), "%s/%s", dir, disc_dirs[i]);
        if (is_dir(p))
            return 1;
    }
    return exists(dir, "VIDEO_TS.IFO") || exists(dir, "video_ts.ifo") || exists(dir, "index.bdmv") ||
           exists(dir, "INDEX.BDMV");
}

/* A VIDEO_TS or BDMV folder says nothing about the disc: its parent's name. */
static void folder_name(const char *dir, char *out, size_t size)
{
    const char *n = base_name(dir);

    if ((!strcasecmp(n, "VIDEO_TS") || !strcasecmp(n, "BDMV")) && strcmp(dir, "/")) {
        char up[PATH_MAX];
        parent(dir, up, sizeof(up));
        snprintf(out, size, "%s", base_name(up));
        return;
    }
    snprintf(out, size, "%s", n);
}

/* an image's name: its file name without the last extension */
static void image_name(const char *path, char *out, size_t size)
{
    const char *b = base_name(path), *dot = strrchr(b, '.');

    snprintf(out, size, "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
}

/* ---- the search ---- */

static int add(EngineFoundList *l, const char *path, const char *name, const char *rel, const char *root)
{
    EngineFound *f;

    if (l->nb == l->alloc) {
        int alloc = l->alloc ? 2 * l->alloc : 16;
        EngineFound *n = dr_realloc_array(l->discs, alloc, sizeof(*n));
        if (!n)
            return -1;
        l->discs = n;
        l->alloc = alloc;
    }
    f = &l->discs[l->nb];
    f->path     = dr_strdup(path);
    f->name     = dr_strdup(name);
    f->relative = dr_strdup(rel);
    f->root     = dr_strdup(root);
    if (!f->path || !f->name || !f->relative || !f->root) {
        dr_free(f->path);
        dr_free(f->name);
        dr_free(f->relative);
        dr_free(f->root);
        return -1;
    }
    l->nb++;
    return 0;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static int search(const char *dir, const char *root, int depth, int max_depth, EngineFoundList *out)
{
    DIR *d;
    struct dirent *e;
    char **names = NULL;
    int nb = 0, alloc = 0, ret = 0;

    if (depth > max_depth)
        return 0;
    if (is_disc_folder(dir)) {
        char name[NAME_MAX + 1];
        folder_name(dir, name, sizeof(name));
        return add(out, dir, name, relative(root, dir), root);
    }
    /* images and subfolders as they come in name order: a folder may hold
     * both (extras as an image, the volumes as disc folders); hidden ones
     * left out */
    if (!(d = opendir(dir)))
        return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (nb == alloc) {
            char **n = dr_realloc_array(names, alloc = alloc ? 2 * alloc : 32, sizeof(*n));
            if (!n) {
                ret = -1;
                break;
            }
            names = n;
        }
        if (!(names[nb] = dr_strdup(e->d_name))) {
            ret = -1;
            break;
        }
        nb++;
    }
    closedir(d);
    if (ret >= 0)
        qsort(names, nb, sizeof(*names), by_name);
    for (int i = 0; ret >= 0 && i < nb; i++) {
        char p[PATH_MAX * 2];
        snprintf(p, sizeof(p), "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", names[i]);
        if (engine_is_disc_image(p)) {
            char name[NAME_MAX + 1];
            image_name(p, name, sizeof(name));
            ret = add(out, p, name, relative(root, p), root);
        } else if (is_dir(p)) {
            ret = search(p, root, depth + 1, max_depth, out);
        }
    }
    for (int i = 0; i < nb; i++)
        dr_free(names[i]);
    dr_free(names);
    return ret;
}

int engine_find_discs(const char *path, int max_depth, EngineFoundList *out)
{
    char abs[PATH_MAX], name[NAME_MAX + 1];

    absolute(path, abs, sizeof(abs));
    if (engine_is_disc_image(abs)) {
        char dir[PATH_MAX];
        image_name(abs, name, sizeof(name));
        parent(abs, dir, sizeof(dir));
        return add(out, abs, name, ".", dir);
    }
    if (!is_dir(abs))
        return 0;
    if (is_disc_folder(abs)) {
        folder_name(abs, name, sizeof(name));
        return add(out, abs, name, ".", abs);
    }
    return search(abs, abs, 0, max_depth, out);
}

void engine_found_free(EngineFoundList *l)
{
    for (int i = 0; i < l->nb; i++) {
        dr_free(l->discs[i].path);
        dr_free(l->discs[i].name);
        dr_free(l->discs[i].relative);
        dr_free(l->discs[i].root);
    }
    dr_freep(&l->discs);
    l->nb = l->alloc = 0;
}

/* ---- where a disc's output goes ---- */

void engine_found_folder(const EngineFound *f, const char *root, char *out, size_t size)
{
    char clean[1024];

    if (root)
        snprintf(out, size, "%s", root);
    else
        engine_output_root(out, size);
    if (setting_bool("output.keep_folder_structure") && strcmp(f->relative, ".")) {
        /* the named folder, then the folders down to the disc (the disc itself: its name below) */
        char rel[PATH_MAX], *save = NULL, *p, *last;
        engine_clean_name(base_name(f->root), clean, sizeof(clean));
        snprintf(out + strlen(out), size - strlen(out), "/%s", clean);
        snprintf(rel, sizeof(rel), "%s", f->relative);
        if ((last = strrchr(rel, '/')))
            *last = 0;
        else
            rel[0] = 0;
        for (p = strtok_r(rel, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
            engine_clean_name(p, clean, sizeof(clean));
            snprintf(out + strlen(out), size - strlen(out), "/%s", clean);
        }
    }
    engine_clean_name(f->name, clean, sizeof(clean));
    snprintf(out + strlen(out), size - strlen(out), "/%s", clean);
}

void engine_disc_name(const char *source, char *out, size_t size)
{
    EngineFoundList l = { 0 };
    char tmp[PATH_MAX], *base, *dot;
    size_t n;

    /* a disc the finder recognises: its name */
    if (engine_find_discs(source, 0, &l) >= 0 && l.nb == 1 && !strcmp(l.discs[0].relative, ".")) {
        clean_name(l.discs[0].name, 0, out, (int)size);
        engine_found_free(&l);
        return;
    }
    engine_found_free(&l);
    /* else the source's last part */
    snprintf(tmp, sizeof(tmp), "%s", source);
    while ((n = strlen(tmp)) > 1 && tmp[n - 1] == '/')
        tmp[n - 1] = 0;
    base = strrchr(tmp, '/') ? strrchr(tmp, '/') + 1 : tmp;
    if ((!strcasecmp(base, "VIDEO_TS") || !strcasecmp(base, "BDMV")) && base > tmp + 1) {
        base[-1] = 0;
        base = strrchr(tmp, '/') ? strrchr(tmp, '/') + 1 : tmp;
    }
    if ((dot = strrchr(base, '.')) && !strcasecmp(dot, ".iso"))
        *dot = 0;
    clean_name(base, 0, out, (int)size);
}
