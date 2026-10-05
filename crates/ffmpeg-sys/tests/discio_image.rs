//! `libavformat/discio_fs.c`: the file system a disc image is read through
//! (`ff_discio_mount_image`), the DVD-Video check, the label copy rule and the
//! disc format probe, on images built here byte by byte: ISO 9660 / Joliet
//! trees (ECMA-119) with or without a UDF volume (ECMA-167 / OSTA UDF) over
//! the same sectors, as on DVD-Video bridge discs. With
//! `DISCIO_FS_REFERENCE=<file>` (tab-separated: id, image path, disc format,
//! file system, label), every listed image must give the recorded choice
//! with either UDF reader.

#![allow(clippy::many_single_char_names, reason = "one short name per descriptor being built")]

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::Path;

use ffmpeg_sys::discio::{self, Fs, ImageOptions, Source, SourceOps};

mod common;
use common::*;

const ENOENT: c_int = -2;
const EIO: c_int = -5;
const EINVAL: c_int = -22;
const INVALIDDATA: c_int = -0x4144_4E49; // FFERRTAG('I','N','D','A')

// ---- running the C code ----

/// An image in memory; reads touching a sector in `bad` fail.
struct Mem {
    d: Vec<u8>,
    bad: Vec<u32>,
}

unsafe extern "C" fn mem_read(opaque: *mut c_void, pos: i64, buf: *mut u8, len: c_int) -> c_int {
    // SAFETY: opaque is the boxed Mem of `Image::memory`.
    let m = unsafe { &*opaque.cast::<Mem>() };
    let pos = usize::try_from(pos).unwrap();
    let len = usize::try_from(len).unwrap();
    if (pos / S..(pos + len).div_ceil(S)).any(|s| m.bad.contains(&u32::try_from(s).unwrap())) {
        return EIO;
    }
    if pos >= m.d.len() {
        return 0;
    }
    let n = (m.d.len() - pos).min(len);
    // SAFETY: buf has room for len bytes.
    unsafe { std::ptr::copy_nonoverlapping(m.d.as_ptr().add(pos), buf, n) };
    c_int::try_from(n).unwrap()
}

unsafe extern "C" fn mem_close(opaque: *mut c_void) {
    // SAFETY: opaque came from Box::into_raw in `Image::memory`.
    drop(unsafe { Box::from_raw(opaque.cast::<Mem>()) });
}

static MEM_OPS: SourceOps = SourceOps { read_at: mem_read, close: Some(mem_close) };

struct Image {
    src: *mut Source,
}

impl Image {
    fn memory(im: &Img) -> Self {
        let size = i64::try_from(im.d.len()).unwrap();
        let opaque = Box::into_raw(Box::new(Mem { d: im.d.clone(), bad: im.bad.clone() })).cast::<c_void>();
        // SAFETY: MEM_OPS outlives the source; opaque is released through it.
        let src = unsafe {
            discio::ff_discio_source_new(std::ptr::null_mut(), c"memory".as_ptr(), size, &raw const MEM_OPS, opaque)
        };
        assert!(!src.is_null());
        Image { src }
    }

    fn open(path: &Path) -> Self {
        let c = CString::new(path.to_str().unwrap()).unwrap();
        let mut src = std::ptr::null_mut();
        // SAFETY: valid path and out pointer.
        let ret = unsafe { discio::ff_discio_source_open_file(std::ptr::null_mut(), c.as_ptr(), &raw mut src) };
        assert_eq!(ret, 0, "opening {}: {}", path.display(), ffmpeg_sys::error_text(ret));
        Image { src }
    }

    fn mount_with(self, opts: &ImageOptions) -> Result<Vol, c_int> {
        let mut fs = std::ptr::null_mut();
        // SAFETY: src is open; opts is a valid DiscIOImageOptions.
        let ret = unsafe { discio::ff_discio_mount_image(self.src, opts, &raw mut fs) };
        if ret < 0 {
            Err(ret)
        } else {
            Ok(Vol { fs, _image: self })
        }
    }

    fn mount(self) -> Result<Vol, c_int> {
        let mut fs = std::ptr::null_mut();
        // SAFETY: src is open; NULL options = the defaults.
        let ret = unsafe { discio::ff_discio_mount_image(self.src, std::ptr::null(), &raw mut fs) };
        if ret < 0 {
            Err(ret)
        } else {
            Ok(Vol { fs, _image: self })
        }
    }

