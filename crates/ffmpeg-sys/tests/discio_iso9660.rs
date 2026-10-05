//! `libavformat/discio_iso9660.c`: ISO 9660 / Joliet volumes, built here byte
//! by byte (ECMA-119), and the corpus images when `DISCIO_CORPUS` names the
//! folder holding them.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};

use ffmpeg_sys::discio::{self, Extent, File, Fs, Source};

const ENOENT: c_int = -2;
const INVALIDDATA: c_int = -0x4144_4E49; // FFERRTAG('I','N','D','A')

// ---- building images ----

fn put_both16(b: &mut [u8], v: u16) {
    b[..2].copy_from_slice(&v.to_le_bytes());
    b[2..4].copy_from_slice(&v.to_be_bytes());
}

fn put_both32(b: &mut [u8], v: u32) {
    b[..4].copy_from_slice(&v.to_le_bytes());
    b[4..8].copy_from_slice(&v.to_be_bytes());
}

/// One directory record.
fn record(id: &[u8], loc: u32, len: u32, flags: u8) -> Vec<u8> {
    let mut r = vec![0u8; 33 + id.len() + usize::from(id.len() % 2 == 0)];
    r[0] = u8::try_from(r.len()).unwrap();
    put_both32(&mut r[2..10], loc);
    put_both32(&mut r[10..18], len);
    r[25] = flags;
    put_both16(&mut r[28..32], 1);
    r[32] = u8::try_from(id.len()).unwrap();
    r[33..33 + id.len()].copy_from_slice(id);
    r
}

fn ucs2(s: &str) -> Vec<u8> {
    s.encode_utf16().flat_map(u16::to_be_bytes).collect()
}

/// A volume descriptor: type, label field (32 bytes) and root record.
fn descriptor(kind: u8, label: &[u8; 32], root: &[u8]) -> [u8; 2048] {
    let mut d = [0u8; 2048];
    d[0] = kind;
    d[1..6].copy_from_slice(b"CD001");
    d[6] = 1;
    d[0x28..0x48].copy_from_slice(label);
    put_both16(&mut d[0x80..0x84], 2048);
    d[156..156 + root.len()].copy_from_slice(root);
    d[813..829].copy_from_slice(b"2004030112000000");
    d
}

const IFO_SIZE: u32 = 3000;

/// Sectors: 16 PVD, 17 SVD, 18 terminator, 20/21 root (ISO/Joliet),
/// 22/23 `VIDEO_TS` (ISO/Joliet), 24-25 `VIDEO_TS.IFO` data.
fn image() -> Vec<u8> {
    let mut img = vec![0u8; 26 * 2048];
    let at = |s: usize| s * 2048;
    let dot = |loc| record(&[0], loc, 2048, 2);
    let dotdot = |loc| record(&[1], loc, 2048, 2);

    let mut root = dot(20);
    root.extend(dotdot(20));
    root.extend(record(b"VIDEO_TS", 22, 2048, 2));
    img[at(20)..at(20) + root.len()].copy_from_slice(&root);
    let mut vts = dot(22);
    vts.extend(dotdot(20));
    vts.extend(record(b"VIDEO_TS.IFO;1", 24, IFO_SIZE, 0));
    vts.extend(record(b"README.TXT;1", 25, 10, 1)); // hidden flag
    img[at(22)..at(22) + vts.len()].copy_from_slice(&vts);

    let mut jroot = dot(21);
    jroot.extend(dotdot(21));
    jroot.extend(record(&ucs2("VIDEO_TS"), 23, 2048, 2));
    img[at(21)..at(21) + jroot.len()].copy_from_slice(&jroot);
    let mut jvts = dot(23);
    jvts.extend(dotdot(21));
    jvts.extend(record(&ucs2("VIDEO_TS.IFO;1"), 24, IFO_SIZE, 0));
    img[at(23)..at(23) + jvts.len()].copy_from_slice(&jvts);

    for (i, b) in img[at(24)..at(24) + IFO_SIZE as usize].iter_mut().enumerate() {
        *b = u8::try_from(i % 253).unwrap();
    }
    let mut label = [b' '; 32];
    label[..9].copy_from_slice(b"TEST_DISC");
    label[9] = 0x80; // Windows-1252: euro sign
    img[at(16)..at(17)].copy_from_slice(&descriptor(1, &label, &record(&[0], 20, 2048, 2)));
    let mut jlabel = [0u8; 32];
    let j = ucs2("Joliet Disc");
    jlabel[..j.len()].copy_from_slice(&j);
    for k in (j.len()..32).step_by(2) {
        jlabel[k..k + 2].copy_from_slice(&0x20u16.to_be_bytes());
    }
    img[at(17)..at(18)].copy_from_slice(&descriptor(2, &jlabel, &record(&[0], 21, 2048, 2)));
    img[at(18)] = 0xff;
    img[at(18) + 1..at(18) + 6].copy_from_slice(b"CD001");
    img
}

