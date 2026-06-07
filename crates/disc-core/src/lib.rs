//! Core types shared by all disc backends.
//!
//! This crate carries the traits and error types that every backend
//! (`disc-dvd`, future `disc-bd` and `disc-uhd`) implements. It has no FFI;
//! it's pure-Rust so it can be reused from tools, tests, and the safe
//! Python/PyQt bindings we'll add later.

pub mod backend;
pub mod check;
pub mod detect;
pub mod error;
pub mod model;
pub mod selection;
pub mod source;

pub use backend::{DiscBackend, Session};
pub use check::{check, check_eq, check_in_range, require_eq};
pub use detect::{detect_disc_type, DiscType};
pub use error::DiscError;
pub use model::{SkipReason, Title, TitleCollection, Track, TrackKind};
pub use selection::{mark_min_length, Selection, TitleSelector, TrackSelector};
pub use source::DiscSource;