    /// One file system mounted directly: ISO 9660, Joliet or a UDF reader.
    fn mount_one(self, kind: &str) -> Vol {
        let mut fs = std::ptr::null_mut();
        // SAFETY: src is open.
        let ret = unsafe {
            match kind {
                "iso" => discio::ff_discio_iso9660_mount(self.src, 0, &raw mut fs),
                "joliet" => discio::ff_discio_iso9660_mount(self.src, 1, &raw mut fs),
                "netbsd" => discio::ff_discio_udf_netbsd_mount(self.src, &raw mut fs),
                _ => discio::ff_discio_udf_linux_mount(self.src, &raw mut fs),
            }
        };
        assert_eq!(ret, 0, "mounting {kind}: {}", ffmpeg_sys::error_text(ret));
        Vol { fs, _image: self }
    }
}

impl Drop for Image {
    fn drop(&mut self) {
        // SAFETY: src came from ff_discio_source_new / _open_file.
        unsafe { discio::ff_discio_source_free(&raw mut self.src) };
    }
}

/// A mounted file system with its image (closed before the image is freed).
struct Vol {
    fs: *mut Fs,
    _image: Image,
}

impl Vol {
    fn name(&self) -> String {
        // SAFETY: ops and its name are static.
        unsafe { CStr::from_ptr((*(*self.fs).ops).name) }.to_string_lossy().into_owned()
    }
    fn kind(&self) -> c_int {
        // SAFETY: ops is static.
        unsafe { (*(*self.fs).ops).kind }
    }
    fn label(&self) -> String {
        // SAFETY: label is NUL-terminated within its room.
        unsafe { CStr::from_ptr((*self.fs).label.as_ptr()) }.to_string_lossy().into_owned()
    }
    fn check(&self) -> bool {
        // SAFETY: fs is mounted.
        let r = unsafe { discio::ff_discio_dvd_video_check(self.fs) };
        assert!(r == 0 || r == 1, "check returned {r}");
        r == 1
    }
    fn format(&self) -> c_int {
        // SAFETY: fs is mounted.
        unsafe { discio::ff_discio_disc_format(self.fs) }
    }
    fn find_dir(&self, path: &str) -> c_int {
        let c = CString::new(path).unwrap();
        // SAFETY: fs is mounted; c is a valid path.
        unsafe { ((*(*self.fs).ops).find_dir)(self.fs, c.as_ptr()) }
    }
}

impl Drop for Vol {
    fn drop(&mut self) {
        // SAFETY: fs came from a mount.
        unsafe { discio::ff_discio_fs_close(&raw mut self.fs) };
    }
}

fn chosen(im: &Img) -> (String, String) {
    let v = Image::memory(im).mount().unwrap();
    (v.name(), v.label())
}

fn opts(udf_reader: c_int, force: bool) -> ImageOptions {
    ImageOptions { udf_reader, prefer_iso_for_old_udf102: c_int::from(force) }
}

// ---- the choice with a UDF volume ----

#[test]
fn a_udf_volume_of_another_revision_is_kept_without_the_check() {
    let u = Udf { label: "UDF_DISC_LABEL", revision: 0x0150, year: 2004, files: broken_files(), pieces: Vec::new() };
    let im = bridge(Some(&u), dvd_files(), None);
    let v = Image::memory(&im).mount().unwrap();
    assert_eq!((v.name().as_str(), v.kind(), v.label().as_str()), ("UDF (NetBSD)", discio::FS_UDF, "UDF_DISC_LABEL"));
    assert!(!v.check(), "the UDF tree fails the check it was not given");
}

#[test]
fn a_recent_valid_udf_102_volume_is_kept() {
    let im = bridge(Some(&udf102(2009, dvd_files())), dvd_files(), None);
    assert_eq!(chosen(&im), ("UDF (NetBSD)".into(), "UDF_DISC_LABEL".into()));
}

