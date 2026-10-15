/*
 * Blu-ray: playlist names from a BD-J menu's project data
 */

/*
 * A widely used BD-J menu framework keeps its project in
 * BDMV/JAR/<dir>/bluray_project.bin, written with Java's DataOutputStream
 * (big-endian 4-byte ints and floats, strings as a 2-byte length and
 * modified UTF-8). The layout, as the framework's own loader reads it:
 *   project: name, int discs, the discs [, int major, minor, patch]
 *   disc:    name, id (32 hex digits: the disc ID of CERTIFICATE/id.bdmv),
 *            int n + n x { int number, name } (sounds),
 *            int n + n x playlist
 *   playlist: name, [tag], int id (the playlist number), [presentation],
 *            float frame rate, float duration (s),
 *            int n + n x { [tag], int id, float in, float out, [int still mode],
 *                          int n + n x angle },
 *            int n + n x { int number, type, float time } (marks)
 *   angle:   int, video { name, [tag], int number, int application type,
 *            int width, int height, aspect, [dynamic range] },
 *            int n + n x audio { name, [tag], int number, language, [sub-language] },
 *            int n + n x subtitle (older: name, int number, type, language;
 *                newer: name, tag, int number, forced, all forced, type,
 *                language, sub-language),
 *            int n + n x secondary video { name, [tag], int number, int width,
 *                int height, aspect },
 *            int n + n x secondary audio (as audio)
 * ([...]: only in the newer layout; the version ints only in some newer
 * files.) The file is accepted when one layout reads it to its last byte.
 * The playlists the menu names are what the reference program asks the
 * menu program about: they are listed first and keep a title even when
 * they are one clip piece; their names go to the titles. The reference
 * program lets the menu program map each id to its playlist; here the id
 * is taken as the playlist and checked: every named playlist that does not
 * play a clip twice must last what the project says (within 2 s). Any
 * doubt: a warning and nothing of the data is used.
 */

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/str.h"
#include "base/macros.h"
#include "base/bytes.h"
#include "base/bytes.h"
#include "base/mem.h"

#include "bluray_internal.h"

#define FILE_NAME   "bluray_project"
#define LENGTH_SLACK 2.0                    /**< s */

typedef struct Reader {
    const uint8_t *p, *end;
    int            v2;                      /**< the newer layout */
    int            bad;                     /**< read past the end or a string that is not text */
} Reader;

typedef struct Entry {
    char    *name;
    int      id;
    float    duration;
} Entry;

typedef struct Disc {
    char    *name, *id;
    Entry   *pl;
    int      nb_pl;
} Disc;

typedef struct Project {
    char    *name;
    Disc    *discs;
    int      nb_discs;
} Project;

static int32_t rd_int(Reader *r)
{
    int32_t v;

    if (r->bad || r->end - r->p < 4) {
        r->bad = 1;
        return 0;
    }
    v = dr_rb32(r->p);
    r->p += 4;
    return v;
}

static float rd_float(Reader *r)
{
    return dr_int2float(rd_int(r));
}

/* A count: not negative and not more than the bytes left could hold. */
static int rd_count(Reader *r)
{
    int32_t n = rd_int(r);

    if (n < 0 || n > r->end - r->p)
        r->bad = 1;
    return r->bad ? 0 : n;
}

/* A string; *out (dr_free()) when out is set. Modified UTF-8 must be valid
 * UTF-8 apart from its two-byte NUL. */
/* One UTF-8 code point, accepted exactly as the processing library's
 * reader accepted it (continuation bytes checked, overlong forms not
 * rejected, 0xFE / 0xFF leads bad): 0 and *code, -1 bad. */
static int utf8_code(const uint8_t **qp, const uint8_t *e, uint32_t *code)
{
    const uint8_t *q = *qp;
    uint32_t val = q < e ? *q++ : 0;
    uint32_t top = (val & 128) >> 1;

    if ((val & 0xc0) == 0x80 || val >= 0xfe) {
        *qp = q;
        return -1;
    }
    while (val & top) {
        uint32_t tmp = (uint32_t)(q < e ? *q++ : 0) - 128;
        if (tmp >> 6) {
            *qp = q;
            return -1;
        }
        val = (val << 6) + tmp;
        top <<= 5;
    }
    *code = val & ((top << 1) - 1);
    *qp = q;
    return 0;
}

