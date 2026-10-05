//! `libavformat/dvdvideo_source.c`: the DVD-Video demuxer's disc source. libdvdread
//! opens the disc through our file callbacks (`DVDOpenFiles`), so its files come
//! from the file system our choice rule picked (images) or from the disc folder,
//! read through our disc readers; images that are not DVD-Video, or whose DVD
//! files are scattered, are refused; an IFO block that cannot be read comes from
//! its BUP; the IFO layout checks warn; the title-set checks (VOBU address map,
//! cells inside the title VOBs) on built IFOs. With `DVDVIDEO_CSS_IMAGE` / `DVDVIDEO_CSS_FOLDER`
//! (a CSS-scrambled disc as an image / as a folder of its files), blocks
//! descrambled by the source equal libdvdcss's own decrypting read.

#![allow(clippy::many_single_char_names, reason = "one short name per descriptor being built")]

use std::ffi::CString;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};

use ffmpeg_sys::discio::{self, ImageOptions};
use libdvdcss_sys as css;

mod common;
use common::*;

const ENOENT: c_int = -2;
const EINVAL: c_int = -22;
const EOF: c_int = -0x2046_4F45; // FFERRTAG('E','O','F',' ')
const INVALIDDATA: c_int = -0x4144_4E49; // FFERRTAG('I','N','D','A')
const PATCHWELCOME: c_int = -0x4557_4150; // FFERRTAG('P','A','W','E')

/// `dvd_read_domain_t`.
const DVD_READ_INFO_FILE: c_int = 0;
const DVD_READ_TITLE_VOBS: c_int = 3;

#[repr(C)]
struct Source {
    _private: [u8; 0],
}
#[repr(C)]
struct Files {
    _private: [u8; 0],
}
#[repr(C)]
struct Reader {
    _private: [u8; 0],
}
#[repr(C)]
struct DvdFile {
    _private: [u8; 0],
}

extern "C" {
    fn ff_dvdvideo_source_open(
        log: *mut c_void,
        path: *const c_char,
        opts: *const ImageOptions,
        attempts: c_int,
        out: *mut *mut Source,
    ) -> c_int;
    fn ff_dvdvideo_source_close(src: *mut *mut Source);
    fn ff_dvdvideo_source_files(src: *mut Source) -> *mut Files;
    fn ff_dvdvideo_source_descramble(src: *mut Source, vtsn: c_int, menu: c_int, block: *mut u8) -> c_int;
    fn ff_dvdvideo_source_vob_read(src: *mut Source, vtsn: c_int, menu: c_int, sector: i64, buf: *mut u8, attempts: c_int)
        -> c_int;
    fn DVDOpenFiles(priv_: *mut c_void, logcb: *const c_void, path: *const c_char, fs: *mut Files) -> *mut Reader;
    fn DVDClose(dvd: *mut Reader);
    fn DVDOpenFile(dvd: *mut Reader, title: c_int, domain: c_int) -> *mut DvdFile;
    fn DVDCloseFile(file: *mut DvdFile);
    fn DVDReadBytes(file: *mut DvdFile, buf: *mut c_void, size: usize) -> isize;
    fn DVDReadBlocks(file: *mut DvdFile, offset: c_int, count: usize, buf: *mut u8) -> isize;
    fn ifoOpen(dvd: *mut Reader, title: c_int) -> *mut c_void;
    fn ifoClose(ifo: *mut c_void);
    fn ff_dvdvideo_check_vts(log: *mut c_void, dvd: *mut Reader, vtsn: c_int, ifo: *const c_void) -> c_int;
}

// ---- running the C code ----

/// An open disc source.
struct Src(*mut Source);

impl Src {
    fn open(path: &Path) -> Result<Self, c_int> {
        let c = CString::new(path.to_str().unwrap()).unwrap();
        let mut src = std::ptr::null_mut();
        let opts = ImageOptions::default();
        // SAFETY: valid path, options and out pointer.
        let ret = unsafe { ff_dvdvideo_source_open(std::ptr::null_mut(), c.as_ptr(), &raw const opts, 5, &raw mut src) };
        if ret < 0 { Err(ret) } else { Ok(Src(src)) }
    }

    /// libdvdread on the source's file callbacks.
    fn reader(&self) -> Dvd {
        // SAFETY: the source is open; libdvdread takes the callbacks over on success.
        unsafe {
            let fs = ff_dvdvideo_source_files(self.0);
            assert!(!fs.is_null());
            let dvd = DVDOpenFiles(std::ptr::null_mut(), std::ptr::null(), c"/".as_ptr(), fs);
            assert!(!dvd.is_null(), "DVDOpenFiles failed");
            Dvd(dvd)
        }
    }
}

impl Drop for Src {
    fn drop(&mut self) {
        // SAFETY: the source came from ff_dvdvideo_source_open.
        unsafe { ff_dvdvideo_source_close(&raw mut self.0) };
    }
}

struct Dvd(*mut Reader);

impl Dvd {
    /// The first `n` bytes of the file of `title` in `domain`.
    fn read_bytes(&self, title: c_int, domain: c_int, n: usize) -> Vec<u8> {
        // SAFETY: the reader is open; buf has room for n bytes.
        unsafe {
            let f = DVDOpenFile(self.0, title, domain);
            assert!(!f.is_null(), "DVDOpenFile({title}, {domain}) failed");
            let mut buf = vec![0u8; n];
            let got = DVDReadBytes(f, buf.as_mut_ptr().cast(), n);
            DVDCloseFile(f);
            assert_eq!(got, isize::try_from(n).unwrap());
            buf
        }
    }

