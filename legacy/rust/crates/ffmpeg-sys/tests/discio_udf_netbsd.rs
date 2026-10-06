//! `libavformat/discio_udf_netbsd.c`: the UDF reader based on NetBSD, on small
//! images built here (ECMA-167 / OSTA UDF layouts: UDF 1.02 with one type-1
//! partition, and virtual, sparable and metadata partitions), and on the
//! corpus images when `DISCIO_UDF_REFERENCE` names a folder of reference
//! dumps (`index.tsv`: disc id and image path; `<disc id>.txt`: the dump).

#![allow(clippy::many_single_char_names, reason = "one short name per descriptor being built")]

use std::ffi::{CStr, CString};
use std::fmt::Write as _;
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};

use ffmpeg_sys::discio::{self, Extent, Source};

const ENOENT: c_int = -2;
const EINVAL: c_int = -22;
const INVALIDDATA: c_int = -0x4144_4E49; // FFERRTAG('I','N','D','A')

/// `DiscIOFS` with its UDF fields.
#[repr(C)]
struct Fs {
    ops: *const discio::FsOps,
    priv_: *mut c_void,
    src: *mut Source,
    label: [c_char; discio::LABEL_SIZE],
    udf_revision: u16,
    udf_recording_time: [u8; 12],
}

/// `DiscIOFile` with its embedded data.
#[repr(C)]
struct File {
    size: i64,
    nb_extents: c_int,
    extents: *mut Extent,
    data: *mut u8,
}

extern "C" {
    fn ff_discio_udf_netbsd_mount(src: *mut Source, out: *mut *mut Fs) -> c_int;
    fn ff_discio_fs_close(fs: *mut *mut Fs);
    fn ff_discio_file_free(file: *mut *mut File);
    fn ff_discio_file_read(fs: *mut Fs, file: *const File, pos: i64, buf: *mut u8, len: c_int) -> c_int;
    fn av_sha_alloc() -> *mut c_void;
    fn av_sha_init(ctx: *mut c_void, bits: c_int) -> c_int;
    fn av_sha_update(ctx: *mut c_void, data: *const u8, len: usize);
    fn av_sha_final(ctx: *mut c_void, digest: *mut u8);
    fn av_free(ptr: *mut c_void);
}

const S: usize = 2048;
const PART_START: u32 = 300;

// ---------------------------------------------------------------------------
// Running the reader.

struct Image {
    src: *mut Source,
}

impl Image {
    fn open(path: &Path) -> Self {
        let c = CString::new(path.to_str().unwrap()).unwrap();
        let mut src = std::ptr::null_mut();
        // SAFETY: valid path and out pointer.
        let ret = unsafe { discio::ff_discio_source_open_file(std::ptr::null_mut(), c.as_ptr(), &raw mut src) };
        assert_eq!(ret, 0, "opening {}: {}", path.display(), ffmpeg_sys::error_text(ret));
        Image { src }
    }

    fn write(bytes: &[u8]) -> Self {
        use std::sync::atomic::{AtomicUsize, Ordering};
        static N: AtomicUsize = AtomicUsize::new(0);
        let dir = std::env::temp_dir().join(format!("discio-udf-netbsd-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join(format!("image{}.iso", N.fetch_add(1, Ordering::Relaxed)));
        std::fs::write(&path, bytes).unwrap();
        let im = Self::open(&path);
        std::fs::remove_file(&path).unwrap();
        im
    }

    fn mount(&self) -> Result<Vol, c_int> {
        let mut fs = std::ptr::null_mut();
        // SAFETY: src is open.
        let ret = unsafe { ff_discio_udf_netbsd_mount(self.src, &raw mut fs) };
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

/// An open file.
struct OpenFile<'v> {
    vol: &'v Vol,
    f: *mut File,
}

impl OpenFile<'_> {
    fn size(&self) -> i64 {
        // SAFETY: f is a valid file.
        unsafe { (*self.f).size }
    }

    fn extents(&self) -> Vec<Extent> {
        // SAFETY: f holds nb_extents extents.
        unsafe {
            let n = usize::try_from((*self.f).nb_extents).unwrap();
            if n == 0 { Vec::new() } else { std::slice::from_raw_parts((*self.f).extents, n).to_vec() }
        }
    }

    fn read(&self, pos: i64, len: usize) -> Result<Vec<u8>, c_int> {
        let mut buf = vec![0u8; len];
        // SAFETY: fs mounted, f open, buf writable.
        let ret = unsafe { ff_discio_file_read(self.vol.fs, self.f, pos, buf.as_mut_ptr(), c_int::try_from(len).unwrap()) };
        if ret < 0 { Err(ret) } else { Ok(buf) }
    }
}

impl Drop for OpenFile<'_> {
    fn drop(&mut self) {
        // SAFETY: f came from open_file.
        unsafe { ff_discio_file_free(&raw mut self.f) };
    }
}

impl Vol {
    fn label(&self) -> Vec<u8> {
        // SAFETY: label is NUL-terminated within its room.
        unsafe { CStr::from_ptr((*self.fs).label.as_ptr()) }.to_bytes().to_vec()
    }

    fn revision(&self) -> u16 {
        // SAFETY: fs is mounted.
        unsafe { (*self.fs).udf_revision }
    }

    fn year(&self) -> u16 {
        // SAFETY: fs is mounted.
        let t = unsafe { (*self.fs).udf_recording_time };
        u16::from_le_bytes([t[2], t[3]])
    }

    fn open_bytes(&self, path: &[u8]) -> Result<OpenFile<'_>, c_int> {
        let c = CString::new(path).unwrap();
        let mut f: *mut File = std::ptr::null_mut();
        // SAFETY: fs is mounted; ops is its table (File extends discio::File).
        let ret = unsafe { ((*(*self.fs).ops).open_file)(self.fs.cast(), c.as_ptr(), (&raw mut f).cast()) };
        if ret < 0 { Err(ret) } else { Ok(OpenFile { vol: self, f }) }
    }

