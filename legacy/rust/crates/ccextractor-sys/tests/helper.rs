//! The built `ccextractor` helper through [`ccextractor_sys::Captions`]:
//! CEA-608 caption blocks (millisecond time, `cc_data` triplets) in, SRT
//! cues out; and the SRT reader on its own.

use ccextractor_sys::{parse_srt, Captions, Cue};

/// A CEA-608 byte with odd parity.
fn parity(b: u8) -> u8 {
    if b.count_ones() % 2 == 0 {
        b | 0x80
    } else {
        b
    }
}

/// Triplets: each field-1 pair followed by a field-2 null pair.
fn triplets(pairs: &[(u8, u8)]) -> Vec<u8> {
    pairs.iter().flat_map(|&(a, b)| [0x04, parity(a), parity(b), 0x05, 0x80, 0x80]).collect()
}

/// Erase non-displayed memory, resume caption loading, row 15, the text.
fn load(text: &[u8]) -> Vec<(u8, u8)> {
    let mut v = vec![(0x14, 0x2E), (0x14, 0x2E), (0x14, 0x20), (0x14, 0x20), (0x14, 0x70), (0x14, 0x70)];
    let mut t = text.to_vec();
    if t.len() % 2 == 1 {
        t.push(0);
    }
    v.extend(t.chunks(2).map(|c| (c[0], c[1])));
    v
}

const EOC: [(u8, u8); 2] = [(0x14, 0x2F), (0x14, 0x2F)];
const EDM: [(u8, u8); 2] = [(0x14, 0x2C), (0x14, 0x2C)];

fn convert(blocks: &[(u64, Vec<(u8, u8)>)]) -> Vec<Cue> {
    let program = ccextractor_sys::program().expect("the build puts ccextractor next to the test");
    let mut c = Captions::start(&program).unwrap();
    for (ms, pairs) in blocks {
        c.push(*ms, &triplets(pairs)).unwrap();
    }
    c.finish().unwrap().cues
}

#[test]
fn pop_on_captions_become_srt_cues() {
    let cues = convert(&[
        (500, load(b"HELLO WORLD")),
        (1000, EOC.to_vec()),
        (3000, EDM.to_vec()),
        (4000, load(b"SECOND")),
        (4500, EOC.to_vec()),
        (6000, EDM.to_vec()),
    ]);
    // CCExtractor keeps 1 ms off both ends; lines are padded to 32 columns
    assert_eq!(
        cues,
        vec![
            Cue { start_ms: 1001, end_ms: 2999, text: "HELLO WORLD                     \n".into() },
            Cue { start_ms: 4501, end_ms: 5999, text: "SECOND                          \n".into() },
        ]
    );
}

#[test]
fn no_blocks_no_cues() {
    assert!(convert(&[]).is_empty());
}

#[test]
fn the_srt_reader_takes_cues_as_the_helper_writes_them() {
    let srt = "\u{feff}1\r\n00:00:03,604 --> 00:00:05,371\r\nLACROIX:\r\n<i> Do you remember?</i>\r\n\r\n2\r\n\
               01:02:03,004 --> 01:02:05,000\r\nX\r\n\r\n";
    assert_eq!(
        parse_srt(srt.as_bytes()).unwrap(),
        vec![
            Cue { start_ms: 3604, end_ms: 5371, text: "LACROIX:\n<i> Do you remember?</i>\n".into() },
            Cue { start_ms: 3_723_004, end_ms: 3_725_000, text: "X\n".into() },
        ]
    );
}

#[test]
fn the_srt_reader_refuses_what_the_helper_does_not_write() {
    assert!(parse_srt(b"2\r\n00:00:01,000 --> 00:00:02,000\r\nX\r\n").is_err(), "numbers start at 1");
    assert!(parse_srt(b"1\r\n00:00:01,000 -> 00:00:02,000\r\nX\r\n").is_err(), "timing line");
    assert!(parse_srt(b"1\r\n0:00:01,000 --> 00:00:02,000\r\nX\r\n").is_err(), "timing line length");
    assert!(parse_srt("1\r\n00:00:01,0\u{e9} --> 00:00:02,000\r\nX\r\n".as_bytes()).is_err(), "not ASCII");
}
