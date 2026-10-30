/*
 * disc-remuxer's engine (see engine.h): the disc opened once, the overview
 * of its titles, the choice of titles and tracks, a run's folder, log and
 * titles.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#include "base/bytes.h"
#include "base/err.h"
#include "base/log.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/str.h"
#include "disc/disc.h"
#include "analyze/analyze.h"
#include "base/msg.h"
#include "names.h"
#include "base/settings.h"
#include "codecs.h"
#include "discs.h"
#include "emit.h"
#include "engine.h"
#include "events.h"
#include "select.h"
#include "writer.h"
#include "cuts.h"
#include "cut_preview.h"
#include "timeline/disctl.h"


/* Library state (key caches, key files) stays next to the program. */
static void config_dirs(void)
{
    char dir[PATH_MAX], sub[PATH_MAX + 32];

    snprintf(dir, sizeof(dir), "%s/keydb", program_dir());
    mkdir(dir, 0777);   /* the AACS key files (setting aacs.key_files) */
    snprintf(dir, sizeof(dir), "%s/.config", program_dir());
    mkdir(dir, 0777);
    snprintf(sub, sizeof(sub), "%s/dvdcss", dir);
    setenv("DVDCSS_CACHE", sub, 1);
    setenv("XDG_CONFIG_HOME", dir, 1);
    snprintf(sub, sizeof(sub), "%s/cache", dir);
    setenv("XDG_CACHE_HOME", sub, 1);
    snprintf(sub, sizeof(sub), "%s/data", dir);
    setenv("XDG_DATA_HOME", sub, 1);
}

static void hms(char *buf, size_t size, int64_t us)
{
    int64_t t = (us + 500000) / 1000000;
    snprintf(buf, size, "%d:%02d:%02d", (int)(t / 3600), (int)(t / 60 % 60), (int)(t % 60));
}

/* A linked stream's index for JSON, or null (-1: none). */
static const char *json_index(char *buf, size_t size, int v)
{
    if (v < 0)
        return "null";
    snprintf(buf, size, "%d", v);
    return buf;
}

/* A stream detail for JSON: the number, or null when not known (0). */
static const char *json_num(char *buf, size_t size, int64_t v)
{
    if (!v)
        return "null";
    snprintf(buf, size, "%"PRId64, v);
    return buf;
}

/* A stream's roles as words (sep between them), "" none. */
static const char *roles_text(int roles, const char *sep, int quoted, char *buf, size_t size)
{
    static const struct { int bit; const char *word; } r[] = {
        { DISC_ROLE_COMMENTARY, "commentary" }, { DISC_ROLE_ALT_COMMENTARY, "alternate commentary" },
        { DISC_ROLE_VISUALLY_IMPAIRED, "visually impaired" }, { DISC_ROLE_LARGE, "large" },
        { DISC_ROLE_CHILDREN, "children" }, { DISC_ROLE_FORCED, "forced" },
    };
    *buf = 0;
    for (size_t i = 0; i < sizeof(r) / sizeof(*r); i++)
        if (roles & r[i].bit)
            snprintf(buf + strlen(buf), size - strlen(buf), "%s%s%s%s", *buf ? sep : "", quoted ? "\"" : "",
                     r[i].word, quoted ? "\"" : "");
    return buf;
}

void engine_track_pieces(const DiscTitleInfo *t, int i, char *codec, size_t csize, char *body, size_t bsize,
                         char *link_out, size_t lsize)
{
    const DiscStreamInfo *k = &t->streams[i];
    char extra[160] = "", link[96] = "", det[160] = "", a[24], r[48], l[64], b[24];

    if (k->type == DISC_MEDIA_SUBTITLE && k->width)
        snprintf(extra, sizeof(extra), "  %dx%d", k->width, k->height);
    if (k->forced)
        snprintf(link, sizeof(link), "  (forced subtitles of track %d)", k->from_track);
    else if (k->second_view)
        snprintf(link, sizeof(link), "  (3-D second view of track %d)", k->from_track);
    else if (k->from_track >= 0)
        snprintf(link, sizeof(link), "  (taken out of track %d)", k->from_track);
    else if (k->inner_track >= 0)
        snprintf(link, sizeof(link), "  (track %d is taken out of it)", k->inner_track);
    if (k->second_layer)
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "  with its second layer");
    if (k->roles) {
        char rt[96];
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "  (%s)",
                 roles_text(k->roles, ", ", 0, rt, sizeof(rt)));
    }
    if (k->offset_sequence >= 0)
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "  3-D offset sequence %d",
                 k->offset_sequence);
    /* what the stream is: its codec in full, then its details */
    if (k->type == DISC_MEDIA_VIDEO && k->width && !k->forced)
        snprintf(det, sizeof(det), "  %dx%d %s %s", k->width, k->height, aspect_text(k, a, sizeof(a)),
                 frame_rate_text(k, r, sizeof(r)));
    if (k->type == DISC_MEDIA_AUDIO && k->channels) {
        snprintf(det, sizeof(det), "  %s %d Hz", layout_text(k, l, sizeof(l)), k->sample_rate);
        if (k->bits && codec_lossless(k))
            snprintf(det + strlen(det), sizeof(det) - strlen(det), " %d-bit", k->bits);
        if (*bit_rate_text(k->bit_rate, b, sizeof(b)) && !codec_lossless(k))
            snprintf(det + strlen(det), sizeof(det) - strlen(det), " %s", b);
    }
    codec_description(k, k->second_view && k->from_track >= 0 ? &t->streams[k->from_track] : NULL, codec, csize);
    snprintf(body, bsize, "%s%s", det, extra);
    snprintf(link_out, lsize, "%s", link);
}

void engine_track_parts(const DiscTitleInfo *t, int i, char *codec, size_t csize, char *rest, size_t rsize)
{
    char body[512], link[96];

    engine_track_pieces(t, i, codec, csize, body, sizeof(body), link, sizeof(link));
    snprintf(rest, rsize, "%s%s", body, link);
}

const char *engine_track_text(const DiscTitleInfo *t, int i, int kept, char *buf, size_t size)
{
    char codec[96], rest[512];

    engine_track_parts(t, i, codec, sizeof(codec), rest, sizeof(rest));
    snprintf(buf, size, "%s%s%s", codec, rest, kept ? "" : "  not selected");
    return buf;
}

void engine_title_file_name(const DiscTitleInfo *t, char *out, size_t size)
{
    title_name(setting_text("output.file_name_template"), t->name, NULL, NULL, t->index, out, size);
}

const char *engine_track_extension(const DiscStreamInfo *k)
{
    return track_extension(k);
}

/* One title's line (and, with detail, its files and streams) plus its JSON
 * record. */
/* A title's angle for the detail line: "; angle 3 (main); the disc plays
 * angles 3, 1, 2 in that order; cell 1 shared by all angles" or "; angle 1,
 * main is angle 3 (#18)". */