    /// `count` blocks of the title VOBs of `title` from block `offset`.
    fn read_title_blocks(&self, title: c_int, offset: c_int, count: usize) -> Vec<u8> {
        // SAFETY: the reader is open; buf has room for count blocks.
        unsafe {
            let f = DVDOpenFile(self.0, title, DVD_READ_TITLE_VOBS);
            assert!(!f.is_null());
            let mut buf = vec![0u8; count * S];
            let got = DVDReadBlocks(f, offset, count, buf.as_mut_ptr());
            DVDCloseFile(f);
            assert_eq!(got, isize::try_from(count).unwrap());
            buf
        }
    }
}

impl Drop for Dvd {
    fn drop(&mut self) {
        // SAFETY: the reader came from DVDOpenFiles.
        unsafe { DVDClose(self.0) };
    }
}

/// A fresh folder for one test.
fn scratch(test: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("dvdvideo-source-{test}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn write_image(test: &str, im: &Img) -> PathBuf {
    let path = scratch(test).join("disc.iso");
    std::fs::write(&path, &im.d).unwrap();
    path
}

/// Fills sectors `from..from + n` with a pattern of their own.
fn fill(im: &mut Img, from: u32, n: u32, seed: u8) {
    for s in from..from + n {
        for (i, b) in im.sector(s).iter_mut().enumerate() {
            *b = u8::try_from((i + s as usize) % 251).unwrap() ^ seed;
        }
    }
}

fn sectors(im: &Img, from: u32, n: u32) -> Vec<u8> {
    im.d[from as usize * S..(from + n) as usize * S].to_vec()
}

// ---- images ----

#[test]
fn image_files_come_from_the_chosen_file_system() {
    // UDF 1.02 recorded in 2004: the ISO 9660 tree is chosen; the UDF tree points
    // VIDEO_TS.IFO somewhere else.
    let udf_files = vec![("VIDEO_TS.IFO", NO_VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096)];
    let mut im = bridge(Some(&udf102(2004, udf_files)), dvd_files(), None);
    fill(&mut im, NO_VMG, 2, 0x5a);
    for (i, b) in im.sector(VMG)[0x40..].iter_mut().enumerate() {
        *b = u8::try_from(i % 249).unwrap();
    }
    let path = write_image("chosen", &im);
    let src = Src::open(&path).unwrap();
    let got = src.reader().read_bytes(0, DVD_READ_INFO_FILE, 4096);
    assert_eq!(got, sectors(&im, VMG, 2), "VIDEO_TS.IFO as the ISO 9660 tree records it");
    assert_ne!(got, sectors(&im, NO_VMG, 2));
}

#[test]
fn a_scattered_dvd_file_is_refused() {
    // VTS_01_1.VOB in two pieces that do not follow each other.
    let mut u = udf102(2009, dvd_files());
    u.pieces = vec![("VTS_01_1.VOB", vec![(PART_START + 50, 4096), (PART_START + 60, 4096)])];
    let im = bridge(Some(&u), dvd_files(), None);
    assert_eq!(Src::open(&write_image("scattered", &im)).err(), Some(PATCHWELCOME));
    // Two pieces that follow each other are one piece on the disc.
    let mut u = udf102(2009, dvd_files());
    u.pieces = vec![("VTS_01_1.VOB", vec![(PART_START + 50, 4096), (PART_START + 52, 4096)])];
    let im = bridge(Some(&u), dvd_files(), None);
    assert!(Src::open(&write_image("adjacent", &im)).is_ok());
}

#[test]
fn images_that_are_not_dvd_video_are_refused() {
    let f = |name| (name, 50, 100);
    let mut im = Img::new();
    iso(&mut im, "BD", &[("BDMV", vec![f("index.bdmv"), f("MovieObject.bdmv")])], None);
    assert_eq!(Src::open(&write_image("bluray", &im)).err(), Some(PATCHWELCOME));
    let mut im = Img::new();
    iso(&mut im, "HDDVD", &[("ADV_OBJ", vec![f("DISCID.DAT")]), ("HVDVD_TS", vec![f("HVA00001.VTI")])], None);
    assert_eq!(Src::open(&write_image("hddvd", &im)).err(), Some(PATCHWELCOME));
    let mut im = Img::new();
    iso(&mut im, "DATA", &[("DOCS", vec![f("README.TXT")])], None);
    assert_eq!(Src::open(&write_image("data", &im)).err(), Some(INVALIDDATA));
    assert_eq!(Src::open(&write_image("blank", &Img::new())).err(), Some(INVALIDDATA));
}

#[test]
fn paths_that_are_not_discs_are_refused() {
    assert_eq!(Src::open(&scratch("missing").join("nothing.iso")).err(), Some(ENOENT));
    assert_eq!(Src::open(Path::new("/dev/null")).err(), Some(EINVAL));
}

// ---- IFO blocks from the BUP ----

/// Sector of the image's `VIDEO_TS.IFO` whose second block lies past the end of
/// the image (the image is cut after it).
const CUT_IFO: u32 = 449;

fn image_with_a_cut_ifo(bup: bool) -> Img {
    let mut files = vec![("VIDEO_TS.IFO", CUT_IFO, 4096), ("VTS_01_0.IFO", VTS1, 4096)];
    if bup {
        files.push(("VIDEO_TS.BUP", BUP, 4096));
    }
    let mut im = bridge(Some(&udf102(2009, files)), dvd_files(), None);
    im.vmg(CUT_IFO, b"DVDVIDEO-VMG", 1);
    for (i, b) in im.sector(CUT_IFO)[0x400..].iter_mut().enumerate() {
        *b = u8::try_from(i % 241).unwrap();
    }
    fill(&mut im, BUP + 1, 1, 0x33);
    im.d.truncate((CUT_IFO as usize + 1) * S);
    im
}

#[test]
fn an_unreadable_ifo_block_is_read_from_the_bup() {
    let im = image_with_a_cut_ifo(true);
    let src = Src::open(&write_image("cut-ifo", &im)).unwrap();
    let got = src.reader().read_bytes(0, DVD_READ_INFO_FILE, 4096);
    let mut want = sectors(&im, CUT_IFO, 1);
    want.extend(sectors(&im, BUP + 1, 1));
    assert_eq!(got, want, "block 0 from the IFO, block 1 from the BUP");
}

#[test]
fn without_a_bup_the_unreadable_ifo_block_is_a_read_error() {
    let im = image_with_a_cut_ifo(false);
    let src = Src::open(&write_image("cut-ifo-no-bup", &im)).unwrap();
    let dvd = src.reader();
    // SAFETY: the reader is open; buf has room for 4096 bytes.
    unsafe {
        let f = DVDOpenFile(dvd.0, 0, DVD_READ_INFO_FILE);
        assert!(!f.is_null());
        let mut buf = vec![0u8; 4096];
        assert!(DVDReadBytes(f, buf.as_mut_ptr().cast(), 4096) < 4096);
        DVDCloseFile(f);
    }
}

// ---- the IFO layout checks (warnings) ----

static LOG: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());

