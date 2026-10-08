/*
 * Hooks for the tests (tests/unit/test_vm_*.py, test_dvdnav_*.py): drive libdvdnav's VM
 * directly with a hand-made state, so navigation behaviour can be tested without a disc,
 * and play a built disc folder with every event recorded.
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <dvdread/nav_types.h>
#include <dvdread/ifo_read.h>
#include "dvdnav/dvdnav.h"
#include "decoder.h"
#include "vm.h"
#include "dvdnav_selftest.h"

/* Runs one VM command (8 bytes, DVD-Video command format) in a title-domain
 * program chain of nr_of_programs programs, positioned at program pgN.
 * Returns what vm_exec_cmd() returns; *failures / *first receive the VM's
 * broken-assumption record afterwards. */
int dr_selftest_vm_exec(const uint8_t command[8], int nr_of_programs, int pgN,
                        unsigned *failures, const char **first)
{
    static const char what_none[] = "";
    vm_t *vm = vm_new_vm(NULL, NULL);
    pgc_t *pgc = calloc(1, sizeof(*pgc));
    pgc_program_map_t *map = calloc(nr_of_programs > 0 ? nr_of_programs : 1,
                                    sizeof(*map));
    vm_cmd_t cmd;
    int ret = -1;

    if (!vm || !pgc || !map)
        goto end;
    for (int i = 0; i < nr_of_programs; i++)
        map[i] = (pgc_program_map_t)(i + 1);
    pgc->nr_of_programs = nr_of_programs;
    pgc->nr_of_cells    = nr_of_programs;
    pgc->program_map    = map;

    vm->state.domain = DVD_DOMAIN_VTSTitle;
    vm->state.pgc    = pgc;
    vm->state.pgN    = pgN;
    vm->state.cellN  = pgN;

    memcpy(cmd.bytes, command, 8);
    ret = vm_exec_cmd(vm, &cmd);
    *failures = vm->failures;
    *first    = vm->first_failure ? vm->first_failure : what_none;

end:
    free(map);
    free(pgc);
    free(vm);
    return ret;
}

/* Runs nb_commands VM commands (8 bytes each) one after another in a
 * title-domain program chain of nr_of_programs programs at program pgN,
 * advancing the playback clock by ticks_between (units of 512/90000 s)
 * before each command after the first, with an optional source for the Rnd
 * operation. Returns what the last vm_exec_cmd() returned; gprm[16] receives
 * the GPRMs afterwards (counters read as the VM reads them), *pg_n the
 * program, *failures the broken-assumption count, *ignored / *ign_reg /
 * *ign_value the ignored counter sets and *sprm8 the highlighted button
 * register (SPRM 8). */
int dr_selftest_vm_run(const uint8_t *commands, int nb_commands, int nr_of_programs, int pgN,
                       uint32_t ticks_between, int (*rnd)(void *), void *rnd_priv,
                       uint16_t gprm[16], int *pg_n, unsigned *failures,
                       unsigned *ignored, int *ign_reg, int *ign_value, uint16_t *sprm8)
{
    vm_t *vm = vm_new_vm(NULL, NULL);
    pgc_t *pgc = calloc(1, sizeof(*pgc));
    pgc_program_map_t *map = calloc(nr_of_programs > 0 ? nr_of_programs : 1, sizeof(*map));
    int ret = -1;

    if (!vm || !pgc || !map)
        goto end;
    for (int i = 0; i < nr_of_programs; i++)
        map[i] = (pgc_program_map_t)(i + 1);
    pgc->nr_of_programs = nr_of_programs;
    pgc->nr_of_cells    = nr_of_programs;
    pgc->program_map    = map;

    vm->state.domain = DVD_DOMAIN_VTSTitle;
    vm->state.pgc    = pgc;
    vm->state.pgN    = pgN;
    vm->state.cellN  = pgN;
    vm->rnd_fn       = rnd;
    vm->rnd_priv     = rnd_priv;

    for (int i = 0; i < nb_commands; i++) {
        vm_cmd_t cmd;

        if (i)
            vm->state.registers.time_counter += ticks_between;
        memcpy(cmd.bytes, commands + 8 * i, 8);
        ret = vm_exec_cmd(vm, &cmd);
    }
    for (int r = 0; r < 16; r++) {
        if (vm->state.registers.GPRM_mode & (1 << r))
            vm->state.registers.GPRM[r] = (uint16_t)(((vm->state.registers.time_counter -
                                                       vm->state.registers.GPRM_time[r]) * 32u) / 5625u);
        gprm[r] = vm->state.registers.GPRM[r];
    }
    *pg_n      = vm->state.pgN;
    *failures  = vm->failures;
    *ignored   = vm->state.registers.counter_sets_ignored;
    *ign_reg   = vm->state.registers.ignored_counter_reg;
    *ign_value = vm->state.registers.ignored_counter_value;
    *sprm8     = vm->state.registers.SPRM[8];

end:
    free(map);
    free(pgc);
    free(vm);
    return ret;
}

/* ---- a navigator playing a disc folder, every event recorded ---- */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dvdread/dvd_filesystem.h>

/* File callbacks over the host file system (dvdnav_open_files), counting the
 * files they open, so a test can see the reads went through them. */
typedef struct {
    dvd_reader_filesystem_h fs;
    unsigned *opened;
} host_files;

