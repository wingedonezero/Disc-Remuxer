//! VC-1 in the rip core (`discrip_vc1.c`, SMPTE 421M Advanced Profile): the
//! sequence header gives the frame rate and the header flags; the frame header
//! gives the picture type (PTYPE; for field pairs the 3-bit FPTYPE, Table 105)
//! and the fields (RFF / RPTFRM); display order is counted (B pictures after
//! a reference picture first, I pictures restart); sequence header + entry
//! point make a key frame. The core's own cutter makes a unit of one frame
//! with the headers before it and the user data after it; an end of sequence
//! stays only after a second field. Streams built bit by bit here.

use std::ffi::CString;
use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discrip::{
    codec_id, ff_discrip_cutter_close, ff_discrip_cutter_flush, ff_discrip_cutter_open, ff_discrip_cutter_write,
    ff_discrip_frame_unref, ff_discrip_video_close, ff_discrip_video_flush, ff_discrip_video_open,
    ff_discrip_video_stats, ff_discrip_video_unit, Cutter, Event, Frame, Video, VideoStats, EV_VIDEO_TIMECODE,
    F_DISCARD, F_KEY,
};

const FIELD: i64 = 18_018_000; // 1001/60000 s
const BASE: i64 = 5 * 1_080_000_000;

#[derive(Default)]
struct BitW {
    out: Vec<u8>,
    cur: u8,
    n: u8,
}

impl BitW {
    fn put(&mut self, v: u32, bits: u8) {
        for i in (0..bits).rev() {
            self.cur = (self.cur << 1) | u8::try_from((v >> i) & 1).unwrap();
            self.n += 1;
            if self.n == 8 {
                self.out.push(self.cur);
                self.cur = 0;
                self.n = 0;
            }
        }
    }
    fn finish(mut self) -> Vec<u8> {
        while self.n != 0 {
            self.put(1, 1);
        }
        self.out
    }
}

struct Flags {
    interlace: bool,
    pulldown: bool,
}

fn seq(f: &Flags) -> Vec<u8> {
    let mut b = BitW::default();
    b.put(3, 2); // profile: advanced
    b.put(3, 3); // level
    b.put(0b010_0000_0000, 11); // colordiff 1, postproc fields 0
    b.put(959, 12);
    b.put(539, 12);
    b.put(u32::from(f.pulldown), 1);
    b.put(u32::from(f.interlace), 1);
    b.put(0, 1); // tfcntrflag
    b.put(0, 2); // finterpflag, reserved
    b.put(0, 1); // psf
    b.put(1, 1); // display extension
    b.put(1919, 14);
    b.put(1079, 14);
    b.put(0, 1); // aspect ratio flag
    b.put(1, 1); // frame rate flag
    b.put(0, 1); // frameratind
    b.put(3, 8); // 30000
    b.put(2, 4); // /1001
    b.put(0, 1); // color format flag
    b.put(0, 1); // hrd param flag
    let mut v = vec![0, 0, 1, 0x0F];
    v.extend(b.finish());
    v.extend([0, 0, 1, 0x0E, 0x88, 0x88, 0x80]); // entry point
    v
}

/// A frame: the frame-header bits, then filler; `field2`: a second field unit.
fn frame(bits: &[(u32, u8)], field2: bool) -> Vec<u8> {
    let mut b = BitW::default();
    for &(v, n) in bits {
        b.put(v, n);
    }
    let mut v = vec![0, 0, 1, 0x0D];
    v.extend(b.finish());
    v.extend(std::iter::repeat_n(0x55u8, 200));
    if field2 {
        v.extend([0, 0, 1, 0x0C]);
        v.extend(std::iter::repeat_n(0x55u8, 150));
    }
    v
}

