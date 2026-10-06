//! `libavformat/discrip_units.c`: the rip core's stage 1. Payloads are cut
//! into units by FFmpeg's parser; a payload's time goes to the first unit
//! whose anchor (audio: the frame start; MPEG-2 video: the picture start code)
//! lies in that payload's bytes (ISO/IEC 13818-1 2.4.3.7). Every expectation
//! is computed here from the built stream's layout, independently of the code.

use std::ffi::CString;
use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discrip::{
    codec_id, ff_discrip_cutter_close, ff_discrip_cutter_flush, ff_discrip_cutter_open, ff_discrip_cutter_stats,
    ff_discrip_cutter_write, Cutter, CutterStats, Unit, TICKS_PER_PTS,
};
use ffmpeg_sys::AV_NOPTS_VALUE;

#[derive(Debug, Clone, PartialEq)]
struct Out {
    pos: i64,
    size: usize,
    time: i64,
    head: Vec<u8>,
}

unsafe extern "C" fn on_unit(opaque: *mut c_void, u: *const Unit) -> c_int {
    // SAFETY: opaque is the &mut Vec<Out> passed to the cutter; u is valid during the call.
    let (outs, u) = unsafe { (&mut *opaque.cast::<Vec<Out>>(), &*u) };
    let size = usize::try_from(u.size).unwrap();
    // SAFETY: data holds size bytes during the call.
    let data = unsafe { std::slice::from_raw_parts(u.data, size) };
    outs.push(Out { pos: u.pos, size, time: u.time, head: data[..size.min(4)].to_vec() });
    0
}

/// Runs the cutter for `codec` over the payloads (bytes, time).
fn run(codec: &str, payloads: &[(&[u8], Option<i64>)]) -> (Vec<Out>, CutterStats) {
    let id = codec_id(&CString::new(codec).unwrap()).expect("codec name");
    let mut outs: Vec<Out> = Vec::new();
    let mut c: *mut Cutter = std::ptr::null_mut();
    // SAFETY: outs outlives the cutter; the cutter is closed below.
    unsafe {
        assert_eq!(
            ff_discrip_cutter_open(&raw mut c, std::ptr::null_mut(), id, on_unit, std::ptr::addr_of_mut!(outs).cast()),
            0
        );
        for (data, time) in payloads {
            let len = c_int::try_from(data.len()).unwrap();
            assert_eq!(ff_discrip_cutter_write(c, data.as_ptr(), len, time.unwrap_or(AV_NOPTS_VALUE)), 0);
        }
        assert_eq!(ff_discrip_cutter_flush(c), 0);
        let mut st = CutterStats::default();
        ff_discrip_cutter_stats(c, &raw mut st);
        ff_discrip_cutter_close(&raw mut c);
        (outs, st)
    }
}

/// The expected time of an anchor at stream offset `a`: payload k's time goes
/// to the first anchor in `[start_k, end_k)`.
fn expected_time(payloads: &[(&[u8], Option<i64>)], anchors: &[i64], a: i64) -> i64 {
    let mut start = 0i64;
    for (data, time) in payloads {
        let end = start + i64::try_from(data.len()).unwrap();
        if let Some(t) = time {
            if anchors.iter().find(|&&x| x >= start && x < end) == Some(&a) {
                return *t;
            }
        }
        start = end;
    }
    AV_NOPTS_VALUE
}

/// One AC-3 syncframe: 48 kHz, 192 kbit/s (768 bytes), bsid 8, 2/0; the rest
/// is zero (the parser reads only the header).
fn ac3_frame() -> Vec<u8> {
    let mut f = vec![0u8; 768];
    f[..7].copy_from_slice(&[0x0B, 0x77, 0x00, 0x00, 0x14, 0x40, 0x40]);
    f
}

#[test]
fn ac3_times_go_to_the_first_frame_starting_in_each_payload() {
    // 92 junk bytes, then 60 frames; DVD-sized payloads (2016 bytes) so that
    // frame starts fall at every position against payload boundaries,
    // including headers split over two payloads. Every 3rd payload has no time.
    let lead = 92usize;
    let mut stream = vec![0x11u8; lead];
    for _ in 0..60 {
        stream.extend_from_slice(&ac3_frame());
    }
    let payloads: Vec<(&[u8], Option<i64>)> = stream
        .chunks(2016)
        .enumerate()
        .map(|(k, p)| (p, (k % 3 != 2).then(|| i64::try_from(k).unwrap() * 1000 * TICKS_PER_PTS)))
        .collect();
    let anchors: Vec<i64> = (0..60).map(|n| i64::try_from(lead + n * 768).unwrap()).collect();

    let (outs, st) = run("ac3", &payloads);

    assert_eq!(outs.len(), 60, "every frame is a unit, the junk is not");
    assert_eq!(st.skipped, 1);
    assert_eq!(st.skipped_bytes, 92);
    for (o, &a) in outs.iter().zip(&anchors) {
        assert_eq!(o.pos, a);
        assert_eq!(o.size, 768);
        assert_eq!(o.time, expected_time(&payloads, &anchors, a), "frame at {a}");
    }
    // payloads with a time but no frame start in them give their time to no unit
    let timed_with_start = payloads
        .iter()
        .scan(0i64, |s, (d, t)| {
            let start = *s;
            *s += i64::try_from(d.len()).unwrap();
            Some((start, *s, *t))
        })
        .filter(|(s, e, t)| t.is_some() && anchors.iter().any(|a| a >= s && a < e))
        .count();
    assert_eq!(st.timed, i64::try_from(timed_with_start).unwrap());
    assert_eq!(st.records_unused, st.records - st.timed);
}

