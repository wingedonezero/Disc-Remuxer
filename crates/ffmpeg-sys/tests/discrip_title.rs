//! One title through every stage of the rip core (`discrip_title.c`):
//! payloads of an MPEG-2 video track and an AC-3 (or LPCM) track in one or
//! two segments in, joined, junctioned and checked frames out. Audio frames
//! leave only once the video has been handed on past their time.

mod common;

use std::os::raw::{c_int, c_void};

use common::discrip::pictures;
use ffmpeg_sys::discrip::{
    codec_id, ff_discrip_frame_unref, ff_discrip_title_close, ff_discrip_title_duration, ff_discrip_title_finish,
    ff_discrip_title_frame, ff_discrip_title_open, ff_discrip_title_payload, ff_discrip_title_segment, Event, Frame,
    Title, TitleConfig, TitleTrack, EV_GAP, EV_OVERLAP, EV_START_SHIFT, F_TAIL, KIND_AUDIO, KIND_VIDEO,
};
use ffmpeg_sys::AV_NOPTS_VALUE;

const M2V: &[u8] = include_bytes!("data/testsrc_1200.m2v");
const AC3: &[u8] = include_bytes!("data/sine_7680.ac3");
const FRAME: i64 = 36_036_000; // 1001/30000 s
const AC3_DUR: i64 = 34_560_000; // 1536 samples at 48 kHz
const MS: i64 = 1_080_000;
const BASE: i64 = 10 * 1_080_000_000;

#[derive(Default)]
struct Sink {
    events: Vec<Event>,
}

unsafe extern "C" fn on_event(o: *mut c_void, e: *const Event) {
    // SAFETY: o is the &mut Sink; e valid during the call.
    unsafe { (*o.cast::<Sink>()).events.push(*e) };
}

/// A payload of a track: bytes, PES time, from the source's tail; `order`
/// places it among the other track's payloads (video: its decode time).
struct Payload {
    track: c_int,
    data: Vec<u8>,
    time: i64,
    tail: bool,
    order: i64,
}

/// An output frame: track, time, duration, flags, the largest video time
/// given out before it.
#[derive(Debug, Clone, Copy)]
struct Out {
    track: c_int,
    time: i64,
    dur: i64,
    flags: u32,
    vmax_before: i64,
}

/// The video payloads of one segment: every picture of the test stream with
/// its PES time on the grid from `base`.
fn video(base: i64) -> Vec<Payload> {
    pictures(M2V)
        .iter()
        .zip(0i64..)
        .map(|(&(a, b, disp, _), k)| Payload {
            track: 0,
            data: M2V[a..b].to_vec(),
            time: base + disp * FRAME,
            tail: false,
            order: base + (k - 1) * FRAME,
        })
        .collect()
}

/// AC-3 payloads: `n` frames (the test stream's frames over and over), one
/// per payload, the first at `start`, each `step` after the one before.
fn ac3(start: i64, step: i64, n: usize) -> Vec<Payload> {
    let frames: Vec<&[u8]> = AC3.chunks(768).collect();
    (0..n)
        .map(|k| {
            let time = start + i64::try_from(k).unwrap() * step;
            Payload { track: 1, data: frames[k % frames.len()].to_vec(), time, tail: false, order: time }
        })
        .collect()
}

/// Payloads interleaved by decode time (the way they lie in a program
/// stream); each track keeps its own order.
fn interleave(mut a: Vec<Payload>, b: Vec<Payload>) -> Vec<Payload> {
    a.extend(b);
    a.sort_by_key(|p| p.order);
    a
}

