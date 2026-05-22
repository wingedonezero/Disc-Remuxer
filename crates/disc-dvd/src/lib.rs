//! Safe wrappers around `libdvdread` and `libdvdnav` for DVD-Video discs.
//!
//! The module structure tracks the libdvdread API surface:
//!
//! * [`reader`] — `DvdReader`, RAII wrapper for `DVDOpen` / `DVDClose`.
//! * [`ifo`] — `IfoHandle`, RAII wrapper for `ifoOpen` / `ifoClose`, plus
//!   safe accessors for the public IFO structures (`vmgi_mat_t`,
//!   `tt_srpt_t`, `pgcit_t`, etc.). Field names are exposed under their
//!   libdvdread C-header names.
//! * [`decode`] — pure-Rust decoders for the bit-packed audio / video /
//!   subpicture attribute fields, plus MPEG-PS stream-ID mapping. Returns
//!   `&'static str` for human-readable names and bit-helpers for the
//!   per-PGC `audio_control` / `subp_control` arrays.
//! * [`source`] — `DvdSource`, the `disc_core::DiscSource` impl that the
//!   CLI hands a generic disc to.
//! * [`css`] — `CssProbe`, a thin libdvdcss wrapper that reports whether
//!   a disc is CSS-scrambled. libdvdread itself doesn't expose this state
//!   through its API; libdvdcss does, via `dvdcss_is_scrambled()`.
//! * [`file`] — `DvdFile`, RAII wrapper for `DVDOpenFile` / `DVDReadBlocks` /
//!   `DVDCloseFile`. The sector-reading layer, with built-in range checks
//!   and short-read warnings.
//! * [`cell`] — `CellInfo` + `cells_in_pgc()` iterator: decoded snapshot
//!   of a PGC's `cell_playback` array, with per-cell range checks used
//!   to drive title dumps.
//!
//! All public types log via the `log` crate at appropriate levels:
//! `info!` for major lifecycle events (open/close), `debug!` for IFO
//! reads, `trace!` for byte-level activity once we add demuxing.

pub mod cell;
pub mod chapters;
pub mod css;
pub mod decode;
pub mod demux;
pub mod file;
pub mod ifo;
pub mod mpegps;
pub mod nav;
pub mod nav_cells;
pub mod reader;
pub mod source;
pub mod video_es;
pub mod vobsub;

pub use cell::{
    cells_in_pgc, check_cell_vs_c_adt, check_cell_walk, find_cell_adr, CellInfo,
};
pub use css::CssProbe;
pub use demux::{DemuxError, DemuxSummary, Demuxer, MagicCheck, StreamKey, StreamStats};
pub use file::{DvdFile, ReadDomain, BLOCK_SIZE};
pub use ifo::{IfoHandle, IfoKind};
pub use mpegps::{scan_sector, stream_kind, MpegPsError, PackHeader, PesPacket, StreamKind};
pub use nav::{DvdNav, NavEvent};
pub use nav_cells::{CellKey, CellLookup};
pub use reader::DvdReader;
pub use source::DvdSource;
