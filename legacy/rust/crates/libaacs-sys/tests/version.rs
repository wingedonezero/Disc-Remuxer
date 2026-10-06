//! The compiled libaacs, libgcrypt and libgpg-error link statically and are
//! our versions.

use std::ffi::CStr;

#[test]
fn the_compiled_versions_are_ours() {
    let (mut a, mut b, mut c) = (0, 0, 0);
    unsafe { libaacs_sys::aacs_get_version(&raw mut a, &raw mut b, &raw mut c) };
    assert_eq!(format!("{a}.{b}.{c}"), libaacs_sys::VERSION);
    let v = unsafe { CStr::from_ptr(libgcrypt_sys::gcry_check_version(std::ptr::null())) };
    assert_eq!(v.to_str().unwrap(), libgcrypt_sys::VERSION);
}
