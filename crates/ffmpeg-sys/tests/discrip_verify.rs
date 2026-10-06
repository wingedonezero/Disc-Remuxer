//! `libavformat/discrip_verify.c`: the checks of a track's output. Units are
//! parsed again (AC-3 CRC, `TrueHD` length and major-sync checksum, MPEG-2
//! start codes); times are checked (audio in order, video without holes in
//! display order); the audio sync error is measured as a stream file plays
//! the frames (back to back from the start delay) and by the output times;
//! `TrueHD` input timing breaks are counted. Units from FFmpeg's encoders
//! (tests/data/README.md).

mod common;

use std::ffi::CString;
use std::os::raw::{c_int, c_void};

use common::discrip::pictures;
use ffmpeg_sys::discrip::{
    codec_id, ff_discrip_verify_close, ff_discrip_verify_finish, ff_discrip_verify_frame, ff_discrip_verify_open,
    ff_discrip_verify_stats, Event, Frame, Verify, VerifyStats, EV_VERIFY_CRC, EV_VERIFY_HOLE, EV_VERIFY_ORDER,
    EV_VERIFY_OVERLAP, EV_VERIFY_THD_TIMING, EV_VERIFY_UNIT, F_KEY, F_MARKER, KIND_AUDIO, KIND_VIDEO,
};

const AC3: &[u8] = include_bytes!("data/sine_7680.ac3");
const THD: &[u8] = include_bytes!("data/sine_4813.thd");
const M2V: &[u8] = include_bytes!("data/testsrc_1200.m2v");
const MS: i64 = 1_080_000;
const AC3_DUR: i64 = 32 * MS;

/// One output frame: bytes, output time, duration, own (source) time.
struct In<'a> {
    data: &'a [u8],
    time: i64,
    dur: i64,
    src: i64,
    flags: u32,
}

unsafe extern "C" fn on_event(o: *mut c_void, e: *const Event) {
    // SAFETY: o is the &mut Vec<Event>; e valid during the call.
    unsafe { (*o.cast::<Vec<Event>>()).push(*e) };
}

fn check(codec: &str, kind: c_int, frames: &[In]) -> (VerifyStats, Vec<Event>) {
    let id = codec_id(&CString::new(codec).unwrap()).unwrap();
    let mut events: Vec<Event> = Vec::new();
    let mut v: *mut Verify = std::ptr::null_mut();
    let mut st = VerifyStats::default();
    // SAFETY: events outlives the checker; frames point into live slices during each call.
    unsafe {
        let ep = (&raw mut events).cast();
        assert_eq!(ff_discrip_verify_open(&raw mut v, std::ptr::null_mut(), id, 1, kind, Some(on_event), ep), 0);
        for f in frames {
            let fr = Frame {
                buf: std::ptr::null_mut(),
                data: f.data.as_ptr().cast_mut(),
                size: c_int::try_from(f.data.len()).unwrap(),
                time: f.time,
                dur: f.dur,
                pos: 0,
                flags: f.flags,
                samples: 0,
                rate: 0,
                src: f.src,
            };
            assert_eq!(ff_discrip_verify_frame(v, &raw const fr), 0);
        }
        assert_eq!(ff_discrip_verify_finish(v), 0);
        ff_discrip_verify_stats(v, &raw mut st);
        ff_discrip_verify_close(&raw mut v);
    }
    (st, events)
}

fn ac3_frames() -> Vec<&'static [u8]> {
    AC3.chunks(768).collect()
}

fn kinds(e: &[Event]) -> Vec<c_int> {
    e.iter().map(|e| e.kind).collect()
}

#[test]
fn good_ac3_frames_pass() {
    let f: Vec<In> = ac3_frames()
        .into_iter()
        .enumerate()
        .map(|(k, d)| {
            let t = i64::try_from(k).unwrap() * AC3_DUR;
            In { data: d, time: t + 5 * MS, dur: AC3_DUR, src: t + 5 * MS, flags: F_KEY }
        })
        .collect();
    let (st, ev) = check("ac3", KIND_AUDIO, &f);
    assert!(ev.is_empty(), "{ev:?}");
    assert_eq!((st.frames, st.bad_units, st.crc_errors, st.unchecked), (5, 0, 0, 0));
    assert_eq!(st.delay, 5 * MS);
    assert_eq!((st.es_err_max, st.es_err_end, st.mkv_err_max), (0, 0, 0));
}

