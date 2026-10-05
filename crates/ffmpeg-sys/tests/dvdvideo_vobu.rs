//! `libavformat/dvdvideo_vobu.c`: stepping from VOBU to VOBU through a title
//! set's title VOBs, on discs built byte by byte. The VOBU address map is used
//! while it can be trusted; a block it lacks (found by a lookup, or a cell off
//! the map at open) makes it untrusted, and the NAV packs are used from then
//! on. The NAV-pack rules: the end of a cell, the VOBU after one that ends its
//! cell inside a continuing interleaved unit (a blanked NAV pack counts), a NAV
//! pack pointing at itself; seamless-angle cells step by the map only when
//! their VOBU chain checks out.

use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use ffmpeg_sys::discio::ImageOptions;
use ffmpeg_sys::dvdvideo::{
    ff_dvdvideo_disc_close, ff_dvdvideo_disc_open, ff_dvdvideo_source_close, ff_dvdvideo_source_open, Disc, Source,
};

mod common;
use common::dvd::{flags, title_vobs, vmg_ifo, vts_ifo, write_disc, Cell, Nav, Pgc, Title, Vts, END_OF_CELL};

/// `DVDVIDEO_VOBU_END_OF_CELL`.
const STEP_END_OF_CELL: u32 = 0xffff_fffe;
/// `sml_pbi.category` bit: inside an interleaved unit.
const IN_ILVU: u16 = 0x4000;

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
struct Step {
    next: u32,
    len: u32,
}

extern "C" {
    fn ff_dvdvideo_disc_cell(d: *const Disc, vtsn: c_int, pgcn: c_int, celln: c_int) -> *const c_void;
    fn ff_dvdvideo_vobu_step(d: *mut Disc, vtsn: c_int, cell: *const c_void, sector: u32, step: *mut Step) -> c_int;
}

static LOG: Mutex<Vec<String>> = Mutex::new(Vec::new());

unsafe extern "C" fn log_sink(_level: c_int, line: *const c_char) {
    // SAFETY: the glue passes a NUL-terminated line.
    let text = unsafe { std::ffi::CStr::from_ptr(line) }.to_string_lossy().into_owned();
    LOG.lock().unwrap().push(text);
}

/// An open disc built from one title set with one PGC of `cells`.
struct TestDisc {
    src: *mut Source,
    disc: *mut Disc,
}

impl TestDisc {
    fn new(test: &str, cells: Vec<Cell>, vobus: &[u32], vob_sectors: u32, navs: &[Nav]) -> Self {
        // SAFETY: log_sink is valid for the whole test run.
        unsafe { ffmpeg_sys::dr_log_install(log_sink, ffmpeg_sys::log_level::DEBUG) };
        let dir: PathBuf = std::env::temp_dir().join(format!("dvdvideo-vobu-{test}-{}", std::process::id()));
        let vts = Vts { pgcs: vec![Pgc::new(cells)], ptts: vec![vec![(1, 1)]], vobus: vobus.to_vec(), vob_sectors };
        let vmg = vmg_ifo(&[Title::new(1, 1, 1)], 1);
        write_disc(&dir, &vmg, &[(vts_ifo(&vts), title_vobs(vob_sectors, navs))]);
        open(&dir)
    }

    fn step(&self, celln: c_int, sector: u32) -> Option<Step> {
        let mut st = Step::default();
        // SAFETY: an open disc; the cell pointer comes from it.
        let ret = unsafe {
            let cell = ff_dvdvideo_disc_cell(self.disc, 1, 1, celln);
            assert!(!cell.is_null());
            ff_dvdvideo_vobu_step(self.disc, 1, cell, sector, &raw mut st)
        };
        assert!(ret >= 0, "error {ret}");
        (ret == 1).then_some(st)
    }
}

