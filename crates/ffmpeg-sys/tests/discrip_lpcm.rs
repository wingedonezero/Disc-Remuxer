//! `libavformat/discrip_lpcm.c` and the LPCM rules of the rip core: the audio
//! frame header of DVD-Video (3 bytes) and HD DVD (5 bytes, read in the DVD
//! layout), the sample conversion to little-endian PCM, and units of one
//! frame through the cutter and the audio stage. The 16-bit stereo 48 kHz
//! case is the one the reference program was observed on (a byte swap of
//! each sample); the other sample sizes follow the DVD-Video sample layout.

use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discrip::{
    codec_id, ff_discrip_audio_close, ff_discrip_audio_flush, ff_discrip_audio_open, ff_discrip_audio_set_state,
    ff_discrip_audio_unit, ff_discrip_cutter_close, ff_discrip_cutter_flush, ff_discrip_cutter_open,
    ff_discrip_cutter_set_state, ff_discrip_cutter_write, ff_discrip_frame_unref, ff_discrip_lpcm_convert,
    ff_discrip_lpcm_header, Audio, Cutter, Frame, Lpcm, F_KEY, F_SYNC,
};

fn header(p: &mut Lpcm, h: &[u8], hd: bool) -> c_int {
    // SAFETY: valid pointers.
    unsafe { ff_discrip_lpcm_header(p, std::ptr::null_mut(), h.as_ptr(), c_int::try_from(h.len()).unwrap(), c_int::from(hd)) }
}

fn convert(p: &Lpcm, data: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; data.len() * 6 / 5 + 16];
    // SAFETY: out has room for the conversion.
    let n = unsafe { ff_discrip_lpcm_convert(p, data.as_ptr(), c_int::try_from(data.len()).unwrap(), out.as_mut_ptr()) };
    out.truncate(usize::try_from(n).unwrap());
    out
}

#[test]
fn the_dvd_header_and_its_checks() {
    // 16 bits, 48 kHz, 2 channels, dynamic range off (as on the corpus disc)
    let mut p = Lpcm::default();
    assert_eq!(header(&mut p, &[0x00, 0x01, 0x80], false), 0);
    assert_eq!((p.bits, p.rate, p.channels, p.spf, p.frame_bytes), (16, 48_000, 2, 80, 320));
    // the frame number and the dynamic range may change; nothing else
    assert_eq!(header(&mut p, &[0x05, 0x01, 0x40], false), 0);
    assert_eq!(p.drc, 0x40);
    assert!(header(&mut p, &[0x00, 0x11, 0x80], false) < 0, "another sampling frequency");
    assert!(header(&mut p, &[0x20, 0x01, 0x80], false) < 0, "the reserved bit");
    // 24 bits, 96 kHz, 6 channels
    let mut q = Lpcm::default();
    assert_eq!(header(&mut q, &[0x00, 0x95, 0x80], false), 0);
    assert_eq!((q.bits, q.rate, q.channels, q.spf, q.frame_bytes, q.out_frame_bytes), (24, 96_000, 6, 160, 2880, 2880));
    assert!(header(&mut Lpcm::default(), &[0x00, 0xC1, 0x80], false) < 0, "no 28-bit samples");
    assert!(header(&mut Lpcm::default(), &[0x00, 0x31, 0x80], false) < 0, "no fourth frequency");
}

#[test]
fn the_hd_dvd_header_is_read_in_the_dvd_layout() {
    // quantisation 20 bits (byte 0 bit 0 = 0, byte 1 bit 7 = 1), frequency
    // code 1 (96 kHz), channel code 1 (2 channels), channel assignment 3
    let mut p = Lpcm::default();
    assert_eq!(header(&mut p, &[0x00, 0x91, 0x80, 0x00, 0x03], true), 0);
    assert_eq!((p.bits, p.rate, p.channels, p.spf, p.chmask), (20, 96_000, 2, 80, 0x33));
    assert_eq!(p.frame_bytes, 20 * 2 * 80 / 8);
    assert!(header(&mut Lpcm::default(), &[0x00, 0x48, 0x80, 0, 3], true) < 0, "frequency code 4");
    assert!(header(&mut Lpcm::default(), &[0x00, 0x08, 0x80, 0, 3], true) < 0, "channel code 8");
}