    fn open(&self, path: &str) -> Result<OpenFile<'_>, c_int> {
        self.open_bytes(path.as_bytes())
    }

    /// (name bytes, `is_dir`) in directory order.
    fn list_bytes(&self, path: &[u8]) -> Result<Vec<(Vec<u8>, bool)>, c_int> {
        unsafe extern "C" fn cb(opaque: *mut c_void, name: *const c_char, is_dir: c_int) -> c_int {
            // SAFETY: opaque is the Vec below; name a C string.
            let v = unsafe { &mut *opaque.cast::<Vec<(Vec<u8>, bool)>>() };
            v.push((unsafe { CStr::from_ptr(name) }.to_bytes().to_vec(), is_dir != 0));
            0
        }
        let c = CString::new(path).unwrap();
        let mut out: Vec<(Vec<u8>, bool)> = Vec::new();
        // SAFETY: fs mounted; out outlives the call.
        let ret = unsafe { ((*(*self.fs).ops).list_dir)(self.fs.cast(), c.as_ptr(), cb, (&raw mut out).cast()) };
        if ret < 0 { Err(ret) } else { Ok(out) }
    }

    fn list(&self, path: &str) -> Result<Vec<(String, bool)>, c_int> {
        Ok(self.list_bytes(path.as_bytes())?.into_iter().map(|(n, d)| (String::from_utf8_lossy(&n).into_owned(), d)).collect())
    }

    fn names(&self, path: &str) -> Vec<String> {
        self.list(path).unwrap().into_iter().map(|(n, _)| n).collect()
    }
}

impl Drop for Vol {
    fn drop(&mut self) {
        // SAFETY: fs came from the mount.
        unsafe { ff_discio_fs_close(&raw mut self.fs) };
    }
}

fn mount(data: &[u8]) -> Result<(Image, Vol), c_int> {
    let im = Image::write(data);
    let v = im.mount()?;
    Ok((im, v))
}

// ---------------------------------------------------------------------------
// Descriptor builders.

fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for &b in data {
        crc ^= u16::from(b) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}

fn put16(b: &mut [u8], at: usize, v: u16) {
    b[at..at + 2].copy_from_slice(&v.to_le_bytes());
}
fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

fn checksum(b: &mut [u8]) {
    b[4] = 0;
    b[4] = b[..16].iter().enumerate().filter(|&(i, _)| i != 4).fold(0u8, |s, (_, &x)| s.wrapping_add(x));
}

/// Fills in a descriptor tag: id, version 2, CRC over `crclen` bytes, checksum.
fn tag(b: &mut [u8], id: u16, crclen: u16) {
    put16(b, 0, id);
    put16(b, 2, 2);
    put16(b, 10, crclen);
    let crc = crc16(&b[16..16 + usize::from(crclen)]);
    put16(b, 8, crc);
    checksum(b);
}

fn dstring(b: &mut [u8], at: usize, size: usize, text: &str) {
    b[at] = 8;
    b[at + 1..at + 1 + text.len()].copy_from_slice(text.as_bytes());
    b[at + size - 1] = u8::try_from(1 + text.len()).unwrap();
}

fn osta(b: &mut [u8], at: usize) {
    b[at] = 0;
    b[at + 1..at + 24].copy_from_slice(b"OSTA Compressed Unicode");
}

