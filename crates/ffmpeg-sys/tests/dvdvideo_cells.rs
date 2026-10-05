//! `libavformat/dvdvideo_cells.c`: which program chains and cells are real
//! content, on discs built byte by byte. Program chains: no cells (reason 0),
//! protection patterns of many discontinuous cells on shared title VOBs (1, 2),
//! too little data per second of play (3, 4), a broken program map (5, 6),
//! cells that should join seamlessly but do not (7, 8, 9). Cells: no playback
//! time, outside the title VOBs, short cells and cell commands, a uniform
//! filler cell, the NAV check of EVERY VOBU of a cell. Cell commands that may
//! link away.

use std::os::raw::{c_int, c_void};

use ffmpeg_sys::dvdvideo::Disc;

mod common;
use common::dvd::{cell_navs, flags, nav, time, Cell, Nav, OpenDisc, Pgc, Title, Vts};

extern "C" {
    fn ff_dvdvideo_disc_pgc(d: *const Disc, vtsn: c_int, pgcn: c_int) -> *const c_void;
    fn ff_dvdvideo_pgc_rejection(d: *mut Disc, vtsn: c_int, pgcn: c_int, pgc: *const c_void, reason: *mut c_int) -> c_int;
    fn ff_dvdvideo_cell_rejected(d: *mut Disc, vtsn: c_int, pgc: *const c_void, i: c_int, quick: c_int, prev: c_int) -> c_int;
    fn ff_dvdvideo_cell_cmd_may_link(pgc: *const c_void, cmd_nr: c_int) -> c_int;
}

/// A disc of one title set holding `pgcs` (one title, chapter 1 = PGC 1), its
/// VOBU map, title VOBs and NAV packs.
fn one_set(test: &str, pgcs: Vec<Pgc>, vobus: &[u32], vob_sectors: u32, navs: Vec<Nav>) -> OpenDisc {
    let vts = Vts { pgcs, ptts: vec![vec![(1, 1)]], vobus: vobus.to_vec(), vob_sectors };
    OpenDisc::build(&format!("cells-{test}"), &[Title::new(1, 1, 1)], &[(vts, navs)])
}

/// Two title sets with the same `pgcs`, whose title VOBs overlap by the title
/// search table (both start at sector 0).
fn shared_sets(test: &str, pgcs: &[Pgc], vobus: &[u32], vob_sectors: u32, navs: &[Nav]) -> OpenDisc {
    let vts = || (Vts { pgcs: pgcs.to_vec(), ptts: vec![vec![(1, 1)]], vobus: vobus.to_vec(), vob_sectors }, navs.to_vec());
    OpenDisc::build(&format!("cells-{test}"), &[Title::new(1, 1, 1), Title::new(2, 1, 1)], &[vts(), vts()])
}

fn rejection(d: &OpenDisc, pgcn: c_int) -> Option<c_int> {
    let mut reason = -1;
    // SAFETY: an open disc; the PGC pointer comes from it.
    let ret = unsafe {
        let pgc = ff_dvdvideo_disc_pgc(d.disc, 1, pgcn);
        assert!(!pgc.is_null());
        ff_dvdvideo_pgc_rejection(d.disc, 1, pgcn, pgc, &raw mut reason)
    };
    assert!(ret >= 0, "error {ret}");
    (ret == 1).then_some(reason)
}

fn cell_rejected(d: &OpenDisc, pgcn: c_int, i: c_int, quick: bool, prev: c_int) -> bool {
    // SAFETY: as above.
    let ret = unsafe {
        let pgc = ff_dvdvideo_disc_pgc(d.disc, 1, pgcn);
        ff_dvdvideo_cell_rejected(d.disc, 1, pgc, i, c_int::from(quick), prev)
    };
    assert!(ret >= 0, "error {ret}");
    ret == 1
}

