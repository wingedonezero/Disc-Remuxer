//! `libavformat/hddvd_vti.c`: the HD DVD Advanced VTS information file
//! (`HVDVD_TS/HVA00001.VTI`), built byte by byte; with `HDDVD_CORPUS` (a folder
//! of HD DVD images) every image's VTI is read through the disc readers and
//! each EVOB record's size is compared with its EVO file.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;

use ffmpeg_sys::discio;
use ffmpeg_sys::hddvd::{self, Vti};

const INVALIDDATA: c_int = -0x4144_4E49; // AVERROR_INVALIDDATA
const EINVAL: c_int = -22;

// ---- building a VTI ----

struct Attr {
    audio: u16,
    subpic: u16,
    word0: u32,
}

struct Rec {
    name: Vec<u8>,
    attr: u32,
    start: u32,
    end: u32,
    sectors: u32,
    slot: u16,
}

fn rec(name: &str, attr: u32, slot: u16) -> Rec {
    Rec { name: name.as_bytes().to_vec(), attr, start: 1000, end: 91000, sectors: 77, slot }
}

fn attr(audio: u16, subpic: u16) -> Attr {
    Attr { audio, subpic, word0: 0x0102_0304 }
}

/// Header in sector 0, attribute table in sector 1, EVOB table after it.
fn vti(attrs: &[Attr], attr_count: u16, recs: &[Rec], rec_count: u32) -> Vec<u8> {
    let mut a = vec![0u8; 8 + 4 * attrs.len()];
    a[0..2].copy_from_slice(&attr_count.to_be_bytes());
    for (i, at) in attrs.iter().enumerate() {
        let off = u32::try_from(a.len()).unwrap();
        a[8 + 4 * i..12 + 4 * i].copy_from_slice(&off.to_be_bytes());
        let mut r = vec![0u8; 0x206];
        r[0] = i.to_le_bytes()[0]; // marks the record
        r[0x0e..0x10].copy_from_slice(&at.audio.to_be_bytes());
        r[0xe4..0xe6].copy_from_slice(&at.subpic.to_be_bytes());
        r[0x186..0x18a].copy_from_slice(&at.word0.to_be_bytes());
        a.extend(r);
    }
    let mut b = vec![0u8; 8 + 4 * recs.len()];
    b[0..4].copy_from_slice(&rec_count.to_be_bytes());
    for (i, e) in recs.iter().enumerate() {
        let off = u32::try_from(b.len()).unwrap();
        b[8 + 4 * i..12 + 4 * i].copy_from_slice(&off.to_be_bytes());
        let mut r = vec![0u8; 0x140];
        r[2..2 + e.name.len()].copy_from_slice(&e.name);
        r[0x106..0x10a].copy_from_slice(&e.attr.to_be_bytes());
        r[0x10a..0x10e].copy_from_slice(&e.start.to_be_bytes());
        r[0x10e..0x112].copy_from_slice(&e.end.to_be_bytes());
        r[0x112..0x116].copy_from_slice(&e.sectors.to_be_bytes());
        r[0x116..0x118].copy_from_slice(&e.slot.to_be_bytes());
        b.extend(r);
    }
    let a_sector = 1u32;
    let b_sector = a_sector + u32::try_from(a.len().div_ceil(2048)).unwrap();
    let mut f = vec![0u8; 2048];
    f[..12].copy_from_slice(b"ADVANCED-VTS");
    f[0xb8..0xbc].copy_from_slice(&a_sector.to_be_bytes());
    f[0xbc..0xc0].copy_from_slice(&b_sector.to_be_bytes());
    a.resize(a.len().div_ceil(2048) * 2048, 0);
    f.extend(a);
    f.extend(b);
    f
}

fn good() -> Vec<u8> {
    vti(
        &[attr(1, 0), attr(5, 4)],
        2,
        &[rec("FEATURE_2.EVO", 2, 3), rec("BLACK.EVO", 1, 1), rec("FEATURE_1.EVO", 2, 2)],
        3,
    )
}

