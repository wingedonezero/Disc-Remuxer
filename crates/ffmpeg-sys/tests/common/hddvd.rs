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
    let mut a = vec![0u8; 12];
    a[0..2].copy_from_slice(&1u16.to_be_bytes());
    a[8..12].copy_from_slice(&12u32.to_be_bytes());
    a.extend(vec![0u8; 0x206]);
    let mut b = vec![0u8; 8 + 4 * evobs.len()];
    b[0..4].copy_from_slice(&u32::try_from(evobs.len()).unwrap().to_be_bytes());
    for (i, e) in evobs.iter().enumerate() {
        let off = u32::try_from(b.len()).unwrap();
        b[8 + 4 * i..12 + 4 * i].copy_from_slice(&off.to_be_bytes());
        let mut r = vec![0u8; 0x140];
        r[2..2 + e.name.len()].copy_from_slice(e.name.as_bytes());
        r[0x106..0x10a].copy_from_slice(&1u32.to_be_bytes());
        r[0x10a..0x10e].copy_from_slice(&e.start.to_be_bytes());
        r[0x10e..0x112].copy_from_slice(&e.end.to_be_bytes());
        r[0x112..0x116].copy_from_slice(&1u32.to_be_bytes());
        r[0x116..0x118].copy_from_slice(&e.slot.to_be_bytes());
        b.extend(r);
    }
    let mut f = vec![0u8; S];
    f[..12].copy_from_slice(b"ADVANCED-VTS");
    f[0xb8..0xbc].copy_from_slice(&1u32.to_be_bytes());
    f[0xbc..0xc0].copy_from_slice(&2u32.to_be_bytes());
    a.resize(S, 0);
    f.extend(a);
    f.extend(b);
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
