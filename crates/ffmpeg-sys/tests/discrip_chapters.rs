//! `libavformat/discrip_chapters.c` and the joiner's chapter marks: a
//! title's chapter records (times) become marks; the joiner puts each on the
//! first video key frame at, or up to 0.4 s before, it; the k-th marked
//! frame starts chapter k, which ends where the next starts (the last at the
//! title's end); fewer than two: no chapters.

use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discrip::{
    ff_discrip_chapter_list, ff_discrip_chapter_plan, ff_discrip_chapter_plan_free, ff_discrip_frame_unref,
    ff_discrip_join_chapters, ff_discrip_join_close, ff_discrip_join_finish, ff_discrip_join_open,
    ff_discrip_join_push, ff_discrip_join_segment, Chapter, ChapterPlan, Frame, Join, JoinConfig, CHAPTER_00,
    F_BATCH, F_KEY, F_MARKER, KIND_VIDEO,
};

const S: i64 = 1_080_000_000; // one second

/// The plan of `records` (marks, atoms), or the error.
fn plan(records: &[i64], skip: i64, chapter00: bool) -> Result<(Vec<i64>, Vec<c_int>), c_int> {
    let mut p = ChapterPlan { marks: std::ptr::null_mut(), nb_marks: 0, atoms: std::ptr::null_mut(), nb_atoms: 0 };
    // SAFETY: records is a valid slice; p is freed below.
    unsafe {
        let n = c_int::try_from(records.len()).unwrap();
        let ret = ff_discrip_chapter_plan(std::ptr::null_mut(), records.as_ptr(), n, skip, c_int::from(chapter00), &raw mut p);
        if ret < 0 {
            return Err(ret);
        }
        let v = |ptr: *mut i64, n: c_int| if n == 0 { Vec::new() } else { std::slice::from_raw_parts(ptr, usize::try_from(n).unwrap()).to_vec() };
        let marks = v(p.marks, p.nb_marks);
        let atoms = if p.nb_atoms == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(p.atoms, usize::try_from(p.nb_atoms).unwrap()).to_vec()
        };
        ff_discrip_chapter_plan_free(&raw mut p);
        Ok((marks, atoms))
    }
}

#[test]
fn a_first_record_at_the_start_and_the_snap() {
    // within 0.1 s of the start: at 0, nothing added
    assert_eq!(plan(&[0, 60 * S, 120 * S], 0, true), Ok((vec![0, 60 * S, 120 * S], vec![0, 1, 2])));
    assert_eq!(plan(&[107_999_999, 60 * S], 0, true), Ok((vec![0, 60 * S], vec![0, 1])));
    // from 0.1 s on it is a chapter of its own; one is added at 0 only when asked
    assert_eq!(plan(&[108_000_000, 60 * S], 0, false), Ok((vec![108_000_000, 60 * S], vec![0, 1])));
    assert_eq!(
        plan(&[108_000_000, 60 * S], 0, true),
        Ok((vec![0, 108_000_000, 60 * S], vec![CHAPTER_00, 0, 1]))
    );
}

#[test]
fn records_in_leading_segments_left_out() {
    // 30 s of leading segments left out: record 0 is dropped, the rest move
    // back; record 0 starts the added chapter at 0
    assert_eq!(plan(&[0, 60 * S, 120 * S], 30 * S, false), Ok((vec![0, 30 * S, 90 * S], vec![0, 1, 2])));
    // the first kept record lands within 0.1 s of the start: it is at 0
    assert_eq!(plan(&[0, 30 * S, 120 * S], 30 * S, false), Ok((vec![0, 90 * S], vec![1, 2])));
}

#[test]
fn a_broken_tail_is_cut_and_a_backward_time_refuses_the_title() {
    assert_eq!(plan(&[0, 60 * S, 120 * S, 0], 0, false), Ok((vec![0, 60 * S, 120 * S], vec![0, 1, 2])));
    assert_eq!(plan(&[0, 60 * S, 120 * S, 90 * S], 0, false), Ok((vec![0, 60 * S, 120 * S], vec![0, 1, 2])));
    assert!(plan(&[0, 60 * S, 30 * S, 120 * S], 0, false).is_err());
}

#[test]
fn a_record_at_the_time_of_the_one_before_it_is_dropped() {
    // one chapter per time; its record keeps its name (the next one is not
    // shifted onto it)
    assert_eq!(plan(&[0, 60 * S, 60 * S, 120 * S], 0, false), Ok((vec![0, 60 * S, 120 * S], vec![0, 1, 3])));
}

