//! The seamless overlap search of the rip core's junction
//! (`discrip_seamless.c`, `discrip_junction.c`): `TrueHD` units decoded to one
//! channel of 32-bit samples, the integer correlation of two stretches, and
//! a join whose next segment repeats the last two access units of the one
//! before it. The `TrueHD` stream is `tests/data/sine_4813.thd` (FFmpeg's
//! encoder, 16-bit stereo, a major sync every 16 access units).

use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discrip::{
    ff_discrip_frame_unref, ff_discrip_junction_close, ff_discrip_junction_finish, ff_discrip_junction_open,
    ff_discrip_junction_push, ff_discrip_junction_stats, ff_discrip_seamless_corr, ff_discrip_seamless_decode,
    Event, Frame, Junction, JunctionConfig, JunctionStats, VideoRef, EV_DROP, EV_SEAMLESS_DROP, EV_SEAMLESS_SEARCH, F_KEY,
    F_SYNC, F_TAIL,
};

const THD: &[u8] = include_bytes!("data/sine_4813.thd");
const AU: i64 = 900_000; // 40 samples at 48 kHz

fn truehd_id() -> c_int {
    ffmpeg_sys::discrip::codec_id(c"truehd").unwrap()
}

/// The access units (bytes, major sync).
fn units() -> Vec<(&'static [u8], bool)> {
    let mut v = Vec::new();
    let mut o = 0;
    while o + 8 <= THD.len() {
        let len = ((usize::from(THD[o] & 0xF) << 8) | usize::from(THD[o + 1])) * 2;
        v.push((&THD[o..o + len], THD[o + 4..o + 8] == [0xF8, 0x72, 0x6F, 0xBA]));
        o += len;
    }
    v
}

fn frame(data: &[u8], time: i64, flags: u32) -> Frame {
    Frame {
        buf: std::ptr::null_mut(),
        data: data.as_ptr().cast_mut(),
        size: c_int::try_from(data.len()).unwrap(),
        time,
        dur: AU,
        pos: 0,
        flags,
        samples: 0,
        rate: 0,
        src: time,
    }
}

/// `ff_discrip_seamless_decode` of units `range` after `pre`.
fn decode(pre: &[usize], range: &[usize]) -> Option<Vec<i32>> {
    let all = units();
    let mk = |ix: &[usize]| -> Vec<Frame> { ix.iter().map(|&k| frame(all[k].0, 0, 0)).collect() };
    let (p, r) = (mk(pre), mk(range));
    let mut out: *mut i32 = std::ptr::null_mut();
    let mut n: c_int = 0;
    // SAFETY: the frames point into THD; out is freed below.
    unsafe {
        let ret = ff_discrip_seamless_decode(
            std::ptr::null_mut(),
            truehd_id(),
            48_000,
            p.as_ptr(),
            c_int::try_from(p.len()).unwrap(),
            r.as_ptr(),
            c_int::try_from(r.len()).unwrap(),
            &raw mut out,
            &raw mut n,
        );
        assert!(ret >= 0);
        if ret == 0 {
            return None;
        }
        let v = std::slice::from_raw_parts(out, usize::try_from(n).unwrap()).to_vec();
        ffmpeg_sys::av_free(out.cast());
        Some(v)
    }
}

fn corr(a: &[i32], b: &[i32]) -> u32 {
    let n = u32::try_from(a.len().min(b.len())).unwrap();
    // SAFETY: both hold at least n samples.
    unsafe { ff_discrip_seamless_corr(a.as_ptr(), b.as_ptr(), n) }
}

/// The Pearson correlation in floating point.
fn pearson(a: &[i32], b: &[i32]) -> f64 {
    let n = a.len().min(b.len());
    let (a, b) = (&a[..n], &b[..n]);
    let len = f64::from(u32::try_from(n).unwrap());
    let ma = a.iter().map(|&x| f64::from(x)).sum::<f64>() / len;
    let mb = b.iter().map(|&x| f64::from(x)).sum::<f64>() / len;
    let cov: f64 = a.iter().zip(b).map(|(&x, &y)| (f64::from(x) - ma) * (f64::from(y) - mb)).sum();
    let va: f64 = a.iter().map(|&x| (f64::from(x) - ma).powi(2)).sum();
    let vb: f64 = b.iter().map(|&y| (f64::from(y) - mb).powi(2)).sum();
    cov / (va * vb).sqrt()
}

#[test]
fn units_decode_to_one_channel_and_a_preroll_is_dropped() {
    let u = units();
    let syncs: Vec<usize> = (0..u.len()).filter(|&k| u[k].1).collect();
    assert!(syncs.len() >= 3 && syncs[0] == 0);
    let all: Vec<usize> = (0..120).collect();
    let whole = decode(&[], &all).unwrap();
    assert_eq!(whole.len(), 120 * 40);
    assert!(whole.iter().any(|&s| s.abs() > 1 << 24), "16-bit samples are scaled to 32 bits");
    // from inside a group of units: the units since its major sync first
    let m = syncs[1];
    let pre: Vec<usize> = (m..m + 3).collect();
    let range: Vec<usize> = (m + 3..m + 10).collect();
    assert_eq!(decode(&pre, &range).unwrap(), whole[(m + 3) * 40..(m + 10) * 40]);
    // without them the decoder cannot start there
    assert!(decode(&[], &range).is_none());
}

