/*
 * Hooks for the tests (tests/unit/test_vm_*.py): drive libdvdnav's VM directly with a
 * hand-made state, so navigation behaviour can be tested without a disc.
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <dvdread/nav_types.h>
#include <dvdread/ifo_read.h>
#include "dvdnav/dvdnav.h"
#include "decoder.h"
#include "vm.h"

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
 * program, *failures the broken-assumption count and *ignored / *ign_reg /
 * *ign_value the ignored counter sets. */
int dr_selftest_vm_run(const uint8_t *commands, int nb_commands, int nr_of_programs, int pgN,
                       uint32_t ticks_between, int (*rnd)(void *), void *rnd_priv,
                       uint16_t gprm[16], int *pg_n, unsigned *failures,
                       unsigned *ignored, int *ign_reg, int *ign_value)
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

end:
    free(map);
    free(pgc);
    free(vm);
    return ret;
}
