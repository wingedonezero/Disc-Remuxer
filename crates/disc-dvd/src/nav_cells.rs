//! Static (`title`, `pgcn`, `cellN`) → [`CellInfo`] lookup, built once
//! at session start by walking every PGC in every VTS via libdvdread.
//!
//! libdvdnav drives playback and tells us *which* cell we're in
//! (`DVDNAV_CELL_CHANGE` event carries `cellN`; `dvdnav_current_title_program`
//! returns the current title + `pgcn`), but it does NOT surface the
//! per-cell metadata the demuxer needs — particularly the
//! `stc_discontinuity` flag that drives FAP-based audio resync.
//!
//! Rather than open IFOs reactively on each `CellChange` (which would
//! complicate lifetimes — `IfoHandle` borrows from a `DvdReader`), we
//! do an eager walk: open VMG, build `(title → vts_nr)` from
//! `tt_srpt`, open each referenced VTS once, copy every cell's
//! `CellInfo` into a flat `HashMap`, then drop the IFO handles. The
//! map outlives the IFOs because `CellInfo` is `Copy` and holds no
//! pointers.
//!
//! Memory cost is small in practice: a typical DVD has on the order
//! of dozens to a few hundred cells total, and each [`CellInfo`] is
//! ~50 bytes. Worst-case bound is ~99 VTSes × 32 PGCs × 255 cells ≈
//! 800k entries (~40 MB), which is still trivial; real discs are
//! orders of magnitude smaller.

use std::collections::HashMap;

use crate::DvdError;

use crate::cell::{cells_in_pgc, CellInfo};
use crate::ifo::{IfoHandle, IfoKind};
use crate::reader::DvdReader;

/// Composite key into the cell-info map. All three components are
/// 1-based per the DVD-Video spec:
///
/// * `title_set_nr` — VTS number 1..=99.
/// * `pgcn` — PGC number within the VTS (1..=`nr_of_pgci_srp`).
/// * `cell_nr` — cell index within the PGC (1..=`pgc.nr_of_cells`).
///
/// libdvdnav reports `cellN` as 1-based via its `CellChange` event, so
/// callers pass that value through unchanged.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct CellKey {
    pub title_set_nr: u8,
    pub pgcn: u16,
    pub cell_nr: u8,
}

/// Frozen snapshot of every cell on a disc, indexed by [`CellKey`] and
/// with a side-table mapping libdvdnav's 1-based title number to the
/// `title_set_nr` (VTS number) that owns it.
pub struct CellLookup {
    /// `tt_srpt[title-1].title_set_nr` for every title — i.e. the
    /// 1-based libdvdnav title number → VTS number.
    title_to_vts: Vec<u8>,
    /// Every cell on the disc, keyed by `(VTS, PGC, cell)`.
    cells: HashMap<CellKey, CellInfo>,
}

impl CellLookup {
    /// Build the table by walking VMG + every VTS IFO on the disc.
    ///
    /// Failures opening an individual VTS IFO are logged at `warn` and
    /// the VTS is skipped — its cells just won't appear in the map
    /// (callers will get `None` from [`Self::get`]). This mirrors how
    /// the demuxer treats unknown discontinuities: defer to the
    /// caller's policy rather than abort the whole rip.
    pub fn build(reader: &DvdReader) -> Result<Self, DvdError> {
        let vmg = IfoHandle::open(reader, IfoKind::Vmg)?;
        let titles = vmg.titles();
        let mut title_to_vts = Vec::with_capacity(titles.len());
        // Collect the set of VTSes the disc actually uses — only open
        // those, not every VTS slot.
        let mut needed_vts = std::collections::BTreeSet::new();
        for t in titles {
            let vts: u8 = { t.title_set_nr };
            title_to_vts.push(vts);
            if vts > 0 {
                needed_vts.insert(vts);
            }
        }
        log::debug!(
            "CellLookup: {} titles span {} VTSes",
            title_to_vts.len(),
            needed_vts.len()
        );

        let mut cells = HashMap::new();
        for vts_nr in needed_vts {
            let vts_ifo = match IfoHandle::open(reader, IfoKind::Vts(u32::from(vts_nr))) {
                Ok(h) => h,
                Err(e) => {
                    log::warn!(
                        "CellLookup: could not open VTS_{vts_nr:02}_0.IFO: {e} — cells in this VTS will be missing"
                    );
                    continue;
                }
            };
            let pgcs = vts_ifo.pgcs();
            for (pgci_idx, srp) in pgcs.iter().enumerate() {
                // pgcn is 1-based in libdvdnav's reporting; the
                // pgci_srp array index is 0-based.
                let pgcn = u16::try_from(pgci_idx + 1).unwrap_or(u16::MAX);
                let pgc_ptr = { srp.pgc };
                // SAFETY: libdvdread populates the PGC pointer when
                // the IFO parses; NULL means a malformed slot which
                // we just skip.
                let Some(pgc) = (unsafe { pgc_ptr.as_ref() }) else {
                    continue;
                };
                let pgc_cells = cells_in_pgc(pgc);
                for cell in pgc_cells {
                    // CellInfo::idx is 0-based; cellN in libdvdnav
                    // events is 1-based. Convert.
                    let cell_nr = cell.idx.saturating_add(1);
                    let key = CellKey {
                        title_set_nr: vts_nr,
                        pgcn,
                        cell_nr,
                    };
                    cells.insert(key, cell);
                }
            }
        }
        log::info!(
            "CellLookup: built table of {} cells across {} titles",
            cells.len(),
            title_to_vts.len()
        );
        Ok(Self {
            title_to_vts,
            cells,
        })
    }

    /// Number of cells in the lookup table.
    #[must_use]
    pub fn len(&self) -> usize {
        self.cells.len()
    }

    /// Is the table empty?
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.cells.is_empty()
    }

    /// VTS number that owns the given title (1-based per libdvdnav /
    /// libdvdread). Returns `None` if `title` is out of range.
    #[must_use]
    pub fn vts_for_title(&self, title: u32) -> Option<u8> {
        if title == 0 {
            return None;
        }
        let idx = (title as usize) - 1;
        self.title_to_vts.get(idx).copied()
    }

    /// Look up a cell by (libdvdnav title, pgcn, cellN) — all 1-based
    /// as the API surfaces them. Returns `None` when the title is out
    /// of range, the VTS was skipped during build, or no cell matches
    /// the (pgcn, cellN) pair (e.g. libdvdnav reported a value our
    /// IFO walk doesn't have, which can happen on discs with
    /// malformed PGCIT structures).
    #[must_use]
    pub fn get(&self, title: u32, pgcn: u16, cell_nr: u8) -> Option<&CellInfo> {
        let vts = self.vts_for_title(title)?;
        self.cells.get(&CellKey {
            title_set_nr: vts,
            pgcn,
            cell_nr,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cell_key_round_trips_in_hashmap() {
        let mut m = HashMap::new();
        let k = CellKey {
            title_set_nr: 3,
            pgcn: 1,
            cell_nr: 5,
        };
        let v = 42u32;
        m.insert(k, v);
        assert_eq!(m.get(&k), Some(&42));
        assert_eq!(
            m.get(&CellKey {
                title_set_nr: 3,
                pgcn: 1,
                cell_nr: 6
            }),
            None
        );
    }
}