#[test]
fn an_old_udf_102_volume_prefers_iso_9660_with_the_udf_label() {
    let im = bridge(Some(&udf102(2004, dvd_files())), dvd_files(), None);
    let v = Image::memory(&im).mount().unwrap();
    assert_eq!((v.name().as_str(), v.kind(), v.label().as_str()), ("ISO 9660", discio::FS_ISO9660, "UDF_DISC_LABEL"));
    // 2005 is still before 2006; 2006 is not.
    let im = bridge(Some(&udf102(2005, dvd_files())), dvd_files(), None);
    assert_eq!(chosen(&im).0, "ISO 9660");
    let im = bridge(Some(&udf102(2006, dvd_files())), dvd_files(), None);
    assert_eq!(chosen(&im).0, "UDF (NetBSD)");
}

#[test]
fn the_option_off_keeps_an_old_valid_udf_102_volume() {
    let im = bridge(Some(&udf102(2004, dvd_files())), dvd_files(), None);
    let v = Image::memory(&im).mount_with(&opts(discio::UDF_NETBSD, false)).unwrap();
    assert_eq!(v.name(), "UDF (NetBSD)");
}

#[test]
fn a_udf_102_volume_failing_the_check_gives_way_to_iso_9660() {
    let im = bridge(Some(&udf102(2009, broken_files())), dvd_files(), None);
    assert_eq!(chosen(&im), ("ISO 9660".into(), "UDF_DISC_LABEL".into()));
    // The option off changes nothing here: the UDF volume was checked.
    let v = Image::memory(&im).mount_with(&opts(discio::UDF_NETBSD, false)).unwrap();
    assert_eq!(v.name(), "ISO 9660");
}

#[test]
fn joliet_comes_after_iso_9660() {
    let im = bridge(Some(&udf102(2004, dvd_files())), broken_files(), Some(dvd_files()));
    assert_eq!(chosen(&im), ("Joliet".into(), "UDF_DISC_LABEL".into()));
    let v = Image::memory(&im).mount().unwrap();
    assert_eq!(v.kind(), discio::FS_JOLIET);
}

#[test]
fn the_udf_volume_is_kept_unchecked_when_nothing_else_passes() {
    let im = bridge(Some(&udf102(2004, broken_files())), broken_files(), Some(broken_files()));
    assert_eq!(chosen(&im), ("UDF (NetBSD)".into(), "UDF_DISC_LABEL".into()));
}

#[test]
fn a_short_udf_label_leaves_the_iso_9660_label() {
    // Over "ISO_LABEL" (9 bytes, half = 4): "UDF_DISC_LABEL" (14) and "UDF_"
    // (4) are copied, "UDF" (3) is not.
    for (udf_label, want) in [("UDF_DISC_LABEL", "UDF_DISC_LABEL"), ("UDF_", "UDF_"), ("UDF", "ISO_LABEL")] {
        let u = Udf { label: udf_label, ..udf102(2004, dvd_files()) };
        let im = bridge(Some(&u), dvd_files(), None);
        assert_eq!(chosen(&im), ("ISO 9660".into(), want.into()), "UDF label {udf_label:?}");
    }
}

#[test]
fn the_linux_based_reader_is_chosen_by_the_option() {
    let linux = opts(discio::UDF_LINUX, true);
    let im = bridge(Some(&udf102(2009, dvd_files())), dvd_files(), None);
    let v = Image::memory(&im).mount_with(&linux).unwrap();
    assert_eq!((v.name().as_str(), v.kind(), v.label().as_str()), ("UDF (Linux)", discio::FS_UDF, "UDF_DISC_LABEL"));
    let im = bridge(Some(&udf102(2004, dvd_files())), dvd_files(), None);
    let v = Image::memory(&im).mount_with(&linux).unwrap();
    assert_eq!((v.name().as_str(), v.label().as_str()), ("ISO 9660", "UDF_DISC_LABEL"));
    let im = bridge(Some(&udf102(2009, broken_files())), dvd_files(), None);
    assert_eq!(Image::memory(&im).mount_with(&linux).unwrap().name(), "ISO 9660");
}

#[test]
fn an_unknown_udf_reader_is_refused() {
    let im = bridge(Some(&udf102(2009, dvd_files())), dvd_files(), None);
    assert_eq!(Image::memory(&im).mount_with(&opts(7, true)).err(), Some(EINVAL));
}

// ---- the choice without a UDF volume ----

#[test]
fn without_udf_a_valid_iso_9660_volume_is_used() {
    let im = bridge(None, dvd_files(), Some(dvd_files()));
    assert_eq!(chosen(&im), ("ISO 9660".into(), "ISO_LABEL".into()));
}

