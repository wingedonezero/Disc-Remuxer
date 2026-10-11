/*
 * disc-remuxer's engine (see engine.h): the disc opened once, the overview
 * of its titles, the choice of titles and tracks, a run's folder, log and
 * titles.
 */

#include <errno.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>
#include <libavformat/discinfo.h>
#include <libavutil/avutil.h>
#include <libavutil/bprint.h>

#include "log.h"
#include "msg.h"
#include "names.h"
#include "settings.h"
#include "codecs.h"
#include "engine.h"
#include "select.h"
#include "writer.h"


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

/* The disc demuxer: it tells the kind of disc and follows the contract of
 * docs/FORMATS.md (the disc info, each track's result at the end, read
 * progress as an option). */
#define DISC_FORMAT "disc"

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

void engine_track_parts(const DiscTitleInfo *t, int i, char *codec, size_t csize, char *rest, size_t rsize)
{
    const DiscStreamInfo *k = &t->streams[i];
    char extra[160] = "", link[96] = "", det[160] = "", a[24], r[48], l[64], b[24];

    if (k->type == AVMEDIA_TYPE_SUBTITLE && k->width)
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
    if (k->type == AVMEDIA_TYPE_VIDEO && k->width && !k->forced)
        snprintf(det, sizeof(det), "  %dx%d %s %s", k->width, k->height, aspect_text(k, a, sizeof(a)),
                 frame_rate_text(k, r, sizeof(r)));
    if (k->type == AVMEDIA_TYPE_AUDIO && k->channels) {
        snprintf(det, sizeof(det), "  %s %d Hz", layout_text(k, l, sizeof(l)), k->sample_rate);
        if (k->bits && codec_lossless(k))
            snprintf(det + strlen(det), sizeof(det) - strlen(det), " %d-bit", k->bits);
        if (*bit_rate_text(k->bit_rate, b, sizeof(b)) && !codec_lossless(k))
            snprintf(det + strlen(det), sizeof(det) - strlen(det), " %s", b);
    }
    codec_description(k, k->second_view && k->from_track >= 0 ? &t->streams[k->from_track] : NULL, codec, csize);
    snprintf(rest, rsize, "%s%s%s", det, extra, link);
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
static void title_overview(const DiscTitleInfo *t, int detail, const uint8_t *keep)
{
    char dur[32], size[32], files[2048] = "", js[512], jl[16], extra[160] = "", pt[24] = "";
    AVBPrint streams, parts;
    int nv = 0, na = 0, ns = 0;

    hms(dur, sizeof(dur), t->duration);
    size_text(size, sizeof(size), t->size);
    av_bprint_init(&streams, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (int i = 0; i < t->nb_streams; i++) {
        const DiscStreamInfo *k = &t->streams[i];
        char jc[64], jsl[16], n1[24], n2[24], n3[24], n4[24], n5[24], n6[24], ji[24], jf[24], jo[24];
        char d[96], jd[128], pl[48], jp[64], fr[48], jr[64], as[24], ja[32], lt[64], jl2[96], dv[48], jv[64], fq[32];
        char lm[24], rl[128], jn[96], jm[48], je[16], an[32], jan[48], jh[32], jco[128], jma[320], nc1[24], nc2[24];
        const char *lname = k->language[0] ? disc_language_name(k->language) : NULL, *mkv = disc_mkv_codec_id(k);
        (void)dv; (void)fr;
        nv += k->type == AVMEDIA_TYPE_VIDEO;
        na += k->type == AVMEDIA_TYPE_AUDIO;
        ns += k->type == AVMEDIA_TYPE_SUBTITLE;
        av_bprintf(&streams,
                 "%s{\"index\":%d,\"kind\":\"%s\",\"codec\":%s,\"language\":%s,\"inner_track\":%s,"
                 "\"from_track\":%s,\"forced\":%s,\"second_view\":%s,\"second_layer\":%s,\"offset_sequence\":%s,\"width\":%s,"
                 "\"height\":%s,\"channels\":%s,\"sample_rate\":%s,\"bits\":%s,\"bit_rate\":%s,"
                 "\"description\":%s,\"profile\":%s,\"dolby_vision\":%s,\"frame_rate\":%s,\"aspect\":%s,"
                 "\"layout\":%s,\"layout_mask\":%s,\"roles\":[%s],\"selected\":%s,\"language_name\":%s,"
                 "\"codec_id\":%s,\"file_type\":%s,\"name\":%s,\"hdr\":%s,\"colour\":%s,\"mastering\":%s,"
                 "\"max_cll\":%s,\"max_fall\":%s}",
                 i ? "," : "", i, av_get_media_type_string(k->type),
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
    if (t->nb_parts)
        snprintf(pt, sizeof(pt), ", %d parts", t->nb_parts);
    log_text(LOG_INFO, "  #%-2d %-40s %9s  %-6s %d audio, %d subtitle%s, %d chapter%s%s", t->index,
             t->name ? t->name : "", dur,
             nv ? codec_label(t->streams[0].codec, 0) : "-", na, ns, ns == 1 ? "" : "s",
             t->nb_chapters, t->nb_chapters == 1 ? "" : "s", pt);
    if (detail) {
        char list[1024] = "";
        for (int i = 0; i < t->nb_files; i++)
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", i ? ", " : "", t->files[i]);
        char from[64] = "";
        if (t->source)
            snprintf(from, sizeof(from), "from %s%s", t->source, t->nb_files ? ": " : "");
        if (t->angle)
            snprintf(from + strlen(from), sizeof(from) - strlen(from), "angle %d, ", t->angle);
        log_text(LOG_INFO, "       %s, %s%s%s%s%s", size, from, list, t->language[0] ? ", language " : "",
                 t->language, t->selected ? "" : ", not selected (shorter than the minimum length)");
        for (int i = 0; i < t->nb_streams; i++) {
            const DiscStreamInfo *k = &t->streams[i];
            char line[512];
            log_text(LOG_INFO, "       %-2d %-8s %-3s %s", i, av_get_media_type_string(k->type),
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
    if (t->source) {
        char jsrc[96];
        snprintf(extra, sizeof(extra), ",\"source\":%s", json_str(jsrc, sizeof(jsrc), t->source));
    }
    if (t->angle)
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"angle\":%d", t->angle);
    /* parts: the part number (from 1) as --split names its files, where it starts in the title */
    av_bprint_init(&parts, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (int p = 0; p < t->nb_parts; p++) {
        const DiscPartInfo *q = &t->parts[p];
        av_bprintf(&parts, "%s{\"part\":%d,\"start\":%.3f,\"duration\":%.3f,\"chapters\":%d,\"files\":[",
                   p ? "," : ",\"parts\":[", p + 1, q->start / (double)AV_TIME_BASE,
                   q->duration / (double)AV_TIME_BASE, q->nb_chapters);
        for (int f = 0; f < q->nb_files; f++) {
            char jf[300];
            av_bprintf(&parts, "%s%s", f ? "," : "", json_str(jf, sizeof(jf), t->files[q->first_file + f]));
        }
        av_bprintf(&parts, "]}%s", p + 1 == t->nb_parts ? "]" : "");
    }
    /* what the format part tells more of the title */
    for (int p = 0; p < t->nb_props; p++) {
        char jn[128], jv[512];
        av_bprintf(&parts, "%s%s:%s%s", p ? "," : ",\"properties\":{", json_str(jn, sizeof(jn), t->props[p].name),
                   json_str(jv, sizeof(jv), t->props[p].value), p + 1 == t->nb_props ? "}" : "");
    }
    log_json("title", "\"index\":%d,\"name\":%s,\"duration\":%.3f,\"chapters\":%d,\"size\":%"PRId64",\"language\":%s,"
             "\"selected\":%s,\"files\":[%s],\"tracks\":[%s]%s%s", t->index,
             json_str(js, sizeof(js), t->name ? t->name : ""), t->duration / (double)AV_TIME_BASE, t->nb_chapters,
             t->size, json_str(jl, sizeof(jl), t->language), t->selected ? "true" : "false", files, streams.str, extra,
             parts.str);
    av_bprint_finalize(&streams, NULL);
    av_bprint_finalize(&parts, NULL);
}

struct EngineDisc {
    AVFormatContext *ctx;
    const DiscInfo  *info;
    char             source[PATH_MAX];
    uint8_t        **keeps;      /* each title's tracks to rip */
};

static int parse_titles(const char *spec, const DiscInfo *d, int only_selected, char *sel);

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
    int ret = AVERROR(ENOMEM);

    *out = NULL;
    if (!e || (ret = open_disc(DISC_FORMAT, source, &e->ctx)) < 0 || !(e->info = disc_info(e->ctx))) {
        if (ret == AVERROR(ENOKEY))
            log_msg(MSG_SOURCE_UNSUPPORTED, LOG_ERROR, "%s is encrypted (AACS) and the key files have no key for "
                    "it (its disc ID is in the AACS line above): add its key to a KEYDB.cfg (setting "
                    "aacs.key_files)", source);
        else
            log_msg(MSG_SOURCE_UNSUPPORTED, LOG_ERROR, "%s cannot be read as a disc this program supports (%s); "
                    "DVD-Video, HD DVD and Blu-ray are supported", source, av_err2str(ret < 0 ? ret : AVERROR_BUG));
        engine_disc_close(&e);
        return -1;
    }
    snprintf(e->source, sizeof(e->source), "%s", source);
    if (title_choices(e->info, c, &e->keeps) < 0) {
        engine_disc_close(&e);
        return -1;
    }
    *out = e;
    return 0;
}

const DiscInfo *engine_disc_info(const EngineDisc *d)
{
    return d->info;
}

uint8_t *engine_disc_tracks(EngineDisc *d, int t)
{
    return t >= 0 && t < d->info->nb_titles ? d->keeps[t] : NULL;
}

void engine_disc_close(EngineDisc **pd)
{
    EngineDisc *d = *pd;

    if (!d)
        return;
    if (d->info)
        free_keeps(d->keeps, d->info->nb_titles);
    avformat_close_input(&d->ctx);
    free(d);
    *pd = NULL;
}

void engine_disc_overview(const EngineDisc *e, int detail)
{
    const DiscInfo *d = e->info;
    char revs[32] = "", js1[64], js2[256], js3[64], js4[1024], jname[1100 + 16 * 300];

    if (d->udf_revision)
        snprintf(revs, sizeof(revs), " %x.%02x", (unsigned)(d->udf_revision >> 8), (unsigned)(d->udf_revision & 0xff));
    log_msg(MSG_SOURCE, LOG_INFO, "Disc: %s '%s', %s%s, %s, %d title%s", d->kind, d->label ? d->label : "",
            d->filesystem ? d->filesystem : "?", revs, d->encrypted ? "encrypted" : "not encrypted",
            d->nb_titles, d->nb_titles == 1 ? "" : "s");
    if (d->name)
        log_msg(MSG_SOURCE, LOG_INFO, "Disc name: %s", d->name);
    jname[0] = 0;
    if (d->name)
        snprintf(jname, sizeof(jname), ",\"name\":%s", json_str(js4, sizeof(js4), d->name));
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
    for (int t = 0; t < d->nb_titles; t++)
        title_overview(&d->titles[t], detail, e->keeps[t]);
}

/* "all" | "3" | "1,3-5" into a 0/1 list of d's titles; all with
 * only_selected: the titles not shorter than the minimum length (each one
 * left out said); returns the count selected, or -1 (logged). */
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
                log_msg(MSG_TITLE_SELECTION, LOG_INFO, "Title #%d is shorter than the minimum length: not ripped "
                        "with all (name it to rip it)", t);
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

/* The disc's name for its folder and log: the image's or folder's name (a
 * VIDEO_TS or BDMV folder: the folder holding it). */
static void disc_name(const char *source, char *out, size_t size)
{
    char tmp[PATH_MAX], *base, *dot;
    size_t n;

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
    clean_name(base, 0, out, size);
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

    disc_name(source, name, sizeof(name));
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
    disc_name(source, name, sizeof(name));
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

/* The run's titles (sel; spl: as parts, may be NULL) from the open disc,
 * then the summary; the log files closed. 0, or 1 (a title failed). */
static int run_titles(EngineDisc *e, const char *sel, const char *split, const char *out_dir, const char *logp)
{
    const DiscInfo *di = e->info;
    int nb = di->nb_titles, count = 0, ordinal = 0, failed = 0, done = 0, errors, warnings;
    char *spl = calloc(nb ? nb : 1, 1);

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
    if (setting_bool("output.split_joined_titles"))
        memset(spl, 1, (size_t)FFMAX(nb, 0));
    for (int t = 0; t < nb; t++) {
        int parts = spl[t] ? di->titles[t].nb_parts : 0, part_failed = 0, part_stopped = 0;
        if (!sel[t])
            continue;
        if (engine_cancelled())
            break;
        ++ordinal;
        for (int p = parts ? 0 : -1; p < parts && !engine_cancelled(); p++) {
            /* the second view: read when one of its tracks is selected */
            /* stream files: every title, and each part as a title of its own, in a folder of its name */
            TitleJob job = { e->ctx, t, p, parts, 1, e->keeps[t], ordinal, count, out_dir, 1 };
            TitleOutcome res;
            demux_title(&job, &res);
            /* a title stopped as asked is not written, not failed */
            part_stopped += res.stopped;
            part_failed  += res.failed && !res.stopped;
        }
        failed += !!part_failed;
        done   += !part_failed && !part_stopped;
    }
    free(spl);

    log_counts(&errors, &warnings);
    log_step("=== Summary ===");
    if (engine_cancelled())
        log_msg(MSG_SUMMARY, LOG_WARNING, "Stopped as asked: %d of %d title%s not written", count - done,
                count, count == 1 ? "" : "s");
    log_msg(MSG_SUMMARY, failed ? LOG_ERROR : warnings ? LOG_INFO : LOG_OK,
            "%d title%s written, %d failed; %d warning%s, %d error%s", done, done == 1 ? "" : "s", failed, warnings,
            warnings == 1 ? "" : "s", errors, errors == 1 ? "" : "s");
    log_msg(MSG_LOG_FILE, LOG_INFO, "Log: %s", logp);
    {
        char jl[PATH_MAX + 700];
        log_json("result", "\"written\":%d,\"failed\":%d,\"warnings\":%d,\"errors\":%d,\"stopped\":%s,\"log\":%s",
                 done, failed, warnings, errors, engine_cancelled() ? "true" : "false", json_str(jl, sizeof(jl), logp));
    }
    log_close_files();
    return failed || engine_cancelled() ? 1 : 0;
}

int engine_disc_rip(EngineDisc *e, const char *sel, const char *split, const char *out_dir)
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
    return run_titles(e, sel, split, out_dir, logp);
}

int engine_info(const char *source, const EngineChoice *c)
{
    EngineDisc *e;

    log_msg(MSG_SOURCE, LOG_INFO, "Source: %s", source);
    if (engine_disc_open(source, c, &e) < 0)
        return 1;
    engine_disc_overview(e, 1);
    engine_disc_close(&e);
    return 0;
}

int engine_demux(const char *source, const char *titles, const char *out_dir, const char *split,
                 const EngineChoice *c)
{
    char folder[PATH_MAX], logp[PATH_MAX + 600], *sel = NULL, *spl = NULL;
    EngineDisc *e;
    int nb, count = 0, ret;

    atomic_store(&cancelled, 0);
    if (!out_dir) {
        engine_output_folder(source, folder, sizeof(folder));
        out_dir = folder;
    }
    if (run_begin(source, out_dir, logp, sizeof(logp)))
        return 1;
    /* the disc is read once: every title is opened on it */
    if (engine_disc_open(source, c, &e) < 0) {
        log_close_files();
        return 1;
    }
    engine_disc_overview(e, 0);
    nb  = e->info->nb_titles;
    sel = calloc(nb ? nb : 1, 1);
    spl = calloc(nb ? nb : 1, 1);
    if (!sel || !spl || (count = parse_titles(titles, e->info, 1, sel)) <= 0 ||
        (split && parse_titles(split, e->info, 0, spl) < 0)) {
        if (sel && spl && !count)
            log_msg(MSG_TITLE_SELECTION, LOG_ERROR, "No title selected");
        free(sel);
        free(spl);
        engine_disc_close(&e);
        log_close_files();
        return 2;
    }
    ret = run_titles(e, sel, spl, out_dir, logp);
    free(sel);
    free(spl);
    engine_disc_close(&e);
    return ret;
}

const char *engine_codec_label(enum AVCodecID id, int profile)
{
    return codec_label(id, profile);
}

int engine_check_choice(const EngineChoice *c)
{
    for (int k = 0; k < c->nb_tracks; k++)
        if (tracks_check(c->tracks[k]) < 0)
            return -1;
    return 0;
}

int engine_init(const char *config, const char *const *sets, int nb_sets, int json, int verbose)
{
    log_init(json, verbose);
    if (settings_load(config) < 0)
        return 1;
    for (int k = 0; k < nb_sets; k++)
        if (settings_set(sets[k]) < 0)
            return 2;
    config_dirs();
    return 0;
}
