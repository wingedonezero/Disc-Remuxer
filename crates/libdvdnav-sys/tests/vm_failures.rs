//! Broken navigation data must not abort the program: the VM records the
//! broken assumption and stops (libdvdnav upstream asserts).

use std::ffi::CStr;
use std::os::raw::{c_char, c_uint};

/// DVD-Video command `LinkPrevPG` (link instruction, sub-instruction 7,
/// no condition, no button).
const LINK_PREV_PG: [u8; 8] = [0x20, 0x01, 0, 0, 0, 0, 0, 0x07];

fn exec(command: [u8; 8], programs: i32, pg_n: i32) -> (i32, u32, String) {
    let mut failures: c_uint = 0;
    let mut first: *const c_char = std::ptr::null();
    // SAFETY: command has 8 bytes; the out pointers are valid.
    let ret = unsafe {
        libdvdnav_sys::dr_selftest_vm_exec(
            command.as_ptr(),
            programs,
            pg_n,
            &raw mut failures,
            &raw mut first,
        )
    };
    // SAFETY: first points to a static NUL-terminated string.
    let first = unsafe { CStr::from_ptr(first) }.to_string_lossy().into_owned();
    (ret, failures, first)
}

#[test]
fn link_prev_pg_at_the_first_program_is_recorded_not_aborted() {
    let (ret, failures, first) = exec(LINK_PREV_PG, 3, 1);
    assert_eq!(ret, 0, "the command must not continue playback");
    assert_eq!(failures, 1);
    assert!(first.contains("pgN > 1"), "first failure: {first}");
}
