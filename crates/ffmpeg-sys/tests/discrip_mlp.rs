//! `libavformat/discrip_mlp.c`: MLP / Dolby `TrueHD` access units (AUs) in the
//! rip core. An AU lasts the stream's samples per AU; only major-sync AUs are
//! sync units and key frames; an AU that ends the stream (marker D2 34 at the
//! end of its substreams) lasts the samples it decodes to. Streams made by
//! FFmpeg's encoders (tests/data/README.md).

mod common;

use common::discrip::{dur, run, Out};
use ffmpeg_sys::discrip::{AudioHeader, F_KEY, F_SYNC};
use ffmpeg_sys::AV_NOPTS_VALUE;

const THD: &[u8] = include_bytes!("data/sine_4813.thd");
const MLP: &[u8] = include_bytes!("data/sine_4813.mlp");

/// The AUs of a stream (length field of each AU header).
fn aus(stream: &[u8]) -> Vec<&[u8]> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < stream.len() {
        let len = ((usize::from(stream[i] & 0x0F) << 8) | usize::from(stream[i + 1])) * 2;
        out.push(&stream[i..i + len]);
        i += len;
    }
    out
}

fn is_major(au: &[u8]) -> bool {
    au[4..7] == [0xF8, 0x72, 0x6F]
}

/// The stream in 2016-byte payloads, each with a time.
fn payloads(stream: &[u8]) -> Vec<(&[u8], Option<i64>)> {
    stream.chunks(2016).enumerate().map(|(k, p)| (p, Some(i64::try_from(k).unwrap() * 1000 + 1))).collect()
}

fn check_units(outs: &[Out], aus: &[&[u8]]) {
    assert_eq!(outs.len(), aus.len());
    for (o, au) in outs.iter().zip(aus) {
        assert_eq!(o.bytes, *au);
        let sync = if is_major(au) { F_KEY | F_SYNC } else { 0 };
        assert_eq!(o.flags, sync, "only major-sync AUs are sync units and key frames");
    }
}

#[test]
fn truehd_aus_last_40_samples_and_the_stream_end_au_its_decoded_13() {
    let aus = aus(THD);
    assert_eq!(aus.len(), 121);
    let r = run("truehd", &payloads(THD), None);
    check_units(&r.outs, &aus);
    assert_eq!(r.audio.header, AudioHeader { rate: 48000, samples: 40 });
    for o in &r.outs[..120] {
        assert_eq!(o.dur, dur(40, 48000));
    }
    assert_eq!(r.outs[120].dur, dur(13, 48000), "4813 samples = 120 x 40 + 13");
    let total: i64 = r.outs.iter().map(|o| o.dur).sum();
    assert_eq!(total, 120 * dur(40, 48000) + dur(13, 48000));
    assert_eq!(r.outs[0].time, 1, "the first AU starts the first payload");
    assert_eq!(r.audio.review, 0);
}

#[test]
fn mlp_aus_without_a_stream_end_marker_all_last_40_samples() {
    let aus = aus(MLP);
    let r = run("mlp", &payloads(MLP), None);
    check_units(&r.outs, &aus);
    assert!(r.outs.iter().all(|o| o.dur == dur(40, 48000)));
}

#[test]
fn a_major_sync_au_with_a_bad_checksum_is_not_a_unit() {
    let aus = aus(THD);
    let second_major = aus.iter().enumerate().filter(|(_, a)| is_major(a)).nth(1).unwrap().0;
    let offset: usize = aus[..second_major].iter().map(|a| a.len()).sum();
    let mut stream = THD.to_vec();
    stream[offset + 12] ^= 0x01; // inside the major sync block, covered by its checksum
    let r = run("truehd", &[(stream.as_slice(), Some(5))], None);
    assert_eq!(r.cutter.skipped, 1, "the damaged AU is left out");
    assert_eq!(r.outs.len(), aus.len() - 1);
    assert!(r.outs.iter().all(|o| o.bytes != stream[offset..offset + aus[second_major].len()]));
    assert_eq!(r.outs[1].time, AV_NOPTS_VALUE);
}

#[test]
fn junk_between_aus_is_skipped_up_to_the_next_major_sync() {
    // 100 bytes of junk after AU 10: the AUs after it up to the next major
    // sync cannot be found by length any more and are skipped with the junk
    let aus = aus(THD);
    let at: usize = aus[..10].iter().map(|a| a.len()).sum();
    let mut stream = THD[..at].to_vec();
    stream.extend(std::iter::repeat_n(0x00u8, 100));
    stream.extend_from_slice(&THD[at..]);
    let next_major = (10..aus.len()).find(|&k| is_major(aus[k])).unwrap();
    let r = run("truehd", &[(stream.as_slice(), Some(5))], None);
    let lost: usize = aus[10..next_major].iter().map(|a| a.len()).sum();
    assert_eq!(r.cutter.skipped_bytes, i64::try_from(100 + lost).unwrap());
    assert_eq!(r.outs.len(), 10 + aus.len() - next_major);
    assert!(r.outs[10].bytes == aus[next_major]);
}

#[test]
fn a_cut_off_last_au_is_not_a_unit() {
    let aus = aus(THD);
    let stream = &THD[..THD.len() - 5];
    let r = run("truehd", &[(stream, Some(5))], None);
    assert_eq!(r.outs.len(), aus.len() - 1);
    assert_eq!(r.cutter.skipped_bytes, i64::try_from(aus[120].len() - 5).unwrap());
}
