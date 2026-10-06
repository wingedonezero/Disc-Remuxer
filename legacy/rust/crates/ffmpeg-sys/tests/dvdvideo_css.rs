//! `libavformat/dvdvideo_css.c`: which sectors are CSS-scrambled, and the content
//! check that confirms a title key (a descrambled sector holds well-formed AC-3
//! or MPEG-2 video), on sectors built here.

use std::os::raw::c_int;

const S: usize = 2048;

extern "C" {
    fn ff_dvdvideo_css_scrambled_pes(sec: *const u8) -> c_int;
    fn ff_dvdvideo_css_content_valid(sec: *const u8) -> c_int;
    fn ff_dvdvideo_css_can_test(sec: *const u8) -> c_int;
}

// Links our FFmpeg.
use ffmpeg_sys as _;

fn scrambled_pes(sec: &[u8]) -> c_int {
    assert_eq!(sec.len(), S);
    // SAFETY: a full sector.
    unsafe { ff_dvdvideo_css_scrambled_pes(sec.as_ptr()) }
}
fn valid(sec: &[u8]) -> bool {
    assert_eq!(sec.len(), S);
    // SAFETY: a full sector.
    unsafe { ff_dvdvideo_css_content_valid(sec.as_ptr()) != 0 }
}
fn testable(sec: &[u8]) -> bool {
    assert_eq!(sec.len(), S);
    // SAFETY: a full sector.
    unsafe { ff_dvdvideo_css_can_test(sec.as_ptr()) != 0 }
}

/// An MPEG-2 pack header with `stuffing` stuffing bytes, the first of them
/// `first_stuffing` (the rest 0xFF).
fn pack_header(stuffing: u8, first_stuffing: u8) -> Vec<u8> {
    let mut s = vec![0, 0, 1, 0xBA, 0x44, 0, 4, 0, 4, 1, 0x01, 0x89, 0xc3, 0xf8 | stuffing];
    if stuffing > 0 {
        s.push(first_stuffing);
        s.extend(std::iter::repeat_n(0xff, usize::from(stuffing) - 1));
    }
    s
}

/// A sector with a pack header and one PES packet of stream `id` whose flag
/// byte (byte 6 of the PES) is `flags`.
fn pack(id: u8, flags: u8, stuffing: u8, first_stuffing: u8) -> Vec<u8> {
    let mut s = pack_header(stuffing, first_stuffing);
    s.extend_from_slice(&[0, 0, 1, id]);
    let len = u16::try_from(S - s.len() - 6).unwrap();
    s.extend_from_slice(&len.to_be_bytes());
    s.extend_from_slice(&[flags, 0, 0]);
    s.resize(S, 0x55);
    s
}

// ---- which sectors are scrambled ----

#[test]
fn usable_packs() {
    for id in [0xBD, 0xE0, 0xC0, 0xC7, 0xBF, 0xBE, 0xE1, 0xC8] {
        assert_eq!(scrambled_pes(&pack(id, 0x80, 0, 0)), 0, "{id:#x}");
    }
    // scrambling bits on a stream that is not checked
    assert_eq!(scrambled_pes(&pack(0xE1, 0xb0, 0, 0)), 0);
    assert_eq!(scrambled_pes(&pack(0xBF, 0xb0, 0, 0)), 0);
}

#[test]
fn scrambled_packs() {
    assert_eq!(scrambled_pes(&pack(0xBD, 0x90, 0, 0)), 14);
    assert_eq!(scrambled_pes(&pack(0xE0, 0xa0, 0, 0)), 14);
    assert_eq!(scrambled_pes(&pack(0xC3, 0xb0, 3, 0xff)), 17);
}

#[test]
fn stuffing_counts_only_when_its_first_byte_is_not_zero() {
    // stuffing length 2 with a first stuffing byte of 0: the PES must be at 14
    // (there it finds the stuffing bytes, not a start code)
    assert_eq!(scrambled_pes(&pack(0xE0, 0xb0, 2, 0)), -1);
    assert_eq!(scrambled_pes(&pack(0xE0, 0xb0, 2, 0xff)), 16);
}

#[test]
fn sectors_that_are_not_usable_packs() {
    assert_eq!(scrambled_pes(&[0x5A; S]), -1);
    // an MPEG-1 pack header (marker bits 0010)
    let mut m = pack(0xE0, 0xb0, 0, 0);
    m[4] = 0x21;
    assert_eq!(scrambled_pes(&m), -1);
    // a pack header without a PES start code after it
    let mut m = pack(0xE0, 0xb0, 0, 0);
    m[16] = 7;
    assert_eq!(scrambled_pes(&m), -1);
}

// ---- the content check: AC-3 ----

/// CRC-16 of A/52 (polynomial 0x8005, MSB first, initial 0), bit by bit.
fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for &b in data {
        for i in (0..8).rev() {
            let bit = (b >> i) & 1 != 0;
            let top = crc & 0x8000 != 0;
            crc <<= 1;
            if top != bit {
                crc ^= 0x8005;
            }
        }
    }
    crc
}

