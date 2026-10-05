//! `libavformat/discrip_join.c`: the rip core's joiner. Segment 0 starts the
//! title timeline at its first video time (the earliest of the video timing's
//! first batch); segment k is placed where the video of segment k-1 really
//! ended. Audio without a time takes where its track is; audio or subtitles
//! early by more than their duration are moved there; video keeps its times;
//! chapter marks go on the video key frame at or up to 0.4 s before them.

use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discrip::{
    ff_discrip_frame_unref, ff_discrip_join_close, ff_discrip_join_finish, ff_discrip_join_open,
    ff_discrip_join_push, ff_discrip_join_segment, ff_discrip_join_stats, Event, Frame, Join, JoinConfig, JoinStats,
    EV_RETIME, F_BATCH, F_CHAPTER, F_KEY, KIND_AUDIO, KIND_SUBTITLE, KIND_VIDEO,
};
use ffmpeg_sys::AV_NOPTS_VALUE;

const FRAME: i64 = 36_036_000; // 1001/30000 s
const AC3: i64 = 34_560_000;

struct Sink {
    out: Vec<(c_int, i64, u32)>, // track, time, flags
    events: Vec<Event>,
}

unsafe extern "C" fn on_out(o: *mut c_void, track: c_int, f: *mut Frame) -> c_int {
    // SAFETY: o is the &mut Sink; f a frame we own.
    let (s, fr) = unsafe { (&mut *o.cast::<Sink>(), &mut *f) };
    s.out.push((track, fr.time, fr.flags));
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}
unsafe extern "C" fn on_event(o: *mut c_void, e: *const Event) {
    // SAFETY: o is the &mut Sink; e valid during the call.
    unsafe { (*o.cast::<Sink>()).events.push(*e) };
}

fn fr(time: i64, dur: i64, flags: u32) -> Frame {
    Frame { buf: std::ptr::null_mut(), data: std::ptr::null_mut(), size: 1, time, dur, pos: 0, flags }
}

/// Segments of (track, frame) in input order.
fn join(kinds: &[c_int], marks: &[i64], segments: Vec<Vec<(c_int, Frame)>>) -> Result<(Sink, JoinStats), c_int> {
    let mut sink = Sink { out: Vec::new(), events: Vec::new() };
    let cfg = JoinConfig {
        nb_tracks: c_int::try_from(kinds.len()).unwrap(),
        kinds: kinds.as_ptr(),
        marks: marks.as_ptr(),
        nb_marks: c_int::try_from(marks.len()).unwrap(),
        out: on_out,
        out_opaque: (&raw mut sink).cast(),
        event: Some(on_event),
        event_opaque: (&raw mut sink).cast(),
    };
    let mut j: *mut Join = std::ptr::null_mut();
    let mut st = JoinStats::default();
    // SAFETY: cfg and sink outlive the joiner; it is closed below.
    unsafe {
        assert_eq!(ff_discrip_join_open(&raw mut j, std::ptr::null_mut(), &raw const cfg), 0);
        let mut ret = 0;
        'all: for seg in segments {
            ret = ff_discrip_join_segment(j);
            if ret < 0 {
                break;
            }
            for (t, mut f) in seg {
                ret = ff_discrip_join_push(j, t, &raw mut f);
                if ret < 0 {
                    break 'all;
                }
            }
        }
        if ret >= 0 {
            ret = ff_discrip_join_finish(j);
        }
        ff_discrip_join_stats(j, &raw mut st);
        ff_discrip_join_close(&raw mut j);
        if ret < 0 {
            return Err(ret);
        }
    }
    Ok((sink, st))
}

/// Video frames of a GOP in decode order I P B B ... from display position
/// `first` (presentation times base + position x FRAME), the last one ending
/// the batch.
fn gop(base: i64, first: i64, n: i64) -> Vec<(c_int, Frame)> {
    let mut v = vec![(0, fr(base + (first + 1) * FRAME, FRAME, F_KEY))];
    for k in 0..n - 1 {
        v.push((0, fr(base + (first + k + i64::from(k != 0)) * FRAME, FRAME, 0)));
    }
    v.last_mut().unwrap().1.flags |= F_BATCH;
    v
}

fn track_times(s: &Sink, track: c_int) -> Vec<i64> {
    s.out.iter().filter(|o| o.0 == track).map(|o| o.1).collect()
}

