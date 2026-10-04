/*
 * Hooks for this crate's tests: drive libdvdnav's VM directly with a
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
