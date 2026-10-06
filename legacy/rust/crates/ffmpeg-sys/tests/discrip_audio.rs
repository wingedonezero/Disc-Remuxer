//! `libavformat/discrip_audio.c` with the AC-3 / E-AC-3 rules of
//! `discrip_codecs.c`: units of one segment get their duration (samples /
//! rate of their own first syncframe, ATSC A/52), the sync-unit flag and
//! their final bytes (an AC-3 track keeps only AC-3 syncframes, an E-AC-3
//! track only E-AC-3 ones; dependent frames and further substreams stay in
//! the unit of their independent frame and are reported for review).

mod common;

use common::discrip::{dur, run, timed};
use ffmpeg_sys::discrip::{AudioHeader, F_KEY, F_MARKER, F_SYNC};
use ffmpeg_sys::AV_NOPTS_VALUE;

/// AC-3 syncframe: 48 kHz, 192 kbit/s (768 bytes), bsid 8, 2/0; `tag` marks
/// the payload so frames can be told apart.
fn ac3(tag: u8) -> Vec<u8> {
    let mut f = vec![tag; 768];
    f[..7].copy_from_slice(&[0x0B, 0x77, 0x00, 0x00, 0x14, 0x40, 0x40]);
    f
}

/// E-AC-3 syncframe (bsid 16, 48 kHz, 2/0): stream type (0 independent,
/// 1 dependent), substream id, numblkscod (0..3 = 1, 2, 3, 6 blocks), size.
fn eac3(strmtyp: u8, sub: u8, numblkscod: u8, size: usize, tag: u8) -> Vec<u8> {
    let mut f = vec![tag; size];
    let w = (u16::from(strmtyp) << 14) | (u16::from(sub) << 11) | u16::try_from(size / 2 - 1).unwrap();
    f[0] = 0x0B;
    f[1] = 0x77;
    f[2..4].copy_from_slice(&w.to_be_bytes());
    f[4] = (numblkscod << 4) | (2 << 1);
    f[5] = 16 << 3;
    f[6] = 0;
    f[7] = 0;
    f
}

#[test]
fn ac3_units_take_1536_samples_and_are_sync_units() {
    let frames: Vec<Vec<u8>> = (0..10).map(ac3).collect();
    let r = run("ac3", &timed(&frames), None);
    assert_eq!(r.outs.len(), 10);
    for (k, o) in r.outs.iter().enumerate() {
        assert_eq!(o.bytes, frames[k]);
        assert_eq!(o.time, i64::try_from(k).unwrap() * 1000);
        assert_eq!(o.dur, 34_560_000, "1536 samples at 48 kHz");
        assert_eq!(o.flags, F_KEY | F_SYNC);
    }
    assert_eq!(r.audio.header, AudioHeader { rate: 48000, samples: 1536 });
    assert_eq!(r.audio.review, 0);
}

#[test]
fn eac3_one_block_frames_take_256_samples() {
    let frames: Vec<Vec<u8>> = (0..12).map(|k| eac3(0, 0, 0, 1024, k)).collect();
    let r = run("eac3", &timed(&frames), None);
    assert_eq!(r.outs.len(), 12);
    assert!(r.outs.iter().all(|o| o.dur == dur(256, 48000) && o.flags == F_KEY | F_SYNC));
    assert_eq!(r.audio.header, AudioHeader { rate: 48000, samples: 256 });
    assert_eq!(r.audio.review, 0);
}

#[test]
fn eac3_dependent_frames_join_their_independent_frame_and_are_reviewed() {
    // independent 5.1-style frame + dependent frame (extra channels), 8 times;
    // each pair in its own payload, the payload time at the independent frame
    let mut payloads_data = Vec::new();
    for k in 0..8u8 {
        let mut p = eac3(0, 0, 3, 768, k);
        p.extend(eac3(1, 0, 3, 512, 0x80 | k));
        payloads_data.push(p);
    }
    let r = run("eac3", &timed(&payloads_data), None);
    assert_eq!(r.outs.len(), 8, "one unit per independent frame");
    for (k, o) in r.outs.iter().enumerate() {
        assert_eq!(o.bytes, payloads_data[k], "independent + dependent bytes kept together");
        assert_eq!(o.dur, dur(1536, 48000), "one duration for the pair");
        assert_eq!(o.time, i64::try_from(k).unwrap() * 1000);
    }
    assert!(r.audio.review > 0);
}

