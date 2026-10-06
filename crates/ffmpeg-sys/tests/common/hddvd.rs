//! HD DVD images built for the tests: an ISO 9660 tree with `ADV_OBJ` (the
//! playlist) and `HVDVD_TS` (the Advanced VTS information file, time maps and
//! EVO files), mounted from memory through the disc readers.

use std::ffi::CStr;
use std::os::raw::{c_int, c_void};
use std::ptr;

use super::{iso, Img, D, S};
use ffmpeg_sys::discio::{self, Fs, Source, SourceOps};
use ffmpeg_sys::hddvd;

/// An EVOB record of the VTI: file name, slot, start / end time (90 kHz).
pub struct Evob {
    pub name: &'static str,
    pub slot: u16,
    pub start: u32,
    pub end: u32,
}

pub fn evob(name: &'static str, slot: u16, secs: u32) -> Evob {
    Evob { name, slot, start: 1000, end: 1000 + secs * 90000 }
}

/// The VTI: one attribute record, the EVOB records.
pub fn vti(evobs: &[Evob]) -> Vec<u8> {
    vti_with(evobs, &vec![1; evobs.len()], &[vec![0u8; 0x206]])
}

/// The VTI with attribute records `attrs` (0x206 bytes each) and, per EVOB,
/// its attribute record (1-based).
pub fn vti_with(evobs: &[Evob], attr_of: &[u32], attrs: &[Vec<u8>]) -> Vec<u8> {
    let mut a = vec![0u8; 8 + 4 * attrs.len()];
    a[0..2].copy_from_slice(&u16::try_from(attrs.len()).unwrap().to_be_bytes());
    for (i, r) in attrs.iter().enumerate() {
        let off = u32::try_from(a.len()).unwrap();
        a[8 + 4 * i..12 + 4 * i].copy_from_slice(&off.to_be_bytes());
        assert_eq!(r.len(), 0x206);
        a.extend_from_slice(r);
    }
    let mut b = vec![0u8; 8 + 4 * evobs.len()];
    b[0..4].copy_from_slice(&u32::try_from(evobs.len()).unwrap().to_be_bytes());
    for (i, e) in evobs.iter().enumerate() {
        let off = u32::try_from(b.len()).unwrap();
        b[8 + 4 * i..12 + 4 * i].copy_from_slice(&off.to_be_bytes());
        let mut r = vec![0u8; 0x140];
        r[2..2 + e.name.len()].copy_from_slice(e.name.as_bytes());
        r[0x106..0x10a].copy_from_slice(&attr_of[i].to_be_bytes());
        r[0x10a..0x10e].copy_from_slice(&e.start.to_be_bytes());
        r[0x10e..0x112].copy_from_slice(&e.end.to_be_bytes());
        r[0x112..0x116].copy_from_slice(&1u32.to_be_bytes());
        r[0x116..0x118].copy_from_slice(&e.slot.to_be_bytes());
        b.extend(r);
    }
    let mut f = vec![0u8; S];
    f[..12].copy_from_slice(b"ADVANCED-VTS");
    f[0xb8..0xbc].copy_from_slice(&1u32.to_be_bytes());
    let blocks_a = u32::try_from(a.len().div_ceil(S)).unwrap();
    f[0xbc..0xc0].copy_from_slice(&(1 + blocks_a).to_be_bytes());
    a.resize(usize::try_from(blocks_a).unwrap() * S, 0);
    f.extend(a);
    f.extend(b);
    f
}

/// An attribute record: video attribute bytes 0x02..0x04, audio entries
/// (first byte each), sub-picture entries (5 bytes each), 32 palette words.
pub fn attr_record(video: [u8; 3], audio: &[u8], subs: &[[u8; 5]], palette: &[u32; 32]) -> Vec<u8> {
    let mut r = vec![0u8; 0x206];
    r[2..5].copy_from_slice(&video);
    r[0x0e..0x10].copy_from_slice(&u16::try_from(audio.len()).unwrap().to_be_bytes());
    for (j, a) in audio.iter().enumerate() {
        r[0x10 + 4 * j] = *a;
    }
    r[0xe4..0xe6].copy_from_slice(&u16::try_from(subs.len()).unwrap().to_be_bytes());
    for (j, p) in subs.iter().enumerate() {
        r[0xe6 + 5 * j..0xeb + 5 * j].copy_from_slice(p);
    }
    for (j, w) in palette.iter().enumerate() {
        r[0x186 + 4 * j..0x18a + 4 * j].copy_from_slice(&w.to_be_bytes());
    }
    r
}

