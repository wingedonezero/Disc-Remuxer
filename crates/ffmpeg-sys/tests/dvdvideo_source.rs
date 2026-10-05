//! `libavformat/dvdvideo_source.c`: the DVD-Video demuxer's disc source. libdvdread
//! opens the disc through our file callbacks (`DVDOpenFiles`), so its files come
//! from the file system our choice rule picked (images) or from the disc folder,
//! read through our disc readers; images that are not DVD-Video, or whose DVD
//! files are scattered, are refused. With `DVDVIDEO_CSS_IMAGE` / `DVDVIDEO_CSS_FOLDER`
//! (a CSS-scrambled disc as an image / as a folder of its files), blocks
//! descrambled by the source equal libdvdcss's own decrypting read.

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
    fn DVDOpenFiles(priv_: *mut c_void, logcb: *const c_void, path: *const c_char, fs: *mut Files) -> *mut Reader;
    fn DVDClose(dvd: *mut Reader);
    fn DVDOpenFile(dvd: *mut Reader, title: c_int, domain: c_int) -> *mut DvdFile;
    fn DVDCloseFile(file: *mut DvdFile);
    fn DVDReadBytes(file: *mut DvdFile, buf: *mut c_void, size: usize) -> isize;
    fn DVDReadBlocks(file: *mut DvdFile, offset: c_int, count: usize, buf: *mut u8) -> isize;
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
