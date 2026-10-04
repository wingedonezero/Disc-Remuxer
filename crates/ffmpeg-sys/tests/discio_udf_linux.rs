//! `libavformat/discio_udf_linux.c`: the UDF reader based on Linux `fs/udf`,
//! on small, fully valid images built here (volume recognition sequence, tag
//! locations, CRC lengths) and their damaged variants; and the corpus images
//! against reference dumps when `DISCIO_UDF_REFERENCE` names their folder.

#![allow(clippy::many_single_char_names, reason = "one short name per descriptor being built")]

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::Path;

use ffmpeg_sys::discio::{self, Extent, Source, SourceOps};

const ENOENT: c_int = -2;
const INVALIDDATA: c_int = -0x4144_4E49; // FFERRTAG('I','N','D','A')

// ---- the C interface with the UDF fields (discio.h) ----

/// `DiscIOFile` with its embedded data.
#[repr(C)]
struct File {
    size: i64,
    nb_extents: c_int,
    extents: *mut Extent,
    data: *mut u8,
}

type DirCallback = unsafe extern "C" fn(opaque: *mut c_void, name: *const c_char, is_dir: c_int) -> c_int;

/// `DiscIOFSOps`.
#[repr(C)]
struct FsOps {
    name: *const c_char,
    open_file: unsafe extern "C" fn(fs: *mut Fs, path: *const c_char, out: *mut *mut File) -> c_int,
    list_dir: unsafe extern "C" fn(fs: *mut Fs, path: *const c_char, cb: DirCallback, opaque: *mut c_void) -> c_int,
    close: Option<unsafe extern "C" fn(fs: *mut Fs)>,
}

/// `DiscIOFS` with the UDF volume facts.
#[repr(C)]
struct Fs {
    ops: *const FsOps,
    priv_: *mut c_void,
    src: *mut Source,
    label: [c_char; discio::LABEL_SIZE],
    udf_revision: u16,
    udf_recording_time: [u8; 12],
}

extern "C" {
    fn ff_discio_udf_linux_mount(src: *mut Source, out: *mut *mut Fs) -> c_int;
    fn ff_discio_fs_close(fs: *mut *mut Fs);
    fn ff_discio_file_free(file: *mut *mut File);
    fn ff_discio_file_read(fs: *mut Fs, file: *const File, pos: i64, buf: *mut u8, len: c_int) -> c_int;
    fn av_sha_alloc() -> *mut c_void;
    fn av_sha_init(ctx: *mut c_void, bits: c_int) -> c_int;
    fn av_sha_update(ctx: *mut c_void, data: *const u8, len: usize);
    fn av_sha_final(ctx: *mut c_void, digest: *mut u8);
    fn av_free(ptr: *mut c_void);
}

// ---- sources ----

unsafe extern "C" fn mem_read(opaque: *mut c_void, pos: i64, buf: *mut u8, len: c_int) -> c_int {
    // SAFETY: opaque is the boxed image of `Image::memory`.
    let d = unsafe { &*opaque.cast::<Vec<u8>>() };
    let pos = usize::try_from(pos).unwrap();
    if pos >= d.len() {
        return 0;
    }
    let n = (d.len() - pos).min(usize::try_from(len).unwrap());
    // SAFETY: buf has room for len bytes.
    unsafe { std::ptr::copy_nonoverlapping(d.as_ptr().add(pos), buf, n) };
    c_int::try_from(n).unwrap()
}

unsafe extern "C" fn mem_close(opaque: *mut c_void) {
    // SAFETY: opaque came from Box::into_raw in `Image::memory`.
    drop(unsafe { Box::from_raw(opaque.cast::<Vec<u8>>()) });
}

static MEM_OPS: SourceOps = SourceOps { read_at: mem_read, close: Some(mem_close) };

struct Image {
    src: *mut Source,
}

impl Image {
    fn memory(bytes: &[u8]) -> Self {
        let size = i64::try_from(bytes.len()).unwrap();
        let opaque = Box::into_raw(Box::new(bytes.to_vec())).cast::<c_void>();
        // SAFETY: MEM_OPS outlives the source; opaque is released through it.
        let src = unsafe { discio::ff_discio_source_new(std::ptr::null_mut(), c"memory".as_ptr(), size, &raw const MEM_OPS, opaque) };
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

    fn mount(&self) -> Result<Vol, c_int> {
        let mut fs = std::ptr::null_mut();
        // SAFETY: src is open.
        let ret = unsafe { ff_discio_udf_linux_mount(self.src, &raw mut fs) };
        if ret < 0 { Err(ret) } else { Ok(Vol { fs }) }
    }
}

impl Drop for Image {
    fn drop(&mut self) {
        // SAFETY: src came from ff_discio_source_new / _open_file.
        unsafe { discio::ff_discio_source_free(&raw mut self.src) };
    }
}

/// A file as the reader opened it.
#[derive(Debug, PartialEq, Eq)]
struct Opened {
    size: i64,
    extents: Vec<(i64, i64)>,
    data: Option<Vec<u8>>,
}

struct Vol {
    fs: *mut Fs,
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