/* fmt's text appended to buf (cut at size). */
static void cat(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void cat(char *buf, size_t size, const char *fmt, ...)
{
    size_t n = strlen(buf);
    va_list vl;

    if (n + 1 >= size)
        return;
    va_start(vl, fmt);
    vsnprintf(buf + n, size - n, fmt, vl);
    va_end(vl);
}

static void angle_text(const DiscTitleInfo *t, char *buf, size_t size)
{
    snprintf(buf, size, "; angle %d", t->angle);
    if (t->main_angle == t->angle) {
        cat(buf, size, " (main)");
        if (t->angle_order[0] && t->angle_order[1]) {
            cat(buf, size, "; the disc plays angles ");
            for (int k = 0; k < 9 && t->angle_order[k]; k++)
                cat(buf, size, "%s%d", k ? ", " : "", t->angle_order[k]);
            cat(buf, size, " in that order");
        }
    } else if (t->main_angle) {
        cat(buf, size, ", main is angle %d", t->main_angle);
        if (t->main_title >= 0)
            cat(buf, size, " (#%d)", t->main_title);
    }
    if (t->shared_cells)
        cat(buf, size, "; cell%s %s shared by all angles", strchr(t->shared_cells, ',') ? "s" : "",
                    t->shared_cells);
}

/* The title as info lists it: its line (and with detail its files, tracks
 * and parts), then its record (DR_EVENT_TITLE). */
static void title_overview(const DiscTitleInfo *t, int detail, const uint8_t *keep)
{
    char dur[32], size[32], pt[192] = "";
    int nv = 0, na = 0, ns = 0;

    hms(dur, sizeof(dur), t->duration);
    size_text(size, sizeof(size), t->size);
    for (int i = 0; i < t->nb_streams; i++) {
        const DiscStreamInfo *k = &t->streams[i];
        nv += k->type == DISC_MEDIA_VIDEO;
        na += k->type == DISC_MEDIA_AUDIO;
        ns += k->type == DISC_MEDIA_SUBTITLE;
    }
    if (t->nb_parts)
        snprintf(pt, sizeof(pt), ", %d parts", t->nb_parts);
    if (t->angle)
        snprintf(pt + strlen(pt), sizeof(pt) - strlen(pt), ", angle %d%s", t->angle,
                 t->angle == t->main_angle ? " (main)" : "");
    if (t->menu_name)
        snprintf(pt + strlen(pt), sizeof(pt) - strlen(pt), ", menu name %s", t->menu_name);
    log_text(LOG_INFO, "  #%-2d %-40s %9s  %-6s %d audio, %d subtitle%s, %d chapter%s%s", t->index,
             t->name ? t->name : "", dur,
             nv ? codec_label(t->streams[0].codec, 0) : "-", na, ns, ns == 1 ? "" : "s",
             t->nb_chapters, t->nb_chapters == 1 ? "" : "s", pt);
    if (detail) {
        char list[1024] = "";
        for (int i = 0; i < t->nb_files; i++)
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", i ? ", " : "", t->files[i]);
        char from[64] = "", ang[256] = "";
        if (t->source)
            snprintf(from, sizeof(from), "from %s%s", t->source, t->nb_files ? ": " : "");
        if (t->angle)
            angle_text(t, ang, sizeof(ang));
        log_text(LOG_INFO, "       %s, %s%s%s%s%s%s", size, from, list, ang, t->language[0] ? ", language " : "",
                 t->language, t->selected ? "" : ", not selected (not the main angle: name it to rip it)");
        for (int i = 0; i < t->nb_streams; i++) {
            const DiscStreamInfo *k = &t->streams[i];
            char line[512];
            log_text(LOG_INFO, "       %-2d %-8s %-3s %s", i, disc_media_name(k->type),
                     k->language[0] ? k->language : "   ", engine_track_text(t, i, keep[i], line, sizeof(line)));
        }
        for (int p = 0; p < t->nb_parts; p++) {
            const DiscPartInfo *q = &t->parts[p];
            char pd[32], ps[32], pl[512] = "";
            hms(pd, sizeof(pd), q->duration);
            hms(ps, sizeof(ps), q->start);
            for (int f = 0; f < q->nb_files; f++)
                snprintf(pl + strlen(pl), sizeof(pl) - strlen(pl), "%s%s", f ? ", " : "", t->files[q->first_file + f]);
            log_text(LOG_INFO, "       part %-2d at %s  %9s  %s, %d chapter%s", p + 1, ps, pd, pl, q->nb_chapters,
                     q->nb_chapters == 1 ? "" : "s");
        }
    }
    {
        DrEvent ev = { .kind = DR_EVENT_TITLE };
        ev.u.title = (DrEventTitle){ t, keep };
        engine_event(&ev);
    }
}

void engine_title_json(const DiscTitleInfo *t, const uint8_t *keep)
{
    char files[2048] = "", js[512], jl[16], extra[512] = "";
    DrStr streams = DR_STR_INIT, parts = DR_STR_INIT;

    for (int i = 0; i < t->nb_streams; i++) {
        const DiscStreamInfo *k = &t->streams[i];
        char jc[64], jsl[16], n1[24], n2[24], n3[24], n4[24], n5[24], n6[24], ji[24], jf[24], jo[24];
        char d[96], jd[128], pl[48], jp[64], fr[48], jr[64], as[24], ja[32], lt[64], jl2[96], dv[48], jv[64], fq[32];
        char lm[24], rl[128], jn[96], jm[48], je[16], an[32], jan[48], jh[32], jco[128], jma[320], nc1[24], nc2[24];
        const char *lname = k->language[0] ? disc_language_name(k->language) : NULL, *mkv = disc_mkv_codec_id(k);
        (void)dv; (void)fr;
        dr_str_catf(&streams,
                 "%s{\"index\":%d,\"kind\":\"%s\",\"codec\":%s,\"language\":%s,\"inner_track\":%s,"
                 "\"from_track\":%s,\"forced\":%s,\"second_view\":%s,\"second_layer\":%s,\"offset_sequence\":%s,\"width\":%s,"
                 "\"height\":%s,\"channels\":%s,\"sample_rate\":%s,\"bits\":%s,\"bit_rate\":%s,"
                 "\"description\":%s,\"profile\":%s,\"dolby_vision\":%s,\"frame_rate\":%s,\"aspect\":%s,"
                 "\"layout\":%s,\"layout_mask\":%s,\"roles\":[%s],\"selected\":%s,\"language_name\":%s,"
                 "\"codec_id\":%s,\"file_type\":%s,\"name\":%s,\"hdr\":%s,\"colour\":%s,\"mastering\":%s,"
                 "\"max_cll\":%s,\"max_fall\":%s}",
                 i ? "," : "", i, disc_media_name(k->type),
                 json_str(jc, sizeof(jc), codec_short(k)),
                 json_str(jsl, sizeof(jsl), k->language), json_index(ji, sizeof(ji), k->inner_track),
                 json_index(jf, sizeof(jf), k->from_track), k->forced ? "true" : "false",
                 k->second_view ? "true" : "false", k->second_layer ? "true" : "false",
                 json_index(jo, sizeof(jo), k->offset_sequence),
                 json_num(n1, sizeof(n1), k->width),
                 json_num(n2, sizeof(n2), k->height), json_num(n3, sizeof(n3), k->channels),
                 json_num(n4, sizeof(n4), k->sample_rate), json_num(n5, sizeof(n5), k->bits),
                 json_num(n6, sizeof(n6), k->bit_rate),
                 json_str(jd, sizeof(jd), codec_description(k, k->second_view && k->from_track >= 0 ? &t->streams[k->from_track] : NULL, d, sizeof(d))),
                 *codec_profile_level(k, pl, sizeof(pl)) ? json_str(jp, sizeof(jp), pl) : "null",
                 k->dolby_vision[0] ? json_str(jv, sizeof(jv), k->dolby_vision) : "null",
                 k->frame_rate.num > 0 ? (snprintf(fq, sizeof(fq), "%d/%d", k->frame_rate.num, k->frame_rate.den),
                                          json_str(jr, sizeof(jr), fq)) : "null",
                 *aspect_text(k, as, sizeof(as)) ? json_str(ja, sizeof(ja), as) : "null",
                 *layout_text(k, lt, sizeof(lt)) && k->layout ? json_str(jl2, sizeof(jl2), lt) : "null",
                 json_num(lm, sizeof(lm), (int64_t)k->layout), roles_text(k->roles, ",", 1, rl, sizeof(rl)),
                 keep[i] ? "true" : "false", lname ? json_str(jn, sizeof(jn), lname) : "null",
                 mkv ? json_str(jm, sizeof(jm), mkv) : "null", json_str(je, sizeof(je), track_extension(k)),
                 *audio_name(k, an, sizeof(an)) ? json_str(jan, sizeof(jan), an) : "null",
                 k->hdr[0] ? json_str(jh, sizeof(jh), k->hdr) : "null",
                 k->colour[0] ? json_str(jco, sizeof(jco), k->colour) : "null",
                 k->mastering[0] ? json_str(jma, sizeof(jma), k->mastering) : "null",
                 json_num(nc1, sizeof(nc1), k->max_cll), json_num(nc2, sizeof(nc2), k->max_fall));
    }
    for (int i = 0; i < t->nb_files; i++) {
        char jf[300];
        snprintf(files + strlen(files), sizeof(files) - strlen(files), "%s%s", i ? "," : "",
                 json_str(jf, sizeof(jf), t->files[i]));
    }
    if (t->source) {
        char jsrc[96];
        snprintf(extra, sizeof(extra), ",\"source\":%s", json_str(jsrc, sizeof(jsrc), t->source));
    }
    if (t->menu_name) {
        char jmn[300];
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"menu_name\":%s",
                 json_str(jmn, sizeof(jmn), t->menu_name));
    }
    if (t->angle) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"angle\":%d", t->angle);
        if (t->main_angle) {
            char mt[24] = "null";
            if (t->main_title >= 0)
                snprintf(mt, sizeof(mt), "%d", t->main_title);
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"main_angle\":%d,\"main_title\":%s",
                     t->main_angle, mt);
        }
        if (t->angle_order[0] && t->angle_order[1])
            for (int k = 0; k < 9 && t->angle_order[k]; k++)
                snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "%s%d%s",
                         k ? "," : ",\"angle_order\":[", t->angle_order[k],
                         k == 8 || !t->angle_order[k + 1] ? "]" : "");
        if (t->shared_cells) {
            char jsc[128];
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"shared_cells\":%s",
                     json_str(jsc, sizeof(jsc), t->shared_cells));
        }
    }
    /* parts: the part number (from 1) as --split names its files, where it starts in the title */
    for (int p = 0; p < t->nb_parts; p++) {
        const DiscPartInfo *q = &t->parts[p];
        dr_str_catf(&parts, "%s{\"part\":%d,\"start\":%.3f,\"duration\":%.3f,\"chapters\":%d,\"files\":[",
                   p ? "," : ",\"parts\":[", p + 1, q->start / (double)1000000.0,
                   q->duration / (double)1000000.0, q->nb_chapters);
        for (int f = 0; f < q->nb_files; f++) {
            char jf[300];
            dr_str_catf(&parts, "%s%s", f ? "," : "", json_str(jf, sizeof(jf), t->files[q->first_file + f]));
        }
        dr_str_catf(&parts, "]}%s", p + 1 == t->nb_parts ? "]" : "");
    }
    /* what the format part tells more of the title */
    for (int p = 0; p < t->nb_props; p++) {
        char jn[128], jv[512];
        dr_str_catf(&parts, "%s%s:%s%s", p ? "," : ",\"properties\":{", json_str(jn, sizeof(jn), t->props[p].name),
                   json_str(jv, sizeof(jv), t->props[p].value), p + 1 == t->nb_props ? "}" : "");
    }
    log_json("title", "\"index\":%d,\"name\":%s,\"duration\":%.3f,\"chapters\":%d,\"size\":%"PRId64",\"language\":%s,"
             "\"selected\":%s,\"files\":[%s],\"tracks\":[%s]%s%s", t->index,
             json_str(js, sizeof(js), t->name ? t->name : ""), t->duration / (double)1000000.0, t->nb_chapters,
             t->size, json_str(jl, sizeof(jl), t->language), t->selected ? "true" : "false", files, streams.str, extra,
             parts.str);
    dr_str_free(&streams);
    dr_str_free(&parts);
}