#[test]
fn the_integer_correlation() {
    let whole = decode(&[], &(0..120).collect::<Vec<usize>>()).unwrap();
    let a = &whole[400..800];
    assert_eq!(corr(a, a), 0xFFFF_FFFF, "identical");
    let neg: Vec<i32> = a.iter().map(|&x| -x).collect();
    assert_eq!(corr(a, &neg), 1, "opposite");
    let quiet: Vec<i32> = a.iter().map(|&x| (x >> 26).clamp(-99, 99)).collect();
    assert_eq!(corr(&quiet, &quiet), 0, "silence");
    // shifted stretches of the sine: as the floating-point Pearson value
    for shift in [1, 7, 20, 40, 55] {
        let b = &whole[400 + shift..800 + shift];
        let want = pearson(a, b);
        let got = f64::from(corr(a, b)) / 4_294_967_296.0;
        if want > 0.0 {
            assert!((got - want).abs() < 1e-6, "shift {shift}: {got} against {want}");
        } else {
            assert_eq!(corr(a, b), 1, "shift {shift}: no positive correlation");
        }
    }
}

// ---- a join ----

unsafe extern "C" fn v_max(_: *mut c_void) -> i64 {
    i64::MAX
}
unsafe extern "C" fn v_advance(_: *mut c_void, _: i64, ended: *mut c_int) -> c_int {
    // SAFETY: valid out pointer.
    unsafe { *ended = 0 };
    0
}

#[derive(Default)]
struct Sink {
    out: Vec<(i64, usize)>, // time, size
    events: Vec<Event>,
}

unsafe extern "C" fn on_out(o: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: o is the &mut Sink; f a frame we own.
    let (s, fr) = unsafe { (&mut *o.cast::<Sink>(), &mut *f) };
    s.out.push((fr.time, usize::try_from(fr.size).unwrap()));
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}
unsafe extern "C" fn on_event(o: *mut c_void, e: *const Event) {
    // SAFETY: o is the &mut Sink; e valid during the call.
    unsafe { (*o.cast::<Sink>()).events.push(*e) };
}

/// Segment A = units [0, m + 2) (its last 10 read from the segment's tail;
/// unit m there is a copy without its sync flag), segment B = units [m, 100)
/// starting at unit m's time: the next segment repeats A's last two units.
fn join(codec: c_int) -> (Sink, JunctionStats) {
    let u = units();
    let m = (1..u.len()).find(|&k| u[k].1 && k >= 40).unwrap();
    let mut frames = Vec::new();
    for (k, &(data, sync)) in u.iter().enumerate().take(m + 2) {
        let t = i64::try_from(k).unwrap() * AU;
        let mut flags = F_KEY;
        if sync && k < m {
            flags |= F_SYNC;
        }
        if k + 10 >= m + 2 {
            flags |= F_TAIL;
        }
        frames.push(frame(data, t, flags));
    }
    for (k, &(data, sync)) in u.iter().enumerate().take(100).skip(m) {
        let t = i64::try_from(k).unwrap() * AU;
        frames.push(frame(data, t, F_KEY | if sync { F_SYNC } else { 0 }));
    }
    let mut sink = Sink::default();
    let cfg = JunctionConfig {
        track: 1,
        frame_dur: AU,
        tolerance: 0,
        video: VideoRef { opaque: std::ptr::null_mut(), max_time: v_max, advance: v_advance },
        out: on_out,
        out_opaque: (&raw mut sink).cast(),
        event: Some(on_event),
        event_opaque: (&raw mut sink).cast(),
        codec,
        rate: 48_000,
    };
    let mut j: *mut Junction = std::ptr::null_mut();
    let mut st = JunctionStats::default();
    // SAFETY: cfg and sink outlive the junction; it is closed below.
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

#[test]
fn duplicated_units_at_a_join_are_found_and_dropped() {
    let u = units();
    let m = (1..u.len()).find(|&k| u[k].1 && k >= 40).unwrap();
    let (s, st) = join(truehd_id());
    // A's two repeated units dropped: every unit once, back to back
    assert_eq!(st.dropped, 2);
    assert_eq!(s.out.len(), 100);
    assert!(s.out.iter().enumerate().all(|(k, o)| o.0 == i64::try_from(k).unwrap() * AU));
    let search: Vec<&Event> = s.events.iter().filter(|e| e.kind == EV_SEAMLESS_SEARCH).collect();
    assert_eq!(search.len(), 1);
    assert_eq!((search[0].count, search[0].dur, search[0].skew), (0xFFFF_FFFF, 2, 1));
    let drop: Vec<&Event> = s.events.iter().filter(|e| e.kind == EV_SEAMLESS_DROP).collect();
    assert_eq!(drop.len(), 1);
    assert_eq!((drop[0].count, drop[0].dur, drop[0].skew, drop[0].pos), (2, 2 * AU, 0, i64::try_from(m).unwrap() * AU));
}

#[test]
fn without_decoding_the_join_takes_the_duration_rule() {
    // the same join, not compared: one unit re-timed to end at the next
    // segment's major sync, the overlap then cut by the skew rule, which here
    // drops the same two units (as on the discs where only this rule ran)
    let (s, st) = join(0);
    assert!(s.events.iter().any(|e| e.kind == EV_SEAMLESS_SEARCH && e.count == 0 && e.skew == 0));
    assert!(!s.events.iter().any(|e| e.kind == EV_SEAMLESS_DROP));
    assert!(s.events.iter().any(|e| e.kind == EV_DROP && e.count == 2), "{:?}", s.events);
    assert_eq!(st.dropped, 2);
}
