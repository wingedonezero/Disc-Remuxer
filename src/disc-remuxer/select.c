/*
 * disc-remuxer: which tracks of a title are ripped (see select.h).
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/avutil.h>

#include "log.h"
#include "msg.h"
#include "select.h"

enum { W_ALL, W_VIDEO, W_AUDIO, W_SUBTITLE, W_FORCED, W_LANGUAGE, W_NUMBER };

typedef struct Step {
    int  add;           /* + select, - leave out */
    int  word;          /* W_* */
    int  number;        /* W_NUMBER */
    char lang[4];       /* W_LANGUAGE: ISO 639-2 as the disc info has it */
} Step;

/* The title a value is for ("3:..."), -1 every title; *rest: its steps. */
static int spec_title(const char *spec, const char **rest)
{
    const char *p = spec;

    while (isdigit((unsigned char)*p))
        p++;
    if (p > spec && *p == ':') {
        *rest = p + 1;
        return atoi(spec);
    }
    *rest = spec;
    return -1;
}

/* One step "+word" / "-word" (len bytes) into st; 0, or -1 (logged). */
static int parse_step(const char *s, size_t len, Step *st)
{
    static const struct { const char *name; int word; } words[] = {
        { "all", W_ALL }, { "video", W_VIDEO }, { "audio", W_AUDIO }, { "subtitle", W_SUBTITLE },
        { "forced", W_FORCED },
    };
    char w[16];
    size_t i;

    while (len && isspace((unsigned char)*s))
        s++, len--;
    while (len && isspace((unsigned char)s[len - 1]))
        len--;
    if (len < 2 || (*s != '+' && *s != '-') || len - 1 >= sizeof(w))
        goto bad;
    memset(st, 0, sizeof(*st));
    st->add = *s == '+';
    memcpy(w, s + 1, len - 1);
    w[len - 1] = 0;
    for (i = 0; i < sizeof(words) / sizeof(*words); i++)
        if (!strcmp(w, words[i].name)) {
            st->word = words[i].word;
            return 0;
        }
    for (i = 0; w[i] && isdigit((unsigned char)w[i]); i++)
        ;
    if (!w[i]) {
        st->word   = W_NUMBER;
        st->number = atoi(w);
        return 0;
    }
    if (strlen(w) == 2 || strlen(w) == 3) {
        const char *code = disc_language(w);
        if (code) {
            st->word = W_LANGUAGE;
            snprintf(st->lang, sizeof(st->lang), "%s", code);
            return 0;
        }
    }
bad:
    log_msg(MSG_TRACK_SELECTION, LOG_ERROR, "--tracks: '%.*s' is no step: use + or - and all, video, audio, subtitle, "
            "forced, a language code (eng, fr, ...) or a track number", (int)len, s);
    return -1;
}

/* Calls fn for each step of a value's steps; 0, or -1 (logged). */
static int each_step(const char *steps, void (*fn)(void *opaque, const Step *st), void *opaque)
{
    const char *p = steps;

    if (!*p) {
        log_msg(MSG_TRACK_SELECTION, LOG_ERROR, "--tracks: no steps given");
        return -1;
    }
    for (;;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        Step st;
        if (parse_step(p, len, &st) < 0)
            return -1;
        if (fn)
            fn(opaque, &st);
        if (!end)
            return 0;
        p = end + 1;
    }
}

int tracks_check(const char *spec)
{
    const char *steps;

    spec_title(spec, &steps);
    return each_step(steps, NULL, NULL);
}

int tracks_unknown_title(const char *const *specs, int nb_specs, int nb_titles)
{
    for (int i = 0; i < nb_specs; i++) {
        const char *steps;
        int t = spec_title(specs[i], &steps);
        if (t >= nb_titles)
            return t;
    }
    return -1;
}

typedef struct Apply {
    const DiscTitleInfo *t;
    uint8_t             *keep;
    int                  named;        /* the value names this title ("3:...") */
    int                  video_kept;   /* track 0 kept against a step: said once */
} Apply;

static int matches(const DiscStreamInfo *k, int index, const Step *st)
{
    /* a 3-D title's second view is put back only by its number (or
     * --second-view), taken out by any step that names it */
    if (k->second_view && st->add && st->word != W_NUMBER)
        return 0;
    switch (st->word) {
    case W_ALL:      return 1;
    case W_VIDEO:    return k->type == AVMEDIA_TYPE_VIDEO;
    case W_AUDIO:    return k->type == AVMEDIA_TYPE_AUDIO;
    case W_SUBTITLE: return k->type == AVMEDIA_TYPE_SUBTITLE;
    case W_FORCED:   return k->forced;
    case W_LANGUAGE: return !strcmp(k->language, st->lang);
    default:         return index == st->number;
    }
}

static void apply_step(void *opaque, const Step *st)
{
    Apply *a = opaque;

    /* a number in a value for every title: titles without that track are
     * left as they are */
    if (a->named && st->word == W_NUMBER && st->number >= a->t->nb_streams)
        log_msg(MSG_TRACK_SELECTION, LOG_WARNING, "--tracks: title #%d has no track %d (it has %d)", a->t->index,
                st->number, a->t->nb_streams);
    for (int i = 0; i < a->t->nb_streams; i++) {
        if (!matches(&a->t->streams[i], i, st))
            continue;
        if (!i && !st->add) {
            if (!a->video_kept++)
                log_msg(MSG_TRACK_SELECTION, LOG_INFO, "Title #%d: track 0, the video, is always ripped",
                        a->t->index);
            continue;
        }
        a->keep[i] = st->add;
    }
}

int tracks_select(const DiscTitleInfo *t, int second_view, const char *const *specs, int nb_specs, uint8_t *keep)
{
    Apply a = { t, keep, 0, 0 };
    int n = 0;

    for (int i = 0; i < t->nb_streams; i++)
        keep[i] = !t->streams[i].second_view || second_view;
    for (int i = 0; i < nb_specs; i++) {
        const char *steps;
        int title = spec_title(specs[i], &steps);
        a.named = title >= 0;
        if (title < 0 || title == t->index)
            each_step(steps, apply_step, &a);
    }
    for (int i = 0; i < t->nb_streams; i++)
        n += keep[i];
    return n;
}
