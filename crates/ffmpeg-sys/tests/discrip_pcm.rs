//! `libavformat/discrip_pcm.c`: the PCM strategy of the rip core. Input
//! frames carry 16-bit stereo 48 kHz samples whose values are their own
//! sample numbers, so the output shows exactly which samples were written,
//! where silence went, and what was cut. Output frames are 1600 samples
//! (1/30 s), timed by a sample counter from 0.

use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discrip::{
    ff_discrip_frame_unref, ff_discrip_pcm_close, ff_discrip_pcm_finish, ff_discrip_pcm_open, ff_discrip_pcm_push,
    ff_discrip_pcm_stats, Event, Frame, Pcm, PcmConfig, PcmStats, VideoRef, EV_PCM_SILENCE, EV_PCM_SKIP,
    EV_PCM_TIMECODE, EV_START_DROP, EV_START_SHIFT, EV_VIDEO_ENDED, F_KEY,
};

const RATE: i64 = 48_000;
const T: i64 = 22_500; // ticks per sample at 48 kHz
const MS: i64 = 1_080_000;
const N: i64 = 80; // samples per input frame (DVD-Video LPCM at 48 kHz)

struct Video {
    end: i64,
}
unsafe extern "C" fn v_max(o: *mut c_void) -> i64 {
    // SAFETY: o is the &mut Video.
    unsafe { (*o.cast::<Video>()).end }
}
unsafe extern "C" fn v_advance(o: *mut c_void, target: i64, ended: *mut c_int) -> c_int {
    // SAFETY: o is the &mut Video, ended valid.
    unsafe { *ended = c_int::from((*o.cast::<Video>()).end < target) };
    0
}

#[derive(Default)]
struct Sink {
    samples: Vec<u16>, // left channel of every sample written
    frames: Vec<(i64, i64, u32)>,
    events: Vec<Event>,
}
unsafe extern "C" fn on_out(o: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: o is the &mut Sink; f a frame we own.
    let (s, fr) = unsafe { (&mut *o.cast::<Sink>(), &mut *f) };
    // SAFETY: data holds size bytes.
    let b = unsafe { std::slice::from_raw_parts(fr.data, usize::try_from(fr.size).unwrap()) };
    s.samples.extend(b.chunks(4).map(|c| u16::from_le_bytes([c[0], c[1]])));
    s.frames.push((fr.time, fr.dur, fr.flags));
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}
unsafe extern "C" fn on_event(o: *mut c_void, e: *const Event) {
    // SAFETY: o is the &mut Sink; e valid during the call.
    unsafe { (*o.cast::<Sink>()).events.push(*e) };
}

/// An input frame at `time` holding samples `first .. first + n`
/// (little-endian, both channels the sample number; 0 is kept for silence).
fn frame(time: i64, first: i64, n: i64) -> (Vec<u8>, i64, i64) {
    let mut d = Vec::new();
    for k in first..first + n {
        let v = u16::try_from(k % 65_000 + 1).unwrap().to_le_bytes();
        d.extend_from_slice(&[v[0], v[1], v[0], v[1]]);
    }
    (d, time, n * T)
}

fn val(k: i64) -> u16 {
    u16::try_from(k % 65_000 + 1).unwrap()
}

fn run(frames: &[(Vec<u8>, i64, i64)], video_end: i64) -> (Sink, PcmStats) {
    let mut sink = Sink::default();
    let mut video = Video { end: video_end };
    let cfg = PcmConfig {
        track: 1,
        rate: c_int::try_from(RATE).unwrap(),
        bits: 16,
        bytes_per_sample_frame: 4,
        video: VideoRef { opaque: (&raw mut video).cast(), max_time: v_max, advance: v_advance },
        out: on_out,
        out_opaque: (&raw mut sink).cast(),
        event: Some(on_event),
        event_opaque: (&raw mut sink).cast(),
    };
    let mut p: *mut Pcm = std::ptr::null_mut();
    let mut st = PcmStats::default();
    // SAFETY: cfg, sink, video and the frames' bytes outlive the calls; p is closed below.
    unsafe {
        assert_eq!(ff_discrip_pcm_open(&raw mut p, std::ptr::null_mut(), &raw const cfg), 0);
        for (d, time, dur) in frames {
            let mut f = Frame {
                buf: std::ptr::null_mut(),
                data: d.as_ptr().cast_mut(),
                size: c_int::try_from(d.len()).unwrap(),
                time: *time,
                dur: *dur,
                pos: 0,
                flags: F_KEY,
                samples: 0,
                rate: 0,
                src: *time,
            };
            assert_eq!(ff_discrip_pcm_push(p, &raw mut f), 0);
        }
        assert_eq!(ff_discrip_pcm_finish(p), 0);
        ff_discrip_pcm_stats(p, &raw mut st);
        ff_discrip_pcm_close(&raw mut p);
    }
    (sink, st)
}

/// `count` frames from `time`, samples from `first`, back to back.
fn run_of(time: i64, first: i64, count: i64) -> Vec<(Vec<u8>, i64, i64)> {
    (0..count).map(|k| frame(time + k * N * T, first + k * N, N)).collect()
}

fn kinds(s: &Sink) -> Vec<c_int> {
    s.events.iter().map(|e| e.kind).collect()
}

