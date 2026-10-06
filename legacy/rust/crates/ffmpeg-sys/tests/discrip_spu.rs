//! `libavformat/discrip_spu.c`: sub-picture units (DVD-Video, and the HD DVD
//! form with 32-bit sizes) cut from the byte stream, each lasting until its
//! stop command (`STP_DSP` delay x 1024 / 90000 s; one step when it has none),
//! timed at the video grid point nearest to its PES time. The video grid
//! comes from the MPEG-2 test stream (tests/data/README.md) with every
//! picture on its grid; the units are built byte by byte.

mod common;

use std::ffi::CString;
use std::os::raw::{c_int, c_void};

use common::discrip::pictures;
use ffmpeg_sys::discrip::{
    codec_id, ff_discrip_cutter_close, ff_discrip_cutter_flush, ff_discrip_cutter_open, ff_discrip_cutter_stats,
    ff_discrip_cutter_write, ff_discrip_frame_unref, ff_discrip_spu_close, ff_discrip_spu_flush, ff_discrip_spu_open,
    ff_discrip_spu_stats, ff_discrip_spu_unit, ff_discrip_video_close, ff_discrip_video_flush, ff_discrip_video_open,
    ff_discrip_video_snap, ff_discrip_video_unit, Cutter, CutterStats, Event, Frame, Spu, SpuStats, Video,
    EV_SUB_EARLY, EV_SUB_UNTIMED, F_KEY, F_NO_STOP,
};
use ffmpeg_sys::AV_NOPTS_VALUE;

const M2V: &[u8] = include_bytes!("data/testsrc_1200.m2v");
const FRAME: i64 = 36_036_000; // 1001/30000 s
const FIELD: i64 = FRAME / 2;
const BASE: i64 = 10 * 1_080_000_000; // the first displayed picture at 10 s
const STEP: i64 = 12_288_000; // one SP_DCSQ_STM step: 1024 / 90000 s
const MS: i64 = 1_080_000;

// ---- units ----

/// A DVD-Video sub-picture unit: `SPDSZ`, `SP_DCSQTA`, 4 bytes of pixel data,
/// `SP_DCSQ` 1 (delay 0: `STA_DSP`, `cmds`, `SET_DSPXA` when `dspxa`), `SP_DCSQ` 2
/// (delay `stop`: `STP_DSP`) when there is a stop. `mark` makes units differ.
fn spu(stop: Option<u16>, dspxa: bool, cmds: &[u8], mark: u8) -> Vec<u8> {
    let mut u = vec![0, 0, 0, 0, mark, 0x22, 0x33, 0x44];
    let d1 = u.len();
    let mut seq = vec![0x01];
    seq.extend_from_slice(cmds);
    if dspxa {
        seq.extend_from_slice(&[0x06, 0, 4, 0, 6]);
    }
    seq.push(0xFF);
    let d2 = d1 + 4 + seq.len();
    u.extend_from_slice(&[0, 0]);
    u.extend_from_slice(&u16::try_from(if stop.is_some() { d2 } else { d1 }).unwrap().to_be_bytes());
    u.extend_from_slice(&seq);
    if let Some(s) = stop {
        u.extend_from_slice(&s.to_be_bytes());
        u.extend_from_slice(&u16::try_from(d2).unwrap().to_be_bytes());
        u.extend_from_slice(&[0x02, 0xFF]);
    }
    let n = u16::try_from(u.len()).unwrap();
    u[0..2].copy_from_slice(&n.to_be_bytes());
    u[2..4].copy_from_slice(&u16::try_from(d1).unwrap().to_be_bytes());
    u
}

