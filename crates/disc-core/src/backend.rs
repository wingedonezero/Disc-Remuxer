//! The disc-backend trait and an opened-disc [`Session`].
//!
//! A backend (`disc-dvd` now; `disc-bd` / `disc-uhd` / `disc-hddvd` later)
//! knows how to read its format and produce the uniform
//! [`TitleCollection`]. The facade opens the right backend for a disc's
//! type, wraps it in a [`Session`] — the title tree plus the user's
//! selection state — which the CLI and a future UI drive. Format-specific
//! rip execution stays inside each backend's crate; this trait carries
//! only what the format-agnostic layer needs.

use std::path::Path;

use crate::model::TitleCollection;
use crate::{DiscError, DiscType};

/// A format backend: reports its type and enumerates a disc into the
/// uniform model.
///
/// Object-safe so the facade can hold a `Box<dyn DiscBackend>` and dispatch
/// by disc type.
pub trait DiscBackend {
    /// The kind of disc this backend handles.
    fn disc_type(&self) -> DiscType;

    /// The path the backend was opened from.
    fn path(&self) -> &Path;

    /// Build the uniform title / track tree for this disc.
    fn enumerate(&self) -> Result<TitleCollection, DiscError>;
}

/// An opened disc: the backend plus its enumerated title tree and the
/// current selection state.
///
/// `Session::new` enumerates eagerly so callers immediately have the tree
/// to present; the backend is retained for later operations (and so a
/// future UI can re-enumerate or rip without reopening).
pub struct Session {
    backend: Box<dyn DiscBackend>,
    collection: TitleCollection,
}

impl Session {
    /// Open a session over a backend, enumerating its titles up front.
    pub fn new(backend: Box<dyn DiscBackend>) -> Result<Self, DiscError> {
        let collection = backend.enumerate()?;
        Ok(Self {
            backend,
            collection,
        })
    }

    /// The disc type.
    #[must_use]
    pub fn disc_type(&self) -> DiscType {
        self.backend.disc_type()
    }

    /// The path the session was opened from.
    #[must_use]
    pub fn path(&self) -> &Path {
        self.backend.path()
    }

    /// The underlying backend (for backend-specific operations).
    #[must_use]
    pub fn backend(&self) -> &dyn DiscBackend {
        self.backend.as_ref()
    }

    /// The enumerated title tree (with current `enabled` flags).
    #[must_use]
    pub fn collection(&self) -> &TitleCollection {
        &self.collection
    }

    /// Mutable access to the title tree (for callers driving selection
    /// directly).
    pub fn collection_mut(&mut self) -> &mut TitleCollection {
        &mut self.collection
    }

    /// Apply a [`Selection`](crate::selection::Selection), setting the
    /// `enabled` flags across the title tree.
    pub fn select(&mut self, selection: &crate::selection::Selection) {
        selection.apply(&mut self.collection);
    }
}