    fn with_file<T>(&self, path: &str, f: impl FnOnce(*mut File) -> T) -> Result<T, c_int> {
        let c = CString::new(path).unwrap();
        let mut file: *mut File = std::ptr::null_mut();
        // SAFETY: fs is mounted; ops is its table.
        let ret = unsafe { ((*(*self.fs).ops).open_file)(self.fs, c.as_ptr(), &raw mut file) };
        if ret < 0 {
            return Err(ret);
        }
        let out = f(file);
        // SAFETY: file came from open_file.
        unsafe { ff_discio_file_free(&raw mut file) };
        Ok(out)
    }

    fn open(&self, path: &str) -> Result<Opened, c_int> {
        self.with_file(path, |f| {
            // SAFETY: f is a valid file with nb_extents extents and size bytes of data when data is set.
            unsafe {
                let n = usize::try_from((*f).nb_extents).unwrap();
                let extents = if n == 0 { Vec::new() } else { std::slice::from_raw_parts((*f).extents, n).to_vec() };
                let data = (!(*f).data.is_null())
                    .then(|| std::slice::from_raw_parts((*f).data, usize::try_from((*f).size).unwrap()).to_vec());
                Opened { size: (*f).size, extents: extents.into_iter().map(|e| (e.sector, e.count)).collect(), data }
            }
        })
    }

    /// Reads the whole file in pieces of at most 1 MiB, handing each to `sink`.
    fn read_all(&self, path: &str, mut sink: impl FnMut(&[u8])) -> Result<(), c_int> {
        self.with_file(path, |f| {
            // SAFETY: f is valid.
            let size = unsafe { (*f).size };
            let mut buf = vec![0u8; 1 << 20];
            let mut pos = 0i64;
            while pos < size {
                let n = usize::try_from((size - pos).min(1 << 20)).unwrap();
                // SAFETY: buf holds n bytes; the range lies inside the file.
                let ret = unsafe { ff_discio_file_read(self.fs, f, pos, buf.as_mut_ptr(), c_int::try_from(n).unwrap()) };
                if ret < 0 {
                    return Err(ret);
                }
                sink(&buf[..n]);
                pos += i64::try_from(n).unwrap();
            }
            Ok(())
        })?
    }

    fn read(&self, path: &str) -> Vec<u8> {
        let mut out = Vec::new();
        self.read_all(path, |b| out.extend_from_slice(b)).unwrap();
        out
    }

    fn list(&self, path: &str) -> Result<Vec<(Vec<u8>, bool)>, c_int> {
        unsafe extern "C" fn cb(opaque: *mut c_void, name: *const c_char, is_dir: c_int) -> c_int {
            // SAFETY: opaque is the Vec below; name a C string.
            let v = unsafe { &mut *opaque.cast::<Vec<(Vec<u8>, bool)>>() };
            v.push((unsafe { CStr::from_ptr(name) }.to_bytes().to_vec(), is_dir != 0));
            0
        }
        let c = CString::new(path).unwrap();
        let mut out: Vec<(Vec<u8>, bool)> = Vec::new();
        // SAFETY: fs mounted; out outlives the call.
        let ret = unsafe { ((*(*self.fs).ops).list_dir)(self.fs, c.as_ptr(), cb, (&raw mut out).cast()) };
        if ret < 0 { Err(ret) } else { Ok(out) }
    }