/// Allocation descriptors of a file entry.
enum Ads<'a> {
    /// (length with extent type bits, logical block)
    Short(&'a [(u32, u32)]),
    /// (length with extent type bits, logical block, partition reference)
    Long(&'a [(u32, u32, u16)]),
    /// Data recorded in the file entry.
    Inline(&'a [u8]),
}

/// A file entry: `file_type` (4 directory, 5 file, 248 VAT), `strategy`.
fn file_entry(file_type: u8, size: u64, strategy: u16, ads: &Ads) -> Vec<u8> {
    let mut b = vec![0u8; S];
    put16(&mut b, 0x14, strategy);
    b[0x1b] = file_type;
    put64(&mut b, 0x38, size);
    put64(&mut b, 0xa0, 0x1234_5678); // unique id
    let mut at = 0xb0;
    let flags = match ads {
        Ads::Short(list) => {
            for &(len, lbn) in *list {
                put32(&mut b, at, len);
                put32(&mut b, at + 4, lbn);
                at += 8;
            }
            0
        }
        Ads::Long(list) => {
            for &(len, lbn, part) in *list {
                put32(&mut b, at, len);
                put32(&mut b, at + 4, lbn);
                put16(&mut b, at + 8, part);
                at += 16;
            }
            1
        }
        Ads::Inline(data) => {
            b[at..at + data.len()].copy_from_slice(data);
            at += data.len();
            3
        }
    };
    put16(&mut b, 0x22, flags);
    put32(&mut b, 0xa8, 0);
    put32(&mut b, 0xac, u32::try_from(at - 0xb0).unwrap());
    tag(&mut b, 0x105, 0);
    b
}

/// An indirect entry pointing at (`lbn`, `part`).
fn indirect_entry(lbn: u32, part: u16) -> Vec<u8> {
    let mut b = vec![0u8; S];
    put16(&mut b, 0x14, 4);
    put32(&mut b, 0x24, 2048);
    put32(&mut b, 0x28, lbn);
    put16(&mut b, 0x2c, part);
    tag(&mut b, 0x103, 0);
    b
}

/// One file identifier: (name, characteristics, ICB block, ICB partition).
type Fid = (&'static str, u8, u32, u16);

/// One file identifier descriptor with a recorded name of raw bytes
/// (compression ID first; empty = no name).
fn fid_raw(name: &[u8], chars: u8, lbn: u32, part: u16) -> Vec<u8> {
    let len = (38 + name.len() + 3) & !3;
    let mut b = vec![0u8; len];
    put16(&mut b, 0x10, 1);
    b[0x12] = chars;
    b[0x13] = u8::try_from(name.len()).unwrap();
    put32(&mut b, 0x14, 2048);
    put32(&mut b, 0x18, lbn);
    put16(&mut b, 0x1c, part);
    b[0x26..0x26 + name.len()].copy_from_slice(name);
    tag(&mut b, 0x101, 0);
    b
}

fn name8(s: &str) -> Vec<u8> {
    if s.is_empty() {
        return Vec::new();
    }
    let mut v = vec![8u8];
    v.extend_from_slice(s.as_bytes());
    v
}

/// Directory data: a parent entry, then `fids`.
fn directory(parent: (u32, u16), fids: &[Fid]) -> Vec<u8> {
    let mut out = fid_raw(&[], 0x0a, parent.0, parent.1);
    for &(name, chars, lbn, part) in fids {
        out.extend(fid_raw(&name8(name), chars, lbn, part));
    }
    out
}

/// Partition maps.
fn map_type1(part_num: u16) -> Vec<u8> {
    let mut m = vec![1u8, 6, 1, 0, 0, 0];
    put16(&mut m, 4, part_num);
    m
}

fn map_type2(id: &str, part_num: u16) -> Vec<u8> {
    let mut m = vec![0u8; 64];
    m[0] = 2;
    m[1] = 64;
    m[5..5 + id.len()].copy_from_slice(id.as_bytes());
    put16(&mut m, 36, 1);
    put16(&mut m, 38, part_num);
    m
}

struct Volume {
    label: &'static str,
    maps: Vec<Vec<u8>>,
    /// File set descriptor location (block, partition reference).
    fsd: (u32, u16),
    /// Partition descriptors: (number, start, length).
    partitions: Vec<(u16, u32, u32)>,
    integrity_closed: bool,
}

/// Writes anchor (at 256), volume descriptor sequence (main at 32, reserve
/// at 48) and integrity descriptor (at 64) for `v` into `d`.
fn volume(d: &mut [u8], v: &Volume) {
    let sec = |n: usize| -> std::ops::Range<usize> { n * S..(n + 1) * S };
    let a = &mut d[sec(256)];
    put32(a, 0x10, 16 * 2048);
    put32(a, 0x14, 32);
    put32(a, 0x18, 16 * 2048);
    put32(a, 0x1c, 48);
    tag(a, 2, 0);
    let mut n = 32;
    let p = &mut d[sec(n)];
    put16(p, 0x178 + 2, 2009);
    tag(p, 1, 0);
    n += 1;
    let iu = &mut d[sec(n)];
    iu[0x15..0x21].copy_from_slice(b"*UDF LV Info");
    put16(iu, 0x2c, 0x0102);
    tag(iu, 4, 0);
    n += 1;
    for &(num, start, len) in &v.partitions {
        let pd = &mut d[sec(n)];
        pd[0x14] = 1;
        put16(pd, 0x16, num);
        put32(pd, 0xbc, start);
        put32(pd, 0xc0, len);
        tag(pd, 5, 0);
        n += 1;
    }
    let l = &mut d[sec(n)];
    osta(l, 0x14);
    dstring(l, 0x54, 128, v.label);
    put32(l, 0xd4, 2048);
    l[0xd9..0xd9 + 19].copy_from_slice(b"*OSTA UDF Compliant");
    put32(l, 0xf8, 2048);
    put32(l, 0xfc, v.fsd.0);
    put16(l, 0x100, v.fsd.1);
    let mut at = 0x1b8;
    for m in &v.maps {
        l[at..at + m.len()].copy_from_slice(m);
        at += m.len();
    }
    put32(l, 0x108, u32::try_from(at - 0x1b8).unwrap());
    put32(l, 0x10c, u32::try_from(v.maps.len()).unwrap());
    put32(l, 0x1b0, 2048);
    put32(l, 0x1b4, 64);
    tag(l, 6, 0);
    n += 1;
    tag(&mut d[sec(n)], 8, 0);
    let lvid = &mut d[sec(64)];
    put32(lvid, 0x1c, u32::from(v.integrity_closed));
    put32(lvid, 0x48, 1);
    tag(lvid, 9, 0);
}

/// A file set descriptor with the root directory at (`lbn`, `part`).
fn fsd(root: (u32, u16)) -> Vec<u8> {
    let mut f = vec![0u8; S];
    put32(&mut f, 0x190, 2048);
    put32(&mut f, 0x194, root.0);
    put16(&mut f, 0x198, root.1);
    tag(&mut f, 0x100, 0);
    f
}

// ---------------------------------------------------------------------------
// The standard UDF 1.02 image.

struct Built {
    data: Vec<u8>,
}

impl Built {
    fn sector(&mut self, n: u32) -> &mut [u8] {
        let at = n as usize * S;
        &mut self.data[at..at + S]
    }
    fn lb(&mut self, lbn: u32) -> &mut [u8] {
        self.sector(PART_START + lbn)
    }
    fn put_lb(&mut self, lbn: u32, bytes: &[u8]) {
        let at = (PART_START + lbn) as usize * S;
        self.data[at..at + bytes.len()].copy_from_slice(bytes);
    }
    fn file(&mut self, lbn: u32, file_type: u8, size: u64, data_lbn: u32) {
        let fe = file_entry(file_type, size, 4, &Ads::Short(&[(u32::try_from(size).unwrap(), data_lbn)]));
        self.put_lb(lbn, &fe);
    }
}

/// A UDF 1.02 image: root holds `VIDEO_TS` (directory) and `README` (file);
/// `VIDEO_TS` holds the entries `vts` (directory data at block 5; file entries
/// at blocks 20.., 100 + k bytes of data at blocks 30..).
fn build_raw(label: &'static str, vts: &[u8]) -> Built {
    let mut im = Built { data: vec![0u8; 520 * S] };
    volume(
        &mut im.data,
        &Volume { label, maps: vec![map_type1(0)], fsd: (0, 0), partitions: vec![(0, PART_START, 64)], integrity_closed: true },
    );
    im.put_lb(0, &fsd((1, 0)));
    let root = directory((1, 0), &[("VIDEO_TS", 0x02, 3, 0), ("README", 0, 4, 0)]);
    im.put_lb(2, &root);
    im.file(1, 4, root.len() as u64, 2);
    im.put_lb(5, vts);
    im.file(3, 4, vts.len() as u64, 5);
    for (i, x) in im.data[(PART_START as usize + 10) * S..(PART_START as usize + 10) * S + 5000].iter_mut().enumerate() {
        *x = u8::try_from(i % 256).unwrap();
    }
    im.file(4, 5, 5000, 10);
    for k in 0..4u32 {
        im.file(20 + k, 5, 100 + u64::from(k), 30 + k);
    }
    im
}

fn build(label: &'static str, video_ts_fids: &[Fid]) -> Built {
    build_raw(label, &directory((1, 0), video_ts_fids))
}

fn standard() -> Built {
    build("TEST_DISC", &[("VIDEO_TS.IFO", 0, 20, 0), ("VTS_01_0.IFO", 0, 21, 0)])
}

// ---------------------------------------------------------------------------
// Tests on built images.

#[test]
fn mounts_and_reports_label_revision_and_year() {
    let (_im, v) = mount(&standard().data).unwrap();
    assert_eq!(v.label(), b"TEST_DISC");
    assert_eq!(v.revision(), 0x0102);
    assert_eq!(v.year(), 2009);
}

#[test]
fn lists_in_directory_order_with_the_parent_entry() {
    let (_im, v) = mount(&standard().data).unwrap();
    assert_eq!(v.list("/").unwrap(), vec![("..".into(), true), ("VIDEO_TS".into(), true), ("README".into(), false)]);
    assert_eq!(v.open("/README").unwrap().size(), 5000);
}

#[test]
fn opens_files_by_path_and_reads_them() {
    let (_im, v) = mount(&standard().data).unwrap();
    let f = v.open("/README").unwrap();
    assert_eq!(f.size(), 5000);
    assert_eq!(f.extents(), vec![Extent { sector: i64::from(PART_START + 10), count: 3 }]);
    let buf = f.read(0, 5000).unwrap();
    assert!(buf.iter().enumerate().all(|(i, &x)| x == u8::try_from(i % 256).unwrap()));
    assert_eq!(f.read(4990, 10).unwrap().len(), 10);
    assert_eq!(f.read(4995, 10).err(), Some(EINVAL), "reads stop at the end of the file");
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO").unwrap().size(), 101);
}

#[test]
fn the_allocation_type_is_the_low_two_bits_of_the_icb_flags() {
    let mut im = standard();
    // README's file entry (block 4): short descriptors with reserved flag bit 2 set.
    let lb = im.lb(4);
    lb[0x22] |= 0x04;
    let (_im, v) = mount(&im.data).unwrap();
    let f = v.open("/README").unwrap();
    assert_eq!(f.size(), 5000);
    assert_eq!(f.extents()[0].sector, i64::from(PART_START + 10));
}

#[test]
fn extended_descriptors_with_an_empty_area_read_as_no_data() {
    let mut im = standard();
    let lb = im.lb(4);
    lb[0x22] = (lb[0x22] & !3) | 2;
    lb[0xac..0xb0].fill(0);
    lb[0xb0..0xb8].fill(0);
    let (_im, v) = mount(&im.data).unwrap();
    // The node loads; its 5000 bytes have no extent to come from.
    assert_eq!(v.open("/README").err(), Some(INVALIDDATA));
}

#[test]
fn an_unreadable_root_entry_fails_the_mount() {
    let mut im = standard();
    im.lb(1)[0] ^= 0xff;
    assert!(mount(&im.data).is_err());
}

#[test]
fn path_separators_and_empty_components() {
    let (_im, v) = mount(&standard().data).unwrap();
    assert!(v.open("\\VIDEO_TS\\VIDEO_TS.IFO").is_ok());
    assert!(v.open("//VIDEO_TS//VIDEO_TS.IFO").is_ok());
    assert_eq!(v.open("VIDEO_TS/VIDEO_TS.IFO").err(), Some(ENOENT), "a path must start with a separator");
    assert_eq!(v.open("/video_ts/VIDEO_TS.IFO").err(), Some(ENOENT), "names are case-sensitive");
    assert_eq!(v.open("/VIDEO_TS").err(), Some(ENOENT), "a directory is not a file");
    assert!(v.list("/").is_ok());
    assert_eq!(v.list("/README").err(), Some(ENOENT), "a file is not a directory");
}

#[test]
fn duplicate_names_resolve_to_the_last_entry() {
    let im = build("DUP", &[("A.IFO", 0, 20, 0), ("A.IFO", 0, 22, 0)]);
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.open("/VIDEO_TS/A.IFO").unwrap().size(), 102);
    let names = v.names("/VIDEO_TS");
    assert_eq!(names, vec!["..", "A.IFO", "A.IFO"], "both entries are listed");
}

#[test]
fn deleted_entries_are_skipped() {
    let im = build("DEL", &[("GONE.IFO", 0x04, 20, 0), ("KEPT.IFO", 0, 21, 0)]);
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.open("/VIDEO_TS/GONE.IFO").err(), Some(ENOENT));
    assert_eq!(v.names("/VIDEO_TS"), vec!["..", "KEPT.IFO"]);
}

#[test]
fn hidden_entries_are_listed_and_found() {
    let fids = [("SEEN.IFO", 0, 20, 0), ("HIDDEN.IFO", 0x01, 21, 0)];
    let (_im, v) = mount(&build("HID", &fids).data).unwrap();
    assert_eq!(v.names("/VIDEO_TS"), vec!["..", "SEEN.IFO", "HIDDEN.IFO"]);
    assert_eq!(v.open("/VIDEO_TS/HIDDEN.IFO").unwrap().size(), 101);
}

#[test]
fn illegal_characters_follow_the_windows_name_rules() {
    let (_im, v) = mount(&build("NAMES", &[("A:B?C.", 0, 20, 0)]).data).unwrap();
    assert_eq!(v.names("/VIDEO_TS"), vec!["..", "A_B_C#D24F"]);
    assert!(v.open("/VIDEO_TS/A_B_C#D24F").is_ok());
    assert_eq!(v.open("/VIDEO_TS/A:B?C.").err(), Some(ENOENT), "a lookup does not apply the Windows rules");
}

/// The name a single recorded name in `VIDEO_TS` is listed as.
fn listed_as(raw: &[u8]) -> String {
    let mut vts = fid_raw(&[], 0x0a, 1, 0);
    vts.extend(fid_raw(raw, 0, 20, 0));
    let (_im, v) = mount(&build_raw("NAMES", &vts).data).unwrap();
    let names = v.names("/VIDEO_TS");
    assert_eq!(names.len(), 2);
    names[1].clone()
}

#[test]
fn osta_name_translation() {
    assert_eq!(listed_as(&name8("VIDEO_TS.IFO")), "VIDEO_TS.IFO", "plain names are unchanged");
    let trailing = listed_as(&name8("ABC  "));
    assert!(trailing.starts_with("ABC#") && trailing.len() == 8, "trailing spaces are removed: {trailing}");
    let del = listed_as(&name8("A\u{7f}B"));
    assert!(del.starts_with("A_B#"), "DEL counts as not printable: {del}");
    assert_eq!(listed_as(&[3, b'A']), "", "a bad compression ID gives an empty name");
    assert_eq!(listed_as(&[16, 0x30, 0x42, 0x00, 0x41]), "\u{3042}A", "16-bit names");
    assert_eq!(listed_as(&[254, b'X', b'Y']), "XY", "compression ID 254 counts as 8");
}

#[test]
fn sixteen_bit_names_are_found_by_their_utf8() {
    let mut vts = fid_raw(&[], 0x0a, 1, 0);
    vts.extend(fid_raw(&[16, 0x30, 0x42, 0x00, 0x41], 0, 21, 0));
    let (_im, v) = mount(&build_raw("NAMES", &vts).data).unwrap();
    assert_eq!(v.open("/VIDEO_TS/\u{3042}A").unwrap().size(), 101);
}

#[test]
fn the_parent_entry_is_found_by_name() {
    let (_im, v) = mount(&standard().data).unwrap();
    assert_eq!(v.names("/VIDEO_TS/.."), v.names("/"));
}

#[test]
fn a_broken_entry_fails_the_listing_and_every_lookup() {
    let mut im = build("BROKEN", &[("X.IFO", 0, 20, 0), ("Y.IFO", 0, 21, 0)]);
    let at = (PART_START as usize + 5) * S + 40 + 4;
    im.data[at] ^= 0xff;
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.list("/VIDEO_TS").err(), Some(INVALIDDATA));
    assert_eq!(v.open("/VIDEO_TS/X.IFO").err(), Some(INVALIDDATA));
}

