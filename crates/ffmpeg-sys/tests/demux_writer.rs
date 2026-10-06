//! The elementary-stream writer of the C glue (`dr_demux`): each stream's
//! packets back to back in a file named `<prefix>_<NN>_<lang>_<codec>
//! [_<channel layout>][ DELAY <ms>ms].<ext>` (the layout from FFmpeg's decoder,
//! the delay = the first packet's time); PCM in a RIFF WAVE file whose sizes
//! are filled in at the end; sub-pictures as a `VobSub` pair (.sub packs,
//! .idx header and times). FFmpeg's own demuxers feed it here.

use std::ffi::{CStr, CString};
use std::fmt::Write as _;
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
    let mut untested = -1i64;
    // SAFETY: valid strings; run outlives the call; the callbacks match the glue's types.
    let ret = unsafe {
        let rp = (&raw mut run).cast();
        dr_demux(f.as_ptr(), p.as_ptr(), o.as_ptr(), std::ptr::null(), name_cb, rp, &raw mut nb, &raw mut untested,
            stream_cb, rp)
    };
    assert_eq!(ret, 0);
    assert_eq!(untested, 0);
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

/// The `VobSub` index header of a sub-picture track, with `palette`.
fn vobsub_header(width: i32, height: i32, palette: Option<&[u32; 16]>) -> String {
    let mut buf = vec![0 as c_char; 1024];
    // SAFETY: buf has 1024 bytes; the palette has 16 entries or is NULL.
    let n = unsafe {
        ffmpeg_sys::discrip::ff_discrip_vobsub_header(
            buf.as_mut_ptr(),
            1024,
            width,
            height,
            palette.map_or(std::ptr::null(), |p| p.as_ptr()),
        )
    };
    assert!(n > 0 && n < 1024);
    // SAFETY: NUL-terminated by snprintf.
    unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned()
}

#[test]
fn the_index_header_converts_the_palette_as_the_reference_does() {
    // 0x00 Y Cr Cb: black, white, and a colour whose green shows the
    // coefficients (0.714 x Cb, 0.346 x Cr as the reference applies them)
    let mut pal = [0x0010_8080u32; 16];
    pal[1] = 0x00eb_8080;
    pal[2] = 0x0051_5a5a; // Y 81, Cr -38, Cb -38
    let hdr = vobsub_header(1920, 1080, Some(&pal));
    assert!(hdr.starts_with("# VobSub index file, v7 (do not modify this line!)\n"));
    assert!(hdr.contains("\nsize: 1920x1080\n"));
    // Y 81: (255 * 65) / 219 = 75; R = 75 - 1.40222 x 38, G = 75 + (0.71448 + 0.34569) x 38, B = 75 - 1.771 x 38
    let luma = 75 * 65_536i64;
    let red = (luma - 91_894 * 38) >> 16;
    let green = (luma + 46_825 * 38 + 22_655 * 38) >> 16;
    let blue = (luma - 116_064 * 38) >> 16;
    let want = format!("palette: 000000, ffffff, {red:02x}{green:02x}{blue:02x}, 000000,");
    assert!(hdr.contains(&want), "{hdr}");
}

