//! `libavformat/dvdvideo_titles.c`: the title plan, on discs built byte by
//! byte: one title per distinct program chain of a title's parts of title
//! ("1", "1/1"), chapters placed in the segments, segments split at a
//! discontinuity, the cell list from a navigation-scan result (cell walk) or
//! by trimming, titles with a fake length or below the minimum length listed
//! but not selected, duplicates dropped, the title order, one title per angle.

#![allow(clippy::many_single_char_names, reason = "cells keep short names")]

use std::os::raw::{c_char, c_int};

use ffmpeg_sys::dvdvideo::{
    ff_dvdvideo_titles_free, ff_dvdvideo_titles_plan, plan_from_c, Plan, PlanC, ScanC, ScanResultC, TitleOptions,
    CELLS_AUTO, CELLS_WALK, ORDER_AUTO, ORDER_TABLE, TITLE_FAKE, TITLE_SHORT,
};

mod common;
use common::dvd::{cell_navs, flags, time, Cell, Nav, OpenDisc, Pgc, Title, Vts};

/// A cell of `vobus` VOBUs of 10 blocks from block `first`, `secs` seconds;
/// its NAV packs (times from `t0`, 0.5 s per VOBU) are added to `navs`.
fn cell(first: u32, vobus: u32, secs: u32, navs: &mut Vec<Nav>, t0: &mut u32) -> Cell {
    let starts: Vec<u32> = (0..vobus).map(|k| first + 10 * k).collect();
    navs.extend(cell_navs(&starts, first + 10 * vobus, *t0));
    *t0 += 45_000 * vobus;
    Cell::new(first, first + 10 * (vobus - 1), first + 10 * vobus - 1, secs)
}

fn vobus(navs: &[Nav]) -> Vec<u32> {
    let mut v: Vec<u32> = navs.iter().map(|n| n.lbn).collect();
    v.sort_unstable();
    v
}

/// A scan with one result per (title, pgcn, cells).
struct TestScan {
    _cells: Vec<Vec<u8>>,
    _results: Vec<ScanResultC>,
    c: ScanC,
}

fn scan(results: &[(i32, i32, &[u8])]) -> TestScan {
    let mut cells: Vec<Vec<u8>> = results.iter().map(|r| r.2.to_vec()).collect();
    let mut rs: Vec<ScanResultC> = results
        .iter()
        .zip(cells.iter_mut())
        .map(|(r, c)| {
            let mut name = [0 as c_char; 16];
            name[0] = c_char::try_from(b'A').unwrap();
            name[1] = c_char::try_from(b'1').unwrap();
            ScanResultC { name, title: r.0, pgcn: r.1, cells: c.as_mut_ptr(), nb_cells: c_int::try_from(c.len()).unwrap() }
        })
        .collect();
    let c = ScanC {
        results: rs.as_mut_ptr(),
        nb_results: c_int::try_from(rs.len()).unwrap(),
        failed: 0,
        failure: [0; 512],
        entered: [0; 100],
    };
    TestScan { _cells: cells, _results: rs, c }
}

fn plan(d: &OpenDisc, scan: Option<&TestScan>, cell_mode: c_int, title_order: c_int, min_length: c_int) -> Plan {
    let opt = TitleOptions { cell_mode, title_order, min_length };
    let mut p: *mut PlanC = std::ptr::null_mut();
    // SAFETY: an open disc; the scan (if any) lives across the call.
    unsafe {
        let s = scan.map_or(std::ptr::null(), |s| &raw const s.c);
        assert!(ff_dvdvideo_titles_plan(std::ptr::null_mut(), d.disc, s, &raw const opt, &raw mut p) >= 0);
        let out = plan_from_c(&*p, None);
        ff_dvdvideo_titles_free(&raw mut p);
        out
    }
}

fn names(p: &Plan) -> Vec<&str> {
    p.titles.iter().map(|t| t.name.as_str()).collect()
}