struct EngineDisc {
    DrDisc          *disc;
    const DiscInfo  *info;
    char             source[PATH_MAX];
    uint8_t        **keeps;      /* each title's tracks to rip */
    char            *rip;        /* per title: 1 ripped (the selection) */
    char            *split;      /* per title: 1 written as parts (as chosen; the setting adds to it) */
    char           **cut;        /* per title: the cuts asked for (cuts.h cut_parse()), NULL none */
    CutIndex       **index;      /* per title: the cut editor's index (engine_cut_index()), NULL not read */
};

static int parse_titles(const char *spec, const DiscInfo *d, int only_selected, char *sel);
static int resolve_cuts(EngineDisc *e, int t, const char *items, DrDiscCut **cuts, int *nb, int64_t *end,
                        int64_t **starts);
static int cut_value(const char *v, int *title, const char **items);
static int cut_check(int t, const CutTrackBytes *sums, int nb_sums, const CutTrackBytes *title, int nb_title);

/* Each title's tracks to rip (select.h) into keeps[title] (freed with
 * free_keeps()); 0, or -1 (logged). */
static int title_choices(const DiscInfo *d, const EngineChoice *c, uint8_t ***keeps)
{
    char *second;
    int t;

    *keeps = calloc(d->nb_titles ? d->nb_titles : 1, sizeof(**keeps));
    second = calloc(d->nb_titles ? d->nb_titles : 1, 1);
    if (!*keeps || !second)
        goto fail;
    if ((t = tracks_unknown_title(c->tracks, c->nb_tracks, d->nb_titles)) >= 0) {
        log_msg(MSG_TITLE_MISSING, LOG_ERROR, "--tracks: title #%d does not exist (the disc has %d)", t,
                d->nb_titles);
        goto fail;
    }
    if (c->second && parse_titles(c->second, d, 0, second) < 0)
        goto fail;
    if (setting_bool("bluray.second_view"))
        memset(second, 1, d->nb_titles);
    for (t = 0; t < d->nb_titles; t++) {
        if (!((*keeps)[t] = calloc(d->titles[t].nb_streams ? d->titles[t].nb_streams : 1, 1)))
            goto fail;
        tracks_select(&d->titles[t], second[t], c->tracks, c->nb_tracks, (*keeps)[t]);
    }
    free(second);
    return 0;
fail:
    free(second);
    return -1;
}

static atomic_int cancelled;

void engine_cancel(void)
{
    atomic_store(&cancelled, 1);
}

int engine_cancelled(void)
{
    return atomic_load(&cancelled);
}

static void free_keeps(uint8_t **keeps, int nb)
{
    for (int t = 0; keeps && t < nb; t++)
        free(keeps[t]);
    free(keeps);
}

int engine_disc_open(const char *source, const EngineChoice *c, EngineDisc **out)
{
    EngineDisc *e = calloc(1, sizeof(*e));
    int ret = DR_ERR(ENOMEM);

    *out = NULL;
    if (!e || (ret = open_disc(source, &e->disc)) < 0 || !(e->info = dr_disc_info(e->disc))) {
        if (ret == DR_ERR(ENOKEY))
            log_msg(MSG_SOURCE_UNSUPPORTED, LOG_ERROR, "%s is encrypted (AACS) and the key files have no key for "
                    "it (its disc ID is in the AACS line above): add its key to a KEYDB.cfg (setting "
                    "aacs.key_files)", source);
        else
            log_msg(MSG_SOURCE_UNSUPPORTED, LOG_ERROR, "%s cannot be read as a disc this program supports (%s); "
                    "DVD-Video, HD DVD and Blu-ray are supported", source, dr_err_text(ret < 0 ? ret : DR_ERR_BUG));
        engine_disc_close(&e);
        return -1;
    }
    snprintf(e->source, sizeof(e->source), "%s", source);
    if (title_choices(e->info, c, &e->keeps) < 0) {
        engine_disc_close(&e);
        return -1;
    }
    /* the selection to start with: every title but the angles other than
     * the main one; none written as parts */
    if (!(e->rip = calloc(e->info->nb_titles ? e->info->nb_titles : 1, 1)) ||
        !(e->split = calloc(e->info->nb_titles ? e->info->nb_titles : 1, 1))) {
        engine_disc_close(&e);
        return -1;
    }
    for (int t = 0; t < e->info->nb_titles; t++)
        e->rip[t] = e->info->titles[t].selected;
    if (!(e->cut = calloc(e->info->nb_titles ? e->info->nb_titles : 1, sizeof(*e->cut)))) {
        engine_disc_close(&e);
        return -1;
    }
    for (int k = 0; c && k < c->nb_cuts; k++) {
        const char *items;
        int t;
        if (cut_value(c->cuts[k], &t, &items) < 0 || t >= e->info->nb_titles) {
            log_msg(MSG_TITLE_MISSING, LOG_ERROR, "--cut %s: the disc has no title #%d (it has %d)", c->cuts[k], t,
                    e->info->nb_titles);
            engine_disc_close(&e);
            return -1;
        }
        free(e->cut[t]);
        if (!(e->cut[t] = strdup(items))) {
            engine_disc_close(&e);
            return -1;
        }
    }
    *out = e;
    return 0;
}

/* ---- the selection ---- */

static int title_ok(const EngineDisc *d, int t)
{
    return d && t >= 0 && t < d->info->nb_titles;
}

int engine_title_rip(const EngineDisc *d, int t)
{
    return title_ok(d, t) && d->rip[t];
}

int engine_set_title_rip(EngineDisc *d, int t, int on)
{
    if (!title_ok(d, t))
        return -1;
    d->rip[t] = !!on;
    return 0;
}

int engine_titles_ripped(const EngineDisc *d)
{
    int n = 0;

    for (int t = 0; d && t < d->info->nb_titles; t++)
        n += d->rip[t];
    return n;
}

int engine_select_titles(EngineDisc *d, const char *spec)
{
    return parse_titles(spec, d->info, 1, d->rip);
}

int engine_title_split(const EngineDisc *d, int t)
{
    return title_ok(d, t) && d->split[t];
}

int engine_set_title_split(EngineDisc *d, int t, int on)
{
    if (!title_ok(d, t))
        return -1;
    d->split[t] = !!on;
    return 0;
}

int engine_split_titles(EngineDisc *d, const char *spec)
{
    return parse_titles(spec, d->info, 0, d->split);
}

const CutIndex *engine_cut_index(EngineDisc *d, int t)
{
    CutIndex *ix;

    if (!title_ok(d, t))
        return NULL;
    if (!d->index && !(d->index = calloc(d->info->nb_titles, sizeof(*d->index))))
        return NULL;
    if (d->index[t])
        return d->index[t];
    if (!(ix = calloc(1, sizeof(*ix))))
        return NULL;
    /* the whole title: every picture and the chapters as a rip places them */
    if (cut_index_read(d->disc, t, -1, NULL, 0, ix) < 0) {
        free(ix);
        return NULL;
    }
    return d->index[t] = ix;
}

int engine_cut_editor_open(EngineDisc *d, int t, CutPreview **p)
{
    const CutIndex *ix = engine_cut_index(d, t);

    *p = NULL;
    if (!ix)
        return -1;
    return cut_preview_open(p, d->disc, t, ix, (int64_t)setting_int("cuts.preview_cache_mb") << 20) < 0 ? -1 : 0;
}

void engine_cut_editor_close(EngineDisc *d, int t, CutPreview **p)
{
    cut_preview_close(p);
    /* only the cut list stays: the index goes too */
    if (title_ok(d, t) && d->index && d->index[t]) {
        cut_index_free(d->index[t]);
        free(d->index[t]);
        d->index[t] = NULL;
    }
}

const char *engine_title_cuts(const EngineDisc *d, int t)
{
    return title_ok(d, t) && d->cut ? d->cut[t] : NULL;
}

int engine_set_title_cuts(EngineDisc *d, int t, const char *items)
{
    CutRequest *reqs;
    const char *bad;
    char *copy = NULL;
    int nb;

    if (!title_ok(d, t) || !d->cut)
        return -1;
    if (items && *items) {
        if (cut_parse(items, &reqs, &nb, &bad) < 0)
            return -1;
        free(reqs);
        if (!(copy = strdup(items)))
            return -1;
    }
    free(d->cut[t]);
    d->cut[t] = copy;
    return 0;
}

int engine_track_kept(const EngineDisc *d, int t, int i)
{
    return title_ok(d, t) && i >= 0 && i < d->info->titles[t].nb_streams && d->keeps[t][i];
}