#[test]
fn a_damaged_or_cut_ac3_frame_is_found() {
    let mut bad = ac3_frames()[1].to_vec();
    bad[300] ^= 0x10;
    let cut = &ac3_frames()[2][..700];
    let f = [
        In { data: ac3_frames()[0], time: 0, dur: AC3_DUR, src: 0, flags: F_KEY },
        In { data: &bad, time: AC3_DUR, dur: AC3_DUR, src: AC3_DUR, flags: F_KEY },
        In { data: cut, time: 2 * AC3_DUR, dur: AC3_DUR, src: 2 * AC3_DUR, flags: F_KEY },
    ];
    let (st, ev) = check("ac3", KIND_AUDIO, &f);
    assert_eq!((st.crc_errors, st.bad_units), (1, 1));
    assert_eq!(kinds(&ev), vec![EV_VERIFY_CRC, EV_VERIFY_UNIT]);
}

#[test]
fn the_sync_error_of_a_stream_file_and_of_the_output_times() {
    // frame 2 dropped (the junction cut a skew): later frames play 32 ms
    // early in a stream file, their output times are their own; then a
    // real 100 ms gap the stream file cannot hold: 132 ms early from there
    let fr = ac3_frames();
    let src = [0, AC3_DUR, 3 * AC3_DUR, 4 * AC3_DUR + 100 * MS];
    let f: Vec<In> =
        src.iter().enumerate().map(|(k, &s)| In { data: fr[k], time: s, dur: AC3_DUR, src: s, flags: F_KEY }).collect();
    let (st, _) = check("ac3", KIND_AUDIO, &f);
    assert_eq!((st.es_err_max, st.es_err_at), (-132 * MS, 4 * AC3_DUR + 100 * MS));
    assert_eq!(st.es_err_end, -132 * MS);
    assert_eq!(st.mkv_err_max, 0);
    // output times shifted against the own times (a junction skew) show up
    // in the output-time error
    let g: Vec<In> = (0..3i64)
        .map(|k| {
            let s = k * AC3_DUR;
            In { data: fr[usize::try_from(k).unwrap()], time: s + 3 * MS, dur: AC3_DUR, src: s, flags: F_KEY }
        })
        .collect();
    let (st, _) = check("ac3", KIND_AUDIO, &g);
    assert_eq!((st.mkv_err_max, st.es_err_max, st.delay), (3 * MS, 3 * MS, 3 * MS));
}

#[test]
fn audio_out_of_order_or_without_duration() {
    let fr = ac3_frames();
    let f = [
        In { data: fr[0], time: AC3_DUR, dur: AC3_DUR, src: AC3_DUR, flags: F_KEY },
        In { data: fr[1], time: AC3_DUR, dur: AC3_DUR, src: AC3_DUR, flags: F_KEY },
        In { data: fr[2], time: 2 * AC3_DUR, dur: 0, src: 2 * AC3_DUR, flags: F_KEY },
    ];
    let (st, ev) = check("ac3", KIND_AUDIO, &f);
    assert_eq!(st.order_errors, 2);
    assert_eq!(kinds(&ev), vec![EV_VERIFY_ORDER, EV_VERIFY_ORDER]);
}

/// The `TrueHD` access units of the test stream.
fn thd_units() -> Vec<&'static [u8]> {
    let mut v = Vec::new();
    let mut o = 0;
    while o + 4 <= THD.len() {
        let len = ((usize::from(THD[o] & 0xF) << 8) | usize::from(THD[o + 1])) * 2;
        v.push(&THD[o..o + len]);
        o += len;
    }
    v
}

fn thd_frames<'a>(units: &[&'a [u8]], first: i64) -> Vec<In<'a>> {
    let au = 40 * 1_080_000_000 / 48_000; // 40 samples at 48 kHz
    units
        .iter()
        .enumerate()
        .map(|(k, &d)| {
            let t = first + i64::try_from(k).unwrap() * au;
            In { data: d, time: t, dur: au, src: t, flags: F_KEY }
        })
        .collect()
}

