//! DTS in the rip core (`discrip_codecs.c`, ETSI TS 102 114): a core frame
//! lasts its own npcmblocks x 32 samples; a DTS-HD unit (core + extension
//! substream, kept together by FFmpeg's parser) is timed by its core; a
//! core-only track keeps only the core frame; every unit is a sync unit.
//! Core frames made by FFmpeg's DTS encoder (tests/data/README.md).

mod common;

use common::discrip::{dur, run, Out};
use ffmpeg_sys::discrip::{AudioHeader, F_KEY, F_SYNC};

const DTS: &[u8] = include_bytes!("data/sine_5120.dts");
const FRAME: usize = 1884;

/// A DTS-HD extension substream of `size` bytes (sync 64 58 20 25; header
/// size type 0: 8-bit header size, 16-bit frame size), filled with `tag`.
fn exss(size: usize, tag: u8) -> Vec<u8> {
    let mut x = vec![tag; size];
    x[..4].copy_from_slice(&[0x64, 0x58, 0x20, 0x25]);
    x[4] = 0; // user defined bits
    // nExtSSIndex (2) = 0, bHeaderSizeType (1) = 0, nuExtSSHeaderSize - 1 (8), nuExtSSFsize - 1 (16), 5 bits 0
    let bits: u64 = (15u64 << 21) | ((u64::try_from(size).unwrap() - 1) << 5);
    x[5..9].copy_from_slice(&u32::try_from(bits).unwrap().to_be_bytes());
    x
}

fn frames() -> Vec<&'static [u8]> {
    DTS.chunks(FRAME).collect()
}

#[test]
fn dts_core_frames_last_512_samples() {
    let payloads: Vec<(&[u8], Option<i64>)> = frames().into_iter().map(|f| (f, Some(9))).collect();
    let r = run("dts", &payloads, None);
    assert_eq!(r.outs.len(), 10);
    for (o, f) in r.outs.iter().zip(frames()) {
        assert_eq!(o.bytes, f);
        assert_eq!(o.dur, dur(512, 48000));
        assert_eq!(o.flags, F_KEY | F_SYNC);
    }
    assert_eq!(r.audio.header, AudioHeader { rate: 48000, samples: 512 });
    assert_eq!(r.audio.review, 0);
}

fn dtshd_stream() -> Vec<u8> {
    let mut s = Vec::new();
    for (k, f) in frames().into_iter().enumerate() {
        s.extend_from_slice(f);
        s.extend(exss(600, u8::try_from(k).unwrap()));
    }
    s
}

#[test]
fn a_dtshd_unit_keeps_core_and_extension_and_is_timed_by_the_core() {
    let s = dtshd_stream();
    let r = run("dts", &[(s.as_slice(), Some(1))], None);
    assert_eq!(r.outs.len(), 10);
    assert!(r.outs.iter().all(|o: &Out| o.bytes.len() == FRAME + 600 && o.dur == dur(512, 48000)));
    assert_eq!(r.audio.cut_bytes, 0);
}

#[test]
fn a_core_only_track_keeps_only_the_core_frames() {
    let s = dtshd_stream();
    let r = common::discrip::run_flags("dts", ffmpeg_sys::discrip::AUDIO_CORE_ONLY, &[(s.as_slice(), Some(1))], None);
    assert_eq!(r.outs.len(), 10);
    for (o, f) in r.outs.iter().zip(frames()) {
        assert_eq!(o.bytes, f);
    }
    assert_eq!(r.audio.cut_bytes, 10 * 600);
}
