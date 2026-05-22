//! Cell-level access for DVD-Video PGCs.
//!
//! A PGC (Program Chain) contains a `cell_playback` array — the sequence
//! of cells (sector ranges within `TITLE_VOBS`) that make up a title's
//! playback. Reading those cells in order, concatenating their sector
//! ranges, gives the title's MPEG-PS byte stream.
//!
//! This module provides:
//!
//! * [`CellInfo`] — a safe snapshot of one `cell_playback_t` entry, with
//!   the inclusive `[first_sector..=last_sector]` range pre-computed as
//!   `block_count`. Field names mirror libdvdread's `cell_playback_t`.
//! * [`cells_in_pgc`] — collect the PGC's cells in IFO order.
//! * [`check_cell_walk`] — per-cell invariant checks logged through
//!   `disc_core::check::*`.
//!
//! Cell ordering: for simple linear PGCs the IFO-order walk is the same
//! as the playback order. Multi-angle and branching titles need the
//! libdvdnav VM to drive cell selection; that arrives later in the
//! roadmap. Any time the walk encounters a non-monotonic sector range
//! the per-cell check logs a warning so it's visible in the job log.

use disc_core::{check, check_in_range};
use libdvdread_sys as sys;

pub use sys::dvd_time_t;

/// Decoded snapshot of one `cell_playback_t` entry from a PGC's
/// `cell_playback` array.
#[derive(Debug, Clone, Copy)]
pub struct CellInfo {
    /// 0-based index of this cell within `pgc.cell_playback`.
    pub idx: u8,
    /// `cell_playback_t::first_sector` — inclusive LBA start of the
    /// cell, addressed within the owning VTS's `TITLE_VOBS` logical
    /// block stream.
    pub first_sector: u32,
    /// `cell_playback_t::last_sector` — inclusive LBA end of the cell.
    pub last_sector: u32,
    /// `last_sector - first_sector + 1` — the number of 2048-byte blocks
    /// to read. libdvdnav uses this same expression internally
    /// (`vendor/libdvdnav/src/searching.c`).
    pub block_count: u32,
    /// `cell_playback_t::playback_time` — BCD-encoded HH:MM:SS.FF with
    /// framerate flag in the top two bits of `frame_u`.
    pub playback_time: dvd_time_t,
    /// `cell_playback_t::block_mode` (2-bit).
    pub block_mode: u8,
    /// `cell_playback_t::block_type` (2-bit; 0 = not in block,
    /// 1 = angle-block cell).
    pub block_type: u8,
    /// `cell_playback_t::seamless_play`.
    pub seamless_play: bool,
    /// `cell_playback_t::interleaved`.
    pub interleaved: bool,
    /// `cell_playback_t::stc_discontinuity` — when set the cell starts a
    /// new STC reference so PTS/DTS reset.
    pub stc_discontinuity: bool,
    /// `cell_playback_t::seamless_angle`.
    pub seamless_angle: bool,
}

impl CellInfo {
    /// Build a `CellInfo` from libdvdread's raw `cell_playback_t`.
    ///
    /// `cell_playback_t` is `#[repr(packed)]`; the `{ raw.field }` block
    /// syntax copies each primitive field by value, which is the safe
    /// way to read from a packed struct.
    pub(crate) fn from_raw(idx: u8, raw: &sys::cell_playback_t) -> Self {
        let first_sector = { raw.first_sector };
        let last_sector = { raw.last_sector };
        let playback_time = { raw.playback_time };
        let block_count = last_sector
            .saturating_sub(first_sector)
            .saturating_add(1);
        Self {
            idx,
            first_sector,
            last_sector,
            block_count,
            playback_time,
            block_mode: raw.block_mode(),
            block_type: raw.block_type(),
            seamless_play: raw.seamless_play() != 0,
            interleaved: raw.interleaved() != 0,
            stc_discontinuity: raw.stc_discontinuity() != 0,
            seamless_angle: raw.seamless_angle() != 0,
        }
    }

    /// Total whole seconds of the cell's playback time, ignoring the
    /// fractional-frame part. Used for the soft cross-check that the
    /// sum of cell durations approximates the PGC's playback_time.
    #[must_use]
    pub fn playback_seconds(&self) -> u32 {
        let t = self.playback_time;
        let h = u32::from(bcd_to_u8(t.hour));
        let m = u32::from(bcd_to_u8(t.minute));
        let s = u32::from(bcd_to_u8(t.second));
        h * 3600 + m * 60 + s
    }
}

/// Total whole seconds in a `dvd_time_t`. Mirrors [`CellInfo::playback_seconds`]
/// for use on raw PGC times.
#[must_use]
pub fn dvd_time_seconds(t: &sys::dvd_time_t) -> u32 {
    let h = u32::from(bcd_to_u8(t.hour));
    let m = u32::from(bcd_to_u8(t.minute));
    let s = u32::from(bcd_to_u8(t.second));
    h * 3600 + m * 60 + s
}

const fn bcd_to_u8(bcd: u8) -> u8 {
    ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F)
}

