//! libaacs, compiled from `libs/libaacs` by `build.rs` and linked statically
//! on our libgcrypt / libgpg-error.
//!
//! Declarations are added here as the workspace starts to call the library
//! directly.

use std::os::raw::c_int;

// Keeps libgcrypt and libgpg-error linked after libaacs.
use libgcrypt_sys as _;

/// Version of the compiled libaacs copy.
pub const VERSION: &str = "0.12.0";

extern "C" {
    /// `aacs_get_version`: major, minor, micro.
    pub fn aacs_get_version(major: *mut c_int, minor: *mut c_int, micro: *mut c_int);
}