#[test]
fn segments_follow_where_the_previous_video_really_ended() {
    // segment 0: video times 1000 s.. (decode order: the I frame is displayed
    // after the first B frame, so the start is the batch's earliest time);
    // segment 1 restarts its own clock at 5 s
    let b0 = 1000 * 1_080_000_000;
    let b1 = 5 * 1_080_000_000;
    let mut s0 = gop(b0, 0, 12);
    s0.extend((0..10).map(|k| (1, fr(b0 + k * AC3, AC3, F_KEY))));
    let mut s1 = gop(b1, 0, 12);
    s1.extend((0..10).map(|k| (1, fr(b1 + k * AC3, AC3, F_KEY))));
    let (s, st) = join(&[KIND_VIDEO, KIND_AUDIO], &[], vec![s0, s1]).unwrap();
    let v = track_times(&s, 0);
    assert_eq!(v[1], 0, "segment 0 starts at its earliest video time");
    assert_eq!(*v.iter().min().unwrap(), 0);
    let end0 = 12 * FRAME; // the video of segment 0 ended here on the title timeline
    assert_eq!(st.offset, end0);
    assert_eq!(*v[12..].iter().min().unwrap(), end0);
    let a = track_times(&s, 1);
    assert_eq!(a[0], 0);
    assert_eq!(a[10], end0, "segment 1's audio follows its own video");
    assert_eq!(st.segments, 2);
}

#[test]
fn audio_without_a_time_takes_where_the_track_is() {
    let mut s0 = gop(0, 0, 4);
    s0.push((1, fr(AV_NOPTS_VALUE, AC3, F_KEY)));
    s0.push((1, fr(AV_NOPTS_VALUE, AC3, F_KEY)));
    s0.push((1, fr(5 * AC3, AC3, F_KEY))); // first timed frame: the two before it end there
    s0.push((1, fr(AV_NOPTS_VALUE, AC3, F_KEY)));
    let (s, _) = join(&[KIND_VIDEO, KIND_AUDIO], &[], vec![s0]).unwrap();
    assert_eq!(track_times(&s, 1), vec![3 * AC3, 4 * AC3, 5 * AC3, 6 * AC3]);
}

#[test]
fn audio_early_by_more_than_its_duration_is_moved_and_reported_once() {
    let mut s0 = gop(0, 0, 4);
    for k in 0..5 {
        s0.push((1, fr(k * AC3, AC3, F_KEY)));
    }
    s0.push((1, fr(5 * AC3 - 2 * AC3, AC3, F_KEY))); // 2 frames early: moved to 5 x AC3
    s0.push((1, fr(6 * AC3 - 2 * AC3, AC3, F_KEY))); // same value again: moved, not reported
    s0.push((1, fr(7 * AC3 - AC3 / 2, AC3, F_KEY))); // half a frame early: passes unchanged
    let (s, st) = join(&[KIND_VIDEO, KIND_AUDIO], &[], vec![s0]).unwrap();
    let a = track_times(&s, 1);
    assert_eq!(a[5..], [5 * AC3, 6 * AC3, 7 * AC3 - AC3 / 2]);
    assert_eq!(s.events.len(), 1);
    assert_eq!((s.events[0].kind, s.events[0].pos, s.events[0].skew), (EV_RETIME, 5 * AC3, -2 * AC3));
    assert_eq!(st.retimed, 2);
}

#[test]
fn subtitles_are_moved_silently() {
    let mut s0 = gop(0, 0, 4);
    s0.push((1, fr(10 * FRAME, 3 * FRAME, F_KEY)));
    s0.push((1, fr(8 * FRAME, FRAME, F_KEY)));
    let (s, _) = join(&[KIND_VIDEO, KIND_SUBTITLE], &[], vec![s0]).unwrap();
    assert_eq!(track_times(&s, 1), vec![10 * FRAME, 13 * FRAME]);
    assert!(s.events.is_empty());
}

#[test]
fn chapter_marks_go_on_the_key_frame_at_or_just_before_them() {
    // key frames every 12 frames; marks at 0, at 1 s, at 2.1 s
    let mut s0 = Vec::new();
    for g in 0..8 {
        s0.extend(gop(0, g * 12, 12));
    }
    let marks = [0, 1_080_000_000, 2_268_000_000];
    let (s, st) = join(&[KIND_VIDEO], &marks, vec![s0]).unwrap();
    let ch: Vec<i64> = s.out.iter().filter(|o| o.2 & F_CHAPTER != 0).map(|o| o.1).collect();
    // key frames at display positions 1, 13, 25, ... (x FRAME): the first one
    // >= mark - 0.4 s
    assert_eq!(ch, vec![FRAME, 25 * FRAME, 61 * FRAME]);
    assert_eq!(st.chapters, 3);
}

#[test]
fn a_segment_without_video_fails() {
    let s0 = gop(0, 0, 4);
    let s1 = vec![(1, fr(0, AC3, F_KEY))];
    assert!(join(&[KIND_VIDEO, KIND_AUDIO], &[], vec![s0, s1]).is_err());
}
