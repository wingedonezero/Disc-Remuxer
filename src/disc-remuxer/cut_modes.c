/*
 * disc-remuxer: the cut editor's analysis modes (cut_modes.h). The steps,
 * numbers and log lines follow the MKV splitter's Python step by step;
 * where Python's arithmetic decides a result its way is kept: times in
 * whole microseconds as its timedelta keeps them, floor division and
 * remainder as its floats do them, sum() with Neumaier's compensation,
 * round() half to even, min() the first of equals, statistics.mean() and
 * stdev() exact (with 113-bit floats here).
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/err.h"
#include "cut_modes.h"

const char *const cut_mode_names[CUT_MODE_COUNT] = {
    "Time-based Grouping", "Pattern Recognition", "Statistical Gap Analysis", "Shortest Chapter Analysis",
    "Manual Episode Count", "Remove Chapters from End", "Before Chapters (Manual)", "After Timestamps (Manual)",
};
const char *const cut_mode_keys[CUT_MODE_COUNT] = {
    "time", "pattern", "gap", "shortest", "episodes", "remove_end", "chapters", "timestamps",
};

int cut_mode_of_key(const char *key)
{
    for (int m = 0; key && m < CUT_MODE_COUNT; m++)
        if (!strcmp(key, cut_mode_keys[m]))
            return m;
    return -1;
}

static const char TIMESTAMP_HELP[] = "Use HH:MM:SS[.mmm], MM:SS[.mmm], or a number with a unit (90s, 1500ms). "
                                     "A bare number is rejected on purpose - '90' could mean seconds or minutes.";

/* ---- the log: lines joined by "\n" ---- */

typedef struct Log {
    char  *s;
    size_t len, cap;
    int    lines, nomem;
} Log;

static void add(Log *l, const char *fmt, va_list ap)
{
    va_list aq;
    int n;

    va_copy(aq, ap);
    n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0 || l->nomem)
        return;
    if (l->len + n + 1 > l->cap) {
        size_t cap = (l->len + n + 1) * 2;
        char *s = realloc(l->s, cap);
        if (!s) {
            l->nomem = 1;
            return;
        }
        l->s   = s;
        l->cap = cap;
    }
    vsnprintf(l->s + l->len, n + 1, fmt, ap);
    l->len += n;
}

/* A part of the line being written. */
static void put(Log *l, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    add(l, fmt, ap);
    va_end(ap);
}

/* A new line. */
static void line(Log *l, const char *fmt, ...)
{
    va_list ap;

    if (l->lines++)
        put(l, "\n");
    va_start(ap, fmt);
    add(l, fmt, ap);
    va_end(ap);
}

/* The whole log replaced by an error's text (the splitter shows the
 * exception's message instead of its log). */
static void replace(Log *l, const char *text)
{
    l->len   = 0;
    l->lines = 0;
    if (l->s)
        l->s[0] = 0;
    line(l, "%s", text);
}

/* ---- Python's arithmetic ---- */

/* float // and % (CPython float_divmod). */
static void py_divmod(double vx, double wx, double *floordiv, double *rem)
{
    double mod = fmod(vx, wx), div = (vx - mod) / wx, fd;

    if (mod) {
        if ((wx < 0) != (mod < 0)) {
            mod += wx;
            div -= 1.0;
        }
    } else
        mod = copysign(0.0, wx);
    if (div) {
        fd = floor(div);
        if (div - fd > 0.5)
            fd += 1.0;
    } else
        fd = copysign(0.0, vx / wx);
    *floordiv = fd;
    *rem      = mod;
}

/* round() of a float: half to even. */
static double py_round(double x)
{
    double r = round(x);
    if (fabs(r - x) == 0.5)
        r = 2.0 * round(x / 2.0);
    return r;
}

/* timedelta(<unit>=v): whole microseconds (factor: microseconds per unit),
 * as CPython's timedelta constructor counts them. */
static int64_t py_td_us(double v, int64_t factor)
{
    double ip, fp = modf(v, &ip), left = 0, whole;
    int64_t x = (int64_t)ip * factor;

    if (fp != 0.0) {
        double d = (double)factor * fp, ip2, fp2 = modf(d, &ip2);
        x += (int64_t)ip2;
        left = fp2;
    }
    if (left) {
        whole = round(left);
        if (fabs(whole - left) == 0.5) {
            int odd = (int)(x & 1);
            whole = 2.0 * round((left + odd) * 0.5) - odd;
        }
        x += (int64_t)whole;
    }
    return x;
}

/* timedelta.total_seconds(). */
static double td_seconds(int64_t us)
{
    return (double)us / 1e6;
}

/* sum() of floats (CPython 3.12+: Neumaier's compensated sum). */
typedef struct PySum {
    double f, c;
} PySum;

static void sum_add(PySum *s, double x)
{
    double t = s->f + x;
    if (fabs(s->f) >= fabs(x))
        s->c += (s->f - t) + x;
    else
        s->c += (x - t) + s->f;
    s->f = t;
}

static double sum_result(const PySum *s)
{
    return s->c && isfinite(s->c) ? s->f + s->c : s->f;
}

/* statistics.mean() and stdev(): exact in Python (fractions); here in
 * 113-bit floats, where the sums of these few values are exact. */
