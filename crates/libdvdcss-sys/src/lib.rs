//! libdvdcss (CSS decryption for DVD-Video), compiled from `libs/libdvdcss`
//! by `build.rs` and linked statically.
//!
//! Declarations are added here as the workspace starts to call the library
//! directly.

use std::os::raw::{c_int, c_uchar, c_void};

/// Version of the compiled libdvdcss copy.
pub const VERSION: &str = "1.6.0";

/// `DVDCSS_BLOCK_SIZE`.
pub const BLOCK_SIZE: usize = 2048;
/// `DVDCSS_KEY_SIZE`.
pub const KEY_SIZE: usize = 5;
/// `DVDCSS_NOFLAGS`.
pub const NOFLAGS: c_int = 0;
/// `DVDCSS_READ_DECRYPT`.
pub const READ_DECRYPT: c_int = 1;
/// `DVDCSS_SEEK_KEY`.
pub const SEEK_KEY: c_int = 2;

/// `struct dvdcss_s` (only handled through pointers).
#[repr(C)]
pub struct Dvdcss {
    _private: [u8; 0],
}

/// `dvdcss_stream_cb`: seek to a byte position, read bytes.
#[repr(C)]
pub struct StreamCb {
    pub pf_seek: Option<unsafe extern "C" fn(p_stream: *mut c_void, i_pos: u64) -> c_int>,
    pub pf_read: Option<unsafe extern "C" fn(p_stream: *mut c_void, buffer: *mut c_void, i_read: c_int) -> c_int>,
    pub pf_readv: Option<unsafe extern "C" fn(p_stream: *mut c_void, p_iovec: *const c_void, i_blocks: c_int) -> c_int>,
}

extern "C" {
    pub fn dvdcss_open_stream(p_stream: *mut c_void, p_stream_cb: *mut StreamCb) -> *mut Dvdcss;
    /// Our addition: `dvdcss_open_stream` without the title key cache.
    pub fn dvdcss_open_stream_uncached(p_stream: *mut c_void, p_stream_cb: *mut StreamCb) -> *mut Dvdcss;
    pub fn dvdcss_close(dvdcss: *mut Dvdcss) -> c_int;
    pub fn dvdcss_seek(dvdcss: *mut Dvdcss, i_blocks: c_int, i_flags: c_int) -> c_int;
    pub fn dvdcss_read(dvdcss: *mut Dvdcss, p_buffer: *mut c_void, i_blocks: c_int, i_flags: c_int) -> c_int;
    /// Our addition: the title key of the title starting at `i_block`
    /// (1 = key, 0 = not scrambled, < 0 = failure).
    pub fn dvdcss_title_key(dvdcss: *mut Dvdcss, i_block: c_int, p_key: *mut c_uchar) -> c_int;
    /// Our addition: descramble one sector in place (1 = descrambled, 0 = not
    /// scrambled, -1 = scrambled but no key).
    pub fn dvdcss_unscramble_sector(p_key: *const c_uchar, p_sector: *mut c_uchar) -> c_int;
}