#[test]
fn back_to_back_frames_leave_in_frames_of_1600_samples() {
    let (s, st) = run(&run_of(0, 0, 50), i64::MAX);
    assert_eq!(s.samples, (0..50 * N).map(val).collect::<Vec<_>>());
    assert_eq!(s.frames.len(), 3);
    assert_eq!(s.frames[0], (0, 1600 * T, F_KEY));
    assert_eq!(s.frames[1].0, 1600 * T);
    assert_eq!(s.frames[2], (3200 * T, 800 * T, F_KEY), "the last one as it is");
    assert!(s.events.is_empty(), "{:?}", s.events);
    assert_eq!((st.input, st.out), (50, 3));
}

#[test]
fn a_lead_in_within_1_ms_is_kept_and_its_track_starts_at_0() {
    // 0.8 ms before the video: all samples, from 0
    let (s, _) = run(&run_of(-864_000, 0, 30), i64::MAX);
    assert_eq!(s.samples, (0..30 * N).map(val).collect::<Vec<_>>());
    assert_eq!(s.frames[0].0, 0);
    assert_eq!(kinds(&s), vec![EV_START_SHIFT]);
}

#[test]
fn frames_more_than_1_ms_early_are_dropped() {
    // frames at -9.4, -7.73, -6.07, -4.4, -2.73, -1.07 ms ... (1.667 ms each):
    // those before -1 ms dropped; the first left is 1.07 ms early, inside 1 ms?
    // no: 1.067 ms > 1 ms is dropped too; the one at +0.6 ms starts the track
    // after 0.6 ms of silence
    let start = -9 * MS - 400 * 1_080;
    let (s, st) = run(&run_of(start, 0, 40), i64::MAX);
    let first_kept = (0..40).find(|k| start + k * N * T >= -MS).unwrap();
    assert_eq!(st.dropped, first_kept);
    let t0 = start + first_kept * N * T;
    let lead = t0 * RATE / 1_080_000_000;
    assert!(t0 >= 0 && lead > 0);
    assert_eq!(s.samples[..usize::try_from(lead).unwrap()], vec![0; usize::try_from(lead).unwrap()][..]);
    assert_eq!(s.samples[usize::try_from(lead).unwrap()], val(first_kept * N));
    assert!(kinds(&s).contains(&EV_START_DROP) == (t0.abs() > 3 * MS));
}

#[test]
fn a_late_start_below_5_s_is_filled_with_silence_from_5_s_on_skipped() {
    let (s, _) = run(&run_of(100 * MS, 0, 10), i64::MAX);
    assert_eq!(&s.samples[..4800], &[0u16; 4800][..], "100 ms of silence");
    assert_eq!(s.samples[4800], val(0));
    assert!(kinds(&s).contains(&EV_PCM_SILENCE));
    let (s, _) = run(&run_of(6000 * MS, 0, 30), i64::MAX);
    assert_eq!(s.samples[0], val(0), "no silence written");
    assert_eq!(s.frames[0].0, 6 * 1_080_000_000, "the first frame at 6 s");
    assert_eq!(kinds(&s), vec![EV_PCM_SKIP]);
}

#[test]
fn a_gap_is_filled_with_silence() {
    // 30 frames, a 50 ms gap, 30 frames
    let mut f = run_of(0, 0, 30);
    let t = 30 * N * T + 50 * MS;
    f.extend(run_of(t, 30 * N, 30));
    let (s, st) = run(&f, i64::MAX);
    let gap = 50 * MS / T;
    assert_eq!(st.silence, gap);
    let mut want: Vec<u16> = (0..30 * N).map(val).collect();
    want.extend(std::iter::repeat_n(0, usize::try_from(gap).unwrap()));
    want.extend((30 * N..60 * N).map(val));
    assert_eq!(s.samples, want);
}

#[test]
fn frames_with_a_broken_time_are_appended_where_they_follow_on() {
    // frames 10..12 carry times 50 ms late; frame 13 goes on from where 9
    // ended without them: they were broken, not a gap
    let mut f = run_of(0, 0, 20);
    for g in &mut f[10..13] {
        g.1 += 50 * MS;
    }
    let (s, st) = run(&f, i64::MAX);
    assert_eq!(s.samples, (0..20 * N).map(val).collect::<Vec<_>>());
    assert_eq!(st.silence, 0);
    assert_eq!(st.broken, 3);
    let e: Vec<&Event> = s.events.iter().filter(|e| e.kind == EV_PCM_TIMECODE).collect();
    assert_eq!((e.len(), e[0].count), (1, 3));
}

#[test]
fn a_frame_running_into_the_next_is_cut_at_its_start() {
    // frame 10 starts 10 samples before frame 9 ends: frame 9 is cut
    let mut f = run_of(0, 0, 20);
    for (k, g) in f.iter_mut().enumerate().skip(10) {
        let k = i64::try_from(k).unwrap();
        *g = frame(k * N * T - 10 * T, k * N, N);
    }
    let (s, st) = run(&f, i64::MAX);
    let mut want: Vec<u16> = (0..10 * N - 10).map(val).collect();
    want.extend((10 * N..20 * N).map(val));
    assert_eq!(s.samples, want);
    assert_eq!(st.overlap, 1);
}

#[test]
fn audio_after_the_end_of_the_video_is_left_out() {
    // a gap at 40 frames; the video ends before the frames after it
    let mut f = run_of(0, 0, 40);
    f.extend(run_of(40 * N * T + 500 * MS, 40 * N, 20));
    let (s, _) = run(&f, 40 * N * T + 100 * MS);
    assert_eq!(s.samples, (0..40 * N).map(val).collect::<Vec<_>>());
    assert!(kinds(&s).contains(&EV_VIDEO_ENDED));
}