/// A time map of `blocks` blocks (entries of at most 8000 blocks).
pub fn tmap_blocks(blocks: usize) -> Vec<u8> {
    let mut counts = Vec::new();
    let mut left = blocks;
    while left > 0 {
        let n = left.min(8000);
        counts.push(u16::try_from(n).unwrap());
        left -= n;
    }
    tmap(&counts, 0, 0, false)
}

/// A pack: pack header, the packets, then a padding packet over the rest.
pub fn pack(packets: &[Vec<u8>]) -> Vec<u8> {
    let mut s = pack_header();
    let mut at = 14;
    for p in packets {
        s[at..at + p.len()].copy_from_slice(p);
        at += p.len();
    }
    let rem = 2048 - at;
    if rem > 0 {
        assert!(rem >= 6, "{rem} bytes left: no room for a padding packet");
        s[at..at + 4].copy_from_slice(&[0, 0, 1, 0xbe]);
        s[at + 4..at + 6].copy_from_slice(&u16::try_from(rem - 6).unwrap().to_be_bytes());
        s[at + 6..].fill(0xff);
    }
    s
}

/// A private stream 1 packet: sub-stream id, PTS (90 kHz) when given, the
/// 4-byte audio sub-stream header (1 frame, first access unit at 1), payload.
pub fn ps1_packet(sub: u8, pts: Option<u64>, payload: &[u8]) -> Vec<u8> {
    let hdr: Vec<u8> = match pts {
        Some(t) => {
            let b = |v: u64| u8::try_from(v & 0xff).unwrap();
            vec![0x81, 0x80, 5, 0x21 | b((t >> 29) & 0x0e), b(t >> 22), 0x01 | b((t >> 14) & 0xfe), b(t >> 7), 0x01 | b((t << 1) & 0xfe)]
        }
        None => vec![0x81, 0x00, 0],
    };
    let len = hdr.len() + 4 + payload.len();
    let mut p = vec![0, 0, 1, 0xbd];
    p.extend_from_slice(&u16::try_from(len).unwrap().to_be_bytes());
    p.extend(hdr);
    p.extend_from_slice(&[sub, 1, 0, 1]);
    p.extend_from_slice(payload);
    p
}

/// A video packet (stream 0xe0, no PTS) of `len` bytes in all.
pub fn video_packet(len: usize) -> Vec<u8> {
    let mut p = vec![0u8; len];
    p[..4].copy_from_slice(&[0, 0, 1, 0xe0]);
    p[4..6].copy_from_slice(&u16::try_from(len - 6).unwrap().to_be_bytes());
    p[6] = 0x81;
    p
}

/// An E-AC-3 frame of `size` bytes (48 kHz, 6 blocks, 2/0): stream type, and
/// for a dependent stream the channel map.
pub fn eac3_frame(size: usize, strmtyp: u8, chanmap: Option<u16>) -> Vec<u8> {
    let mut f = vec![0u8; size];
    let w = (u16::from(strmtyp) << 14) | u16::try_from(size / 2 - 1).unwrap();
    f[0] = 0x0b;
    f[1] = 0x77;
    f[2..4].copy_from_slice(&w.to_be_bytes());
    f[4] = (3 << 4) | (2 << 1); // fscod 0, numblkscod 3, acmod 2, lfeon 0
    f[5] = 16 << 3; // bsid 16, dialnorm 0
    if let Some(m) = chanmap {
        // byte 6: dialnorm (2 bits), compre 0, chanmape 1, chanmap bits 15..12
        let [hi, lo] = m.to_be_bytes();
        f[6] = 0x10 | (hi >> 4);
        f[7] = (hi << 4) | (lo >> 4);
        f[8] = lo << 4;
    }
    f
}

