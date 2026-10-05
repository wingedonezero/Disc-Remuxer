//! The static FFmpeg build links and reports the version it was built from.

use std::ffi::CStr;

#[test]
fn version_is_the_libs_copy() {
    // SAFETY: av_version_info returns a static NUL-terminated string.
    let v = unsafe { CStr::from_ptr(ffmpeg_sys::av_version_info()) };
    assert_eq!(v.to_str().unwrap(), "9.0.2");
}

#[test]
fn dvdvideo_demuxer_is_built() {
    // A missing input is an error from inside the demuxer, not
    // AVERROR_DEMUXER_NOT_FOUND (which the glue returns without it).
    let path = std::ffi::CString::new("/nonexistent-disc-remuxer-test").unwrap();
    // SAFETY: path is a valid NUL-terminated string.
    let ret = unsafe { ffmpeg_sys::dr_probe_dvdvideo(path.as_ptr(), c"title=1".as_ptr()) };
    assert!(ret < 0);
    let not_found = -i32::from_le_bytes([0xF8, b'D', b'E', b'M']);
    assert_ne!(ret, not_found, "{}", ffmpeg_sys::error_text(ret));
}