fn run(tracks: &[TitleTrack], tolerance: i64, lpcm_hd: bool, segments: &[Vec<Payload>]) -> (Vec<Out>, Sink, i64) {
    let mut sink = Sink::default();
    let mut outs = Vec::new();
    let mut t: *mut Title = std::ptr::null_mut();
    let mut vmax = i64::MIN;
    let mut drain = |t: *mut Title, outs: &mut Vec<Out>| loop {
        let mut f = Frame {
            buf: std::ptr::null_mut(),
            data: std::ptr::null_mut(),
            size: 0,
            time: 0,
            dur: 0,
            pos: 0,
            flags: 0,
            samples: 0,
            rate: 0,
            src: 0,
        };
        let mut k: c_int = 0;
        // SAFETY: t is open; f receives a frame we own.
        let r = unsafe { ff_discrip_title_frame(t, &raw mut k, &raw mut f) };
        if r != 0 {
            return r;
        }
        outs.push(Out { track: k, time: f.time, dur: f.dur, flags: f.flags, vmax_before: vmax });
        if k == 0 {
            vmax = vmax.max(f.time);
        }
        // SAFETY: ours.
        unsafe { ff_discrip_frame_unref(&raw mut f) };
    };
    // SAFETY: sink outlives the title; the title is closed below.
    let duration = unsafe {
        let sp = (&raw mut sink).cast();
        let cfg = TitleConfig {
            nb_tracks: c_int::try_from(tracks.len()).unwrap(),
            tracks: tracks.as_ptr(),
            tolerance,
            lpcm_hd: c_int::from(lpcm_hd),
            marks: std::ptr::null(),
            nb_marks: 0,
            event: Some(on_event),
            event_opaque: sp,
        };
        assert_eq!(ff_discrip_title_open(&raw mut t, std::ptr::null_mut(), &raw const cfg), 0);
        for seg in segments {
            assert_eq!(ff_discrip_title_segment(t), 0);
            for p in seg {
                let len = c_int::try_from(p.data.len()).unwrap();
                assert_eq!(ff_discrip_title_payload(t, p.track, p.data.as_ptr(), len, p.time, c_int::from(p.tail)), 0);
                assert_eq!(drain(t, &mut outs), -11, "EAGAIN while the title goes on"); // AVERROR(EAGAIN)
            }
        }
        assert_eq!(ff_discrip_title_finish(t), 0);
        assert_eq!(drain(t, &mut outs), -0x2046_4F45, "AVERROR_EOF after the last frame");
        let d = ff_discrip_title_duration(t);
        ff_discrip_title_close(&raw mut t);
        d
    };
    (outs, sink, duration)
}

fn tracks(audio: &str) -> Vec<TitleTrack> {
    vec![
        TitleTrack { codec: codec_id(c"mpeg2video").unwrap(), kind: KIND_VIDEO, audio_flags: 0 },
        TitleTrack {
            codec: codec_id(&std::ffi::CString::new(audio).unwrap()).unwrap(),
            kind: KIND_AUDIO,
            audio_flags: 0,
        },
    ]
}

fn kinds(s: &Sink) -> Vec<c_int> {
    s.events.iter().filter(|e| e.track == 1).map(|e| e.kind).collect()
}

#[test]
fn one_segment_starts_at_its_video_and_audio_waits_for_the_video() {
    // audio starts 5 ms before the video: within HD DVD's 102 ms it is kept
    // and the whole track shifted by it
    let n = 1200 * 36_036 / 34_560;
    let seg = interleave(video(BASE), ac3(BASE - 5 * MS, AC3_DUR, n));
    let (outs, sink, duration) = run(&tracks("ac3"), 110_160_000, true, &[seg]);
    let v: Vec<&Out> = outs.iter().filter(|o| o.track == 0).collect();
    let a: Vec<&Out> = outs.iter().filter(|o| o.track == 1).collect();
    assert_eq!(v.len(), 1200);
    assert_eq!(v.iter().map(|o| o.time).min(), Some(0), "the title starts at its first video time");
    assert_eq!(duration, 1200 * FRAME);
    assert_eq!(a.len(), n);
    assert_eq!(a[0].time, 0, "the lead-in became a shift");
    assert_eq!(kinds(&sink), vec![EV_START_SHIFT]);
    assert_eq!(sink.events[0].dur, 5 * MS);
    for (k, o) in a.iter().enumerate() {
        assert_eq!(o.time, i64::try_from(k).unwrap() * AC3_DUR);
        assert_eq!(o.dur, AC3_DUR);
    }
    // every audio frame left after the video reached its time on the title
    // timeline (its output time less the 5 ms shift), except those after the
    // last video frame (they leave when the video has ended)
    let last_video = v.iter().map(|o| o.time).max().unwrap();
    for o in &a {
        let t = o.time - 5 * MS;
        assert!(o.vmax_before >= t || t > last_video, "audio at {t} left at video {}", o.vmax_before);
    }
}

