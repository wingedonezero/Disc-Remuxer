//! IFO tables that break one rule each, on a small built disc (DVD-Video Book
//! layout, field names as libdvdread's `ifo_types.h`): missing or short tables,
//! tables whose entries run past their recorded end, program chains whose
//! offsets or command counts are out of range. Each case is one change to the
//! same valid disc.
//!
//! With `DVDVIDEO_IFO_CASES=<folder>`, the ignored test `write_ifo_cases`
//! writes every case as a disc folder `<folder>/<case>` (to run other DVD
//! software on the same inputs).

#![allow(clippy::many_single_char_names, reason = "cells keep short names")]

use std::path::Path;

mod common;
use common::dvd::{cell_navs, time, title_vobs, vmg_ifo, vts_ifo, write_disc, Cell, Nav, Pgc, Title, Vts};
use common::S;

fn rd32(d: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(d[at..at + 4].try_into().unwrap())
}
fn wr32(d: &mut [u8], at: usize, v: u32) {
    d[at..at + 4].copy_from_slice(&v.to_be_bytes());
}
fn wr16(d: &mut [u8], at: usize, v: u16) {
    d[at..at + 2].copy_from_slice(&v.to_be_bytes());
}
/// The byte where the table named by the sector pointer at `ptr` starts.
fn table(d: &[u8], ptr: usize) -> usize {
    rd32(d, ptr) as usize * S
}

/// `VMGI_MAT` / `VTSI_MAT` pointers.
const VMG_TT_SRPT: usize = 0xc4;
const VMG_VTS_ATRT: usize = 0xd0;
const VMG_NR_OF_TITLE_SETS: usize = 0x3e;
const VTS_PTT_SRPT: usize = 0xc8;
const VTS_PGCIT: usize = 0xcc;
const VTSM_VOBU_ADMAP: usize = 0xdc;
const VTS_C_ADT: usize = 0xe0;
const VTS_VOBU_ADMAP: usize = 0xe4;

/// A disc as files: the VMG and the title sets (IFO, title VOBs).
#[derive(Clone)]
pub struct Disc {
    pub vmg: Vec<u8>,
    pub sets: Vec<(Vec<u8>, Vec<u8>)>,
}

impl Disc {
    fn write(&self, dir: &Path) {
        write_disc(dir, &self.vmg, &self.sets);
    }
}

/// The valid disc: one title set with three titles of one program chain each
/// (cells of 4, 6 and 8 VOBUs, 0.5 s per VOBU).
fn base() -> Disc {
    let mut navs: Vec<Nav> = Vec::new();
    let mut t0 = 0;
    let mut cell = |first: u32, vobus: u32| {
        let starts: Vec<u32> = (0..vobus).map(|k| first + 10 * k).collect();
        navs.extend(cell_navs(&starts, first + 10 * vobus, t0));
        t0 += 45_000 * vobus;
        let mut c = Cell::new(first, first + 10 * (vobus - 1), first + 10 * vobus - 1, vobus / 2);
        c.playback_time = time(vobus / 2);
        c
    };
    let (a, b, c) = (cell(0, 4), cell(40, 6), cell(100, 8));
    let mut vobus: Vec<u32> = navs.iter().map(|n| n.lbn).collect();
    vobus.sort_unstable();
    let pgc = |cells: Vec<Cell>, title: u8| Pgc { entry_id: 0x80 | title, ..Pgc::new(cells) };
    let vts = Vts {
        pgcs: vec![pgc(vec![a], 1), pgc(vec![b], 2), pgc(vec![c], 3)],
        ptts: vec![vec![(1, 1)], vec![(2, 1)], vec![(3, 1)]],
        vobus,
        vob_sectors: 180,
    };
    let titles = [Title::new(1, 1, 1), Title::new(1, 2, 1), Title::new(1, 3, 1)];
    Disc { vmg: vmg_ifo(&titles, 1), sets: vec![(vts_ifo(&vts), title_vobs(vts.vob_sectors, &navs))] }
}

