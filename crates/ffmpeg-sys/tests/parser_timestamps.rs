//! Timestamps the AC-3 parser gives frames when an AC-3 header is split across
//! two packets.
//!
//! MPEG systems rule (ISO/IEC 13818-1 2.4.3.7): a PES packet's PTS belongs to
//! the first access unit that starts in that packet. A frame that starts in
//! one packet and whose 7-byte header ends in the next must not get the next
//! packet's PTS (that one belongs to the frame after it). DVD-Video audio
//! packs carry such splits wherever the frame size and the pack payload size
//! line up that way.

use std::ffi::CString;
use std::os::raw::{c_int, c_void};

use ffmpeg_sys::{dr_parser_run, AV_NOPTS_VALUE};

/// One AC-3 frame: 48 kHz, 192 kbit/s (frmsizecod 0x14, 768 bytes), bsid 8,
/// 2/0 stereo; the rest of the frame is zero (the parser only reads headers).
fn ac3_frame() -> Vec<u8> {
    let mut f = vec![0u8; 768];
    f[..7].copy_from_slice(&[0x0B, 0x77, 0x00, 0x00, 0x14, 0x40, 0x40]);
    f
}

const FRAME: usize = 768;
const PAYLOAD: usize = 2016; // DVD AC-3 pack payload size
const TICKS: i64 = 2880; // 1536 samples at 48 kHz in 90 kHz units

struct Frames(Vec<(usize, i64)>);

unsafe extern "C" fn on_frame(opaque: *mut c_void, size: c_int, pts: i64, _dts: i64) {
    // SAFETY: opaque is the &mut Frames passed below.
    let frames = unsafe { &mut *opaque.cast::<Frames>() };
    frames.0.push((usize::try_from(size).unwrap(), pts));
}

#[test]
fn split_header_frame_does_not_take_the_next_packets_pts() {
    // `lead` junk bytes before frame 0 shift frame starts against packet
    // boundaries so that headers are split (e.g. frame 13 starts 4 bytes before
    // the end of packet 4, counting from 0).
    let lead = 92;
    let nb_frames = 60;
    let mut stream = vec![0u8; lead];
    for _ in 0..nb_frames {
        stream.extend_from_slice(&ac3_frame());
    }
    let frame_start = |n: usize| lead + n * FRAME;

    let packets: Vec<&[u8]> = stream.chunks(PAYLOAD).collect();
    let mut pts = Vec::new();
    let mut split = Vec::new();
    for (k, p) in packets.iter().enumerate() {
        let (start, end) = (k * PAYLOAD, k * PAYLOAD + p.len());
        // PTS of the first frame starting in this packet, if any.
        let first = (0..nb_frames).find(|&n| (start..end).contains(&frame_start(n)));
        pts.push(first.map_or(AV_NOPTS_VALUE, |n| i64::try_from(n).unwrap() * TICKS));
        split.extend((0..nb_frames).filter(|&n| frame_start(n) < end && frame_start(n) + 7 > end));
    }
    assert!(!split.is_empty(), "test layout has no split header");

    let data: Vec<*const u8> = packets.iter().map(|p| p.as_ptr()).collect();
    let sizes: Vec<c_int> = packets.iter().map(|p| c_int::try_from(p.len()).unwrap()).collect();
    let codec = CString::new("ac3").unwrap();
    let mut frames = Frames(Vec::new());
    // SAFETY: all arrays have `packets.len()` entries and outlive the call.
    let ret = unsafe {
        dr_parser_run(
            codec.as_ptr(),
            data.as_ptr(),
            sizes.as_ptr(),
            pts.as_ptr(),
            c_int::try_from(packets.len()).unwrap(),
            on_frame,
            std::ptr::addr_of_mut!(frames).cast(),
        )
    };
    assert_eq!(ret, 0);

    // The parser returns the lead bytes as a first chunk, then the frames.
    let frames: Vec<i64> = frames.0.iter().filter(|(size, _)| *size == FRAME).map(|f| f.1).collect();
    assert_eq!(frames.len(), nb_frames);
    for (n, &got) in frames.iter().enumerate() {
        let want = i64::try_from(n).unwrap() * TICKS;
        assert!(
            got == AV_NOPTS_VALUE || got == want,
            "frame {n} (split header: {}) got pts {got}, belongs to {want}",
            split.contains(&n)
        );
    }
    for &n in &split {
        assert_eq!(frames[n], AV_NOPTS_VALUE, "split-header frame {n} must get no PTS of its own");
    }
}
