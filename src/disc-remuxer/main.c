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
#include <libavutil/avutil.h>

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

static int64_t meta_int(AVDictionary *m, const char *key, int64_t def)
{
    const AVDictionaryEntry *e = av_dict_get(m, key, NULL, 0);
    return e ? strtoll(e->value, NULL, 10) : def;
}

/* The format demuxers, tried in this order. Each one follows the same
 * contract (docs/FORMATS.md): the disc and its titles in the metadata on
 * open, each track's result at the end, read progress as an option. */
static const char *const formats[] = { "hddvd", NULL };

/* Which demuxer reads source (NULL: none, logged); logs the disc line. */
static const char *source_format(const char *source, int *nb_titles)
{
    AVFormatContext *ctx = NULL;
    const char *format = NULL;
    int ret = AVERROR_INVALIDDATA;

    for (int i = 0; formats[i] && !format; i++)
        if ((ret = open_title(formats[i], source, 0, &ctx)) >= 0)
            format = formats[i];
    if (!format) {
        log_msg(MSG_SOURCE_UNSUPPORTED, LOG_ERROR, "%s cannot be read as a disc this program supports (%s); "
                "only HD DVD images are wired so far", source, av_err2str(ret));
        return NULL;
    }
    *nb_titles = (int)meta_int(ctx->metadata, "titles", 0);
    {
        const AVDictionaryEntry *disc  = av_dict_get(ctx->metadata, "disc", NULL, 0);
        const AVDictionaryEntry *label = av_dict_get(ctx->metadata, "label", NULL, 0);
        const AVDictionaryEntry *fs    = av_dict_get(ctx->metadata, "filesystem", NULL, 0);
        int64_t rev = meta_int(ctx->metadata, "udf_revision", 0);
        char revs[32] = "", js1[64], js2[256], js3[64];
        if (rev)
            snprintf(revs, sizeof(revs), " %x.%02x", (unsigned)(rev >> 8), (unsigned)(rev & 0xff));
        log_msg(MSG_SOURCE, LOG_INFO, "Disc: %s '%s', %s%s, %s, %d title%s", disc ? disc->value : format,
                label ? label->value : "", fs ? fs->value : "?", revs,
                meta_int(ctx->metadata, "encrypted", 0) ? "encrypted" : "not encrypted",
                *nb_titles, *nb_titles == 1 ? "" : "s");
        log_json("disc", "\"kind\":%s,\"label\":%s,\"filesystem\":%s,\"encrypted\":%s,\"titles\":%d",
                 json_str(js1, sizeof(js1), disc ? disc->value : format), json_str(js2, sizeof(js2), label ? label->value : ""),
                 json_str(js3, sizeof(js3), fs ? fs->value : ""), meta_int(ctx->metadata, "encrypted", 0) ? "true" : "false",
                 *nb_titles);
    }
    avformat_close_input(&ctx);
    return format;
}

/* The overview of a disc's titles (and JSON title records). */
static void overview(const char *format, const char *source, int nb_titles)
{
    for (int t = 0; t < nb_titles; t++) {
        AVFormatContext *ctx = NULL;
        const AVDictionaryEntry *e;
        char dur[32], tracks[1024] = "", js[512];
        int nv = 0, na = 0, ns = 0, ret;

        if ((ret = open_title(format, source, t, &ctx)) < 0) {
            log_text(LOG_WARNING, "  #%-2d cannot be opened: %s", t, av_err2str(ret));
            continue;
        }
        e = av_dict_get(ctx->metadata, "title", NULL, 0);
        hms(dur, sizeof(dur), ctx->duration);
        for (unsigned i = 0; i < ctx->nb_streams; i++) {
            const AVCodecParameters *p = ctx->streams[i]->codecpar;
            const AVDictionaryEntry *l = av_dict_get(ctx->streams[i]->metadata, "language", NULL, 0);
            char jc[64], jl[16];
            nv += p->codec_type == AVMEDIA_TYPE_VIDEO;
            na += p->codec_type == AVMEDIA_TYPE_AUDIO;
            ns += p->codec_type == AVMEDIA_TYPE_SUBTITLE;
            snprintf(tracks + strlen(tracks), sizeof(tracks) - strlen(tracks), "%s{\"index\":%u,\"kind\":\"%s\","
                     "\"codec\":%s,\"language\":%s}", i ? "," : "", i, av_get_media_type_string(p->codec_type),
                     json_str(jc, sizeof(jc), codec_label(p->codec_id, p->profile)), json_str(jl, sizeof(jl), l ? l->value : ""));
        }
        log_text(LOG_INFO, "  #%-2d %-40s %9s  %-6s %d audio, %d subtitle%s, %"PRId64" chapter%s", t,
                 e ? e->value : "", dur, nv ? codec_label(ctx->streams[0]->codecpar->codec_id, 0) : "-",
                 na, ns, ns == 1 ? "" : "s", meta_int(ctx->metadata, "chapters", 0),
                 meta_int(ctx->metadata, "chapters", 0) == 1 ? "" : "s");
        log_json("title", "\"index\":%d,\"name\":%s,\"duration\":%.3f,\"chapters\":%"PRId64",\"tracks\":[%s]", t,
                 json_str(js, sizeof(js), e ? e->value : ""), ctx->duration / (double)AV_TIME_BASE,
                 meta_int(ctx->metadata, "chapters", 0), tracks);
        avformat_close_input(&ctx);
    }
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
    if (!(format = source_format(source, &nb)))
        return 1;
    overview(format, source, nb);
    return 0;
}

static int cmd_demux(const char *source, const char *titles, const char *out_dir)
{
    char name[512], logp[PATH_MAX + 600], dbgp[PATH_MAX + 600], *sel;
    const char *format;
    int nb, count, ordinal = 0, failed = 0, done = 0, errors, warnings;
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

    if (!(format = source_format(source, &nb))) {
        log_close_files();
        return 1;
    }
    overview(format, source, nb);
    sel = calloc(nb ? nb : 1, 1);
    if ((count = parse_titles(titles, nb, sel)) <= 0) {
        if (!count)
            log_msg(MSG_TITLE_SELECTION, LOG_ERROR, "No title selected");
        free(sel);
        log_close_files();
        return 2;
    }
    for (int t = 0; t < nb; t++) {
        TitleJob job = { format, source, t, 0, count, out_dir, count > 1 };
        TitleOutcome res;
        if (!sel[t])
            continue;
        job.ordinal = ++ordinal;
        demux_title(&job, &res);
        failed += res.failed;
        done   += !res.failed;
        track_warnings += res.warnings;
    }
    free(sel);

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
    const char *config = NULL, **sets = calloc(argc, sizeof(char *));
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
        ret = cmd_demux(argv[i + 1], argv[i + 2], argv[i + 3]);
    } else {
        fprintf(stderr, "disc-remuxer: unknown command or missing arguments: %s\n\n", argv[i]);
        usage(stderr);
        ret = 2;
    }
    free(sets);
    return ret;
}
