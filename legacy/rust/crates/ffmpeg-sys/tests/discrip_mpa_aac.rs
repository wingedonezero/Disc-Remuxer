//! MPEG audio and AAC in the rip core (`discrip_codecs.c`). MPEG audio frames
//! last their own samples, are never sync units, and any MPEG audio track is
//! reviewed (user rule). AAC ADTS frames last 1024 samples per raw block and
//! are sync units when they carry a channel configuration or a program config
//! element; frames with the MPEG-4 ID count the same and are reviewed. LATM takes
//! its values from FFmpeg's decoder on the frame that carries the
//! `StreamMuxConfig` and is reviewed. Streams made by FFmpeg's encoders.

mod common;

use common::discrip::{dur, run};
use ffmpeg_sys::discrip::{AudioHeader, F_KEY, F_SYNC};

const MP2: &[u8] = include_bytes!("data/sine_11520.mp2");
const ADTS: &[u8] = include_bytes!("data/sine_11520.aac");
const LATM: &[u8] = include_bytes!("data/sine_11520.latm");

/// ADTS frames (13-bit frame length of each header).
fn adts_frames(s: &[u8]) -> Vec<(usize, usize)> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < s.len() {
        let len = (usize::from(s[i + 3] & 3) << 11) | (usize::from(s[i + 4]) << 3) | usize::from(s[i + 5] >> 5);
        out.push((i, len));
        i += len;
    }
    out
}

#[test]
fn mp2_frames_last_1152_samples_are_not_sync_units_and_are_reviewed() {
    let r = run("mp2", &[(MP2, Some(3))], None);
    assert_eq!(r.outs.len(), 10);
    assert!(r.outs.iter().all(|o| o.bytes.len() == 576 && o.dur == dur(1152, 48000) && o.flags == F_KEY));
    assert_eq!(r.audio.header, AudioHeader { rate: 48000, samples: 1152 });
    assert!(r.audio.review > 0);
}

#[test]
fn adts_mpeg4_id_frames_are_kept_timed_sync_units_and_reviewed() {
    let frames = adts_frames(ADTS);
    let r = run("aac", &[(ADTS, Some(3))], None);
    assert_eq!(r.outs.len(), frames.len());
    assert!(r.outs.iter().all(|o| o.dur == dur(1024, 48000) && o.flags == F_KEY | F_SYNC));
    assert!(r.audio.review > 0);
}

#[test]
fn adts_mpeg2_id_frames_are_sync_units() {
    let mut s = ADTS.to_vec();
    for (at, _) in adts_frames(ADTS) {
        s[at + 1] |= 0x08; // ID = MPEG-2 (no CRC in these frames)
    }
    let r = run("aac", &[(s.as_slice(), Some(3))], None);
    assert!(r.outs.iter().all(|o| o.dur == dur(1024, 48000) && o.flags == F_KEY | F_SYNC));
    assert_eq!(r.audio.header, AudioHeader { rate: 48000, samples: 1024 });
    assert_eq!(r.audio.review, 0);
}

#[test]
fn latm_takes_its_values_from_the_frame_with_the_stream_mux_config() {
    let r = run("aac_latm", &[(LATM, Some(3))], None);
    assert_eq!(r.outs.len(), 13);
    assert_eq!(r.audio.header, AudioHeader { rate: 48000, samples: 1024 });
    assert!(r.outs.iter().all(|o| o.dur == dur(1024, 48000)));
    assert_eq!(r.outs[0].flags, F_KEY | F_SYNC);
    assert!(r.outs[1..].iter().all(|o| o.flags == F_KEY), "useSameStreamMux frames are not sync units");
    assert!(r.audio.review > 0);
}
