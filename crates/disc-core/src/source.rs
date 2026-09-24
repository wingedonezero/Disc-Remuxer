//! The `DiscSource` trait — the common surface every backend implements.
//!
//! Right now this is intentionally minimal. Each backend exposes its
//! disc-type-specific details (IFO walkers, title trees, etc.) as concrete
//! types in its own crate; the trait only carries what the CLI / future
//! bindings need for dispatch and reporting. We grow this surface as the
//! backends mature.

use std::path::Path;

use crate::DiscType;

/// A handle to an open disc — the unified abstraction over DVD / Blu-ray /
/// UHD backends.
pub trait DiscSource {
    /// The kind of disc this source represents. Stable for the source's
    /// lifetime.
    fn disc_type(&self) -> DiscType;

    /// The path the source was opened from.
    fn path(&self) -> &Path;
}