unsafe extern "C" fn log_sink(_level: c_int, line: *const c_char) {
    // SAFETY: the glue passes a NUL-terminated line.
    let text = unsafe { std::ffi::CStr::from_ptr(line) }.to_string_lossy().into_owned();
    LOG.lock().unwrap().push(text);
}

/// Whether a log line containing `text` was written while `f` ran.
fn logged(text: &str, f: impl FnOnce()) -> bool {
    // SAFETY: log_sink is a valid callback for the whole test run.
    unsafe { ffmpeg_sys::dr_log_install(log_sink, ffmpeg_sys::log_level::VERBOSE) };
    f();
    LOG.lock().unwrap().iter().any(|l| l.contains(text))
}

/// Sets `vmg/vts_last_sector` and `vmgi/vtsi_last_sector` of the IFO header at `sector`.
fn ifo_sizes(im: &mut Img, sector: u32, last: u32, ifo_last: u32) {
    let h = im.sector(sector);
    h[0x0c..0x10].copy_from_slice(&last.to_be_bytes());
    h[0x1c..0x20].copy_from_slice(&ifo_last.to_be_bytes());
}

#[test]
fn ifo_layout_warnings() {
    // image: the header puts the BUP 7 sectors after the IFO, the image 2
    let mut im = bridge(Some(&udf102(2009, dvd_files())), dvd_files(), None);
    ifo_sizes(&mut im, VMG, 8, 1);
    let path = write_image("layout", &im);
    assert!(logged("puts the backup copy (BUP) 7 sectors after the IFO; on the image it is 2 sectors", || {
        Src::open(&path).unwrap();
    }));
    // ... and where they agree, no warning
    let mut im = bridge(Some(&udf102(2009, dvd_files())), dvd_files(), None);
    ifo_sizes(&mut im, VMG, 3, 1);
    let path = write_image("layout-ok", &im);
    assert!(!logged("VIDEO_TS.IFO: its header puts the backup copy (BUP) 2 sectors", || {
        Src::open(&path).unwrap();
    }));
    // the provider identifier of a disc processed by DVDFab
    let mut im = bridge(Some(&udf102(2009, dvd_files())), dvd_files(), None);
    im.sector(VMG)[0x40..0x49].copy_from_slice(b"(Fab4321)");
    let path = write_image("fab", &im);
    assert!(logged("provider identifier '(Fab4321)'", || {
        Src::open(&path).unwrap();
    }));
    // folder: IFO (2 sectors) + VIDEO_TS.VOB (3 sectors) before the BUP; a header saying 99
    let dir = scratch("layout-folder").join("VIDEO_TS");
    std::fs::create_dir_all(&dir).unwrap();
    let mut ifo = vec![0u8; 2 * S];
    ifo[..12].copy_from_slice(b"DVDVIDEO-VMG");
    ifo[0x0c..0x10].copy_from_slice(&100u32.to_be_bytes());
    ifo[0x1c..0x20].copy_from_slice(&1u32.to_be_bytes());
    std::fs::write(dir.join("VIDEO_TS.IFO"), &ifo).unwrap();
    std::fs::write(dir.join("VIDEO_TS.VOB"), vec![0u8; 3 * S]).unwrap();
    let root = dir.parent().unwrap().to_path_buf();
    assert!(logged("BUP) 99 sectors after the IFO; the IFO and VOB files before it hold 5 sectors", || {
        Src::open(&root).unwrap();
    }));
    ifo[0x0c..0x10].copy_from_slice(&6u32.to_be_bytes());
    std::fs::write(dir.join("VIDEO_TS.IFO"), &ifo).unwrap();
    assert!(!logged("BUP) 5 sectors after the IFO", || {
        Src::open(&root).unwrap();
    }));
}