static double py_mean(const double *x, int n)
{
    __float128 s = 0;
    for (int i = 0; i < n; i++)
        s += x[i];
    return (double)(s / n);
}

static double py_stdev(const double *x, int n)
{
    __float128 ss = 0, v, y;
    double seed;

    /* n * sum(x^2) - sum(x)^2 = the sum of (x_i - x_j)^2 over the pairs */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            __float128 d = (__float128)x[i] - x[j];
            ss += d * d;
        }
    v = ss / ((__float128)n * (n - 1));
    if (v <= 0)
        return 0.0;
    seed = sqrt((double)v);
    y = seed;
    for (int k = 0; k < 3; k++)
        y = (y + v / y) / 2;
    return (double)y;
}

/* str() of a float: the fewest digits that read back as it, written as
 * Python writes them (fixed from 1e-4 up to 1e16, with ".0" when whole). */
static const char *py_repr(double x, char *buf, int size)
{
    char e[40], digits[24];
    int p, exp, nd = 0;

    if (!isfinite(x) || x == 0) {
        snprintf(buf, size, isnan(x) ? "nan" : isinf(x) ? (x < 0 ? "-inf" : "inf") : signbit(x) ? "-0.0" : "0.0");
        return buf;
    }
    for (p = 1; p <= 17; p++) {
        snprintf(e, sizeof(e), "%.*e", p - 1, x);
        if (strtod(e, NULL) == x)
            break;
    }
    for (const char *c = e; *c && *c != 'e'; c++)
        if (isdigit((unsigned char)*c))
            digits[nd++] = *c;
    while (nd > 1 && digits[nd - 1] == '0')
        nd--;
    digits[nd] = 0;
    exp = atoi(strchr(e, 'e') + 1);
    buf[0] = 0;
    if (x < 0)
        snprintf(buf, size, "-");
    if (exp >= -4 && exp < 16) {
        if (exp >= 0) {
            for (int i = 0; i <= exp; i++)
                snprintf(buf + strlen(buf), size - strlen(buf), "%c", i < nd ? digits[i] : '0');
            snprintf(buf + strlen(buf), size - strlen(buf), ".%s", nd > exp + 1 ? digits + exp + 1 : "0");
        } else
            snprintf(buf + strlen(buf), size - strlen(buf), "0.%.*s%s", -exp - 1, "0000", digits);
    } else
        snprintf(buf + strlen(buf), size - strlen(buf), "%c%s%se%c%02d", digits[0], nd > 1 ? "." : "",
                 digits + 1, exp < 0 ? '-' : '+', abs(exp));
    return buf;
}

/* format_timestamp(): a time as "H:MM:SS.mmm". */
static const char *ts_text(int64_t us, char *buf, int size)
{
    double total = td_seconds(us), h, r, m, r2, s, sec;

    py_divmod(total, 3600, &h, &r);
    py_divmod(r, 60, &m, &r2);
    py_divmod(total, 60, &s, &sec);
    snprintf(buf, size, "%d:%02d:%06.3f", (int)h, (int)m, sec);
    return buf;
}

/* ---- the chapters as the splitter lists them ---- */

typedef struct Row {
    int         num;
    double      start, duration;     /* minutes */
    const char *title;
    char        start_str[40];
} Row;

static Row *chapter_rows(const CutModeInput *in, int64_t duration_us, int *nb)
{
    Row *rows = calloc(in->nb_chapters ? in->nb_chapters : 1, sizeof(*rows));

    *nb = 0;
    if (!rows)
        return NULL;
    for (int i = 0; i < in->nb_chapters; i++) {
        /* the chapter's start read from its "HH:MM:SS.nnnnnnnnn": whole us */
        int64_t start = in->chapters[i] / 1000, end = duration_us;
        Row *r = &rows[(*nb)++];
        if (i + 1 < in->nb_chapters)
            end = in->chapters[i + 1] / 1000;
        r->num      = i + 1;
        r->start    = td_seconds(start) / 60;
        r->duration = td_seconds(end - start) / 60;
        r->title    = in->names && in->names[i] ? in->names[i] : "";
        ts_text(start, r->start_str, sizeof(r->start_str));
    }
    return rows;
}

/* ---- the split points ---- */

static int add_point(CutModeResult *out, int num)
{
    int *p = realloc(out->chapters, (out->nb_chapters + 1) * sizeof(*p));
    if (!p)
        return DR_ERR(ENOMEM);
    out->chapters = p;
    out->chapters[out->nb_chapters++] = num;
    return 0;
}

