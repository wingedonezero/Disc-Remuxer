//! The rip core's audio stage run over built payloads: helpers shared by the
//! discrip_* tests.

use std::ffi::CString;
use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discrip::{
    codec_id, ff_discrip_audio_close, ff_discrip_audio_flush, ff_discrip_audio_marker, ff_discrip_audio_open,
    ff_discrip_audio_stats, ff_discrip_audio_unit, ff_discrip_cutter_close, ff_discrip_cutter_flush,
    ff_discrip_cutter_open, ff_discrip_cutter_stats, ff_discrip_cutter_write, ff_discrip_frame_unref, Audio,
    AudioStats, Cutter, CutterStats, Frame,
};
use ffmpeg_sys::AV_NOPTS_VALUE;

pub const TICKS: u64 = 1_080_000_000;

#[derive(Debug, Clone, PartialEq)]
pub struct Out {
    pub bytes: Vec<u8>,
    pub time: i64,
    pub dur: i64,
    pub flags: u32,
}

unsafe extern "C" fn on_frame(opaque: *mut c_void, f: *mut Frame) -> c_int {
    // SAFETY: opaque is the &mut Vec<Out> given to the audio stage; f is a valid frame we now own.
    let (outs, fr) = unsafe { (&mut *opaque.cast::<Vec<Out>>(), &mut *f) };
    let size = usize::try_from(fr.size).unwrap();
    let bytes = if size == 0 {
        Vec::new()
    } else {
        // SAFETY: data holds size bytes.
        unsafe { std::slice::from_raw_parts(fr.data, size) }.to_vec()
    };
    outs.push(Out { bytes, time: fr.time, dur: fr.dur, flags: fr.flags });
    // SAFETY: the frame is ours.
    unsafe { ff_discrip_frame_unref(f) };
    0
}

pub struct Run {
    pub outs: Vec<Out>,
    pub audio: AudioStats,
    pub cutter: CutterStats,
}

/// Payloads (bytes, time) of one segment through the cutter and the audio stage.
pub fn run(codec: &str, payloads: &[(&[u8], Option<i64>)], marker: Option<i64>) -> Run {
    let id = codec_id(&CString::new(codec).unwrap()).unwrap();
    let mut outs: Vec<Out> = Vec::new();
    let mut a: *mut Audio = std::ptr::null_mut();
    let mut c: *mut Cutter = std::ptr::null_mut();
    // SAFETY: outs outlives both stages; both are closed below.
    unsafe {
        assert_eq!(
            ff_discrip_audio_open(&raw mut a, std::ptr::null_mut(), id, 0, on_frame, (&raw mut outs).cast()),
            0
        );
        assert_eq!(ff_discrip_cutter_open(&raw mut c, std::ptr::null_mut(), id, ff_discrip_audio_unit, a.cast()), 0);
        for (data, time) in payloads {
            let len = c_int::try_from(data.len()).unwrap();
            assert_eq!(ff_discrip_cutter_write(c, data.as_ptr(), len, time.unwrap_or(AV_NOPTS_VALUE)), 0);
        }
        assert_eq!(ff_discrip_cutter_flush(c), 0);
        if let Some(t) = marker {
            assert_eq!(ff_discrip_audio_marker(a, t), 0);
        }
        assert_eq!(ff_discrip_audio_flush(a), 0);
        let mut audio = AudioStats::default();
        let mut cutter = CutterStats::default();
        ff_discrip_audio_stats(a, &raw mut audio);
        ff_discrip_cutter_stats(c, &raw mut cutter);
        ff_discrip_cutter_close(&raw mut c);
        ff_discrip_audio_close(&raw mut a);
        Run { outs, audio, cutter }
    }
}

pub fn dur(samples: u64, rate: u64) -> i64 {
    i64::try_from(samples * TICKS / rate).unwrap()
}

/// One payload per frame, each with a time (k * 1000).
pub fn timed(frames: &[Vec<u8>]) -> Vec<(&[u8], Option<i64>)> {
    frames.iter().enumerate().map(|(k, f)| (f.as_slice(), Some(i64::try_from(k).unwrap() * 1000))).collect()
}