static void rd_str(Reader *r, char **out)
{
    const uint8_t *s, *e;
    unsigned len;

    if (r->bad || r->end - r->p < 2) {
        r->bad = 1;
        return;
    }
    len = dr_rb16(r->p);
    if (r->end - r->p - 2 < len) {
        r->bad = 1;
        return;
    }
    s = r->p + 2;
    e = s + len;
    r->p = e;
    for (const uint8_t *q = s; q < e && !r->bad;) {
        uint32_t code;
        if (q + 1 < e && q[0] == 0xc0 && q[1] == 0x80) {
            q += 2;
            continue;
        }
        if (utf8_code(&q, e, &code)) {
            r->bad = 1;
            break;
        }
        if (!code)
            r->bad = 1;
    }
    if (!r->bad && out && !(*out = dr_strndup((const char *)s, len)))
        r->bad = 2;
}

static void angle(Reader *r)
{
    int n;

    rd_int(r);
    rd_str(r, NULL);                        /* primary video */
    if (r->v2)
        rd_str(r, NULL);
    rd_int(r); rd_int(r); rd_int(r); rd_int(r);
    rd_str(r, NULL);
    if (r->v2)
        rd_str(r, NULL);
    n = rd_count(r);                        /* primary audio */
    for (int i = 0; i < n && !r->bad; i++) {
        rd_str(r, NULL);
        if (r->v2)
            rd_str(r, NULL);
        rd_int(r);
        rd_str(r, NULL);
        if (r->v2)
            rd_str(r, NULL);
    }
    n = rd_count(r);                        /* subtitles */
    for (int i = 0; i < n && !r->bad; i++) {
        rd_str(r, NULL);
        if (r->v2)
            rd_str(r, NULL);
        rd_int(r);
        for (int k = 0; k < (r->v2 ? 5 : 2); k++)
            rd_str(r, NULL);
    }
    n = rd_count(r);                        /* secondary video */
    for (int i = 0; i < n && !r->bad; i++) {
        rd_str(r, NULL);
        if (r->v2)
            rd_str(r, NULL);
        rd_int(r); rd_int(r); rd_int(r);
        rd_str(r, NULL);
    }
    n = rd_count(r);                        /* secondary audio */
    for (int i = 0; i < n && !r->bad; i++) {
        rd_str(r, NULL);
        if (r->v2)
            rd_str(r, NULL);
        rd_int(r);
        rd_str(r, NULL);
        if (r->v2)
            rd_str(r, NULL);
    }
}

static void playlist(Reader *r, Entry *e)
{
    int n;

    rd_str(r, &e->name);
    if (r->v2)
        rd_str(r, NULL);
    e->id = rd_int(r);
    if (r->v2)
        rd_str(r, NULL);
    rd_float(r);
    e->duration = rd_float(r);
    n = rd_count(r);                        /* items */
    for (int i = 0; i < n && !r->bad; i++) {
        int na;
        if (r->v2)
            rd_str(r, NULL);
        rd_int(r); rd_float(r); rd_float(r);
        if (r->v2)
            rd_int(r);
        na = rd_count(r);
        for (int a = 0; a < na && !r->bad; a++)
            angle(r);
    }
    n = rd_count(r);                        /* marks */
    for (int i = 0; i < n && !r->bad; i++) {
        rd_int(r);
        rd_str(r, NULL);
        rd_float(r);
    }
}

static void project_free(Project *p)
{
    for (int i = 0; i < p->nb_discs; i++) {
        for (int k = 0; k < p->discs[i].nb_pl; k++)
            dr_free(p->discs[i].pl[k].name);
        dr_free(p->discs[i].pl);
        dr_free(p->discs[i].name);
        dr_free(p->discs[i].id);
    }
    dr_free(p->discs);
    dr_free(p->name);
    memset(p, 0, sizeof(*p));
}

/* The project in one layout: 0 read to its last byte, DR_ERR_INVALID
 * not this layout, DR_ERR(ENOMEM). */
