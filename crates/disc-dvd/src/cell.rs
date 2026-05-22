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

use disc_core::{check, check_eq, check_in_range};
use libdvdread_sys as sys;

pub use sys::{cell_adr_t, dvd_time_t};

/// Decoded snapshot of one `cell_playback_t` (+ paired `cell_position_t`)
/// entry from a PGC's `cell_playback` / `cell_position` arrays. Field
/// names mirror libdvdread's C structs verbatim.
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
    /// `cell_playback_t::first_ilvu_end_sector` — last sector of the
    /// first ILVU when this cell is part of an interleaved (multi-
    /// angle) block. Meaningful only when `block_type == 1`.
    pub first_ilvu_end_sector: u32,
    /// `cell_playback_t::last_vobu_start_sector` — start sector of the
    /// last VOBU in the cell. Used by libdvdnav to find a clean exit
    /// point near the cell's end.
    pub last_vobu_start_sector: u32,
    /// `last_sector - first_sector + 1` — the number of 2048-byte blocks
    /// to read. libdvdnav uses this same expression internally
    /// (`vendor/libdvdnav/src/searching.c`).
    pub block_count: u32,
    /// `cell_playback_t::playback_time` — BCD-encoded HH:MM:SS.FF with
    /// framerate flag in the top two bits of `frame_u`.
    pub playback_time: dvd_time_t,
    /// `cell_playback_t::still_time` — seconds to pause after this
    /// cell. `0xff` = pause indefinitely (DVD spec).
    pub still_time: u8,
    /// `cell_playback_t::cell_cmd_nr` — index into the PGC's
    /// `command_tbl.cell_cmds` array of a VM command to execute on
    /// cell-end. `0` = no command.
    pub cell_cmd_nr: u8,
    /// `cell_playback_t::block_mode` (2-bit; 0 = not-in-block,
    /// 1 = first cell of an angle block, 2 = in-block, 3 = last cell).
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
    /// `cell_playback_t::playback_mode` — when set, the player enters
    /// StillMode after each VOBU in this cell.
    pub playback_mode: bool,
    /// `cell_playback_t::restricted` — playback restriction flag
    /// (libdvdread header marks the exact semantics as "?? drop out
    /// of fastforward").
    pub restricted: bool,
    /// `cell_playback_t::cell_type` (5-bit; karaoke metadata, reserved
    /// otherwise).
    pub cell_type: u8,
    /// `cell_position_t::vob_id_nr` — the VOB ID this cell belongs to,
    /// keyed against `vts_c_adt::cell_adr_table[i].vob_id`.
    pub vob_id_nr: u16,
    /// `cell_position_t::cell_nr` — the cell number within the VOB,
    /// keyed against `vts_c_adt::cell_adr_table[i].cell_id`.
    pub cell_nr: u8,
}