#[test]
fn an_entry_with_a_bad_crc_but_a_consistent_length_is_used() {
    let fids = [("A.IFO", 0, 20, 0), ("B.IFO", 0, 21, 0)];
    let mut im = build("CRC", &fids);
    // The second entry after the parent (40 bytes) is 44 bytes long: CRC
    // length 28 (= 44 - 16) with a wrong CRC.
    let at = (PART_START as usize + 5) * S + 40;
    let b = &mut im.data[at..at + 44];
    put16(b, 10, 28);
    put16(b, 8, 0xbeef);
    checksum(b);
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.names("/VIDEO_TS"), vec!["..", "A.IFO", "B.IFO"]);
    // CRC length 27: inconsistent, the entry is broken.
    let b = &mut im.data[at..at + 44];
    put16(b, 10, 27);
    checksum(b);
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.list("/VIDEO_TS").err(), Some(INVALIDDATA));
}

#[test]
fn a_blank_integrity_sector_fails_the_mount() {
    let mut im = standard();
    im.sector(64).fill(0);
    assert_eq!(mount(&im.data).err(), Some(INVALIDDATA));
}

#[test]
fn an_unclean_volume_is_read() {
    let mut im = standard();
    put32(im.sector(64), 0x1c, 0);
    tag(im.sector(64), 9, 0);
    assert_eq!(mount(&im.data).unwrap().1.label(), b"TEST_DISC");
}

