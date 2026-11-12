/*
 * disc-remuxer: rips disc titles into stream files (MKV to follow).
 *
 *   disc-remuxer [options] <command> ...
 *
 * Commands decide what one run does; settings (disc-remuxer.toml next to
 * the program) decide how output is made. Every run that writes output
 * leaves a log in its output folder. The work is the engine's (engine.h),
 * which the GUI calls too; this file reads the command line.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#include "analyze/analyze.h"
#include "engine.h"
#include "base/settings.h"

static void usage(FILE *f)
{
    fputs("usage: disc-remuxer [options] <command> ...\n"
          "\n"
          "commands:\n"
          "  info  <source>                        the titles and tracks of a disc\n"
          "  demux <source> <titles> [<out folder>]  each title as one MKV file: its stream files muxed by\n"
          "                                        mkvmerge (the default, setting output.format = mkvmerge);\n"
          "                                        or the stream files, chapters and a log as they are\n"
          "                                        (output.format = streams), or the program's own MKV (mkv);\n"
          "                                        no out folder: <main output folder>/<disc name>\n"
          "                                        (setting output.folder; empty: output next to the program);\n"
          "                                        a folder of discs: each disc in turn into its own folder\n"
          "                                        (the out folder: the main output folder for them)\n"
          "  cuts  <source> <title> <cuts>         where cuts in a title fall (nothing written): the title's\n"
          "                                        video is read as a rip reads it; each cut is said with\n"
          "                                        the key frame it takes and the part it starts. <cuts>:\n"
          "                                        c<chapter> or a time on the title's timeline (h:mm:ss.fff\n"
          "                                        or m:ss.fff), comma separated, e.g. c5,45:42.322; a time\n"
          "                                        not on a key frame: setting cuts.key_frame; with\n"
          "                                        --split <title>: inside the title's own parts\n"
          "  frame <source> <title> <time> <file.bmp>  the picture shown at that time on the title's\n"
          "                                        timeline, as the cut editor shows it, as a BMP file\n"
          "  suggest-cuts <source> <title>         the cut editor's analysis of a title's chapters (the mode\n"
          "                                        and values of settings cuts.helper_*, e.g. --set\n"
          "                                        cuts.helper_target=23:40): its log, the cuts it suggests,\n"
          "                                        where their parts start and the --cut value\n"
          "  find  <folder>                        the discs in a folder (as the GUI's queue takes them)\n"
          "  settings [show|path]                  the settings in effect / where the settings file is\n"
          "  version                               program and library versions\n"
          "\n"
          "  <source>  a disc image (.iso) or a disc folder\n"
          "  <titles>  all | 3 | 1,3-5   (numbers as info lists them, from 0; all: the titles not shorter\n"
          "            than the minimum length, a number: that title whatever its length)\n"
          "\n"
          "options:\n"
          "  --config <file>      settings file to use (default: disc-remuxer.toml next to the program)\n"
          "  --set group.key=val  change a setting for this run only (repeatable)\n"
          "  --json               JSON records on stdout, one per line, instead of text\n"
          "  --details            info: also every detail the GUI shows (its queue lines and its details\n"
          "                       panel's rows for the disc, each title, track and chapter); demux: the\n"
          "                       queue's states as the GUI shows them while it rips\n"
          "  --second-view <titles>  demux: also write the second view of these 3-D titles (the .mvc\n"
          "                       next to the base view); setting bluray.second_view does it for every title\n"
          "  --tracks [<title>:]<steps>  which tracks: every one to start with, then steps left to right,\n"
          "                       + (rip) or - (leave out) and all, video, audio, subtitle, forced,\n"
          "                       lossless (the lossless audio, or all audio when there is none), a\n"
          "                       language (eng, fr, ...) or a track number; a kind of track may be limited\n"
          "                       to languages (audio=eng/fre, subtitle=English); e.g. \"-subtitle,+eng\"\n"
          "                       (every title), \"3:-2,-5\" (title 3 only), \"-audio,+lossless=eng\"; a track\n"
          "                       number takes its forced subtitles and core along; repeatable; info shows\n"
          "                       the result\n"
          "  --split <titles>     demux: write these titles' parts one by one (a title that plays whole\n"
          "                       stream files back to back, e.g. episodes; info lists the parts);\n"
          "                       setting output.split_joined_titles does it for every title\n"
          "  --cut <title>:<cuts>  demux: write this title in parts cut at these places (c<chapter> or a\n"
          "                       time on the title's timeline, comma separated; the cuts command shows\n"
          "                       where they fall); repeatable, one title each; a title also written\n"
          "                       as its parts (--split) is split at the disc's own breaks first and\n"
          "                       each cut is made inside the part it falls in; a 3-D second view is\n"
          "                       left out of the parts (said in the log); setting cuts.lossless_to_flac\n"
          "                       writes such a title's TrueHD / DTS-HD Master Audio as FLAC, cut\n"
          "                       between two samples (else they start at their next start unit)\n"
          "  -v, -vv              more detail on the terminal\n"
          "\n"
          "DVD-Video, HD DVD and Blu-ray (UHD, 3-D) are wired.\n", f);
}

/* Ctrl+C: the run stops cleanly (the title being written ends, said in the
 * log); a second Ctrl+C ends the program at once. */
