//! `libavformat/discrip_junction.c`: the audio junction of the rip core.
//! Every case is built from an event the reference program logged on a real
//! disc, with the numbers in ticks (1/1,080,000,000 s): the title start
//! (lead-in kept as a shift, or frames dropped beyond the format's tolerance),
//! overlaps growing the skew, short gaps absorbed, real gaps (skew reset,
//! nothing inserted), whole-frame drops only up to a sync unit.

use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discrip::{
    ff_discrip_frame_unref, ff_discrip_junction_close, ff_discrip_junction_finish, ff_discrip_junction_open,
    ff_discrip_junction_push, ff_discrip_junction_stats, Event, Frame, Junction, JunctionConfig, JunctionStats,
    VideoRef, EV_DROP, EV_GAP, EV_GAP_ABSORBED, EV_GAP_MARKER, EV_OVERLAP, EV_START_DROP, EV_START_GAP,
    EV_START_SHIFT, EV_VIDEO_ENDED, F_KEY, F_MARKER, F_SYNC,
};

const MS: i64 = 1_080_000;
const HDDVD_TOL: i64 = 110_160_000; // 102 ms
const AC3: i64 = 34_560_000; // 1536 samples at 48 kHz = 32 ms

/// Video seen by the junction: handed on up to `max`, ends at `end`.
struct Video {
    max: i64,
    end: i64,
}

unsafe extern "C" fn v_max(o: *mut c_void) -> i64 {
    // SAFETY: o is the &mut Video of the run.
    unsafe { (*o.cast::<Video>()).max }
}
unsafe extern "C" fn v_advance(o: *mut c_void, target: i64, ended: *mut c_int) -> c_int {
    // SAFETY: o is the &mut Video of the run, ended a valid pointer.
    let v = unsafe { &mut *o.cast::<Video>() };
    v.max = v.max.max(target.min(v.end));
    // SAFETY: valid out pointer.
    unsafe { *ended = c_int::from(v.max < target) };
    0
}

struct Sink {
    out: Vec<(i64, i64, u32)>, // time, dur, flags
    events: Vec<Event>,
}

unsafe extern "C" fn on_out(o: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: o is the &mut Sink of the run; f a frame we own.
    let (s, fr) = unsafe { (&mut *o.cast::<Sink>(), &mut *f) };
    s.out.push((fr.time, fr.dur, fr.flags));
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}
unsafe extern "C" fn on_event(o: *mut c_void, e: *const Event) {
    // SAFETY: o is the &mut Sink of the run; e valid during the call.
    unsafe { (*o.cast::<Sink>()).events.push(*e) };
}

/// Input frame: time, duration, sync unit.
fn fr(time: i64, dur: i64, sync: bool) -> Frame {
    Frame {
        buf: std::ptr::null_mut(),
        data: std::ptr::null_mut(),
        size: 1,
        time,
        dur,
        pos: 0,
        flags: if sync { F_KEY | F_SYNC } else { 0 },
        samples: 0,
        rate: 0,
    }
}

/// Contiguous frames of `dur` from `start`.
fn run_of(start: i64, dur: i64, n: usize, sync: bool) -> Vec<Frame> {
    (0..n).map(|k| fr(start + i64::try_from(k).unwrap() * dur, dur, sync)).collect()
}

fn junction(frames: Vec<Frame>, frame_dur: i64, tol: i64, video: &mut Video) -> (Sink, JunctionStats) {
    let mut sink = Sink { out: Vec::new(), events: Vec::new() };
    let cfg = JunctionConfig {
        track: 1,
        frame_dur,
        tolerance: tol,
        video: VideoRef { opaque: std::ptr::from_mut(video).cast(), max_time: v_max, advance: v_advance },
        out: on_out,
        out_opaque: (&raw mut sink).cast(),
        event: Some(on_event),
        event_opaque: (&raw mut sink).cast(),
    };
    let mut j: *mut Junction = std::ptr::null_mut();
    let mut st = JunctionStats::default();
    // SAFETY: cfg, sink and video outlive the junction; it is closed below.
    unsafe {
        assert_eq!(ff_discrip_junction_open(&raw mut j, std::ptr::null_mut(), &raw const cfg), 0);
        for mut f in frames {
            assert_eq!(ff_discrip_junction_push(j, &raw mut f), 0);
        }
        assert_eq!(ff_discrip_junction_finish(j), 0);
        ff_discrip_junction_stats(j, &raw mut st);
        ff_discrip_junction_close(&raw mut j);
    }
    (sink, st)
}

fn video() -> Video {
    Video { max: 0, end: i64::MAX }
}