#[test]
fn without_udf_joliet_is_used_when_only_it_passes() {
    let im = bridge(None, broken_files(), Some(dvd_files()));
    assert_eq!(chosen(&im), ("Joliet".into(), "JOLIET_LABEL".into()));
}

#[test]
fn without_udf_iso_9660_is_kept_unchecked_when_nothing_passes() {
    let im = bridge(None, broken_files(), Some(broken_files()));
    assert_eq!(chosen(&im), ("ISO 9660".into(), "ISO_LABEL".into()));
    let im = bridge(None, broken_files(), None);
    assert_eq!(chosen(&im).0, "ISO 9660");
}

#[test]
fn no_file_system_at_all() {
    assert_eq!(Image::memory(&Img::new()).mount().err(), Some(INVALIDDATA));
}

// ---- the DVD-Video check ----

fn iso_only(files: Vec<F>) -> Img {
    let mut im = bridge(None, files, None);
    im.vmg(VMG, b"DVDVIDEO-VMG", 1);
    im
}

fn check_iso(im: &Img) -> bool {
    Image::memory(im).mount_one("iso").check()
}

#[test]
fn dvd_video_check_rules() {
    assert!(check_iso(&iso_only(dvd_files())));
    // No VIDEO_TS directory: not DVD-Video, passes.
    let mut im = Img::new();
    iso(&mut im, "ISO_LABEL", &[("OTHER", vec![])], None);
    assert!(check_iso(&im));
    // VIDEO_TS.IFO missing; empty (zero sectors).
    assert!(!check_iso(&iso_only(vec![("VTS_01_0.IFO", VTS1, 4096)])));
    assert!(!check_iso(&iso_only(vec![("VIDEO_TS.IFO", VMG, 0), ("VTS_01_0.IFO", VTS1, 4096)])));
    // Wrong identifier.
    let mut im = iso_only(dvd_files());
    im.vmg(VMG, b"DVDVIDEO-VTS", 1);
    assert!(!check_iso(&im));
    // Title set counts 0 and 100 are out of range; 2 needs VTS_02_0.IFO.
    for n in [0, 100, 2] {
        let mut im = iso_only(dvd_files());
        im.vmg(VMG, b"DVDVIDEO-VMG", n);
        assert!(!check_iso(&im), "{n} title sets");
    }
    let mut im =
        iso_only(vec![("VIDEO_TS.IFO", VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096), ("VTS_02_0.IFO", VTS1, 2048)]);
    im.vmg(VMG, b"DVDVIDEO-VMG", 2);
    assert!(check_iso(&im));
    // VTS IFO of zero sectors.
    assert!(!check_iso(&iso_only(vec![("VIDEO_TS.IFO", VMG, 4096), ("VTS_01_0.IFO", VTS1, 0)])));
    // VIDEO_TS.IFO unreadable: VIDEO_TS.BUP is read instead, and checked the
    // same way.
    let mut im = iso_only(dvd_files());
    im.bad.push(VMG);
    assert!(check_iso(&im));
    im.vmg(BUP, b"XXXXXXXXXXXX", 1);
    assert!(!check_iso(&im), "the backup copy is checked the same way");
    // Unreadable and no separate backup: fails.
    let mut im = iso_only(vec![("VIDEO_TS.IFO", VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096)]);
    im.bad.push(VMG);
    assert!(!check_iso(&im));
    // A backup at the same sector as the IFO is no second chance.
    let mut im = iso_only(vec![("VIDEO_TS.IFO", VMG, 4096), ("VIDEO_TS.BUP", VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096)]);
    im.bad.push(VMG);
    assert!(!check_iso(&im));
}

#[test]
fn dvd_video_check_reads_through_every_reader() {
    let im = bridge(Some(&udf102(2009, dvd_files())), dvd_files(), Some(dvd_files()));
    for kind in ["iso", "joliet", "netbsd", "linux"] {
        assert!(Image::memory(&im).mount_one(kind).check(), "{kind}");
    }
    let im = bridge(Some(&udf102(2009, broken_files())), broken_files(), Some(broken_files()));
    for kind in ["iso", "joliet", "netbsd", "linux"] {
        assert!(!Image::memory(&im).mount_one(kind).check(), "{kind}");
    }
}