// ---- parsing ----

unsafe extern "C" fn mem_read(opaque: *mut c_void, pos: i64, buf: *mut u8, len: c_int) -> c_int {
    // SAFETY: opaque is the Vec the caller keeps alive; buf has len bytes.
    let data = unsafe { &*opaque.cast::<Vec<u8>>() };
    let (p, n) = (usize::try_from(pos).unwrap(), usize::try_from(len).unwrap());
    if p + n > data.len() {
        return EINVAL; // as ff_discio_file_read past the end of a file
    }
    unsafe { ptr::copy_nonoverlapping(data[p..].as_ptr(), buf, n) };
    0
}

struct Parsed(*mut Vti);

impl Drop for Parsed {
    fn drop(&mut self) {
        unsafe { hddvd::ff_hddvd_vti_free(&raw mut self.0) };
    }
}

impl Parsed {
    fn vti(&self) -> &Vti {
        unsafe { &*self.0 }
    }
    /// (slot, name, base, attribute, start, end, sectors) of every slot in use.
    fn evobs(&self) -> Vec<(i32, String, String, i32, u32, u32, u32)> {
        let v = self.vti();
        v.evobs
            .iter()
            .filter(|e| !e.is_null())
            .map(|&e| {
                let e = unsafe { &*e };
                let s = |c: &[c_char; 256]| unsafe { CStr::from_ptr(c.as_ptr()) }.to_string_lossy().into_owned();
                (e.slot, s(&e.name), s(&e.base), e.attr, e.start_ptm, e.end_ptm, e.sectors)
            })
            .collect()
    }
}

fn parse(mut data: Vec<u8>) -> Result<Parsed, c_int> {
    install_log();
    let mut out = ptr::null_mut();
    let ret = unsafe {
        hddvd::ff_hddvd_vti_parse(ptr::null_mut(), mem_read, ptr::from_mut(&mut data).cast(), &raw mut out)
    };
    if ret < 0 {
        assert!(out.is_null());
        Err(ret)
    } else {
        Ok(Parsed(out))
    }
}

static LOG: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());

unsafe extern "C" fn log_sink(_level: c_int, line: *const c_char) {
    // SAFETY: the glue passes a NUL-terminated line.
    let text = unsafe { CStr::from_ptr(line) }.to_string_lossy().into_owned();
    LOG.lock().unwrap().push(text);
}

fn install_log() {
    // SAFETY: log_sink is a valid callback for the whole test run.
    unsafe { ffmpeg_sys::dr_log_install(log_sink, ffmpeg_sys::log_level::DEBUG) };
}

fn logged(part: &str) -> bool {
    LOG.lock().unwrap().iter().any(|l| l.contains(part))
}

// ---- tests ----

#[test]
fn a_valid_file_gives_its_records_by_slot() {
    let p = parse(good()).unwrap();
    let v = p.vti();
    assert_eq!((v.nb_attrs, v.nb_evobs), (2, 3));
    let a = unsafe { std::slice::from_raw_parts(v.attrs, 2) };
    assert_eq!((a[0].nb_audio, a[0].nb_subpic, a[0].raw[0]), (1, 0, 0));
    assert_eq!((a[1].nb_audio, a[1].nb_subpic, a[1].raw[0]), (5, 4, 1));
    assert_eq!(a[1].words[0], 0x0102_0304);
    let s = |x: &str| x.to_string();
    assert_eq!(
        p.evobs(),
        [
            (1, s("BLACK.EVO"), s("BLACK"), 1, 1000, 91000, 77),
            (2, s("FEATURE_1.EVO"), s("FEATURE_1"), 2, 1000, 91000, 77),
            (3, s("FEATURE_2.EVO"), s("FEATURE_2"), 2, 1000, 91000, 77),
        ]
    );
    assert!(logged("VTI check EVOB record 3 at +0x"));
}