#[test]
fn eac3_second_program_joins_substream_0_and_is_reviewed() {
    let mut payloads_data = Vec::new();
    for k in 0..6u8 {
        let mut p = eac3(0, 0, 3, 768, k);
        p.extend(eac3(0, 1, 3, 512, 0x80 | k));
        payloads_data.push(p);
    }
    let r = run("eac3", &timed(&payloads_data), None);
    assert_eq!(r.outs.len(), 6);
    assert!(r.outs.iter().all(|o| o.bytes.len() == 768 + 512 && o.dur == dur(1536, 48000)));
    assert!(r.audio.review > 0);
}

#[test]
fn ac3_track_keeps_only_its_ac3_syncframes() {
    // AC-3 core + E-AC-3 dependent frame (core cut), and AC-3 + E-AC-3
    // independent frame (the E-AC-3 frame is not a unit of an AC-3 track)
    for strmtyp in [1u8, 0] {
        let mut stream = Vec::new();
        for k in 0..6u8 {
            stream.extend(ac3(k));
            stream.extend(eac3(strmtyp, 0, 3, 512, 0x80 | k));
        }
        let r = run("ac3", &[(stream.as_slice(), Some(0))], None);
        assert_eq!(r.outs.len(), 6, "stream type {strmtyp}");
        for (k, o) in r.outs.iter().enumerate() {
            assert_eq!(o.bytes, ac3(u8::try_from(k).unwrap()), "stream type {strmtyp}");
            assert_eq!(o.dur, 34_560_000);
        }
        assert_eq!(r.audio.cut_bytes + r.cutter.skipped_bytes, 6 * 512, "stream type {strmtyp}");
        assert_eq!(r.audio.review, 0);
    }
}

#[test]
fn eac3_track_leaves_out_ac3_syncframes() {
    let mut stream = Vec::new();
    for k in 0..6u8 {
        stream.extend(ac3(k));
        stream.extend(eac3(0, 0, 3, 512, 0x80 | k));
    }
    let r = run("eac3", &[(stream.as_slice(), Some(0))], None);
    assert_eq!(r.outs.len(), 6);
    assert!(r.outs.iter().all(|o| o.bytes.len() == 512 && o.bytes[2] & 0xC0 == 0));
    assert_eq!(r.cutter.skipped, 6);
    // the payload's time goes to the first UNIT starting in it: the AC-3 frame
    // before it is not a unit of this track
    assert_eq!(r.outs[0].time, 0);
    assert!(r.outs[1..].iter().all(|o| o.time == AV_NOPTS_VALUE));
}

#[test]
fn a_frame_with_another_length_is_timed_by_its_own_header_and_reviewed() {
    let mut frames: Vec<Vec<u8>> = (0..4).map(|k| eac3(0, 0, 3, 768, k)).collect();
    frames.extend((4..8).map(|k| eac3(0, 0, 2, 512, k)));
    let r = run("eac3", &timed(&frames), None);
    let durs: Vec<i64> = r.outs.iter().map(|o| o.dur).collect();
    assert_eq!(durs[..4], [dur(1536, 48000); 4]);
    assert_eq!(durs[4..], [dur(768, 48000); 4]);
    assert_eq!(r.audio.header.samples, 1536, "the stream's values stay those of its first frame");
    assert_eq!(r.audio.review, 4);
}

#[test]
fn a_marker_is_an_empty_frame_that_keeps_its_time() {
    let frames: Vec<Vec<u8>> = (0..2).map(ac3).collect();
    let r = run("ac3", &timed(&frames), Some(777));
    let m = r.outs.last().unwrap();
    assert_eq!((m.bytes.len(), m.time, m.dur, m.flags), (0, 777, 0, F_KEY | F_MARKER));
    assert_eq!(r.audio.markers, 1);
}