/// An AC-3 frame: 48 kHz, 128 kb/s (512 bytes), bsid 8.
pub fn ac3_frame() -> Vec<u8> {
    let mut f = vec![0u8; 512];
    f[0] = 0x0b;
    f[1] = 0x77;
    f[4] = 16; // fscod 0, frmsizecod 16 (128 kb/s)
    f[5] = 8 << 3;
    f
}
/// A time map: its first table's block counts (each entry's last u16), the
/// first table's value, header byte 0x14, and a second table when `two`.
pub fn tmap(counts: &[u16], value: u16, byte14: u8, two: bool) -> Vec<u8> {
    let tables: u16 = if two { 2 } else { 1 };
    let mut m = vec![0u8; 0x200];
    m[..12].copy_from_slice(b"HDDVD_TMAP00");
    m[0x14] = byte14;
    m[0x37..0x39].copy_from_slice(&tables.to_be_bytes());
    for t in 0..usize::from(tables) {
        let off = u32::try_from(m.len()).unwrap();
        let d = 0x180 + 0x20 * t;
        m[d..d + 4].copy_from_slice(&off.to_be_bytes());
        m[d + 6..d + 8].copy_from_slice(&u16::try_from(counts.len()).unwrap().to_be_bytes());
        m[d + 8..d + 10].copy_from_slice(&(if t == 0 { value } else { 1 }).to_be_bytes());
        for &c in counts {
            m.extend_from_slice(&[0xab, 0xcd]); // first u16: not used
            m.extend_from_slice(&(c | 0xe000).to_be_bytes()); // top 3 bits not counted
        }
    }
    m
}

/// A file of the image: folder, name, content.
pub type File = (&'static str, &'static str, Vec<u8>);

/// The image: every file from sector 100 on, in its folder.
pub fn image(files: &[File]) -> Img {
    let mut im = Img::new();
    let mut next = 100u32;
    let mut dirs: Vec<D> = Vec::new();
    for (dir, name, data) in files {
        let blocks = u32::try_from(data.len().div_ceil(S).max(1)).unwrap();
        let end = usize::try_from(next + blocks).unwrap() * S;
        if im.d.len() < end {
            im.d.resize(end, 0);
        }
        im.put(next, data);
        let entry = (*name, next, u32::try_from(data.len()).unwrap());
        match dirs.iter_mut().find(|d| d.0 == *dir) {
            Some(d) => d.1.push(entry),
            None => dirs.push((*dir, vec![entry])),
        }
        next += blocks;
    }
    iso(&mut im, "HDDVD", &dirs, None);
    im
}

struct Mem(Vec<u8>);

unsafe extern "C" fn mem_read(opaque: *mut c_void, pos: i64, buf: *mut u8, len: c_int) -> c_int {
    // SAFETY: opaque is the boxed Mem of `plan`.
    let m = unsafe { &*opaque.cast::<Mem>() };
    let (p, n) = (usize::try_from(pos).unwrap(), usize::try_from(len).unwrap());
    if p >= m.0.len() {
        return 0;
    }
    let k = n.min(m.0.len() - p);
    unsafe { ptr::copy_nonoverlapping(m.0.as_ptr().add(p), buf, k) };
    c_int::try_from(k).unwrap()
}

unsafe extern "C" fn mem_close(opaque: *mut c_void) {
    // SAFETY: opaque came from Box::into_raw in `plan`.
    drop(unsafe { Box::from_raw(opaque.cast::<Mem>()) });
}

static MEM_OPS: SourceOps = SourceOps { read_at: mem_read, close: Some(mem_close) };