/// `n` cells of `len` blocks each from block 0, `secs` seconds each, one VOBU
/// per cell; with their NAV packs.
fn plain_cells(n: u32, len: u32, secs: u32) -> (Vec<Cell>, Vec<u32>, Vec<Nav>) {
    let cells = (0..n).map(|k| Cell::new(k * len, k * len, k * len + len - 1, secs)).collect();
    let starts: Vec<u32> = (0..n).map(|k| k * len).collect();
    let navs = starts.iter().map(|&s| Nav { vobu_e_ptm: 45_000, ..nav(s, len, None) }).collect();
    (cells, starts, navs)
}

/// One PGC with programs at `programs`.
fn pgc(cells: Vec<Cell>, programs: &[u8]) -> Pgc {
    Pgc { programs: programs.to_vec(), ..Pgc::new(cells) }
}

#[test]
fn a_chain_without_cells_is_rejected() {
    let (cells, starts, navs) = plain_cells(1, 10, 10);
    let d = one_set("none", vec![Pgc::new(cells), Pgc::new(Vec::new())], &starts, 10, navs);
    assert_eq!(rejection(&d, 1), None);
    assert_eq!(rejection(&d, 2), Some(0));
}

#[test]
fn too_little_data_per_second_is_rejected() {
    // 4 cells without a playback time: no accepted second at all
    let (mut cells, starts, navs) = plain_cells(4, 1000, 10);
    for c in &mut cells {
        c.playback_time = [0, 0, 0, 0x40];
    }
    let d = one_set("rate0", vec![Pgc::new(cells)], &starts, 4000, navs);
    assert_eq!(rejection(&d, 1), Some(3));
    // 4 cells of 10 blocks and 60 s each: 40 blocks in 240 s, below 50 a second
    let (cells, starts, navs) = plain_cells(4, 10, 60);
    let d = one_set("rate", vec![Pgc::new(cells)], &starts, 40, navs);
    assert_eq!(rejection(&d, 1), Some(4));
    // 1000 blocks per 10 s cell: accepted
    let (cells, starts, navs) = plain_cells(4, 1000, 10);
    let d = one_set("rate-ok", vec![Pgc::new(cells)], &starts, 4000, navs);
    assert_eq!(rejection(&d, 1), None);
}

#[test]
fn a_broken_program_map_is_rejected() {
    let (cells, starts, navs) = plain_cells(3, 10, 10);
    let pgcs = vec![pgc(cells.clone(), &[1, 4]), pgc(cells.clone(), &[2, 1, 3]), pgc(cells, &[1, 2, 3])];
    let d = one_set("map", pgcs, &starts, 30, navs);
    assert_eq!(rejection(&d, 1), Some(5), "an entry cell past the last cell");
    assert_eq!(rejection(&d, 2), Some(6), "entry cells out of order");
    assert_eq!(rejection(&d, 3), None);
}

#[test]
fn seamless_joins_must_be_contiguous() {
    // cell 3 joins cell 2 seamlessly; program 1 is not examined
    let joined = |gap: u32| {
        vec![Cell::new(0, 0, 9, 10), Cell::new(10, 10, 19, 10), Cell::new(20 + gap, 20 + gap, 29 + gap, 10).flags(flags::SEAMLESS_PLAY)]
    };
    let starts = [0, 10, 20, 25];
    let navs: Vec<Nav> = starts.iter().map(|&s| nav(s, 5, None)).collect();
    let pgcs = vec![pgc(joined(0), &[1, 2, 3]), pgc(joined(5), &[1, 2, 3])];
    let d = one_set("join", pgcs, &starts, 40, navs);
    assert_eq!(rejection(&d, 1), None);
    assert_eq!(rejection(&d, 2), Some(7), "a gap between seamlessly joined cells");
}