fn kinds(s: &Sink) -> Vec<i32> {
    s.events.iter().map(|e| e.kind).collect()
}

#[test]
fn lead_in_within_the_tolerance_is_kept_and_the_whole_track_delayed() {
    // Patch Adams t00 (HD DVD): first audio frame 1.266 ms before the video
    let lead = 1_367_280;
    let (s, st) = junction(run_of(-lead, AC3, 20, true), AC3, HDDVD_TOL, &mut video());
    assert_eq!(s.out.len(), 20);
    assert_eq!(s.out[0].0, 0, "the first frame leaves at 0");
    assert!(s.out.iter().enumerate().all(|(k, o)| o.0 == i64::try_from(k).unwrap() * AC3));
    assert_eq!(s.events, vec![Event { kind: EV_START_SHIFT, track: 1, pos: 0, dur: lead, skew: lead, count: 0 }]);
    assert_eq!((st.skew, st.base), (lead, lead));
}

#[test]
fn overlap_grows_the_skew_and_short_gaps_are_absorbed() {
    // Patch Adams t00: lead-in 1.266 ms; at 22.66 s a frame 26.666 ms early
    // (skew 27.933 ms); later gaps of 1.166 ms and 4.166 ms are absorbed
    let lead = 1_367_280;
    let ov = 28_799_280; // 26.666 ms
    let g1 = 1_259_280; // 1.166 ms
    let g2 = 4_499_280; // 4.166 ms
    let mut frames = run_of(-lead, AC3, 10, true);
    let mut t = -lead + 10 * AC3 - ov;
    frames.extend(run_of(t, AC3, 10, true));
    t += 10 * AC3 + g1;
    frames.extend(run_of(t, AC3, 10, true));
    t += 10 * AC3 + g2;
    frames.extend(run_of(t, AC3, 10, true));
    let (s, st) = junction(frames, AC3, HDDVD_TOL, &mut video());
    assert_eq!(kinds(&s), vec![EV_START_SHIFT, EV_OVERLAP, EV_GAP_ABSORBED, EV_GAP_ABSORBED]);
    assert_eq!(s.events[1].dur, ov);
    assert_eq!(s.events[1].skew, lead + ov, "27.933 ms");
    assert_eq!(s.events[2].skew, lead + ov - g1, "26.766 ms");
    assert_eq!(s.events[3].skew, lead + ov - g1 - g2, "22.6 ms");
    assert_eq!(s.out.len(), 40, "no frame dropped: 32 ms + 1.266 ms > 27.933 ms");
    assert_eq!(st.dropped, 0);
    // frames leave back to back except where the skew changed
    assert!(s.out.windows(2).all(|w| w[1].0 >= w[0].0));
}

#[test]
fn a_real_gap_resets_the_skew_and_inserts_nothing() {
    // Patch Adams t01: E-AC-3 frames of 256 samples (5.333 ms); a 56.033 ms
    // gap -> "audio gap - 10.506 missing frame(s)"
    let eac3 = 5_760_000;
    let gap = 60_515_640; // 56.033 ms
    let mut frames = run_of(0, eac3, 30, true);
    frames.extend(run_of(30 * eac3 + gap, eac3, 30, true));
    let (s, st) = junction(frames, eac3, HDDVD_TOL, &mut video());
    assert_eq!(kinds(&s), vec![EV_GAP]);
    let e = s.events[0];
    assert_eq!((e.pos, e.dur, e.skew), (30 * eac3, gap, 0));
    assert_eq!(e.count, 10_506, "10.506 missing frames (x1000)");
    assert_eq!(s.out.len(), 60);
    assert_eq!(s.out[30].0, 30 * eac3 + gap, "the gap stays a gap");
    assert_eq!(st.skew, 0);
}

#[test]
fn a_large_overlap_drops_whole_frames_up_to_a_sync_unit() {
    // DVDDEMYSTIFIED2 (DVD): a 2942.633 ms overlap on 32 ms AC-3 frames ->
    // "92 frame(s) dropped to reduce audio skew to -1.366ms"
    let ov = 3_178_043_640; // 2942.633 ms
    let mut frames = run_of(0, AC3, 200, true);
    frames.extend(run_of(200 * AC3 - ov, AC3, 200, true));
    let (s, st) = junction(frames, AC3, MS, &mut video());
    assert_eq!(kinds(&s), vec![EV_OVERLAP, EV_DROP]);
    let d = s.events[1];
    assert_eq!(d.count, 92, "92 x 32 ms = 2944 ms <= 2942.633 + 2 ms");
    assert_eq!(d.dur, 92 * AC3);
    assert_eq!(d.skew, ov - 92 * AC3, "-1.367 ms");
    assert_eq!(st.dropped, 92);
    assert_eq!(s.out.len(), 400 - 92);
}