#[test]
fn no_records_no_chapters() {
    assert_eq!(plan(&[], 0, true), Ok((Vec::new(), Vec::new())));
}

// ---- through the joiner ----

unsafe extern "C" fn drop_out(_: *mut c_void, _: c_int, f: *mut Frame) -> c_int {
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}

fn fr(time: i64, dur: i64, flags: u32, size: c_int) -> Frame {
    Frame { buf: std::ptr::null_mut(), data: std::ptr::null_mut(), size, time, dur, pos: 0, flags, samples: 0, rate: 0 }
}

/// A title of `frames` 25 fps video frames, a key frame every 12, an empty
/// key marker at `marker` (if any); the chapters of `records` (end =
/// `duration`).
fn title(records: &[i64], frames: i64, marker: Option<i64>, duration: i64) -> Vec<Chapter> {
    const F: i64 = S / 25;
    let mut p = ChapterPlan { marks: std::ptr::null_mut(), nb_marks: 0, atoms: std::ptr::null_mut(), nb_atoms: 0 };
    let kinds = [KIND_VIDEO];
    let mut out = Vec::new();
    // SAFETY: every pointer is valid for the calls; all is freed below.
    unsafe {
        let n = c_int::try_from(records.len()).unwrap();
        assert_eq!(ff_discrip_chapter_plan(std::ptr::null_mut(), records.as_ptr(), n, 0, 0, &raw mut p), 0);
        let cfg = JoinConfig {
            nb_tracks: 1,
            kinds: kinds.as_ptr(),
            marks: p.marks,
            nb_marks: p.nb_marks,
            out: drop_out,
            out_opaque: std::ptr::null_mut(),
            event: None,
            event_opaque: std::ptr::null_mut(),
        };
        let mut j: *mut Join = std::ptr::null_mut();
        assert_eq!(ff_discrip_join_open(&raw mut j, std::ptr::null_mut(), &raw const cfg), 0);
        assert_eq!(ff_discrip_join_segment(j), 0);
        for k in 0..frames {
            if marker == Some(k) {
                let mut m = fr(k * F, 0, F_KEY | F_MARKER, 0);
                assert_eq!(ff_discrip_join_push(j, 0, &raw mut m), 0);
            }
            let flags = if k % 12 == 0 { F_KEY } else { 0 } | if k == frames - 1 || k == 0 { F_BATCH } else { 0 };
            let mut f = fr(k * F, F, flags, 1);
            assert_eq!(ff_discrip_join_push(j, 0, &raw mut f), 0);
        }
        assert_eq!(ff_discrip_join_finish(j), 0);
        let mut starts: *const i64 = std::ptr::null();
        let ns = ff_discrip_join_chapters(j, &raw mut starts);
        let mut list: *mut Chapter = std::ptr::null_mut();
        let mut nl: c_int = 0;
        assert_eq!(ff_discrip_chapter_list(&raw const p, starts, ns, duration, &raw mut list, &raw mut nl), 0);
        if nl > 0 {
            out = std::slice::from_raw_parts(list, usize::try_from(nl).unwrap()).to_vec();
        }
        ffmpeg_sys::av_free(list.cast());
        ff_discrip_join_close(&raw mut j);
        ff_discrip_chapter_plan_free(&raw mut p);
    }
    out
}

#[test]
fn chapters_start_at_the_key_frames_that_take_the_marks() {
    // key frames every 0.48 s; marks at 0, 1.0 s, 2.3 s: the key frames at 0,
    // 0.96 s (0.04 s before), 1.92 s (0.38 s before)
    let c = title(&[0, S, 2_300_000_000], 100, None, 4 * S);
    let k = |n: i64| n * 12 * S / 25;
    assert_eq!(
        c,
        vec![
            Chapter { start: 0, end: k(2), record: 0 },
            Chapter { start: k(2), end: k(4), record: 1 },
            Chapter { start: k(4), end: 4 * S, record: 2 },
        ]
    );
}

#[test]
fn a_mark_without_a_key_frame_after_it_is_no_chapter() {
    // the second mark lies past the last key frame: one chapter only = none
    assert!(title(&[0, 10 * S], 100, None, 4 * S).is_empty());
}

#[test]
fn an_empty_marker_takes_no_chapter() {
    // an empty key marker (a placeholder) at 0.92 s, within 0.4 s of the
    // mark at 0.96 s: the mark goes on the real key frame at 0.96 s
    let c = title(&[0, 960_000_000], 100, Some(23), 4 * S);
    assert_eq!(c[1].start, 24 * S / 25);
    assert_eq!(c.len(), 2);
}