#[test]
fn the_identifier_must_be_advanced_vts() {
    let mut f = good();
    f[..12].copy_from_slice(b"STANDARD-VTS");
    assert_eq!(parse(f).err(), Some(INVALIDDATA));
    assert!(logged("identifier \"STANDARD-VTS\" (\"ADVANCED-VTS\"): FAIL"));
}

#[test]
fn the_attribute_table_holds_1_to_511_records() {
    let recs = [rec("A.EVO", 1, 1)];
    // no record at all is an error too, not only too many
    assert_eq!(parse(vti(&[], 0, &[], 0)).err(), Some(INVALIDDATA));
    assert!(logged("attribute record count 0 (1..511): FAIL"));
    let many: Vec<Attr> = (0..511).map(|_| attr(0, 0)).collect();
    assert!(parse(vti(&many, 511, &recs, 1)).is_ok());
    assert_eq!(parse(vti(&many, 512, &recs, 1)).err(), Some(INVALIDDATA));
    assert!(logged("attribute record count 512 (1..511): FAIL"));
}

#[test]
fn an_attribute_record_has_up_to_8_audio_and_32_sub_picture_streams() {
    let recs = [rec("A.EVO", 1, 1)];
    assert!(parse(vti(&[attr(8, 32)], 1, &recs, 1)).is_ok());
    assert_eq!(parse(vti(&[attr(9, 0)], 1, &recs, 1)).err(), Some(INVALIDDATA));
    assert!(logged("attribute record 1: audio stream count 9 (0..8): FAIL"));
    assert_eq!(parse(vti(&[attr(0, 33)], 1, &recs, 1)).err(), Some(INVALIDDATA));
    assert!(logged("attribute record 1: sub-picture stream count 33 (0..32): FAIL"));
}

#[test]
fn the_evob_table_holds_0_to_1998_records() {
    assert_eq!(parse(vti(&[attr(0, 0)], 1, &[], 0)).unwrap().vti().nb_evobs, 0);
    let all: Vec<Rec> = (1..=1998).map(|i| rec(&format!("E{i}.EVO"), 1, i)).collect();
    assert_eq!(parse(vti(&[attr(0, 0)], 1, &all, 1998)).unwrap().vti().nb_evobs, 1998);
    assert_eq!(parse(vti(&[attr(0, 0)], 1, &all, 1999)).err(), Some(INVALIDDATA));
    assert!(logged("EVOB record count 1999 (0..1998): FAIL"));
}

#[test]
fn an_evob_record_names_an_existing_attribute_record() {
    let at = [attr(0, 0), attr(0, 0)];
    assert_eq!(parse(vti(&at, 2, &[rec("A.EVO", 0, 1)], 1)).err(), Some(INVALIDDATA));
    assert!(logged("EVOB record 1 (A.EVO): attribute record 0 (1..2): FAIL"));
    assert_eq!(parse(vti(&at, 2, &[rec("B.EVO", 3, 1)], 1)).err(), Some(INVALIDDATA));
    assert!(logged("EVOB record 1 (B.EVO): attribute record 3 (1..2): FAIL"));
}

#[test]
fn an_evob_record_has_its_own_slot_from_1_to_1998() {
    let at = [attr(0, 0)];
    assert_eq!(parse(vti(&at, 1, &[rec("A.EVO", 1, 0)], 1)).err(), Some(INVALIDDATA));
    assert!(logged("EVOB record 1 (A.EVO): slot 0 (1..1998): FAIL"));
    assert_eq!(parse(vti(&at, 1, &[rec("B.EVO", 1, 1999)], 1)).err(), Some(INVALIDDATA));
    assert!(logged("EVOB record 1 (B.EVO): slot 1999 (1..1998): FAIL"));
    let dup = [rec("C.EVO", 1, 7), rec("D.EVO", 1, 7)];
    assert_eq!(parse(vti(&at, 1, &dup, 2)).err(), Some(INVALIDDATA));
    assert!(logged("EVOB record 2 (D.EVO): slot 7 already taken by C.EVO: FAIL"));
}

