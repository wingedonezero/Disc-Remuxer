//! libdvdcss (CSS decryption for DVD-Video), compiled from `libs/libdvdcss`
//! by `build.rs` and linked statically.
//!
//! Declarations are added here as the workspace starts to call the library
//! directly; today it is used through libdvdread.

/// Version of the compiled libdvdcss copy.
pub const VERSION: &str = "1.6.0";
