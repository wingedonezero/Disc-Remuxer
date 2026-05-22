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
//!
//! All public types log via the `log` crate at appropriate levels:
//! `info!` for major lifecycle events (open/close), `debug!` for IFO
//! reads, `trace!` for byte-level activity once we add demuxing.

pub mod decode;
pub mod ifo;
pub mod reader;
pub mod source;

pub use ifo::{IfoHandle, IfoKind};
pub use reader::DvdReader;
pub use source::DvdSource;