impl CellInfo {
    /// Build a `CellInfo` from libdvdread's raw `cell_playback_t` plus
    /// the matching `cell_position_t` entry (same index within the PGC).
    ///
    /// `cell_playback_t` and `cell_position_t` are `#[repr(packed)]`;
    /// the `{ raw.field }` block syntax copies each primitive field by
    /// value, which is the safe way to read from a packed struct.
    pub(crate) fn from_raw(
        idx: u8,
        play: &sys::cell_playback_t,
        pos: Option<&sys::cell_position_t>,
    ) -> Self {
        let first_sector = { play.first_sector };
        let last_sector = { play.last_sector };
        let first_ilvu_end_sector = { play.first_ilvu_end_sector };
        let last_vobu_start_sector = { play.last_vobu_start_sector };
        let playback_time = { play.playback_time };
        let still_time = { play.still_time };
        let cell_cmd_nr = { play.cell_cmd_nr };
        let block_count = last_sector
            .saturating_sub(first_sector)
            .saturating_add(1);
        let (vob_id_nr, cell_nr) = pos.map_or((0, 0), |p| {
            let vid = { p.vob_id_nr };
            let cnr = { p.cell_nr };
            (vid, cnr)
        });
        Self {
            idx,
            first_sector,
            last_sector,
            first_ilvu_end_sector,
            last_vobu_start_sector,
            block_count,
            playback_time,
            still_time,
            cell_cmd_nr,
            block_mode: play.block_mode(),
            block_type: play.block_type(),
            seamless_play: play.seamless_play() != 0,
            interleaved: play.interleaved() != 0,
            stc_discontinuity: play.stc_discontinuity() != 0,
            seamless_angle: play.seamless_angle() != 0,
            playback_mode: play.playback_mode() != 0,
            restricted: play.restricted() != 0,
            cell_type: play.cell_type(),
            vob_id_nr,
            cell_nr,
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
    let play_ptr = { pgc.cell_playback };
    let pos_ptr = { pgc.cell_position };
    if play_ptr.is_null() || nr == 0 {
        return Vec::new();
    }
    // SAFETY: libdvdread guarantees `cell_playback[0..nr_of_cells]` is
    // initialized whenever its pointer is non-null. The `cell_position`
    // array tracks the same length when present; we treat NULL as "no
    // position info" and fall back to zeros rather than fail.
    let play: &[sys::cell_playback_t] =
        unsafe { std::slice::from_raw_parts(play_ptr, usize::from(nr)) };
    let pos: Option<&[sys::cell_position_t]> = if pos_ptr.is_null() {
        None
    } else {
        Some(unsafe { std::slice::from_raw_parts(pos_ptr, usize::from(nr)) })
    };
    play.iter()
        .enumerate()
        .map(|(i, c)| {
            CellInfo::from_raw(
                u8::try_from(i).unwrap_or(u8::MAX),
                c,
                pos.and_then(|p| p.get(i)),
            )
        })
        .collect()
}

/// Locate a `cell_adr_t` entry in `vts_c_adt` matching `(vob_id, cell_id)`.
/// libdvdread doesn't sort the cell-address table, so we walk it
/// linearly. The table is typically small (one entry per VOB cell —
/// dozens, occasionally low hundreds).
#[must_use]
pub fn find_cell_adr(table: &[sys::cell_adr_t], vob_id: u16, cell_id: u8) -> Option<&sys::cell_adr_t> {
    table
        .iter()
        .find(|e| {
            let v = { e.vob_id };
            let c = { e.cell_id };
            v == vob_id && c == cell_id
        })
}

/// Cross-check a PGC cell's sector range against the `vts_c_adt` entry
/// keyed by `(vob_id_nr, cell_nr)`. Both libdvdread tables describe the
/// same cell on disc, so they must agree: any divergence is an IFO
/// inconsistency.
///
/// Logs PASS/FAIL via `disc_check`. Returns `true` when both
/// `start_sector` and `last_sector` match (or when there's no
/// corresponding `c_adt` entry, which we report as a soft check
/// failure but don't treat as a hard error — a few discs ship with
/// PGC cells that don't appear in c_adt).
pub fn check_cell_vs_c_adt(cell: &CellInfo, c_adt_table: &[sys::cell_adr_t]) -> bool {
    let Some(adr) = find_cell_adr(c_adt_table, cell.vob_id_nr, cell.cell_nr) else {
        check(
            &format!(
                "cell[{}] (vob_id={}, cell_nr={}) has a c_adt entry",
                cell.idx, cell.vob_id_nr, cell.cell_nr
            ),
            "matching c_adt row exists",
            || false,
        );
        return false;
    };
    let start = { adr.start_sector };
    let last = { adr.last_sector };
    let s_ok = check_eq(
        &format!("cell[{}] first_sector matches c_adt.start_sector", cell.idx),
        cell.first_sector,
        start,
    );
    let l_ok = check_eq(
        &format!("cell[{}] last_sector matches c_adt.last_sector", cell.idx),
        cell.last_sector,
        last,
    );
    s_ok && l_ok
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
            first_ilvu_end_sector: 0,
            last_vobu_start_sector: last,
            block_count: last.saturating_sub(first).saturating_add(1),
            playback_time: time(0, 0, 0),
            still_time: 0,
            cell_cmd_nr: 0,
            block_mode: 0,
            block_type: 0,
            seamless_play: false,
            interleaved: false,
            stc_discontinuity: false,
            seamless_angle: false,
            playback_mode: false,
            restricted: false,
            cell_type: 0,
            vob_id_nr: 0,
            cell_nr: 0,
        }
    }

    fn c_adt_row(vob_id: u16, cell_id: u8, start: u32, last: u32) -> sys::cell_adr_t {
        sys::cell_adr_t {
            vob_id,
            cell_id,
            zero_1: 0,
            start_sector: start,
            last_sector: last,
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

    #[test]
    fn find_cell_adr_locates_matching_row() {
        let table = [
            c_adt_row(1, 1, 0, 99),
            c_adt_row(1, 2, 100, 199),
            c_adt_row(2, 1, 200, 299),
        ];
        let row = find_cell_adr(&table, 1, 2).expect("row should exist");
        assert_eq!({ row.start_sector }, 100);
        assert_eq!({ row.last_sector }, 199);
        assert!(find_cell_adr(&table, 99, 99).is_none());
    }

    #[test]
    fn check_cell_vs_c_adt_passes_when_sectors_agree() {
        let mut c = cell(0, 100, 199);
        c.vob_id_nr = 1;
        c.cell_nr = 2;
        let table = [
            c_adt_row(1, 1, 0, 99),
            c_adt_row(1, 2, 100, 199),
        ];
        assert!(check_cell_vs_c_adt(&c, &table));
    }

    #[test]
    fn check_cell_vs_c_adt_fails_on_sector_mismatch() {
        let mut c = cell(0, 100, 199);
        c.vob_id_nr = 1;
        c.cell_nr = 1;
        let table = [c_adt_row(1, 1, 0, 99)];
        assert!(!check_cell_vs_c_adt(&c, &table));
    }

    #[test]
    fn check_cell_vs_c_adt_fails_when_no_match() {
        let mut c = cell(0, 100, 199);
        c.vob_id_nr = 7;
        c.cell_nr = 7;
        let table = [c_adt_row(1, 1, 0, 99)];
        assert!(!check_cell_vs_c_adt(&c, &table));
    }
}