#[test]
fn truehd_input_timing_and_checksums() {
    let units = thd_units();
    assert_eq!(units.len(), 121);
    let (st, ev) = check("truehd", KIND_AUDIO, &thd_frames(&units, 0));
    assert!(ev.is_empty(), "{ev:?}");
    assert_eq!((st.bad_units, st.crc_errors, st.thd_breaks), (0, 0, 0));

    // the stream twice in a row (as at a join): the second one's input
    // timing starts again, one break
    let mut both = units[..120].to_vec();
    both.extend_from_slice(&units[..10]);
    let (st, ev) = check("truehd", KIND_AUDIO, &thd_frames(&both, 0));
    assert_eq!(st.thd_breaks, 1);
    assert_eq!(kinds(&ev), vec![EV_VERIFY_THD_TIMING]);

    // a damaged major sync
    let mut bad = units[0].to_vec();
    bad[12] ^= 0x01;
    let mut f = thd_frames(&units[..3], 0);
    f[0].data = &bad;
    let (st, _) = check("truehd", KIND_AUDIO, &f);
    assert_eq!(st.crc_errors, 1);
}

/// The MPEG-2 test stream's pictures as output frames on a 29.97 fps grid
/// (display order from the bitstream), except those in `skip`.
fn m2v_frames(skip: &[usize]) -> Vec<In<'static>> {
    let frame = 36_036_000;
    pictures(M2V)
        .iter()
        .enumerate()
        .filter(|(k, _)| !skip.contains(k))
        .map(|(_, &(a, b, disp, _))| In { data: &M2V[a..b], time: disp * frame, dur: frame, src: disp * frame, flags: 0 })
        .collect()
}

#[test]
fn video_in_display_order_without_holes() {
    let (st, ev) = check("mpeg2video", KIND_VIDEO, &m2v_frames(&[]));
    assert!(ev.is_empty(), "{ev:?}");
    assert_eq!((st.bad_units, st.holes, st.overlaps), (0, 0, 0));
}

#[test]
fn a_missing_picture_is_a_hole_and_placeholders_are_counted_in_it() {
    let frame = 36_036_000;
    let mut f = m2v_frames(&[100, 101]);
    let disp: Vec<i64> = pictures(M2V).iter().map(|p| p.2).collect();
    let (a, b) = (disp[100].min(disp[101]), disp[100].max(disp[101]));
    let ph = In { data: &[], time: a * frame, dur: 0, src: a * frame, flags: F_KEY | F_MARKER };
    f.insert(50, ph);
    let (st, ev) = check("mpeg2video", KIND_VIDEO, &f);
    let holes: Vec<&Event> = ev.iter().filter(|e| e.kind == EV_VERIFY_HOLE).collect();
    if b == a + 1 {
        assert_eq!(holes.len(), 1);
        assert_eq!((holes[0].pos, holes[0].dur, holes[0].count), (a * frame, 2 * frame, 1));
    } else {
        assert_eq!(holes.len(), 2);
    }
    assert_eq!(st.holes, i64::try_from(holes.len()).unwrap());
    assert_eq!(st.markers, 1);
}

#[test]
fn video_overlaps_and_bad_units() {
    let frame = 36_036_000;
    let mut f = m2v_frames(&[]);
    f[10].dur = frame + frame / 2; // runs half a frame into the next one shown
    let junk = [0xAAu8; 16];
    f[20].data = &junk;
    let (st, ev) = check("mpeg2video", KIND_VIDEO, &f);
    assert_eq!((st.overlaps, st.overlap_dur, st.bad_units), (1, frame / 2, 1));
    assert!(kinds(&ev).contains(&EV_VERIFY_OVERLAP));
    assert!(kinds(&ev).contains(&EV_VERIFY_UNIT));
}

#[test]
fn a_codec_without_a_unit_check_is_counted() {
    let d = [0u8; 64];
    let f = [In { data: &d, time: 0, dur: MS, src: 0, flags: F_KEY }];
    let (st, _) = check("pcm_dvd", KIND_AUDIO, &f);
    assert_eq!(st.unchecked, 1);
}