#[test]
fn anchors_at_512_or_at_the_end_are_enough() {
    let mut im = standard();
    let a: Vec<u8> = im.sector(256).to_vec();
    im.sector(512).copy_from_slice(&a);
    im.sector(256).fill(0);
    assert_eq!(mount(&im.data).unwrap().1.label(), b"TEST_DISC");
    for end in [519u32, 519 - 256] {
        let mut im = standard();
        let a: Vec<u8> = im.sector(256).to_vec();
        im.sector(end).copy_from_slice(&a);
        im.sector(256).fill(0);
        assert_eq!(mount(&im.data).unwrap().1.label(), b"TEST_DISC", "anchor at sector {end}");
    }
}

#[test]
fn no_anchor_is_not_udf() {
    let mut im = standard();
    im.sector(256).fill(0);
    assert_eq!(mount(&im.data).err(), Some(INVALIDDATA));
}

#[test]
fn the_reserve_sequence_is_used_when_the_main_one_fails() {
    let mut im = standard();
    for s in 32..37 {
        let d: Vec<u8> = im.sector(s).to_vec();
        im.sector(s + 16).copy_from_slice(&d);
    }
    im.sector(32)[0] ^= 0xff;
    assert_eq!(mount(&im.data).unwrap().1.label(), b"TEST_DISC");
}

/// An unallocated space descriptor whose size (24 + 8 * count) wraps to 0.
fn zero_size_descriptor() -> Vec<u8> {
    let mut u = vec![0u8; S];
    put32(&mut u, 0x14, 536_870_909);
    tag(&mut u, 7, 0);
    u
}

#[test]
fn a_descriptor_of_size_0_ends_the_sequence_with_an_error() {
    let mut im = standard();
    im.sector(33).copy_from_slice(&zero_size_descriptor());
    assert!(mount(&im.data).is_err(), "no reserve sequence: the mount fails (and does not hang)");
    let mut im = standard();
    for s in 32..37 {
        let d: Vec<u8> = im.sector(s).to_vec();
        im.sector(s + 16).copy_from_slice(&d);
    }
    im.sector(33).copy_from_slice(&zero_size_descriptor());
    assert_eq!(mount(&im.data).unwrap().1.label(), b"TEST_DISC", "the reserve sequence is used");
}

#[test]
fn the_last_logical_volume_descriptor_wins() {
    let mut im = standard();
    // Sequence: PVD 32, IUVD 33, PD 34, LVD 35, TD 36. Put a second LVD with
    // another label at 36 and the terminator at 37.
    let mut l2: Vec<u8> = im.sector(35).to_vec();
    l2[0x54..0xd4].fill(0);
    dstring(&mut l2, 0x54, 128, "SECOND");
    tag(&mut l2, 6, 0);
    let td: Vec<u8> = im.sector(36).to_vec();
    im.sector(36).copy_from_slice(&l2);
    im.sector(37).copy_from_slice(&td);
    assert_eq!(mount(&im.data).unwrap().1.label(), b"SECOND");
}

#[test]
fn a_label_with_illegal_characters_follows_the_windows_name_rules() {
    let mut im = standard();
    let l = im.sector(35);
    l[0x54..0xd4].fill(0);
    dstring(l, 0x54, 128, "A:B?C.");
    tag(l, 6, 0);
    assert_eq!(mount(&im.data).unwrap().1.label(), b"A_B_C#D24F");
}

#[test]
fn a_file_with_its_data_in_the_file_entry_cannot_be_opened() {
    let mut im = standard();
    let fe = file_entry(5, 11, 4, &Ads::Inline(b"hello world"));
    im.put_lb(21, &fe);
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO").err(), Some(INVALIDDATA));
    assert_eq!(v.names("/VIDEO_TS"), vec!["..", "VIDEO_TS.IFO", "VTS_01_0.IFO"], "it is still listed");
}