#[test]
fn every_reader_finds_directories_only() {
    let im = bridge(Some(&udf102(2009, dvd_files())), dvd_files(), Some(dvd_files()));
    for kind in ["iso", "joliet", "netbsd", "linux"] {
        let v = Image::memory(&im).mount_one(kind);
        assert_eq!(v.find_dir("/"), 0, "{kind}");
        assert_eq!(v.find_dir("/VIDEO_TS"), 0, "{kind}");
        assert_eq!(v.find_dir("/NOPE"), ENOENT, "{kind}");
        assert_eq!(v.find_dir("/VIDEO_TS/VIDEO_TS.IFO"), ENOENT, "{kind}: a file is not a directory");
        assert_eq!(v.find_dir("/NOPE/VIDEO_TS"), ENOENT, "{kind}");
    }
}

// ---- the label copy rule ----

fn copied(dst: &[u8], src: &[u8]) -> Vec<u8> {
    let mut d = [0 as c_char; discio::LABEL_SIZE];
    for (i, &b) in dst.iter().enumerate() {
        d[i] = c_char::from_ne_bytes([b]);
    }
    let s = CString::new(src).unwrap();
    // SAFETY: d has DISCIO_LABEL_SIZE bytes and holds a NUL-terminated label;
    // s is NUL-terminated.
    unsafe { discio::ff_discio_label_copy(d.as_mut_ptr(), s.as_ptr()) };
    // SAFETY: the result is NUL-terminated within d.
    unsafe { CStr::from_ptr(d.as_ptr()) }.to_bytes().to_vec()
}

#[test]
fn label_copied_when_at_least_half_as_long() {
    assert_eq!(copied(b"LABYRINTH_1", b"U"), b"LABYRINTH_1");
    assert_eq!(copied(b"LABYRINTH_1", b"UDFLB"), b"UDFLB", "5 >= 11 / 2");
    assert_eq!(copied(b"LABYRINTH_1", b"UDFL"), b"LABYRINTH_1");
    assert_eq!(copied(b"", b""), b"");
    assert_eq!(copied(b"ISO", b""), b"ISO", "0 < 3 / 2");
    assert_eq!(copied(b"IS", b"U"), b"U", "1 >= 2 / 2");
}

#[test]
fn long_labels_are_kept_whole() {
    // Decision for this program: a label is never emptied for its length.
    // UDF labels decode to at most 258 bytes; all of the room is usable.
    for n in [160, 161, 200, 258, 259] {
        assert_eq!(copied(b"", &vec![b'A'; n]), vec![b'A'; n], "{n} bytes");
    }
    // 54 three-byte characters (162 bytes), the shortest non-Latin label a
    // 161-byte room would empty.
    let s = "\u{3042}".repeat(54);
    assert_eq!(copied(b"", s.as_bytes()), s.as_bytes());
    // Past the room: cut at a whole character.
    let s = "\u{3042}".repeat(90);
    assert_eq!(copied(b"", s.as_bytes()), "\u{3042}".repeat(86).as_bytes(), "258 of 270 bytes");
}

#[test]
fn labels_are_cut_at_the_first_invalid_sequence() {
    // A lone surrogate encoded on its own (ED A0 80) is not valid UTF-8.
    assert_eq!(copied(b"", b"AB\xed\xa0\x80C"), b"AB");
    assert_eq!(copied(b"", b"A\xc3"), b"A", "a lead byte at the end");
    assert_eq!(copied(b"", b"A\xe9B"), b"A", "a Latin-1 byte");
    assert_eq!(copied(b"", "Ä€😀".as_bytes()), "Ä€😀".as_bytes());
    assert_eq!(copied(b"", b"A\xc0\x80"), b"A", "overlong");
    assert_eq!(copied(b"", b"A\xf8\x88\x80\x80\x80"), b"A", "5-byte form");
    assert_eq!(copied(b"", b"A\xf4\x90\x80\x80"), b"A", "above U+10FFFF");
    assert_eq!(copied(b"", b"A\xf4\x8f\xbf\xbf"), b"A\xf4\x8f\xbf\xbf", "U+10FFFF");
    // After ED / F4 only the upper limit of the second byte is checked.
    assert_eq!(copied(b"", b"A\xed\x61\xb0"), b"A\xed\x61\xb0");
}

// ---- the disc format ----

