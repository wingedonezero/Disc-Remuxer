//! libgcrypt, compiled from `libs/libgcrypt` by `build.rs` and linked
//! statically (the cryptography of libaacs).

use std::os::raw::c_char;

// Keeps libgpg-error linked after libgcrypt.
use libgpg_error_sys as _;

/// Version of the compiled libgcrypt copy.
pub const VERSION: &str = "1.12.4";

extern "C" {
    /// The library's version when it is at least `req` (NULL: any), else NULL.
    pub fn gcry_check_version(req: *const c_char) -> *const c_char;
}
