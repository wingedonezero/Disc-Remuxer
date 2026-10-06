//! `libavformat/discrip_video.c` with the MPEG-2 rules of `discrip_mpv.c`:
//! pictures timed on a fixed grid (base + display position x field duration).
//! The PES times fed in are computed here from the bitstream (each GOP's
//! display start + `temporal_reference`), independently of the code. Stream
//! made by FFmpeg's MPEG-2 encoder (tests/data/README.md).

mod common;

use std::ffi::CString;
use std::os::raw::{c_int, c_void};

use common::discrip::pictures;
use ffmpeg_sys::discrip::{
    codec_id, ff_discrip_cutter_close, ff_discrip_cutter_flush, ff_discrip_cutter_open, ff_discrip_cutter_write,
    ff_discrip_frame_unref, ff_discrip_video_close, ff_discrip_video_flush, ff_discrip_video_open,
    ff_discrip_video_stats, ff_discrip_video_unit, Cutter, Event, Frame, Video, VideoStats, EV_VIDEO_INVALID,
    EV_VIDEO_RATE_CHANGE, EV_VIDEO_REPAIR, EV_VIDEO_TIMECODE, F_BATCH, F_DISCARD, F_KEY, F_MARKER,
};

const M2V: &[u8] = include_bytes!("data/testsrc_1200.m2v");
const FRAME: i64 = 36_036_000; // 1001/30000 s
const BASE: i64 = 10 * 1_080_000_000; // the first displayed picture at 10 s

struct Sink {
    out: Vec<(i64, i64, u32)>, // time, dur, flags
    events: Vec<Event>,
}

unsafe extern "C" fn on_frame(o: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: o is the &mut Sink; f a frame we own.
    let (s, fr) = unsafe { (&mut *o.cast::<Sink>(), &mut *f) };
    s.out.push((fr.time, fr.dur, fr.flags));
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}
unsafe extern "C" fn on_event(o: *mut c_void, e: *const Event) {
    // SAFETY: o is the &mut Sink; e valid during the call.
    unsafe { (*o.cast::<Sink>()).events.push(*e) };
}

/// The stream through the cutter and the video timing, each picture's
/// payload with the PES time `time(display index, decode index)`.
fn run(time: impl Fn(i64, usize) -> Option<i64>) -> (Sink, VideoStats) {
    run_on(M2V, time)
}

fn run_on(m2v: &[u8], time: impl Fn(i64, usize) -> Option<i64>) -> (Sink, VideoStats) {
    let id = codec_id(&CString::new("mpeg2video").unwrap()).unwrap();
    let mut sink = Sink { out: Vec::new(), events: Vec::new() };
    let mut v: *mut Video = std::ptr::null_mut();
    let mut c: *mut Cutter = std::ptr::null_mut();
    let mut st = VideoStats::default();
    // SAFETY: sink outlives both stages; both are closed below.
    unsafe {
        let sp = (&raw mut sink).cast();
        assert_eq!(ff_discrip_video_open(&raw mut v, std::ptr::null_mut(), id, 0, on_frame, sp, Some(on_event), sp), 0);
        assert_eq!(ff_discrip_cutter_open(&raw mut c, std::ptr::null_mut(), id, ff_discrip_video_unit, v.cast()), 0);
        for (k, &(a, b, disp, _)) in pictures(m2v).iter().enumerate() {
            let t = time(disp, k).unwrap_or(ffmpeg_sys::AV_NOPTS_VALUE);
            assert_eq!(ff_discrip_cutter_write(c, m2v[a..b].as_ptr(), c_int::try_from(b - a).unwrap(), t), 0);
        }
        assert_eq!(ff_discrip_cutter_flush(c), 0);
        assert_eq!(ff_discrip_video_flush(v), 0);
        ff_discrip_video_stats(v, &raw mut st);
        ff_discrip_cutter_close(&raw mut c);
        ff_discrip_video_close(&raw mut v);
    }
    (sink, st)
}

fn pes(disp: i64) -> i64 {
    BASE + disp * FRAME
}

#[test]
fn pictures_on_the_grid_keep_their_times_in_decode_order() {
    let pics = pictures(M2V);
    let (s, st) = run(|d, _| Some(pes(d)));
    assert_eq!((st.num, st.den), (30000, 1001));
    assert_eq!(st.base, BASE);
    assert_eq!(s.out.len(), pics.len());
    for (o, p) in s.out.iter().zip(&pics) {
        assert_eq!(o.0, pes(p.2), "grid time = PES time");
        assert_eq!(o.1, FRAME, "two fields");
        assert_eq!(o.2 & F_KEY != 0, p.3 == 1, "I pictures are key frames");
        assert_eq!(o.2 & F_DISCARD != 0, p.3 == 3, "B pictures are discardable");
    }
    assert!(s.events.is_empty(), "{:?}", s.events);
    assert!(s.out.iter().any(|o| o.2 & F_BATCH != 0));
    assert_eq!(s.out.last().unwrap().2 & F_BATCH, F_BATCH, "the last picture ends a batch");
}