// ---- the title-set checks ----

const MAP_DISTRUSTED: c_int = 1;
const CELL_PAST_VOBS: c_int = 2;

/// A cell: first sector, last VOBU start, last sector (title-VOB sectors).
type Cell = (u32, u32, u32);

/// `VTS_01_0.IFO` with one PGC of `cells` (one program), a cell address table
/// of the same cells and a VOBU address map of `vobus`: `VTSI_MAT` in sector 0,
/// `VTS_PTT_SRPT` in 1, `VTS_PGCIT` in 2, `VTS_C_ADT` in 3, `VTS_VOBU_ADMAP` in 4.
fn vts_ifo(cells: &[Cell], vobus: &[u32], vob_sectors: u32) -> Vec<u8> {
    let mut d = vec![0u8; 5 * S];
    let be32 = |d: &mut Vec<u8>, at: usize, v: u32| d[at..at + 4].copy_from_slice(&v.to_be_bytes());
    let be16 = |d: &mut Vec<u8>, at: usize, v: u16| d[at..at + 2].copy_from_slice(&v.to_be_bytes());
    // VTSI_MAT
    d[..12].copy_from_slice(b"DVDVIDEO-VTS");
    be32(&mut d, 0x0c, 5 + vob_sectors + 5 - 1); // vts_last_sector: IFO, title VOBs, BUP
    be32(&mut d, 0x1c, 4); // vtsi_last_sector
    be32(&mut d, 0x80, 0x3ff); // vtsi_last_byte
    be32(&mut d, 0xc4, 5); // vtstt_vobs
    be32(&mut d, 0xc8, 1); // vts_ptt_srpt
    be32(&mut d, 0xcc, 2); // vts_pgcit
    be32(&mut d, 0xe0, 3); // vts_c_adt
    be32(&mut d, 0xe4, 4); // vts_vobu_admap
    // VTS_PTT_SRPT: one title, one chapter (PGC 1, program 1)
    let p = S;
    be16(&mut d, p, 1);
    be32(&mut d, p + 4, 15);
    be32(&mut d, p + 8, 12);
    be16(&mut d, p + 12, 1);
    be16(&mut d, p + 14, 1);
    // VTS_PGCIT: one PGC at byte 16
    let t = 2 * S;
    let pgc = t + 16;
    let n = u8::try_from(cells.len()).unwrap();
    let (map_off, play_off) = (0xec, 0xee);
    let pos_off = play_off + 24 * cells.len();
    let pgc_len = pos_off + 4 * cells.len();
    be16(&mut d, t, 1);
    be32(&mut d, t + 4, u32::try_from(16 + pgc_len - 1).unwrap());
    d[t + 8] = 0x81; // entry PGC of title 1
    be32(&mut d, t + 12, 16);
    d[pgc + 2] = 1; // nr_of_programs
    d[pgc + 3] = n; // nr_of_cells
    be16(&mut d, pgc + 0xe6, u16::try_from(map_off).unwrap());
    be16(&mut d, pgc + 0xe8, u16::try_from(play_off).unwrap());
    be16(&mut d, pgc + 0xea, u16::try_from(pos_off).unwrap());
    d[pgc + map_off] = 1; // program 1 starts at cell 1
    for (k, &(first, last_vobu, last)) in cells.iter().enumerate() {
        let c = pgc + play_off + 24 * k;
        be32(&mut d, c + 8, first);
        be32(&mut d, c + 16, last_vobu);
        be32(&mut d, c + 20, last);
        let q = pgc + pos_off + 4 * k;
        be16(&mut d, q, 1);
        d[q + 3] = u8::try_from(k + 1).unwrap();
    }
    // VTS_C_ADT
    let a = 3 * S;
    be16(&mut d, a, 1);
    be32(&mut d, a + 4, u32::try_from(8 + 12 * cells.len() - 1).unwrap());
    for (k, &(first, _, last)) in cells.iter().enumerate() {
        let e = a + 8 + 12 * k;
        be16(&mut d, e, 1);
        d[e + 2] = u8::try_from(k + 1).unwrap();
        be32(&mut d, e + 4, first);
        be32(&mut d, e + 8, last);
    }
    // VTS_VOBU_ADMAP
    let m = 4 * S;
    be32(&mut d, m, u32::try_from(4 + 4 * vobus.len() - 1).unwrap());
    for (k, v) in vobus.iter().enumerate() {
        be32(&mut d, m + 4 + 4 * k, *v);
    }
    d
}

/// The title-set checks on a folder holding `ifo` and title VOBs of `vob_sectors`.
fn check_vts(test: &str, cells: &[Cell], vobus: &[u32], vob_sectors: u32) -> c_int {
    let dir = scratch(test).join("VIDEO_TS");
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("VTS_01_0.IFO"), vts_ifo(cells, vobus, vob_sectors)).unwrap();
    std::fs::write(dir.join("VTS_01_1.VOB"), vec![0u8; vob_sectors as usize * S]).unwrap();
    let src = Src::open(dir.parent().unwrap()).unwrap();
    let dvd = src.reader();
    // SAFETY: the reader is open; the IFO handle is closed below.
    unsafe {
        let ifo = ifoOpen(dvd.0, 1);
        assert!(!ifo.is_null(), "libdvdread accepts the built IFO");
        let r = ff_dvdvideo_check_vts(std::ptr::null_mut(), dvd.0, 1, ifo);
        ifoClose(ifo);
        r
    }
}