#[test]
fn a_gap_after_an_interleaved_cell_must_be_interleaved_units() {
    // cell 2 interleaved, cell 3 plain and seamless: the 5 blocks between them
    // must be interleaved units
    let cells = vec![
        Cell::new(0, 0, 9, 10),
        Cell::new(10, 10, 19, 10).flags(flags::INTERLEAVED),
        Cell::new(25, 25, 34, 10).flags(flags::SEAMLESS_PLAY),
    ];
    let units = Nav { category: 0x6000, ilvu_ea: 4, ..nav(20, 5, None) };
    let base = vec![nav(0, 10, None), nav(10, 10, None), nav(25, 10, None)];
    let d = one_set("ilvu-ok", vec![pgc(cells.clone(), &[1, 2, 3])], &[0, 10, 20, 25], 35, [base.clone(), vec![units]].concat());
    assert_eq!(rejection(&d, 1), None, "an interleaved unit fills the gap");
    let d = one_set("ilvu-gap", vec![pgc(cells, &[1, 2, 3])], &[0, 10, 25], 35, base);
    assert_eq!(rejection(&d, 1), Some(8), "nothing fills the gap");
}

#[test]
fn protection_patterns_on_shared_title_vobs_are_rejected() {
    // 12 cells (100 blocks, 2 s each), all after the first with an STC discontinuity
    let (mut cells, starts, navs) = plain_cells(12, 100, 2);
    for c in cells.iter_mut().skip(1) {
        c.flags = flags::STC_DISCONTINUITY;
    }
    let d = shared_sets("stc12", &[Pgc::new(cells.clone())], &starts, 1200, &navs);
    assert_eq!(rejection(&d, 1), Some(1));
    // the same on title VOBs of its own: accepted
    let d = one_set("stc12-own", vec![Pgc::new(cells)], &starts, 1200, navs);
    assert_eq!(rejection(&d, 1), None);
    // 7 flagged cells, not in sector order
    let (mut cells, starts, navs) = plain_cells(7, 100, 2);
    for c in cells.iter_mut().skip(1) {
        c.flags = flags::STC_DISCONTINUITY;
    }
    cells.swap(2, 4);
    let d = shared_sets("stc7", &[Pgc::new(cells)], &starts, 700, &navs);
    assert_eq!(rejection(&d, 1), Some(2));
}

#[test]
fn a_broken_interleaved_angle_block_on_shared_title_vobs_is_rejected() {
    // cell 2 starts an interleaved angle block that cell 3 does not continue
    let cells = vec![
        Cell::new(0, 0, 9, 10),
        Cell::new(10, 10, 19, 10).flags(flags::FIRST_IN_BLOCK | flags::ANGLE_BLOCK | flags::INTERLEAVED),
        Cell::new(20, 20, 29, 10).flags(flags::SEAMLESS_PLAY),
    ];
    let navs: Vec<Nav> = [0, 10, 20].iter().map(|&s| nav(s, 10, None)).collect();
    let d = shared_sets("angle", &[pgc(cells, &[1, 2, 3])], &[0, 10, 20], 30, &navs);
    assert_eq!(rejection(&d, 1), Some(9));
}

#[test]
fn cell_rejects() {
    let mut cells = vec![
        Cell::new(0, 0, 9, 10),
        // no playback time
        Cell { playback_time: [0, 0, 0, 0x40], ..Cell::new(10, 10, 19, 10) },
        // one second, one VOBU
        Cell::new(20, 20, 29, 1),
        // five seconds with a cell command
        Cell { cell_cmd_nr: 1, ..Cell::new(30, 30, 39, 5) },
        // six seconds with a cell command
        Cell { cell_cmd_nr: 1, ..Cell::new(40, 40, 49, 6) },
        Cell::new(50, 50, 59, 10),
    ];
    cells[2].playback_time = time(1);
    let starts: Vec<u32> = (0..6).map(|k| 10 * k).collect();
    let navs: Vec<Nav> = starts.iter().map(|&s| Nav { vobu_e_ptm: 45_000, ..nav(s, 10, None) }).collect();
    let p = Pgc { cell_cmds: vec![[0x20, 0x01, 0, 0, 0, 0, 0, 0x0d]], ..Pgc::new(cells) };
    let d = one_set("cells", vec![p], &starts, 60, navs);
    assert!(!cell_rejected(&d, 1, 0, false, -1));
    assert!(cell_rejected(&d, 1, 1, true, 0), "no playback time");
    assert!(cell_rejected(&d, 1, 2, false, -1), "a one-second cell without the cell before it");
    assert!(!cell_rejected(&d, 1, 2, false, 1), "a one-second cell after the cell before it");
    assert!(cell_rejected(&d, 1, 3, false, 2), "a cell command on a short cell");
    assert!(!cell_rejected(&d, 1, 4, false, 3), "a cell command on a six-second cell");
}

