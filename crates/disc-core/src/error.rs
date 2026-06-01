//! Common error type returned at the cross-format boundary.
//!
//! `disc-core` carries no format-specific concepts. Each backend
//! (`disc-dvd`, future `disc-bd` / `disc-uhd`) defines its own internal
//! error type (e.g. `disc_dvd::DvdError`) and converts into `DiscError` at
//! the public boundary via the `Backend` variant. The CLI / future bindings
//! only need to handle these top-level variants.

use std::io;
use std::path::PathBuf;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum DiscError {
    #[error("path does not exist: {0}")]
    PathNotFound(PathBuf),

    #[error("disc type at {0} is not recognized (no VIDEO_TS/, BDMV/, or recognized image found)")]
    UnknownDiscType(PathBuf),

    #[error("requested feature not yet implemented: {0}")]
    Unsupported(&'static str),

    /// A backend's internal error, surfaced at the format-agnostic boundary.
    /// Backends construct this via their own `From<XxxError> for DiscError`.
    #[error("backend error: {source}")]
    Backend {
        #[source]
        source: Box<dyn std::error::Error + Send + Sync + 'static>,
    },

    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
}