#[test]
fn an_unknown_strategy_is_an_error_not_a_hang() {
    let mut im = standard();
    put16(im.lb(21), 0x14, 7);
    tag(im.lb(21), 0x105, 0);
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO").err(), Some(INVALIDDATA));
}

#[test]
fn indirect_entries_are_followed_and_loops_stopped() {
    // VTS_01_0.IFO's ICB (block 21) is an indirect entry to block 40, which
    // holds the file entry.
    let mut im = standard();
    let fe: Vec<u8> = im.lb(21).to_vec();
    im.put_lb(40, &fe);
    im.put_lb(21, &indirect_entry(40, 0));
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO").unwrap().size(), 101);
    // Block 21 -> 40 -> 21.
    let mut im = standard();
    im.put_lb(21, &indirect_entry(40, 0));
    im.put_lb(40, &indirect_entry(21, 0));
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO").err(), Some(INVALIDDATA));
}

#[test]
fn a_strategy_4096_chain_does_not_open() {
    // NetBSD's direct strategy never sees the end of a 4096 chain: the
    // blank block after it reads as a tag-0 descriptor.
    let mut im = standard();
    put16(im.lb(21), 0x14, 4096);
    tag(im.lb(21), 0x105, 0);
    let (_im, v) = mount(&im.data).unwrap();
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO").err(), Some(INVALIDDATA));
}

/// A file at ICB block 21 whose data is spread over `n` allocation extent
/// descriptors (blocks 100..100+n), one data block each (blocks after them).
fn aed_image(n: u32) -> (Built, u32) {
    let data_lbn = 100 + n + 4;
    let part_len = data_lbn + n + 8;
    let sectors = PART_START + part_len + 260;
    let mut im = Built { data: vec![0u8; sectors as usize * S] };
    volume(
        &mut im.data,
        &Volume { label: "AED", maps: vec![map_type1(0)], fsd: (0, 0), partitions: vec![(0, PART_START, part_len)], integrity_closed: true },
    );
    im.put_lb(0, &fsd((1, 0)));
    let root = directory((1, 0), &[("F", 0, 21, 0)]);
    im.put_lb(2, &root);
    im.file(1, 4, root.len() as u64, 2);
    // File entry: one data block, then a redirect to AED 0.
    let fe = file_entry(5, u64::from(n + 1) * 2048, 4, &Ads::Short(&[(2048, data_lbn), ((3 << 30) | 2048, 100)]));
    im.put_lb(21, &fe);
    for k in 0..n {
        let mut b = vec![0u8; S];
        let mut ads = vec![(2048u32, data_lbn + 1 + k)];
        if k + 1 < n {
            ads.push(((3 << 30) | 2048, 101 + k));
        }
        for (i, (len, lbn)) in ads.iter().enumerate() {
            put32(&mut b, 24 + 8 * i, *len);
            put32(&mut b, 28 + 8 * i, *lbn);
        }
        put32(&mut b, 20, u32::try_from(8 * ads.len()).unwrap());
        tag(&mut b, 0x102, 0);
        im.put_lb(100 + k, &b);
    }
    for k in 0..=n {
        im.lb(data_lbn + k)[0] = u8::try_from(k % 256).unwrap();
    }
    // the image size moves the end anchor: keep the one at 256 only
    (im, data_lbn)
}

#[test]
fn allocation_extent_descriptors_and_their_limit() {
    let (im, data_lbn) = aed_image(3);
    let (_im, v) = mount(&im.data).unwrap();
    let f = v.open("/F").unwrap();
    // One extent per allocation descriptor, even though the four blocks
    // are adjacent on the disc.
    let one = |k: u32| Extent { sector: i64::from(PART_START + data_lbn + k), count: 1 };
    assert_eq!(f.extents(), vec![one(0), one(1), one(2), one(3)]);
    assert_eq!(f.read(3 * 2048, 1).unwrap(), vec![3]);
    // 60 and 250 descriptors: within the limit of 250 (NetBSD: 50); 251 not.
    for (n, ok) in [(60, true), (250, true), (251, false)] {
        let (im, _) = aed_image(n);
        let (_im, v) = mount(&im.data).unwrap();
        let r = v.open("/F");
        assert_eq!(r.is_ok(), ok, "{n} allocation extent descriptors");
        if let Ok(f) = r {
            assert_eq!(f.extents().len(), n as usize + 1);
        }
    }
}

/// A UDF image with a virtual partition (map 1 over physical map 0) and a
/// UDF 2.00 VAT file at sector `vat_at`. Virtual block k = VAT entry k.
fn vat_image(vat_at: u32, with_vat: bool) -> Vec<u8> {
    let mut d = vec![0u8; 520 * S];
    volume(
        &mut d,
        &Volume {
            label: "LVD_LABEL",
            maps: vec![map_type1(0), map_type2("*UDF Virtual Partition", 0)],
            fsd: (0, 1),
            partitions: vec![(0, PART_START, 200)],
            integrity_closed: false,
        },
    );
    let mut lb = |lbn: u32, bytes: &[u8]| {
        let at = (PART_START + lbn) as usize * S;
        d[at..at + bytes.len()].copy_from_slice(bytes);
    };
    // Physical layout: FSD 10, root FE 11, root data 12, file FE 13, data 14.
    lb(10, &fsd((1, 1)));
    let root = directory((1, 1), &[("F.TXT", 0, 3, 1)]);
    lb(12, &root);
    // Directory data through the virtual partition: short ADs take the
    // node's partition (1), so virtual block 2 -> physical 12.
    lb(11, &file_entry(4, root.len() as u64, 4, &Ads::Short(&[(u32::try_from(root.len()).unwrap(), 2)])));
    lb(13, &file_entry(5, 5, 4, &Ads::Long(&[(5, 14, 0)])));
    lb(14, b"virt!");
    if with_vat {
        // VAT data at absolute sector vat_at + 1: header (152 bytes, logical
        // volume identifier "VAT_LABEL"), entries 0..3 -> physical 10..13.
        let mut vat = vec![0u8; 152 + 16];
        put16(&mut vat, 0, 152);
        dstring(&mut vat, 4, 128, "VAT_LABEL");
        for (k, p) in [10u32, 11, 12, 13].iter().enumerate() {
            put32(&mut vat, 152 + 4 * k, *p);
        }
        let at = (vat_at as usize + 1) * S;
        d[at..at + vat.len()].copy_from_slice(&vat);
        // VAT file entry (file type 248) at vat_at; it is read through the
        // raw map, so its short AD is an absolute sector.
        let fe = file_entry(248, vat.len() as u64, 4, &Ads::Short(&[(u32::try_from(vat.len()).unwrap(), vat_at + 1)]));
        let at = vat_at as usize * S;
        d[at..at + S].copy_from_slice(&fe);
    }
    d
}

