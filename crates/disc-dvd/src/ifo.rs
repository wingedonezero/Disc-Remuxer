//! RAII wrapper around libdvdread's `ifo_handle_t` plus safe accessors
//! for the public IFO sub-structures.
//!
//! libdvdread's C `ifo_handle_t` is a union of DVD-Video fields (VMGI /
//! VTSI pointers like `vmgi_mat`, `tt_srpt`, `vts_pgcit`, etc.) and
//! DVD-Audio fields (SAMG / AMGI / ATSI). bindgen renders that as a
//! nested anonymous-union/anonymous-struct, which is awkward to access
//! directly — `(*ifo).__bindgen_anon_1.__bindgen_anon_1.vmgi_mat`.
//!
//! This module hides that boilerplate behind one helper, `dvd_video()`,
//! that returns the DVD-Video anon struct. Callers see clean
//! libdvdread-style field access:
//!
//! ```ignore
//! let nr_titles = ifo.tt_srpt().map_or(0, |s| s.nr_of_srpts);
//! ```

use std::marker::PhantomData;
use std::slice;

use disc_core::DiscError;
use libdvdread_sys as sys;

use crate::reader::DvdReader;

// Re-export the public IFO struct types so callers can use libdvdread's
// field-name conventions (`tt_srpt_t::nr_of_srpts`, `pgc_t::nr_of_cells`,
// etc.) without depending on `libdvdread-sys` directly. The structs are
// `#[repr(packed)]` because they mirror DVD on-disc layout — readers must
// copy fields into local variables before formatting / referencing.
pub use libdvdread_sys::{
    cell_playback_t, dvd_time_t, pgc_t, pgci_srp_t, pgcit_t, title_info_t, tt_srpt_t,
    vmgi_mat_t, vtsi_mat_t,
};

/// Which IFO file to open.
///
/// libdvdread's `ifoOpen(reader, n)` takes `n == 0` for the VMG and
/// `n == 1..=99` for VTS `n`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IfoKind {
    /// `VIDEO_TS.IFO` — the disc-wide Video Manager Information File.
    Vmg,
    /// `VTS_<nn>_0.IFO` — Video Title Set `n` Information File.
    /// `n` must be in 1..=99.
    Vts(u32),
}

impl IfoKind {
    fn as_ifo_nr(self) -> u32 {
        match self {
            Self::Vmg => 0,
            Self::Vts(n) => n,
        }
    }
}

/// RAII handle for an opened `ifo_handle_t`. Bound by lifetime to the
/// owning `DvdReader` because the IFO holds a reference into the reader's
/// file table.
pub struct IfoHandle<'r> {
    handle: *mut sys::ifo_handle_t,
    kind: IfoKind,
    _parent: PhantomData<&'r DvdReader>,
}

impl<'r> IfoHandle<'r> {
    /// Open an IFO. Returns `IfoOpenFailed` if libdvdread can't parse it.
    pub fn open(reader: &'r DvdReader, kind: IfoKind) -> Result<Self, DiscError> {
        let ifo_nr = kind.as_ifo_nr();
        log::debug!("ifoOpen ifo_nr={ifo_nr}");
        // SAFETY: `reader.raw()` is valid for the lifetime `'r`; `ifoOpen`
        // returns NULL on failure (handled below). `ifo_nr` is bounded by
        // the DVD spec to 0..=99 — the i32 cast is lossless.
        #[allow(clippy::cast_possible_wrap)]
        let handle = unsafe { sys::ifoOpen(reader.raw(), ifo_nr as i32) };

        if handle.is_null() {
            return Err(DiscError::IfoOpenFailed { ifo_nr });
        }

        log::debug!("ifoOpen ok ifo_nr={ifo_nr} handle={handle:p}");
        Ok(Self {
            handle,
            kind,
            _parent: PhantomData,
        })
    }

    /// Which IFO this handle refers to.
    #[must_use]
    pub fn kind(&self) -> IfoKind {
        self.kind
    }

    // --- Access to the DVD-Video union arm ---

    /// The bindgen-rendered union arm holding the DVD-Video fields. Hides
    /// the `__bindgen_anon_1.__bindgen_anon_1` indirection.
    ///
    /// # Safety
    /// libdvdread's `ifoOpen` always populates the DVD-Video arm for
    /// `IfoKind::Vmg` and `IfoKind::Vts(_)`. We only open those, so the
    /// arm is always the live one.
    fn dvd_video(&self) -> &sys::ifo_handle_t__bindgen_ty_1__bindgen_ty_1 {
        // SAFETY: see doc comment above. We never construct an IFO of
        // DVD-Audio type.
        unsafe { &(*self.handle).__bindgen_anon_1.__bindgen_anon_1 }
    }