#[test]
fn a_time_is_never_given_to_a_frame_that_started_before_the_payload() {
    // Frames larger than a payload: payload 1 holds only the middle of frame 0,
    // so its time belongs to no unit; payload 2 holds the start of frame 1.
    let mut stream = Vec::new();
    for _ in 0..3 {
        stream.extend_from_slice(&ac3_frame());
    }
    let p: Vec<&[u8]> = vec![&stream[..300], &stream[300..700], &stream[700..]];
    let payloads = [(p[0], Some(100)), (p[1], Some(200)), (p[2], Some(300))];
    let (outs, st) = run("ac3", &payloads);
    let times: Vec<i64> = outs.iter().map(|o| o.time).collect();
    assert_eq!(times, vec![100, 300, AV_NOPTS_VALUE]);
    assert_eq!(st.records_unused, 1);
}

/// MPEG-2 video: sequence header (720x480, 29.97 fps), GOP header, picture
/// header (temporal reference `tr`, type `kind`), one slice of filler.
fn mpeg2_picture(seq: bool, tr: u16, kind: u8) -> Vec<u8> {
    let mut v = Vec::new();
    if seq {
        v.extend_from_slice(&[0, 0, 1, 0xB3, 0x2D, 0x01, 0xE0, 0x24, 0xFF, 0xFF, 0xE0, 0x18]);
        v.extend_from_slice(&[0, 0, 1, 0xB8, 0x00, 0x08, 0x00, 0x00]);
    }
    let tr = u8::try_from((tr >> 2) & 0xFF).unwrap();
    v.extend_from_slice(&[0, 0, 1, 0x00, tr, (kind & 7) << 3, 0xFF, 0xF8]);
    v.extend_from_slice(&[0, 0, 1, 0x01]);
    v.extend(std::iter::repeat_n(0x55u8, 600));
    v
}

#[test]
fn mpeg2_times_go_to_the_picture_start_code_not_the_sequence_header() {
    // 12 pictures, a sequence header + GOP every 4th (pictures of 632 bytes
    // with the headers, 612 without). Payloads of 2470 bytes: the boundary at
    // 2470 falls between picture 4's sequence header (2468) and its picture
    // start code (2488).
    let mut stream = Vec::new();
    let mut anchors = Vec::new();
    for n in 0..12u16 {
        let pic = mpeg2_picture(n % 4 == 0, n, if n % 4 == 0 { 1 } else { 2 });
        let a = pic.windows(4).position(|w| w == [0, 0, 1, 0]).unwrap();
        anchors.push(i64::try_from(stream.len() + a).unwrap());
        stream.extend_from_slice(&pic);
    }
    let payloads: Vec<(&[u8], Option<i64>)> = stream
        .chunks(2470)
        .enumerate()
        .map(|(k, p)| (p, Some(i64::try_from(k).unwrap() * 7 + 1)))
        .collect();

    let (outs, st) = run("mpeg2video", &payloads);

    assert_eq!(outs.len(), 12, "{outs:?}");
    assert_eq!(st.skipped, 0);
    let mut seq_split = 0;
    for (o, &a) in outs.iter().zip(&anchors) {
        assert!(o.pos <= a && a < o.pos + i64::try_from(o.size).unwrap(), "unit {o:?} holds anchor {a}");
        assert_eq!(o.time, expected_time(&payloads, &anchors, a), "picture at {a}");
        if o.head == [0, 0, 1, 0xB3] && o.pos / 2470 != a / 2470 {
            seq_split += 1;
        }
    }
    assert!(seq_split > 0, "layout has a sequence header in an earlier payload than its picture");
}

#[test]
fn a_codec_without_a_table_entry_is_refused() {
    let id = codec_id(&CString::new("hevc").unwrap()).unwrap();
    let mut outs: Vec<Out> = Vec::new();
    let mut c: *mut Cutter = std::ptr::null_mut();
    // SAFETY: a failed open leaves c NULL; close accepts NULL.
    let ret = unsafe {
        let r = ff_discrip_cutter_open(&raw mut c, std::ptr::null_mut(), id, on_unit, std::ptr::addr_of_mut!(outs).cast());
        ff_discrip_cutter_close(&raw mut c);
        r
    };
    assert!(ret < 0);
    assert!(c.is_null());
}
