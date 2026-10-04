//! Calls into our FFmpeg: versions, log routing, the stock DVD-Video probe.

use ffmpeg_sys::log_level;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::path::Path;

pub fn version() -> String {
    // SAFETY: av_version_info returns a static NUL-terminated string.
    unsafe { CStr::from_ptr(ffmpeg_sys::av_version_info()) }
        .to_string_lossy()
        .into_owned()
}

pub fn library_versions() -> [(&'static str, (u32, u32, u32)); 3] {
    use ffmpeg_sys::version_triplet as t;
    // SAFETY: plain version getters without arguments.
    unsafe {
        [
            ("libavutil", t(ffmpeg_sys::avutil_version())),
            ("libavcodec", t(ffmpeg_sys::avcodec_version())),
            ("libavformat", t(ffmpeg_sys::avformat_version())),
        ]
    }
}

/// FFmpeg's log (with libdvdread / libdvdnav's, which FFmpeg forwards) goes
/// to our log under the target `ffmpeg`.
pub fn route_log(level: log::LevelFilter) {
    let max = match level {
        log::LevelFilter::Off => log_level::QUIET,
        log::LevelFilter::Error => log_level::ERROR,
        log::LevelFilter::Warn => log_level::WARNING,
        log::LevelFilter::Info => log_level::INFO,
        log::LevelFilter::Debug => log_level::DEBUG,
        log::LevelFilter::Trace => log_level::TRACE,
    };
    // SAFETY: sink is a valid extern "C" fn for the program's lifetime.
    unsafe { ffmpeg_sys::dr_log_install(sink, max) };
}

unsafe extern "C" fn sink(level: c_int, line: *const c_char) {
    // SAFETY: glue.c passes a NUL-terminated line.
    let text = unsafe { CStr::from_ptr(line) }.to_string_lossy();
    let level = match level {
        l if l <= log_level::ERROR => log::Level::Error,
        l if l <= log_level::WARNING => log::Level::Warn,
        l if l <= log_level::INFO => log::Level::Info,
        l if l <= log_level::DEBUG => log::Level::Debug,
        _ => log::Level::Trace,
    };
    log::log!(target: "ffmpeg", level, "{text}");
}

/// Opens one title with FFmpeg's DVD-Video demuxer and logs FFmpeg's stream
/// dump; the error is FFmpeg's reason.
pub fn probe_dvdvideo(source: &Path, title: i32) -> Result<(), String> {
    let path = CString::new(source.as_os_str().as_encoded_bytes())
        .map_err(|_| "the path contains a NUL byte".to_string())?;
    // SAFETY: path is a valid NUL-terminated string.
    let ret = unsafe { ffmpeg_sys::dr_probe_dvdvideo(path.as_ptr(), title) };
    if ret < 0 {
        return Err(ffmpeg_sys::error_text(ret));
    }
    Ok(())
}