/// An HD DVD sub-picture unit: a zero word, 32-bit size and `SP_DCSQTA`, 4
/// bytes of pixel data, `SP_DCSQ` 1 with the 8-bit commands (`SET_COLOR` 0x83,
/// `SET_CONTR` 0x84, `SET_DAREA` 0x85, `SET_DSPXA` 0x86), `SP_DCSQ` 2 with `STP_DSP`.
fn spu_hd(stop: u16) -> Vec<u8> {
    let mut u = vec![0u8; 10];
    u.extend_from_slice(&[0x11, 0x22, 0x33, 0x44]);
    let d1 = u.len();
    let mut seq = vec![0x01, 0x83];
    seq.extend(std::iter::repeat_n(0x10, 768));
    seq.push(0x84);
    seq.extend(std::iter::repeat_n(0x0F, 256));
    seq.extend_from_slice(&[0x85, 0, 0, 0, 0, 0, 0]);
    seq.extend_from_slice(&[0x86, 0, 0, 0, 10, 0, 0, 0, 12, 0xFF]);
    let d2 = d1 + 6 + seq.len();
    u.extend_from_slice(&[0, 0]);
    u.extend_from_slice(&u32::try_from(d2).unwrap().to_be_bytes());
    u.extend_from_slice(&seq);
    u.extend_from_slice(&stop.to_be_bytes());
    u.extend_from_slice(&u32::try_from(d2).unwrap().to_be_bytes());
    u.extend_from_slice(&[0x02, 0xFF]);
    let n = u32::try_from(u.len()).unwrap();
    u[2..6].copy_from_slice(&n.to_be_bytes());
    u[6..10].copy_from_slice(&u32::try_from(d1).unwrap().to_be_bytes());
    u
}

// ---- the run ----