static int project_read(const BlurayFile *f, int v2, Project *p)
{
    Reader r = { f->data, f->data + f->size, v2, 0 };
    int n;

    memset(p, 0, sizeof(*p));
    rd_str(&r, &p->name);
    n = rd_count(&r);
    if (n && !r.bad && !(p->discs = dr_calloc(n, sizeof(*p->discs))))
        r.bad = 2;
    for (int i = 0; i < n && !r.bad; i++) {
        Disc *d = &p->discs[p->nb_discs++];
        int ns, np;
        rd_str(&r, &d->name);
        rd_str(&r, &d->id);
        ns = rd_count(&r);
        for (int k = 0; k < ns && !r.bad; k++) {
            rd_int(&r);
            rd_str(&r, NULL);
        }
        np = rd_count(&r);
        if (np && !r.bad && !(d->pl = dr_calloc(np, sizeof(*d->pl))))
            r.bad = 2;
        for (int k = 0; k < np && !r.bad; k++)
            playlist(&r, &d->pl[d->nb_pl++]);
    }
    if (v2 && !r.bad && r.end - r.p == 12)  /* the format version */
        r.p += 12;
    if (r.bad == 2 || (!r.bad && r.p == r.end))
        return r.bad ? DR_ERR(ENOMEM) : 0;
    project_free(p);
    return DR_ERR_INVALID;
}

/* The disc's entry: its id the disc ID of id.bdmv, else the only one. */
static const Disc *pick_disc(const BlurayDisc *d, const Project *p)
{
    if (d->has_id)
        for (int i = 0; i < p->nb_discs; i++) {
            const char *id = p->discs[i].id;
            int same = id && strlen(id) == 32;
            for (int k = 0; k < 16 && same; k++) {
                char hex[3] = { id[2 * k], id[2 * k + 1], 0 };
                same = strtoul(hex, NULL, 16) == d->id.disc_id[k];
            }
            if (same)
                return &p->discs[i];
        }
    return p->nb_discs == 1 ? &p->discs[0] : NULL;
}

struct Dirs {
    char  found[96];
    int   n;
    void *log;
    DiscIOFS *fs;
};

