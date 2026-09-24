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

use crate::DvdError;
use libdvdread_sys as sys;

use crate::reader::DvdReader;

// Re-export the public IFO struct types so callers can use libdvdread's
// field-name conventions (`tt_srpt_t::nr_of_srpts`, `pgc_t::nr_of_cells`,
// etc.) without depending on `libdvdread-sys` directly. The structs are
// `#[repr(packed)]` because they mirror DVD on-disc layout — readers must
// copy fields into local variables before formatting / referencing.
pub use libdvdread_sys::{
    audio_attr_t, c_adt_t, cell_adr_t, cell_playback_t, cell_position_t, dvd_time_t,
    map_ent_t, pgc_t, pgci_srp_t, pgcit_t, ptl_mait_country_t, ptl_mait_t, ptt_info_t,
    subp_attr_t, title_info_t, tt_srpt_t, ttu_t, video_attr_t, vmgi_mat_t, vobu_admap_t,
    vts_atrt_t, vts_ptt_srpt_t, vts_tmap_t, vts_tmapt_t, vtsi_mat_t,
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
    pub fn open(reader: &'r DvdReader, kind: IfoKind) -> Result<Self, DvdError> {
        let ifo_nr = kind.as_ifo_nr();
        log::debug!("ifoOpen ifo_nr={ifo_nr}");
        // SAFETY: `reader.raw()` is valid for the lifetime `'r`; `ifoOpen`
        // returns NULL on failure (handled below). `ifo_nr` is bounded by
        // the DVD spec to 0..=99 — the i32 cast is lossless.
        #[allow(clippy::cast_possible_wrap)]
        let handle = unsafe { sys::ifoOpen(reader.raw(), ifo_nr as i32) };

        if handle.is_null() {
            return Err(DvdError::IfoOpenFailed { ifo_nr });
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

    /// `first_play_pgc` — the PGC the player runs on disc insert
    /// (FP_PGC in the DVD-Video spec). Typical use: studio logos and
    /// menu intros. `None` when there is no first-play PGC (rare) or
    /// for VTS IFOs.
    pub fn first_play_pgc(&self) -> Option<&sys::pgc_t> {
        unsafe { self.dvd_video().first_play_pgc.as_ref() }
    }

    /// `ptl_mait` — Parental Management Information Table. Maps each
    /// (country, VTS) pair to a parental level. `None` if the disc has
    /// no parental controls or this is a VTS IFO.
    pub fn ptl_mait(&self) -> Option<&sys::ptl_mait_t> {
        unsafe { self.dvd_video().ptl_mait.as_ref() }
    }

    /// `vts_atrt` — VTS Attribute Table. For each VTS on the disc,
    /// stores a copy of the VTS attribute block — useful when scanning
    /// disc-wide attributes without opening every VTS IFO. VMG only.
    pub fn vts_atrt(&self) -> Option<&sys::vts_atrt_t> {
        unsafe { self.dvd_video().vts_atrt.as_ref() }
    }

    // --- VTS-side accessors (only meaningful when `kind() == Vts(_)`) ---

    /// `vtsi_mat` — VTS Information Management Table. Only present in
    /// VTS IFOs.
    pub fn vtsi_mat(&self) -> Option<&sys::vtsi_mat_t> {
        unsafe { self.dvd_video().vtsi_mat.as_ref() }
    }

    /// `vts_ptt_srpt` — VTS Part-of-Title Search Pointer Table. Holds the
    /// per-title chapter (PTT) arrays for titles owned by this VTS.
    pub fn vts_ptt_srpt(&self) -> Option<&sys::vts_ptt_srpt_t> {
        unsafe { self.dvd_video().vts_ptt_srpt.as_ref() }
    }

    /// `vts_pgcit` — the PGC Information Table for this VTS. Holds the
    /// per-PGC pointers used during title playback.
    pub fn vts_pgcit(&self) -> Option<&sys::pgcit_t> {
        unsafe { self.dvd_video().vts_pgcit.as_ref() }
    }

    /// `vts_c_adt` — VTS Cell Address Table. A flat
    /// `(vob_id, cell_id, start_sector, last_sector)` directory of
    /// every VOB cell on the VTS, independent of any PGC. Useful for
    /// cross-checking the sector ranges advertised in
    /// [`pgc_t::cell_playback`].
    pub fn vts_c_adt(&self) -> Option<&sys::c_adt_t> {
        unsafe { self.dvd_video().vts_c_adt.as_ref() }
    }

    /// `vts_vobu_admap` — VOBU Address Map. A flat list of starting
    /// sectors for every VOBU (Video OBject Unit, ~0.4–1 s of MPEG-PS)
    /// in the VTS. Used by the navigation VM for time-based seeks and
    /// useful to us as a hard list of valid pack-header sector
    /// positions when validating MPEG-PS structure.
    pub fn vts_vobu_admap(&self) -> Option<&sys::vobu_admap_t> {
        unsafe { self.dvd_video().vts_vobu_admap.as_ref() }
    }

    /// `vts_tmapt` — VTS Time Map Table. Maps PGC playback time to
    /// sector LBA. The navigation VM uses this for time-search;
    /// libdvdnav will too once we integrate it (roadmap step 6).
    pub fn vts_tmapt(&self) -> Option<&sys::vts_tmapt_t> {
        unsafe { self.dvd_video().vts_tmapt.as_ref() }
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

    /// The `ttu_t` array embedded in `vts_ptt_srpt` — one entry per title
    /// owned by this VTS, each holding the title's chapter (PTT) array.
    pub fn ttus(&self) -> &[sys::ttu_t] {
        let Some(srpt) = self.vts_ptt_srpt() else {
            return &[];
        };
        if srpt.title.is_null() {
            return &[];
        }
        // SAFETY: libdvdread guarantees `title[0..nr_of_srpts]` is
        // initialized when `vts_ptt_srpt` is non-null.
        unsafe { slice::from_raw_parts(srpt.title, usize::from(srpt.nr_of_srpts)) }
    }

    /// The chapter list (`ptt_info_t[]`) for the title at `vts_ttn` within
    /// this VTS. Returns an empty slice if `vts_ttn` is out of range or the
    /// title's `ptt` pointer is NULL.
    ///
    /// Note: `vts_ttn` is 1-based per libdvdread's `title_info_t::vts_ttn`,
    /// so this method subtracts one internally.
    pub fn chapters_for(&self, vts_ttn: u8) -> &[sys::ptt_info_t] {
        let ttus = self.ttus();
        let Some(ttu) = ttus.get(usize::from(vts_ttn.saturating_sub(1))) else {
            return &[];
        };
        let nr = ttu.nr_of_ptts;
        let ptt = ttu.ptt;
        if ptt.is_null() || nr == 0 {
            return &[];
        }
        // SAFETY: libdvdread guarantees `ptt[0..nr_of_ptts]` is initialized
        // when `ttu.ptt` is non-null.
        unsafe { slice::from_raw_parts(ptt, usize::from(nr)) }
    }

    /// Cell-address table entries (`cell_adr_t[]`) from `vts_c_adt`.
    ///
    /// libdvdread doesn't store an explicit entry count on `c_adt_t`;
    /// the on-disc count is derived from `last_byte` per the DVD-Video
    /// spec:
    ///
    /// ```text
    /// nr_of_entries = (last_byte + 1 - C_ADT_SIZE) / CELL_ADDR_SIZE
    ///               = (last_byte - 7) / 12
    /// ```
    ///
    /// Note that libdvdread's own `ifo_print.c` uses `sizeof(c_adt_t)`
    /// (= 8) in this formula by mistake; that bug is cosmetic (it
    /// only affects the library's debug print) and we use the correct
    /// divisor 12 (= `CELL_ADDR_SIZE`) here.
    pub fn cell_adr_table(&self) -> &[sys::cell_adr_t] {
        let Some(c_adt) = self.vts_c_adt() else {
            return &[];
        };
        // Copy packed fields by value before doing arithmetic.
        let raw_last_byte: u32 = { c_adt.last_byte };
        let last_byte = u64::from(raw_last_byte);
        let ptr = { c_adt.cell_adr_table };
        if ptr.is_null() || last_byte + 1 < 8 + 12 {
            return &[];
        }
        let nr = ((last_byte + 1 - 8) / 12) as usize;
        // SAFETY: libdvdread reads `nr * CELL_ADDR_SIZE` bytes from the
        // IFO into `cell_adr_table` when the IFO parses successfully;
        // see `ifo_read.c:2202`.
        unsafe { slice::from_raw_parts(ptr, nr) }
    }

    /// VOBU starting-sector list from `vts_vobu_admap`.
    ///
    /// `vobu_admap_t` carries only a `last_byte` field plus the array
    /// pointer — the entry count is derived per the DVD-Video spec:
    ///
    /// ```text
    /// nr_of_entries = (last_byte + 1 - VOBU_ADMAP_SIZE) / 4
    ///               = (last_byte - 3) / 4
    /// ```
    pub fn vobu_start_sectors(&self) -> &[u32] {
        let Some(admap) = self.vts_vobu_admap() else {
            return &[];
        };
        let raw_last_byte: u32 = { admap.last_byte };
        let last_byte = u64::from(raw_last_byte);
        let ptr = { admap.vobu_start_sectors };
        if ptr.is_null() || last_byte + 1 < 4 + 4 {
            return &[];
        }
        let nr = ((last_byte + 1 - 4) / 4) as usize;
        // SAFETY: see the c_adt slice helper — same read pattern.
        unsafe { slice::from_raw_parts(ptr, nr) }
    }

    /// Time-map array (`vts_tmap_t[]`) from `vts_tmapt`. One entry per
    /// PGC that has a time-search table; each entry stores a list of
    /// `(time, sector_lba)` pairs for fast seek.
    pub fn tmaps(&self) -> &[sys::vts_tmap_t] {
        let Some(tmapt) = self.vts_tmapt() else {
            return &[];
        };
        let nr: u16 = { tmapt.nr_of_tmaps };
        let ptr = { tmapt.tmap };
        if ptr.is_null() || nr == 0 {
            return &[];
        }
        // SAFETY: libdvdread populates the time-map array when the IFO
        // parses successfully.
        unsafe { slice::from_raw_parts(ptr, usize::from(nr)) }
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