#[test]
fn sub_pictures_go_into_a_vobsub_pair() {
    // a .idx/.sub pair as input (FFmpeg's VobSub demuxer), two units: the
    // output .sub holds them in packs at the stated positions, the .idx the
    // header and their times
    let dir = tmp("vobsub");
    std::fs::create_dir_all(&dir).unwrap();
    let unit = |n: usize, fill: u8| -> Vec<u8> {
        // a DVD-Video unit: size, control offset, pixel data, one control
        // sequence (STA_DSP, SET_DSPXA, CMD_END) pointing to itself
        let mut u = vec![0u8; 4];
        u.extend(std::iter::repeat_n(fill, n));
        let dcsq = u16::try_from(u.len()).unwrap();
        u.extend_from_slice(&[0, 0]);
        u.extend_from_slice(&dcsq.to_be_bytes());
        u.extend_from_slice(&[0x01, 0x06, 0, 4, 0, 5, 0xFF]);
        let len = u.len();
        u[0..2].copy_from_slice(&u16::try_from(len).unwrap().to_be_bytes());
        u[2..4].copy_from_slice(&dcsq.to_be_bytes());
        u
    };
    let units = [unit(100, 0x11), unit(3000, 0x22)];
    // the input pair, written by hand: one pack per unit part
    let mut sub = Vec::new();
    let mut idx = vobsub_header(720, 480, None);
    idx.push_str("\nid: en, index: 0\n");
    for (u, ms) in units.iter().zip([1000u64, 2500]) {
        writeln!(idx, "timestamp: 00:00:{:02}:{:03}, filepos: {:09x}", ms / 1000, ms % 1000, sub.len()).unwrap();
        let pts = ms * 90;
        let mut rest = &u[..];
        let mut first = true;
        while first || !rest.is_empty() {
            let hdr = if first { 5 } else { 0 };
            let room = 2048 - 14 - 9 - hdr - 1;
            let n = rest.len().min(room);
            let mut pk = vec![0u8, 0, 1, 0xBA, 0x44, 0, 4, 0, 4, 1, 1, 0x89, 0xC3, 0xF8];
            pk.extend_from_slice(&[0, 0, 1, 0xBD]);
            pk.extend_from_slice(&u16::try_from(3 + hdr + 1 + n).unwrap().to_be_bytes());
            pk.extend_from_slice(&[0x81, if first { 0x80 } else { 0 }, u8::try_from(hdr).unwrap()]);
            if first {
                // '0010' PTS[32..30] '1' PTS[29..15] '1' PTS[14..0] '1'
                let b = |v: u64| u8::try_from(v & 0xFF).unwrap();
                pk.extend_from_slice(&[0x21 | b((pts >> 29) & 0x0E), b(pts >> 22), b(pts >> 14) | 1, b(pts >> 7), b(pts << 1) | 1]);
            }
            pk.push(0x20);
            pk.extend_from_slice(&rest[..n]);
            let left = 2048 - pk.len();
            if left > 0 {
                pk.extend_from_slice(&[0, 0, 1, 0xBE]);
                pk.extend_from_slice(&u16::try_from(left - 6).unwrap().to_be_bytes());
                pk.resize(2048, 0xFF);
            }
            sub.extend(pk);
            rest = &rest[n..];
            first = false;
        }
    }
    std::fs::write(dir.join("in.sub"), &sub).unwrap();
    std::fs::write(dir.join("in.idx"), &idx).unwrap();
    let files = demux(&dir, "vobsub", &dir.join("in.idx"), "");
    let want = dir.join("T_t00_00_en_VobSub.sub");
    assert_eq!(files, vec![want.to_string_lossy().into_owned()]);
    let out = std::fs::read(&want).unwrap();
    assert_eq!(out.len(), 3 * 2048, "the second unit takes two packs");
    // the units back out of our packs, as FFmpeg's demuxer reads them
    let mut got = Vec::new();
    for pk in out.chunks(2048) {
        assert_eq!(&pk[0..4], &[0, 0, 1, 0xBA]);
        assert_eq!(&pk[14..18], &[0, 0, 1, 0xBD]);
        let len = usize::from(u16::from_be_bytes([pk[18], pk[19]]));
        let hl = usize::from(pk[22]);
        assert_eq!(pk[23 + hl], 0x20);
        got.extend_from_slice(&pk[24 + hl..20 + len]);
    }
    assert_eq!(got, units.concat());
    let out_idx = std::fs::read_to_string(dir.join("T_t00_00_en_VobSub.idx")).unwrap();
    assert!(out_idx.starts_with("# VobSub index file, v7"));
    assert!(out_idx.contains("id: en, index: 0\n"), "the language as the input names it");
    assert!(out_idx.contains("timestamp: 00:00:01:000, filepos: 000000000\n"), "{out_idx}");
    assert!(out_idx.contains("timestamp: 00:00:02:500, filepos: 000000800\n"), "{out_idx}");
}
