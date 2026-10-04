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
}