// ---- running the reader ----

struct Image {
    _path: PathBuf,
    src: *mut Source,
}

impl Image {
    fn open(path: &Path) -> Self {
        let c = CString::new(path.to_str().unwrap()).unwrap();
        let mut src = std::ptr::null_mut();
        // SAFETY: valid path and out pointer.
        let ret = unsafe { discio::ff_discio_source_open_file(std::ptr::null_mut(), c.as_ptr(), &raw mut src) };
        assert_eq!(ret, 0, "opening {}: {}", path.display(), ffmpeg_sys::error_text(ret));
        Image { _path: path.to_path_buf(), src }
    }

    fn write(test: &str, bytes: &[u8]) -> Self {
        let dir = std::env::temp_dir().join(format!("discio-iso-{test}-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("image.iso");
        std::fs::write(&path, bytes).unwrap();
        Self::open(&path)
    }

    fn mount(&self, joliet: bool) -> Result<Vol, c_int> {
        let mut fs = std::ptr::null_mut();
        // SAFETY: src is open.
        let ret = unsafe { discio::ff_discio_iso9660_mount(self.src, c_int::from(joliet), &raw mut fs) };
        if ret < 0 { Err(ret) } else { Ok(Vol { fs }) }
    }
}

impl Drop for Image {
    fn drop(&mut self) {
        // SAFETY: src came from ff_discio_source_open_file.
        unsafe { discio::ff_discio_source_free(&raw mut self.src) };
    }
}

struct Vol {
    fs: *mut Fs,
}

impl Vol {
    fn label(&self) -> String {
        // SAFETY: label is NUL-terminated within its room.
        unsafe { CStr::from_ptr((*self.fs).label.as_ptr()) }.to_string_lossy().into_owned()
    }

    fn open(&self, path: &str) -> Result<(i64, Vec<Extent>), c_int> {
        let c = CString::new(path).unwrap();
        let mut f: *mut File = std::ptr::null_mut();
        // SAFETY: fs is mounted; ops is its table.
        let ret = unsafe { ((*(*self.fs).ops).open_file)(self.fs, c.as_ptr(), &raw mut f) };
        if ret < 0 {
            return Err(ret);
        }
        // SAFETY: f is a valid file with nb_extents extents.
        let out = unsafe { ((*f).size, std::slice::from_raw_parts((*f).extents, usize::try_from((*f).nb_extents).unwrap()).to_vec()) };
        // SAFETY: f came from open_file.
        unsafe { discio::ff_discio_file_free(&raw mut f) };
        Ok(out)
    }

    fn read(&self, path: &str, pos: i64, len: usize) -> Vec<u8> {
        let c = CString::new(path).unwrap();
        let mut f: *mut File = std::ptr::null_mut();
        let mut buf = vec![0u8; len];
        // SAFETY: as above; buf writable.
        unsafe {
            assert_eq!(((*(*self.fs).ops).open_file)(self.fs, c.as_ptr(), &raw mut f), 0);
            assert_eq!(discio::ff_discio_file_read(self.fs, f, pos, buf.as_mut_ptr(), c_int::try_from(len).unwrap()), 0);
            discio::ff_discio_file_free(&raw mut f);
        }
        buf
    }

    fn list(&self, path: &str) -> Result<Vec<(String, bool)>, c_int> {
        unsafe extern "C" fn cb(opaque: *mut c_void, name: *const c_char, is_dir: c_int) -> c_int {
            // SAFETY: opaque is the Vec below; name a C string.
            let v = unsafe { &mut *opaque.cast::<Vec<(String, bool)>>() };
            v.push((unsafe { CStr::from_ptr(name) }.to_string_lossy().into_owned(), is_dir != 0));
            0
        }
        let c = CString::new(path).unwrap();
        let mut out: Vec<(String, bool)> = Vec::new();
        // SAFETY: fs mounted; out outlives the call.
        let ret = unsafe { ((*(*self.fs).ops).list_dir)(self.fs, c.as_ptr(), cb, (&raw mut out).cast()) };
        if ret < 0 { Err(ret) } else { Ok(out) }
    }
}

impl Drop for Vol {
    fn drop(&mut self) {
        // SAFETY: fs came from ff_discio_iso9660_mount.
        unsafe { discio::ff_discio_fs_close(&raw mut self.fs) };
    }
}

// ---- tests on built images ----

#[test]
fn iso9660_names_extents_data_and_label() {
    let img = Image::write("iso", &image());
    let v = img.mount(false).unwrap();
    assert_eq!(v.label(), "TEST_DISC\u{20ac}");
    assert_eq!(v.open("/VIDEO_TS/VIDEO_TS.IFO").unwrap(), (3000, vec![Extent { sector: 24, count: 2 }]));
    // separators: '\' too, empty components skipped
    assert_eq!(v.open("\\VIDEO_TS\\VIDEO_TS.IFO").unwrap().0, 3000);
    assert_eq!(v.open("//VIDEO_TS//VIDEO_TS.IFO").unwrap().0, 3000);
    assert_eq!(v.open("/VIDEO_TS/VIDEO_TS.IFO;1").unwrap().0, 3000);
    assert_eq!(v.open("/VIDEO_TS/README.TXT").unwrap().0, 10, "hidden files are found");
    assert_eq!(v.open("/VIDEO_TS/NOPE.IFO"), Err(ENOENT));
    assert_eq!(v.open("/AUDIO_TS/X"), Err(ENOENT));
    assert_eq!(v.open("/VIDEO_TS"), Err(ENOENT), "a directory is not a file");
    let data = v.read("/VIDEO_TS/VIDEO_TS.IFO", 1000, 1500);
    assert!(data.iter().enumerate().all(|(i, &b)| b == u8::try_from((i + 1000) % 253).unwrap()));
    assert_eq!(
        v.list("/VIDEO_TS").unwrap(),
        [(".".into(), true), ("..".into(), true), ("VIDEO_TS.IFO".into(), false), ("README.TXT".into(), false)]
    );
    let mut created = [0 as c_char; 15];
    // SAFETY: created has 15 bytes.
    assert_eq!(unsafe { discio::ff_discio_iso9660_creation_date(v.fs, created.as_mut_ptr()) }, 0);
    assert_eq!(unsafe { CStr::from_ptr(created.as_ptr()) }.to_str().unwrap(), "20040301120000");
}

#[test]
fn joliet_names_and_label() {
    let img = Image::write("joliet", &image());
    let v = img.mount(true).unwrap();
    assert_eq!(v.label(), "Joliet Disc");
    assert_eq!(v.open("/VIDEO_TS/VIDEO_TS.IFO").unwrap(), (3000, vec![Extent { sector: 24, count: 2 }]));
    assert_eq!(v.list("/").unwrap()[2], ("VIDEO_TS".into(), true));
}

/// Appends `recs` after the last record of the directory at `sector`.
fn append_records(img: &mut [u8], sector: usize, recs: &[Vec<u8>]) {
    let mut off = sector * 2048;
    while img[off] != 0 {
        off += usize::from(img[off]);
    }
    for r in recs {
        img[off..off + r.len()].copy_from_slice(r);
        off += r.len();
    }
}

#[test]
fn a_name_starting_with_nul_is_empty_and_only_a_name_of_no_characters_is_x() {
    let mut img = image();
    append_records(&mut img, 22, &[record(&[0, b'A'], 25, 1, 0), record(&[], 25, 1, 0)]);
    append_records(&mut img, 23, &[record(&[0, 0, 0, b'A'], 25, 1, 0), record(&[], 25, 1, 0)]);
    let img = Image::write("nul-names", &img);
    for joliet in [false, true] {
        let v = img.mount(joliet).unwrap();
        let names: Vec<String> = v.list("/VIDEO_TS").unwrap().into_iter().map(|e| e.0).collect();
        assert_eq!(names[names.len() - 2..], [String::new(), "x".to_owned()], "joliet {joliet}: {names:?}");
    }
}

#[test]
fn corrupt_and_missing_structures() {
    // zero record length at a directory's sector start: corrupt
    let mut bad = image();
    bad[22 * 2048] = 0;
    let img = Image::write("corrupt", &bad);
    let v = img.mount(false).unwrap();
    assert_eq!(v.list("/VIDEO_TS"), Err(INVALIDDATA));
    assert_eq!(v.open("/VIDEO_TS/VIDEO_TS.IFO"), Err(ENOENT));
    drop(v);
    drop(img);

    // no CD001 in the primary descriptor
    let mut bad = image();
    bad[16 * 2048 + 1] = b'X';
    assert!(Image::write("nocd001", &bad).mount(false).is_err());

    // Joliet without a supplementary descriptor
    let mut bad = image();
    bad[17 * 2048] = 0xff;
    let img = Image::write("nosvd", &bad);
    assert!(img.mount(true).is_err());
    assert!(img.mount(false).is_ok());
}

#[test]
fn a_read_failure_after_the_primary_descriptor_keeps_the_volume() {
    // the image ends after sector 16 (PVD) + 4 more sectors: sector 21+ unreadable
    let mut cut = image();
    cut[17 * 2048] = 0; // no SVD, no terminator: the scan reads on
    cut[18 * 2048] = 0;
    cut.truncate(21 * 2048);
    let img = Image::write("cut", &cut);
    let v = img.mount(false).unwrap();
    assert_eq!(v.label(), "TEST_DISC\u{20ac}");
}

// ---- corpus: our ISO 9660 vs libdvdread's UDF, labels vs the reference ----

extern "C" {
    fn DVDOpen(path: *const c_char) -> *mut c_void;
    fn DVDClose(dvd: *mut c_void);
    fn UDFFindFile(dvd: *mut c_void, filename: *const c_char, size: *mut u32) -> u32;
}

/// Labels the reference program gives the corpus images it reads through ISO 9660
/// (archived corpus run of the file-system choice, 16 of 16 matching).
const ISO_LABELS: &[(&str, &str)] = &[
    ("AVBA_14083.iso", "AVBA_14083"),
    ("SAINTBEAST_OVA1_1.ISO", "ORS_1003"),
    ("SPACE_SYMPHONY_MAETEL_1.iso", "SPACE_SYMPHONY_MAETEL_1"),
    ("LABYRINTH_1.iso", "LABYRINTH_1"),
    ("LABYRINTH_2.iso", "LABYRINTH_2"),
    ("THE_THING_1998_DVD.ISO", "THING"),
];

fn images(dir: &Path, out: &mut Vec<PathBuf>) {
    for e in std::fs::read_dir(dir).unwrap().flatten() {
        let p = e.path();
        if p.is_dir() {
            images(&p, out);
        } else if p.extension().is_some_and(|x| x.eq_ignore_ascii_case("iso")) {
            if p.exists() {
                out.push(p);
            } else {
                eprintln!("{}: link to a missing file (drive not mounted?), skipped", p.display());
            }
        }
    }
}

#[test]
fn corpus_iso9660_matches_udf_extents_and_reference_labels() {
    let Some(dir) = std::env::var_os("DISCIO_CORPUS") else {
        eprintln!("DISCIO_CORPUS not set: corpus check skipped");
        return;
    };
    let mut list = Vec::new();
    images(Path::new(&dir), &mut list);
    list.sort();
    assert!(!list.is_empty(), "no .iso under {dir:?}");
    let mut checked_files = 0;
    let mut checked_labels = 0;
    let mut label_names = std::collections::HashSet::new();
    for path in &list {
        let img = Image::open(path);
        let Ok(v) = img.mount(false) else {
            eprintln!("{}: no ISO 9660 file system", path.display());
            continue;
        };
        let name = path.file_name().unwrap().to_str().unwrap();
        if let Some((_, want)) = ISO_LABELS.iter().find(|(n, _)| *n == name) {
            assert_eq!(v.label(), *want, "{name}: label");
            checked_labels += 1;
            label_names.insert(name.to_string());
        }
        let cpath = CString::new(path.to_str().unwrap()).unwrap();
        // SAFETY: valid path.
        let dvd = unsafe { DVDOpen(cpath.as_ptr()) };
        assert!(!dvd.is_null(), "{name}: libdvdread cannot open it");
        let mut files = vec!["/VIDEO_TS/VIDEO_TS.IFO".to_string()];
        for n in 1..100 {
            files.push(format!("/VIDEO_TS/VTS_{n:02}_0.IFO"));
        }
        for f in files {
            let cf = CString::new(f.as_str()).unwrap();
            let mut size = 0u32;
            // SAFETY: dvd open; size writable.
            let start = unsafe { UDFFindFile(dvd, cf.as_ptr(), &raw mut size) };
            let ours = v.open(&f);
            if start == 0 {
                if f.ends_with("_0.IFO") && ours.is_err() {
                    break; // past the last title set
                }
                continue;
            }
            let (osize, ext) = ours.unwrap_or_else(|e| panic!("{name}: {f} missing in ISO 9660 ({e})"));
            assert_eq!((ext[0].sector, osize), (i64::from(start), i64::from(size)), "{name}: {f}");
            checked_files += 1;
        }
        // SAFETY: dvd came from DVDOpen.
        unsafe { DVDClose(dvd) };
    }
    eprintln!("corpus: {} images, {checked_files} IFO files equal to libdvdread's UDF, {checked_labels} labels = the reference", list.len());
    // (the corpus holds AVBA_14083.iso twice: a known duplicate)
    assert_eq!(label_names.len(), ISO_LABELS.len(), "every reference label checked");
}
