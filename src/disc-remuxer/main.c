/*
 * disc-remuxer: rips disc titles into stream files (MKV to follow).
 *
 *   disc-remuxer [options] <command> ...
 *
 * Commands decide what one run does; settings (disc-remuxer.toml next to
 * the program) decide how output is made. Every run that writes output
 * leaves a log in its output folder.
 */

#include <errno.h>
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
#include "writer.h"

#ifndef DISC_REMUXER_VERSION
#define DISC_REMUXER_VERSION "0.2.0"
#endif

static void usage(FILE *f)
{
    fputs("usage: disc-remuxer [options] <command> ...\n"
          "\n"
          "commands:\n"
          "  info  <source>                        the titles and tracks of a disc\n"
          "  demux <source> <titles> <out folder>  the titles' tracks as stream files, chapters, a log\n"
          "  settings [show|path]                  the settings in effect / where the settings file is\n"
          "  version                               program and library versions\n"
          "\n"
          "  <source>  a disc image (.iso) or a disc folder\n"
          "  <titles>  all | 3 | 1,3-5   (numbers as info lists them, from 0)\n"
          "\n"
          "options:\n"
          "  --config <file>      settings file to use (default: disc-remuxer.toml next to the program)\n"
          "  --set group.key=val  change a setting for this run only (repeatable)\n"
          "  --json               JSON records on stdout, one per line, instead of text\n"
          "  --split <titles>     demux: write these titles' parts one by one (a title that plays whole\n"
          "                       stream files back to back, e.g. episodes; info lists the parts);\n"
          "                       setting output.split_joined_titles does it for every title\n"
          "  -v, -vv              more detail on the terminal\n"
          "\n"
          "Only HD DVD images are wired so far; DVD and Blu-ray follow.\n", f);
}

/* Library state (key caches, key files) stays next to the program. */
static void config_dirs(void)
{
    char dir[PATH_MAX], sub[PATH_MAX + 32];

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
#define ONLY_DISC_INFO (-1)

/* A size in GB / MB of 1024^3 / 1024^2 bytes, one decimal, cut off (not rounded). */
static void size_text(char *buf, size_t size, int64_t bytes)
{
    int gb = bytes >= (1LL << 30);
    int64_t tenths = bytes * 10 / (gb ? 1LL << 30 : 1LL << 20);

    snprintf(buf, size, "%"PRId64".%d %s", tenths / 10, (int)(tenths % 10), gb ? "GB" : "MB");
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

/* One title's line (and, with detail, its files and streams) plus its JSON
 * record. */
static void title_overview(const DiscTitleInfo *t, int detail)
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
        nv += k->type == AVMEDIA_TYPE_VIDEO;
        na += k->type == AVMEDIA_TYPE_AUDIO;
        ns += k->type == AVMEDIA_TYPE_SUBTITLE;
        av_bprintf(&streams,
                 "%s{\"index\":%d,\"kind\":\"%s\",\"codec\":%s,\"language\":%s,\"inner_track\":%s,"
                 "\"from_track\":%s,\"forced\":%s,\"second_view\":%s,\"second_layer\":%s,\"offset_sequence\":%s,\"width\":%s,"
                 "\"height\":%s,\"channels\":%s,\"sample_rate\":%s,\"bits\":%s,\"bit_rate\":%s}",
                 i ? "," : "", i, av_get_media_type_string(k->type),
                 json_str(jc, sizeof(jc), codec_label(k->codec, AV_PROFILE_UNKNOWN)),
                 json_str(jsl, sizeof(jsl), k->language), json_index(ji, sizeof(ji), k->inner_track),
                 json_index(jf, sizeof(jf), k->from_track), k->forced ? "true" : "false",
                 k->second_view ? "true" : "false", k->second_layer ? "true" : "false",
                 json_index(jo, sizeof(jo), k->offset_sequence),
                 json_num(n1, sizeof(n1), k->width),
                 json_num(n2, sizeof(n2), k->height), json_num(n3, sizeof(n3), k->channels),
                 json_num(n4, sizeof(n4), k->sample_rate), json_num(n5, sizeof(n5), k->bits),
                 json_num(n6, sizeof(n6), k->bit_rate));
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
            char extra[64] = "", link[96] = "";
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
            if (k->offset_sequence >= 0)
                snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "  3-D offset sequence %d",
                         k->offset_sequence);
            log_text(LOG_INFO, "       %-2d %-8s %-8s %-3s%s%s", i, av_get_media_type_string(k->type),
                     codec_label(k->codec, AV_PROFILE_UNKNOWN), k->language, extra, link);
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
    log_json("title", "\"index\":%d,\"name\":%s,\"duration\":%.3f,\"chapters\":%d,\"size\":%"PRId64",\"language\":%s,"
             "\"selected\":%s,\"files\":[%s],\"tracks\":[%s]%s%s", t->index,
             json_str(js, sizeof(js), t->name ? t->name : ""), t->duration / (double)AV_TIME_BASE, t->nb_chapters,
             t->size, json_str(jl, sizeof(jl), t->language), t->selected ? "true" : "false", files, streams.str, extra,
             parts.str);
    av_bprint_finalize(&streams, NULL);
    av_bprint_finalize(&parts, NULL);
}