fn events<'a>(p: &'a Plan, name: &str) -> Vec<&'a str> {
    p.events.iter().filter(|e| e.0 == name).map(|e| e.1.as_str()).collect()
}

#[test]
fn a_title_its_chapters_and_segments() {
    let (mut navs, mut t0) = (Vec::new(), 0);
    let cells = vec![
        cell(0, 4, 2, &mut navs, &mut t0),
        cell(40, 4, 2, &mut navs, &mut t0),
        // a discontinuity starts a new segment
        cell(80, 4, 2, &mut navs, &mut t0).flags(flags::STC_DISCONTINUITY),
    ];
    let pgc = Pgc { programs: vec![1, 2, 3], ..Pgc::new(cells) };
    let vts = Vts { pgcs: vec![pgc], ptts: vec![vec![(1, 1), (1, 2), (1, 3)]], vobus: vobus(&navs), vob_sectors: 120 };
    let d = OpenDisc::build("titles-one", &[Title::new(1, 1, 3)], &[(vts, navs)]);
    let p = plan(&d, None, CELLS_AUTO, ORDER_AUTO, 0);
    assert_eq!(names(&p), ["1"]);
    let t = &p.titles[0];
    assert_eq!(t.cells, [0, 1, 2]);
    assert_eq!(t.segments.len(), 2);
    assert_eq!((t.segments[0].label.as_str(), t.segments[0].size), ("1-2", 80 * 2048));
    assert_eq!(t.segments[0].extents.len(), 1, "contiguous VOBUs join into one extent");
    assert_eq!(t.segments[1].label, "3");
    // chapter 2 at cell 2 (block 40), 2 s after the start; chapter 3 starts segment 2
    let ch: Vec<(u32, u64)> = t.chapters.iter().map(|c| (c.segment, c.offset)).collect();
    assert_eq!(ch, [(0, 0), (0, 40 * 2048), (1, 0)]);
    // its time runs to the END of its first VOBU (2 s + 0.5 s)
    assert_eq!(t.chapters[1].time, 5 * 45_000 * 12_000);
    assert_eq!((t.declared_secs, t.measured_secs, t.not_selected), (6, 6, 0));
    assert_eq!(events(&p, "title"), ["1\t3\t0:00:06"]);
}

#[test]
fn one_title_per_program_chain_of_its_parts() {
    let (mut navs, mut t0) = (Vec::new(), 0);
    let a = cell(0, 4, 2, &mut navs, &mut t0);
    let b = cell(40, 4, 2, &mut navs, &mut t0);
    let vts = Vts {
        pgcs: vec![Pgc::new(vec![a]), Pgc::new(vec![b])],
        ptts: vec![vec![(1, 1), (2, 1)]],
        vobus: vobus(&navs),
        vob_sectors: 80,
    };
    let d = OpenDisc::build("titles-pgcs", &[Title::new(1, 1, 2)], &[(vts, navs)]);
    let p = plan(&d, None, CELLS_AUTO, ORDER_AUTO, 0);
    assert_eq!(names(&p), ["1", "1/1"]);
    assert_eq!((p.titles[0].pgcn, p.titles[1].pgcn), (1, 2));
}

#[test]
fn cell_walk_takes_the_cells_the_navigation_played() {
    let (mut navs, mut t0) = (Vec::new(), 0);
    let cells: Vec<Cell> = (0..4).map(|k| cell(40 * k, 4, 2, &mut navs, &mut t0)).collect();
    let vts = Vts { pgcs: vec![Pgc::new(cells)], ptts: vec![vec![(1, 1)]], vobus: vobus(&navs), vob_sectors: 160 };
    let d = OpenDisc::build("titles-walk", &[Title::new(1, 1, 1)], &[(vts, navs)]);
    let s = scan(&[(1, 1, &[2, 3])]);
    let p = plan(&d, Some(&s), CELLS_WALK, ORDER_AUTO, 0);
    assert_eq!(p.titles[0].cells, [1, 2]);
    assert_eq!(events(&p, "cells-cut-start"), ["1"]);
    assert_eq!(events(&p, "cells-cut-end"), ["4\t4"]);
    // cell walk without a scan result for the title: no title
    let p = plan(&d, Some(&scan(&[])), CELLS_WALK, ORDER_AUTO, 0);
    assert!(p.titles.is_empty());
}