const VOBUS: [u32; 4] = [0, 10, 20, 30];

#[test]
fn title_set_cells_on_the_vobu_map_and_inside_the_vobs() {
    assert_eq!(check_vts("vts-ok", &[(0, 10, 19), (20, 30, 39)], &VOBUS, 40), 0);
    // a cell that starts on no VOBU of the map
    assert_eq!(check_vts("vts-first", &[(0, 10, 19), (21, 30, 39)], &VOBUS, 40), MAP_DISTRUSTED);
    // a cell whose last VOBU is not in the map
    assert_eq!(check_vts("vts-last", &[(0, 15, 19), (20, 30, 39)], &VOBUS, 40), MAP_DISTRUSTED);
    // a cell that ends past the end of the title VOBs: its data is missing
    assert_eq!(check_vts("vts-past", &[(0, 10, 19), (20, 30, 45)], &VOBUS, 40), CELL_PAST_VOBS);
    // the last sector itself is the limit
    assert_eq!(check_vts("vts-edge", &[(0, 10, 19), (20, 30, 40)], &VOBUS, 40), CELL_PAST_VOBS);
    assert_eq!(check_vts("vts-both", &[(0, 10, 19), (25, 30, 45)], &VOBUS, 40), MAP_DISTRUSTED | CELL_PAST_VOBS);
}

// ---- folders ----

/// The files of a small DVD-Video folder: `VIDEO_TS.IFO` and two title VOBs.
fn folder_files() -> Vec<(&'static str, Vec<u8>)> {
    let pattern = |n: usize, seed: u8| (0..n).map(|i| u8::try_from(i % 253).unwrap() ^ seed).collect::<Vec<u8>>();
    vec![("VIDEO_TS.IFO", pattern(4096, 1)), ("VTS_01_1.VOB", pattern(3 * S, 2)), ("VTS_01_2.VOB", pattern(2 * S, 3))]
}

fn write_files(dir: &Path, lower: bool) {
    std::fs::create_dir_all(dir).unwrap();
    for (name, data) in folder_files() {
        let name = if lower { name.to_lowercase() } else { name.to_owned() };
        std::fs::write(dir.join(name), data).unwrap();
    }
}

#[test]
fn every_folder_layout_opens() {
    let files = folder_files();
    let disc = scratch("layouts");
    // A folder holding VIDEO_TS; the VIDEO_TS folder itself; a folder with the
    // files directly; lower-case names.
    write_files(&disc.join("Movie").join("VIDEO_TS"), false);
    write_files(&disc.join("Bare"), false);
    write_files(&disc.join("Lower").join("video_ts"), true);
    for root in [disc.join("Movie"), disc.join("Movie").join("VIDEO_TS"), disc.join("Bare"), disc.join("Lower")] {
        let src = Src::open(&root).unwrap();
        let dvd = src.reader();
        assert_eq!(dvd.read_bytes(0, DVD_READ_INFO_FILE, 4096), files[0].1, "{}", root.display());
        // The title VOBs are read as one: blocks 2-3 run from VTS_01_1 into VTS_01_2.
        let mut want = files[1].1[2 * S..].to_vec();
        want.extend_from_slice(&files[2].1[..S]);
        assert_eq!(dvd.read_title_blocks(1, 2, 2), want, "{}", root.display());
    }
}

// ---- CSS on a scrambled disc ----

/// libdvdcss's own decrypting read of `n` blocks from block `start` of `path`.
fn libdvdcss_read(path: &Path, start: c_int, n: usize) -> Vec<u8> {
    unsafe extern "C" fn seek(p: *mut c_void, pos: u64) -> c_int {
        // SAFETY: p is the File below.
        let f = unsafe { &mut *p.cast::<File>() };
        if f.seek(SeekFrom::Start(pos)).is_ok() { 0 } else { -1 }
    }
    unsafe extern "C" fn read(p: *mut c_void, buf: *mut c_void, len: c_int) -> c_int {
        // SAFETY: p is the File below; buf has room for len bytes.
        let (file, out) =
            unsafe { (&mut *p.cast::<File>(), std::slice::from_raw_parts_mut(buf.cast::<u8>(), usize::try_from(len).unwrap())) };
        let mut done = 0;
        while done < out.len() {
            match file.read(&mut out[done..]) {
                Ok(0) => break,
                Ok(k) => done += k,
                Err(_) => return -1,
            }
        }
        c_int::try_from(done).unwrap()
    }
    let mut file = File::open(path).unwrap();
    let mut cb = css::StreamCb { pf_seek: Some(seek), pf_read: Some(read), pf_readv: None };
    let mut out = vec![0u8; n * S];
    // SAFETY: file and cb outlive the handle; out has room for n blocks.
    unsafe {
        let h = css::dvdcss_open_stream_uncached((&raw mut file).cast(), &raw mut cb, None, std::ptr::null_mut());
        assert!(!h.is_null());
        assert_eq!(css::dvdcss_seek(h, start, css::SEEK_KEY), start);
        let got = css::dvdcss_read(h, out.as_mut_ptr().cast(), c_int::try_from(n).unwrap(), css::READ_DECRYPT);
        css::dvdcss_close(h);
        assert_eq!(got, c_int::try_from(n).unwrap());
    }
    out
}

