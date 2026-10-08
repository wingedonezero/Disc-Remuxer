/*
 * The test hooks of dvdnav_selftest.c (compiled with libdvdnav's private
 * headers): libdvdnav's VM driven directly, and a navigator playing a disc
 * folder with every event recorded.
 */

#ifndef DVDNAV_SELFTEST_H
#define DVDNAV_SELFTEST_H

#include <stdint.h>

/* libdvdnav's VM (src/vm/rand.h) */
uint32_t vm_rand_next(uint32_t *state);
void vm_rand_seed(uint32_t *state, uint32_t value);
int vm_rand_shuffle(uint32_t state, unsigned int bound, unsigned int step);

int dr_selftest_vm_exec(const uint8_t command[8], int nr_of_programs, int pgN,
                        unsigned *failures, const char **first);
int dr_selftest_vm_run(const uint8_t *commands, int nb_commands, int nr_of_programs, int pgN,
                       uint32_t ticks_between, int (*rnd)(void *), void *rnd_priv,
                       uint16_t gprm[16], int *pg_n, unsigned *failures,
                       unsigned *ignored, int *ign_reg, int *ign_value, uint16_t *sprm8);

/* What one event of a played title left: the event code (DVDNAV_*), the
 * position then (dvdnav_current_title_program2; title -2 when it failed)
 * and a value: the still length (STILL_FRAME), the PCI's nv_pck_lbn
 * (NAV_PACKET), 0 otherwise. */
typedef struct {
    int code, title, vtsn, pgcn, pgn, celln, value;
} dr_nav_event;

/* What the whole run left. */
typedef struct {
    int status;              /* 0 played to the step limit or a STOP, -1 open failed, -2 title play failed, -3 copy failed */
    int steps;               /* dvdnav_get_next_block() calls */
    int events;              /* events recorded (at most max_events are kept) */
    unsigned data_blocks;    /* BLOCK_OK with data */
    unsigned empty_blocks;   /* BLOCK_OK without data (navigation-only mode) */
    unsigned failures;       /* dvdnav_get_vm_failures() */
    uint64_t clock;          /* dvdnav_get_absolute_time(), 90 kHz ticks */
    unsigned files_opened;   /* files opened through the callbacks (DR_NAV_FILES) */
    uint32_t sprm_flags;     /* dvdnav_get_prm(DVDNAV_PRM_SPRM_FLAGS) */
} dr_nav_result;

enum {
    DR_NAV_FOLLOW_JUMPS = 1, /* dvdnav_set_title_play_follows_jumps(1) */
    DR_NAV_NAV_ONLY     = 2, /* dvdnav_set_nav_only_flag(1) */
    DR_NAV_COPY         = 4, /* play a copy (dvdnav_dup) made right after the open */
    DR_NAV_FILES        = 8, /* open through file callbacks (dvdnav_open_files) */
};

/* Opens the disc folder, plays title (dvdnav_title_play) and steps the
 * navigator up to max_steps times or to a STOP, skipping stills and waits;
 * the recorded events go to ev[max_events]. The Rnd operation draws from a
 * fixed sequence (the same in every trace). Returns res->status. */
int dr_nav_trace(const char *folder, int title, unsigned opts, int max_steps,
                 dr_nav_event *ev, int max_events, dr_nav_result *res);

/* The same on a navigator opened elsewhere (closed at the end). */
struct dvdnav_s;
int dr_nav_play(struct dvdnav_s *nav, int title, unsigned opts, int max_steps,
                dr_nav_event *ev, int max_events, dr_nav_result *res);

#endif