#[test]
fn fake_and_short_titles_are_listed_not_selected() {
    let (mut navs, mut t0) = (Vec::new(), 0);
    // declares 1000 s, its NAV packs play 2 s
    let fake = Cell { playback_time: time(1000), ..cell(0, 4, 2, &mut navs, &mut t0) };
    let short = cell(40, 4, 2, &mut navs, &mut t0);
    let vts = Vts {
        pgcs: vec![Pgc::new(vec![fake]), Pgc::new(vec![short])],
        ptts: vec![vec![(1, 1)], vec![(2, 1)]],
        vobus: vobus(&navs),
        vob_sectors: 80,
    };
    let d = OpenDisc::build("titles-fake", &[Title::new(1, 1, 1), Title::new(1, 2, 1)], &[(vts, navs)]);
    let p = plan(&d, None, CELLS_AUTO, ORDER_AUTO, 120);
    assert_eq!(names(&p), ["1", "2"]);
    assert_eq!(p.titles[0].not_selected, TITLE_FAKE);
    assert!(p.titles[0].segments.is_empty(), "a fake title gets no segments");
    assert_eq!(events(&p, "fake-length"), ["1\t0:16:40\t0:00:02"]);
    assert_eq!(p.titles[1].not_selected, TITLE_SHORT);
    assert_eq!(p.titles[1].segments.len(), 1, "a short title is built: it can still be chosen");
    assert_eq!(events(&p, "short"), ["2\t2\t120"]);
}

#[test]
fn a_title_that_repeats_another_is_dropped() {
    let (mut navs, mut t0) = (Vec::new(), 0);
    let a = cell(0, 4, 2, &mut navs, &mut t0);
    let vts = Vts { pgcs: vec![Pgc::new(vec![a])], ptts: vec![vec![(1, 1)], vec![(1, 1)]], vobus: vobus(&navs), vob_sectors: 40 };
    let d = OpenDisc::build("titles-dup", &[Title::new(1, 1, 1), Title::new(1, 2, 1)], &[(vts, navs)]);
    let p = plan(&d, None, CELLS_AUTO, ORDER_AUTO, 0);
    assert_eq!(names(&p), ["1"]);
    assert_eq!(events(&p, "duplicate"), ["1\t#01\t#02"]);
}

#[test]
fn title_order() {
    let (mut navs, mut t0) = (Vec::new(), 0);
    let a = cell(0, 4, 2, &mut navs, &mut t0);
    let b = cell(40, 4, 2, &mut navs, &mut t0);
    let vts = Vts {
        pgcs: vec![Pgc::new(vec![a]), Pgc::new(vec![b])],
        ptts: vec![vec![(1, 1)], vec![(2, 1)]],
        vobus: vobus(&navs),
        vob_sectors: 80,
    };
    let d = OpenDisc::build("titles-order", &[Title::new(1, 1, 1), Title::new(1, 2, 1)], &[(vts, navs)]);
    // the scan reached title 2 only: it comes first (title 1 has no scan
    // result and is left out by cell walk, but listed by the trim of walk_trim)
    let s = scan(&[(2, 2, &[1])]);
    let p = plan(&d, Some(&s), CELLS_AUTO, ORDER_AUTO, 0);
    assert_eq!(names(&p), ["2"]);
    let p = plan(&d, Some(&s), 3, ORDER_AUTO, 0);
    assert_eq!(names(&p), ["1", "2"], "walk_trim: the title table's order");
    let p = plan(&d, Some(&s), 3, 1, 0);
    assert_eq!(names(&p), ["2", "1"], "scan-first asked for");
    let p = plan(&d, Some(&s), 3, ORDER_TABLE, 0);
    assert_eq!(names(&p), ["1", "2"]);
}