fn open(dir: &Path) -> TestDisc {
    let c = CString::new(dir.to_str().unwrap()).unwrap();
    let (mut src, mut disc) = (std::ptr::null_mut(), std::ptr::null_mut());
    // SAFETY: valid path, options and out pointers.
    unsafe {
        assert!(ff_dvdvideo_source_open(std::ptr::null_mut(), c.as_ptr(), &ImageOptions::default(), 1, &raw mut src) >= 0);
        assert!(ff_dvdvideo_disc_open(std::ptr::null_mut(), src, &raw mut disc) >= 0, "the built disc opens");
    }
    TestDisc { src, disc }
}

impl Drop for TestDisc {
    fn drop(&mut self) {
        // SAFETY: opened in `open`, closed once, the disc first.
        unsafe {
            ff_dvdvideo_disc_close(&raw mut self.disc);
            ff_dvdvideo_source_close(&raw mut self.src);
        }
    }
}

fn logged(text: &str) -> bool {
    LOG.lock().unwrap().iter().any(|l| l.contains(text))
}

/// A NAV pack at `lbn` of a VOBU of `len` blocks followed by the next one
/// (`next` = None: the last VOBU of its cell).
fn nav(lbn: u32, len: u32, next: Option<u32>) -> Nav {
    Nav { lbn, vobu_ea: len - 1, next_vobu: next.map_or(END_OF_CELL, |n| n - lbn), ..Nav::default() }
}

#[test]
fn a_trusted_map_steps_plain_cells_without_nav_packs() {
    // only the last VOBU has a NAV pack: every step before it is the map's
    let d = TestDisc::new("map", vec![Cell::new(0, 20, 29, 10)], &[0, 10, 20], 30, &[nav(20, 10, None)]);
    assert_eq!(d.step(1, 0), Some(Step { next: 10, len: 10 }));
    assert_eq!(d.step(1, 10), Some(Step { next: 20, len: 10 }));
    assert_eq!(d.step(1, 20), Some(Step { next: STEP_END_OF_CELL, len: 10 }), "the last VOBU by its NAV pack");
}

#[test]
fn a_block_the_map_lacks_makes_it_untrusted() {
    // block 15 is a VOBU start the map does not list (NAV packs at 15 and 20)
    let d = TestDisc::new("lacks", vec![Cell::new(0, 20, 29, 10)], &[0, 10, 20], 30, &[nav(15, 5, Some(20)), nav(20, 10, None)]);
    assert_eq!(d.step(1, 0), Some(Step { next: 10, len: 10 }), "trusted: no NAV pack needed at 0");
    assert_eq!(d.step(1, 15), Some(Step { next: 20, len: 5 }));
    assert!(logged("block 15 of its title VOBs is not in the VOBU address map"));
    // untrusted now: block 0 has no NAV pack, so there is no step from it
    assert_eq!(d.step(1, 0), None);
}

#[test]
fn a_cell_off_the_map_makes_it_untrusted_at_open() {
    // the cell's last VOBU (15) is not in the map: the NAV packs are used
    let d = TestDisc::new("off-map", vec![Cell::new(0, 15, 29, 10)], &[0, 10, 20], 30, &[nav(0, 15, Some(15)), nav(15, 15, None)]);
    assert_eq!(d.step(1, 0), Some(Step { next: 15, len: 15 }));
}

#[test]
fn nav_pack_steps() {
    let ilvu = |n: Nav, ilvu_ea: u32| Nav { category: IN_ILVU, ilvu_ea, ..n };
    let cells = vec![Cell::new(0, 0, 19, 5).flags(flags::INTERLEAVED), Cell::new(20, 30, 39, 5).flags(flags::INTERLEAVED)];
    let navs = [
        // VOBU 0 ends its cell, but its interleaved unit goes on to block 9:
        // the VOBU at 5 follows it
        ilvu(nav(0, 5, None), 9),
        nav(5, 5, None),
        // VOBU 20 ends its cell while its unit ends before it: damaged
        ilvu(nav(20, 10, None), 4),
        // VOBU 30 points at itself: the cell's last VOBU -> end of cell
        Nav { lbn: 30, vobu_ea: 9, next_vobu: 0, ..Nav::default() },
    ];
    let d = TestDisc::new("nav", cells, &[0, 5, 20, 30], 40, &navs);
    assert_eq!(d.step(1, 0), Some(Step { next: 5, len: 5 }), "repaired: the unit goes on");
    assert_eq!(d.step(2, 20), Some(Step { next: STEP_END_OF_CELL, len: 10 }));
    assert!(logged("the interleaved unit of the VOBU at block 20 of its title VOBs (byte 40960) ends before the VOBU does"));
    assert_eq!(d.step(2, 30), Some(Step { next: STEP_END_OF_CELL, len: 10 }), "points at itself at the cell's end");
}

