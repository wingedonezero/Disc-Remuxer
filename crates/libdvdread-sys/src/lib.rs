//! libdvdread (DVD-Video file and IFO reading), compiled from `libs/libdvdread`
//! by `build.rs` against our libdvdcss and linked statically.
//!
//! Declarations are added here as the workspace starts to call the library
//! directly; today it is used through FFmpeg's DVD-Video demuxer.

// Keeps libdvdcss linked after libdvdread.
use libdvdcss_sys as _;

/// Version of the compiled libdvdread copy.
pub const VERSION: &str = "7.1.1";