#[test]
fn the_second_segment_follows_the_video_of_the_first() {
    // segment 2 has its own clock; its audio starts 20 ms before its video,
    // segment 1's audio runs on 15 ms past its video: an overlap at the join
    let n1 = 1200 * 36_036 / 34_560 + 1;
    let s1 = interleave(video(BASE), ac3(BASE, AC3_DUR, n1));
    let base2 = 500 * 1_080_000_000;
    let s2 = interleave(video(base2), ac3(base2 - 20 * MS, AC3_DUR, 100));
    let (outs, sink, duration) = run(&tracks("ac3"), 110_160_000, true, &[s1, s2]);
    let v: Vec<&Out> = outs.iter().filter(|o| o.track == 0).collect();
    assert_eq!(v.len(), 2400);
    assert_eq!(duration, 2400 * FRAME, "segment 2 is placed where segment 1's video ended");
    let ev = kinds(&sink);
    assert!(ev.contains(&EV_OVERLAP), "{:?}", sink.events);
    assert!(!ev.contains(&EV_GAP), "{:?}", sink.events);
    let a: Vec<&Out> = outs.iter().filter(|o| o.track == 1).collect();
    for w in a.windows(2) {
        assert!(w[1].time >= w[0].time, "audio leaves in time order");
    }
}

#[test]
fn payloads_from_the_source_tail_mark_their_frames() {
    let n = 1200 * 36_036 / 34_560;
    let mut a = ac3(BASE, AC3_DUR, n);
    for p in a.iter_mut().skip(n - 10) {
        p.tail = true;
    }
    let (outs, _, _) = run(&tracks("ac3"), 110_160_000, true, &[interleave(video(BASE), a)]);
    let a: Vec<&Out> = outs.iter().filter(|o| o.track == 1).collect();
    assert_eq!(a.iter().filter(|o| o.flags & F_TAIL != 0).count(), 10);
    assert!(a[n - 10..].iter().all(|o| o.flags & F_TAIL != 0));
}

#[test]
fn lpcm_payloads_carry_their_header_and_go_through_the_pcm_strategy() {
    // DVD-Video LPCM, 16-bit stereo 48 kHz: each payload = 3-byte header +
    // 6 frames of 320 bytes (1/100 s); output frames of 1/30 s (1600 samples)
    let mut pay = Vec::new();
    for k in 0..4000i64 {
        let mut d = vec![u8::try_from(k % 32).unwrap(), 0x01, 0x80];
        d.extend(std::iter::repeat_n(0u8, 6 * 320));
        pay.push(Payload { track: 1, data: d, time: BASE + k * 10 * MS, tail: false, order: BASE + k * 10 * MS });
    }
    let (outs, sink, _) = run(&tracks("pcm_dvd"), 1_080_000, false, &[interleave(video(BASE), pay)]);
    let a: Vec<&Out> = outs.iter().filter(|o| o.track == 1).collect();
    assert!(!a.is_empty());
    assert_eq!(a[0].time, 0);
    assert!(a.iter().all(|o| o.dur == 36_000_000), "1600 samples at 48 kHz");
    // the video ends at 40.04 s: the PCM track ends with it
    let end = a.last().map(|o| o.time + o.dur).unwrap();
    assert!(end <= 1200 * FRAME + 36_000_000, "{end}");
    assert!(sink.events.iter().all(|e| e.kind != EV_GAP));
    let _ = AV_NOPTS_VALUE;
}