int engine_set_track_kept(EngineDisc *d, int t, int i, int on)
{
    if (!title_ok(d, t) || i < 0 || i >= d->info->titles[t].nb_streams)
        return -1;
    if (i == 0)                         /* the video is always ripped */
        return on ? 0 : -1;
    tracks_set(&d->info->titles[t], d->keeps[t], i, on);
    return 0;
}

int engine_apply_tracks(EngineDisc *d, int t, const char *steps)
{
    if (!title_ok(d, t))
        return -1;
    return tracks_apply(&d->info->titles[t], steps, d->keeps[t]);
}

const DiscInfo *engine_disc_info(const EngineDisc *d)
{
    return d->info;
}

const uint8_t *engine_disc_tracks(const EngineDisc *d, int t)
{
    return title_ok(d, t) ? d->keeps[t] : NULL;
}

void engine_disc_close(EngineDisc **pd)
{
    EngineDisc *d = *pd;

    if (!d)
        return;
    if (d->info)
        free_keeps(d->keeps, d->info->nb_titles);
    free(d->rip);
    free(d->split);
    for (int t = 0; d->cut && d->info && t < d->info->nb_titles; t++)
        free(d->cut[t]);
    free(d->cut);
    for (int t = 0; d->index && d->info && t < d->info->nb_titles; t++)
        if (d->index[t]) {
            cut_index_free(d->index[t]);
            free(d->index[t]);
        }
    free(d->index);
    dr_disc_close(&d->disc);
    free(d);
    *pd = NULL;
}

void engine_disc_json(const DiscInfo *d)
{
    char js1[64], js2[256], js3[64], js4[1024], jname[1100 + 16 * 300];

    jname[0] = 0;
    if (d->name)
        snprintf(jname, sizeof(jname), ",\"name\":%s", json_str(js4, sizeof(js4), d->name));
    if (d->menu_name) {
        char jmn[300];
        snprintf(jname + strlen(jname), sizeof(jname) - strlen(jname), ",\"menu_name\":%s",
                 json_str(jmn, sizeof(jmn), d->menu_name));
    }
    /* what the format part tells more of the disc */
    for (int p = 0; p < d->nb_props; p++) {
        char jn[96], jv[192];
        snprintf(jname + strlen(jname), sizeof(jname) - strlen(jname), "%s%s:%s", p ? "," : ",\"properties\":{",
                 json_str(jn, sizeof(jn), d->props[p].name), json_str(jv, sizeof(jv), d->props[p].value));
    }
    if (d->nb_props)
        snprintf(jname + strlen(jname), sizeof(jname) - strlen(jname), "}");
    log_json("disc", "\"kind\":%s,\"label\":%s,\"filesystem\":%s,\"encrypted\":%s,\"titles\":%d%s",
             json_str(js1, sizeof(js1), d->kind), json_str(js2, sizeof(js2), d->label ? d->label : ""),
             json_str(js3, sizeof(js3), d->filesystem ? d->filesystem : ""), d->encrypted ? "true" : "false",
             d->nb_titles, jname);
}

void engine_disc_overview(const EngineDisc *e, int detail)
{
    const DiscInfo *d = e->info;
    char revs[32] = "", label[256] = "";

    if (d->udf_revision)
        snprintf(revs, sizeof(revs), " %x.%02x", (unsigned)(d->udf_revision >> 8), (unsigned)(d->udf_revision & 0xff));
    /* a disc folder has no volume label: none is said */
    if (d->label && *d->label)
        snprintf(label, sizeof(label), " '%s'", d->label);
    log_msg(MSG_SOURCE, LOG_INFO, "Disc: %s%s, %s%s, %s, %d title%s", d->kind, label,
            d->filesystem ? d->filesystem : "?", revs, d->encrypted ? "encrypted" : "not encrypted",
            d->nb_titles, d->nb_titles == 1 ? "" : "s");
    if (d->name)
        log_msg(MSG_SOURCE, LOG_INFO, "Disc name: %s", d->name);
    if (d->menu_name)
        log_msg(MSG_SOURCE, LOG_INFO, "Disc menu name: %s", d->menu_name);
    /* what the format part tells more of the disc (region, protection, checks, ...) */
    for (int p = 0; p < d->nb_props; p++)
        log_msg(MSG_SOURCE, LOG_INFO, "Disc %s: %s", d->props[p].name, d->props[p].value);
    {
        DrEvent ev = { .kind = DR_EVENT_DISC };
        ev.u.disc = (DrEventDisc){ d };
        engine_event(&ev);
    }
    for (int t = 0; t < d->nb_titles; t++)
        title_overview(&d->titles[t], detail, e->keeps[t]);
}

/* "all" | "3" | "1,3-5" into a 0/1 list of d's titles; all with
 * only_selected: the selected titles (an angle other than the main one is
 * not; each one left out said); returns the count selected, or -1 (logged). */
static int parse_titles(const char *spec, const DiscInfo *d, int only_selected, char *sel)
{
    int count = 0, nb_titles = d->nb_titles;
    const char *p = spec;

    memset(sel, 0, nb_titles);
    if (!strcmp(spec, "all")) {
        for (int t = 0; t < nb_titles; t++) {
            sel[t] = !only_selected || d->titles[t].selected;
            count += sel[t];
            if (!sel[t])
                log_msg(MSG_TITLE_SELECTION, LOG_INFO, "Title #%d is angle %d, not the main angle %d: not ripped "
                        "with all (name it to rip it)", t, d->titles[t].angle, d->titles[t].main_angle);
        }
        return count;
    }
    while (*p) {
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p)
            goto bad;
        p = end;
        if (*p == '-') {
            b = strtol(p + 1, &end, 10);
            if (end == p + 1)
                goto bad;
            p = end;
        }
        if (a > b || a < 0)
            goto bad;
        for (long t = a; t <= b; t++) {
            if (t >= nb_titles) {
                log_msg(MSG_TITLE_MISSING, LOG_ERROR, "Title #%ld does not exist (the disc has %d: #0 to #%d)", t,
                        nb_titles, nb_titles - 1);
                return -1;
            }
            count += !sel[t];
            sel[t] = 1;
        }
        if (*p == ',')
            p++;
        else if (*p)
            goto bad;
    }
    return count;
bad:
    log_msg(MSG_TITLE_SELECTION, LOG_ERROR, "Titles '%s': use all, a number, or a list like 1,3-5", spec);
    return -1;
}

static int mkdirs(const char *path)
{
    char tmp[PATH_MAX];

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    return mkdir(tmp, 0777) < 0 && errno != EEXIST ? -1 : 0;
}

void engine_output_root(char *out, size_t size)
{
    const char *root = setting_text("output.folder");

    if (root && *root)
        snprintf(out, size, "%s", root);
    else
        snprintf(out, size, "%s/output", program_dir());
}

void engine_clean_name(const char *name, char *out, size_t size)
{
    clean_name(name, 0, out, size);
}

void engine_unique_folder(const char *base, char *out, size_t size)
{
    struct stat st;

    snprintf(out, size, "%s", base);
    for (int n = 1; !stat(out, &st) && n < 1000; n++)
        snprintf(out, size, "%s_%03d", base, n);
}

void engine_output_folder(const char *source, char *out, size_t size)
{
    char name[512], root[PATH_MAX], base[PATH_MAX + 520];

    engine_disc_name(source, name, sizeof(name));
    engine_output_root(root, sizeof(root));
    snprintf(base, sizeof(base), "%s/%s", root, name);
    engine_unique_folder(base, out, size);
}

/* A run: its folder and log files (named after the disc), the log's first
 * lines. 0, or 1 (logged). */
static int run_begin(const char *source, const char *out_dir, char *logp, size_t size)
{
    char name[512], dbgp[PATH_MAX + 600];

    if (mkdirs(out_dir) < 0) {
        log_msg(MSG_OUTPUT_FOLDER, LOG_ERROR, "The output folder %s cannot be created: %s", out_dir, strerror(errno));
        return 1;
    }
    engine_disc_name(source, name, sizeof(name));
    snprintf(logp, size, "%s/%s.log", out_dir, name);
    snprintf(dbgp, sizeof(dbgp), "%s/%s_debug.log", out_dir, name);
    if (log_open_files(logp, setting_bool("log.debug_file") ? dbgp : NULL) < 0) {
        log_msg(MSG_LOG_FILE, LOG_ERROR, "The log %s cannot be written: %s", logp, strerror(errno));
        return 1;
    }
    log_reset_counts();
    log_msg(MSG_PROGRAM_START, LOG_INFO, "disc-remuxer %s — %s", DISC_REMUXER_VERSION, source);
    settings_log_changed();
    log_msg(MSG_OUTPUT_FOLDER, LOG_INFO, "Output folder: %s", out_dir);
    return 0;
}

/* The run's titles (the disc's selection: those ripped, those written as
 * parts) from the open disc, then the summary; the log files closed. 0, or
 * 1 (a title failed). */