/// Every case: name, what it breaks, the change.
#[allow(clippy::type_complexity, reason = "a table of closures")]
#[must_use]
pub fn cases() -> Vec<(&'static str, &'static str, Box<dyn Fn(&mut Disc)>)> {
    vec![
        ("base", "nothing (the valid disc)", Box::new(|_| {})),
        // ---- VMG ----
        ("vmg_atrt_sector_0", "VMG: VTS_ATRT pointer 0", Box::new(|d| wr32(&mut d.vmg, VMG_VTS_ATRT, 0))),
        ("vmg_tt_srpt_short", "TT_SRPT last_byte covers 2 of its 3 entries", Box::new(|d| {
            let t = table(&d.vmg, VMG_TT_SRPT);
            wr32(&mut d.vmg, t + 4, 8 + 12 * 2 - 1);
        })),
        ("vmg_tt_srpt_last_byte_0", "TT_SRPT last_byte 0", Box::new(|d| {
            let t = table(&d.vmg, VMG_TT_SRPT);
            wr32(&mut d.vmg, t + 4, 0);
        })),
        ("vmg_title_bad_set", "title 3 names title set 5 (the disc has 1)", Box::new(|d| {
            let t = table(&d.vmg, VMG_TT_SRPT);
            d.vmg[t + 8 + 12 * 2 + 6] = 5;
        })),
        ("vmg_100_title_sets", "vmg_nr_of_title_sets 100", Box::new(|d| wr16(&mut d.vmg, VMG_NR_OF_TITLE_SETS, 100))),
        ("vmg_set_start_mismatch", "titles 1 / 2 give title set 1 the start sectors 100 / 200", Box::new(|d| {
            let t = table(&d.vmg, VMG_TT_SRPT);
            wr32(&mut d.vmg, t + 8 + 8, 100);
            wr32(&mut d.vmg, t + 8 + 12 + 8, 200);
        })),
        // ---- VTS tables ----
        ("vts_c_adt_sector_0", "VTS_C_ADT pointer 0", Box::new(|d| wr32(&mut d.sets[0].0, VTS_C_ADT, 0))),
        ("vts_c_adt_short", "VTS_C_ADT last_byte 3 (shorter than its header)", Box::new(|d| {
            let t = table(&d.sets[0].0, VTS_C_ADT);
            wr32(&mut d.sets[0].0, t + 4, 3);
        })),
        ("vts_vobu_admap_sector_0", "VTS_VOBU_ADMAP pointer 0", Box::new(|d| wr32(&mut d.sets[0].0, VTS_VOBU_ADMAP, 0))),
        ("vts_vobu_admap_short", "VTS_VOBU_ADMAP last_byte 2 (no entry)", Box::new(|d| {
            let t = table(&d.sets[0].0, VTS_VOBU_ADMAP);
            wr32(&mut d.sets[0].0, t, 2);
        })),
        ("vtsm_vobu_admap_unloadable", "VTSM_VOBU_ADMAP names a sector whose length runs past the IFO", Box::new(|d| {
            let p = rd32(&d.sets[0].0, VTS_PTT_SRPT);
            wr32(&mut d.sets[0].0, VTSM_VOBU_ADMAP, p);
        })),
        ("vts_ptt_count_large", "VTS_PTT_SRPT says 4 titles, holds 3", Box::new(|d| {
            let t = table(&d.sets[0].0, VTS_PTT_SRPT);
            wr16(&mut d.sets[0].0, t, 4);
        })),
        ("vts_ptt_misaligned", "VTS_PTT_SRPT: title 2's offset 2 bytes into its entry", Box::new(|d| {
            let t = table(&d.sets[0].0, VTS_PTT_SRPT);
            let o = rd32(&d.sets[0].0, t + 8 + 4);
            wr32(&mut d.sets[0].0, t + 8 + 4, o + 2);
        })),
        ("vts_ptt_last_byte_plus_1", "VTS_PTT_SRPT last_byte one past its end", Box::new(|d| {
            let t = table(&d.sets[0].0, VTS_PTT_SRPT);
            let l = rd32(&d.sets[0].0, t + 4);
            wr32(&mut d.sets[0].0, t + 4, l + 1);
        })),
        ("vts_pgcit_sector_0", "VTS_PGCIT pointer 0", Box::new(|d| wr32(&mut d.sets[0].0, VTS_PGCIT, 0))),
        // ---- program chains ----
        ("pgc_cells_offset_0", "PGC 2: cell_playback_offset 0 with 1 cell", Box::new(|d| {
            let t = table(&d.sets[0].0, VTS_PGCIT);
            let p = t + rd32(&d.sets[0].0, t + 8 + 8 + 4) as usize;
            wr16(&mut d.sets[0].0, p + 0xe8, 0);
        })),
        ("pgc_129_commands", "PGC 2: a command table of 129 pre commands", Box::new(|d| {
            // the command table goes at the end of the PGCIT (its sector has room)
            let ifo = &mut d.sets[0].0;
            let t = table(ifo, VTS_PGCIT);
            let p = t + rd32(ifo, t + 8 + 8 + 4) as usize;
            let end = t + rd32(ifo, t + 4) as usize + 1;
            let tbl = end.next_multiple_of(4);
            let len = 8 + 8 * 129;
            assert!(tbl + len <= t + S, "room for the table");
            wr16(ifo, p + 0xe4, u16::try_from(tbl - p).unwrap());
            wr16(ifo, tbl, 129);
            wr16(ifo, tbl + 6, u16::try_from(len - 1).unwrap());
            wr32(ifo, t + 4, u32::try_from(tbl + len - t - 1).unwrap());
        })),
        ("pgc_command_table_past_pgcit", "PGC 2: command table offset past the PGCIT's end", Box::new(|d| {
            let ifo = &mut d.sets[0].0;
            let t = table(ifo, VTS_PGCIT);
            let p = t + rd32(ifo, t + 8 + 8 + 4) as usize;
            let end = t + rd32(ifo, t + 4) as usize + 1;
            wr16(ifo, p + 0xe4, u16::try_from(end + 64 - p).unwrap());
        })),
        ("pgc_duplicate_of_unreadable", "PGCs 2 and 3 share a start past the end of the IFO", Box::new(|d| {
            let ifo = &mut d.sets[0].0;
            let t = table(ifo, VTS_PGCIT);
            wr32(ifo, t + 8 + 8 + 4, 0x8000);
            wr32(ifo, t + 8 + 16 + 4, 0x8000);
        })),
        // ---- the IFO itself ----
        ("vts_ifo_bad_signature", "VTS_01_0.IFO signature broken, its BUP intact", Box::new(|d| {
            d.sets[0].0[0] = b'X';
        })),
    ]
}