#[test]
fn a_nav_pack_pointing_at_itself_inside_a_cell_steps_by_its_length() {
    let navs = [Nav { lbn: 0, vobu_ea: 9, next_vobu: 0, ..Nav::default() }, nav(10, 10, None)];
    let d = TestDisc::new("self", vec![Cell::new(0, 10, 19, 5).flags(flags::INTERLEAVED)], &[0, 10], 20, &navs);
    assert_eq!(d.step(1, 0), Some(Step { next: 10, len: 10 }));
}

#[test]
fn a_blanked_nav_pack_after_an_interleaved_unit_counts() {
    let navs = [Nav { category: IN_ILVU, ilvu_ea: 9, ..nav(0, 5, None) }];
    let d = TestDisc::new("blanked", vec![Cell::new(0, 0, 9, 5).flags(flags::INTERLEAVED)], &[0, 5], 10, &navs);
    // block 5: the first 32 bytes 0xFF, the PCI and DSI still naming block 5
    let dir = std::env::temp_dir().join(format!("dvdvideo-vobu-blanked-{}", std::process::id()));
    let vob = dir.join("VIDEO_TS").join("VTS_01_1.VOB");
    let mut data = std::fs::read(&vob).unwrap();
    let b = &mut data[5 * 2048..6 * 2048];
    b[..32].fill(0xff);
    b[0x2d..0x31].copy_from_slice(&5u32.to_be_bytes());
    b[0x40b..0x40f].copy_from_slice(&5u32.to_be_bytes());
    b[0x40f..0x413].copy_from_slice(&4u32.to_be_bytes());
    std::fs::write(&vob, data).unwrap();
    assert_eq!(d.step(1, 0), Some(Step { next: 5, len: 5 }));
    assert!(logged("block 5 is a blanked NAV pack, taken as one"));
}

#[test]
fn a_block_without_a_nav_pack_gives_no_step() {
    let d = TestDisc::new("none", vec![Cell::new(0, 0, 9, 5).flags(flags::INTERLEAVED)], &[0], 10, &[]);
    assert_eq!(d.step(1, 0), None);
}

#[test]
fn seamless_angle_cells_use_the_map_only_when_their_chain_checks_out() {
    // the map says 0 -> 12; the NAV packs chain 0 -> 10 -> 20 -> 30
    let chain = [nav(0, 10, Some(10)), nav(10, 10, Some(20)), nav(20, 10, Some(30)), nav(30, 10, None)];
    let cell = || vec![Cell::new(0, 30, 39, 5).flags(flags::SEAMLESS_ANGLE)];
    let d = TestDisc::new("chain-ok", cell(), &[0, 12, 20, 30], 40, &chain);
    assert_eq!(d.step(1, 0), Some(Step { next: 12, len: 12 }), "chain checks out: the map");
    // VOBU 10 says its successor is at 15, not right after it: no chain
    let mut broken = chain;
    broken[1] = nav(10, 10, Some(15));
    let d = TestDisc::new("chain-broken", cell(), &[0, 12, 20, 30], 40, &broken);
    assert_eq!(d.step(1, 0), Some(Step { next: 10, len: 10 }), "no chain: the NAV pack");
}