static int hf_stat(dvd_reader_filesystem_h *fs, const char *path, dvdstat_t *st)
{
    struct stat s;
    if (stat(path, &s) < 0)
        return -1;
    st->size    = (uint64_t)s.st_size;
    st->st_mode = S_ISDIR(s.st_mode) ? DVD_S_IFDIR : S_ISREG(s.st_mode) ? DVD_S_IFREG : 0;
    return 0;
}

static void *hf_dir_open(dvd_reader_filesystem_h *fs, const char *name) { return opendir(name); }

static int hf_dir_read(void *dir, dvd_dirent_t *entry)
{
    struct dirent *e = readdir(dir);
    if (!e)
        return 1;
    snprintf(entry->d_name, sizeof(entry->d_name), "%s", e->d_name);
    return 0;
}

static void hf_dir_close(void *dir) { closedir(dir); }

static void *hf_file_open(dvd_reader_filesystem_h *fs, const char *name)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0)
        return NULL;
    (*((host_files *)fs)->opened)++;
    return (void *)(intptr_t)(fd + 1);
}

static ssize_t hf_file_read(void *f, char *buf, size_t size) { return read((int)(intptr_t)f - 1, buf, size); }
static off64_t hf_file_seek(void *f, off64_t off, int whence) { return lseek((int)(intptr_t)f - 1, off, whence); }
static int hf_file_close(void *f) { return close((int)(intptr_t)f - 1); }
static void hf_close(dvd_reader_filesystem_h *fs) { free(fs); }

/* Opens the disc folder, plays title (dvdnav_title_play) and steps the
 * navigator up to max_steps times or to a STOP, skipping stills and waits;
 * the recorded events go to ev[max_events]. */
int dr_nav_trace(const char *folder, int title, unsigned opts, int max_steps,
                 dr_nav_event *ev, int max_events, dr_nav_result *res)
{
    dvdnav_t *nav = NULL, *copy = NULL, *play;
    uint8_t buf[DVD_VIDEO_LB_LEN];
    unsigned opened = 0;

    memset(res, 0, sizeof(*res));
    if (opts & DR_NAV_FILES) {
        host_files *hf = calloc(1, sizeof(*hf));
        if (!hf)
            return res->status = -1;
        hf->fs = (dvd_reader_filesystem_h){ NULL, hf_close, hf_stat, hf_dir_open, hf_dir_read, hf_dir_close,
                                            hf_file_open, hf_file_read, hf_file_seek, hf_file_close };
        hf->opened = &opened;
        if (dvdnav_open_files(&nav, NULL, NULL, folder, &hf->fs) != DVDNAV_STATUS_OK)
            return res->status = -1;
    } else if (dvdnav_open(&nav, folder) != DVDNAV_STATUS_OK) {
        return res->status = -1;
    }
    dvdnav_set_readahead_flag(nav, 0);
    dvdnav_set_PGC_positioning_flag(nav, 1);
    dvdnav_set_title_play_follows_jumps(nav, !!(opts & DR_NAV_FOLLOW_JUMPS));
    dvdnav_set_nav_only_flag(nav, !!(opts & DR_NAV_NAV_ONLY));
    play = nav;
    if (opts & DR_NAV_COPY) {
        if (dvdnav_dup(&copy, nav) != DVDNAV_STATUS_OK) {
            res->status = -3;
            goto end;  /* the original's record */
        }
        play = copy;
    }
    if (dvdnav_title_play(play, title) != DVDNAV_STATUS_OK) {
        res->status = -2;
        goto end;
    }

    for (res->steps = 0; res->steps < max_steps; res->steps++) {
        int32_t event, len = 0;
        dr_nav_event e = { 0, -2, 0, 0, 0, 0, 0 };

        if (dvdnav_get_next_block(play, buf, &event, &len) != DVDNAV_STATUS_OK)
            break;
        if (event == DVDNAV_BLOCK_OK) {
            if (len > 0)
                res->data_blocks++;
            else
                res->empty_blocks++;
            continue;
        }
        if (event != DVDNAV_CELL_CHANGE && event != DVDNAV_NAV_PACKET && event != DVDNAV_STILL_FRAME &&
            event != DVDNAV_WAIT && event != DVDNAV_STOP)
            continue;
        e.code = event;
        if (dvdnav_current_title_program2(play, &e.title, &e.vtsn, &e.pgcn, &e.pgn, &e.celln) != DVDNAV_STATUS_OK)
            e.title = -2;
        if (event == DVDNAV_STILL_FRAME)
            e.value = ((dvdnav_still_event_t *)buf)->length;
        else if (event == DVDNAV_NAV_PACKET)
            e.value = (int)dvdnav_get_current_nav_pci(play)->pci_gi.nv_pck_lbn;
        if (res->events < max_events)
            ev[res->events] = e;
        res->events++;
        if (event == DVDNAV_STILL_FRAME)
            dvdnav_still_skip(play);
        else if (event == DVDNAV_WAIT)
            dvdnav_wait_skip(play);
        else if (event == DVDNAV_STOP)
            break;
    }

end:
    res->failures   = dvdnav_get_vm_failures(play, NULL);
    res->clock      = dvdnav_get_absolute_time(play);
    res->sprm_flags = dvdnav_get_prm(play, DVDNAV_PRM_SPRM_FLAGS, 0);
    if (copy)
        dvdnav_free_dup(copy);
    dvdnav_close(nav);
    res->files_opened = opened;
    return res->status;
}
