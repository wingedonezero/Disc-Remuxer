//! DVD-Video line-21 captions in the rip core: the MPEG-2 rules take GOP
//! user data out of the video as side units, timed at the next picture
//! after their unit (`discrip_mpv.c`, `discrip_video.c`); `discrip_cc.c`
//! checks a caption block and turns its entries into the caption decoder's
//! input (field markers 4 / 5). The video is the MPEG-2 test stream
//! (tests/data/README.md) with user data inserted after its GOP headers.

mod common;

use std::ffi::CString;
use std::os::raw::{c_int, c_void};

use common::discrip::pictures;
use ffmpeg_sys::discrip::{
    codec_id, ff_discrip_cc_check, ff_discrip_cc_triplets, ff_discrip_cutter_close, ff_discrip_cutter_flush,
    ff_discrip_cutter_open, ff_discrip_cutter_write, ff_discrip_frame_unref, ff_discrip_video_close,
    ff_discrip_video_flush, ff_discrip_video_open, ff_discrip_video_set_side, ff_discrip_video_stats,
    ff_discrip_video_unit, Cutter, Frame, Video, VideoStats, F_KEY,
};

const M2V: &[u8] = include_bytes!("data/testsrc_1200.m2v");
const FRAME: i64 = 36_036_000; // 1001/30000 s
const BASE: i64 = 10 * 1_080_000_000;

#[derive(Default)]
struct Sink {
    out: Vec<(Vec<u8>, i64, i64, u32)>, // bytes, time, dur, flags
}

unsafe extern "C" fn on_frame(o: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: o is the &mut Sink; f a frame we own.
    let (s, fr) = unsafe { (&mut *o.cast::<Sink>(), &mut *f) };
    // SAFETY: data holds size bytes.
    let bytes = unsafe { std::slice::from_raw_parts(fr.data, usize::try_from(fr.size).unwrap()) }.to_vec();
    s.out.push((bytes, fr.time, fr.dur, fr.flags));
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}

/// A caption block with `n` entries (marker byte, two data bytes).
fn cc_block(count: u8, entries: &[[u8; 3]]) -> Vec<u8> {
    let mut b = vec![0x00, 0x00, 0x01, 0xB2, b'C', b'C', 0x01, 0xF8, count];
    for e in entries {
        b.extend_from_slice(e);
    }
    b
}

/// The stream with `insert(gop)` after each GOP header.
fn with_user_data(insert: impl Fn(usize) -> Vec<u8>) -> Vec<u8> {
    let mut out = Vec::new();
    let (mut i, mut gop) = (0, 0);
    while i < M2V.len() {
        if i + 4 <= M2V.len() && M2V[i..i + 4] == [0, 0, 1, 0xB8] {
            out.extend_from_slice(&M2V[i..i + 8]);
            out.extend(insert(gop));
            gop += 1;
            i += 8;
        } else {
            out.push(M2V[i]);
            i += 1;
        }
    }
    out
}

/// The stream through the cutter and the video timing (every picture on
/// its grid); video frames and side units.
fn run(m2v: &[u8], side: bool) -> (Sink, Sink, VideoStats) {
    let id = codec_id(&CString::new("mpeg2video").unwrap()).unwrap();
    let (mut video, mut sides) = (Sink::default(), Sink::default());
    let mut v: *mut Video = std::ptr::null_mut();
    let mut c: *mut Cutter = std::ptr::null_mut();
    let mut st = VideoStats::default();
    // SAFETY: both sinks outlive the stages; both are closed below.
    unsafe {
        let null = std::ptr::null_mut();
        assert_eq!(ff_discrip_video_open(&raw mut v, null, id, 0, on_frame, (&raw mut video).cast(), None, null), 0);
        if side {
            ff_discrip_video_set_side(v, Some(on_frame), (&raw mut sides).cast());
        }
        assert_eq!(ff_discrip_cutter_open(&raw mut c, null, id, ff_discrip_video_unit, v.cast()), 0);
        for &(a, b, disp, _) in &pictures(m2v) {
            let len = c_int::try_from(b - a).unwrap();
            assert_eq!(ff_discrip_cutter_write(c, m2v[a..b].as_ptr(), len, BASE + disp * FRAME), 0);
        }
        assert_eq!(ff_discrip_cutter_flush(c), 0);
        assert_eq!(ff_discrip_video_flush(v), 0);
        ff_discrip_video_stats(v, &raw mut st);
        ff_discrip_cutter_close(&raw mut c);
        ff_discrip_video_close(&raw mut v);
    }
    (video, sides, st)
}