    fn names(&self, path: &str) -> Vec<String> {
        self.list(path).unwrap().into_iter().map(|(n, _)| String::from_utf8(n).unwrap()).collect()
    }
}

impl Drop for Vol {
    fn drop(&mut self) {
        // SAFETY: fs came from ff_discio_udf_linux_mount.
        unsafe { ff_discio_fs_close(&raw mut self.fs) };
    }
}

/// The volume first: a tuple drops its fields in order, and the source must
/// outlive the file system.
fn mount(im: &Img) -> Result<(Vol, Image), c_int> {
    let img = Image::memory(&im.d);
    let v = img.mount()?;
    Ok((v, img))
}

// ---- building images (ECMA-167 / OSTA UDF) ----

const S: usize = 2048;
const PART_START: u32 = 300;
const PART_LEN: u32 = 219; // to the last sector of a 520-sector image

/// CRC-ITU-T (polynomial 0x1021, initial 0, MSB first).
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

/// A tag: id, version 2, tag location, CRC over `crclen` bytes, checksum.
fn tag(b: &mut [u8], id: u16, loc: u32, crclen: u16) {
    put16(b, 0, id);
    put16(b, 2, 2);
    put32(b, 12, loc);
    put16(b, 10, crclen);
    let crc = crc16(&b[16..16 + usize::from(crclen)]);
    put16(b, 8, crc);
    b[4] = 0;
    b[4] = b[..16].iter().enumerate().filter(|&(i, _)| i != 4).fold(0u8, |s, (_, &x)| s.wrapping_add(x));
}

/// The same tag with another tag location.
fn retag(b: &mut [u8], loc: u32) {
    let id = u16::from_le_bytes([b[0], b[1]]);
    let crclen = u16::from_le_bytes([b[10], b[11]]);
    tag(b, id, loc, crclen);
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

struct Img {
    d: Vec<u8>,
}

/// One file identifier: (name as recorded, compression ID first; empty =
/// none), characteristics, ICB block, ICB partition.
type Fid = (Vec<u8>, u8, u32, u16);

/// An 8-bit OSTA name.
fn n8(s: &str) -> Vec<u8> {
    let mut v = vec![8u8];
    v.extend_from_slice(s.as_bytes());
    v
}

fn fid(name: &str, chars: u8, icb: u32, part: u16) -> Fid {
    (n8(name), chars, icb, part)
}

impl Img {
    fn sector(&mut self, n: u32) -> &mut [u8] {
        let at = n as usize * S;
        &mut self.d[at..at + S]
    }
    fn put(&mut self, abs: u32, bytes: &[u8]) {
        let at = abs as usize * S;
        self.d[at..at + bytes.len()].copy_from_slice(bytes);
    }
    /// A file entry at partition block `lbn` with short ADs `(len, lbn)`.
    fn file_entry(&mut self, lbn: u32, file_type: u8, size: u64, strategy: u16, ads: &[(u32, u32)]) {
        let mut b = vec![0u8; S];
        put16(&mut b, 0x14, strategy);
        b[0x1b] = file_type;
        put16(&mut b, 0x30, 1); // link count
        put64(&mut b, 0x38, size);
        for (i, &(len, at)) in ads.iter().enumerate() {
            put32(&mut b, 0xb0 + 8 * i, len);
            put32(&mut b, 0xb4 + 8 * i, at);
        }
        put32(&mut b, 0xac, u32::try_from(8 * ads.len()).unwrap());
        tag(&mut b, 0x105, lbn, u16::try_from(176 + 8 * ads.len() - 16).unwrap());
        self.put(PART_START + lbn, &b);
    }
    /// Directory data at partition block `lbn`: parent entry, then `fids`.
    fn directory(&mut self, lbn: u32, parent: u32, fids: &[Fid]) -> u64 {
        let mut out = Vec::new();
        let mut all = vec![(Vec::new(), 0x0a_u8, parent, 0u16)];
        all.extend_from_slice(fids);
        for (name, chars, icb, part) in all {
            let l_fi = name.len();
            let len = (38 + l_fi + 3) & !3;
            let mut b = vec![0u8; len];
            put16(&mut b, 0x10, 1);
            b[0x12] = chars;
            b[0x13] = u8::try_from(l_fi).unwrap();
            put32(&mut b, 0x14, 2048);
            put32(&mut b, 0x18, icb);
            put16(&mut b, 0x1c, part);
            b[0x26..0x26 + l_fi].copy_from_slice(&name);
            tag(&mut b, 0x101, lbn, u16::try_from(len - 16).unwrap());
            out.extend_from_slice(&b);
        }
        self.put(PART_START + lbn, &out);
        out.len() as u64
    }
}

/// Volume structure: VRS (BEA01, NSR02, TEA01 at 16..18), anchor at 256,
/// main VDS at 32 (PVD, IUVD, PD, LVD, TD), reserve at 48, LVID at 64.
fn volume(im: &mut Img, label: &str, lvd_seq: u32) {
    for (i, id) in [b"BEA01", b"NSR02", b"TEA01"].iter().enumerate() {
        let v = im.sector(16 + u32::try_from(i).unwrap());
        v[1..6].copy_from_slice(*id);
        v[6] = 1;
    }
    let a = im.sector(256);
    put32(a, 0x10, 16 * 2048);
    put32(a, 0x14, 32);
    put32(a, 0x18, 16 * 2048);
    put32(a, 0x1c, 48);
    tag(a, 2, 256, 496);
    let p = im.sector(32);
    put16(p, 0x178 + 2, 2009);
    tag(p, 1, 32, 496);
    let iu = im.sector(33);
    iu[0x15..0x21].copy_from_slice(b"*UDF LV Info");
    put16(iu, 0x2c, 0x0102);
    tag(iu, 4, 33, 496);
    let pd = im.sector(34);
    pd[0x14] = 1;
    put16(pd, 0x16, 0);
    pd[0x19..0x1f].copy_from_slice(b"+NSR02");
    put32(pd, 0xb8, 1); // read-only access
    put32(pd, 0xbc, PART_START);
    put32(pd, 0xc0, PART_LEN);
    tag(pd, 5, 34, 496);
    let l = im.sector(35);
    put32(l, 0x10, lvd_seq);
    osta(l, 0x14);
    dstring(l, 0x54, 128, label);
    put32(l, 0xd4, 2048);
    l[0xd9..0xd9 + 19].copy_from_slice(b"*OSTA UDF Compliant");
    put32(l, 0xf8, 2048);
    put32(l, 0xfc, 0);
    put32(l, 0x108, 6);
    put32(l, 0x10c, 1);
    put32(l, 0x1b0, 2048);
    put32(l, 0x1b4, 64);
    l[0x1b8] = 1;
    l[0x1b9] = 6;
    tag(l, 6, 35, 446 - 16);
    tag(im.sector(36), 8, 36, 0);
    for s in 32..37 {
        let mut d: Vec<u8> = im.sector(s).to_vec();
        let loc = s + 16;
        retag(&mut d, loc);
        im.sector(loc).copy_from_slice(&d);
    }
    let v = im.sector(64);
    put32(v, 0x1c, 1);
    put32(v, 0x48, 1);
    put32(v, 0x4c, 46);
    put16(v, 80 + 8 + 40, 0x0102);
    tag(v, 9, 64, 80 + 8 + 46 - 16);
    tag(im.sector(65), 8, 65, 0);
}

fn fsd(im: &mut Img, lbn: u32, root: u32) {
    let mut f = vec![0u8; S];
    put32(&mut f, 0x190, 2048);
    put32(&mut f, 0x194, root);
    f[0x1a1..0x1a1 + 19].copy_from_slice(b"*OSTA UDF Compliant");
    tag(&mut f, 0x100, lbn, 496);
    im.put(PART_START + lbn, &f);
}

/// Root holds `VIDEO_TS` and `README` (5000 bytes at 10..12); `VIDEO_TS` holds
/// `fids` (file entries at 20.., 100 + k bytes at 30..).
fn build(label: &str, fids: &[Fid]) -> Img {
    let mut im = Img { d: vec![0u8; 520 * S] };
    volume(&mut im, label, 1);
    fsd(&mut im, 0, 1);
    let root = im.directory(2, 1, &[fid("VIDEO_TS", 0x02, 3, 0), fid("README", 0, 4, 0)]);
    im.file_entry(1, 4, root, 4, &[(u32::try_from(root).unwrap(), 2)]);
    let vt = im.directory(5, 1, fids);
    im.file_entry(3, 4, vt, 4, &[(u32::try_from(vt).unwrap(), 5)]);
    let base = (PART_START as usize + 10) * S;
    for (i, x) in im.d[base..base + 5000].iter_mut().enumerate() {
        *x = u8::try_from(i % 256).unwrap();
    }
    im.file_entry(4, 5, 5000, 4, &[(5000, 10)]);
    for k in 0..4u32 {
        im.file_entry(20 + k, 5, 100 + u64::from(k), 4, &[(100 + k, 30 + k)]);
    }
    im
}

fn standard() -> Img {
    build("TEST_DISC", &[fid("VIDEO_TS.IFO", 0, 20, 0), fid("VTS_01_0.IFO", 0, 21, 0)])
}

fn p(lbn: u32) -> i64 {
    i64::from(PART_START + lbn)
}

// ---- tests on built images ----

#[test]
fn mounts_lists_and_reads() {
    let (v, _img) = mount(&standard()).unwrap();
    assert_eq!(v.label(), b"TEST_DISC");
    assert_eq!(v.revision(), 0x0102);
    assert_eq!(v.year(), 2009);
    assert_eq!(
        v.list("/").unwrap(),
        vec![(b"..".to_vec(), true), (b"VIDEO_TS".to_vec(), true), (b"README".to_vec(), false)]
    );
    assert_eq!(v.open("/README").unwrap(), Opened { size: 5000, extents: vec![(p(10), 3)], data: None });
    let buf = v.read("/README");
    assert!(buf.iter().enumerate().all(|(i, &x)| x == u8::try_from(i % 256).unwrap()));
    assert_eq!(v.open("/VIDEO_TS"), Err(ENOENT), "a directory is not a file");
    assert_eq!(v.open("/NOPE"), Err(ENOENT));
    assert_eq!(v.list("/README"), Err(ENOENT), "a file is not a directory");
}

#[test]
fn extents_of_a_valid_image() {
    // (the archived test compared these with the NetBSD-based reader)
    let (v, _img) = mount(&standard()).unwrap();
    assert_eq!(v.open("/VIDEO_TS/VIDEO_TS.IFO").unwrap().extents, vec![(p(30), 1)]);
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO").unwrap().extents, vec![(p(31), 1)]);
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO").unwrap().size, 101);
}

#[test]
fn adjacent_pieces_are_joined() {
    // README in three allocation descriptors: 2 blocks at 10, 1 at 12 (adjacent), 1 at 40.
    let mut im = standard();
    im.file_entry(4, 5, 4 * 2048, 4, &[(2 * 2048, 10), (2048, 12), (2048, 40)]);
    let (v, _img) = mount(&im).unwrap();
    assert_eq!(v.open("/README").unwrap().extents, vec![(p(10), 3), (p(40), 1)]);
}

#[test]
fn blocks_past_the_allocation_are_one_unrecorded_run() {
    // README records 2^50 bytes but has one block of allocation: the rest is
    // not recorded (and mapping it must not take one step per block).
    let mut im = standard();
    im.file_entry(4, 5, 1 << 50, 4, &[(2048, 10)]);
    let (v, _img) = mount(&im).unwrap();
    assert_eq!(v.open("/README").unwrap().extents, vec![(p(10), 1), (-1, (1 << 39) - 1)]);
}

#[test]
fn paths_follow_the_vfs() {
    let (v, _img) = mount(&standard()).unwrap();
    assert!(v.open("/VIDEO_TS/./VIDEO_TS.IFO").is_ok());
    assert!(v.open("/VIDEO_TS/../VIDEO_TS/VIDEO_TS.IFO").is_ok());
    assert!(v.open("/../README").is_ok(), "the root's parent is the root");
    assert_eq!(v.open("\\VIDEO_TS\\VIDEO_TS.IFO"), Err(ENOENT), "'\\' is a plain character");
    assert_eq!(v.names("/VIDEO_TS/.."), vec!["..", "VIDEO_TS", "README"]);
}

#[test]
fn hidden_entries_are_skipped_and_duplicates_resolve_to_the_first() {
    let (v, _img) = mount(&build("H", &[fid("SEEN.IFO", 0, 20, 0), fid("HIDDEN.IFO", 0x01, 21, 0)])).unwrap();
    assert_eq!(v.names("/VIDEO_TS"), vec!["..", "SEEN.IFO"]);
    assert_eq!(v.open("/VIDEO_TS/HIDDEN.IFO"), Err(ENOENT));
    drop(v);
    let (v, _img) = mount(&build("D", &[fid("A.IFO", 0, 20, 0), fid("A.IFO", 0, 22, 0)])).unwrap();
    assert_eq!(v.open("/VIDEO_TS/A.IFO").unwrap().size, 100);
    assert_eq!(v.names("/VIDEO_TS"), vec!["..", "A.IFO", "A.IFO"]);
}

#[test]
fn names_keep_windows_characters_and_mangle_slash() {
    let (v, _img) = mount(&build("N", &[fid("A:B?C.", 0, 20, 0), fid("a/b", 0, 21, 0)])).unwrap();
    let n = v.names("/VIDEO_TS");
    assert_eq!(n[1], "A:B?C.", "only '/' and NUL are changed");
    assert_eq!(n[2], format!("a_b#{:04X}", crc16(b"a/b")));
}

#[test]
fn names_are_translated_as_linux_does() {
    let mut smiley = vec![16u8]; // U+1F600 as UTF-16: D83D DE00
    smiley.extend_from_slice(&[0xD8, 0x3D, 0xDE, 0x00]);
    let fids = [
        fid(".", 0, 20, 0),
        fid("..", 0, 21, 0),
        fid("x/y.txt", 0, 22, 0),
        (smiley, 0, 23, 0),
        (vec![3, b'a'], 0, 20, 0),  // unknown compression ID: skipped
        (vec![16, b'a'], 0, 20, 0), // odd 16-bit length: skipped
    ];
    let (v, _img) = mount(&build("U", &fids)).unwrap();
    assert_eq!(
        v.names("/VIDEO_TS"),
        vec![
            "..".to_string(),
            format!(".#{:04X}", crc16(b".")),
            format!("..#{:04X}", crc16(b"..")),
            format!("x_y#{:04X}.txt", crc16(b"x/y.txt")),
            "\u{1F600}".to_string(),
        ]
    );
    assert_eq!(v.open("/VIDEO_TS/\u{1F600}").unwrap().size, 103);
}

#[test]
fn an_unconvertible_name_fails_a_lookup_in_its_directory() {
    // Linux's lookup fails on a name it cannot convert (the listing skips it).
    let (v, _img) = mount(&build("B", &[(vec![3, b'a'], 0, 20, 0), fid("B.IFO", 0, 21, 0)])).unwrap();
    assert_eq!(v.open("/VIDEO_TS/B.IFO"), Err(INVALIDDATA));
    assert_eq!(v.list("/VIDEO_TS"), Err(INVALIDDATA), "the listing looks every name up");
}

#[test]
fn a_volume_without_a_recognition_sequence_is_refused() {
    let mut im = standard();
    for s in 16..19 {
        im.sector(s).fill(0);
    }
    assert!(mount(&im).is_err());
}

#[test]
fn the_highest_sequence_number_wins() {
    // A second LVD (label SECOND) with a LOWER sequence number after the
    // first: Linux keeps the first.
    let mut im = standard();
    let mut l2: Vec<u8> = im.sector(35).to_vec();
    l2[0x54..0xd4].fill(0);
    dstring(&mut l2, 0x54, 128, "SECOND");
    put32(&mut l2, 0x10, 0);
    tag(&mut l2, 6, 36, 446 - 16);
    let mut td: Vec<u8> = im.sector(36).to_vec();
    tag(&mut td, 8, 37, 0);
    im.sector(36).copy_from_slice(&l2);
    im.sector(37).copy_from_slice(&td);
    assert_eq!(mount(&im).unwrap().0.label(), b"TEST_DISC");
}

#[test]
fn the_reserve_sequence_is_used_when_the_main_one_has_no_primary_descriptor() {
    let mut im = standard();
    im.sector(32).fill(0);
    let (v, _img) = mount(&im).unwrap();
    assert_eq!(v.label(), b"TEST_DISC");
    assert_eq!(v.open("/README").unwrap().size, 5000);
}

#[test]
fn a_wrong_tag_location_is_not_a_descriptor() {
    let mut im = standard();
    let mut moved = im.sector(PART_START + 21).to_vec();
    retag(&mut moved, 99);
    im.put(PART_START + 21, &moved);
    let (v, _img) = mount(&im).unwrap();
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO"), Err(INVALIDDATA));
}

#[test]
fn strategy_4096_follows_an_indirect_entry_in_the_next_block() {
    let mut im = standard();
    // VTS_01_0.IFO (block 21): strategy 4096; block 22 holds an indirect
    // entry to block 40, which holds the real file entry (size 101).
    im.file_entry(40, 5, 101, 4, &[(101, 31)]);
    im.file_entry(21, 5, 7, 4096, &[(7, 31)]);
    let mut ie = vec![0u8; S];
    put16(&mut ie, 0x14, 4096);
    put32(&mut ie, 0x24, 2048);
    put32(&mut ie, 0x28, 40);
    tag(&mut ie, 0x103, 22, 52 - 16);
    im.put(PART_START + 22, &ie);
    assert_eq!(mount(&im).unwrap().0.open("/VIDEO_TS/VTS_01_0.IFO").unwrap().size, 101);
}

#[test]
fn an_unknown_strategy_or_file_type_is_an_error() {
    let mut im = standard();
    im.file_entry(21, 5, 101, 7, &[(101, 31)]);
    assert_eq!(mount(&im).unwrap().0.open("/VIDEO_TS/VTS_01_0.IFO"), Err(INVALIDDATA));
    let mut im = standard();
    im.file_entry(21, 13, 101, 4, &[(101, 31)]);
    assert_eq!(mount(&im).unwrap().0.open("/VIDEO_TS/VTS_01_0.IFO"), Err(INVALIDDATA));
}

#[test]
fn a_directory_entry_whose_crc_length_does_not_match_breaks_the_directory() {
    let mut im = standard();
    let at = (PART_START as usize + 5) * S + 40;
    let b = &mut im.d[at..at + 52];
    put16(b, 10, 0);
    b[4] = 0;
    b[4] = b[..16].iter().enumerate().filter(|&(i, _)| i != 4).fold(0u8, |s, (_, &x)| s.wrapping_add(x));
    let (v, _img) = mount(&im).unwrap();
    assert_eq!(v.list("/VIDEO_TS"), Err(INVALIDDATA));
}

#[test]
fn embedded_data_and_unrecorded_extents() {
    let mut im = standard();
    let mut b = vec![0u8; S];
    put16(&mut b, 0x14, 4);
    b[0x1b] = 5;
    put16(&mut b, 0x22, 3);
    put16(&mut b, 0x30, 1);
    put64(&mut b, 0x38, 5);
    put32(&mut b, 0xac, 5);
    b[0xb0..0xb5].copy_from_slice(b"hello");
    tag(&mut b, 0x105, 21, 176 + 5 - 16);
    im.put(PART_START + 21, &b);
    // VIDEO_TS.IFO: one not-recorded extent of 3000 bytes.
    im.file_entry(20, 5, 3000, 4, &[((1 << 30) | 3000, 0)]);
    let (v, _img) = mount(&im).unwrap();
    assert_eq!(v.open("/VIDEO_TS/VTS_01_0.IFO").unwrap(), Opened { size: 5, extents: vec![], data: Some(b"hello".to_vec()) });
    assert_eq!(v.read("/VIDEO_TS/VTS_01_0.IFO"), b"hello");
    // not recorded: sector -1 (ff_discio_file_read does not serve such runs yet)
    assert_eq!(v.open("/VIDEO_TS/VIDEO_TS.IFO").unwrap().extents, vec![(-1, 2)]);
}

#[test]
fn a_vat_in_the_last_block_maps_a_virtual_partition() {
    let mut im = Img { d: vec![0u8; 520 * S] };
    volume(&mut im, "VAT", 1);
    // LVD: maps type 1 (partition 0) + virtual (partition 0, UDF 2.00); FSD
    // at virtual block 0.
    let l = im.sector(35);
    put32(l, 0x108, 6 + 64);
    put32(l, 0x10c, 2);
    put16(l, 0x100, 1);
    let m = 0x1b8 + 6;
    l[m] = 2;
    l[m + 1] = 64;
    l[m + 5..m + 5 + 22].copy_from_slice(b"*UDF Virtual Partition");
    put16(l, m + 28, 0x0200);
    put16(l, m + 38, 0);
    tag(l, 6, 35, 446 + 64 - 16);
    // Physical: FSD 10, root FE 11, root data 12, file FE 13, data 14.
    fsd(&mut im, 10, 1);
    // tag locations of virtual-partition descriptors are their virtual block
    let mut f = im.sector(PART_START + 10).to_vec();
    put16(&mut f, 0x198, 1);
    tag(&mut f, 0x100, 0, 496);
    im.put(PART_START + 10, &f);
    let root = im.directory(12, 1, &[fid("F.TXT", 0, 3, 1)]);
    let base = (PART_START as usize + 12) * S;
    let mut d: Vec<u8> = im.d[base..base + usize::try_from(root).unwrap()].to_vec();
    let mut at = 0;
    while at < d.len() {
        let len = 16 + usize::from(u16::from_le_bytes([d[at + 10], d[at + 11]]));
        if at == 0 {
            put16(&mut d[at..], 0x1c, 1);
        }
        tag(&mut d[at..at + len], 0x101, 2, u16::try_from(len - 16).unwrap());
        at += len;
    }
    im.put(PART_START + 12, &d);
    im.file_entry(11, 4, root, 4, &[(u32::try_from(root).unwrap(), 2)]);
    let mut fe = im.sector(PART_START + 11).to_vec();
    retag(&mut fe, 1);
    im.put(PART_START + 11, &fe);
    im.file_entry(13, 5, 5, 4, &[(5, 14)]);
    let mut fe = im.sector(PART_START + 13).to_vec();
    // Data through the type-1 partition: long AD (5, 14, part 0).
    fe[0x22] = 1;
    put32(&mut fe, 0xac, 16);
    put32(&mut fe, 0xb0, 5);
    put32(&mut fe, 0xb4, 14);
    put16(&mut fe, 0xb8, 0);
    tag(&mut fe, 0x105, 3, 176 + 16 - 16);
    im.put(PART_START + 13, &fe);
    im.put(PART_START + 14, b"virt!");
    // VAT 2.00 data at partition block 217, its file entry in the last
    // block (518 = partition block 218).
    let mut vat = vec![0u8; 152 + 16];
    put16(&mut vat, 0, 152);
    for (k, p) in [10u32, 11, 12, 13].iter().enumerate() {
        put32(&mut vat, 152 + 4 * k, *p);
    }
    im.put(PART_START + 217, &vat);
    im.file_entry(218, 248, vat.len() as u64, 4, &[(u32::try_from(vat.len()).unwrap(), 217)]);
    let d = im.d.len() / S;
    assert_eq!(PART_START + 218, u32::try_from(d - 2).unwrap());
    // Make the VAT entry the image's last block.
    im.d.truncate((PART_START as usize + 219) * S);
    let (v, _img) = mount(&im).unwrap();
    assert_eq!(v.label(), b"VAT");
    assert_eq!(v.read("/F.TXT"), b"virt!");
    assert_eq!(v.open("/F.TXT").unwrap().extents, vec![(p(14), 1)]);
}

// ---- corpus: the reader against reference dumps ----

fn sha256_hex(v: &Vol, path: &str) -> Result<String, c_int> {
    // SAFETY: the context is used only between alloc and free.
    unsafe {
        let ctx = av_sha_alloc();
        assert!(!ctx.is_null());
        assert_eq!(av_sha_init(ctx, 256), 0);
        let r = v.read_all(path, |b| av_sha_update(ctx, b.as_ptr(), b.len()));
        let mut digest = [0u8; 32];
        av_sha_final(ctx, digest.as_mut_ptr());
        av_free(ctx);
        r.map(|()| {
            digest.iter().fold(String::new(), |mut s, b| {
                use std::fmt::Write;
                let _ = write!(s, "{b:02x}");
                s
            })
        })
    }
}

#[derive(Default)]
struct Counts {
    listings: usize,
    files: usize,
    extents: usize,
    contents: usize,
}

/// The dump of one image, in the reference format (see the reference
/// script): info, every listing by tree walk, every file's extents, sha256 of
/// files under 64 MiB.
fn dump(img: &Image, key: &str, n: &mut Counts) -> Vec<String> {
    let mut out = vec![format!("image {key}")];
    let Ok(v) = img.mount() else {
        out.push("info rc 1".into());
        return out;
    };
    out.push("info rc 0".into());
    out.push(format!("label {}", String::from_utf8_lossy(&v.label())));
    out.push(format!("revision {:#06x}", v.revision()));
    out.push(format!("year {}", v.year()));
    let mut queue = std::collections::VecDeque::from(["/".to_string()]);
    while let Some(d) = queue.pop_front() {
        n.listings += 1;
        let Ok(entries) = v.list(&d) else {
            out.push(format!("ls {d} rc 1"));
            continue;
        };
        out.push(format!("ls {d} rc 0"));
        let base = d.trim_end_matches('/').to_string();
        let mut files = Vec::new();
        for (name, is_dir) in entries {
            let name = String::from_utf8_lossy(&name).into_owned();
            let path = format!("{base}/{name}");
            if is_dir {
                out.push(format!("d {name}"));
                if name != ".." {
                    queue.push_back(path);
                }
            } else {
                match v.open(&path) {
                    Ok(f) => {
                        out.push(format!("f {} {name}", f.size));
                        files.push((path, f));
                    }
                    Err(ENOENT) => out.push(format!("o {name}")),
                    Err(e) => out.push(format!("f? {e} {name}")),
                }
            }
        }
        for (path, f) in files {
            n.files += 1;
            out.push(format!("ext {path} rc 0"));
            out.push(format!("size {}", f.size));
            if let Some(data) = &f.data {
                out.push(format!("embedded {}", data.len()));
            }
            for (s, c) in &f.extents {
                n.extents += 1;
                out.push(if *s < 0 { format!("- {c}") } else { format!("{s} {c}") });
            }
            if f.size < 64 << 20 {
                n.contents += 1;
                match sha256_hex(&v, &path) {
                    Ok(h) => out.push(format!("cat {path} rc 0 {h}")),
                    Err(_) => out.push(format!("cat {path} rc 1 -")),
                }
            }
        }
    }
    out
}

#[test]
fn corpus_matches_the_reference_dumps() {
    let Some(dir) = std::env::var_os("DISCIO_UDF_REFERENCE") else {
        eprintln!("DISCIO_UDF_REFERENCE not set: corpus check skipped");
        return;
    };
    let dir = Path::new(&dir);
    let index = std::fs::read_to_string(dir.join("index.tsv")).expect("index.tsv in the reference folder");
    let mut images = 0;
    let mut total = Counts::default();
    let mut diffs = 0;
    for line in index.lines().filter(|l| !l.is_empty()) {
        let (key, path) = line.split_once('\t').unwrap();
        let path = Path::new(path);
        if !path.exists() {
            eprintln!("{}: image missing (link to an unmounted drive?), skipped", path.display());
            continue;
        }
        let want: Vec<String> =
            std::fs::read_to_string(dir.join(format!("{key}.txt"))).unwrap().lines().map(str::to_string).collect();
        let img = Image::open(path);
        let mut n = Counts::default();
        let got = dump(&img, key, &mut n);
        images += 1;
        let bad: Vec<usize> = (0..want.len().max(got.len())).filter(|&i| want.get(i) != got.get(i)).collect();
        if bad.is_empty() {
            eprintln!("SAME {key}: {} listings, {} files, {} extents, {} contents", n.listings, n.files, n.extents, n.contents);
        } else {
            diffs += bad.len();
            eprintln!("DIFF {key}: {} of {} lines", bad.len(), want.len().max(got.len()));
            for &i in bad.iter().take(8) {
                eprintln!("    line {}: reference {:?} | ours {:?}", i + 1, want.get(i), got.get(i));
            }
        }
        total.listings += n.listings;
        total.files += n.files;
        total.extents += n.extents;
        total.contents += n.contents;
    }
    eprintln!(
        "corpus: {images} images, {} listings, {} files, {} extents, {} contents compared, {diffs} differing lines",
        total.listings, total.files, total.extents, total.contents
    );
    assert!(images > 0, "no image of the index exists");
    assert_eq!(diffs, 0, "the reader differs from the reference dumps");
}