/// The title plan of a built image: Ok(dump) or Err(code).
pub fn plan(im: &Img, min_length: c_int) -> Result<String, c_int> {
    let opaque = Box::into_raw(Box::new(Mem(im.d.clone()))).cast::<c_void>();
    let size = i64::try_from(im.d.len()).unwrap();
    let mut src: *mut Source =
        unsafe { discio::ff_discio_source_new(ptr::null_mut(), c"memory".as_ptr(), size, &raw const MEM_OPS, opaque) };
    let mut fs: *mut Fs = ptr::null_mut();
    let (mut vti, mut xs, mut nb, mut p) = (ptr::null_mut(), ptr::null_mut(), 0, ptr::null_mut());
    let out = unsafe {
        assert_eq!(discio::ff_discio_mount_image(src, ptr::null(), &raw mut fs), 0);
        assert_eq!(hddvd::ff_hddvd_vti_open(ptr::null_mut(), fs, &raw mut vti), 0);
        assert_eq!(hddvd::ff_hddvd_xpl_load(ptr::null_mut(), fs, &raw mut xs, &raw mut nb), 0);
        let ret = hddvd::ff_hddvd_titles_plan(ptr::null_mut(), fs, vti, xs, nb, min_length, &raw mut p);
        let out = if ret < 0 {
            assert!(p.is_null());
            Err(ret)
        } else {
            let d = hddvd::ff_hddvd_titles_dump(p);
            let s = CStr::from_ptr(d).to_str().unwrap().to_owned();
            ffmpeg_sys::av_free(d.cast());
            Ok(s)
        };
        hddvd::ff_hddvd_titles_free(&raw mut p);
        hddvd::ff_hddvd_xpl_free_all(&raw mut xs, nb);
        hddvd::ff_hddvd_vti_free(&raw mut vti);
        discio::ff_discio_fs_close(&raw mut fs);
        discio::ff_discio_source_free(&raw mut src);
        out
    };
    out
}

/// A `PrimaryAudioVideoClip` element.
pub fn clip(map: &str, begin: &str, end: &str, seamless: bool) -> String {
    format!(
        r#"<PrimaryAudioVideoClip src="file:///dvddisc/HVDVD_TS/{map}" titleTimeBegin="{begin}" titleTimeEnd="{end}" seamless="{seamless}"/>"#
    )
}