const CSS_BLOCKS: usize = 4096;

/// Raw blocks of `file` from `start`, descrambled one by one by the source.
fn descrambled(src: &Src, file: &Path, start: u64, n: usize) -> (Vec<u8>, usize) {
    let mut raw = vec![0u8; n * S];
    let mut f = File::open(file).unwrap();
    f.seek(SeekFrom::Start(start * S as u64)).unwrap();
    f.read_exact(&mut raw).unwrap();
    let mut scrambled = 0;
    for block in raw.chunks_mut(S) {
        // SAFETY: the source is open; block has 2048 bytes.
        let r = unsafe { ff_dvdvideo_source_descramble(src.0, 1, 0, block.as_mut_ptr()) };
        assert!(r >= 0, "descramble failed: {r}");
        scrambled += usize::try_from(r).unwrap();
    }
    (raw, scrambled)
}

#[test]
fn corpus_css_image_equals_libdvdcss() {
    let Some(image) = std::env::var_os("DVDVIDEO_CSS_IMAGE").map(PathBuf::from) else {
        eprintln!("DVDVIDEO_CSS_IMAGE not set: skipped");
        return;
    };
    // Where VTS_01_1.VOB starts, as the chosen file system records it.
    let start = {
        let c = CString::new(image.to_str().unwrap()).unwrap();
        let (mut s, mut fs, mut f) = (std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut());
        // SAFETY: valid pointers; everything opened is closed below.
        unsafe {
            assert_eq!(discio::ff_discio_source_open_file(std::ptr::null_mut(), c.as_ptr(), &raw mut s), 0);
            assert_eq!(discio::ff_discio_mount_image(s, std::ptr::null(), &raw mut fs), 0);
            assert_eq!(((*(*fs).ops).open_file)(fs, c"/VIDEO_TS/VTS_01_1.VOB".as_ptr(), &raw mut f), 0);
            let start = (*(*f).extents).sector;
            discio::ff_discio_file_free(&raw mut f);
            discio::ff_discio_fs_close(&raw mut fs);
            discio::ff_discio_source_free(&raw mut s);
            start
        }
    };
    let src = Src::open(&image).unwrap();
    let (ours, scrambled) = descrambled(&src, &image, u64::try_from(start).unwrap(), CSS_BLOCKS);
    assert!(scrambled > 0, "no scrambled block in the sample");
    assert!(ours == libdvdcss_read(&image, c_int::try_from(start).unwrap(), CSS_BLOCKS), "image: blocks differ");
    eprintln!("image: {scrambled} of {CSS_BLOCKS} blocks descrambled, equal to libdvdcss");
}

#[test]
fn corpus_css_folder_equals_libdvdcss() {
    let Some(folder) = std::env::var_os("DVDVIDEO_CSS_FOLDER").map(PathBuf::from) else {
        eprintln!("DVDVIDEO_CSS_FOLDER not set: skipped");
        return;
    };
    let vob = folder.join("VIDEO_TS").join("VTS_01_1.VOB");
    let src = Src::open(&folder).unwrap();
    let (ours, scrambled) = descrambled(&src, &vob, 0, CSS_BLOCKS);
    assert!(scrambled > 0, "no scrambled block in the sample");
    assert!(ours == libdvdcss_read(&vob, 0, CSS_BLOCKS), "folder: blocks differ");
    eprintln!("folder: {scrambled} of {CSS_BLOCKS} blocks descrambled, equal to libdvdcss");
}

// ---- VOB groups ----

impl Src {
    /// Block `sector` of the menu VOBs (`menu`) or title VOBs of title set
    /// `vtsn`, or the error code.
    fn vob_block(&self, vtsn: c_int, menu: bool, sector: i64) -> Result<Vec<u8>, c_int> {
        let mut buf = vec![0u8; S];
        // SAFETY: the source is open; buf holds one block.
        let ret = unsafe { ff_dvdvideo_source_vob_read(self.0, vtsn, c_int::from(menu), sector, buf.as_mut_ptr(), 1) };
        if ret < 0 { Err(ret) } else { Ok(buf) }
    }
}

/// Image blocks of the files of title set 1 in the VOB-group images.
const VTS_IFO: u32 = VTS1; // 2 blocks
const MENU_VOB: u32 = PART_START + 40;
const TITLE_VOB: u32 = PART_START + 50;

/// Makes `sector` the first NAV pack of a VOB (its PCI and DSI name sector 0).
fn first_nav_pack(im: &mut Img, sector: u32) {
    let s = im.sector(sector);
    s.fill(0);
    s[..5].copy_from_slice(&[0, 0, 1, 0xba, 0x44]);
    s[0x0e..0x14].copy_from_slice(&[0, 0, 1, 0xbb, 0, 0x12]);
    s[0x26..0x2d].copy_from_slice(&[0, 0, 1, 0xbf, 0x03, 0xd4, 0]);
    s[0x400..0x407].copy_from_slice(&[0, 0, 1, 0xbf, 0x03, 0xfa, 1]);
}