#[test]
fn gop_user_data_leaves_the_video_with_the_next_pictures_time() {
    // a caption block after every GOP header, other user data after every
    // 10th one as well
    let block = |g: usize| cc_block(0x82, &[[0xFF, u8::try_from(g % 256).unwrap(), 0x80], [0xFE, 0x80, 0x80]]);
    let other = |g: usize| if g % 10 == 0 { vec![0x00, 0x00, 0x01, 0xB2, b'X', b'Y', 0x42] } else { Vec::new() };
    let stream = with_user_data(|g| [block(g), other(g)].concat());
    let (video, sides, st) = run(&stream, true);

    // the video is the stream without the user data, picture by picture
    let bytes: Vec<u8> = video.out.iter().flat_map(|o| o.0.clone()).collect();
    assert_eq!(bytes, M2V);
    assert_eq!(st.out, i64::try_from(pictures(M2V).len()).unwrap());

    // each piece, in stream order, at the time of the next picture after
    // its I picture when that one is in the I picture's batch (shown before
    // it: the first B picture of an open GOP, the GOP's first picture on
    // screen), else at the I picture's own time (a closed GOP: the I picture
    // is a batch of its own and the first on screen); one field long
    let pics = pictures(&stream);
    let gop_at: Vec<usize> = (0..stream.len() - 3).filter(|&i| stream[i..i + 4] == [0, 0, 1, 0xB8]).collect();
    let mut want = Vec::new();
    for (g, &at) in gop_at.iter().enumerate() {
        let k = pics.iter().position(|p| p.0 <= at && at < p.1).unwrap();
        let shown = if pics[k + 1].2 < pics[k].2 { pics[k + 1].2 } else { pics[k].2 };
        let t = BASE + shown * FRAME;
        want.push((block(g), t));
        if g % 10 == 0 {
            want.push((other(g), t));
        }
    }
    let got: Vec<(Vec<u8>, i64)> = sides.out.iter().map(|o| (o.0.clone(), o.1)).collect();
    assert_eq!(got, want);
    assert!(sides.out.iter().all(|o| o.2 == FRAME / 2 && o.3 == F_KEY));
    assert_eq!(st.side, i64::try_from(want.len()).unwrap());
}

#[test]
fn without_a_side_callback_the_user_data_is_still_taken_out() {
    let stream = with_user_data(|_| cc_block(0x80, &[]));
    let (video, _, st) = run(&stream, false);
    let bytes: Vec<u8> = video.out.iter().flat_map(|o| o.0.clone()).collect();
    assert_eq!(bytes, M2V);
    assert_eq!(st.side, 101);
}

fn check(b: &[u8]) -> c_int {
    // SAFETY: b is a valid slice.
    unsafe { ff_discrip_cc_check(b.as_ptr(), c_int::try_from(b.len()).unwrap()) }
}

#[test]
fn a_caption_block_is_its_header_and_its_entries() {
    let b = cc_block(0x03, &[[0xFF, 1, 2], [0xFE, 3, 4], [0xFF, 5, 6]]);
    assert_eq!(check(&b), 18);
    let mut longer = b.clone();
    longer.extend_from_slice(&[0, 0, 0]);
    assert_eq!(check(&longer), 18, "bytes after the entries are not part of it");
    assert_eq!(check(&b[..17]), 0, "shorter than its count");
    let mut bit6 = b.clone();
    bit6[8] |= 0x40;
    assert_eq!(check(&bit6), 0);
    let mut other = b.clone();
    other[4] = b'X';
    assert_eq!(check(&other), 0, "other user data");
    assert_eq!(check(&b[..8]), 0);
}

fn triplets(b: &[u8]) -> Vec<[u8; 3]> {
    let mut out = [0u8; 189];
    // SAFETY: b is a valid slice; out has room for 63 entries.
    let n = unsafe { ff_discrip_cc_triplets(b.as_ptr(), c_int::try_from(b.len()).unwrap(), out.as_mut_ptr()) };
    out[..usize::try_from(n).unwrap() * 3].chunks(3).map(|c| [c[0], c[1], c[2]]).collect()
}

#[test]
fn entries_get_field_markers_from_their_pattern() {
    // odd field first (bit 7): FF FE -> field 1, field 2
    let b = cc_block(0x82, &[[0xFF, 0x94, 0x2C], [0xFE, 0x80, 0x80]]);
    assert_eq!(triplets(&b), vec![[4, 0x94, 0x2C], [5, 0x80, 0x80]]);
    // odd field first, FF FF: field 1, field 2 (both marked odd)
    assert_eq!(triplets(&cc_block(0x82, &[[0xFF, 1, 2], [0xFF, 3, 4]])), vec![[4, 1, 2], [5, 3, 4]]);
    // odd field first, FE FF: field 2, field 1
    assert_eq!(triplets(&cc_block(0x82, &[[0xFE, 1, 2], [0xFF, 3, 4]])), vec![[5, 1, 2], [4, 3, 4]]);
    // even field first: FF FE -> field 1, field 2; FE FE / FE FF / FF FF -> field 2, field 1
    assert_eq!(triplets(&cc_block(0x02, &[[0xFF, 1, 2], [0xFE, 3, 4]])), vec![[4, 1, 2], [5, 3, 4]]);
    for pat in [[0xFE, 0xFE], [0xFE, 0xFF], [0xFF, 0xFF]] {
        assert_eq!(triplets(&cc_block(0x02, &[[pat[0], 1, 2], [pat[1], 3, 4]])), vec![[5, 1, 2], [4, 3, 4]]);
    }
}

#[test]
fn an_unknown_marker_ends_the_block() {
    let b = cc_block(0x85, &[[0xFF, 1, 2], [0xFE, 3, 4], [0x7F, 5, 6], [0xFE, 7, 8], [0xFF, 9, 10]]);
    assert_eq!(triplets(&b), vec![[4, 1, 2], [5, 3, 4]], "the odd last entry is not reached either");
}

#[test]
fn a_last_single_entry() {
    // odd field first: FF -> field 1, FE -> field 2; even field first: field 2
    assert_eq!(triplets(&cc_block(0x81, &[[0xFF, 1, 2]])), vec![[4, 1, 2]]);
    assert_eq!(triplets(&cc_block(0x81, &[[0xFE, 1, 2]])), vec![[5, 1, 2]]);
    assert_eq!(triplets(&cc_block(0x01, &[[0xFF, 1, 2]])), vec![[5, 1, 2]]);
    let b = cc_block(0x83, &[[0xFF, 1, 2], [0xFE, 3, 4], [0xFF, 5, 6]]);
    assert_eq!(triplets(&b), vec![[4, 1, 2], [5, 3, 4], [4, 5, 6]]);
}