fn format_of(dirs: &[D]) -> c_int {
    let mut im = Img::new();
    iso(&mut im, "ISO_LABEL", dirs, None);
    Image::memory(&im).mount_one("iso").format()
}

#[test]
fn disc_format_from_the_files_present() {
    let f = |name| (name, 50, 100);
    assert_eq!(format_of(&[("BDMV", vec![f("index.bdmv"), f("MovieObject.bdmv")])]), discio::DISC_BLURAY);
    assert_eq!(format_of(&[("BDMV", vec![f("INDEX.BDM"), f("MOVIEOBJ.BDM")])]), discio::DISC_BLURAY);
    assert_eq!(format_of(&[("BDMV", vec![f("index.bdmv")])]), discio::DISC_NONE, "index alone");
    assert_eq!(format_of(&[("BDAV", vec![f("info.bdav")])]), discio::DISC_BLURAY);
    assert_eq!(
        format_of(&[("ADV_OBJ", vec![f("DISCID.DAT")]), ("HVDVD_TS", vec![f("HVA00001.VTI")])]),
        discio::DISC_HDDVD
    );
    assert_eq!(
        format_of(&[("ADV_OBJ", vec![f("DISCID.DAT")]), ("HDDVD_TS", vec![f("HVA00001.VTI")])]),
        discio::DISC_HDDVD
    );
    assert_eq!(
        format_of(&[("HVDVD_TS", vec![f("HVA00001.VTI")]), ("VIDEO_TS", vec![f("VIDEO_TS.IFO")])]),
        discio::DISC_DVD,
        "an HD DVD title set without DISCID.DAT"
    );
    assert_eq!(format_of(&[("VIDEO_TS", vec![f("VIDEO_TS.IFO")])]), discio::DISC_DVD);
    assert_eq!(
        format_of(&[("BDMV", vec![f("index.bdmv"), f("MovieObject.bdmv")]), ("VIDEO_TS", vec![f("VIDEO_TS.IFO")])]),
        discio::DISC_BLURAY,
        "Blu-ray first"
    );
    assert_eq!(format_of(&[("VIDEO_TS", vec![f("VTS_01_0.IFO")])]), discio::DISC_NONE);
}

// ---- the corpus against the reference record ----

#[test]
fn corpus_choice_matches_the_reference() {
    let Some(reference) = std::env::var_os("DISCIO_FS_REFERENCE") else {
        eprintln!("DISCIO_FS_REFERENCE not set: corpus check skipped");
        return;
    };
    let text = std::fs::read_to_string(&reference).unwrap();
    let (mut ok, mut skipped, mut bad) = (0, 0, Vec::new());
    for line in text.lines().filter(|l| !l.is_empty() && !l.starts_with('#')) {
        let cols: Vec<&str> = line.split('\t').collect();
        let [id, path, format, fs, label] = cols[..] else { panic!("bad reference line {line:?}") };
        if !Path::new(path).exists() {
            eprintln!("SKIP {id}: {path} not found");
            skipped += 1;
            continue;
        }
        for (reader, udf) in [("NetBSD", discio::UDF_NETBSD), ("Linux", discio::UDF_LINUX)] {
            let v = Image::open(Path::new(path))
                .mount_with(&opts(udf, true))
                .unwrap_or_else(|e| panic!("{id}: {}", ffmpeg_sys::error_text(e)));
            let got_fs = match v.kind() {
                discio::FS_UDF => "UDF",
                discio::FS_ISO9660 => "ISO 9660",
                _ => "Joliet",
            };
            let got_format = match v.format() {
                discio::DISC_DVD => "dvd",
                discio::DISC_BLURAY => "bluray",
                discio::DISC_HDDVD => "hddvd",
                _ => "none",
            };
            let got = (got_format, got_fs, v.label());
            eprintln!(
                "{} {id} ({reader}-based UDF reader): {got:?}",
                if got == (format, fs, label.to_owned()) { "OK " } else { "BAD" }
            );
            if got == (format, fs, label.to_owned()) {
                ok += 1;
            } else {
                bad.push(format!("{id} ({reader}): got {got:?}, want {:?}", (format, fs, label)));
            }
        }
    }
    eprintln!("{ok} OK, {} BAD, {skipped} skipped", bad.len());
    assert!(bad.is_empty(), "{bad:#?}");
    assert!(ok > 0, "no image of the reference was checked");
}