/* The disc line and the overview of its titles (with the JSON disc and title
 * records), from one open of source with the disc info only; detail: each
 * title's files and streams too. NULL when source cannot be read as a disc
 * (logged), else the demuxer to open its titles with. */
static const char *disc_overview(const char *source, int detail, int *nb_titles, int **nb_parts)
{
    AVFormatContext *ctx = NULL;
    const DiscInfo *d;
    char revs[32] = "", js1[64], js2[256], js3[64], js4[1024], jname[1100];
    int ret;

    if ((ret = open_title(DISC_FORMAT, source, ONLY_DISC_INFO, -1, &ctx)) < 0 || !(d = disc_info(ctx))) {
        log_msg(MSG_SOURCE_UNSUPPORTED, LOG_ERROR, "%s cannot be read as a disc this program supports (%s); "
                "HD DVD and Blu-ray are wired so far", source, av_err2str(ret < 0 ? ret : AVERROR_BUG));
        avformat_close_input(&ctx);
        return NULL;
    }
    *nb_titles = d->nb_titles;
    if (nb_parts && (*nb_parts = calloc(d->nb_titles ? d->nb_titles : 1, sizeof(**nb_parts))))
        for (int t = 0; t < d->nb_titles; t++)
            (*nb_parts)[t] = d->titles[t].nb_parts;
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
    log_json("disc", "\"kind\":%s,\"label\":%s,\"filesystem\":%s,\"encrypted\":%s,\"titles\":%d%s",
             json_str(js1, sizeof(js1), d->kind), json_str(js2, sizeof(js2), d->label ? d->label : ""),
             json_str(js3, sizeof(js3), d->filesystem ? d->filesystem : ""), d->encrypted ? "true" : "false",
             d->nb_titles, jname);
    for (int t = 0; t < d->nb_titles; t++)
        title_overview(&d->titles[t], detail);
    avformat_close_input(&ctx);
    return DISC_FORMAT;
}

/* "all" | "3" | "1,3-5" into a 0/1 list of nb_titles; returns the count
 * selected, or -1 (logged). */