static int jar_dir(void *opaque, const char *name, int is_dir)
{
    struct Dirs *s = opaque;
    char path[96];
    DiscIOFile *f;

    if (!is_dir || !strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    snprintf(path, sizeof(path), "/BDMV/JAR/%s/" FILE_NAME ".bin", name);
    if (s->fs->ops->open_file(s->fs, path, &f) < 0)
        return 0;
    ff_discio_file_free(&f);
    if (!s->n++)
        dr_strlcpy(s->found, name, sizeof(s->found));
    else
        dr_vlog(s->log, DR_VLOG_WARNING, "Blu-ray: menu project data in BDMV/JAR/%s as well as BDMV/JAR/%s: which one "
               "the menu reads is not known: not used\n", name, s->found);
    return 0;
}

static int playlist_index(const BlurayDisc *d, int id)
{
    for (int i = 0; i < d->nb_playlists; i++)
        if (!d->playlists[i].bdav && d->playlists[i].number == (uint32_t)id)
            return i;
    return -1;
}

/* Whether the playlist plays one of its clips twice (a loop: the project
 * gives one pass of it). */
static int plays_a_clip_twice(const BlurayPlaylist *pl)
{
    for (int i = 0; i < pl->nb_items; i++)
        for (int j = 0; j < i; j++)
            if (!strcmp(pl->items[i].clip, pl->items[j].clip))
                return 1;
    return 0;
}

int ff_bluray_menu_read(void *log, BlurayDisc *d)
{
    struct Dirs dirs = { .log = log, .fs = d->fs };
    BlurayFile f = { 0 };
    Project p = { 0 };
    const Disc *disc;
    char dir[128], **names = NULL;
    DrStr missing;
    int ret, nb_missing = 0, named = 0, v2 = 0;

    ff_bluray_menu_free(d);
    if (d->bdav || !ff_bluray_has_bdj(d) || d->fs->ops->list_dir(d->fs, "/BDMV/JAR", jar_dir, &dirs) < 0 || !dirs.n)
        return 0;
    if (dirs.n > 1)
        return 0;
    snprintf(dir, sizeof(dir), "/JAR/%s", dirs.found);
    if ((ret = ff_bluray_load(log, d->fs, "BDMV", dir, (const char *const[]){ FILE_NAME, NULL },
                              (const char *const[]){ "bin", NULL }, &f)) <= 0) {
        if (ret < 0)
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: BDMV/JAR/%s/" FILE_NAME ".bin cannot be read (%s): menu names "
                   "not used\n", dirs.found, dr_err_text(ret));
        return ret == DR_ERR(ENOMEM) ? ret : 0;
    }
    ret = project_read(&f, 0, &p);
    if (ret == DR_ERR_INVALID)
        ret = project_read(&f, v2 = 1, &p);
    ff_bluray_file_free(&f);
    if (ret == DR_ERR(ENOMEM))
        return ret;
    if (ret < 0) {
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: BDMV/JAR/%s/" FILE_NAME ".bin is not menu project data in a known "
               "layout: menu names not used\n", dirs.found);
        return 0;
    }
    if (!(disc = pick_disc(d, &p))) {
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: the menu project data has %d discs and none is this one (by its disc "
               "ID): menu names not used\n", p.nb_discs);
        goto end;
    }
    dr_str_init(&missing);
    for (int i = 0; i < disc->nb_pl; i++) {
        const Entry *e = &disc->pl[i];
        int k;
        if (e->id < 0 || e->id > 99999) {
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: the menu project data names playlist %d ('%s'), not a playlist "
                   "number: menu names not used\n", e->id, e->name);
            goto bad;
        }
        for (int j = 0; j < i; j++)
            if (disc->pl[j].id == e->id) {
                dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: the menu project data names playlist %05d twice ('%s', "
                       "'%s'): menu names not used\n", e->id, disc->pl[j].name, e->name);
                goto bad;
            }
        if ((k = playlist_index(d, e->id)) < 0) {
            dr_str_catf(&missing, "%s%05d '%s'", nb_missing++ ? ", " : "", e->id, e->name);
            continue;
        }
        if (!plays_a_clip_twice(&d->playlists[k])) {
            const BlurayPlaylist *pl = &d->playlists[k];
            int64_t len = 0;
            for (int j = 0; j < pl->nb_items; j++)
                len += (int64_t)pl->items[j].out_time - pl->items[j].in_time;
            if (fabs(len / 45000.0 - e->duration) > LENGTH_SLACK) {
                dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: the menu project data gives playlist %05d ('%s') %.1f s, the "
                       "playlist lasts %.1f s: the data may not name the playlists it seems to: menu names not used\n",
                       e->id, e->name, e->duration, len / 45000.0);
                goto bad;
            }
        }
    }
    if (!(names = dr_calloc(d->nb_playlists, sizeof(*names))) || !(d->menu_name = dr_strdup(p.name)))
        goto nomem;
    for (int i = 0; i < disc->nb_pl; i++) {
        int k = playlist_index(d, disc->pl[i].id);
        if (k >= 0) {
            if (!(names[k] = dr_strdup(disc->pl[i].name)))
                goto nomem;
            named++;
        }
    }
    d->pl_menu_name  = names;
    d->nb_menu_named = named;
    names = NULL;
    dr_vlog(log, DR_VLOG_INFO, "Blu-ray: the menu project data (BDMV/JAR/%s/" FILE_NAME ".bin, %s layout, project '%s', "
           "disc '%s') names %d playlist(s): they are listed first, keep a title when they are one clip piece and "
           "carry their menu names\n", dirs.found, v2 ? "newer" : "older", p.name, disc->name, named);
    if (nb_missing)
        dr_vlog(log, DR_VLOG_INFO, "Blu-ray: the menu project data names %d playlist(s) the disc does not have: %s\n",
               nb_missing, missing.str);
    dr_str_free(&missing);
    goto end;
nomem:
    dr_str_free(&missing);
    dr_free(names);
    project_free(&p);
    ff_bluray_menu_free(d);
    return DR_ERR(ENOMEM);
bad:
    dr_str_free(&missing);
end:
    project_free(&p);
    return 0;
}

void ff_bluray_menu_free(BlurayDisc *d)
{
    if (d->pl_menu_name)
        for (int i = 0; i < d->nb_playlists; i++)
            dr_free(d->pl_menu_name[i]);
    dr_freep(&d->pl_menu_name);
    dr_freep(&d->menu_name);
    d->nb_menu_named = 0;
}
