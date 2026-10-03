//! libdvdnav (DVD-Video navigation), compiled from `libs/libdvdnav` by
//! `build.rs` against our libdvdread and linked statically.
//!
//! Declarations are added here as the workspace starts to call the library
//! directly; today it is used through FFmpeg's DVD-Video demuxer.

// Keeps libdvdread linked after libdvdnav.
use libdvdread_sys as _;

/// Version of the compiled libdvdnav copy.
pub const VERSION: &str = "7.0.0";