/// A playlist with one `TitleSet` holding `body`.
pub fn playlist(title_set_attrs: &str, body: &str) -> Vec<u8> {
    format!(r#"<?xml version="1.0"?><Playlist><TitleSet {title_set_attrs}>{body}</TitleSet></Playlist>"#).into_bytes()
}

// ---- AACS: cryptography through libavutil, packs, opening a disc as the demuxer does ----

use ffmpeg_sys::avcrypto;
use ffmpeg_sys::hddvd::{Aacs, TitlePlan, Vti, Xpl};
use std::ffi::CString;
use std::fmt::Write as _;

pub const AACS_IV: [u8; 16] = [0x0b, 0xa0, 0xf8, 0xdd, 0xfe, 0xa6, 0x1f, 0xb3, 0xd8, 0xdf, 0x9f, 0x56, 0x6a, 0x05, 0x0f, 0x78];

fn aes(key: &[u8; 16], data: &[u8], decrypt: bool, iv: Option<&[u8; 16]>) -> Vec<u8> {
    let mut out = vec![0u8; data.len()];
    unsafe {
        let a = avcrypto::av_aes_alloc();
        assert_eq!(avcrypto::av_aes_init(a, key.as_ptr(), 128, c_int::from(decrypt)), 0);
        let mut iv = iv.copied();
        let ivp = iv.as_mut().map_or(ptr::null_mut(), |v| v.as_mut_ptr());
        avcrypto::av_aes_crypt(a, out.as_mut_ptr(), data.as_ptr(), c_int::try_from(data.len() / 16).unwrap(), ivp, c_int::from(decrypt));
        ffmpeg_sys::av_free(a);
    }
    out
}

pub fn aes_e(key: &[u8; 16], block: &[u8; 16]) -> [u8; 16] {
    aes(key, block, false, None).try_into().unwrap()
}

pub fn aes_d(key: &[u8; 16], block: &[u8; 16]) -> [u8; 16] {
    aes(key, block, true, None).try_into().unwrap()
}

/// AES-G: AES-128D(key, data) xor data.
pub fn aes_g(key: &[u8; 16], data: &[u8; 16]) -> [u8; 16] {
    let mut r = aes_d(key, data);
    for (a, b) in r.iter_mut().zip(data) {
        *a ^= b;
    }
    r
}

pub fn sha1(data: &[u8]) -> [u8; 20] {
    let mut d = [0u8; 20];
    unsafe {
        let s = avcrypto::av_sha_alloc();
        assert_eq!(avcrypto::av_sha_init(s, 160), 0);
        avcrypto::av_sha_update(s, data.as_ptr(), data.len());
        avcrypto::av_sha_final(s, d.as_mut_ptr());
        ffmpeg_sys::av_free(s);
    }
    d
}

pub fn hex(b: &[u8]) -> String {
    b.iter().fold(String::new(), |mut s, x| {
        let _ = write!(s, "{x:02X}");
        s
    })
}

/// A title key file: 64 title keys encrypted with vuk.
pub fn tkf(vuk: &[u8; 16], keys: &[[u8; 16]; 64]) -> Vec<u8> {
    let mut f = vec![0u8; 0x9b0];
    f[..12].copy_from_slice(b"DVD_HD_V_TKF");
    f[12..16].copy_from_slice(&0x9b0u32.to_be_bytes());
    for (j, k) in keys.iter().enumerate() {
        f[0x84 + 0x24 * j..0x94 + 0x24 * j].copy_from_slice(&aes_e(vuk, k));
    }
    f
}

pub fn title_keys() -> [[u8; 16]; 64] {
    std::array::from_fn(|j| std::array::from_fn(|i| u8::try_from((j * 16 + i) % 251).unwrap() ^ 0x5a))
}

fn pack_header() -> Vec<u8> {
    let mut s = vec![0u8; 2048];
    s[..4].copy_from_slice(&[0, 0, 1, 0xba]);
    s[4] = 0x44;
    s[0xd] = 0xf8; // no stuffing
    s
}

/// A navigation pack: key mode bits (top two bits of byte 0x3c), title key id
/// offset, 12 seed bytes.
pub fn nav_pack(mode_bits: u8, key_offset: u8, seed: &[u8; 12]) -> Vec<u8> {
    let mut s = pack_header();
    s[14..18].copy_from_slice(&[0, 0, 1, 0xbb]);
    s[18..20].copy_from_slice(&0x15u16.to_be_bytes()); // ends at 0x29
    s[0x29..0x30].copy_from_slice(&[0, 0, 1, 0xbf, 0x01, 0x01, 0x04]);
    s[0x130..0x137].copy_from_slice(&[0, 0, 1, 0xbf, 0x03, 0xd1, 0x00]);
    s[0x3c] = mode_bits << 6;
    s[0x3e] = key_offset;
    s[0x40..0x4c].copy_from_slice(seed);
    s[0x507..0x50e].copy_from_slice(&[0, 0, 1, 0xbf, 0x02, 0xf3, 0x01]);
    s
}

/// A pack with one PES packet of stream `sid` (scrambling bits `scr`), its
/// payload a counting pattern from `fill`.
pub fn data_pack(sid: u8, scr: u8, fill: u8) -> Vec<u8> {
    let mut s = pack_header();
    s[14..18].copy_from_slice(&[0, 0, 1, sid]);
    s[18..20].copy_from_slice(&u16::try_from(2048 - 20).unwrap().to_be_bytes());
    s[20] = 0x80 | (scr << 4);
    s[22] = 0;
    for (i, b) in s[23..].iter_mut().enumerate() {
        *b = fill.wrapping_add(u8::try_from(i % 256).unwrap());
    }
    s
}

/// Encrypt a pack as an HD DVD disc holds it (bytes 0x80.. AES-128-CBC with
/// AES-G(title key, bytes 0x54..0x57 + seed)).
pub fn encrypt_pack(plain: &[u8], key: &[u8; 16], seed: &[u8; 12]) -> Vec<u8> {
    let mut s16 = [0u8; 16];
    s16[..4].copy_from_slice(&plain[0x54..0x58]);
    s16[4..].copy_from_slice(seed);
    let bk = aes_g(key, &s16);
    let mut out = plain.to_vec();
    out[0x80..].copy_from_slice(&aes(&bk, &plain[0x80..], false, Some(&AACS_IV)));
    out
}

/// A disc opened as the demuxer opens it: VTI, playlists, marks, AACS, title plan.
pub struct Opened {
    src: *mut Source,
    pub fs: *mut Fs,
    vti: *mut Vti,
    xs: *mut *mut Xpl,
    nb: c_int,
    pub aacs: *mut Aacs,
    pub plan: *mut TitlePlan,
}

impl Drop for Opened {
    fn drop(&mut self) {
        unsafe {
            hddvd::ff_hddvd_titles_free(&raw mut self.plan);
            hddvd::ff_hddvd_aacs_close(&raw mut self.aacs);
            hddvd::ff_hddvd_xpl_free_all(&raw mut self.xs, self.nb);
            hddvd::ff_hddvd_vti_free(&raw mut self.vti);
            discio::ff_discio_fs_close(&raw mut self.fs);
            discio::ff_discio_source_free(&raw mut self.src);
        }
    }
}

fn open_source(src: *mut Source, key_files: &[String]) -> Result<Opened, c_int> {
    let mut o = Opened { src, fs: ptr::null_mut(), vti: ptr::null_mut(), xs: ptr::null_mut(), nb: 0, aacs: ptr::null_mut(), plan: ptr::null_mut() };
    let files: Vec<CString> = key_files.iter().map(|f| CString::new(f.as_str()).unwrap()).collect();
    let ptrs: Vec<*const std::os::raw::c_char> = files.iter().map(|f| f.as_ptr()).collect();
    unsafe {
        assert_eq!(discio::ff_discio_mount_image(o.src, ptr::null(), &raw mut o.fs), 0);
        assert_eq!(hddvd::ff_hddvd_vti_open(ptr::null_mut(), o.fs, &raw mut o.vti), 0);
        assert_eq!(hddvd::ff_hddvd_xpl_load(ptr::null_mut(), o.fs, &raw mut o.xs, &raw mut o.nb), 0);
        hddvd::ff_hddvd_evob_marks(ptr::null_mut(), o.vti, o.xs, o.nb);
        let ret = hddvd::ff_hddvd_aacs_open(ptr::null_mut(), o.fs, ptrs.as_ptr(), c_int::try_from(ptrs.len()).unwrap(), o.nb, &raw mut o.aacs);
        if ret < 0 {
            return Err(ret);
        }
        assert_eq!(hddvd::ff_hddvd_titles_plan(ptr::null_mut(), o.fs, o.vti, o.xs, o.nb, 0, &raw mut o.plan), 0);
    }
    Ok(o)
}

/// Open a built image.
pub fn open(im: &Img, key_files: &[String]) -> Result<Opened, c_int> {
    let opaque = Box::into_raw(Box::new(Mem(im.d.clone()))).cast::<c_void>();
    let size = i64::try_from(im.d.len()).unwrap();
    let src = unsafe { discio::ff_discio_source_new(ptr::null_mut(), c"memory".as_ptr(), size, &raw const MEM_OPS, opaque) };
    open_source(src, key_files)
}

/// Open an image file.
pub fn open_file(path: &std::path::Path, key_files: &[String]) -> Result<Opened, c_int> {
    let p = CString::new(path.to_str().unwrap()).unwrap();
    let mut src = ptr::null_mut();
    assert_eq!(unsafe { discio::ff_discio_source_open_file(ptr::null_mut(), p.as_ptr(), &raw mut src) }, 0);
    open_source(src, key_files)
}

impl Opened {
    /// Build every title's tracks (titles that cannot be used leave the
    /// plan), then the plan as text: Ok(dump) or Err(code).
    pub fn tracks(&self) -> Result<String, c_int> {
        let ret = unsafe { hddvd::ff_hddvd_tracks_build(ptr::null_mut(), self.fs, self.aacs, self.vti, self.xs, self.nb, self.plan) };
        if ret < 0 {
            return Err(ret);
        }
        let d = unsafe { hddvd::ff_hddvd_titles_dump(self.plan) };
        let s = unsafe { CStr::from_ptr(d) }.to_str().unwrap().to_owned();
        unsafe { ffmpeg_sys::av_free(d.cast()) };
        Ok(s)
    }

    /// Block of EVOB slot's stream, made usable: (result, bytes).
    pub fn block(&self, slot: c_int, block: u32) -> (c_int, Vec<u8>) {
        let mut buf = vec![0u8; 2048];
        let clip = unsafe { hddvd::ff_hddvd_titles_clip(self.plan, slot) };
        assert!(!clip.is_null(), "no clip for slot {slot}");
        let r = unsafe { hddvd::ff_hddvd_clip_block(ptr::null_mut(), self.aacs, self.fs, clip, block, buf.as_mut_ptr()) };
        (r, buf)
    }
}