static void on_interrupt(int sig)
{
    static const char msg[] = "\nStopping (Ctrl+C again ends at once)...\n";
    engine_cancel();
    signal(sig, SIG_DFL);
    if (write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0)
        return;
}

static void cmd_version(void)
{
    char ff[256];
    printf("disc-remuxer %s\n", DISC_REMUXER_VERSION);
    printf("%s\n", dr_analyze_version(ff, sizeof(ff)));
}

int main(int argc, char **argv)
{
    const char *config = NULL, *split = NULL, *second = NULL, **sets = calloc(argc, sizeof(char *));
    const char **tracks = calloc(argc, sizeof(char *)), **cuts = calloc(argc, sizeof(char *));
    int json = 0, verbose = 0, details = 0, nb_sets = 0, nb_tracks = 0, nb_cuts = 0, i, ret;
    EngineChoice c;

    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--json"))
            json = 1;
        else if (!strcmp(argv[i], "--details"))
            details = 1;
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
        else if (!strcmp(argv[i], "--second-view") && i + 1 < argc)
            second = argv[++i];
        else if (!strcmp(argv[i], "--cut") && i + 1 < argc)
            cuts[nb_cuts++] = argv[++i];
        else if (!strcmp(argv[i], "--tracks") && i + 1 < argc)
            tracks[nb_tracks++] = argv[++i];
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
    /* what follows the command: no more than it takes, and no option (they
     * go before the command; one after it would be ignored) */
    {
        const char *cmd = argv[i];
        int most = !strcmp(cmd, "settings") || !strcmp(cmd, "info") || !strcmp(cmd, "find") ? 1 :
                   !strcmp(cmd, "suggest-cuts") ? 2 :
                   !strcmp(cmd, "demux") || !strcmp(cmd, "cuts") ? 3 : !strcmp(cmd, "frame") ? 4 :
                   !strcmp(cmd, "version") ? 0 : -1;   /* -1: an unknown command */
        for (int k = i + 1; k < argc; k++) {
            if (argv[k][0] == '-' && argv[k][1]) {
                fprintf(stderr, "disc-remuxer: option %s after the command %s: options go before the command "
                        "(disc-remuxer [options] %s ...)\n", argv[k], cmd, cmd);
                return 2;
            }
            if (most >= 0 && k - i > most) {
                fprintf(stderr, "disc-remuxer: %s takes at most %d argument%s: %s is one too many\n\n", cmd, most,
                        most == 1 ? "" : "s", argv[k]);
                usage(stderr);
                return 2;
            }
        }
    }

    if ((ret = engine_init(config, sets, nb_sets, json, verbose)))
        return ret;
    signal(SIGINT, on_interrupt);
    c = (EngineChoice){ tracks, nb_tracks, second, cuts, nb_cuts };
    if (engine_check_choice(&c) < 0)
        return 2;

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
        ret = engine_info(argv[i + 1], &c, details);
    } else if (!strcmp(argv[i], "frame") && i + 4 < argc) {
        ret = engine_frame(argv[i + 1], argv[i + 2], argv[i + 3], argv[i + 4], &c);
    } else if (!strcmp(argv[i], "suggest-cuts") && i + 2 < argc) {
        ret = engine_suggest_cuts(argv[i + 1], argv[i + 2], &c);
    } else if (!strcmp(argv[i], "cuts") && i + 3 < argc) {
        ret = engine_cuts(argv[i + 1], argv[i + 2], argv[i + 3], split, &c);
    } else if (!strcmp(argv[i], "find") && i + 1 < argc) {
        ret = engine_find(argv[i + 1]);
    } else if (!strcmp(argv[i], "demux") && i + 2 < argc) {
        ret = engine_demux(argv[i + 1], argv[i + 2], i + 3 < argc ? argv[i + 3] : NULL, split, &c, details);
    } else {
        fprintf(stderr, "disc-remuxer: unknown command or missing arguments: %s\n\n", argv[i]);
        usage(stderr);
        ret = 2;
    }
    free(sets);
    free(tracks);
    free(cuts);
    return ret;
}
