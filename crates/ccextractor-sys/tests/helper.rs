//! The built `ccextractor` helper: `CEA-608` caption data in its raw caption
//! format ("bin", RCWT: header, then blocks of a millisecond time and
//! `cc_data` triplets) on stdin, SRT on stdout.

use std::io::Write;
use std::process::{Command, Stdio};

/// A `CEA-608` byte with odd parity.
fn parity(b: u8) -> u8 {
    if b.count_ones() % 2 == 0 {
        b | 0x80
    } else {
        b
    }
}

/// One block at `ms`: each field-1 pair followed by a field-2 null pair.
fn block(ms: u64, pairs: &[(u8, u8)]) -> Vec<u8> {
    let mut trip = Vec::new();
    for &(a, b) in pairs {
        trip.extend_from_slice(&[0x04, parity(a), parity(b), 0x05, 0x80, 0x80]);
    }
    let mut out = ms.to_le_bytes().to_vec();
    out.extend_from_slice(&u16::try_from(trip.len() / 3).unwrap().to_le_bytes());
    out.extend(trip);
    out
}

#[test]
fn a_pop_on_caption_becomes_one_srt_cue() {
    let program = ccextractor_sys::program().expect("the build puts ccextractor next to the test");
    let mut rcwt = vec![0xCC, 0xCC, 0xED, 0xCC, 0x00, 0x50, 0x00, 0x01, 0x00, 0x00, 0x00];
    // erase non-displayed memory, resume caption loading, row 15, the text
    let mut load = vec![(0x14, 0x2E), (0x14, 0x2E), (0x14, 0x20), (0x14, 0x20), (0x14, 0x70), (0x14, 0x70)];
    load.extend(b"HELLO WORLD\0".chunks(2).map(|c| (c[0], c[1])));
    rcwt.extend(block(500, &load));
    rcwt.extend(block(1000, &[(0x14, 0x2F), (0x14, 0x2F)])); // end of caption: shown
    rcwt.extend(block(3000, &[(0x14, 0x2C), (0x14, 0x2C)])); // erase displayed memory
    let mut child = Command::new(program)
        .args(["-in=bin", "-out=srt", "-stdin", "-stdout", "-utf8", "-nobi", "-ff"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    child.stdin.take().unwrap().write_all(&rcwt).unwrap();
    let out = child.wait_with_output().unwrap();
    assert!(out.status.success());
    let srt = String::from_utf8(out.stdout).unwrap();
    // `CCExtractor` keeps 1 ms off both ends; lines are padded to 32 columns
    assert_eq!(srt, "1\r\n00:00:01,001 --> 00:00:02,999\r\nHELLO WORLD                     \r\n\r\n");
}
