//! Verification primitives.
//!
//! Each step of disc handling has invariants we want to verify and log
//! explicitly — file sizes match what the IFO claims, sector reads return
//! the requested count, the first bytes of a cleartext VOB start with the
//! MPEG-PS pack-start code, and so on. These checks are the difference
//! between a black box that silently produces wrong output and a tool the
//! user can debug from the log alone.
//!
//! Every check logs at a stable target (`disc_check`) so users can isolate
//! them with `RUST_LOG=disc_check=info`. Passing checks log at `info`,
//! failing ones at `warn` (soft) or `error` (hard). Checks return `bool`
//! so callers can decide whether to continue on failure.

use std::fmt::Debug;

/// Run an equality check and log the result.
///
/// Returns `true` if `actual == expected`. On mismatch logs at `warn`
/// level (the failure is reported but not fatal — caller decides what to
/// do). For hard failures use [`require_eq`].
pub fn check_eq<T: PartialEq + Debug>(label: &str, actual: T, expected: T) -> bool {
    if actual == expected {
        log::info!(target: "disc_check", "{label}: actual={actual:?} expected={expected:?} PASS");
        true
    } else {
        log::warn!(target: "disc_check", "{label}: actual={actual:?} expected={expected:?} FAIL");
        false
    }
}

/// Like [`check_eq`] but logs failures at `error` level for must-hold
/// invariants. Caller is still responsible for the actual error return.
pub fn require_eq<T: PartialEq + Debug>(label: &str, actual: T, expected: T) -> bool {
    if actual == expected {
        log::info!(target: "disc_check", "{label}: actual={actual:?} expected={expected:?} PASS");
        true
    } else {
        log::error!(target: "disc_check", "{label}: actual={actual:?} expected={expected:?} FAIL");
        false
    }
}

/// Range / bounds check. Returns `true` if `0 <= value <= max`.
pub fn check_in_range(label: &str, value: u64, max: u64) -> bool {
    if value <= max {
        log::info!(target: "disc_check", "{label}: value={value} max={max} PASS");
        true
    } else {
        log::warn!(target: "disc_check", "{label}: value={value} max={max} FAIL (out of range)");
        false
    }
}

/// Custom predicate check with caller-supplied description of what was
/// expected, for cases that don't fit equality / range / contains.
pub fn check<F: FnOnce() -> bool>(label: &str, expected_desc: &str, predicate: F) -> bool {
    if predicate() {
        log::info!(target: "disc_check", "{label}: {expected_desc} PASS");
        true
    } else {
        log::warn!(target: "disc_check", "{label}: {expected_desc} FAIL");
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn check_eq_passes() {
        assert!(check_eq("identity", 5_u32, 5_u32));
    }

    #[test]
    fn check_eq_fails() {
        assert!(!check_eq("mismatch", 5_u32, 6_u32));
    }

    #[test]
    fn check_in_range_works() {
        assert!(check_in_range("in", 5, 10));
        assert!(check_in_range("boundary", 10, 10));
        assert!(!check_in_range("out", 11, 10));
    }

    #[test]
    fn check_custom_predicate() {
        assert!(check("starts with magic", "00 00 01 BA", || true));
        assert!(!check("starts with magic", "00 00 01 BA", || false));
    }
}
