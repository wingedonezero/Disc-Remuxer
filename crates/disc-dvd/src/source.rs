//! `DvdSource` — the `disc_core::DiscSource` implementation for DVD-Video.
//!
//! Wraps a [`DvdReader`] and exposes [`DiscSource`] to generic callers, plus
//! a `reader()` accessor for code that needs the lower-level handle (the
//! CLI's `info` command, future demuxer, etc.).

use std::path::Path;

use disc_core::{DiscSource, DiscType};

use crate::DvdError;

use crate::DvdReader;

pub struct DvdSource {
    reader: DvdReader,
}

impl DvdSource {
    /// Open a DVD-Video source at the given path. Delegates to
    /// [`DvdReader::open`].
    pub fn open(path: &Path) -> Result<Self, DvdError> {
        Ok(Self {
            reader: DvdReader::open(path)?,
        })
    }

    /// Borrow the underlying [`DvdReader`] for IFO / sector access.
    #[must_use]
    pub fn reader(&self) -> &DvdReader {
        &self.reader
    }
}

impl DiscSource for DvdSource {
    fn disc_type(&self) -> DiscType {
        DiscType::Dvd
    }

    fn path(&self) -> &Path {
        self.reader.path()
    }
}