/// An image with `VTS_01_0.VOB` (2 blocks) at `menu_file` and `VTS_01_1.VOB`
/// (2 blocks) at `title_file`; the VTS IFO header puts the menu VOBs `vtsm`
/// and the title VOBs `vtstt` blocks after the IFO. Every block from the
/// menu file on holds a pattern of its own.
fn vob_image(menu_file: u32, title_file: u32, vtsm: u32, vtstt: u32) -> Img {
    let mut files = dvd_files();
    files.push(("VTS_01_0.VOB", menu_file, 4096));
    files.push(("VTS_01_1.VOB", title_file, 4096));
    let mut im = bridge(Some(&udf102(2009, files)), dvd_files(), None);
    let h = im.sector(VTS_IFO);
    h[..12].copy_from_slice(b"DVDVIDEO-VTS");
    h[0xc0..0xc4].copy_from_slice(&vtsm.to_be_bytes());
    h[0xc4..0xc8].copy_from_slice(&vtstt.to_be_bytes());
    fill(&mut im, MENU_VOB, 520 - MENU_VOB, 0x6c);
    im
}

#[test]
fn image_vob_groups_reach_past_their_files() {
    // the IFO and the file system agree: the title VOBs run to the end of the
    // image (beyond VTS_01_1.VOB), the menu VOBs up to the title VOBs
    let im = vob_image(MENU_VOB, TITLE_VOB, MENU_VOB - VTS_IFO, TITLE_VOB - VTS_IFO);
    let src = Src::open(&write_image("vob-groups", &im)).unwrap();
    assert_eq!(src.vob_block(1, false, 0).unwrap(), sectors(&im, TITLE_VOB, 1));
    assert_eq!(src.vob_block(1, false, 100).unwrap(), sectors(&im, TITLE_VOB + 100, 1), "past the file");
    assert_eq!(src.vob_block(1, false, i64::from(519 - TITLE_VOB)).unwrap(), sectors(&im, 519, 1));
    assert_eq!(src.vob_block(1, false, i64::from(520 - TITLE_VOB)).err(), Some(EOF), "past the image");
    assert_eq!(src.vob_block(1, true, 9).unwrap(), sectors(&im, TITLE_VOB - 1, 1));
    assert_eq!(src.vob_block(1, true, 10).err(), Some(EOF), "the menu VOBs end where the title VOBs start");
}

#[test]
fn image_vob_start_when_the_ifo_and_the_file_system_disagree() {
    // the IFO says 4 blocks later than the file: the file's position holds the
    // first NAV pack of a VOB, the IFO's does not -> the file system's start
    let mut im = vob_image(MENU_VOB, TITLE_VOB, MENU_VOB - VTS_IFO, TITLE_VOB + 4 - VTS_IFO);
    first_nav_pack(&mut im, TITLE_VOB);
    let src = Src::open(&write_image("vob-start-fs", &im)).unwrap();
    assert!(logged("the IFO puts its title VOBs 20 blocks after the IFO, the file system 16 blocks after it", || {
        assert_eq!(src.vob_block(1, false, 0).unwrap(), sectors(&im, TITLE_VOB, 1));
    }));
    // both positions start a VOB: the IFO's wins
    first_nav_pack(&mut im, TITLE_VOB + 4);
    let src = Src::open(&write_image("vob-start-both", &im)).unwrap();
    assert_eq!(src.vob_block(1, false, 0).unwrap(), sectors(&im, TITLE_VOB + 4, 1));
    // the menu VOBs: neither position starts a VOB -> they start where the
    // title VOBs start, which is also their end: nothing to read
    let im = vob_image(MENU_VOB, TITLE_VOB, MENU_VOB + 2 - VTS_IFO, TITLE_VOB - VTS_IFO);
    let src = Src::open(&write_image("vob-start-none", &im)).unwrap();
    assert!(logged("neither position starts a VOB", || {
        assert_eq!(src.vob_block(1, true, 0).err(), Some(EOF));
    }));
}

#[test]
fn image_menu_vobs_starting_after_the_title_vobs_reach_to_the_end_of_the_image() {
    // the menu VOBs start 2 blocks after the title VOBs (the file is there too)
    let im = vob_image(TITLE_VOB + 2, TITLE_VOB, TITLE_VOB + 2 - VTS_IFO, TITLE_VOB - VTS_IFO);
    let src = Src::open(&write_image("vob-menu-late", &im)).unwrap();
    assert_eq!(src.vob_block(1, true, 0).unwrap(), sectors(&im, TITLE_VOB + 2, 1));
    assert_eq!(src.vob_block(1, true, 100).unwrap(), sectors(&im, TITLE_VOB + 102, 1));
}

#[test]
fn folder_vob_groups_are_their_files() {
    let dir = scratch("vob-folder");
    let vts = dir.join("VIDEO_TS");
    std::fs::create_dir_all(&vts).unwrap();
    let block = |b: u8| vec![b; S];
    std::fs::write(vts.join("VIDEO_TS.IFO"), [b"DVDVIDEO-VMG".as_slice(), &[0; S - 12]].concat()).unwrap();
    std::fs::write(vts.join("VTS_01_1.VOB"), [block(1), block(2)].concat()).unwrap();
    std::fs::write(vts.join("VTS_01_2.VOB"), [block(3), vec![4; 100]].concat()).unwrap();
    std::fs::write(vts.join("VTS_01_4.VOB"), block(9)).unwrap();
    let src = Src::open(&dir).unwrap();
    assert_eq!(src.vob_block(1, false, 2).unwrap(), block(3), "VTS_01_2.VOB follows VTS_01_1.VOB");
    assert_eq!(src.vob_block(1, false, 3).unwrap(), [vec![4; 100], vec![0xFF; S - 100]].concat(), "0xFF after the end");
    assert_eq!(src.vob_block(1, false, 4).err(), Some(EOF), "VTS_01_4.VOB is not reached: VTS_01_3.VOB is missing");
    assert_eq!(src.vob_block(1, true, 0).err(), Some(ENOENT), "no menu VOB");
}