#[test]
fn a_table_or_record_past_the_end_of_the_file_is_a_read_error() {
    let at = [attr(0, 0)];
    // the count says 2 records, the offset table and the records hold 1
    let mut f = vti(&at, 1, &[rec("A.EVO", 1, 1)], 2);
    let len = f.len();
    f.truncate(len - 0x140 + 0x100); // and the one record is cut short
    assert_eq!(parse(f).err(), Some(EINVAL));
    assert!(logged("VTI: cannot read an EVOB record (320 bytes at"));
    let mut f = good();
    f.truncate(0xbf);
    assert_eq!(parse(f).err(), Some(EINVAL));
    assert!(logged("VTI: cannot read the header (192 bytes at 0)"));
}

#[test]
fn the_name_is_at_most_255_bytes_and_the_base_name_drops_the_last_extension() {
    let mut long = vec![b'N'; 300];
    long[250..254].copy_from_slice(b".EVO");
    let mut r = rec("", 1, 1);
    r.name = long[..254].to_vec(); // fills the 255-byte field but one byte: NUL there
    let mut r2 = rec("A.B.EVO", 1, 2);
    r2.name.clone_from(&b"A.B.EVO".to_vec());
    let mut r3 = rec("", 1, 3);
    r3.name = vec![b'X'; 255]; // no NUL in the field: cut at 255
    let p = parse(vti(&[attr(0, 0)], 1, &[r, r2, r3], 3)).unwrap();
    let e = p.evobs();
    assert_eq!(e[0].1.len(), 254);
    assert_eq!(e[0].2.len(), 250);
    assert_eq!((e[1].1.as_str(), e[1].2.as_str()), ("A.B.EVO", "A.B"));
    assert_eq!((e[2].1.len(), e[2].2.len()), (255, 255));
}

// ---- corpus ----

#[test]
fn corpus_every_evob_record_matches_its_evo_file() {
    let Ok(dir) = std::env::var("HDDVD_CORPUS") else {
        eprintln!("HDDVD_CORPUS not set: corpus check skipped");
        return;
    };
    install_log();
    let mut images: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|x| x.eq_ignore_ascii_case("iso")))
        .collect();
    images.sort();
    assert!(!images.is_empty(), "no .iso in {dir}");
    for img in images {
        let path = CString::new(img.to_str().unwrap()).unwrap();
        let (mut src, mut fs, mut out) = (ptr::null_mut(), ptr::null_mut(), ptr::null_mut());
        unsafe {
            assert_eq!(discio::ff_discio_source_open_file(ptr::null_mut(), path.as_ptr(), &raw mut src), 0);
            assert_eq!(discio::ff_discio_mount_image(src, ptr::null(), &raw mut fs), 0);
            assert_eq!(hddvd::ff_hddvd_vti_open(ptr::null_mut(), fs, &raw mut out), 0, "{}", img.display());
        }
        let p = Parsed(out);
        let folder = unsafe { CStr::from_ptr(p.vti().folder.as_ptr()) }.to_str().unwrap().to_owned();
        let evobs = p.evobs();
        assert!(!evobs.is_empty());
        for (slot, name, _, _, start, end, sectors) in &evobs {
            let fpath = CString::new(format!("/{folder}/{name}")).unwrap();
            let mut file = ptr::null_mut();
            unsafe {
                let ops = &*(*fs).ops;
                assert_eq!((ops.open_file)(fs, fpath.as_ptr(), &raw mut file), 0, "{name}");
                assert_eq!((*file).size, i64::from(*sectors) * 2048, "{} slot {slot} {name}", img.display());
                discio::ff_discio_file_free(&raw mut file);
            }
            assert!(start <= end, "{name}");
        }
        eprintln!("{}: {} EVOB records match their files", img.display(), evobs.len());
        drop(p);
        unsafe {
            discio::ff_discio_fs_close(&raw mut fs);
            discio::ff_discio_source_free(&raw mut src);
        }
    }
}