#[test]
fn only_some_pictures_with_pes_times_still_give_the_grid() {
    // PES times on every 5th picture only (as when PES packets span pictures)
    let pics = pictures(M2V);
    let (s, st) = run(|d, k| (k % 5 == 0).then(|| pes(d)));
    assert_eq!(st.base, BASE);
    for (o, p) in s.out.iter().zip(&pics) {
        assert_eq!(o.0, pes(p.2));
    }
}

#[test]
fn a_picture_off_the_grid_is_reported_and_keeps_its_grid_time() {
    // like the 300 Combo bonus titles: one picture's PES time 2 frames late
    let pics = pictures(M2V);
    let bad = 67;
    let (s, st) = run(|d, k| Some(pes(d) + if k == bad { 2 * FRAME } else { 0 }));
    let tc: Vec<&Event> = s.events.iter().filter(|e| e.kind == EV_VIDEO_TIMECODE).collect();
    assert_eq!(tc.len(), 1);
    assert_eq!((tc[0].pos, tc[0].dur), (pes(pics[bad].2), 2 * FRAME), "+66.733 ms");
    assert_eq!(s.out[bad].0, pes(pics[bad].2), "the grid time is kept");
    let inv: Vec<&Event> = s.events.iter().filter(|e| e.kind == EV_VIDEO_INVALID).collect();
    assert_eq!(inv.len(), 1);
    assert_eq!(inv[0].count, 1);
    assert_eq!(st.invalid, 1);
}

#[test]
fn a_forward_jump_of_the_pes_times_moves_the_grid_with_placeholders() {
    // from the first I picture at decode index 600 or later, the PES times are
    // 60 frames (about 2 s) later
    let pics = pictures(M2V);
    let jump = 60 * FRAME;
    let cut = (600..pics.len()).find(|&k| pics[k].3 == 1).unwrap();
    let (s, st) = run(|d, k| Some(pes(d) + if k >= cut { jump } else { 0 }));
    assert!(s.events.iter().any(|e| e.kind == EV_VIDEO_REPAIR), "{:?}", s.events);
    assert!(st.placeholders > 0);
    let real: Vec<&(i64, i64, u32)> = s.out.iter().filter(|o| o.2 & F_MARKER == 0).collect();
    assert_eq!(real.len(), pics.len());
    // after the jump the pictures are on the new grid again
    let last = pics.len() - 1;
    assert_eq!(real[last].0, pes(pics[last].2) + jump);
    assert_eq!(real[cut + 30].0, pes(pics[cut + 30].2) + jump);
    // the I picture at the jump keeps the old grid (+3/16 s) and so do the B
    // pictures of its (open) GOP shown before it: those, and only those, are
    // off the grid
    let leading_b = pics[cut + 1..].iter().take_while(|p| p.3 == 3).count();
    assert_eq!(st.invalid, i64::try_from(1 + leading_b).unwrap());
    let lo = 11 * 18_018_000; // 3/16 s of fields at 29.97 fps
    let tc: Vec<i64> = s.events.iter().filter(|e| e.kind == EV_VIDEO_TIMECODE).map(|e| e.dur).collect();
    assert_eq!(tc, vec![jump - lo, jump], "the I picture, then its leading B pictures");
}

#[test]
fn a_jump_of_a_fraction_of_a_field_leaves_the_rest_off_the_grid_by_that_fraction() {
    // 2 s = 119.88 fields: the grid moves 120 fields, the pictures after the
    // jump are 2.16 ms early against it (one difference, many pictures)
    let pics = pictures(M2V);
    let jump = 2 * 1_080_000_000;
    let cut = (600..pics.len()).find(|&k| pics[k].3 == 1).unwrap();
    let (s, st) = run(|d, k| Some(pes(d) + if k >= cut { jump } else { 0 }));
    let tc: Vec<&Event> = s.events.iter().filter(|e| e.kind == EV_VIDEO_TIMECODE).collect();
    assert!(!tc.is_empty());
    assert!(tc.iter().all(|e| e.dur == -2_160_000 || e.dur.abs() > FRAME), "{tc:?}");
    assert!(st.invalid > 500);
}

#[test]
fn a_later_header_with_another_frame_rate_is_a_warning_and_the_first_rate_stays() {
    // every sequence header after the 50th says 25 fps (frame_rate_code 3)
    let mut m = M2V.to_vec();
    let heads: Vec<usize> = (0..m.len() - 8).filter(|&i| m[i..i + 4] == [0, 0, 1, 0xB3]).collect();
    for &i in &heads[50..] {
        m[i + 7] = (m[i + 7] & 0xF0) | 3;
    }
    let pics = pictures(&m);
    let (s, st) = run_on(&m, |d, _| Some(pes(d)));
    assert_eq!((st.num, st.den), (30000, 1001));
    let rc: Vec<&Event> = s.events.iter().filter(|e| e.kind == EV_VIDEO_RATE_CHANGE).collect();
    assert_eq!(rc.len(), 1, "one warning for the new rate");
    assert_eq!((rc[0].count, rc[0].dur), (25, 1));
    for (o, p) in s.out.iter().zip(&pics) {
        assert_eq!(o.0, pes(p.2), "still timed at 29.97");
    }
}
