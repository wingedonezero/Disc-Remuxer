//! Our libdvdcss additions: `dvdcss_unscramble_sector` on built sectors, and
//! with `DVDCSS_SCRAMBLED_IMAGE=<image>:<first block of a title>[,<block>...]`
//! the title key and descrambler give the same sectors as libdvdcss's own
//! `dvdcss_read` with `DVDCSS_READ_DECRYPT`.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::os::raw::{c_int, c_void};

use libdvdcss_sys::{self as css, BLOCK_SIZE, KEY_SIZE};

/// A sector with an MPEG pack header and a PES header whose scrambling bits
/// (byte 0x14) are `bits`.
fn sector(bits: u8) -> Vec<u8> {
    let mut s: Vec<u8> = (0..BLOCK_SIZE).map(|i| u8::try_from(i % 251).unwrap()).collect();
    s[..4].copy_from_slice(&[0, 0, 1, 0xba]);
    s[14..18].copy_from_slice(&[0, 0, 1, 0xe0]);
    s[0x14] = 0x80 | bits;
    s
}

fn unscramble(key: &[u8; KEY_SIZE], s: &mut [u8]) -> c_int {
    // SAFETY: key has KEY_SIZE bytes, s has BLOCK_SIZE bytes.
    unsafe { css::dvdcss_unscramble_sector(key.as_ptr(), s.as_mut_ptr()) }
}

#[test]
fn a_clear_sector_is_left_as_it_is() {
    let mut s = sector(0);
    let before = s.clone();
    assert_eq!(unscramble(&[1, 2, 3, 4, 5], &mut s), 0);
    assert_eq!(s, before);
}

#[test]
fn a_scrambled_sector_without_a_key_is_left_as_it_is() {
    let mut s = sector(0x10);
    let before = s.clone();
    assert_eq!(unscramble(&[0; KEY_SIZE], &mut s), -1);
    assert_eq!(s, before);
}

#[test]
fn a_scrambled_sector_is_descrambled_from_byte_128_and_its_bits_cleared() {
    let mut s = sector(0x30);
    let before = s.clone();
    assert_eq!(unscramble(&[1, 2, 3, 4, 5], &mut s), 1);
    assert_eq!(s[0x14], 0x80, "scrambling bits cleared");
    assert_eq!(s[..0x14], before[..0x14]);
    assert_eq!(s[0x15..0x80], before[0x15..0x80], "the first 128 bytes are not scrambled");
    assert_ne!(s[0x80..], before[0x80..]);
}

// ---- the corpus check ----

unsafe extern "C" fn file_seek(p: *mut c_void, pos: u64) -> c_int {
    // SAFETY: p is the File of `open`.
    let f = unsafe { &mut *p.cast::<File>() };
    if f.seek(SeekFrom::Start(pos)).is_ok() { 0 } else { -1 }
}

unsafe extern "C" fn file_read(stream: *mut c_void, buf: *mut c_void, len: c_int) -> c_int {
    // SAFETY: stream is the File of `open`; buf has room for len bytes.
    let (file, out) = unsafe {
        (&mut *stream.cast::<File>(), std::slice::from_raw_parts_mut(buf.cast::<u8>(), usize::try_from(len).unwrap()))
    };
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

struct Handle {
    css: *mut css::Dvdcss,
    _file: Box<File>,
    _cb: Box<css::StreamCb>,
}

impl Handle {
    fn open(path: &str) -> Self {
        let mut file = Box::new(File::open(path).unwrap());
        let mut cb = Box::new(css::StreamCb { pf_seek: Some(file_seek), pf_read: Some(file_read), pf_readv: None });
        // SAFETY: file and cb live as long as the handle.
        let h = unsafe { css::dvdcss_open_stream_uncached((&raw mut *file).cast(), &raw mut *cb) };
        assert!(!h.is_null());
        Handle { css: h, _file: file, _cb: cb }
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        // SAFETY: css came from dvdcss_open_stream_uncached.
        unsafe { css::dvdcss_close(self.css) };
    }
}

const SECTORS: usize = 4096;

#[test]
fn corpus_title_key_and_descrambler_equal_dvdcss_read() {
    let Ok(spec) = std::env::var("DVDCSS_SCRAMBLED_IMAGE") else {
        eprintln!("DVDCSS_SCRAMBLED_IMAGE not set: corpus check skipped");
        return;
    };
    let (path, blocks) = spec.rsplit_once(':').unwrap();
    let mut scrambled_seen = 0;
    for block in blocks.split(',').map(|b| b.parse::<c_int>().unwrap()) {
        // libdvdcss's own path.
        let own = Handle::open(path);
        let mut want = vec![0u8; SECTORS * BLOCK_SIZE];
        // SAFETY: own.css is open; want has room for SECTORS blocks.
        unsafe {
            assert_eq!(css::dvdcss_seek(own.css, block, css::SEEK_KEY), block);
            let n = css::dvdcss_read(own.css, want.as_mut_ptr().cast(), c_int::try_from(SECTORS).unwrap(), css::READ_DECRYPT);
            want.truncate(usize::try_from(n).unwrap() * BLOCK_SIZE);
        }
        // Ours: the title key, then the raw sectors descrambled one by one.
        let ours = Handle::open(path);
        let mut key = [0u8; KEY_SIZE];
        // SAFETY: ours.css is open; key has KEY_SIZE bytes.
        let r = unsafe { css::dvdcss_title_key(ours.css, block, key.as_mut_ptr()) };
        assert_eq!(r, 1, "block {block}: title key result");
        let mut raw = vec![0u8; want.len()];
        let mut f = File::open(path).unwrap();
        f.seek(SeekFrom::Start(u64::try_from(block).unwrap() * BLOCK_SIZE as u64)).unwrap();
        f.read_exact(&mut raw).unwrap();
        for s in raw.chunks_mut(BLOCK_SIZE) {
            if unscramble(&key, s) == 1 {
                scrambled_seen += 1;
            }
        }
        assert!(raw == want, "block {block}: descrambled sectors differ from dvdcss_read");
        eprintln!("block {block}: key {key:02x?}, {} sectors equal", want.len() / BLOCK_SIZE);
    }
    assert!(scrambled_seen > 0, "no scrambled sector was seen");
    eprintln!("{scrambled_seen} scrambled sectors descrambled");
}