/// A sector with an AC-3 packet: two 768-byte frames (48 kHz, 192 kb/s, bsid
/// 8, acmod 2), the first starting at payload byte `fau`.
fn ac3_sector(fau: usize) -> Vec<u8> {
    let mut frame = vec![0u8; 768];
    frame[..2].copy_from_slice(&[0x0b, 0x77]);
    frame[4] = 0x14;
    frame[5] = 8 << 3;
    frame[6] = 0b0100_0000; // acmod 2 (bits 7-5), dsurmod 0
    frame[7] = 0x40;
    for (i, b) in frame.iter_mut().enumerate().skip(8) {
        *b = u8::try_from(i * 7 % 251).unwrap();
    }
    // each CRC region (bytes 2..480 and 480..768) checks to 0 with its CRC at its end
    let c1 = crc16(&frame[2..478]);
    frame[478..480].copy_from_slice(&c1.to_be_bytes());
    let c2 = crc16(&frame[480..766]);
    frame[766..768].copy_from_slice(&c2.to_be_bytes());
    let mut s = pack_header(0, 0);
    let mut payload = vec![0x80, 2, 0, u8::try_from(fau + 1).unwrap()];
    payload.resize(4 + fau, 0xAA);
    payload.extend_from_slice(&frame);
    payload.extend_from_slice(&frame[..8]);
    let len = u16::try_from(3 + payload.len()).unwrap();
    s.extend_from_slice(&[0, 0, 1, 0xBD]);
    s.extend_from_slice(&len.to_be_bytes());
    s.extend_from_slice(&[0x81, 0, 0]);
    s.extend_from_slice(&payload);
    s.resize(S, 0xff);
    s
}

#[test]
fn ac3_content() {
    let s = ac3_sector(10);
    assert!(testable(&s));
    assert!(valid(&s));
    // a changed byte in the scrambled part breaks a CRC
    let mut t = s.clone();
    t[600] ^= 0x10;
    assert!(testable(&t), "the header is unchanged");
    assert!(!valid(&t));
    // the next frame's header must match
    let mut t = s.clone();
    let next = 14 + 9 + 4 + 10 + 768;
    t[next] = 0;
    assert!(!valid(&t));
    // a frame header past the clear bytes cannot be tested
    let late = ac3_sector(100);
    assert!(!testable(&late));
    assert!(!valid(&late));
}

#[test]
fn ac3_needs_bsid_8_or_6_and_a_known_frame_size() {
    let mut s = ac3_sector(10);
    s[14 + 9 + 4 + 10 + 5] = 9 << 3;
    assert!(!testable(&s), "bsid 9");
    let mut s = ac3_sector(10);
    s[14 + 9 + 4 + 10 + 4] = 0xd4;
    assert!(!testable(&s), "fscod 3 (reserved)");
}

// ---- the content check: MPEG-2 video ----

/// A sector whose video payload starts with a sequence header loading both
/// quantiser matrices (the non-intra matrix at offset `v` = 99, its column
/// values `col`), a sequence extension and another start code.
fn video_sector(col: [u8; 8]) -> Vec<u8> {
    let mut s = pack_header(0, 0);
    s.extend_from_slice(&[0, 0, 1, 0xE0]);
    let len = u16::try_from(S - s.len() - 6).unwrap();
    s.extend_from_slice(&len.to_be_bytes());
    s.extend_from_slice(&[0x81, 0, 0]); // no PES header data: the payload starts at 23
    s.resize(S, 0);
    s[23..27].copy_from_slice(&[0, 0, 1, 0xB3]);
    s[27..34].copy_from_slice(&[0x2d, 0x01, 0xe0, 0x24, 0xff, 0xff, 0xe0]);
    s[34] = 0x02; // load_intra_quantiser_matrix
    s[98] = 0x01; // load_non_intra_quantiser_matrix (its last bit)
    let v = 99;
    for k in 0..64 {
        s[v + k] = col[k / 8] + u8::try_from(k % 8).unwrap();
    }
    for (k, c) in col.iter().enumerate() {
        s[v + 8 * k] = *c;
    }
    s[v + 0x40..v + 0x45].copy_from_slice(&[0, 0, 1, 0xB5, 0x14]); // sequence extension
    s[v + 0x45..v + 0x4a].copy_from_slice(&[0x8a, 0, 1, 0, 0x55]);
    s[v + 0x4a..v + 0x4d].copy_from_slice(&[0, 0, 1]); // the next start code
    s
}

const RISING: [u8; 8] = [16, 17, 18, 19, 20, 21, 22, 23];

#[test]
fn video_content() {
    let s = video_sector(RISING);
    assert!(testable(&s));
    assert!(valid(&s));
    // a matrix column that falls
    let mut c = RISING;
    c[4] = 10;
    assert!(testable(&video_sector(c)), "the matrix lies in the scrambled part");
    assert!(!valid(&video_sector(c)));
    // the first step may fall a little, not much
    assert!(valid(&video_sector([20, 17, 18, 19, 20, 21, 22, 23])));
    assert!(!valid(&video_sector([40, 17, 18, 19, 20, 21, 22, 23])));
    // no sequence extension
    let mut s = video_sector(RISING);
    s[99 + 0x43] = 0xB8;
    assert!(!valid(&s));
    // only one matrix loaded: nothing to test
    let mut s = video_sector(RISING);
    s[98] = 0;
    assert!(!testable(&s));
}

#[test]
fn video_probe_with_a_zero_run_to_the_sector_end() {
    // video payload: zeros up to a 01 in the last byte (no sequence header)
    let mut s = pack_header(0, 0);
    s.extend_from_slice(&[0, 0, 1, 0xE0]);
    s.extend_from_slice(&2028u16.to_be_bytes());
    s.extend_from_slice(&[0x80, 0, 0]);
    s.resize(S, 0);
    s[S - 1] = 1;
    assert!(!testable(&s));
    assert!(!valid(&s));
}
