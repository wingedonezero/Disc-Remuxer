//! Our FFmpeg (`libs/ffmpeg`), built by `build.rs` with the DVD-Video demuxer
//! on our libdvdread / libdvdnav / libdvdcss and linked statically.
//!
//! Declarations are added as the workspace uses them. `glue/glue.c` holds the
//! C side of calls that are awkward from Rust (log callback, option sets).

use std::os::raw::{c_char, c_int, c_uint};

// Keeps the DVD libraries linked after FFmpeg.
use libdvdnav_sys as _;

/// FFmpeg log levels (`libavutil/log.h`).
pub mod log_level {
    use std::os::raw::c_int;
    pub const QUIET: c_int = -8;
    pub const PANIC: c_int = 0;
    pub const FATAL: c_int = 8;
    pub const ERROR: c_int = 16;
    pub const WARNING: c_int = 24;
    pub const INFO: c_int = 32;
    pub const VERBOSE: c_int = 40;
    pub const DEBUG: c_int = 48;
    pub const TRACE: c_int = 56;
}

/// Receives one complete FFmpeg log line (without its newline).
pub type LogSink = unsafe extern "C" fn(level: c_int, line: *const c_char);

extern "C" {
    /// FFmpeg's version string (e.g. "9.0.2").
    pub fn av_version_info() -> *const c_char;
    pub fn avutil_version() -> c_uint;
    pub fn avcodec_version() -> c_uint;
    pub fn avformat_version() -> c_uint;
    /// Describes an AVERROR code in `errbuf`; returns < 0 when it is unknown.
    pub fn av_strerror(errnum: c_int, errbuf: *mut c_char, errbuf_size: usize) -> c_int;

    /// glue.c: routes FFmpeg's log (and through it libdvdread / libdvdnav's)
    /// to `sink`, for lines up to `max_level`.
    pub fn dr_log_install(sink: LogSink, max_level: c_int);
    /// glue.c: opens one title with the DVD-Video demuxer, reads stream
    /// information and logs FFmpeg's stream dump. 0 or a negative AVERROR.
    pub fn dr_probe_dvdvideo(path: *const c_char, title: c_int) -> c_int;
}

/// Splits a packed library version (`AV_VERSION_INT`) into major.minor.micro.
#[must_use]
pub fn version_triplet(v: c_uint) -> (u32, u32, u32) {
    (v >> 16, (v >> 8) & 0xff, v & 0xff)
}

/// Text of an AVERROR code.
#[must_use]
pub fn error_text(code: c_int) -> String {
    let mut buf = [0 as c_char; 256];
    // SAFETY: buf is writable for its length; av_strerror NUL-terminates.
    unsafe {
        av_strerror(code, buf.as_mut_ptr(), buf.len());
        std::ffi::CStr::from_ptr(buf.as_ptr()).to_string_lossy().into_owned()
    }
}