#[test]
fn a_cell_outside_the_title_vobs_is_rejected() {
    let cells = vec![Cell::new(0, 0, 9, 10), Cell::new(10, 10, 19, 10)];
    let d = one_set("outside", vec![Pgc::new(cells)], &[0, 10], 15, vec![nav(0, 10, None), nav(10, 5, None)]);
    assert!(!cell_rejected(&d, 1, 0, true, -1));
    assert!(cell_rejected(&d, 1, 1, true, 0), "ends past the 15 blocks of the title VOBs");
}

#[test]
fn every_vobu_of_a_long_cell_must_be_a_nav_pack() {
    // a 31-minute cell of 60 VOBUs of 10 blocks
    let starts: Vec<u32> = (0..60).map(|k| 10 * k).collect();
    let cell = || vec![Cell::new(0, 590, 599, 31 * 60)];
    let navs = cell_navs(&starts, 600, 0);
    let d = one_set("deep-ok", vec![Pgc::new(cell())], &starts, 600, navs.clone());
    assert!(!cell_rejected(&d, 1, 0, false, -1));
    // VOBU 37 has no NAV pack: found on every run (no random sample)
    let mut holed = navs;
    holed.remove(37);
    for run in 0..3 {
        let d = one_set(&format!("deep-hole-{run}"), vec![Pgc::new(cell())], &starts, 600, holed.clone());
        assert!(cell_rejected(&d, 1, 0, false, -1));
    }
}

#[test]
fn a_long_single_cell_of_tiny_uniform_vobus_is_filler() {
    // one cell, 31 minutes, 1001 VOBUs of 2 blocks
    let starts: Vec<u32> = (0..1001).map(|k| 2 * k).collect();
    let d = one_set("filler", vec![Pgc::new(vec![Cell::new(0, 2000, 2001, 31 * 60)])], &starts, 2002, Vec::new());
    assert!(cell_rejected(&d, 1, 0, false, -1));
}

#[test]
fn cell_commands_that_may_link() {
    let cmds = vec![
        [0x20, 0x01, 0, 0, 0, 0, 0, 0x00], // link without a target
        [0x20, 0x01, 0, 0, 0, 0, 0, 0x0d], // link
        [0x20, 0x31, 0, 0x05, 0, 0x05, 0, 0x0d], // link if GPRM 5 > GPRM 5: never
        [0x20, 0x31, 0, 0x05, 0, 0x06, 0, 0x0d], // link if GPRM 5 > GPRM 6
        [0x60, 0x01, 0, 0, 0, 0, 0, 0x00],
        [0x70, 0x04, 0, 0, 0, 0, 0, 0x01],
    ];
    let p = Pgc { cell_cmds: cmds, ..Pgc::new(vec![Cell::new(0, 0, 9, 10)]) };
    let d = one_set("cmds", vec![p], &[0], 10, vec![nav(0, 10, None)]);
    // SAFETY: an open disc; the PGC pointer comes from it.
    let may = |k: c_int| unsafe { ff_dvdvideo_cell_cmd_may_link(ff_dvdvideo_disc_pgc(d.disc, 1, 1), k) } == 1;
    assert!(!may(0), "no command");
    assert!(!may(1));
    assert!(may(2));
    assert!(!may(3));
    assert!(may(4));
    assert!(!may(5));
    assert!(may(6));
    assert!(!may(7), "past the table");
}
