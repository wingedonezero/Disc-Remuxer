//! libdvdnav (DVD-Video navigation), compiled from `libs/libdvdnav` by
//! `build.rs` against our libdvdread and linked statically.
//!
//! Declarations are added here as the workspace starts to call the library
//! directly; today it is used through FFmpeg's DVD-Video demuxer.

// Keeps libdvdread linked after libdvdnav.
use libdvdread_sys as _;

/// Version of the compiled libdvdnav copy.
pub const VERSION: &str = "7.0.0";

extern "C" {
    /// glue/selftest.c: runs one 8-byte VM command in a hand-made
    /// title-domain program chain (`nr_of_programs` programs, at program
    /// `pg_n`) and reports the VM's broken-assumption record.
    pub fn dr_selftest_vm_exec(
        command: *const u8,
        nr_of_programs: std::os::raw::c_int,
        pg_n: std::os::raw::c_int,
        failures: *mut std::os::raw::c_uint,
        first: *mut *const std::os::raw::c_char,
    ) -> std::os::raw::c_int;
    /// glue/selftest.c: runs `nb_commands` 8-byte VM commands one after
    /// another in such a program chain, advancing the playback clock by
    /// `ticks_between` (units of 512/90000 s) before each command after the
    /// first, with an optional source for the Rnd operation; reports the
    /// GPRMs, the program, the broken-assumption count and the ignored
    /// counter sets.
    pub fn dr_selftest_vm_run(
        commands: *const u8,
        nb_commands: std::os::raw::c_int,
        nr_of_programs: std::os::raw::c_int,
        pg_n: std::os::raw::c_int,
        ticks_between: u32,
        rnd: Option<unsafe extern "C" fn(*mut std::os::raw::c_void) -> std::os::raw::c_int>,
        rnd_priv: *mut std::os::raw::c_void,
        gprm: *mut u16,
        pg_n_out: *mut std::os::raw::c_int,
        failures: *mut std::os::raw::c_uint,
        ignored: *mut std::os::raw::c_uint,
        ign_reg: *mut std::os::raw::c_int,
        ign_value: *mut std::os::raw::c_int,
    ) -> std::os::raw::c_int;
    /// Our libdvdnav: the deterministic random program order (vm/rand.h).
    pub fn vm_rand_next(state: *mut u32) -> u32;
    pub fn vm_rand_seed(state: *mut u32, value: u32);
    pub fn vm_rand_shuffle(state: u32, bound: std::os::raw::c_uint, step: std::os::raw::c_uint) -> std::os::raw::c_int;
}
