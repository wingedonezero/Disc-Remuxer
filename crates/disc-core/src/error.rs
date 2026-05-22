//! Common error type returned by every backend.
//!
//! Backends can carry their own internal errors and convert into `DiscError`
//! at the public boundary via the `OpenFailed { reason }` / `BackendError`
//! variants. The CLI / future bindings only need to handle these top-level
//! variants.

use std::io;
use std::path::PathBuf;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum DiscError {
    #[error("path does not exist: {0}")]
    PathNotFound(PathBuf),

    #[error("could not open disc at {path}: {reason}")]
    OpenFailed { path: PathBuf, reason: String },

    #[error("disc type at {0} is not recognized (no VIDEO_TS/, BDMV/, or recognized image found)")]
    UnknownDiscType(PathBuf),

    #[error("IFO {ifo_nr} could not be opened (libdvdread returned NULL)")]
    IfoOpenFailed { ifo_nr: u32 },

    #[error("invalid path: contains interior NUL byte")]
    InvalidPath,

    #[error("requested feature not yet implemented: {0}")]
    Unsupported(&'static str),

    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
}
