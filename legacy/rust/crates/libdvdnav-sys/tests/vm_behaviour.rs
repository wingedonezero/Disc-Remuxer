//! Our libdvdnav's VM rules (libs/README.md): GPRM counters on the playback
//! clock (a set restarts the counter from 0 and is recorded when its value is
//! not 0), the Rnd operation from an application source, the deterministic
//! random program order, and the tolerant link rules.

use std::os::raw::{c_int, c_uint, c_void};

use libdvdnav_sys as nav;

/// What one run of the VM left behind.
#[derive(Debug, Default)]
struct Run {
    ret: c_int,
    gprm: [u16; 16],
    pg_n: c_int,
    failures: c_uint,
    ignored: c_uint,
    ign_reg: c_int,
    ign_value: c_int,
}

fn run(commands: &[[u8; 8]], ticks_between: u32, rnd: Option<unsafe extern "C" fn(*mut c_void) -> c_int>) -> Run {
    let flat: Vec<u8> = commands.iter().flatten().copied().collect();
    let mut r = Run::default();
    // SAFETY: flat holds nb_commands * 8 bytes; every out pointer is valid.
    r.ret = unsafe {
        nav::dr_selftest_vm_run(
            flat.as_ptr(),
            c_int::try_from(commands.len()).unwrap(),
            3,
            2,
            ticks_between,
            rnd,
            std::ptr::null_mut(),
            r.gprm.as_mut_ptr(),
            &raw mut r.pg_n,
            &raw mut r.failures,
            &raw mut r.ignored,
            &raw mut r.ign_reg,
            &raw mut r.ign_value,
        )
    };
    r
}

/// `SetGPRMMD`: GPRM `reg` to counter mode, set to `value` (immediate).
fn set_counter(reg: u8, value: u16) -> [u8; 8] {
    let v = value.to_be_bytes();
    [0x53, 0, v[0], v[1], 0, 0x80 | reg, 0, 0]
}

/// Set (mov): GPRM `dst` = GPRM `src`.
fn mov(dst: u8, src: u8) -> [u8; 8] {
    [0x61, 0, 0, dst, 0, src, 0, 0]
}

/// Set (Rnd): GPRM `dst` = a random number from 1 to `n` (immediate).
fn rnd(dst: u8, n: u16) -> [u8; 8] {
    let v = n.to_be_bytes();
    [0x78, 0, 0, dst, v[0], v[1], 0, 0]
}

/// 10 seconds of playback clock (5625 * 10 / 32 units, rounded up).
const TEN_SECONDS: u32 = 1758;

#[test]
fn counters_count_playback_seconds_from_their_set() {
    // set to 0, 10 s later copied out: 10
    let r = run(&[set_counter(0, 0), mov(1, 0)], TEN_SECONDS, None);
    assert_eq!(r.gprm[1], 10);
    assert_eq!(r.ignored, 0, "a set to 0 is not recorded");
}

#[test]
fn a_counter_set_to_a_non_zero_value_restarts_from_0_and_is_recorded() {
    let r = run(&[set_counter(3, 25), mov(1, 3)], TEN_SECONDS, None);
    assert_eq!(r.gprm[1], 10, "the value 25 does not offset the counter");
    assert_eq!((r.ignored, r.ign_reg, r.ign_value), (1, 3, 25));
}

#[test]
fn counters_follow_the_playback_clock_not_the_wall_clock() {
    // no playback time between the commands: 0, however long the test takes
    let r = run(&[set_counter(0, 0), mov(1, 0)], 0, None);
    assert_eq!(r.gprm[1], 0);
}

unsafe extern "C" fn fixed_half(_: *mut c_void) -> c_int {
    // half of RAND_MAX (2^31 - 1) on glibc
    1 << 30
}

unsafe extern "C" fn fixed_zero(_: *mut c_void) -> c_int {
    0
}

#[test]
fn rnd_takes_its_numbers_from_the_application_source() {
    // 1 + n * r / (RAND_MAX + 1): r = 2^30 -> 1 + n / 2
    assert_eq!(run(&[rnd(0, 10)], 0, Some(fixed_half)).gprm[0], 6);
    assert_eq!(run(&[rnd(0, 10)], 0, Some(fixed_zero)).gprm[0], 1);
}

#[test]
fn the_random_program_order_is_deterministic_and_a_permutation_per_cycle() {
    let mut state = 0u32;
    // SAFETY: plain functions on a local state.
    unsafe {
        nav::vm_rand_seed(&raw mut state, 12345);
        nav::vm_rand_next(&raw mut state);
    }
    for bound in [1u32, 2, 3, 4, 7, 20, 99] {
        // SAFETY: as above.
        let order: Vec<c_int> = (0..bound).map(|s| unsafe { nav::vm_rand_shuffle(state, bound, s) }).collect();
        let again: Vec<c_int> = (0..bound).map(|s| unsafe { nav::vm_rand_shuffle(state, bound, s) }).collect();
        assert_eq!(order, again, "same state, same order");
        let mut sorted = order.clone();
        sorted.sort_unstable();
        assert_eq!(sorted, (0..c_int::try_from(bound).unwrap()).collect::<Vec<_>>(), "bound {bound}: {order:?}");
        if bound >= 4 {
            assert_ne!(order[0], 0, "with 4 or more programs a cycle does not start with program 1");
        }
    }
    // SAFETY: as above.
    unsafe {
        assert_eq!(nav::vm_rand_shuffle(state, 0, 0), -1, "no programs");
    }
}

/// Link instruction `LinkPGN` (link command type 6): program `pg`.
fn link_pgn(pg: u8) -> [u8; 8] {
    [0x20, 0x06, 0, 0, 0, 0, 0, pg]
}

#[test]
fn a_link_to_program_0_stops_without_a_broken_assumption() {
    let r = run(&[link_pgn(0)], 0, None);
    assert_eq!(r.ret, 0, "playback stops");
    assert_eq!(r.failures, 0, "not a broken assumption");
    assert_eq!(r.pg_n, 1);
}
