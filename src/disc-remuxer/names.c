/*
 * disc-remuxer: output names (see names.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "names.h"

#define MAX     378     /* characters of a name before its extension */
#define MAX_ERR 358     /* where an invalid template's result is cut */

/* One code point from UTF-8 at *p (advanced); an invalid sequence is one
 * byte that becomes '_'. */
static unsigned utf8_next(const unsigned char **p)
{
    const unsigned char *s = *p;
    unsigned c = s[0], n, cp;

    if (c < 0x80) {
        *p += 1;
        return c;
    }
    n = c >= 0xF0 && c < 0xF5 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : 0;
    if (!n || c > 0xF4) {
        *p += 1;
        return '_';
    }
    cp = c & (0x3F >> n);
    for (unsigned i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p += 1;
            return '_';
        }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    *p += n + 1;
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return 0xFFFD;
    return cp;
}

static int utf8_put(unsigned cp, char *out)
{
    if (cp < 0x80) {
        out[0] = cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = 0xC0 | cp >> 6;
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = 0xE0 | cp >> 12;
        out[1] = 0x80 | (cp >> 6 & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
    out[0] = 0xF0 | cp >> 18;
    out[1] = 0x80 | (cp >> 12 & 0x3F);
    out[2] = 0x80 | (cp >> 6 & 0x3F);
    out[3] = 0x80 | (cp & 0x3F);
    return 4;
}

void clean_name(const char *s, int rule, char *out, int size)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;

    while (*p && n < size - 5) {
        unsigned u = utf8_next(&p);
        if (u == '/' || u == '\\' || u == '*' || u == ';' || u == '?' || u < 0x20)
            u = '_';
        else if (u == '|')
            u = 'I';
        else if (u == ':')
            u = '-';
        else if (u == '"')
            u = '\'';
        if (rule >= 1 && (u == '#' || u == '$'))
            u = '_';
        if (rule >= 2 && (u == '.' || u == ' '))
            u = '_';
        if (rule >= 1) {
            if (u == 0xB7)
                u = '-';
            if (u == 0x2018 || u == 0x2019 || u == 0x201B || u == 0x2032 || u == 0x2033)
                u = '\'';
        }
        if (u == '_' && n && out[n - 1] == '_')
            continue;
        n += utf8_put(u, out + n);
    }
    while (n && out[n - 1] == '_')
        n--;
    out[n] = 0;
}

/* ---- the template ---- */

enum { V_NONE, V_TEXT, V_NUM, V_DFLT };

typedef struct Var {
    const char *name;
    int         kind;
    char        text[512];
} Var;

typedef struct Vars {
    Var v[40];
    int n;
} Vars;

static void put(Vars *d, const char *name, int kind, const char *text)
{
    Var *v = &d->v[d->n++];
    v->name = name;
    v->kind = kind;
    snprintf(v->text, sizeof(v->text), "%s", text ? text : "");
}

static const Var *get(const Vars *d, const char *name, size_t len)
{
    for (int i = 0; i < d->n; i++)
        if (strlen(d->v[i].name) == len && !strncmp(d->v[i].name, name, len))
            return &d->v[i];
    return NULL;
}

static void vars(Vars *d, const char *name, const char *comment, const char *date, unsigned index)
{
    static const char *const nk[4] = { "NAME", "NAME0", "NAME1", "NAME2" };
    static const char *const ck[4] = { "CMNT", "CMNT0", "CMNT1", "CMNT2" };
    static const int rule[4] = { 0, 0, 1, 2 };
    char buf[512], num[16], num1[16];
    int have_date = date && strlen(date) >= 19;

    d->n = 0;
    put(d, "DFLT", V_DFLT, NULL);
    for (int i = 0; i < 4; i++) {
        if (name)
            clean_name(name, rule[i], buf, sizeof(buf));
        put(d, nk[i], name ? V_TEXT : V_NONE, name ? buf : NULL);
        if (comment)
            clean_name(comment, rule[i], buf, sizeof(buf));
        put(d, ck[i], comment ? V_TEXT : V_NONE, comment ? buf : NULL);
    }
    if (have_date) {
        static const char *const dk[6] = { "DY", "DM", "DD", "TH", "TM", "TS" };
        static const int at[6] = { 0, 5, 8, 11, 14, 17 }, len[6] = { 4, 2, 2, 2, 2, 2 };
        char dt[32] = "";
        for (int i = 0; i < 6; i++) {
            snprintf(buf, sizeof(buf), "%.*s", len[i], date + at[i]);
            put(d, dk[i], V_TEXT, buf);
            strcat(dt, buf);
        }
        put(d, "DT", V_TEXT, dt);
    } else {
        static const char *const dk[7] = { "DY", "DM", "DD", "TH", "TM", "TS", "DT" };
        for (int i = 0; i < 7; i++)
            put(d, dk[i], V_NONE, NULL);
    }
    snprintf(num, sizeof(num), "%02u", index);
    snprintf(num1, sizeof(num1), "%02u", index + 1);
    put(d, "AN", V_NUM, num);
    put(d, "AM", V_NUM, num1);
    if (have_date) {
        static const char *const nn[6] = { "N", "N:", "M", "M:", "T", "T:" };
        for (int i = 0; i < 6; i++)
            put(d, nn[i], V_NONE, NULL);
    } else {
        put(d, "N", V_NUM, num);
        put(d, "M", V_NUM, num1);
        put(d, "T", index ? V_NUM : V_NONE, index ? num : NULL);
        if (!index)
            put(d, "T:", V_NONE, NULL);
    }
    put(d, "AT", index ? V_NUM : V_NONE, index ? num : NULL);
    if (!index)
        put(d, "AT:", V_NONE, NULL);
}

/* A field's value: 1 with *out (text, or NULL for the DFLT marker), 0 no
 * value, -1 an unknown variable. */
static int lookup(const Vars *d, const char *name, size_t len, int at_start, const char **out, char *numbuf)
{
    const Var *v;

    if (!len)
        return -1;
    if (name[0] == '+' || name[0] == '-') {
        const char *t;
        int r;
        if (len < 2 || name[1] == '+' || name[1] == '-')
            return -1;
        if ((r = lookup(d, name + 1, len - 1, at_start, &t, numbuf)) < 0)
            return -1;
        if ((name[0] == '+') != (r == 1))
            return 0;
        *out = r == 1 && !t ? NULL : "";     /* the DFLT marker passes through as itself */
        return 1;
    }
    if ((v = get(d, name, len))) {
        if (v->kind == V_NONE)
            return 0;
        if (v->kind == V_DFLT) {
            if (!at_start)
                return 0;
            *out = NULL;
            return 1;
        }
        *out = v->text;
        return 1;
    }
    /* a width form: VAR<digit> */
    if (len < 2 || name[len - 1] < '1' || name[len - 1] > '9' || (name[len - 2] >= '0' && name[len - 2] <= '9'))
        return -1;
    {
        char key[64];
        int w = name[len - 1] - '0';
        const char *s;
        snprintf(key, sizeof(key), "%.*s:", (int)(len - 1), name);
        if (get(d, key, len))
            return 0;
        if (!(v = get(d, name, len - 1)) || v->kind != V_NUM)
            return -1;
        s = v->text;
        while (*s == '0' && s[1])
            s++;
        if ((int)strlen(s) < w) {
            snprintf(numbuf, 16, "%0*d", w, atoi(s));
            *out = numbuf;
        } else {
            *out = s;
        }
        return 1;
    }
}

static void append(char *out, int *pos, const char *s)
{
    while (*s && *pos < MAX)
        out[(*pos)++] = *s++;
    out[*pos] = 0;
}

/* The template filled in: 1, or 0 when invalid (out holds what was written
 * before the field that makes it invalid). */
static int expand(const Vars *d, const char *t, char *out)
{
    int pos = 0;

    out[0] = 0;
    while (*t && pos < MAX) {
        const char *close, *c1, *c2, *name, *dflt = NULL, *v = NULL;
        size_t nlen;
        char numbuf[16];
        int r;

        if (*t != '{') {
            out[pos++] = *t++;
            out[pos] = 0;
            continue;
        }
        t++;
        if (!(close = strchr(t, '}')) || memchr(t, '{', close - t))
            return 0;
        c1 = memchr(t, ':', close - t);
        if (!c1) {
            name = t;
            nlen = close - t;
        } else {
            name = c1 + 1;
            c2   = memchr(name, ':', close - name);
            nlen = (c2 ? c2 : close) - name;
            if (c2)
                dflt = c2 + 1;
        }
        r = lookup(d, name, nlen, pos == 0, &v, numbuf);
        if (r < 0)
            return 0;
        if (!r && dflt) {
            static char dbuf[MAX + 1];
            snprintf(dbuf, sizeof(dbuf), "%.*s", (int)(close - dflt), dflt);
            v = dbuf;
            r = 1;
        }
        if (r) {
            if (c1 && (pos || !v)) {
                char pre[MAX + 1];
                snprintf(pre, sizeof(pre), "%.*s", (int)(c1 - t), t);
                append(out, &pos, pre);
            }
            if (v)
                append(out, &pos, v);
        }
        t = close + 1;
    }
    if (!pos)
        strcpy(out, "title");
    return 1;
}

void title_name(const char *tmpl, const char *name, const char *comment, const char *date, unsigned index,
                char *out, int size)
{
    static const char *const def = "{NAME1}{-:CMNT1}{-:DT}{title:+DFLT}{_t:N2}";
    char buf[MAX + 64];
    Vars d;
    const char *p;

    for (p = tmpl; p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'); p++)
        ;
    if (!p || !*p)
        tmpl = def;
    vars(&d, name, comment, date, index);
    if (!expand(&d, tmpl, buf)) {
        buf[MAX_ERR] = 0;
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "!ERRtemplate_t%02u", index);
    }
    snprintf(out, size, "%s", buf);
}
