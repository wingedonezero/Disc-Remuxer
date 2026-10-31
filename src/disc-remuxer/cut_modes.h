/*
 * disc-remuxer: the cut editor's analysis modes. Each finds where the
 * episodes of a title split from its chapters (or takes the places given)
 * the way the Remux-Toolkit's MKV splitter does for a Matroska file: the
 * same modes, values, steps and words in its log, and the same split
 * points (chapters to split before, or times).
 */

#ifndef DISC_REMUXER_CUT_MODES_H
#define DISC_REMUXER_CUT_MODES_H

#include <stdint.h>

enum CutMode {
    CUT_MODE_TIME = 0,       /* Time-based Grouping */
    CUT_MODE_PATTERN,        /* Pattern Recognition */
    CUT_MODE_GAP,            /* Statistical Gap Analysis */
    CUT_MODE_SHORTEST,       /* Shortest Chapter Analysis */
    CUT_MODE_EPISODES,       /* Manual Episode Count */
    CUT_MODE_REMOVE_END,     /* Remove Chapters from End */
    CUT_MODE_CHAPTERS,       /* Before Chapters (Manual) */
    CUT_MODE_TIMESTAMPS,     /* After Timestamps (Manual) */
    CUT_MODE_COUNT
};

/* The modes' names as shown, and as the setting cuts.helper_mode takes
 * them. */
extern const char *const cut_mode_names[CUT_MODE_COUNT];
extern const char *const cut_mode_keys[CUT_MODE_COUNT];
int cut_mode_of_key(const char *key);

typedef struct CutModeInput {
    int                mode;
    const int64_t     *chapters;       /* the chapters' starts, ns (as a Matroska file has them) */
    const char *const *names;          /* their names (NULL: none) */
    int                nb_chapters;
    int64_t            duration;       /* the title's length, ns (0: not known) */
    double             target;         /* minutes (cut_mode_target()) */
    double             min_minutes;    /* min content duration */
    int                episodes;       /* expected number of episodes */
    int                from_end;       /* chapters to remove from the end */
    const int         *ticked;         /* Before Chapters: the chapters ticked (from 1) */
    int                nb_ticked;
    const char        *timestamps;     /* After Timestamps: as typed */
} CutModeInput;

typedef struct CutModeResult {
    char  *log;                        /* the analysis, lines in "\n" */
    int   *chapters;                   /* split before these chapters (from 1), in order */
    int    nb_chapters;
    char **stamps;                     /* or after these times ("HH:MM:SS.nnnnnnnnn") */
    int    nb_stamps;
} CutModeResult;

/* Runs the mode. 0, or a negative base error (no memory). */
int  cut_mode_run(const CutModeInput *in, CutModeResult *out);
void cut_mode_result_free(CutModeResult *r);

/* The target episode duration as typed: "mm:ss" or minutes ("23",
 * "23.5"). 0 and *minutes, or -1 and why in err. */
int  cut_mode_target(const char *text, double *minutes, char *err, int size);
/* A chapter's start (ns) as the splitter writes it ("0:22:52.204"). */
void cut_mode_start_text(int64_t ns, char *buf, int size);
/* Minutes as the target field shows them ("23:40"). */
void cut_mode_target_text(double minutes, char *buf, int size);

#endif /* DISC_REMUXER_CUT_MODES_H */