/// Writes case `name` with its IFO also as the BUP; the IFO-signature case
/// keeps an intact BUP.
fn write_case(dir: &Path, name: &str, change: &dyn Fn(&mut Disc)) {
    let good = base();
    let mut d = good.clone();
    change(&mut d);
    d.write(dir);
    if name == "vts_ifo_bad_signature" {
        std::fs::write(dir.join("VIDEO_TS").join("VTS_01_0.BUP"), &good.sets[0].0).unwrap();
    }
}

#[test]
#[ignore = "writes the cases as disc folders for other DVD software (DVDVIDEO_IFO_CASES)"]
fn write_ifo_cases() {
    let Some(out) = std::env::var_os("DVDVIDEO_IFO_CASES") else {
        eprintln!("set DVDVIDEO_IFO_CASES to the folder to write the cases into");
        return;
    };
    let out = Path::new(&out);
    for (name, what, change) in cases() {
        write_case(&out.join(name), name, change.as_ref());
        println!("{name}\t{what}");
    }
}

#[test]
fn every_case_differs_from_the_valid_disc() {
    let good = base();
    for (name, _, change) in cases().into_iter().skip(1) {
        let mut d = good.clone();
        change(&mut d);
        assert!(d.vmg != good.vmg || d.sets != good.sets, "{name} changes nothing");
    }
}

/// The title plan of case `name` through the whole path (source, disc, the
/// navigation scan, titles), cell mode automatic, no minimum length.
fn plan_of(name: &str) -> Result<ffmpeg_sys::dvdvideo::Plan, std::os::raw::c_int> {
    use ffmpeg_sys::dvdvideo::{titles, TitleOptions, CELLS_AUTO, ORDER_AUTO};
    let (_, _, change) = cases().into_iter().find(|c| c.0 == name).unwrap();
    let dir = std::env::temp_dir().join(format!("dvdvideo-ifo-case-{name}-{}", std::process::id()));
    write_case(&dir, name, change.as_ref());
    let opts = ffmpeg_sys::discio::ImageOptions::default();
    titles(&dir, &opts, 1, &TitleOptions { cell_mode: CELLS_AUTO, title_order: ORDER_AUTO, min_length: 0 })
}

#[test]
fn a_disc_without_a_first_play_program_chain_or_menus_fails_the_scan_not_the_program() {
    // the navigator has no program chain to start with: the scan fails (the
    // navigator stops), the titles come from the IFOs alone
    let p = plan_of("base").unwrap();
    let failure = p.scan.as_ref().and_then(|s| s.failure.clone()).unwrap_or_default();
    assert!(!failure.is_empty(), "the scan fails");
    let names: Vec<&str> = p.titles.iter().map(|t| t.name.as_str()).collect();
    assert_eq!(names, ["1", "2", "3"]);
}