#[test]
fn a_virtual_partition_reads_through_its_vat() {
    let (_im, v) = mount(&vat_image(500, true)).unwrap();
    assert_eq!(v.label(), b"VAT_LABEL", "the VAT's identifier replaces the descriptor's");
    let f = v.open("/F.TXT").unwrap();
    assert_eq!(f.extents(), vec![Extent { sector: i64::from(PART_START + 14), count: 1 }]);
    assert_eq!(f.read(0, 5).unwrap(), b"virt!");
}

#[test]
fn a_missing_vat_fails_the_mount_without_hanging() {
    assert_eq!(mount(&vat_image(500, false)).err(), Some(INVALIDDATA));
}

#[test]
fn a_sparable_partition_reads_remapped_packets() {
    let mut d = vec![0u8; 520 * S];
    let mut map = map_type2("*UDF Sparable Partition", 0);
    put16(&mut map, 40, 32); // packet length
    map[42] = 1; // one sparing table
    put32(&mut map, 44, 2048);
    put32(&mut map, 48, 80); // at sector 80
    volume(&mut d, &Volume { label: "SPARE", maps: vec![map], fsd: (0, 0), partitions: vec![(0, PART_START, 128)], integrity_closed: true });
    // Sparing table: packet 1 (blocks 32..63) is moved to sector 400.
    let mut t = vec![0u8; S];
    put16(&mut t, 48, 1);
    put32(&mut t, 56, 1);
    put32(&mut t, 60, 400);
    tag(&mut t, 0, 0);
    d[80 * S..81 * S].copy_from_slice(&t);
    let mut lb = |abs: u32, bytes: &[u8]| {
        let at = abs as usize * S;
        d[at..at + bytes.len()].copy_from_slice(bytes);
    };
    lb(PART_START, &fsd((1, 0)));
    let root = directory((1, 0), &[("S", 0, 3, 0)]);
    lb(PART_START + 2, &root);
    lb(PART_START + 1, &file_entry(4, root.len() as u64, 4, &Ads::Short(&[(u32::try_from(root.len()).unwrap(), 2)])));
    lb(PART_START + 3, &file_entry(5, 4, 4, &Ads::Short(&[(4, 33)])));
    lb(PART_START + 33, b"orig");
    lb(401, b"good");
    let (_im, v) = mount(&d).unwrap();
    let f = v.open("/S").unwrap();
    assert_eq!(f.extents()[0].sector, 401);
    assert_eq!(f.read(0, 4).unwrap(), b"good");
}

/// A UDF 2.50 image: metadata partition (map 1) over physical map 0; the
/// metadata file (block 0) maps metadata blocks 0.. to physical 20..;
/// the mirror file (block 1) maps them to physical 40...
fn metadata_image() -> Vec<u8> {
    let mut d = vec![0u8; 520 * S];
    let mut map = map_type2("*UDF Metadata Partition", 0);
    put32(&mut map, 40, 0); // metadata file
    put32(&mut map, 44, 1); // mirror file
    put32(&mut map, 48, u32::MAX); // no bitmap
    volume(
        &mut d,
        &Volume {
            label: "META",
            maps: vec![map_type1(0), map],
            fsd: (0, 1),
            partitions: vec![(0, PART_START, 128)],
            integrity_closed: true,
        },
    );
    let mut lb = |lbn: u32, bytes: &[u8]| {
        let at = (PART_START + lbn) as usize * S;
        d[at..at + bytes.len()].copy_from_slice(bytes);
    };
    lb(0, &file_entry(250, 4 * 2048, 4, &Ads::Short(&[(4 * 2048, 20)])));
    lb(1, &file_entry(251, 4 * 2048, 4, &Ads::Short(&[(4 * 2048, 40)])));
    // Metadata blocks: 0 FSD, 1 root FE, 2 root data, 3 file FE.
    let root = directory((1, 1), &[("M", 0, 3, 1)]);
    let file = file_entry(5, 4, 4, &Ads::Long(&[(4, 60, 0)]));
    let root_fe = file_entry(4, root.len() as u64, 4, &Ads::Short(&[(u32::try_from(root.len()).unwrap(), 2)]));
    for base in [20u32, 40] {
        lb(base, &fsd((1, 1)));
        lb(base + 1, &root_fe);
        lb(base + 2, &root);
        lb(base + 3, &file);
    }
    lb(60, b"meta");
    d
}

#[test]
fn a_metadata_partition_reads_through_its_file_and_mirror() {
    let (_im, v) = mount(&metadata_image()).unwrap();
    assert_eq!(v.open("/M").unwrap().read(0, 4).unwrap(), b"meta");
    // Main metadata file entry damaged: the mirror stands in.
    let mut d = metadata_image();
    d[PART_START as usize * S] ^= 0xff;
    let (_im, v) = mount(&d).unwrap();
    assert_eq!(v.open("/M").unwrap().read(0, 4).unwrap(), b"meta");
}

#[test]
fn a_metadata_translation_loop_is_an_error_not_a_hang() {
    // The metadata file maps its blocks into the metadata partition itself
    // (long AD, partition reference 1): block 0 -> 0 -> ...
    let mut d = metadata_image();
    let fe = file_entry(250, 4 * 2048, 4, &Ads::Long(&[(4 * 2048, 0, 1)]));
    let at = PART_START as usize * S;
    d[at..at + S].copy_from_slice(&fe);
    let fe = file_entry(251, 4 * 2048, 4, &Ads::Long(&[(4 * 2048, 0, 1)]));
    let at = (PART_START as usize + 1) * S;
    d[at..at + S].copy_from_slice(&fe);
    assert!(mount(&d).is_err());
}

// ---------------------------------------------------------------------------
// Corpus: every image's dump equals the reference dump.