static int run_titles(EngineDisc *e, const char *out_dir, const char *logp)
{
    const char *sel = e->rip, *split = e->split;
    const DiscInfo *di = e->info;
    int nb = di->nb_titles, count = 0, ordinal = 0, failed = 0, done = 0, disk_full = 0, errors, warnings;
    int64_t damaged = 0, damaged_bytes = 0;
    char *spl = calloc(nb ? nb : 1, 1);
    char **ranges = NULL;               /* every title's damaged ranges, for the summary */
    int nb_ranges = 0;
    DrDiscCut *cuts = NULL;             /* the title's cuts (a title with cuts) */
    CutTrackBytes *cut_sums = NULL, *cut_title = NULL;  /* its parts' bytes added up; its last part's */
    int nb_cut_sums = 0, nb_cut_title = 0;
    int nb_cuts = 0;
    int64_t cut_end = 0, *cut_starts = NULL;
    uint8_t *vkeep = NULL;              /* a cut title's tracks: those a cut part carries */

    if (!spl) {
        log_close_files();
        return 1;
    }
    for (int t = 0; t < nb; t++) {
        count += !!sel[t];
        spl[t] = split && split[t];
    }
    /* which titles are written as parts: --split (these), the setting (every title) */
    for (int t = 0; t < nb; t++)
        if (spl[t] && sel[t] && !di->titles[t].nb_parts)
            log_msg(MSG_TITLE_SELECTION, LOG_INFO, "Title #%d has no parts (it plays no whole stream files back to "
                    "back): written whole", t);
    /* (not a title whose parts are written only when it is chosen: DVD clean breaks) */
    if (setting_bool("output.split_joined_titles"))
        for (int t = 0; t < nb; t++)
            spl[t] |= !di->titles[t].parts_on_request;
    for (int t = 0; t < nb; t++) {
        int parts = spl[t] ? di->titles[t].nb_parts : 0, part_failed = 0, part_stopped = 0;
        if (!sel[t])
            continue;
        if (engine_cancelled())
            break;
        ++ordinal;
        /* a title with cuts: where they fall first; its parts are the cut's */
        if (e->cut && e->cut[t]) {
            char dur[32];
            if (parts)
                log_msg(MSG_TITLE_SELECTION, LOG_INFO, "Title #%d is cut at the cuts asked for, not split at its "
                        "parts", t);
            hms(dur, sizeof(dur), di->titles[t].duration);
            log_step("=== Cuts: title #%d (%s) ===", t, dur);
            if (resolve_cuts(e, t, e->cut[t], &cuts, &nb_cuts, &cut_end, &cut_starts) || !nb_cuts ||
                !(vkeep = calloc(di->titles[t].nb_streams ? di->titles[t].nb_streams : 1, 1))) {
                log_msg(MSG_TITLE_FAILED, LOG_ERROR, "Title #%d is not ripped: not every cut asked for can be made",
                        t);
                failed++;
                free(cuts);
                free(cut_starts);
                cuts       = NULL;
                cut_starts = NULL;
                nb_cuts    = 0;
                continue;
            }
            /* the tracks a cut part carries so far; the others said */
            for (int i = 0; i < di->titles[t].nb_streams; i++) {
                const char *why = dr_disc_cut_left_out(e->disc, t, i);
                vkeep[i] = !i || (e->keeps[t][i] && !why);
                if (i && e->keeps[t][i] && why)
                    log_msg(MSG_CUTS, LOG_WARNING, "Title #%d: track %d (%s) is left out of its cut parts: %s", t, i,
                            codec_label(di->titles[t].streams[i].codec, di->titles[t].streams[i].profile), why);
            }
            parts = nb_cuts + 1;
        }
        for (int p = parts ? 0 : -1; p < parts && !engine_cancelled(); p++) {
            /* the second view: read when one of its tracks is selected */
            /* stream files: every title, and each part as a title of its own, in a folder of its name */
            TitleJob job = { e->disc, t, p, parts, !nb_cuts, nb_cuts ? vkeep : e->keeps[t], ordinal, count, out_dir,
                             1, cuts, nb_cuts, cut_end, cut_starts };
            TitleOutcome res;
            demux_title(&job, &res);
            /* each range under the title's (part's) number */
            damaged       += res.damaged_ranges;
            damaged_bytes += res.damaged_bytes;
            for (int i = 0; i < res.nb_damage; i++) {
                char *l = p >= 0 ? dr_asprintf_str("Title #%d part %d: %s", t, p + 1, res.damage[i]) :
                                   dr_asprintf_str("Title #%d: %s", t, res.damage[i]);
                if (!l || dr_array_add_ptr(&ranges, &nb_ranges, l) < 0)
                    dr_free(l);
            }
            /* a cut part: its tracks' bytes added up; the last part's title bytes are the whole title's */
            for (int i = 0; i < res.nb_cut; i++) {
                CutTrackBytes *s = NULL;
                for (int k = 0; k < nb_cut_sums && !s; k++)
                    if (cut_sums[k].number == res.cut[i].number)
                        s = &cut_sums[k];
                if (!s) {
                    CutTrackBytes *n = realloc(cut_sums, (nb_cut_sums + 1) * sizeof(*n));
                    if (!n)
                        break;
                    cut_sums = n;
                    s = &cut_sums[nb_cut_sums++];
                    *s = (CutTrackBytes){ res.cut[i].number, 0, 0, 0, 0, res.cut[i].pcm, res.cut[i].bytes_per_sample };
                }
                s->part += res.cut[i].part;
                s->left += res.cut[i].left;
            }
            if (nb_cuts && p == parts - 1 && res.nb_cut && (cut_title = malloc(res.nb_cut * sizeof(*cut_title)))) {
                memcpy(cut_title, res.cut, res.nb_cut * sizeof(*cut_title));
                nb_cut_title = res.nb_cut;
            }
            title_outcome_free(&res);
            /* a title stopped as asked is not written, not failed */
            part_stopped += res.stopped;
            part_failed  += res.failed && !res.stopped;
            disk_full    |= res.disk_full;
            if (disk_full)
                break;
        }
        /* a cut title: every track's parts add up to the title */
        if (nb_cuts && !part_failed && !part_stopped && !disk_full)
            part_failed += cut_check(t, cut_sums, nb_cut_sums, cut_title, nb_cut_title);
        free(cut_sums);
        free(cut_title);
        cut_sums    = cut_title = NULL;
        nb_cut_sums = nb_cut_title = 0;
        free(cuts);
        free(cut_starts);
        free(vkeep);
        cuts       = NULL;
        cut_starts = NULL;
        vkeep      = NULL;
        nb_cuts    = 0;
        failed += !!part_failed;
        done   += !part_failed && !part_stopped;
        /* a full disk: every title after it would fail the same way */
        if (disk_full)
            break;
    }
    free(spl);

    log_counts(&errors, &warnings);
    log_step("=== Summary ===");
    if (engine_cancelled())
        log_msg(MSG_SUMMARY, LOG_WARNING, "Stopped as asked: %d of %d title%s not written", count - done,
                count, count == 1 ? "" : "s");
    if (disk_full)
        log_msg(MSG_DISK_FULL, LOG_ERROR, "Stopped: the disk is full; %d of %d title%s not written (%d after the "
                "one that failed not tried)", count - done, count, count == 1 ? "" : "s", count - ordinal);
    log_msg(MSG_SUMMARY, failed ? LOG_ERROR : warnings ? LOG_INFO : LOG_OK,
            "%d title%s written, %d failed; %d warning%s, %d error%s", done, done == 1 ? "" : "s", failed, warnings,
            warnings == 1 ? "" : "s", errors, errors == 1 ? "" : "s");
    if (damaged) {
        char dz[32];
        log_msg(MSG_SUMMARY, LOG_INFO, "Damaged data left out: %"PRId64" range%s (%s)", damaged,
                damaged == 1 ? "" : "s", size_text(dz, sizeof(dz), damaged_bytes));
        /* each range a line of its own (no line is cut, however many) */
        for (int i = 0; i < nb_ranges; i++)
            log_msg(MSG_SUMMARY, LOG_INFO, "  %s", ranges[i]);
    }
    for (int i = 0; i < nb_ranges; i++)
        dr_free(ranges[i]);
    dr_free(ranges);
    log_msg(MSG_LOG_FILE, LOG_INFO, "Log: %s", logp);
    {
        DrEvent ev = { .kind = DR_EVENT_RUN_RESULT };
        ev.u.run = (DrEventRunResult){ done, failed, warnings, errors, engine_cancelled(), logp };
        engine_event(&ev);
    }
    log_close_files();
    return failed || engine_cancelled() ? 1 : 0;
}

int engine_disc_rip(EngineDisc *e, const char *out_dir)
{
    char folder[PATH_MAX], logp[PATH_MAX + 600];

    atomic_store(&cancelled, 0);
    if (!out_dir) {
        engine_output_folder(e->source, folder, sizeof(folder));
        out_dir = folder;
    }
    if (run_begin(e->source, out_dir, logp, sizeof(logp)))
        return 1;
    engine_disc_overview(e, 0);
    return run_titles(e, out_dir, logp);
}

/* Details rows as lines: a heading in brackets, a sub-heading and
 * "label: value" indented by their level. */
static void log_rows(EngineRows *r)
{
    for (int i = 0; i < r->nb; i++) {
        const EngineRow *w = &r->rows[i];
        int indent = 2 + 2 * w->level;
        if (w->kind == ENGINE_ROW_HEAD)
            log_text(LOG_INFO, "[%s]", w->label);
        else if (w->kind == ENGINE_ROW_SUB)
            log_text(LOG_INFO, "%*s-- %s", indent, "", w->label);
        else
            log_text(LOG_INFO, "%*s%s: %s", indent, "", w->label, w->value);
    }
    engine_rows_free(r);
}