#[test]
fn frames_are_never_dropped_when_no_sync_unit_follows() {
    let ov = 10 * AC3;
    let mut frames = run_of(0, AC3, 20, false);
    frames.extend(run_of(20 * AC3 - ov, AC3, 20, false));
    let (s, st) = junction(frames, AC3, MS, &mut video());
    assert_eq!(kinds(&s), vec![EV_OVERLAP]);
    assert_eq!(st.dropped, 0);
    assert_eq!(st.skew, ov);
}

#[test]
fn a_dvd_lead_in_beyond_1_ms_drops_frames_before_one_frame_of_lead() {
    // SAINTBEAST (DVD, tolerance 1 ms): "6 frame(s) dropped to reduce audio
    // skew to +24.133ms", then the overlapping-frame line
    let rest = 26_063_640; // 24.133 ms
    let first = -(6 * AC3 + rest);
    let (s, st) = junction(run_of(first, AC3, 20, true), AC3, 0, &mut video());
    assert_eq!(kinds(&s), vec![EV_START_DROP, EV_START_SHIFT]);
    assert_eq!((s.events[0].count, s.events[0].dur, s.events[0].skew), (6, 6 * AC3, rest));
    assert_eq!(s.events[1].dur, 6 * AC3 + rest);
    assert_eq!(s.events[1].skew, rest);
    assert_eq!(s.out.len(), 14);
    assert_eq!(s.out[0].0, 0);
    assert_eq!((st.dropped, st.base), (6, rest));
}

#[test]
fn a_lead_in_inside_one_frame_reports_zero_frames_dropped_on_dvd_and_bd() {
    // UHD Resident Evil (tolerance 1 ms): "0 frame(s) dropped to reduce audio
    // skew to +8ms" then "overlapping frame ... +8ms"
    let lead = 8 * MS;
    let (s, _) = junction(run_of(-lead, AC3, 5, true), AC3, 0, &mut video());
    assert_eq!(kinds(&s), vec![EV_START_DROP, EV_START_SHIFT]);
    assert_eq!((s.events[0].count, s.events[0].skew), (0, lead));
    assert_eq!(s.out.len(), 5);
    // the same lead on HD DVD (102 ms): only the shift
    let (s, _) = junction(run_of(-lead, AC3, 5, true), AC3, HDDVD_TOL, &mut video());
    assert_eq!(kinds(&s), vec![EV_START_SHIFT]);
}

#[test]
fn a_track_starting_up_to_3_ms_after_the_video_keeps_its_delay() {
    let late = 2 * MS;
    let (s, st) = junction(run_of(late, AC3, 5, true), AC3, HDDVD_TOL, &mut video());
    assert_eq!(kinds(&s), vec![EV_START_GAP]);
    assert_eq!(s.out[0].0, late);
    assert_eq!(st.skew, 0);
    let (s, _) = junction(run_of(5 * MS, AC3, 5, true), AC3, HDDVD_TOL, &mut video());
    assert!(s.events.is_empty(), "later than 3 ms: kept, no event");
}

#[test]
fn a_long_gap_while_the_video_goes_on_gets_markers_every_second() {
    // ANGEL_S5D1 (DVD): 11-12 s gaps -> markers, then the gap line
    let gap = 11 * 1_080_000_000;
    let mut frames = run_of(0, AC3, 10, true);
    frames.extend(run_of(10 * AC3 + gap, AC3, 10, true));
    let mut v = Video { max: 0, end: i64::MAX };
    let (s, _) = junction(frames, AC3, MS, &mut v);
    let markers = s.events.iter().filter(|e| e.kind == EV_GAP_MARKER).count();
    assert!(markers >= 6, "{markers} markers");
    assert_eq!(*kinds(&s).last().unwrap(), EV_GAP);
    assert_eq!(s.out.iter().filter(|o| o.2 & F_MARKER != 0).count(), markers);
    assert_eq!(s.out.iter().filter(|o| o.2 & F_MARKER == 0).count(), 20, "markers carry no audio");
}

#[test]
fn audio_after_the_end_of_the_video_is_left_out() {
    let gap = 2 * 1_080_000_000;
    let mut frames = run_of(0, AC3, 10, true);
    frames.extend(run_of(10 * AC3 + gap, AC3, 10, true));
    let mut v = Video { max: 0, end: 10 * AC3 };
    let (s, st) = junction(frames, AC3, MS, &mut v);
    assert_eq!(kinds(&s), vec![EV_VIDEO_ENDED]);
    assert_eq!(s.out.len(), 10);
    assert_eq!(st.ended, 1);
}