    // --- VMG-side accessors (only meaningful when `kind() == Vmg`) ---

    /// `vmgi_mat` — the VMG Information Management Table. `None` if
    /// libdvdread didn't parse it (e.g. this is a VTS IFO).
    pub fn vmgi_mat(&self) -> Option<&sys::vmgi_mat_t> {
        // SAFETY: pointer is either NULL (-> None via `as_ref`) or points
        // into memory owned by the ifo handle, alive for `'self`.
        unsafe { self.dvd_video().vmgi_mat.as_ref() }
    }

    /// `tt_srpt` — Title Search Pointer Table. Holds the per-title
    /// metadata array (which VTS each title lives in, chapter count, etc.).
    /// Only present in the VMG IFO.
    pub fn tt_srpt(&self) -> Option<&sys::tt_srpt_t> {
        unsafe { self.dvd_video().tt_srpt.as_ref() }
    }

    // --- VTS-side accessors (only meaningful when `kind() == Vts(_)`) ---

    /// `vtsi_mat` — VTS Information Management Table. Only present in
    /// VTS IFOs.
    pub fn vtsi_mat(&self) -> Option<&sys::vtsi_mat_t> {
        unsafe { self.dvd_video().vtsi_mat.as_ref() }
    }

    /// `vts_pgcit` — the PGC Information Table for this VTS. Holds the
    /// per-PGC pointers used during title playback.
    pub fn vts_pgcit(&self) -> Option<&sys::pgcit_t> {
        unsafe { self.dvd_video().vts_pgcit.as_ref() }
    }

    // --- Slice convenience over libdvdread's "count + ptr" arrays ---

    /// The `title_info_t` array embedded in `tt_srpt`. Empty if there's
    /// no `tt_srpt` (i.e. not a VMG IFO).
    pub fn titles(&self) -> &[sys::title_info_t] {
        let Some(srpt) = self.tt_srpt() else {
            return &[];
        };
        if srpt.title.is_null() {
            return &[];
        }
        // SAFETY: libdvdread guarantees `title[0..nr_of_srpts]` is initialized
        // when `tt_srpt` is non-null.
        unsafe { slice::from_raw_parts(srpt.title, usize::from(srpt.nr_of_srpts)) }
    }

    /// The `pgci_srp_t` array embedded in `vts_pgcit`. Empty if there's
    /// no PGCIT (i.e. not a VTS IFO).
    pub fn pgcs(&self) -> &[sys::pgci_srp_t] {
        let Some(pgcit) = self.vts_pgcit() else {
            return &[];
        };
        if pgcit.pgci_srp.is_null() {
            return &[];
        }
        // SAFETY: libdvdread guarantees `pgci_srp[0..nr_of_pgci_srp]` is
        // initialized when `vts_pgcit` is non-null.
        unsafe { slice::from_raw_parts(pgcit.pgci_srp, usize::from(pgcit.nr_of_pgci_srp)) }
    }
}

impl Drop for IfoHandle<'_> {
    fn drop(&mut self) {
        log::debug!(
            "ifoClose ifo_nr={} handle={:p}",
            self.kind.as_ifo_nr(),
            self.handle
        );
        // SAFETY: `handle` is non-null (checked in `open`) and we hold
        // unique ownership.
        unsafe { sys::ifoClose(self.handle) };
    }
}

/// Format a `dvd_time_t` as `HH:MM:SS.FFF` for human-readable logs and
/// CLI output. The frame count is in the low 6 bits of `frame_u`; the top
/// two bits encode framerate (25 Hz / 30 Hz NTSC / reserved).
#[must_use]
pub fn format_dvd_time(t: &sys::dvd_time_t) -> String {
    let h = bcd_to_u8(t.hour);
    let m = bcd_to_u8(t.minute);
    let s = bcd_to_u8(t.second);
    let f = bcd_to_u8(t.frame_u & 0x3F);
    format!("{h:02}:{m:02}:{s:02}.{f:02}")
}

/// Convert a packed BCD byte (libdvdread stores DVD time fields as BCD).
#[must_use]
pub const fn bcd_to_u8(bcd: u8) -> u8 {
    ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F)
}