void engine_details(const EngineDisc *e, const char *source)
{
    const DiscInfo *d = e->info;
    char what[1024], state[512], why[2048];
    EngineRows rows = { 0 };

    log_step("=== Details (what the GUI shows) ===");
    engine_disc_line(d, what, sizeof(what), state, sizeof(state), why, sizeof(why));
    log_text(LOG_INFO, "Queue: %s | %s", what, state);
    for (char *w = why, *nl; *w; w = nl ? nl + 1 : w + strlen(w)) {
        nl = strchr(w, '\n');
        log_text(LOG_INFO, "  why: %.*s", nl ? (int)(nl - w) : (int)strlen(w), w);
    }
    if (engine_disc_rows(d, source, &rows) >= 0)
        log_rows(&rows);
    else
        engine_rows_free(&rows);
    for (int t = 0; t < d->nb_titles; t++) {
        const DiscTitleInfo *ti = &d->titles[t];
        char name[64], desc[2048], tstate[512], twhy[1024];
        engine_title_line(ti, name, sizeof(name), desc, sizeof(desc));
        engine_title_state(ti, 0, tstate, sizeof(tstate), twhy, sizeof(twhy));
        log_text(LOG_INFO, "Queue: %s  %s | %s", name, desc, tstate);
        if (*twhy)
            log_text(LOG_INFO, "  why: %s", twhy);
        for (int c = 0; c < ti->nb_chapters; c++) {
            engine_chapter_line(ti, c, desc, sizeof(desc));
            log_text(LOG_INFO, "Queue:   Chapter %d  %s", c + 1, desc);
        }
        for (int i = 0; i < ti->nb_streams; i++) {
            engine_track_line(ti, i, name, sizeof(name), desc, sizeof(desc));
            log_text(LOG_INFO, "Queue:   %s  %s", name, desc);
        }
        if (engine_title_rows(ti, e->keeps[t], &rows) >= 0)
            log_rows(&rows);
        else
            engine_rows_free(&rows);
    }
}

int engine_info(const char *source, const EngineChoice *c, int details)
{
    EngineDisc *e;

    log_msg(MSG_SOURCE, LOG_INFO, "Source: %s", source);
    if (engine_disc_open(source, c, &e) < 0)
        return 1;
    engine_disc_overview(e, 1);
    if (details)
        engine_details(e, source);
    engine_disc_close(&e);
    return 0;
}

/* The words for a key frame's kind (enum DrDiscStart). */
static const char *start_text(int start)
{
    return start == DR_DISC_START_CLEAN  ? "clean: nothing before it is needed" :
           start == DR_DISC_START_OPEN   ? "open: the pictures shown before it may need pictures before it" :
           start == DR_DISC_START_BROKEN ? "a broken link: the pictures shown before it cannot be decoded" :
                                           "its codec does not say how decoding starts there";
}

/* The pictures with a time from a up to (not with) b, in either order. */
static int pictures_between(const CutIndex *ix, int64_t a, int64_t b)
{
    int64_t lo = DR_MIN(a, b), hi = DR_MAX(a, b);
    int n = 0;
    for (int i = 0; i < ix->nb_pics; i++)
        n += ix->pics[i].time >= lo && ix->pics[i].time < hi;
    return n;
}

static int by_zero(const void *a, const void *b)
{
    const DrDiscCut *x = a, *y = b;
    return (x->zero > y->zero) - (x->zero < y->zero);
}

/* Where the cuts asked for (items) fall in title t: the title's video read
 * as a rip reads it, each cut said in the log, then the parts. *cuts (freed
 * with free()) / *nb: the cuts in time order, *end: where the title's
 * video ends (ticks; as the disc records the title's length when it was not
 * read to its end), *starts (freed with free()): per cut and track of the
 * title the unit a start-unit track's part starts at (cuts.h
 * cut_track_start(), INT64_MAX for the other tracks). 0 when every cut is
 * made, 1 when one is refused or the title cannot be read (logged). */
static int resolve_cuts(EngineDisc *e, int t, const char *items, DrDiscCut **cuts, int *nb, int64_t *end,
                        int64_t **starts)
{
    CutRequest *reqs = NULL;
    CutResult *res = NULL;
    CutPoint *pts = NULL;
    CutIndex ix = { 0 };
    int64_t stop = 0, *zeros = NULL;
    int nb_reqs, nb_pts = 0, made = 0, rule, n = 0, ret = 1;
    const char *bad;
    char a[40], b[40];

    int64_t *windows = NULL;
    int nb_windows = 0, nb_streams = e->info->titles[t].nb_streams;

    *cuts   = NULL;
    *nb     = 0;
    *end    = 0;
    *starts = NULL;
    if (cut_parse(items, &reqs, &nb_reqs, &bad) < 0) {
        log_msg(MSG_USAGE, LOG_ERROR, "Cuts \"%s\" cannot be read at \"%s\": the cuts are c<chapter> or a time "
                "(h:mm:ss.fff or m:ss.fff on the title's timeline), comma separated", items, bad);
        return 1;
    }
    /* a chapter needs the title read to its end (its chapters are placed
     * there); times only up to the key frames after the last one */
    for (int i = 0; i < nb_reqs && stop >= 0; i++)
        stop = reqs[i].chapter ? -1 : DR_MAX(stop, reqs[i].time);
    rule = !strcmp(setting_text("cuts.key_frame"), "next") ? CUT_NEXT : CUT_NEAREST;
    log_text(LOG_INFO, "Cuts: %s; a time not on a key frame takes the %s key frame (setting cuts.key_frame)", items,
             rule == CUT_NEXT ? "next" : "nearest");
    /* the units of the start-unit tracks are kept around what is asked (a
     * cut's key frame lies within a few seconds of it): each time, each
     * chapter's mark on the disc when a chapter is asked */
    if (!(windows = calloc(2 * (nb_reqs + e->info->titles[t].nb_chapters + 1), sizeof(*windows))))
        goto end;
    for (int i = 0; i < nb_reqs; i++)
        if (!reqs[i].chapter) {
            windows[2 * nb_windows]       = reqs[i].time - 5 * DR_TICKS_PER_SECOND;
            windows[2 * nb_windows++ + 1] = reqs[i].time + 5 * DR_TICKS_PER_SECOND;
        }
    for (int c = 0; stop < 0 && c < e->info->titles[t].nb_chapters; c++) {
        int64_t m = e->info->titles[t].chapters[c].start * (DR_TICKS_PER_SECOND / 1000000);
        windows[2 * nb_windows]       = m - 5 * DR_TICKS_PER_SECOND;
        windows[2 * nb_windows++ + 1] = m + 5 * DR_TICKS_PER_SECOND;
    }
    if (cut_index_read(e->disc, t, stop, windows, nb_windows, &ix) < 0 ||
        cut_points(ix.pics, ix.nb_pics, ix.codec, &pts, &nb_pts) < 0)
        goto end;
    log_text(LOG_INFO, "Cuts: the title's video read %s: %d picture(s), %d place(s) a part can start",
             ix.complete ? "to its end" : "as far as the cuts need", ix.nb_pics, nb_pts);
    if (!(res = calloc(nb_reqs, sizeof(*res))) || !(zeros = calloc(nb_reqs + 2, sizeof(*zeros))) ||
        !(*cuts = calloc(nb_reqs, sizeof(**cuts))))
        goto end;
    made = cut_resolve(pts, nb_pts, ix.chapters, ix.nb_chapters, ix.end, reqs, nb_reqs, rule, res);
    for (int i = 0; i < nb_reqs; i++) {
        const CutResult *r = &res[i];
        const CutPoint *p;
        char asked[96], extra[640] = "", c[40];

        if (reqs[i].chapter && r->target)
            snprintf(asked, sizeof(asked), "chapter %d (%s)", reqs[i].chapter, cut_time_text(a, sizeof(a), r->target));
        else if (reqs[i].chapter)
            snprintf(asked, sizeof(asked), "chapter %d", reqs[i].chapter);
        else
            snprintf(asked, sizeof(asked), "%s (%s)", reqs[i].text, cut_time_text(a, sizeof(a), r->target));
        if (r->point < 0) {
            if (r->same_as >= 0)
                log_msg(MSG_CUTS, LOG_WARNING, "  - %s: no cut - the same key frame as %s", asked,
                        reqs[r->same_as].text);
            else
                log_msg(MSG_CUTS, LOG_WARNING, "  - %s: no cut - %s", asked, r->why);
            continue;
        }
        p = &pts[r->point];
        (*cuts)[n++] = (DrDiscCut){ p->zero, p->key_time };
        if (p->kept)
            snprintf(extra, sizeof(extra), "; the part starts with %d picture(s) shown before its key frame "
                     "(they decode from it)", p->kept);
        if (p->undecodable)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "; %d picture(s) shown before it "
                     "need the pictures before it and cannot be left out (reference pictures, or a codec that "
                     "does not allow it): kept, the part starts with them (players do not show them; its first "
                     "picture that decodes at %s)", p->undecodable, cut_time_text(c, sizeof(c), p->key_time - p->zero));
        if (p->left_out)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "; %d picture(s) shown before it "
                     "need the pictures before it: left out (in neither part)", p->left_out);
        if (p->block >= 0)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "; its key frame is read from segment "
                     "%d, block %"PRId64, p->seg + 1, p->block);
        if (r->on_point)
            log_text(LOG_INFO, "  - %s -> on a key frame (%s)%s; the part starts at %s", asked,
                     start_text(p->start), extra, cut_time_text(b, sizeof(b), p->zero));
        else
            log_text(LOG_INFO, "  - %s -> the %s key frame (%s), moved by %c%.3f ms (%d picture(s))%s; the part "
                     "starts at %s", asked, rule == CUT_NEXT ? "next" : "nearest", start_text(p->start),
                     p->zero < r->target ? '-' : '+', (double)DR_ABS(p->zero - r->target) * 1000 / DR_TICKS_PER_SECOND,
                     pictures_between(&ix, r->target, p->zero), extra, cut_time_text(b, sizeof(b), p->zero));
    }
    /* the cuts and the parts, in time order */
    qsort(*cuts, n, sizeof(**cuts), by_zero);
    *nb  = n;
    /* where each start-unit track's part starts at each cut */
    if (!(*starts = malloc((size_t)(n ? n : 1) * (nb_streams ? nb_streams : 1) * sizeof(**starts))))
        goto end;
    for (int i = 0; i < n * nb_streams; i++)
        (*starts)[i] = INT64_MAX;
    for (int c = 0; c < n; c++)
        for (int a = 0; a < ix.nb_audio; a++) {
            const CutAudio *au = &ix.audio[a];
            const char *unit = au->codec == DR_CODEC_TRUEHD || au->codec == DR_CODEC_MLP ? "major sync unit" :
                               "unit whose lossless data starts fresh";
            char what[48];
            CutStart st;
            cut_track_start(au->u, au->nb, (*cuts)[c].zero, CUT_START_LIMIT, &st);
            (*starts)[c * nb_streams + au->track] = st.time;
            snprintf(what, sizeof(what), "track %d (%s)", au->track,
                     codec_label(e->info->titles[t].streams[au->track].codec,
                                 e->info->titles[t].streams[au->track].profile));
            if (st.time == INT64_MAX)
                log_text(LOG_INFO, "    cut %d: %s ends before it", c + 1, what);
            else if (st.entry)
                log_text(LOG_INFO, "    cut %d: %s starts at the %s %.3f ms after the part's 0; %d unit(s) before "
                         "it stay in the part before", c + 1, what, unit,
                         (double)(st.time - (*cuts)[c].zero) * 1000 / DR_TICKS_PER_SECOND, st.carried);
            else
                log_msg(MSG_CUTS, LOG_WARNING, "    cut %d: %s has no %s within %.0f ms of it: it starts at its first "
                        "unit at or after the part's 0 (%.3f ms after it); its lossless data runs across the cut: "
                        "%s and the last unit(s) before it decode as the DTS core only", c + 1, what, unit,
                        (double)CUT_START_LIMIT * 1000 / DR_TICKS_PER_SECOND,
                        (double)(st.time - (*cuts)[c].zero) * 1000 / DR_TICKS_PER_SECOND,
                        st.to_entry < 0 ? "the units after it as far as was read" :
                        st.to_entry == 1 ? "1 unit after it" : "the units after it up to the next start unit");
        }
    *end = ix.complete ? ix.end : e->info->titles[t].duration * (DR_TICKS_PER_SECOND / 1000000);
    zeros[0] = 0;
    for (int i = 0; i < n; i++)
        zeros[i + 1] = (*cuts)[i].zero;
    zeros[n + 1] = *end;
    log_text(LOG_INFO, "Cuts: %d part(s)%s", n + 1, ix.complete ? "" : " (the last one's end as the disc records "
             "the title's length)");
    for (int i = 0; i <= n; i++) {
        char l[40];
        log_text(LOG_INFO, "  part %d: %s - %s (%s)", i + 1, cut_time_text(a, sizeof(a), zeros[i]),
                 cut_time_text(b, sizeof(b), zeros[i + 1]), cut_time_text(l, sizeof(l), zeros[i + 1] - zeros[i]));
    }
    ret = made == nb_reqs ? 0 : 1;