/// Collect a PGC's cells in IFO order. Returns an empty `Vec` if the
/// PGC has no cells or its `cell_playback` pointer is NULL.
///
/// The walk follows `pgc.cell_playback[0..nr_of_cells]`. This is the
/// correct order for simple linear PGCs; multi-angle and branching
/// titles will eventually need libdvdnav to drive cell selection
/// (see the v1 roadmap).
#[must_use]
pub fn cells_in_pgc(pgc: &sys::pgc_t) -> Vec<CellInfo> {
    let nr = { pgc.nr_of_cells };
    let ptr = { pgc.cell_playback };
    if ptr.is_null() || nr == 0 {
        return Vec::new();
    }
    // SAFETY: libdvdread guarantees `cell_playback[0..nr_of_cells]` is
    // initialized whenever the `cell_playback` pointer is non-null.
    let raw: &[sys::cell_playback_t] =
        unsafe { std::slice::from_raw_parts(ptr, usize::from(nr)) };
    raw.iter()
        .enumerate()
        .map(|(i, c)| CellInfo::from_raw(u8::try_from(i).unwrap_or(u8::MAX), c))
        .collect()
}

/// Per-cell invariant checks. Each one logs PASS/FAIL through the
/// `disc_check` log target so the operation is self-documenting in the
/// job log.
///
/// Checks:
///
/// 1. `cell.first_sector <= cell.last_sector` — range is non-empty and
///    not inverted.
/// 2. `cell.last_sector < file_block_count` — cell fits inside the
///    title's `TITLE_VOBS` file.
/// 3. `cell.first_sector > prev.last_sector` — the simple-linear PGC
///    invariant. Multi-angle and branching titles can revisit sectors,
///    so a failure here is a warning (soft check), not an error: it
///    flags that the walk has hit a non-linear structure that will
///    eventually need libdvdnav navigation.
///
/// Returns `true` if every check passed.
#[must_use]
pub fn check_cell_walk(
    cell: &CellInfo,
    prev: Option<&CellInfo>,
    file_block_count: u32,
) -> bool {
    let mut ok = true;

    if !check(
        &format!("cell[{}] first_sector <= last_sector", cell.idx),
        &format!("{} <= {}", cell.first_sector, cell.last_sector),
        || cell.first_sector <= cell.last_sector,
    ) {
        ok = false;
    }

    // file_block_count is a u32 and may be 0; saturating_sub keeps the
    // upper bound sensible without underflow.
    if !check_in_range(
        &format!("cell[{}] last_sector within TITLE_VOBS", cell.idx),
        u64::from(cell.last_sector),
        u64::from(file_block_count.saturating_sub(1)),
    ) {
        ok = false;
    }

    if let Some(p) = prev {
        if !check(
            &format!("cell[{}] starts after cell[{}] ends", cell.idx, p.idx),
            &format!(
                "first_sector={} > prev.last_sector={}",
                cell.first_sector, p.last_sector
            ),
            || cell.first_sector > p.last_sector,
        ) {
            ok = false;
        }
    }

    ok
}

#[cfg(test)]
mod tests {
    use super::*;

    fn time(h: u8, m: u8, s: u8) -> sys::dvd_time_t {
        // Encode each component as packed BCD (e.g. 25 -> 0x25).
        let enc = |v: u8| -> u8 { ((v / 10) << 4) | (v % 10) };
        sys::dvd_time_t {
            hour: enc(h),
            minute: enc(m),
            second: enc(s),
            frame_u: 0,
        }
    }

    fn cell(idx: u8, first: u32, last: u32) -> CellInfo {
        CellInfo {
            idx,
            first_sector: first,
            last_sector: last,
            block_count: last.saturating_sub(first).saturating_add(1),
            playback_time: time(0, 0, 0),
            block_mode: 0,
            block_type: 0,
            seamless_play: false,
            interleaved: false,
            stc_discontinuity: false,
            seamless_angle: false,
        }
    }

    #[test]
    fn dvd_time_seconds_decodes_bcd() {
        assert_eq!(dvd_time_seconds(&time(1, 23, 45)), 3600 + 23 * 60 + 45);
    }

    #[test]
    fn cell_playback_seconds_matches_dvd_time() {
        let c = CellInfo {
            playback_time: time(0, 12, 34),
            ..cell(0, 0, 0)
        };
        assert_eq!(c.playback_seconds(), 12 * 60 + 34);
    }

    #[test]
    fn block_count_is_inclusive() {
        let c = cell(0, 100, 199);
        assert_eq!(c.block_count, 100);
    }

    #[test]
    fn check_cell_walk_accepts_sequential_cells() {
        let c0 = cell(0, 0, 99);
        let c1 = cell(1, 100, 199);
        let file_blocks: u32 = 1_000;
        assert!(check_cell_walk(&c0, None, file_blocks));
        assert!(check_cell_walk(&c1, Some(&c0), file_blocks));
    }

    #[test]
    fn check_cell_walk_flags_inverted_range() {
        let bad = cell(0, 200, 100);
        // saturating_sub makes block_count look low but the first-<=-last
        // check should fail.
        assert!(!check_cell_walk(&bad, None, 1_000));
    }

    #[test]
    fn check_cell_walk_flags_out_of_file() {
        let c = cell(0, 0, 999);
        assert!(!check_cell_walk(&c, None, /* file_blocks */ 500));
    }

    #[test]
    fn check_cell_walk_flags_overlapping_cells() {
        let c0 = cell(0, 0, 200);
        let c1 = cell(1, 100, 300);
        assert!(!check_cell_walk(&c1, Some(&c0), 1_000));
    }
}
