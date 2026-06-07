//! DVD backend error type.
//!
//! Every DVD / libdvd* failure mode lives here, keeping
//! `disc_core::DiscError` free of format-specific concepts. At the public
//! boundary (the facade / `DiscBackend` impl) a `DvdError` converts into
//! `DiscError::Backend` via the `From` impl below.

use std::io;
use std::path::PathBuf;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum DvdError {
    #[error("could not open disc at {path}: {reason}")]
    OpenFailed { path: PathBuf, reason: String },

    #[error("invalid path: contains interior NUL byte")]
    InvalidPath,

    #[error("IFO {ifo_nr} could not be opened (libdvdread returned NULL)")]
    IfoOpenFailed { ifo_nr: u32 },

    #[error("could not open file: vts={vts_nr} domain={domain} ({reason})")]
    FileOpenFailed {
        vts_nr: u32,
        domain: &'static str,
        reason: String,
    },

    #[error("sector read out of range: offset={offset} count={count} total_blocks={total}")]
    ReadOutOfRange { offset: u32, count: u32, total: u32 },

    #[error("sector read failed: offset={offset} count={count} (libdvdread returned {ret})")]
    ReadFailed { offset: u32, count: u32, ret: i32 },

    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
}

impl From<DvdError> for disc_core::DiscError {
    fn from(e: DvdError) -> Self {
        disc_core::DiscError::Backend {
            source: Box::new(e),
        }
    }
}
