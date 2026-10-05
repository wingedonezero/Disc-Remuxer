//! Calls into our FFmpeg: versions, log routing, the stock DVD-Video probe.

use disc_core::settings::Settings;
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

/// The DVD-Video demuxer's options for opening title `title` with the read
/// settings in effect, as `key=value` pairs joined by `:`.
#[must_use]
pub fn dvdvideo_options(settings: &Settings, title: i32) -> String {
    format!(
        "title={title}:read_attempts={}:udf_reader={}:prefer_iso_old_udf102={}",
        settings.number("read.attempts"),
        settings.text("read.udf_reader").unwrap_or("netbsd"),
        i32::from(settings.switch("read.prefer_iso_for_old_udf102")),
    )
}

/// Opens one title with FFmpeg's DVD-Video demuxer and logs FFmpeg's stream
/// dump; the error is FFmpeg's reason.
pub fn probe_dvdvideo(source: &Path, title: i32, settings: &Settings) -> Result<(), String> {
    let path = CString::new(source.as_os_str().as_encoded_bytes())
        .map_err(|_| "the path contains a NUL byte".to_string())?;
    let options = CString::new(dvdvideo_options(settings, title)).expect("no NUL in the options");
    // SAFETY: path and options are valid NUL-terminated strings.
    let ret = unsafe { ffmpeg_sys::dr_probe_dvdvideo(path.as_ptr(), options.as_ptr()) };
    if ret < 0 {
        return Err(ffmpeg_sys::error_text(ret));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn read_settings_become_demuxer_options() {
        let mut s = Settings::defaults();
        assert_eq!(dvdvideo_options(&s, 3), "title=3:read_attempts=5:udf_reader=netbsd:prefer_iso_old_udf102=1");
        s.set_from_command_line("read.attempts=9").unwrap();
        s.set_from_command_line("read.udf_reader=linux").unwrap();
        s.set_from_command_line("read.prefer_iso_for_old_udf102=false").unwrap();
        assert_eq!(dvdvideo_options(&s, 1), "title=1:read_attempts=9:udf_reader=linux:prefer_iso_old_udf102=0");
    }
}