struct Sink {
    out: Vec<(i64, i64, u32)>,
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

/// Units (bytes, PES time) through the cutter and the video timing.
fn run(units: &[(Vec<u8>, i64)]) -> (Sink, VideoStats) {
    let id = codec_id(&CString::new("vc1").unwrap()).unwrap();
    let mut sink = Sink { out: Vec::new(), events: Vec::new() };
    let mut v: *mut Video = std::ptr::null_mut();
    let mut c: *mut Cutter = std::ptr::null_mut();
    let mut st = VideoStats::default();
    // SAFETY: sink outlives both stages; both are closed below.
    unsafe {
        let sp = (&raw mut sink).cast();
        assert_eq!(ff_discrip_video_open(&raw mut v, std::ptr::null_mut(), id, 0, on_frame, sp, Some(on_event), sp), 0);
        assert_eq!(ff_discrip_cutter_open(&raw mut c, std::ptr::null_mut(), id, ff_discrip_video_unit, v.cast()), 0);
        for (bytes, t) in units {
            assert_eq!(ff_discrip_cutter_write(c, bytes.as_ptr(), c_int::try_from(bytes.len()).unwrap(), *t), 0);
        }
        assert_eq!(ff_discrip_cutter_flush(c), 0);
        assert_eq!(ff_discrip_video_flush(v), 0);
        ff_discrip_video_stats(v, &raw mut st);
        ff_discrip_cutter_close(&raw mut c);
        ff_discrip_video_close(&raw mut v);
    }
    (sink, st)
}

/// Progressive PTYPE codes.
const P: (u32, u8) = (0, 1);
const B: (u32, u8) = (0b10, 2);
const I: (u32, u8) = (0b110, 3);
const BI: (u32, u8) = (0b1110, 4);

#[test]
fn progressive_b_pictures_are_counted_into_display_order() {
    let f = Flags { interlace: false, pulldown: false };
    // decode order I P B B P B BI, GOPs of 7; display I B B P B BI P
    let pattern: [((u32, u8), i64); 7] = [(I, 0), (P, 3), (B, 1), (B, 2), (P, 6), (B, 4), (BI, 5)];
    let mut units = Vec::new();
    for g in 0..40 {
        for (k, &(pt, disp)) in pattern.iter().enumerate() {
            let mut u = if k == 0 { seq(&f) } else { Vec::new() };
            u.extend(frame(&[pt], false));
            units.push((u, BASE + (g * 7 + disp) * 2 * FIELD));
        }
    }
    let (s, st) = run(&units);
    assert_eq!((st.num, st.den), (30000, 1001));
    assert_eq!(s.out.len(), units.len());
    for (o, u) in s.out.iter().zip(&units) {
        assert_eq!(o.0, u.1, "grid time = PES time");
        assert_eq!(o.1, 2 * FIELD);
    }
    assert!(s.events.is_empty(), "{:?}", s.events);
    assert_eq!(s.out[0].2 & F_KEY, F_KEY);
    assert_eq!(s.out[2].2 & F_DISCARD, F_DISCARD);
    assert_eq!(s.out[6].2 & F_DISCARD, F_DISCARD, "BI pictures are B pictures");
}

#[test]
fn field_pairs_take_their_type_from_fptype() {
    // field-interlaced frames (FCM 11): the GOP starts with an I/P pair and a
    // BI/B pair sits where a B pair belongs -- the 300 Combo case that the
    // PTYPE reading displaces by two frames
    let f = Flags { interlace: true, pulldown: false };
    let fcm_field = (0b11, 2);
    let pattern: [(u32, i64); 4] = [(1, 0), (3, 3), (6, 1), (4, 2)]; // I/P, P/P, BI/B, B/B
    let mut units = Vec::new();
    for g in 0..60 {
        for (k, &(fpt, disp)) in pattern.iter().enumerate() {
            let mut u = if k == 0 { seq(&f) } else { Vec::new() };
            u.extend(frame(&[fcm_field, (fpt, 3)], true));
            units.push((u, BASE + (g * 4 + disp) * 2 * FIELD));
        }
    }
    let (s, _) = run(&units);
    assert_eq!(s.out.len(), units.len());
    for (o, u) in s.out.iter().zip(&units) {
        assert_eq!(o.0, u.1);
        assert_eq!(o.1, 2 * FIELD, "a field pair lasts two fields");
    }
    assert!(!s.events.iter().any(|e| e.kind == EV_VIDEO_TIMECODE), "{:?}", s.events);
    assert!(s.out.iter().step_by(4).all(|o| o.2 & F_KEY != 0), "I/P pairs are key frames");
    assert!(s.out.iter().skip(2).step_by(4).all(|o| o.2 & F_DISCARD != 0), "BI/B pairs are B pictures");
}

#[test]
fn repeated_fields_lengthen_frames() {
    // interlaced pulldown: TFF / RFF after PTYPE (FCM 0 = progressive frame)
    // pattern of 2 and 3 fields as in 3:2 pulldown; I and P only
    let f = Flags { interlace: true, pulldown: true };
    let mut units = Vec::new();
    let mut pos = 0i64;
    for k in 0..200 {
        let rff = u32::from(k % 2 == 1);
        let pt = if k % 12 == 0 { I } else { P };
        let mut u = if k % 12 == 0 { seq(&f) } else { Vec::new() };
        u.extend(frame(&[(0, 1), pt, (1, 1), (rff, 1)], false));
        units.push((u, BASE + pos * FIELD));
        pos += 2 + i64::from(rff);
    }
    let (s, _) = run(&units);
    for (k, (o, u)) in s.out.iter().zip(&units).enumerate() {
        assert_eq!(o.0, u.1);
        assert_eq!(o.1, if k % 2 == 1 { 3 * FIELD } else { 2 * FIELD });
    }
    assert!(s.events.is_empty());
}

#[test]
fn skipped_frames_repeat_fields_too() {
    // 3:2 pulldown in RFF flags where every second P frame is skipped (PTYPE
    // 1111): the skipped frame still carries TFF / RFF (no TFCNTR), as Blu-ray
    // and HD DVD streams have it; 2 + 3 fields per pair of frames
    const SKIPPED: (u32, u8) = (0b1111, 4);
    let f = Flags { interlace: true, pulldown: true };
    let mut units = Vec::new();
    let mut pos = 0i64;
    for k in 0..120 {
        let rff = u32::from(k % 2 == 1);
        let pt = if k % 12 == 0 { I } else if k % 4 == 3 { SKIPPED } else { P };
        let mut u = if k % 12 == 0 { seq(&f) } else { Vec::new() };
        u.extend(frame(&[(0, 1), pt, (1, 1), (rff, 1)], false));
        units.push((u, BASE + pos * FIELD));
        pos += 2 + i64::from(rff);
    }
    let (s, _) = run(&units);
    assert_eq!(s.out.len(), units.len());
    for (k, (o, u)) in s.out.iter().zip(&units).enumerate() {
        assert_eq!(o.0, u.1, "frame {k}");
        assert_eq!(o.1, if k % 2 == 1 { 3 * FIELD } else { 2 * FIELD }, "frame {k}");
    }
    assert!(s.events.is_empty(), "{:?}", s.events);
}

/// A stream through the cutter (fed `chunk` bytes at a time, no PES times)
/// and the video stage: the frames' bytes in decode order, or the first
/// error the cutter returned.
fn frames_of(stream: &[u8], chunk: usize) -> Result<Vec<Vec<u8>>, c_int> {
    let id = codec_id(&CString::new("vc1").unwrap()).unwrap();
    let mut sink = Bytes(Vec::new());
    let mut v: *mut Video = std::ptr::null_mut();
    let mut c: *mut Cutter = std::ptr::null_mut();
    let mut err = 0;
    // SAFETY: sink outlives both stages; both are closed below.
    unsafe {
        let sp = (&raw mut sink).cast();
        assert_eq!(ff_discrip_video_open(&raw mut v, std::ptr::null_mut(), id, 0, on_bytes, sp, None, sp), 0);
        assert_eq!(ff_discrip_cutter_open(&raw mut c, std::ptr::null_mut(), id, ff_discrip_video_unit, v.cast()), 0);
        for part in stream.chunks(chunk) {
            err = ff_discrip_cutter_write(c, part.as_ptr(), c_int::try_from(part.len()).unwrap(), ffmpeg_sys::AV_NOPTS_VALUE);
            if err < 0 {
                break;
            }
        }
        if err == 0 {
            err = ff_discrip_cutter_flush(c);
        }
        if err == 0 {
            assert_eq!(ff_discrip_video_flush(v), 0);
        }
        ff_discrip_cutter_close(&raw mut c);
        ff_discrip_video_close(&raw mut v);
    }
    if err < 0 { Err(err) } else { Ok(sink.0) }
}

struct Bytes(Vec<Vec<u8>>);

unsafe extern "C" fn on_bytes(o: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: o is the &mut Bytes; f a frame we own whose data holds size bytes.
    let (s, fr) = unsafe { (&mut *o.cast::<Bytes>(), &mut *f) };
    // SAFETY: as above.
    s.0.push(unsafe { std::slice::from_raw_parts(fr.data, usize::try_from(fr.size).unwrap()) }.to_vec());
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}

/// The same frames whatever the payload sizes.
fn frames(stream: &[u8]) -> Result<Vec<Vec<u8>>, c_int> {
    let all = frames_of(stream, 2048);
    for chunk in [1, 3, 7] {
        assert_eq!(frames_of(stream, chunk), all, "payloads of {chunk} bytes");
    }
    all
}

fn bdu(code: u8) -> Vec<u8> {
    vec![0, 0, 1, code, 0x4A, 0x55, 0x80]
}

const END_OF_SEQUENCE: [u8; 4] = [0, 0, 1, 0x0A];

#[test]
fn user_data_after_a_frame_header_stays_with_its_frame() {
    // SMPTE 421M Annex E: frame-level (0x1D) and field-level (0x1C) user data
    // follow the frame header they belong to; FFmpeg's parser starts the next
    // unit there
    let f = Flags { interlace: false, pulldown: false };
    let a = [seq(&f), frame(&[I], false), bdu(0x1D)].concat();
    let b = [frame(&[P], false), bdu(0x1C), bdu(0x1D)].concat();
    let c = [seq(&f), frame(&[I], false)].concat();
    assert_eq!(frames(&[a.clone(), b.clone(), c.clone()].concat()), Ok(vec![a, b, c]));
}

#[test]
fn an_end_of_sequence_without_a_second_field_is_left_out() {
    let f = Flags { interlace: false, pulldown: false };
    let a = [seq(&f), frame(&[I], false)].concat();
    let b = [frame(&[P], false), bdu(0x1D)].concat();
    let c = [seq(&f), frame(&[I], false)].concat();
    let stream = [&a[..], &END_OF_SEQUENCE, &b, &END_OF_SEQUENCE, &END_OF_SEQUENCE, &c, &END_OF_SEQUENCE].concat();
    assert_eq!(frames(&stream), Ok(vec![a, b, c]));
}

#[test]
fn a_second_field_keeps_its_user_data_and_the_end_of_sequence_after_it() {
    let f = Flags { interlace: true, pulldown: false };
    let fcm_field = (0b11, 2);
    let mut first = frame(&[fcm_field, (1, 3)], false); // I/P field pair, first field
    first.extend(bdu(0x1C));
    first.extend([0, 0, 1, 0x0C]);
    first.extend(std::iter::repeat_n(0x55u8, 150));
    first.extend(bdu(0x1C));
    first.extend(END_OF_SEQUENCE);
    first.extend(END_OF_SEQUENCE);
    let a = [seq(&f), first].concat();
    let b = [seq(&f), frame(&[fcm_field, (1, 3)], true)].concat();
    let out = frames(&[a.clone(), b.clone()].concat()).unwrap();
    assert_eq!(out, vec![a, b]);
}

#[test]
fn headers_without_a_frame_are_left_out() {
    // a sequence header + entry point followed by another sequence header,
    // or by an end of sequence: those headers belong to no frame
    let f = Flags { interlace: false, pulldown: false };
    let a = [seq(&f), frame(&[I], false)].concat();
    let stream = [seq(&f), a.clone(), seq(&f), END_OF_SEQUENCE.to_vec(), a.clone()].concat();
    assert_eq!(frames(&stream), Ok(vec![a.clone(), a]));
}

#[test]
fn start_codes_where_none_can_be_stop_the_track() {
    let f = Flags { interlace: false, pulldown: false };
    let good = [seq(&f), frame(&[I], false)].concat();
    let invalid = -0x4144_4E49; // AVERROR_INVALIDDATA
    // a slice, slice user data, a reserved code
    for code in [0x0B, 0x1B, 0x10] {
        let stream = [good.clone(), bdu(code), good.clone()].concat();
        assert_eq!(frames_of(&stream, 2048), Err(invalid), "start code 0x{code:02X}");
    }
    // a field after the second field: a field without its frame
    let stream = [good.clone(), bdu(0x0C), bdu(0x0C), good.clone()].concat();
    assert_eq!(frames_of(&stream, 2048), Err(invalid));
    // an entry point after the entry-point user data
    let seq_only = &seq(&f)[..seq(&f).len() - 7];
    let stream = [seq_only, &bdu(0x0E)[..], &bdu(0x1E), &bdu(0x0E), &frame(&[I], false)].concat();
    assert_eq!(frames_of(&stream, 2048), Err(invalid));
    // frame user data before any frame
    let stream = [bdu(0x1D), good.clone()].concat();
    assert_eq!(frames_of(&stream, 2048), Err(invalid));
}

/// With `DISCRIP_VC1_FILE` (a raw VC-1 elementary stream): every unit our
/// rules read, as `pos key discard fields` lines on stdout, to compare with
/// FFmpeg's decoder (`ffprobe -show_frames`).
#[test]
#[ignore = "needs DISCRIP_VC1_FILE"]
fn print_units_of_a_file() {
    let Ok(path) = std::env::var("DISCRIP_VC1_FILE") else { return };
    let data = std::fs::read(path).unwrap();
    let units: Vec<(Vec<u8>, i64)> =
        data.chunks(2048).map(|c| (c.to_vec(), ffmpeg_sys::AV_NOPTS_VALUE)).collect();
    let id = codec_id(&CString::new("vc1").unwrap()).unwrap();
    let mut sink = Sink2(Vec::new());
    let mut v: *mut Video = std::ptr::null_mut();
    let mut c: *mut Cutter = std::ptr::null_mut();
    // SAFETY: sink outlives both stages; both are closed below.
    unsafe {
        let sp = (&raw mut sink).cast();
        assert_eq!(ff_discrip_video_open(&raw mut v, std::ptr::null_mut(), id, 0, on_frame2, sp, None, sp), 0);
        assert_eq!(ff_discrip_cutter_open(&raw mut c, std::ptr::null_mut(), id, ff_discrip_video_unit, v.cast()), 0);
        for (bytes, t) in &units {
            assert_eq!(ff_discrip_cutter_write(c, bytes.as_ptr(), c_int::try_from(bytes.len()).unwrap(), *t), 0);
        }
        assert_eq!(ff_discrip_cutter_flush(c), 0);
        assert_eq!(ff_discrip_video_flush(v), 0);
        ff_discrip_cutter_close(&raw mut c);
        ff_discrip_video_close(&raw mut v);
    }
    for (pos, flags, dur) in sink.0 {
        println!("UNIT {pos} {} {} {}", u8::from(flags & F_KEY != 0), u8::from(flags & F_DISCARD != 0), dur / FIELD);
    }
}

struct Sink2(Vec<(i64, u32, i64)>);

unsafe extern "C" fn on_frame2(o: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: o is the &mut Sink2; f a frame we own.
    let (s, fr) = unsafe { (&mut *o.cast::<Sink2>(), &mut *f) };
    s.0.push((fr.pos, fr.flags, fr.dur));
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}