/// A folder whose title VOBs are `parts` (in order, `VTS_01_1.VOB` on); a part
/// `None` is a directory of that name.
fn vob_folder(test: &str, parts: &[Option<Vec<u8>>]) -> PathBuf {
    let dir = scratch(test);
    let vts = dir.join("VIDEO_TS");
    std::fs::create_dir_all(&vts).unwrap();
    std::fs::write(vts.join("VIDEO_TS.IFO"), [b"DVDVIDEO-VMG".as_slice(), &[0; S - 12]].concat()).unwrap();
    for (k, part) in parts.iter().enumerate() {
        let path = vts.join(format!("VTS_01_{}.VOB", k + 1));
        match part {
            Some(data) => std::fs::write(path, data).unwrap(),
            None => std::fs::create_dir(path).unwrap(),
        }
    }
    dir
}

/// Blocks `from..from + n` of the joined bytes, 0xFF after their end.
fn joined_blocks(bytes: &[u8], from: usize, n: usize) -> Vec<u8> {
    let mut out = vec![0xFF; n * S];
    let end = bytes.len().min((from + n) * S);
    if from * S < end {
        out[..end - from * S].copy_from_slice(&bytes[from * S..end]);
    }
    out
}

#[test]
fn folder_vob_files_are_joined_as_bytes() {
    // 7000 bytes of pattern split into files of 3000 (not whole blocks), 2500
    // and 1500 bytes: the blocks are those of the bytes joined, a block may
    // hold the end of one file and the start of the next; 0xFF after the end
    let all: Vec<u8> = (0..7000).map(|i| u8::try_from(i % 251).unwrap()).collect();
    let dir = vob_folder("vob-joined", &[Some(all[..3000].to_vec()), Some(all[3000..5500].to_vec()),
                                         Some(all[5500..].to_vec())]);
    let src = Src::open(&dir).unwrap();
    for b in 0..4 {
        assert_eq!(src.vob_block(1, false, b).unwrap(), joined_blocks(&all, usize::try_from(b).unwrap(), 1), "block {b}");
    }
    assert_eq!(src.vob_block(1, false, 4).err(), Some(EOF));
    // libdvdread reads the title VOBs through the same blocks
    let dvd = src.reader();
    assert_eq!(dvd.read_title_blocks(1, 0, 4), joined_blocks(&all, 0, 4));
    assert_eq!(dvd.read_title_blocks(1, 1, 2), joined_blocks(&all, 1, 2));
}

#[test]
fn an_empty_vob_file_adds_nothing() {
    let all: Vec<u8> = (0..5000).map(|i| u8::try_from(i % 249).unwrap()).collect();
    let dir = vob_folder("vob-empty", &[Some(all[..2048].to_vec()), Some(vec![]), Some(all[2048..].to_vec())]);
    let src = Src::open(&dir).unwrap();
    assert!(logged("is empty: skipped", || {
        assert_eq!(src.vob_block(1, false, 1).unwrap(), joined_blocks(&all, 1, 1), "VTS_01_3.VOB follows VTS_01_1.VOB");
    }));
    assert_eq!(src.reader().read_title_blocks(1, 0, 3), joined_blocks(&all, 0, 3));
}

#[test]
fn a_vob_name_that_is_not_a_regular_file_ends_the_vob_files() {
    let dir = vob_folder("vob-dir", &[Some(vec![1; S]), None, Some(vec![3; S])]);
    let src = Src::open(&dir).unwrap();
    assert!(logged("is not a regular file: the VOB files of the title set end before it", || {
        assert_eq!(src.vob_block(1, false, 0).unwrap(), vec![1; S]);
    }));
    assert_eq!(src.vob_block(1, false, 1).err(), Some(EOF), "VTS_01_3.VOB is not reached");
}

#[test]
fn a_vob_file_that_cannot_be_opened_makes_the_title_vobs_unreadable() {
    use std::os::unix::fs::PermissionsExt;
    let dir = vob_folder("vob-noread", &[Some(vec![1; S]), Some(vec![2; S])]);
    let part2 = dir.join("VIDEO_TS").join("VTS_01_2.VOB");
    std::fs::set_permissions(&part2, std::fs::Permissions::from_mode(0o000)).unwrap();
    if File::open(&part2).is_ok() {
        eprintln!("skipped: the file stays readable (running as root?)");
        return;
    }
    let src = Src::open(&dir).unwrap();
    assert!(logged("the title VOBs of title set 1 cannot be read", || {
        assert!(src.vob_block(1, false, 0).is_err(), "not even VTS_01_1.VOB is read");
    }));
    let dvd = src.reader();
    // SAFETY: the reader is open.
    let f = unsafe { DVDOpenFile(dvd.0, 1, DVD_READ_TITLE_VOBS) };
    assert!(f.is_null(), "libdvdread has no title VOBs either");
    std::fs::set_permissions(&part2, std::fs::Permissions::from_mode(0o644)).unwrap();
}
