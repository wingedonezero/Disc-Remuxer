//! The elementary-stream writer of the C glue (`dr_demux`): each stream's
//! packets back to back in a file named `<prefix>_<NN>_<lang>_<codec>
//! [_<channel layout>][ DELAY <ms>ms].<ext>` (the layout from FFmpeg's decoder,
//! the delay = the first packet's time); PCM in a RIFF WAVE file whose sizes
//! are filled in at the end. FFmpeg's own demuxers feed it here.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};

use ffmpeg_sys::{dr_demux, DemuxStreamStats};

struct Run {
    prefix: CString,
    files: Vec<String>,
}

unsafe extern "C" fn name_cb(o: *mut c_void, _title: *const c_char, out: *mut c_char, size: c_int) -> c_int {
    // SAFETY: o is the &mut Run; out has size bytes.
    let r = unsafe { &mut *o.cast::<Run>() };
    let b = r.prefix.as_bytes_with_nul();
    assert!(b.len() <= usize::try_from(size).unwrap());
    // SAFETY: room checked above.
    unsafe { std::ptr::copy_nonoverlapping(b.as_ptr(), out.cast::<u8>(), b.len()) };
    0
}

unsafe extern "C" fn stream_cb(o: *mut c_void, st: *const DemuxStreamStats) {
    // SAFETY: o is the &mut Run; st valid during the call with a C string file.
    let (r, s) = unsafe { (&mut *o.cast::<Run>(), &*st) };
    // SAFETY: as above.
    r.files.push(unsafe { CStr::from_ptr(s.file) }.to_string_lossy().into_owned());
}

fn demux(dir: &Path, format: &str, input: &Path, options: &str) -> Vec<String> {
    std::fs::create_dir_all(dir).unwrap();
    let mut run = Run { prefix: CString::new(dir.join("T_t00").to_str().unwrap()).unwrap(), files: Vec::new() };
    let f = CString::new(format).unwrap();
    let p = CString::new(input.to_str().unwrap()).unwrap();
    let o = CString::new(options).unwrap();
    let mut nb: c_int = 0;
    // SAFETY: valid strings; run outlives the call; the callbacks match the glue's types.
    let ret = unsafe {
        let rp = (&raw mut run).cast();
        dr_demux(f.as_ptr(), p.as_ptr(), o.as_ptr(), std::ptr::null(), name_cb, rp, &raw mut nb, stream_cb, rp)
    };
    assert_eq!(ret, 0);
    run.files
}

fn tmp(name: &str) -> PathBuf {
    let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("demux_writer").join(name);
    let _ = std::fs::remove_dir_all(&d);
    d
}

fn le(b: &[u8], at: usize, n: usize) -> u64 {
    b[at..at + n].iter().rev().fold(0, |v, &x| (v << 8) | u64::from(x))
}

#[test]
fn an_audio_stream_is_named_by_codec_layout_and_delay() {
    let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/data/sine_7680.ac3");
    let dir = tmp("ac3");
    let files = demux(&dir, "ac3", &src, "");
    let want = dir.join("T_t00_00_und_DD_stereo DELAY 0ms.ac3");
    assert_eq!(files, vec![want.to_string_lossy().into_owned()]);
    assert_eq!(std::fs::read(&want).unwrap(), std::fs::read(&src).unwrap(), "the frames back to back");
}

#[test]
fn pcm_goes_into_a_wave_file() {
    // 16-bit stereo: WAVE_FORMAT_PCM, 44-byte header
    let dir = tmp("pcm16");
    std::fs::create_dir_all(&dir).unwrap();
    let raw: Vec<u8> = (0..19200u32).map(|i| u8::try_from(i % 253).unwrap()).collect();
    let src = dir.join("in.raw");
    std::fs::write(&src, &raw).unwrap();
    let files = demux(&dir, "s16le", &src, "sample_rate=48000:ch_layout=stereo");
    let want = dir.join("T_t00_00_und_LPCM_stereo DELAY 0ms.wav");
    assert_eq!(files, vec![want.to_string_lossy().into_owned()]);
    let w = std::fs::read(&want).unwrap();
    assert_eq!(&w[0..4], b"RIFF");
    assert_eq!(le(&w, 4, 4), 36 + 19200);
    assert_eq!(&w[8..16], b"WAVEfmt ");
    assert_eq!((le(&w, 16, 4), le(&w, 20, 2), le(&w, 22, 2)), (16, 1, 2));
    assert_eq!((le(&w, 24, 4), le(&w, 28, 4), le(&w, 32, 2), le(&w, 34, 2)), (48000, 192_000, 4, 16));
    assert_eq!(&w[36..40], b"data");
    assert_eq!(le(&w, 40, 4), 19200);
    assert_eq!(&w[44..], &raw[..]);
}

#[test]
fn multichannel_pcm_takes_the_extensible_header() {
    // 24-bit 5.1: WAVE_FORMAT_EXTENSIBLE with the channel mask, 68-byte header
    let dir = tmp("pcm24");
    std::fs::create_dir_all(&dir).unwrap();
    let raw = vec![0x11u8; 18 * 1000];
    let src = dir.join("in.raw");
    std::fs::write(&src, &raw).unwrap();
    let files = demux(&dir, "s24le", &src, "sample_rate=48000:ch_layout=5.1");
    let want = dir.join("T_t00_00_und_LPCM_5.1 DELAY 0ms.wav");
    assert_eq!(files, vec![want.to_string_lossy().into_owned()]);
    let w = std::fs::read(&want).unwrap();
    assert_eq!((le(&w, 16, 4), le(&w, 20, 2), le(&w, 22, 2)), (40, 0xFFFE, 6));
    assert_eq!((le(&w, 32, 2), le(&w, 34, 2)), (18, 24));
    assert_eq!((le(&w, 36, 2), le(&w, 38, 2), le(&w, 40, 4)), (22, 24, 0x3F));
    assert_eq!(&w[60..64], b"data");
    assert_eq!(le(&w, 64, 4), 18000);
    assert_eq!(w.len(), 68 + 18000);
}