end:
    if (ret) {
        free(*cuts);
        free(*starts);
        *cuts   = NULL;
        *starts = NULL;
        *nb     = 0;
    }
    free(windows);
    free(reqs);
    free(res);
    free(pts);
    free(zeros);
    cut_index_free(&ix);
    return ret;
}

int engine_cuts(const char *source, const char *title, const char *items, const EngineChoice *c)
{
    EngineDisc *e = NULL;
    DrDiscCut *cuts;
    int64_t end;
    int t, nb, ret = 1;
    char *x, dur[32];

    t = (int)strtol(title, &x, 10);
    if (!*title || *x || t < 0) {
        log_msg(MSG_USAGE, LOG_ERROR, "cuts: the title is one number as info lists it, not \"%s\"", title);
        return 2;
    }
    {
        CutRequest *reqs;
        const char *bad;
        if (cut_parse(items, &reqs, &nb, &bad) < 0) {
            log_msg(MSG_USAGE, LOG_ERROR, "cuts: \"%s\" cannot be read at \"%s\": the cuts are c<chapter> or a time "
                    "(h:mm:ss.fff or m:ss.fff on the title's timeline), comma separated", items, bad);
            return 2;
        }
        free(reqs);
    }
    log_msg(MSG_SOURCE, LOG_INFO, "Source: %s", source);
    if (engine_disc_open(source, c, &e) < 0)
        return 1;
    if (t >= e->info->nb_titles) {
        log_msg(MSG_TITLE_MISSING, LOG_ERROR, "Title #%d does not exist (the disc has %d)", t, e->info->nb_titles);
        engine_disc_close(&e);
        return 2;
    }
    hms(dur, sizeof(dur), e->info->titles[t].duration);
    log_step("=== Cuts: title #%d (%s) ===", t, dur);
    {
        int64_t *starts;
        ret = resolve_cuts(e, t, items, &cuts, &nb, &end, &starts);
        free(starts);
    }
    free(cuts);
    engine_disc_close(&e);
    return ret;
}

/* The check after a cut title's last part: per track, the bytes of all its
 * parts (and the video's leading pictures left out) against the title's
 * (the last part read the whole title); said per track. Returns the
 * tracks that do not add up. */
static int cut_check(int t, const CutTrackBytes *sums, int nb_sums, const CutTrackBytes *title, int nb_title)
{
    int bad = 0;

    for (int i = 0; i < nb_title; i++) {
        const CutTrackBytes *w = &title[i], *s = NULL;
        int64_t got;
        for (int k = 0; k < nb_sums && !s; k++)
            if (sums[k].number == w->number)
                s = &sums[k];
        if (!s || !w->title)
            continue;                   /* a track left out of the parts */
        got = s->part + s->left;
        if (!w->read_all) {
            log_msg(MSG_CUTS, LOG_ERROR, "Cut check: title #%d track %d: the last part did not read the whole title: "
                    "not checked", t, w->number);
            bad++;
            continue;
        }
        if (w->pcm && w->bytes_per_sample)
            log_msg(MSG_CUTS, got == w->title ? LOG_OK : LOG_ERROR, "Cut check: title #%d track %d (LPCM): the parts' "
                    "%"PRId64" samples %s the title's %"PRId64": %s", t, w->number, got / w->bytes_per_sample,
                    got == w->title ? "=" : "differ from", w->title / w->bytes_per_sample,
                    got == w->title ? "PASS" : "FAIL");
        else if (s->left)
            log_msg(MSG_CUTS, got == w->title ? LOG_OK : LOG_ERROR, "Cut check: title #%d track %d: the parts' %"PRId64
                    " bytes + %"PRId64" of leading pictures left out %s the title's %"PRId64": %s", t, w->number,
                    s->part, s->left, got == w->title ? "=" : "differ from", w->title, got == w->title ? "PASS" : "FAIL");
        else
            log_msg(MSG_CUTS, got == w->title ? LOG_OK : LOG_ERROR, "Cut check: title #%d track %d: the parts' %"PRId64
                    " bytes %s the title's %"PRId64": %s", t, w->number, got, got == w->title ? "=" : "differ from",
                    w->title, got == w->title ? "PASS" : "FAIL");
        bad += got != w->title;
    }
    return bad;
}

/* A BMP file (24 bits, rows bottom up, each padded to 4 bytes) of a BGRA
 * picture. 0, or a negative base error. */
static int write_bmp(const char *path, const CutFrame *f)
{
    int row = (f->width * 3 + 3) & ~3;
    uint32_t data = (uint32_t)row * f->height;
    uint8_t h[54] = { 'B', 'M' }, *line;
    FILE *out;
    int ok = 1;

    if (!(out = fopen(path, "wb")))
        return DR_ERR(errno);
    if (!(line = calloc(row, 1))) {
        fclose(out);
        return DR_ERR(ENOMEM);
    }
    dr_wl32(h + 2, 54 + data);
    dr_wl32(h + 10, 54);
    dr_wl32(h + 14, 40);
    dr_wl32(h + 18, f->width);
    dr_wl32(h + 22, f->height);
    h[26] = 1;
    h[28] = 24;
    dr_wl32(h + 34, data);
    ok = fwrite(h, 1, sizeof(h), out) == sizeof(h);
    for (int y = f->height - 1; ok && y >= 0; y--) {
        const uint8_t *s = f->bgra + (size_t)y * f->stride;
        for (int x = 0; x < f->width; x++)
            memcpy(line + 3 * x, s + 4 * x, 3);
        ok = fwrite(line, 1, row, out) == (size_t)row;
    }
    free(line);
    if (fclose(out) || !ok)
        return DR_ERR(EIO);
    return 0;
}