#[test]
fn samples_become_little_endian() {
    let mut p = Lpcm::default();
    header(&mut p, &[0x00, 0x01, 0x80], false);
    assert_eq!(convert(&p, &[0x12, 0x34, 0xAB, 0xCD]), vec![0x34, 0x12, 0xCD, 0xAB]);
    // 24 bits: a group of 4 samples = their high 16 bits, then their low bytes
    let mut q = Lpcm::default();
    header(&mut q, &[0x00, 0x81, 0x80], false);
    let g = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0xA1, 0xA2, 0xA3, 0xA4];
    assert_eq!(
        convert(&q, &g),
        vec![0xA1, 0x22, 0x11, 0xA2, 0x44, 0x33, 0xA3, 0x66, 0x55, 0xA4, 0x88, 0x77]
    );
    // 20 bits: the low 4 bits of two samples share a byte (high nibble first)
    let mut r = Lpcm::default();
    header(&mut r, &[0x00, 0x41, 0x80], false);
    let g = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x9A, 0xBC];
    assert_eq!(
        convert(&r, &g),
        vec![0x90, 0x22, 0x11, 0xA0, 0x44, 0x33, 0xB0, 0x66, 0x55, 0xC0, 0x88, 0x77]
    );
}

unsafe extern "C" fn on_frame(o: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: o is the &mut Vec; f a frame we own.
    let (v, fr) = unsafe { (&mut *o.cast::<Vec<(Vec<u8>, i64, i64, u32)>>(), &mut *f) };
    // SAFETY: data holds size bytes.
    let b = unsafe { std::slice::from_raw_parts(fr.data, usize::try_from(fr.size).unwrap()) }.to_vec();
    v.push((b, fr.time, fr.dur, fr.flags));
    // SAFETY: ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}

#[test]
fn units_are_frames_of_the_stated_size() {
    // 16-bit stereo 48 kHz: frames of 320 bytes (80 samples, 1/600 s); PES
    // payloads of 2000 bytes each with a header and a time; the bytes after
    // the last whole frame are no unit
    let id = codec_id(c"pcm_dvd").unwrap();
    let stream: Vec<u8> = (0..4100u32).map(|i| u8::try_from(i % 251).unwrap()).collect();
    let mut p = Lpcm::default();
    let mut outs: Vec<(Vec<u8>, i64, i64, u32)> = Vec::new();
    let mut audio: *mut Audio = std::ptr::null_mut();
    let mut c: *mut Cutter = std::ptr::null_mut();
    // SAFETY: p and outs outlive both stages; both are closed below.
    unsafe {
        let null = std::ptr::null_mut();
        assert_eq!(ff_discrip_audio_open(&raw mut audio, null, id, 0, on_frame, (&raw mut outs).cast()), 0);
        assert_eq!(ff_discrip_cutter_open(&raw mut c, null, id, ff_discrip_audio_unit, audio.cast()), 0);
        ff_discrip_cutter_set_state(c, (&raw mut p).cast());
        ff_discrip_audio_set_state(audio, (&raw mut p).cast());
        for (k, chunk) in stream.chunks(2000).enumerate() {
            let h = [u8::try_from(k).unwrap(), 0x01, 0x80];
            assert_eq!(ff_discrip_lpcm_header(&raw mut p, null, h.as_ptr(), 3, 0), 0);
            let t = i64::try_from(k).unwrap() * 1000;
            assert_eq!(ff_discrip_cutter_write(c, chunk.as_ptr(), c_int::try_from(chunk.len()).unwrap(), t), 0);
        }
        assert_eq!(ff_discrip_cutter_flush(c), 0);
        assert_eq!(ff_discrip_audio_flush(audio), 0);
        ff_discrip_cutter_close(&raw mut c);
        ff_discrip_audio_close(&raw mut audio);
    }
    assert_eq!(outs.len(), 4100 / 320);
    for (k, o) in outs.iter().enumerate() {
        let raw = &stream[k * 320..(k + 1) * 320];
        let swapped: Vec<u8> = raw.chunks(2).flat_map(|s| [s[1], s[0]]).collect();
        assert_eq!(o.0, swapped);
        assert_eq!(o.2, 80 * 1_080_000_000 / 48_000);
        assert_eq!(o.3 & (F_KEY | F_SYNC), F_KEY, "key frames, never sync units");
    }
    // the first frame starting in each payload takes its time
    assert_eq!(outs[0].1, 0);
    assert_eq!(outs[7].1, 1000, "frame 7 (byte 2240) is the first to start in the second payload");
}