#[test]
fn one_title_per_angle() {
    let (mut navs, mut t0) = (Vec::new(), 0);
    let first = cell(0, 4, 2, &mut navs, &mut t0);
    let a1 = cell(40, 4, 2, &mut navs, &mut t0).flags(flags::FIRST_IN_BLOCK | flags::ANGLE_BLOCK);
    let a2 = cell(80, 4, 2, &mut navs, &mut t0).flags(flags::LAST_IN_BLOCK | flags::ANGLE_BLOCK);
    let vts = Vts { pgcs: vec![Pgc::new(vec![first, a1, a2])], ptts: vec![vec![(1, 1)]], vobus: vobus(&navs), vob_sectors: 120 };
    let t = Title { nr_of_angles: 2, ..Title::new(1, 1, 1) };
    let d = OpenDisc::build("titles-angles", &[t], &[(vts, navs)]);
    let p = plan(&d, None, CELLS_AUTO, ORDER_AUTO, 0);
    assert_eq!(names(&p), ["1", "1"]);
    assert_eq!((p.titles[0].angle, p.titles[0].cells.clone()), (0, vec![0, 1]));
    assert_eq!((p.titles[1].angle, p.titles[1].cells.clone()), (1, vec![0, 2]));
    assert_eq!(events(&p, "angle"), ["2\t1"]);
}

/// One title set with two titles (program chains 1 and 2).
fn two_title_set() -> (Vts, Vec<Nav>) {
    let (mut navs, mut t0) = (Vec::new(), 0);
    let a = cell(0, 4, 2, &mut navs, &mut t0);
    let b = cell(40, 4, 3, &mut navs, &mut t0);
    let vts = Vts {
        pgcs: vec![Pgc::new(vec![a]), Pgc::new(vec![b])],
        ptts: vec![vec![(1, 1)], vec![(2, 1)]],
        vobus: vobus(&navs),
        vob_sectors: 80,
    };
    (vts, navs)
}

#[test]
fn a_title_naming_a_title_set_the_disc_does_not_have_is_left_out_with_a_warning() {
    let d = OpenDisc::build("titles-bad-vts", &[Title::new(1, 1, 1), Title::new(5, 1, 1)], &[two_title_set()]);
    let p = plan(&d, None, CELLS_AUTO, ORDER_TABLE, 0);
    assert_eq!(names(&p), ["1"], "the rest of the disc stays usable");
    assert_eq!(events(&p, "title-set-invalid"), ["2\t5\t1"]);
}

#[test]
fn a_title_giving_another_start_for_its_title_set_is_reported() {
    let at = |ttn: u8, sector: u32| Title { title_set_starting_sector: sector, ..Title::new(1, ttn, 1) };
    // the first non-zero start is the set's; a later different one (0 too) is
    // reported (set, this start, the first); then the set's start by the
    // disc's layout: the VMG (3 IFO blocks, vmg_last_sector 5) ends before
    // block 6 (set, layout start, the first)
    for (test, starts, want) in [
        ("titles-start-layout", [6, 6], vec![]),
        ("titles-start-same", [100, 100], vec!["1\t6\t100"]),
        ("titles-start-zero-first", [0, 100], vec!["1\t6\t100"]),
        ("titles-start-differs", [100, 200], vec!["1\t200\t100", "1\t6\t100"]),
        ("titles-start-zero-later", [100, 0], vec!["1\t0\t100", "1\t6\t100"]),
        ("titles-start-unset", [0, 0], vec![]),
    ] {
        let d = OpenDisc::build(test, &[at(1, starts[0]), at(2, starts[1])], &[two_title_set()]);
        let p = plan(&d, None, CELLS_AUTO, ORDER_TABLE, 0);
        assert_eq!(events(&p, "title-set-start-mismatch"), want, "{test}");
        assert_eq!(names(&p).len(), 2, "{test}: both titles stay");
    }
}