static int parse_titles(const char *spec, int nb_titles, char *sel)
{
    int count = 0;
    const char *p = spec;

    memset(sel, 0, nb_titles);
    if (!strcmp(spec, "all")) {
        memset(sel, 1, nb_titles);
        return nb_titles;
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

/* The disc's name for the log file: the image's or folder's name. */
static void disc_name(const char *source, char *out, size_t size)
{
    char tmp[PATH_MAX], *base, *dot;
    size_t n;

    snprintf(tmp, sizeof(tmp), "%s", source);
    while ((n = strlen(tmp)) > 1 && tmp[n - 1] == '/')
        tmp[n - 1] = 0;
    base = strrchr(tmp, '/') ? strrchr(tmp, '/') + 1 : tmp;
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

static int cmd_info(const char *source)
{
    const char *format;
    int nb;

    log_msg(MSG_SOURCE, LOG_INFO, "Source: %s", source);
    if (!(format = disc_overview(source, 1, &nb, NULL)))
        return 1;
    return 0;
}

static int cmd_demux(const char *source, const char *titles, const char *out_dir, const char *split)
{
    char name[512], logp[PATH_MAX + 600], dbgp[PATH_MAX + 600], *sel, *spl = NULL;
    const char *format;
    int nb, count, ordinal = 0, failed = 0, done = 0, errors, warnings, *nb_parts = NULL;
    int64_t track_warnings = 0;

    if (mkdirs(out_dir) < 0) {
        log_msg(MSG_OUTPUT_FOLDER, LOG_ERROR, "The output folder %s cannot be created: %s", out_dir, strerror(errno));
        return 1;
    }
    disc_name(source, name, sizeof(name));
    snprintf(logp, sizeof(logp), "%s/%s.log", out_dir, name);
    snprintf(dbgp, sizeof(dbgp), "%s/%s_debug.log", out_dir, name);
    if (log_open_files(logp, setting_bool("log.debug_file") ? dbgp : NULL) < 0) {
        log_msg(MSG_LOG_FILE, LOG_ERROR, "The log %s cannot be written: %s", logp, strerror(errno));
        return 1;
    }
    log_reset_counts();
    log_msg(MSG_PROGRAM_START, LOG_INFO, "disc-remuxer %s — %s", DISC_REMUXER_VERSION, source);
    settings_log_changed();
    log_msg(MSG_OUTPUT_FOLDER, LOG_INFO, "Output folder: %s", out_dir);

    if (!(format = disc_overview(source, 0, &nb, &nb_parts)) || !nb_parts) {
        free(nb_parts);
        log_close_files();
        return 1;
    }
    sel = calloc(nb ? nb : 1, 1);
    spl = calloc(nb ? nb : 1, 1);
    if (!sel || !spl || (count = parse_titles(titles, nb, sel)) <= 0 ||
        (split && parse_titles(split, nb, spl) < 0)) {
        if (sel && spl && !count)
            log_msg(MSG_TITLE_SELECTION, LOG_ERROR, "No title selected");
        free(sel);
        free(spl);
        free(nb_parts);
        log_close_files();
        return 2;
    }
    /* which titles are written as parts: --split (these), the setting (every title) */
    for (int t = 0; t < nb; t++)
        if (spl[t] && sel[t] && !nb_parts[t])
            log_msg(MSG_TITLE_SELECTION, LOG_INFO, "Title #%d has no parts (it plays no whole stream files back to "
                    "back): written whole", t);
    if (setting_bool("output.split_joined_titles"))
        memset(spl, 1, nb);
    for (int t = 0; t < nb; t++) {
        int parts = spl[t] ? nb_parts[t] : 0, part_failed = 0;
        if (!sel[t])
            continue;
        ++ordinal;
        for (int p = parts ? 0 : -1; p < parts; p++) {
            TitleJob job = { format, source, t, p, parts, ordinal, count, out_dir, count > 1 };
            TitleOutcome res;
            demux_title(&job, &res);
            part_failed    += res.failed;
            track_warnings += res.warnings;
        }
        failed += !!part_failed;
        done   += !part_failed;
    }
    free(sel);
    free(spl);
    free(nb_parts);

    log_counts(&errors, &warnings);
    log_step("=== Summary ===");
    log_msg(MSG_SUMMARY, failed ? LOG_ERROR : warnings ? LOG_INFO : LOG_OK,
            "%d title%s written, %d failed; %d warning%s, %d error%s", done, done == 1 ? "" : "s", failed, warnings,
            warnings == 1 ? "" : "s", errors, errors == 1 ? "" : "s");
    log_msg(MSG_LOG_FILE, LOG_INFO, "Log: %s", logp);
    log_json("result", "\"written\":%d,\"failed\":%d,\"warnings\":%d,\"errors\":%d", done, failed, warnings, errors);
    log_close_files();
    (void)track_warnings;
    return failed ? 1 : 0;
}

static void cmd_version(void)
{
    unsigned v = avformat_version();
    printf("disc-remuxer %s\n", DISC_REMUXER_VERSION);
    printf("FFmpeg %s (libavformat %u.%u.%u)\n", av_version_info(), v >> 16, v >> 8 & 0xff, v & 0xff);
}

int main(int argc, char **argv)
{
    const char *config = NULL, *split = NULL, **sets = calloc(argc, sizeof(char *));
    int json = 0, verbose = 0, nb_sets = 0, i, ret;

    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--json"))
            json = 1;
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-vv"))
            verbose = 2;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc)
            sets[nb_sets++] = argv[++i];
        else if (!strcmp(argv[i], "--split") && i + 1 < argc)
            split = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "disc-remuxer: unknown option %s\n\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }

    log_init(json, verbose);
    if (settings_load(config) < 0)
        return 1;
    for (int k = 0; k < nb_sets; k++)
        if (settings_set(sets[k]) < 0)
            return 2;
    config_dirs();

    if (!strcmp(argv[i], "version")) {
        cmd_version();
        ret = 0;
    } else if (!strcmp(argv[i], "settings")) {
        if (i + 1 < argc && !strcmp(argv[i + 1], "path"))
            printf("%s\n", settings_path());
        else
            settings_show();
        ret = 0;
    } else if (!strcmp(argv[i], "info") && i + 1 < argc) {
        ret = cmd_info(argv[i + 1]);
    } else if (!strcmp(argv[i], "demux") && i + 3 < argc) {
        ret = cmd_demux(argv[i + 1], argv[i + 2], argv[i + 3], split);
    } else {
        fprintf(stderr, "disc-remuxer: unknown command or missing arguments: %s\n\n", argv[i]);
        usage(stderr);
        ret = 2;
    }
    free(sets);
    return ret;
}