#[derive(Default)]
struct Sink {
    out: Vec<(Vec<u8>, i64, i64, u32)>, // bytes, time, dur, flags
    events: Vec<Event>,
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
unsafe extern "C" fn on_event(o: *mut c_void, e: *const Event) {
    // SAFETY: o is the &mut Sink; e valid during the call.
    unsafe { (*o.cast::<Sink>()).events.push(*e) };
}
unsafe extern "C" fn drop_frame(_: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}

struct Run {
    sink: Sink,
    st: SpuStats,
    cut: CutterStats,
    snap_before_video: c_int,
}

/// The video (every picture on its grid from BASE) and the sub-picture
/// payloads (bytes, time) of one segment; the payloads go in before the
/// video when `subs_first`.
fn run(payloads: &[(Vec<u8>, Option<i64>)], subs_first: bool) -> Run {
    let vid = codec_id(&CString::new("mpeg2video").unwrap()).unwrap();
    let sid = codec_id(&CString::new("dvd_subtitle").unwrap()).unwrap();
    let mut sink = Sink::default();
    let mut v: *mut Video = std::ptr::null_mut();
    let mut vc: *mut Cutter = std::ptr::null_mut();
    let mut s: *mut Spu = std::ptr::null_mut();
    let mut sc: *mut Cutter = std::ptr::null_mut();
    let mut st = SpuStats::default();
    let mut cut = CutterStats::default();
    let mut t = 0i64;
    // SAFETY: sink outlives the stages; all are closed below.
    let snap_before_video = unsafe {
        let sp = (&raw mut sink).cast();
        let null = std::ptr::null_mut();
        assert_eq!(ff_discrip_video_open(&raw mut v, null, vid, 0, drop_frame, null, None, null), 0);
        assert_eq!(ff_discrip_cutter_open(&raw mut vc, null, vid, ff_discrip_video_unit, v.cast()), 0);
        assert_eq!(ff_discrip_spu_open(&raw mut s, null, 1, v, on_frame, sp, Some(on_event), sp), 0);
        assert_eq!(ff_discrip_cutter_open(&raw mut sc, null, sid, ff_discrip_spu_unit, s.cast()), 0);
        let snap = ff_discrip_video_snap(v, BASE, &raw mut t);
        let subs = |sc: *mut Cutter| {
            for (data, time) in payloads {
                let len = c_int::try_from(data.len()).unwrap();
                assert_eq!(ff_discrip_cutter_write(sc, data.as_ptr(), len, time.unwrap_or(AV_NOPTS_VALUE)), 0);
            }
        };
        if subs_first {
            subs(sc);
            assert!(sink.out.is_empty(), "units wait for the video's grid");
        }
        for &(a, b, disp, _) in &pictures(M2V) {
            let len = c_int::try_from(b - a).unwrap();
            assert_eq!(ff_discrip_cutter_write(vc, M2V[a..b].as_ptr(), len, BASE + disp * FRAME), 0);
        }
        if !subs_first {
            subs(sc);
        }
        assert_eq!(ff_discrip_cutter_flush(vc), 0);
        assert_eq!(ff_discrip_video_flush(v), 0);
        assert_eq!(ff_discrip_cutter_flush(sc), 0);
        assert_eq!(ff_discrip_spu_flush(s), 0);
        ff_discrip_spu_stats(s, &raw mut st);
        ff_discrip_cutter_stats(sc, &raw mut cut);
        ff_discrip_cutter_close(&raw mut sc);
        ff_discrip_spu_close(&raw mut s);
        ff_discrip_cutter_close(&raw mut vc);
        ff_discrip_video_close(&raw mut v);
        snap
    };
    Run { sink, st, cut, snap_before_video }
}

/// Times on the grid: display position `frames` + `fields`.
fn grid(frames: i64, fields: i64) -> i64 {
    BASE + frames * FRAME + fields * FIELD
}

// ---- tests ----

#[test]
fn units_are_cut_from_the_byte_stream_and_keep_their_bytes() {
    // unit 2 starts in the first payload (whose time unit 1 took) and ends in
    // the second; unit 3 starts in the second payload and takes its time
    let (u1, u2, u3) = (spu(Some(100), true, &[], 1), spu(Some(50), true, &[], 2), spu(Some(70), true, &[], 3));
    let mut stream = [u1.clone(), u2.clone(), u3.clone()].concat();
    let cut = u1.len() + u2.len() / 2;
    let p2 = stream.split_off(cut);
    let r = run(&[(stream, Some(grid(10, 0))), (p2, Some(grid(20, 0)))], false);
    let out: Vec<(&[u8], i64)> = r.sink.out.iter().map(|o| (o.0.as_slice(), o.1)).collect();
    assert_eq!(out, vec![(u1.as_slice(), grid(10, 0)), (u3.as_slice(), grid(20, 0))]);
    // unit 2 has no time of its own: left out (as the reference does)
    assert_eq!(r.st.untimed, 1);
    assert_eq!(r.sink.events.len(), 1);
    assert_eq!(r.sink.events[0].kind, EV_SUB_UNTIMED);
    assert_eq!(r.sink.events[0].pos, i64::try_from(u1.len()).unwrap(), "its byte offset");
    assert_eq!((r.st.units, r.st.out), (3, 2));
}

#[test]
fn a_unit_over_several_payloads_takes_the_time_of_the_one_it_starts_in() {
    let u = spu(Some(100), true, &[], 1);
    let (a, b) = u.split_at(5);
    let r = run(&[(a.to_vec(), Some(grid(3, 0))), (b.to_vec(), None)], false);
    assert_eq!(r.sink.out.len(), 1);
    assert_eq!((r.sink.out[0].0.as_slice(), r.sink.out[0].1), (u.as_slice(), grid(3, 0)));
}

#[test]
fn a_unit_lasts_until_its_stop_command() {
    let r = run(
        &[
            (spu(Some(100), true, &[], 1), Some(grid(1, 0))),
            (spu(None, true, &[], 2), Some(grid(2, 0))),
            (spu(None, false, &[], 3), Some(grid(3, 0))),
        ],
        false,
    );
    let d: Vec<(i64, u32)> = r.sink.out.iter().map(|o| (o.2, o.3)).collect();
    assert_eq!(
        d,
        vec![
            (100 * STEP, F_KEY),
            // shows a picture without a stop time: one delay step, marked
            (STEP, F_KEY | F_NO_STOP),
            // no picture and no stop: one delay step
            (STEP, F_KEY),
        ]
    );
    assert_eq!(r.st.no_stop, 1);
}

#[test]
fn hd_dvd_units_with_32_bit_sizes() {
    let (u1, u2) = (spu_hd(250), spu_hd(30));
    let stream = [u1.clone(), u2.clone()].concat();
    let (a, b) = stream.split_at(u1.len() - 3); // unit 2 starts in the second payload
    let r = run(&[(a.to_vec(), Some(grid(4, 0))), (b.to_vec(), Some(grid(8, 0)))], false);
    let out: Vec<(&[u8], i64, i64)> = r.sink.out.iter().map(|o| (o.0.as_slice(), o.1, o.2)).collect();
    assert_eq!(out, vec![(u1.as_slice(), grid(4, 0), 250 * STEP), (u2.as_slice(), grid(8, 0), 30 * STEP)]);
}

#[test]
fn a_unit_whose_commands_do_not_parse_is_left_out() {
    // 0x55 is no display control command
    let r = run(
        &[
            (spu(Some(10), true, &[], 1), Some(grid(1, 0))),
            (spu(Some(10), true, &[0x55], 2), Some(grid(2, 0))),
            (spu(Some(10), true, &[], 3), Some(grid(3, 0))),
        ],
        false,
    );
    let marks: Vec<u8> = r.sink.out.iter().map(|o| o.0[4]).collect();
    assert_eq!(marks, vec![1, 3]);
    assert_eq!(r.cut.skipped, 1);
}

#[test]
fn bytes_where_no_unit_starts_are_given_up() {
    // a header stating a size below 9: the bytes kept so far are dropped and
    // the next unit is found at the start of the next payload
    let junk = vec![0x00, 0x04, 0x00, 0x02, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80];
    let u = spu(Some(10), true, &[], 7);
    let r = run(&[(junk, Some(grid(1, 0))), (u.clone(), Some(grid(2, 0)))], false);
    assert_eq!(r.sink.out.len(), 1);
    assert_eq!((r.sink.out[0].0.as_slice(), r.sink.out[0].1), (u.as_slice(), grid(2, 0)));
    assert_eq!((r.cut.skipped, r.cut.skipped_bytes), (1, 12));
}

#[test]
fn times_go_to_the_nearest_video_field() {
    let r = run(
        &[
            (spu(Some(10), true, &[], 1), Some(grid(5, 0) + 3 * MS)), // 3 ms late -> the field before
            (spu(Some(10), true, &[], 2), Some(grid(7, 0) + 12 * MS)), // 12 ms late -> the next field
            (spu(Some(10), true, &[], 3), Some(grid(9, 1))),           // on a field: unchanged
        ],
        false,
    );
    let t: Vec<i64> = r.sink.out.iter().map(|o| o.1).collect();
    assert_eq!(t, vec![grid(5, 0), grid(7, 1), grid(9, 1)]);
    assert_eq!(r.st.max_shift, FIELD - 12 * MS);
}

#[test]
fn units_before_the_video_start() {
    // 0.05 ms before the first field: on it; 1 ms before: left out
    let r = run(
        &[
            (spu(Some(10), true, &[], 1), Some(BASE - 54_000)),
            (spu(Some(10), true, &[], 2), Some(BASE - MS)),
            (spu(Some(10), true, &[], 3), Some(grid(1, 0))),
        ],
        false,
    );
    let t: Vec<(u8, i64)> = r.sink.out.iter().map(|o| (o.0[4], o.1)).collect();
    assert_eq!(t, vec![(1, BASE), (3, grid(1, 0))]);
    assert_eq!(r.st.early, 1);
    assert_eq!(r.sink.events.len(), 1);
    assert_eq!((r.sink.events[0].kind, r.sink.events[0].pos), (EV_SUB_EARLY, BASE - MS));
}

#[test]
fn units_wait_until_the_video_knows_its_grid() {
    let payloads =
        vec![(spu(Some(10), true, &[], 1), Some(grid(2, 0) + MS)), (spu(Some(10), true, &[], 2), Some(grid(6, 1)))];
    let r = run(&payloads, true);
    assert!(r.snap_before_video < 0, "no grid before the video");
    let t: Vec<i64> = r.sink.out.iter().map(|o| o.1).collect();
    assert_eq!(t, vec![grid(2, 0), grid(6, 1)]);
    assert_eq!(t, run(&payloads, false).sink.out.iter().map(|o| o.1).collect::<Vec<_>>());
}

#[test]
fn colour_changes_and_forced_starts_are_counted() {
    // `CHG_COLCON` with its 16-bit size: the command takes size + 3 bytes
    let colcon = [0x07, 0x00, 0x02, 0xAA, 0xBB];
    let r = run(
        &[(spu(Some(10), true, &colcon, 1), Some(grid(1, 0))), (spu(Some(10), true, &[0x00], 2), Some(grid(2, 0)))],
        false,
    );
    assert_eq!(r.sink.out.len(), 2);
    assert_eq!((r.st.colcon, r.st.forced), (1, 1));
}