fn sha256_hex(data: &[u8]) -> String {
    let mut digest = [0u8; 32];
    // SAFETY: the context is allocated, used and freed here.
    unsafe {
        let ctx = av_sha_alloc();
        assert!(!ctx.is_null());
        assert_eq!(av_sha_init(ctx, 256), 0);
        av_sha_update(ctx, data.as_ptr(), data.len());
        av_sha_final(ctx, digest.as_mut_ptr());
        av_free(ctx);
    }
    digest.iter().fold(String::new(), |mut s, b| {
        let _ = write!(s, "{b:02x}");
        s
    })
}

#[derive(Default)]
struct Counts {
    listings: usize,
    files: usize,
    extents: usize,
    contents: usize,
}

/// The dump of one image, in the reference format (see the reference script):
/// info, then a breadth-first tree walk with every listing, every regular
/// file's extents and the sha256 of every file under 64 MiB.
fn dump(path: &Path, n: &mut Counts) -> String {
    let im = Image::open(path);
    let mut out = String::new();
    let Ok(v) = im.mount() else {
        out.push_str("@info 1\n");
        return out;
    };
    let _ = writeln!(out, "@info 0\nlabel {}\nrevision {:#06x}\nyear {}", String::from_utf8_lossy(&v.label()), v.revision(), v.year());
    let mut queue: std::collections::VecDeque<Vec<u8>> = std::collections::VecDeque::from([b"/".to_vec()]);
    while let Some(dir) = queue.pop_front() {
        n.listings += 1;
        let dir_text = String::from_utf8_lossy(&dir).into_owned();
        let Ok(entries) = v.list_bytes(&dir) else {
            let _ = writeln!(out, "@ls 1 {dir_text}");
            continue;
        };
        let mut body = String::new();
        let mut files = Vec::new();
        for (name, is_dir) in entries {
            let text = String::from_utf8_lossy(&name).into_owned();
            let mut child = dir.clone();
            if child.last() != Some(&b'/') {
                child.push(b'/');
            }
            child.extend_from_slice(&name);
            if is_dir {
                let _ = writeln!(body, "d {text}");
                if name != b".." {
                    queue.push_back(child);
                }
                continue;
            }
            match v.open_bytes(&child) {
                Ok(f) => {
                    let _ = writeln!(body, "f {} {text}", f.size());
                    files.push((child, f.size()));
                }
                Err(ENOENT) => {
                    let _ = writeln!(body, "o {text}");
                }
                Err(e) => {
                    let _ = writeln!(body, "f ?({e}) {text}");
                }
            }
        }
        let _ = writeln!(out, "@ls 0 {dir_text}");
        out.push_str(&body);
        for (file, size) in files {
            n.files += 1;
            let ftext = String::from_utf8_lossy(&file).into_owned();
            let f = v.open_bytes(&file).unwrap();
            let _ = writeln!(out, "@ext 0 {ftext}\nsize {size}");
            for e in f.extents() {
                n.extents += 1;
                if e.sector < 0 {
                    let _ = writeln!(out, "- {}", e.count);
                } else {
                    let _ = writeln!(out, "{} {}", e.sector, e.count);
                }
            }
            if size < 64 << 20 {
                n.contents += 1;
                match f.read(0, usize::try_from(size).unwrap()) {
                    Ok(data) => {
                        let _ = writeln!(out, "@sha 0 {} {ftext}", sha256_hex(&data));
                    }
                    Err(_) => {
                        let _ = writeln!(out, "@sha 1 - {ftext}");
                    }
                }
            }
        }
    }
    out
}

/// Exit codes other than 0 are written as 1 (failed).
fn normalise(reference: &str) -> String {
    let mut out = String::new();
    for line in reference.lines() {
        let mut parts = line.splitn(3, ' ');
        match (parts.next(), parts.next(), parts.next()) {
            (Some(h @ ("@info" | "@ls" | "@ext" | "@sha")), Some(rc), rest) if rc != "0" => {
                let _ = write!(out, "{h} 1");
                if let Some(r) = rest {
                    let _ = write!(out, " {r}");
                }
                out.push('\n');
            }
            _ => {
                out.push_str(line);
                out.push('\n');
            }
        }
    }
    out
}

#[test]
fn corpus_dumps_equal_the_reference() {
    let Some(dir) = std::env::var_os("DISCIO_UDF_REFERENCE") else {
        eprintln!("DISCIO_UDF_REFERENCE not set: corpus check skipped");
        return;
    };
    let dir = PathBuf::from(dir);
    let index = std::fs::read_to_string(dir.join("index.tsv")).expect("index.tsv in the reference folder");
    let mut images = 0;
    let mut total = Counts::default();
    let mut diffs = 0;
    let mut checks = 0;
    for line in index.lines().filter(|l| !l.is_empty()) {
        let (id, path) = line.split_once('\t').unwrap();
        let path = Path::new(path);
        if !path.exists() {
            eprintln!("{id}: {} missing, skipped", path.display());
            continue;
        }
        let reference = normalise(&std::fs::read_to_string(dir.join(format!("{id}.txt"))).unwrap());
        let mut n = Counts::default();
        let ours = dump(path, &mut n);
        images += 1;
        let (a, b): (Vec<&str>, Vec<&str>) = (reference.lines().collect(), ours.lines().collect());
        checks += a.len().max(b.len());
        let bad: Vec<usize> = (0..a.len().max(b.len())).filter(|&i| a.get(i) != b.get(i)).collect();
        if bad.is_empty() {
            eprintln!("SAME {id}: {} listings, {} files, {} extents, {} contents", n.listings, n.files, n.extents, n.contents);
        } else {
            diffs += bad.len();
            eprintln!("DIFF {id}: {} of {} lines", bad.len(), a.len().max(b.len()));
            for &i in bad.iter().take(8) {
                eprintln!("    line {}: reference {:?} | ours {:?}", i + 1, a.get(i), b.get(i));
            }
        }
        total.listings += n.listings;
        total.files += n.files;
        total.extents += n.extents;
        total.contents += n.contents;
    }
    eprintln!(
        "corpus: {images} images, {} listings, {} files, {} extents, {} contents compared, {checks} lines, {diffs} differing lines",
        total.listings, total.files, total.extents, total.contents
    );
    assert!(images > 0, "no image in {}", dir.display());
    assert_eq!(diffs, 0, "the dumps differ from the reference");
}