int engine_frame(const char *source, const char *title, const char *at, const char *path, const EngineChoice *c)
{
    EngineDisc *e = NULL;
    CutIndex ix = { 0 };
    CutPreview *pv = NULL;
    CutRequest *reqs = NULL;
    CutFrame f;
    const char *bad;
    char *x, a[40], b[40];
    int t, nb, n, ret = 1;

    t = (int)strtol(title, &x, 10);
    if (!*title || *x || t < 0 || cut_parse(at, &reqs, &nb, &bad) < 0 || nb != 1 || reqs[0].chapter) {
        log_msg(MSG_USAGE, LOG_ERROR, "frame: a title number and one time (h:mm:ss.fff or m:ss.fff on the title's "
                "timeline) are asked, not \"%s\" \"%s\"", title, at);
        free(reqs);
        return 2;
    }
    log_msg(MSG_SOURCE, LOG_INFO, "Source: %s", source);
    if (engine_disc_open(source, c, &e) < 0)
        goto end;
    if (t >= e->info->nb_titles) {
        log_msg(MSG_TITLE_MISSING, LOG_ERROR, "Title #%d does not exist (the disc has %d)", t, e->info->nb_titles);
        ret = 2;
        goto end;
    }
    /* the title's pictures up to a little past the time, then that picture
     * as the cut editor shows it */
    if (cut_index_read(e->disc, t, reqs[0].time + 2 * DR_TICKS_PER_SECOND, NULL, 0, &ix) < 0 ||
        cut_preview_open(&pv, e->disc, t, &ix, (int64_t)setting_int("cuts.preview_cache_mb") << 20) < 0)
        goto end;
    n = cut_preview_at(pv, reqs[0].time);
    if (cut_preview_picture(pv, n, 0, 0, &f) < 0)
        goto end;
    if ((ret = write_bmp(path, &f)) < 0) {
        log_msg(MSG_OUTPUT_FOLDER, LOG_ERROR, "frame: %s cannot be written: %s", path, dr_err_text(ret));
        ret = 1;
        goto end;
    }
    log_text(LOG_INFO, "Frame: picture %d at %s (%s), %dx%d, decoded from the key frame at %s (picture %d, %d "
             "picture(s) decoded); decoded picture md5 %s; written to %s", n, cut_time_text(a, sizeof(a), f.time),
             f.key ? start_text(f.start) : "no key frame", f.width, f.height,
             cut_time_text(b, sizeof(b), cut_preview_time(pv, f.from)), f.from, f.decoded, f.md5, path);
    ret = 0;
end:
    cut_preview_close(&pv);
    cut_index_free(&ix);
    free(reqs);
    engine_disc_close(&e);
    return ret;
}

/* One disc's demux run (engine_demux()). */
static int demux_one(const char *source, const char *titles, const char *out_dir, const char *split,
                     const EngineChoice *c, int details)
{
    char folder[PATH_MAX], logp[PATH_MAX + 600], name[512];
    EngineDisc *e;
    int count = 0, ret;

    atomic_store(&cancelled, 0);
    if (!out_dir) {
        engine_output_folder(source, folder, sizeof(folder));
        out_dir = folder;
    }
    if (run_begin(source, out_dir, logp, sizeof(logp)))
        return 1;
    /* --details: the queue's states as the GUI shows them */
    engine_disc_name(source, name, sizeof(name));
    engine_queue_details(details ? name : NULL);
    if (details)
        log_text(LOG_INFO, "Queue: %s | %s", name, engine_state_text(ENGINE_STATE_RIPPING));
    /* the disc is read once: every title is opened on it */
    if (engine_disc_open(source, c, &e) < 0) {
        engine_queue_details(NULL);
        log_close_files();
        return 1;
    }
    engine_disc_overview(e, 0);
    /* the choice as the GUI makes it: through the selection's calls */
    if ((count = engine_select_titles(e, titles)) <= 0 || (split && engine_split_titles(e, split) < 0)) {
        if (!count)
            log_msg(MSG_TITLE_SELECTION, LOG_ERROR, "No title selected");
        engine_queue_details(NULL);
        engine_disc_close(&e);
        log_close_files();
        return 2;
    }
    ret = run_titles(e, out_dir, logp);
    engine_queue_details(NULL);
    engine_disc_close(&e);
    return ret;
}


/* A --cut value: its title and where its cuts start in it; 0, or -1. */
static int cut_value(const char *v, int *title, const char **items)
{
    char *end;
    long t = strtol(v, &end, 10);

    if (end == v || *end != ':' || t < 0 || t > 9999)
        return -1;
    *title = (int)t;
    *items = end + 1;
    return 0;
}

int engine_check_choice(const EngineChoice *c)
{
    for (int k = 0; k < c->nb_tracks; k++)
        if (tracks_check(c->tracks[k]) < 0)
            return -1;
    for (int k = 0; k < c->nb_cuts; k++) {
        CutRequest *reqs;
        const char *items, *bad;
        int t, n;
        if (cut_value(c->cuts[k], &t, &items) < 0) {
            log_msg(MSG_USAGE, LOG_ERROR, "--cut %s: <title>:<cuts> is asked, e.g. 1:c5,45:42.323", c->cuts[k]);
            return -1;
        }
        if (cut_parse(items, &reqs, &n, &bad) < 0) {
            log_msg(MSG_USAGE, LOG_ERROR, "--cut %s cannot be read at \"%s\": the cuts are c<chapter> or a time "
                    "(h:mm:ss.fff or m:ss.fff on the title's timeline), comma separated", c->cuts[k], bad);
            return -1;
        }
        free(reqs);
    }
    return 0;
}

int engine_init(const char *config, const char *const *sets, int nb_sets, int json, int verbose)
{
    log_init(json, verbose);                /* installs the modules' log hook (base/vlog.h) */
    dr_analyze_log_install();               /* the processing library's lines into ours */
    dr_analyze_use();
    if (settings_load(config) < 0)
        return 1;
    for (int k = 0; k < nb_sets; k++)
        if (settings_set(sets[k]) < 0)
            return 2;
    config_dirs();
    return 0;
}

/* ---- folders of discs: the GUI's queue on the command line ---- */

/* The discs in path when it is a folder holding discs (not a disc itself),
 * else none. */
static int disc_folder(const char *path, EngineFoundList *l)
{
    if (engine_find_discs(path, 5, l) < 0)
        return -1;
    if (l->nb == 1 && !strcmp(l->discs[0].relative, "."))
        engine_found_free(l);
    return l->nb;
}

int engine_find(const char *path)
{
    EngineFoundList l = { 0 };
    char folder[PATH_MAX];

    if (engine_find_discs(path, 5, &l) < 0)
        return 1;
    if (!l.nb) {
        char note[PATH_MAX + 64];
        engine_no_disc_note(path, note, sizeof(note));
        log_msg(MSG_SOURCE, LOG_WARNING, "%s", note);
        return 1;
    }
    for (int i = 0; i < l.nb; i++) {
        const EngineFound *f = &l.discs[i];
        engine_found_folder(f, NULL, folder, sizeof(folder));
        log_text(LOG_INFO, "Disc %d: %s", i + 1, f->name);
        log_text(LOG_INFO, "  source: %s", f->path);
        log_text(LOG_INFO, "  below:  %s", f->relative);
        log_text(LOG_INFO, "  output: %s", folder);
    }
    log_msg(MSG_SOURCE, LOG_INFO, "%d disc%s found in %s", l.nb, l.nb == 1 ? "" : "s", path);
    engine_found_free(&l);
    return 0;
}

int engine_demux(const char *source, const char *titles, const char *out_dir, const char *split,
                 const EngineChoice *c, int details)
{
    EngineFoundList l = { 0 };
    char folder[PATH_MAX], base[PATH_MAX];
    int n, ret = 0, failed = 0;

    /* a disc (image or folder): one run */
    if ((n = disc_folder(source, &l)) <= 0)
        return demux_one(source, titles, out_dir, split, c, details);
    /* a folder of discs: each one in turn into its own folder, as the GUI's
     * queue rips them (out_dir: the main output folder for them) */
    for (int i = 0; i < n && !engine_cancelled(); i++) {
        const EngineFound *f = &l.discs[i];
        int r;
        log_step("=== Disc %d/%d: %s ===", i + 1, n, f->name);
        engine_found_folder(f, out_dir, base, sizeof(base));
        engine_unique_folder(base, folder, sizeof(folder));
        r = demux_one(f->path, titles, folder, split, c, details);
        failed += r != 0;
        ret = r > ret ? r : ret;
    }
    log_step("=== Discs ===");
    log_msg(MSG_SUMMARY, failed ? LOG_ERROR : LOG_OK, "%d disc%s of %d ripped without failures%s", n - failed,
            n - failed == 1 ? "" : "s", n, engine_cancelled() ? " (stopped as asked)" : "");
    engine_found_free(&l);
    return ret;
}