static int by_int(const void *a, const void *b)
{
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

/* "[3, 7]" or "None". */
static void put_points(Log *l, const CutModeResult *out)
{
    if (!out->nb_chapters) {
        put(l, "None");
        return;
    }
    put(l, "[");
    for (int i = 0; i < out->nb_chapters; i++)
        put(l, i ? ", %d" : "%d", out->chapters[i]);
    put(l, "]");
}

/* ---- After Timestamps ---- */

static int all_digits(const char *s, int n)
{
    if (n <= 0)
        return 0;
    for (int i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

/* normalize_timestamp(): one typed time as "HH:MM:SS.nnnnnnnnn" and its
 * seconds. 0, or -1 with the reason in err. */
static int normalize_timestamp(const char *tok, char *canon, int csize, double *seconds, char *err, int esize)
{
    const char *c1 = strchr(tok, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL, *dot;
    int n = (int)strlen(tok);

    if (!n) {
        snprintf(err, esize, "empty timestamp");
        return -1;
    }
    if (c1 && !(c2 && strchr(c2 + 1, ':'))) {
        /* [hours:]minutes:seconds[.fraction] */
        const char *hs = c2 ? tok : NULL, *ms = c2 ? c1 + 1 : tok, *ss = c2 ? c2 + 1 : c1 + 1;
        int hl = c2 ? (int)(c1 - tok) : 0, ml = (int)((c2 ? c2 : c1) - ms), sl, fl = 0;
        dot = strchr(ss, '.');
        sl  = dot ? (int)(dot - ss) : (int)strlen(ss);
        if (dot)
            fl = (int)strlen(dot + 1);
        if ((!hs || all_digits(hs, hl)) && ml <= 2 && all_digits(ms, ml) && sl <= 2 && all_digits(ss, sl) &&
            (!dot || (fl <= 9 && all_digits(dot + 1, fl)))) {
            long long hours = hs ? strtoll(hs, NULL, 10) : 0, minutes = strtol(ms, NULL, 10),
                      secs = strtol(ss, NULL, 10);
            char frac[10] = "000000000";
            if (dot)
                memcpy(frac, dot + 1, fl);
            if (secs > 59) {
                snprintf(err, esize, "'%s' has a seconds field above 59", tok);
                return -1;
            }
            if (!hs) {
                hours   = (hours + minutes) / 60;
                minutes = minutes % 60;
            } else if (minutes > 59) {
                snprintf(err, esize, "'%s' has a minutes field above 59", tok);
                return -1;
            }
            *seconds = (double)(hours * 3600 + minutes * 60 + secs) + (double)strtoll(frac, NULL, 10) / 1e9;
            snprintf(canon, csize, "%02lld:%02lld:%02lld.%s", hours, minutes, secs, frac);
            return 0;
        }
    } else if (!c1) {
        /* a number with a unit: 90s, 1500ms, 250us */
        int ul = n >= 2 && (tolower((unsigned char)tok[n - 2]) == 'm' || tolower((unsigned char)tok[n - 2]) == 'u') &&
                         tolower((unsigned char)tok[n - 1]) == 's' ? 2 : tolower((unsigned char)tok[n - 1]) == 's';
        int vl = n - ul;
        const char *d = memchr(tok, '.', vl);
        int il = d ? (int)(d - tok) : vl;
        if (ul && all_digits(tok, il) && (!d || all_digits(d + 1, vl - il - 1))) {
            char num[64];
            double total, h, r, m, r2, s, sec, nanos;
            if (vl >= (int)sizeof(num))
                goto bad;
            memcpy(num, tok, vl);
            num[vl] = 0;
            total = strtod(num, NULL) *
                    (ul == 1 ? 1.0 : tolower((unsigned char)tok[n - 2]) == 'm' ? 1e-3 : 1e-6);
            py_divmod(total, 3600, &h, &r);
            py_divmod(r, 60, &m, &r2);
            py_divmod(total, 60, &s, &sec);
            nanos     = py_round((total - (double)(long long)total) * 1e9);
            *seconds  = total;
            snprintf(canon, csize, "%02lld:%02lld:%02lld.%09lld", (long long)h, (long long)m, (long long)sec,
                     (long long)nanos);
            return 0;
        }
    }
bad:
    snprintf(err, esize, "'%s' is not a timestamp. %s", tok, TIMESTAMP_HELP);
    return -1;
}

typedef struct Stamp {
    double seconds;
    char   canon[48];
    char  *raw;
    int    order;
} Stamp;

static int by_seconds(const void *a, const void *b)
{
    const Stamp *x = a, *y = b;
    if (x->seconds != y->seconds)
        return x->seconds < y->seconds ? -1 : 1;
    return x->order - y->order;      /* sorted() keeps equal ones in order */
}

static char *strip(char *s)
{
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static int timestamps(const CutModeInput *in, Log *l, CutModeResult *out)
{
    double total_min = in->duration ? (in->duration / 1e9) / 60 : 0.0;
    char *text = strdup(in->timestamps ? in->timestamps : ""), *p, *s, err[512], buf[40];
    Stamp *st = NULL;
    Row *rows = NULL;
    int nb_rows, nb = 0, ret = 0, sorted_differs = 0;

    if (!text)
        return DR_ERR(ENOMEM);
    line(l, "--- After Timestamps (Manual) ---");
    if (total_min)
        line(l, "  File duration: %.2f min (%s)", total_min,
             ts_text(py_td_us(in->duration / 1e9, 1000000), buf, sizeof(buf)));
    if (!(rows = chapter_rows(in, py_td_us(in->duration / 1000.0, 1), &nb_rows))) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    if (nb_rows) {
        line(l, "\n  Chapter starts, for reference (double-click one to use it):");
        for (int i = 0; i < nb_rows; i++)
            line(l, "    Chapter %-3d | Start %s%s%s", rows[i].num, rows[i].start_str, *rows[i].title ? "  " : "",
                 rows[i].title);
    } else
        line(l, "  (File has no chapters - that is fine for this mode.)");
    if (!*strip(text)) {
        line(l, "\n  Enter one or more timestamps to split after. %s", TIMESTAMP_HELP);
        line(l, "  mkvmerge starts a new file once the stream reaches each timestamp,");
        line(l, "  at the next key frame - the same as mkvtoolnix's 'After specific timestamps'.");
        goto end;
    }
    /* the list: split at commas, new lines and semicolons */
    free(text);
    if (!(text = strdup(in->timestamps)) || !(st = calloc(strlen(in->timestamps) + 1, sizeof(*st)))) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    for (p = text; (s = strsep(&p, ",\n;"));) {
        char *t = strip(s);
        if (!*t)
            continue;
        if (normalize_timestamp(t, st[nb].canon, sizeof(st[nb].canon), &st[nb].seconds, err, sizeof(err)) < 0) {
            line(l, "\n❌ %s", err);
            goto end;
        }
        st[nb].raw   = t;
        st[nb].order = nb;
        nb++;
    }
    line(l, "\n--- Split points ---");
    qsort(st, nb, sizeof(*st), by_seconds);
    /* the order changed when a token now stands where another stood */
    {
        char **was = malloc((nb ? nb : 1) * sizeof(*was));
        if (!was) {
            ret = DR_ERR(ENOMEM);
            goto end;
        }
        for (int i = 0; i < nb; i++)
            was[st[i].order] = st[i].raw;
        for (int i = 0; i < nb; i++)
            sorted_differs |= strcmp(was[i], st[i].raw) != 0;
        free(was);
    }
    if (sorted_differs)
        line(l, "  ⚠️ Timestamps were not in ascending order and have been sorted - mkvmerge consumes them in "
                "order, so an out-of-order entry would never be reached.");
    {
        int kept = 0;
        for (int i = 0; i < nb; i++) {
            int seen = 0;
            for (int k = 0; k < kept && !seen; k++)
                seen = !strcmp(st[k].canon, st[i].canon);
            if (seen) {
                line(l, "  ⚠️ Duplicate timestamp %s ignored.", st[i].raw);
                continue;
            }
            st[kept++] = st[i];
        }
        nb = kept;
    }
    for (int i = 0; i < nb; i++) {
        double sec = st[i].seconds;
        char note[128] = "";
        if (sec <= 0)
            snprintf(note, sizeof(note), "  ⚠️ at 0:00 - produces an empty first file");
        else if (total_min && sec / 60 >= total_min)
            snprintf(note, sizeof(note), "  ⚠️ at or past the end of the file (%.2f min) - mkvmerge will not split "
                     "here", total_min);
        line(l, "  Split %d: %s  (%.2f min)%s", i + 1, st[i].canon, sec / 60, note);
    }
    if (total_min) {
        double prev = 0.0;
        line(l, "\n  📊 Resulting part durations: ");
        for (int i = 0; i <= nb; i++) {
            double b = i < nb ? st[i].seconds / 60 : total_min;
            put(l, i ? ", %.2f" : "%.2f", b - prev);
            prev = b;
        }
        put(l, " min");
    }
    line(l, "\n--- Final Step: Finalizing Split Points ---");
    line(l, "Final split points (timestamps to split AFTER): ");
    for (int i = 0; i < nb; i++)
        put(l, i ? ", %s" : "%s", st[i].canon);
    line(l, "\n✅ Total Parts: %d", nb + 1);
    if (nb && !(out->stamps = calloc(nb, sizeof(*out->stamps)))) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < nb; i++) {
        if (!(out->stamps[i] = strdup(st[i].canon))) {
            ret = DR_ERR(ENOMEM);
            goto end;
        }
        out->nb_stamps++;
    }
end:
    free(rows);
    free(st);
    free(text);
    return ret;
}

/* ---- the chapter modes ---- */

/* Step 2 of the content modes: the chapters longer than the minimum (main
 * content), sorted. 0, or 1 when there is none (logged: the analysis ends
 * there), < 0 no memory. */
static int main_content(const CutModeInput *in, Log *l, const Row *rows, int nb_rows, int **mains, int *nb_mains)
{
    char r[40];

    *nb_mains = 0;
    line(l, "\n--- Step 2: Finding Main Content (Min Duration > %s min) ---", py_repr(in->min_minutes, r, sizeof(r)));
    if (!(*mains = malloc((nb_rows ? nb_rows : 1) * sizeof(**mains))))
        return DR_ERR(ENOMEM);
    for (int i = 0; i < nb_rows; i++)
        if (rows[i].duration > in->min_minutes)
            (*mains)[(*nb_mains)++] = rows[i].num;
    if (!*nb_mains) {
        line(l, "❌ No chapters found longer than %s minutes.", r);
        return 1;
    }
    qsort(*mains, *nb_mains, sizeof(**mains), by_int);
    line(l, "Found potential main content chapters: ");
    for (int i = 0; i < *nb_mains; i++)
        put(l, i ? ", %d" : "%d", (*mains)[i]);
    return 0;
}

static int is_main(const int *mains, int nb, int num)
{
    for (int i = 0; i < nb; i++)
        if (mains[i] == num)
            return 1;
    return 0;
}

static const Row *row_of(const Row *rows, int nb, int num)
{
    for (int i = 0; i < nb; i++)
        if (rows[i].num == num)
            return &rows[i];
    return NULL;
}

/* Time-based Grouping. 0; 1: Python stopped with an error (the log is its
 * text). */
static int time_based(const CutModeInput *in, Log *l, const Row *rows, int nb_rows, double total_min,
                      CutModeResult *out)
{
    double target = in->target, min_episode = target * 0.5, anchor = 0, prev = 0.0;
    const Row **picks = NULL;
    int n_eps, equal = 0, prev_num = 1, ret = 0;

    line(l, "\n--- Step 2 (Time-based): Snapping to Chapter Boundaries ---");
    line(l, "  Total duration: %.2f min | Target episode length: %.2f min", total_min, target);
    line(l, "  Estimated episodes: %.2f", total_min / target);
    /* the disc authored as N equal episodes: every k * (total / N) on a
     * chapter boundary within 0.05 min */
    n_eps = (int)py_round(total_min / target);
    if (n_eps >= 2) {
        double fit_tol = 0.05;
        anchor = total_min / n_eps;
        if (!(picks = calloc(n_eps, sizeof(*picks))))
            return DR_ERR(ENOMEM);
        equal = 1;
        for (int k = 1; k < n_eps; k++) {
            const Row *near = NULL;
            if (nb_rows < 2) {
                free(picks);
                replace(l, "min() iterable argument is empty");
                return 1;
            }
            for (int i = 1; i < nb_rows; i++)
                if (!near || fabs(rows[i].start - k * anchor) < fabs(near->start - k * anchor))
                    near = &rows[i];
            if (fabs(near->start - k * anchor) > fit_tol) {
                line(l, "  Equal-division check: no boundary within %.2f min of %.2f min (closest: Chapter %d at "
                        "%.2f) — disc is not evenly authored.", fit_tol, k * anchor, near->num, near->start);
                equal = 0;
                break;
            }
            picks[k - 1] = near;
        }
        for (int a = 0; equal && a < n_eps - 1; a++)
            for (int b = a + 1; b < n_eps - 1; b++)
                if (picks[a]->num == picks[b]->num)
                    equal = 0;
    }
    if (equal) {
        int m = (int)anchor;
        double s = (anchor - m) * 60;
        line(l, "  ✅ Disc divides evenly into %d episodes of %.2f min (%d:%04.1f) — every boundary matches exactly. "
                "Using equal-parts splits.", n_eps, anchor, m, s);
        for (int k = 1; k < n_eps && ret >= 0; k++) {
            line(l, "  Split %d: Chapter %d start at %.2f min (%+.3f min from exact).", k, picks[k - 1]->num,
                 picks[k - 1]->start, picks[k - 1]->start - k * anchor);
            ret = add_point(out, picks[k - 1]->num);
        }
    }
    free(picks);
    while (!equal && ret >= 0) {
        double ideal = prev + target, remaining;
        const Row *best = NULL, *runner = NULL;
        for (int i = 0; i < nb_rows; i++)
            if (rows[i].num > prev_num && rows[i].start >= prev + min_episode &&
                (!best || fabs(rows[i].start - ideal) < fabs(best->start - ideal)))
                best = &rows[i];
        if (!best)
            break;
        /* a near tie said, for checking by eye */
        for (int i = 0; i < nb_rows; i++)
            if (rows[i].num > prev_num && rows[i].start >= prev + min_episode && rows[i].num != best->num &&
                (!runner || fabs(rows[i].start - ideal) < fabs(runner->start - ideal)))
                runner = &rows[i];
        if (runner && fabs(runner->start - ideal) - fabs(best->start - ideal) < 0.5)
            line(l, "  ⚠️ Near-tie: Chapter %d at %.2f min was almost as close as Chapter %d — worth double-checking "
                    "this split.", runner->num, runner->start, best->num);
        remaining = total_min - best->start;
        if (remaining < min_episode) {
            line(l, "  Remaining %.2f min after Chapter %d is under %.2f min — keeping it with the last episode.",
                 remaining, best->num, min_episode);
            break;
        }
        line(l, "  Split %d: ideal at %.2f min → snapped to Chapter %d start (%.2f min, %+.2f min off). Episode "
                "duration: %.2f min.", out->nb_chapters + 1, ideal, best->num, best->start, best->start - ideal,
             best->start - prev);
        ret      = add_point(out, best->num);
        prev     = best->start;
        prev_num = best->num;
    }
    if (ret < 0)
        return ret;
    if (out->nb_chapters) {
        double *d = malloc((out->nb_chapters + 1) * sizeof(*d)), b = 0.0;
        if (!d)
            return DR_ERR(ENOMEM);
        for (int k = 0; k <= out->nb_chapters; k++) {
            double e = k < out->nb_chapters ? row_of(rows, nb_rows, out->chapters[k])->start : total_min;
            d[k] = e - b;
            b    = e;
        }
        line(l, "\n  📊 Episode durations: ");
        for (int k = 0; k <= out->nb_chapters; k++)
            put(l, k ? ", %.2f" : "%.2f", d[k]);
        put(l, " min");
        line(l, "     Average: %.2f min (last episode may run long by design)", py_mean(d, out->nb_chapters + 1));
        free(d);
    } else
        line(l, "  File is not long enough for more than one episode at this target — no splits.");
    return 0;
}

static int fuzzy_match(const char *a, const char *b, int n)
{
    int miss = 0;
    for (int i = 0; i < n; i++)
        miss += a[i] != b[i];
    return miss <= 1;
}

static int pattern(const CutModeInput *in, Log *l, const Row *rows, int nb_rows, const int *mains, int nb_mains,
                   CutModeResult *out)
{
    char *sig = malloc(nb_rows + 1), best[16] = "", r[40];
    double best_cov = 0;
    int len = nb_rows, ret = 0;

    if (!sig)
        return DR_ERR(ENOMEM);
    line(l, "\n--- Step 3 (Pattern Recognition): Enhanced Pattern Analysis ---");
    for (int i = 0; i < nb_rows; i++)
        sig[i] = is_main(mains, nb_mains, rows[i].num) ? 'L' : rows[i].duration < 2.5 ? 'S' : 'M';
    sig[len] = 0;
    line(l, "  Generated Signature: %s", sig);
    line(l, "  L = Long/Main content (>%s min)", py_repr(in->min_minutes, r, sizeof(r)));
    line(l, "  M = Medium content (2.5-%s min)", r);
    line(l, "  S = Short content (<2.5 min)");
    line(l, "\n--- Step 4 (Pattern Recognition): Finding Repeating Pattern with Fuzzy Matching ---");
    for (int p = len / 2 < 10 ? len / 2 : 10; p > 0; p--) {
        int exact = 1, fuzzy = 0, total;
        double coverage;
        for (int i = p; i < len - p + 1; i += p) {
            if (!memcmp(sig + i, sig, p)) {
                exact++;
                if (fuzzy > 0) {
                    exact += fuzzy;
                    fuzzy = 0;
                }
            } else if (fuzzy_match(sig, sig + i, p)) {
                fuzzy++;
                line(l, "  ~ Fuzzy match at position %d: '%.*s' ≈ '%.*s'", i, p, sig + i, p, sig);
            } else
                break;
        }
        total    = exact + fuzzy;
        coverage = (double)(total * p) / len;
        if (coverage >= 0.75 && total >= 2 &&
            (coverage > best_cov || (coverage >= best_cov * 0.95 && p > (int)strlen(best)))) {
            memcpy(best, sig, p);
            best[p]  = 0;
            best_cov = coverage;
            line(l, "  Candidate pattern: '%s' (length: %d, consecutive matches: %d, coverage: %.1f%%)", best, p,
                 total, coverage * 100);
            if (coverage >= 0.90 && p >= 3)
                break;
        }
    }
    if (*best) {
        int nl = 0, bl = (int)strlen(best);
        line(l, "  ✅ Found repeating pattern: '%s' (length: %d, coverage: %.1f%%)", best, bl, best_cov * 100);
        for (int i = 0; i < bl; i++)
            nl += best[i] == 'L';
        if (nl > 1)
            line(l, "  ℹ️ Pattern contains %d main content chapters. Treating as a single multi-part episode.", nl);
        for (int i = bl; i < len && ret >= 0; i += bl)
            ret = add_point(out, i + 1);
    } else
        line(l, "  ❌ Could not determine a confident repeating pattern (even with fuzzy matching).");
    free(sig);
    return ret;
}

static int gap_analysis(Log *l, const Row *rows, int nb_rows, const int *mains, int nb_mains, CutModeResult *out)
{
    double *dur = malloc((nb_mains ? nb_mains : 1) * sizeof(*dur));
    int *end = malloc((nb_mains ? nb_mains : 1) * sizeof(*end)), nb = 0, ret = 0;

    if (!dur || !end) {
        free(dur);
        free(end);
        return DR_ERR(ENOMEM);
    }
    line(l, "\n--- Step 3 (Statistical Gap): Finding Episode Gaps ---");
    for (int i = 0; i + 1 < nb_mains; i++)
        if (mains[i + 1] > mains[i] + 1) {
            PySum s = { 0, 0 };
            for (int k = 0; k < nb_rows; k++)
                if (rows[k].num >= mains[i] + 1 && rows[k].num <= mains[i + 1] - 1)
                    sum_add(&s, rows[k].duration);
            dur[nb] = sum_result(&s);
            end[nb] = mains[i + 1] - 1;
            nb++;
        }
    for (int g = 0; g < nb; g++)
        line(l, "  Gap ending at chapter %d. Duration: %.2f min.", end[g], dur[g]);
    if (nb > 1) {
        double mean = py_mean(dur, nb), sd = py_stdev(dur, nb), threshold = mean + (1.5 * sd);
        line(l, "\n  Gap stats: Avg=%.2f, StdDev=%.2f", mean, sd);
        line(l, "  Identifying splits as gaps > threshold of %.2f min.", threshold);
        for (int g = 0; g < nb && ret >= 0; g++)
            if (dur[g] > threshold)
                ret = add_point(out, end[g] + 1);
    } else if (nb == 1) {
        line(l, "  Only one gap found, assuming it's the split point.");
        ret = add_point(out, end[0] + 1);
    }
    free(dur);
    free(end);
    return ret;
}

static int shortest(Log *l, const Row *rows, int nb_rows, const int *mains, int nb_mains, CutModeResult *out)
{
    int *first = malloc(nb_mains * sizeof(*first)), *last = malloc(nb_mains * sizeof(*last)), nb = 0, ret = 0;

    if (!first || !last) {
        free(first);
        free(last);
        return DR_ERR(ENOMEM);
    }
    line(l, "\n--- Step 3 (Shortest Chapter): Grouping and Finding Splits ---");
    /* runs of consecutive chapters */
    for (int i = 0; i < nb_mains; i++) {
        if (i && mains[i] == mains[i - 1] + 1)
            last[nb - 1] = mains[i];
        else {
            first[nb] = last[nb] = mains[i];
            nb++;
        }
    }
    line(l, "Detected main content groups: [");
    for (int g = 0, i = 0; g < nb; g++) {
        put(l, g ? ", [" : "[");
        for (int k = 0; i < nb_mains && mains[i] <= last[g]; i++, k++)
            put(l, k ? ", %d" : "%d", mains[i]);
        put(l, "]");
    }
    put(l, "]");
    if (nb > 1) {
        for (int g = 0; g + 1 < nb && ret >= 0; g++) {
            const Row *s = NULL;
            for (int k = 0; k < nb_rows; k++)
                if (rows[k].num >= last[g] + 1 && rows[k].num < first[g + 1] &&
                    (!s || rows[k].duration < s->duration))
                    s = &rows[k];
            if (!s)
                continue;
            line(l, "  Shortest chapter in gap is Chapter %d (%.2f min). Splitting before Chapter %d.", s->num,
                 s->duration, s->num + 1);
            ret = add_point(out, s->num + 1);
        }
    } else
        line(l, "  Only one main content group found — nothing to split. If this file should contain multiple "
                "episodes, try lowering 'Min Content Duration' (episodes chaptered as Part A/Part B may have no "
                "single chapter above the threshold).");
    free(first);
    free(last);
    return ret;
}

typedef struct Gap {
    int size, start, order;
} Gap;

static int by_size_desc(const void *a, const void *b)
{
    const Gap *x = a, *y = b;
    if (x->size != y->size)
        return y->size - x->size;
    return x->order - y->order;      /* sorted(reverse=True) keeps equal ones in order */
}

static int episodes(const CutModeInput *in, Log *l, const int *mains, int nb_mains, CutModeResult *out)
{
    Gap *g = malloc((nb_mains ? nb_mains : 1) * sizeof(*g));
    int nb = 0, ret = 0, want = in->episodes - 1;

    if (!g)
        return DR_ERR(ENOMEM);
    line(l, "\n--- Step 3 (Manual): Clustering into %d Episodes ---", in->episodes);
    for (int i = 0; i + 1 < nb_mains; i++)
        if (mains[i + 1] - mains[i] > 1) {
            g[nb] = (Gap){ mains[i + 1] - mains[i], mains[i + 1], nb };
            nb++;
        }
    if (nb < want) {
        line(l, "⚠️ Warning: Found %d gaps, but expected %d.", nb, want);
        for (int i = 0; i < nb && ret >= 0; i++)
            ret = add_point(out, g[i].start);
    } else {
        qsort(g, nb, sizeof(*g), by_size_desc);
        for (int i = 0; i < want && ret >= 0; i++)
            ret = add_point(out, g[i].start);
        qsort(out->chapters, out->nb_chapters, sizeof(*out->chapters), by_int);
    }
    free(g);
    return ret;
}

int cut_mode_run(const CutModeInput *in, CutModeResult *out)
{
    Log l = { 0 };
    Row *rows = NULL;
    int *mains = NULL, nb_rows = 0, nb_mains = 0, ret = 0, stop = 0;
    int64_t duration_us;
    double total_min;

    memset(out, 0, sizeof(*out));
    if (in->mode == CUT_MODE_TIMESTAMPS) {
        ret = timestamps(in, &l, out);
        goto end;
    }
    if (!in->nb_chapters) {
        line(&l, "❌ No chapters found in this file.");
        goto end;
    }
    if (!in->duration) {
        line(&l, "❌ Could not determine container duration.");
        goto end;
    }
    duration_us = py_td_us(in->duration / 1000.0, 1);
    total_min   = td_seconds(duration_us) / 60;
    line(&l, "--- Step 1: Chapter Analysis ---");
    if (!(rows = chapter_rows(in, duration_us, &nb_rows))) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < nb_rows; i++)
        line(&l, "  Chapter %-3d | Duration: %.2f minutes%s%s%s", rows[i].num, rows[i].duration,
             *rows[i].title ? " (" : "", rows[i].title, *rows[i].title ? ")" : "");

    switch (in->mode) {
    case CUT_MODE_TIME:
        stop = ret = time_based(in, &l, rows, nb_rows, total_min, out);
        break;
    case CUT_MODE_PATTERN:
    case CUT_MODE_GAP:
    case CUT_MODE_SHORTEST:
    case CUT_MODE_EPISODES:
        if ((stop = ret = main_content(in, &l, rows, nb_rows, &mains, &nb_mains)))
            break;
        ret = in->mode == CUT_MODE_PATTERN ? pattern(in, &l, rows, nb_rows, mains, nb_mains, out) :
              in->mode == CUT_MODE_GAP     ? gap_analysis(&l, rows, nb_rows, mains, nb_mains, out) :
              in->mode == CUT_MODE_SHORTEST ? shortest(&l, rows, nb_rows, mains, nb_mains, out) :
                                              episodes(in, &l, mains, nb_mains, out);
        break;
    case CUT_MODE_REMOVE_END: {
        int n = in->from_end, total = nb_rows, before;
        line(&l, "\n--- Step 2 (Remove from End): Removing last %d chapter(s) ---", n);
        line(&l, "  Total chapters: %d", total);
        if (n >= total) {
            line(&l, "❌ Cannot remove %d chapters from a file with only %d chapters.", n, total);
            stop = 1;
            break;
        }
        before = total - n + 1;
        line(&l, "  Splitting before chapter %d to keep chapters 1-%d", before, before - 1);
        ret = add_point(out, before);
        break;
    }
    case CUT_MODE_CHAPTERS: {
        int *t = malloc((in->nb_ticked ? in->nb_ticked : 1) * sizeof(*t));
        if (!t) {
            ret = DR_ERR(ENOMEM);
            break;
        }
        line(&l, "\n--- Step 2 (Before Chapters): Chapter starts ---");
        line(&l, "  Tick chapters in the Chapters table to split before them, then Generate Command.");
        line(&l, "  Same as mkvtoolnix's 'Before chapters' mode: mkvmerge splits at the first");
        line(&l, "  key frame at or after the chapter's start time.");
        for (int i = 0; i < nb_rows; i++)
            line(&l, "  Chapter %-3d | Start %s | %.2f min%s%s%s", rows[i].num, rows[i].start_str, rows[i].duration,
                 *rows[i].title ? "  " : "", rows[i].title,
                 rows[i].start <= 0 ? "   (starts at 0:00 - mkvmerge never splits here)" : "");
        if (in->nb_ticked)
            memcpy(t, in->ticked, in->nb_ticked * sizeof(*t));
        qsort(t, in->nb_ticked, sizeof(*t), by_int);
        for (int i = 0; i < in->nb_ticked && ret >= 0; i++) {
            const Row *r;
            if (i && t[i] == t[i - 1])
                continue;
            r = row_of(rows, nb_rows, t[i]);
            if (r && r->start > 0)
                ret = add_point(out, t[i]);
            else
                line(&l, "  ⚠️ Chapter %d cannot be a split point - skipped.", t[i]);
        }
        free(t);
        break;
    }
    default:
        break;
    }
    if (ret < 0 || stop)
        goto end;
    line(&l, "\n--- Final Step: Finalizing Split Points ---");
    line(&l, "Final split points (chapter numbers to split BEFORE): ");
    put_points(&l, out);
    line(&l, "\n✅ Total Episodes Found: %d", out->nb_chapters + 1);
end:
    free(rows);
    free(mains);
    if (ret > 0)
        ret = 0;
    if (ret >= 0 && l.nomem)
        ret = DR_ERR(ENOMEM);
    out->log = l.s ? l.s : strdup("");
    if (ret < 0 || !out->log) {
        cut_mode_result_free(out);
        return ret < 0 ? ret : DR_ERR(ENOMEM);
    }
    return 0;
}

void cut_mode_result_free(CutModeResult *r)
{
    free(r->log);
    free(r->chapters);
    for (int i = 0; i < r->nb_stamps; i++)
        free(r->stamps[i]);
    free(r->stamps);
    memset(r, 0, sizeof(*r));
}

/* ---- the target duration field ---- */

/* int() of a text: optional sign and digits, spaces around. */
static int py_int(const char *s, long long *v)
{
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    if (!*s || (!isdigit((unsigned char)*s) && !((*s == '+' || *s == '-') && isdigit((unsigned char)s[1]))))
        return -1;
    errno = 0;
    *v = strtoll(s, &e, 10);
    while (isspace((unsigned char)*e))
        e++;
    return *e || errno ? -1 : 0;
}

int cut_mode_target(const char *text, double *minutes, char *err, int size)
{
    char *t = strdup(text ? text : ""), *s, *c;
    long long m, sec;
    int ret = 0;

    if (!t) {
        snprintf(err, size, "no memory");
        return -1;
    }
    s = strip(t);
    if (!*s) {
        snprintf(err, size, "Target episode duration is empty. Use mm:ss, e.g. 23:40.");
        ret = -1;
    } else if ((c = strchr(s, ':'))) {
        *c = 0;
        if (py_int(s, &m) < 0 || py_int(c + 1, &sec) < 0) {
            *c = ':';
            snprintf(err, size, "Invalid target duration '%s'. Use mm:ss, e.g. 23:40.", s);
            ret = -1;
        } else
            *minutes = (double)m + (double)sec / 60.0;
    } else {
        char *e;
        int ok = !strpbrk(s, "xXpP");
        *minutes = strtod(s, &e);
        while (isspace((unsigned char)*e))
            e++;
        if (!ok || *e || e == s) {
            snprintf(err, size, "Invalid target duration '%s'. Use mm:ss, e.g. 23:40.", s);
            ret = -1;
        }
    }
    if (!ret && *minutes <= 0) {
        snprintf(err, size, "Target episode duration must be greater than zero.");
        ret = -1;
    }
    free(t);
    return ret;
}

void cut_mode_start_text(int64_t ns, char *buf, int size)
{
    ts_text(ns / 1000, buf, size);
}

void cut_mode_target_text(double minutes, char *buf, int size)
{
    int m = (int)minutes, s = (int)py_round((minutes - m) * 60);
    if (s == 60) {
        m++;
        s = 0;
    }
    snprintf(buf, size, "%d:%02d", m, s);
}
