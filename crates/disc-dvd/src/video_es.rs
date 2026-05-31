//! Streaming filter for MPEG-2 elementary video that strips
//! `user_data_start_code` (`0x000001B2`) blocks and captures their
//! payloads to a sidecar — matching MakeMKV's video extraction.
//!
//! Why: DVD-Video MPEG-2 carries NTSC Line-21 closed captions inside
//! the `user_data` extension. MakeMKV strips those bytes from the
//! video elementary stream (so the result is "pure" MPEG-2 video) and
//! emits them separately as a track (typically `.srt` after decoding,
//! but we keep the raw bytes for now since the EIA-608 decoder isn't
//! written yet).
//!
//! Empirically verified on ANGEL_S1D1 title 1: our naive demux output
//! has 5,805 user_data start codes (~1.26 MB) that MakeMKV's
//! mkvextract output has 0 of. Filtering those out is the difference
//! between our `video.0xE0.m2v` and MakeMKV's `B1_t00_track1_[eng].mpg`.
//!
//! ## Algorithm
//!
//! MPEG-2 video is a stream of `start_code` units. Each starts with
//! `00 00 01` followed by a 1-byte `start_code_value`. The next start
//! code marks the end of the previous unit. Start codes never appear
//! in payload data (the bitstream's slice / picture coding is bit-
//! stuffed to avoid emulating one).
//!
//! ```text
//!  ... slice ... | 00 00 01 B2 GA94 cc_data ... | 00 00 01 B3 ...
//!  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//!  emit                user_data block (strip)         emit (next start code)
//! ```
//!
//! Our filter is a small state machine:
//!
//! * `Normal` — copying input → output verbatim; watching for the
//!   sequence `00 00 01 B2`.
//! * `InUserData` — captured the start code; routing bytes to the CC
//!   sidecar; watching for the next `00 00 01 XX` (where `XX != B2`).
//!
//! Spans across input-chunk boundaries need a tail buffer that holds
//! back two kinds of trailing bytes:
//!
//! 1. Up to 3 trailing bytes that could be the start of a start_code
//!    prefix (`00 00 01`) whose value byte will arrive in the next
//!    `feed()` call.
//! 2. Any trailing zero-byte run. MPEG-2 allows arbitrary `zero_byte`
//!    stuffing before any start code (via `next_start_code()` in
//!    ISO/IEC 13818-2 §6.2.1), and that stuffing belongs semantically
//!    to the *following* block. If the following block turns out to be
//!    a user_data block, we have to strip the leading zero stuffing
//!    along with it. Until we see the value byte of the next start
//!    code we can't tell which side the zeros belong to, so we hold
//!    them back too.
//!
//! Verified against `mkvextract` of MakeMKV's MKV on ANGEL_S1D1
//! title 1: at the first GOP→user_data transition the disc has 124
//! `zero_byte` stuffing bytes between the GOP header and the
//! `00 00 01 B2` user_data start code, and MakeMKV strips all 124
//! along with the user_data block.

use std::io::{self, Write};

/// Streaming filter that splits an MPEG-2 video byte stream into a
/// "video without user_data" stream and a "user_data only" stream.
///
/// Doesn't own the writers — caller provides them as `&mut impl Write`
/// on each `feed()` call. This keeps the filter testable against
/// byte buffers and lets the caller pick BufWriter / Vec / etc.
pub struct UserDataFilter {
    state: State,
    /// Trailing bytes of the most recent input held back from the
    /// writer. Contains either (or both): the last 0–3 bytes which
    /// could be a partial start-code prefix, and any trailing
    /// `zero_byte` stuffing run whose destination depends on whether
    /// the next start code is `00 00 01 B2` (user_data) or not.
    tail: Vec<u8>,
    /// Value byte of the most recent kept start code (i.e. the last
    /// non-user_data start code we wrote to video). Used to enforce
    /// fixed-length body invariants — e.g. an `00 00 01 B8` GOP
    /// header always has exactly 4 bytes of body per ISO/IEC 13818-2
    /// §6.2.2.6, so if its body's last byte happens to be `0x00` we
    /// must not misclassify it as `zero_byte` stuffing for a
    /// subsequent user_data block.
    last_sc_value: u8,
    /// Bytes written to the video writer since `last_sc_value` was
    /// last updated (= bytes of the current block's body emitted so
    /// far, including bytes still pending in `tail` that will reach
    /// video on the next feed).
    bytes_since_last_sc: u64,
    /// Bytes written to the normal (video) writer so far.
    pub video_bytes: u64,
    /// Bytes written to the user_data (CC) writer so far.
    pub user_data_bytes: u64,
    /// Number of complete user_data blocks observed.
    pub user_data_blocks: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum State {
    Normal,
    InUserData,
}

impl Default for UserDataFilter {
    fn default() -> Self {
        Self::new()
    }
}

impl UserDataFilter {
    #[must_use]
    pub fn new() -> Self {
        Self {
            state: State::Normal,
            tail: Vec::with_capacity(3),
            last_sc_value: 0,
            bytes_since_last_sc: 0,
            video_bytes: 0,
            user_data_bytes: 0,
            user_data_blocks: 0,
        }
    }

    /// Feed `chunk` through the filter. Emits non-user_data bytes to
    /// `video_w` and user_data bytes (including the `00 00 01 B2` start
    /// code prefix and any leading `zero_byte` stuffing) to
    /// `user_data_w`.
    pub fn feed<V: Write, U: Write>(
        &mut self,
        chunk: &[u8],
        video_w: &mut V,
        user_data_w: &mut U,
    ) -> io::Result<()> {
        // Concatenate the held-back tail with the new chunk so the
        // search sees a contiguous slice.
        let mut combined = std::mem::take(&mut self.tail);
        combined.extend_from_slice(chunk);

        let mut i = 0;
        while i < combined.len() {
            if let Some(sc_pos) = find_start_code(&combined[i..]) {
                let abs = i + sc_pos;
                let prefix = &combined[i..abs];

                // If we don't have the start-code value byte yet, hold
                // back the partial start code AND any trailing zero
                // run from the prefix — those zeros may be `zero_byte`
                // stuffing for an upcoming `00 00 01 B2` user_data
                // block, in which case they belong on the cc side.
                let remaining = combined.len() - abs;
                if remaining < 4 {
                    let zeros = trailing_zero_count(prefix);
                    let content_end = prefix.len() - zeros;
                    self.write_state_bytes(
                        &prefix[..content_end],
                        video_w,
                        user_data_w,
                    )?;
                    self.tail.extend_from_slice(&combined[i + content_end..]);
                    return Ok(());
                }

                let sc_val = combined[abs + 3];
                let was_user_data = self.state == State::InUserData;
                let starts_user_data = sc_val == 0xB2;
                let state_changing = was_user_data != starts_user_data;

                if state_changing && !was_user_data {
                    // Normal → InUserData: split the prefix into the
                    // body bytes of the previous kept block + trailing
                    // `zero_byte` stuffing for the upcoming user_data
                    // block. Preserve any structurally-required body
                    // bytes (e.g. GOP body is always 4 bytes per
                    // ISO/IEC 13818-2 §6.2.2.6) even when those bytes
                    // are `0x00` and would otherwise look like
                    // stuffing.
                    let zeros = trailing_zero_count(prefix);
                    let min_body = min_body_bytes_for(self.last_sc_value) as u64;
                    let body_remaining = min_body.saturating_sub(self.bytes_since_last_sc);
                    let preserve_floor = std::cmp::min(body_remaining as usize, prefix.len());
                    let content_end = std::cmp::max(prefix.len() - zeros, preserve_floor);
                    self.write_state_bytes(
                        &prefix[..content_end],
                        video_w,
                        user_data_w,
                    )?;
                    let trailing = &prefix[content_end..];
                    user_data_w.write_all(trailing)?;
                    self.user_data_bytes += trailing.len() as u64;
                } else if state_changing && was_user_data {
                    // InUserData → Normal: the user_data block ends.
                    // Any trailing zero bytes in the prefix are
                    // `zero_byte` stuffing for the kept block we're
                    // entering and belong on the video side.
                    let zeros = trailing_zero_count(prefix);
                    let content_end = prefix.len() - zeros;
                    self.write_state_bytes(
                        &prefix[..content_end],
                        video_w,
                        user_data_w,
                    )?;
                    let trailing = &prefix[content_end..];
                    video_w.write_all(trailing)?;
                    self.video_bytes += trailing.len() as u64;
                } else {
                    // No state change: prefix and any trailing zeros
                    // all go to the current state's writer.
                    self.write_state_bytes(prefix, video_w, user_data_w)?;
                }

                if was_user_data && !starts_user_data {
                    self.user_data_blocks += 1;
                    self.state = State::Normal;
                    video_w.write_all(&combined[abs..abs + 4])?;
                    self.video_bytes += 4;
                    self.last_sc_value = sc_val;
                    self.bytes_since_last_sc = 0;
                } else if !was_user_data && starts_user_data {
                    self.state = State::InUserData;
                    user_data_w.write_all(&combined[abs..abs + 4])?;
                    self.user_data_bytes += 4;
                } else if was_user_data && starts_user_data {
                    // Consecutive user_data blocks (e.g. ATSC GA94
                    // then DTG1). Count the previous block; stay in
                    // InUserData; write the new 4 bytes to user_data.
                    self.user_data_blocks += 1;
                    user_data_w.write_all(&combined[abs..abs + 4])?;
                    self.user_data_bytes += 4;
                } else {
                    video_w.write_all(&combined[abs..abs + 4])?;
                    self.video_bytes += 4;
                    self.last_sc_value = sc_val;
                    self.bytes_since_last_sc = 0;
                }
                i = abs + 4;
            } else {
                // No more start codes in this chunk. Hold back the
                // last 3 bytes (potential partial start-code prefix)
                // AND any trailing zero run — both might be part of
                // a future user_data block's leading bytes.
                let rem = &combined[i..];
                let zeros = trailing_zero_count(rem);
                let hold = std::cmp::max(3, zeros).min(rem.len());
                let commit = rem.len() - hold;
                self.write_state_bytes(&rem[..commit], video_w, user_data_w)?;
                self.tail.extend_from_slice(&rem[commit..]);
                return Ok(());
            }
        }
        Ok(())
    }

    /// Flush any held-back tail bytes. Called once at end-of-stream.
    /// Whatever's in `tail` couldn't have been a start code (we never
    /// saw the trailing byte), so it belongs to the current state's
    /// destination.
    pub fn finish<V: Write, U: Write>(
        mut self,
        video_w: &mut V,
        user_data_w: &mut U,
    ) -> io::Result<UserDataStats> {
        if !self.tail.is_empty() {
            let tail = std::mem::take(&mut self.tail);
            self.write_state_bytes(&tail, video_w, user_data_w)?;
        }
        // If we end in InUserData, count the trailing block.
        if self.state == State::InUserData {
            self.user_data_blocks += 1;
        }
        Ok(UserDataStats {
            video_bytes: self.video_bytes,
            user_data_bytes: self.user_data_bytes,
            user_data_blocks: self.user_data_blocks,
        })
    }

    fn write_state_bytes<V: Write, U: Write>(
        &mut self,
        bytes: &[u8],
        video_w: &mut V,
        user_data_w: &mut U,
    ) -> io::Result<()> {
        if bytes.is_empty() {
            return Ok(());
        }
        match self.state {
            State::Normal => {
                video_w.write_all(bytes)?;
                self.video_bytes += bytes.len() as u64;
                self.bytes_since_last_sc += bytes.len() as u64;
            }
            State::InUserData => {
                user_data_w.write_all(bytes)?;
                self.user_data_bytes += bytes.len() as u64;
            }
        }
        Ok(())
    }
}

/// Count the number of trailing `0x00` bytes in `buf`.
fn trailing_zero_count(buf: &[u8]) -> usize {
    let mut n = 0;
    while n < buf.len() && buf[buf.len() - 1 - n] == 0 {
        n += 1;
    }
    n
}

/// Minimum mandatory body length (bytes after the 4-byte start code)
/// for kept (non-user_data) start codes. Used to guarantee we do not
/// misclassify body bytes ending in `0x00` as `zero_byte` stuffing.
///
/// Only `B8` (group_of_pictures_header — exactly 4 bytes of `time_code
/// closed_gop broken_link reserved` per ISO/IEC 13818-2 §6.2.2.6) is
/// currently load-bearing for byte-identity against MakeMKV; other
/// start codes either have purely variable-length bodies or have not
/// produced a divergence in the test corpus yet.
fn min_body_bytes_for(sc_val: u8) -> usize {
    match sc_val {
        0xB8 => 4,
        _ => 0,
    }
}

/// Locate the first MPEG-2 start-code prefix (`00 00 01`) in `buf`.
/// Returns the byte offset of the leading zero; the start-code value
/// byte is at `result + 3`. Returns `None` if no prefix found.
fn find_start_code(buf: &[u8]) -> Option<usize> {
    if buf.len() < 3 {
        return None;
    }
    let mut i = 0;
    while i + 2 < buf.len() {
        if buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 {
            return Some(i);
        }
        i += 1;
    }
    None
}

#[derive(Debug, Clone, Copy)]
pub struct UserDataStats {
    pub video_bytes: u64,
    pub user_data_bytes: u64,
    pub user_data_blocks: u64,
}

/// Frame rate (frames/sec) for an MPEG-2 `frame_rate_code`
/// (ISO/IEC 13818-2 Table 6-4). `0.0` for forbidden/reserved codes.
fn frame_rate_from_code(code: u8) -> f64 {
    match code {
        1 => 24_000.0 / 1001.0,
        2 => 24.0,
        3 => 25.0,
        4 => 30_000.0 / 1001.0,
        5 => 30.0,
        6 => 50.0,
        7 => 60_000.0 / 1001.0,
        8 => 60.0,
        _ => 0.0,
    }
}

/// Timing facts scanned from the head of an MPEG-2 video elementary
/// stream, used to anchor the audio delay to the first *displayed*
/// frame rather than the first *coded* (I-)frame's PTS.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct VideoStartInfo {
    /// Coded frame rate from the first `sequence_header` (`0.0` if none seen).
    pub frame_rate: f64,
    /// B-frames (in *frame* units) that display before the first I-frame —
    /// the open-GOP reorder offset. The first displayed picture is this many
    /// frame periods *earlier* than the first I-frame's PTS, so the delay
    /// anchor is `first_i_frame_pts - leading_b_frames * (90000 / frame_rate)`.
    pub leading_b_frames: u32,
}

/// Scan the head of an MPEG-2 video elementary stream for the frame rate
/// and the leading-B-frame reorder count.
///
/// The first I-frame's PES PTS is *not* when the picture first appears:
/// open-GOP B-frames that reference the previous GOP display before it, so
/// the true display start is earlier. This ports DGIndex's `LeadingBFrames`
/// logic (DGMPGDec 2005, `DGIndex/mpeg2dec.cpp` lines 422-437, with the
/// `VideoPTS -= LeadingBFrames * picture_period * 90000` adjustment in
/// `getbit.cpp`): after the first I-frame, count consecutive B-frames
/// (frame picture `+= 2`, field picture `+= 1`) up to the first non-B
/// picture, then `/= 2` for frame units. `picture_coding_type` comes from
/// the `picture_header` (`00 00 01 00`, ISO/IEC 13818-2 §6.2.3) and
/// `picture_structure` from the following `picture_coding_extension`
/// (`00 00 01 B5`, extension id 8, §6.2.3.1).
///
/// `data` need only cover the first GOP (a few tens of KB).
#[must_use]
pub fn analyze_video_start(data: &[u8]) -> VideoStartInfo {
    const I_TYPE: u8 = 1;
    const B_TYPE: u8 = 3;
    const PIC_STRUCT_FRAME: u8 = 3;
    const EXT_PICTURE_CODING: u8 = 8;

    let mut frame_rate = 0.0_f64;
    let mut leading_b_fields: u32 = 0;
    let mut seen_first_i = false;
    let mut done = false;
    // Set when the current picture is a leading B-frame awaiting its
    // picture_coding_extension (to learn frame-vs-field structure).
    let mut pending_b = false;

    let mut i = 0usize;
    while i + 3 < data.len() {
        if !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            i += 1;
            continue;
        }
        match data[i + 3] {
            // sequence_header: body[3] low nibble = frame_rate_code.
            0xB3 => {
                if frame_rate == 0.0 && i + 7 < data.len() {
                    frame_rate = frame_rate_from_code(data[i + 7] & 0x0F);
                }
            }
            // picture_header: picture_coding_type = bits [5:3] of body[1].
            0x00 => {
                pending_b = false;
                if !done && i + 5 < data.len() {
                    let pct = (data[i + 5] >> 3) & 0x07;
                    if pct == I_TYPE {
                        if seen_first_i {
                            done = true; // a second I ends the leading-B run
                        } else {
                            seen_first_i = true;
                        }
                    } else if seen_first_i {
                        if pct == B_TYPE {
                            pending_b = true; // count once we read its structure
                        } else {
                            done = true; // first non-B (P) after I ends the run
                        }
                    }
                }
            }
            // picture_coding_extension (ext id 8): picture_structure = body[2] & 0x03.
            0xB5 => {
                if pending_b
                    && i + 6 < data.len()
                    && (data[i + 4] >> 4) == EXT_PICTURE_CODING
                {
                    let pic_struct = data[i + 6] & 0x03;
                    leading_b_fields += if pic_struct == PIC_STRUCT_FRAME { 2 } else { 1 };
                    pending_b = false;
                }
            }
            _ => {}
        }
        i += 3;
    }
    VideoStartInfo {
        frame_rate,
        leading_b_frames: leading_b_fields / 2,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn analyze_video_start_counts_two_leading_b_frames() {
        // seq header (frame_rate_code 4 = 30000/1001), I-frame, two
        // frame-structured B-frames, then a P-frame (ends the run).
        let seq = b"\x00\x00\x01\xB3\x00\x00\x00\x14"; // body[3]=0x14 -> code 4
        let i_pic = b"\x00\x00\x01\x00\x00\x08"; // pct=(0x08>>3)&7 = 1 (I)
        let b_pic = b"\x00\x00\x01\x00\x00\x18"; // pct=(0x18>>3)&7 = 3 (B)
        let p_pic = b"\x00\x00\x01\x00\x00\x10"; // pct=(0x10>>3)&7 = 2 (P)
        let pce_frame = b"\x00\x00\x01\xB5\x88\x00\x03"; // ext 8, picture_structure=3 (frame)
        let mut s = Vec::new();
        s.extend_from_slice(seq);
        s.extend_from_slice(i_pic);
        s.extend_from_slice(pce_frame);
        s.extend_from_slice(b_pic);
        s.extend_from_slice(pce_frame);
        s.extend_from_slice(b_pic);
        s.extend_from_slice(pce_frame);
        s.extend_from_slice(p_pic);
        s.extend_from_slice(pce_frame);
        let info = analyze_video_start(&s);
        assert!((info.frame_rate - 30_000.0 / 1001.0).abs() < 1e-6);
        assert_eq!(info.leading_b_frames, 2);
    }

    #[test]
    fn analyze_video_start_field_structured_b_frames_halve() {
        // Two field-structured B-frame pictures = one frame of leading B.
        let i_pic = b"\x00\x00\x01\x00\x00\x08";
        let b_pic = b"\x00\x00\x01\x00\x00\x18";
        let p_pic = b"\x00\x00\x01\x00\x00\x10";
        let pce_field = b"\x00\x00\x01\xB5\x88\x00\x01"; // picture_structure=1 (top field)
        let mut s = Vec::new();
        s.extend_from_slice(i_pic);
        s.extend_from_slice(pce_field);
        s.extend_from_slice(b_pic);
        s.extend_from_slice(pce_field);
        s.extend_from_slice(b_pic);
        s.extend_from_slice(pce_field);
        s.extend_from_slice(p_pic);
        let info = analyze_video_start(&s);
        assert_eq!(info.leading_b_frames, 1);
    }

    fn run(input: &[u8]) -> (Vec<u8>, Vec<u8>, UserDataStats) {
        let mut video = Vec::new();
        let mut user_data = Vec::new();
        let mut f = UserDataFilter::new();
        f.feed(input, &mut video, &mut user_data).unwrap();
        let stats = f.finish(&mut video, &mut user_data).unwrap();
        (video, user_data, stats)
    }

    #[test]
    fn empty_input_emits_nothing() {
        let (v, u, s) = run(&[]);
        assert!(v.is_empty() && u.is_empty());
        assert_eq!(s.user_data_blocks, 0);
    }

    #[test]
    fn no_user_data_passes_through() {
        // Just a sequence header + GOP header + slice — no user_data.
        let input = b"\x00\x00\x01\xB3SEQ\x00\x00\x01\xB8GOP\x00\x00\x01\x00SLICE";
        let (v, u, s) = run(input);
        assert_eq!(v, input);
        assert!(u.is_empty());
        assert_eq!(s.user_data_blocks, 0);
    }

    #[test]
    fn single_user_data_block_split() {
        // SEQ ... USER_DATA ... GOP
        let input = b"\x00\x00\x01\xB3SEQ\x00\x00\x01\xB2GA94CCDATA\x00\x00\x01\xB8GOP";
        let (v, u, s) = run(input);
        assert_eq!(v, b"\x00\x00\x01\xB3SEQ\x00\x00\x01\xB8GOP");
        assert_eq!(u, b"\x00\x00\x01\xB2GA94CCDATA");
        assert_eq!(s.user_data_blocks, 1);
    }

    #[test]
    fn user_data_at_eof_counts() {
        let input = b"\x00\x00\x01\xB3SEQ\x00\x00\x01\xB2GA94CCDATA";
        let (v, u, s) = run(input);
        assert_eq!(v, b"\x00\x00\x01\xB3SEQ");
        assert_eq!(u, b"\x00\x00\x01\xB2GA94CCDATA");
        assert_eq!(s.user_data_blocks, 1);
    }

    #[test]
    fn consecutive_user_data_blocks() {
        let input = b"\x00\x00\x01\xB3SEQ\x00\x00\x01\xB2GA94XX\x00\x00\x01\xB2DTG1YY\x00\x00\x01\xB8GOP";
        let (v, u, s) = run(input);
        assert_eq!(v, b"\x00\x00\x01\xB3SEQ\x00\x00\x01\xB8GOP");
        assert_eq!(u, b"\x00\x00\x01\xB2GA94XX\x00\x00\x01\xB2DTG1YY");
        assert_eq!(s.user_data_blocks, 2);
    }

    #[test]
    fn fed_byte_by_byte_same_result() {
        // Verify the streaming behaviour: feeding the same input one
        // byte at a time must produce identical output.
        let input = b"\x00\x00\x01\xB3SEQ\x00\x00\x01\xB2CC\x00\x00\x01\xB8GOP";
        let (v_whole, u_whole, _) = run(input);

        let mut video = Vec::new();
        let mut user_data = Vec::new();
        let mut f = UserDataFilter::new();
        for b in input {
            f.feed(&[*b], &mut video, &mut user_data).unwrap();
        }
        f.finish(&mut video, &mut user_data).unwrap();

        assert_eq!(video, v_whole);
        assert_eq!(user_data, u_whole);
    }

    #[test]
    fn start_code_straddling_chunk_boundary() {
        // Split the input so the start code's `01` byte lands in the
        // second chunk.
        let input = b"DATA\x00\x00\x01\xB2CC\x00\x00\x01\xB3END";
        // Split at byte 5: first chunk = b"DATA\x00", second = b"\x00\x01\xB2CC..."
        let mut video = Vec::new();
        let mut user_data = Vec::new();
        let mut f = UserDataFilter::new();
        f.feed(&input[..5], &mut video, &mut user_data).unwrap();
        f.feed(&input[5..], &mut video, &mut user_data).unwrap();
        f.finish(&mut video, &mut user_data).unwrap();

        let (v_whole, u_whole, _) = run(input);
        assert_eq!(video, v_whole);
        assert_eq!(user_data, u_whole);
    }

    #[test]
    fn find_start_code_locates_prefix() {
        assert_eq!(find_start_code(b"\x00\x00\x01"), Some(0));
        assert_eq!(find_start_code(b"AB\x00\x00\x01C"), Some(2));
        assert_eq!(find_start_code(b"\x00\x00\x02"), None);
        assert_eq!(find_start_code(b""), None);
        assert_eq!(find_start_code(b"\x00\x00"), None);
    }

    #[test]
    fn trailing_zero_count_basic() {
        assert_eq!(trailing_zero_count(b""), 0);
        assert_eq!(trailing_zero_count(b"abc"), 0);
        assert_eq!(trailing_zero_count(b"abc\x00"), 1);
        assert_eq!(trailing_zero_count(b"abc\x00\x00\x00"), 3);
        assert_eq!(trailing_zero_count(b"\x00\x00\x00"), 3);
    }

    /// Real-disc pattern from ANGEL_S1D1 title 1: GOP header is
    /// followed by 124 `zero_byte` stuffing bytes, then a user_data
    /// block carrying EIA-608 CC data, then `00 00 01 00` picture
    /// start. MakeMKV strips both the stuffing and the user_data.
    /// Our `.mpg` was previously +124 bytes per such GOP because the
    /// leading zero stuffing was leaking into the video output.
    #[test]
    fn zero_stuffing_before_user_data_goes_to_cc() {
        let mut input = Vec::new();
        // GOP header (8 bytes total, no payload past the start code).
        input.extend_from_slice(b"\x00\x00\x01\xB8\x83\xBF\x40\x40");
        // 124 zero_byte stuffing bytes.
        input.extend_from_slice(&[0u8; 124]);
        // user_data block: start code + "CC01F8" magic + 79 bytes payload.
        input.extend_from_slice(b"\x00\x00\x01\xB2\x43\x43\x01\xF8");
        input.extend_from_slice(&[0xAAu8; 79]);
        // Picture start code + a few picture bytes.
        input.extend_from_slice(b"\x00\x00\x01\x00\x00\x0F\xFF\xF8");

        let (v, u, s) = run(&input);
        assert_eq!(
            v,
            b"\x00\x00\x01\xB8\x83\xBF\x40\x40\x00\x00\x01\x00\x00\x0F\xFF\xF8",
            "video must contain GOP + picture, no stuffing"
        );
        assert_eq!(u.len(), 4 + 4 + 79 + 124, "cc gets stuffing + user_data block");
        assert!(u.starts_with(&[0u8; 124]), "cc starts with the 124 stuffing zeros");
        assert_eq!(&u[124..132], b"\x00\x00\x01\xB2\x43\x43\x01\xF8");
        assert_eq!(s.user_data_blocks, 1);
    }

    /// Zero stuffing before a NON-user_data start code must be
    /// preserved. Verified against MakeMKV: there are 4 zero bytes
    /// before the first slice header in both files.
    #[test]
    fn zero_stuffing_before_slice_is_preserved() {
        let input = b"\
            \x00\x00\x01\xB5\x8F\xFF\xFB\x88\
            \x00\x00\x00\x00\
            \x00\x00\x01\x01slice_data";
        let (v, u, _) = run(input);
        assert_eq!(v, input, "no user_data → entire input passes through");
        assert!(u.is_empty());
    }

    /// Multi-feed: the 124 zero stuffing run straddles a `feed()`
    /// boundary. Filter must still route all 124 zeros to cc.
    #[test]
    fn zero_stuffing_across_feed_boundary() {
        let mut input = Vec::new();
        input.extend_from_slice(b"\x00\x00\x01\xB8\x83\xBF\x40\x40");
        input.extend_from_slice(&[0u8; 124]);
        input.extend_from_slice(b"\x00\x00\x01\xB2payload");
        input.extend_from_slice(b"\x00\x00\x01\x00pic");

        let (v_whole, u_whole, _) = run(&input);

        // Split mid-zero-run.
        for split in [10usize, 50, 100, 130, 135] {
            let mut video = Vec::new();
            let mut user_data = Vec::new();
            let mut f = UserDataFilter::new();
            f.feed(&input[..split], &mut video, &mut user_data).unwrap();
            f.feed(&input[split..], &mut video, &mut user_data).unwrap();
            f.finish(&mut video, &mut user_data).unwrap();
            assert_eq!(video, v_whole, "split={split}");
            assert_eq!(user_data, u_whole, "split={split}");
        }
    }

    /// Real-disc regression from ANGEL_S1D1 title 1 at file offset
    /// 28052: a GOP whose body bytes end in `0x00` (because the
    /// time_code's low bits + closed_gop + broken_link + reserved
    /// happen to all be zero), followed by 124 bytes of `zero_byte`
    /// stuffing, then a user_data block. The GOP's last body byte
    /// must NOT be stripped as stuffing — it's a structurally
    /// mandatory part of the 4-byte GOP body per ISO/IEC 13818-2
    /// §6.2.2.6.
    #[test]
    fn gop_body_ending_in_zero_is_preserved() {
        let mut input = Vec::new();
        input.extend_from_slice(b"\x00\x00\x01\xB8\x83\xBF\x4E\x00");
        input.extend_from_slice(&[0u8; 124]);
        input.extend_from_slice(b"\x00\x00\x01\xB2\x43\x43\x01\xF8payload");
        input.extend_from_slice(b"\x00\x00\x01\x00\x00\x8F\xFF\xF8");

        let (v, _u, _s) = run(&input);
        // All 8 GOP bytes (including the trailing 0x00 of body) must
        // be present in video, followed immediately by the picture
        // start code (no stuffing in between).
        assert!(
            v.starts_with(b"\x00\x00\x01\xB8\x83\xBF\x4E\x00\x00\x00\x01\x00"),
            "expected GOP body intact + picture start code; got {:02x?}",
            &v[..16.min(v.len())]
        );
    }

    /// Zero stuffing immediately AFTER a user_data block (between the
    /// last cc_data byte and the next non-B2 start code) belongs to
    /// the following block. Currently MakeMKV-observed pattern has 0
    /// such zeros, but the rule should be symmetric.
    #[test]
    fn zero_stuffing_after_user_data_goes_to_video() {
        let mut input = Vec::new();
        input.extend_from_slice(b"\x00\x00\x01\xB8\x83\xBF\x40\x40");
        input.extend_from_slice(b"\x00\x00\x01\xB2\x43\x43\x01\xF8payload");
        input.extend_from_slice(&[0u8; 16]);
        input.extend_from_slice(b"\x00\x00\x01\x00pic_data");

        let (v, u, _) = run(&input);
        // The 16 zeros after user_data are stuffing for the picture
        // block → they should appear in video.
        assert!(
            v.windows(16 + 4).any(|w| w == [
                &[0u8; 16][..], b"\x00\x00\x01\x00"
            ].concat().as_slice()),
            "video should contain 16 zero stuffing bytes immediately before picture_start_code"
        );
        assert_eq!(u, b"\x00\x00\x01\xB2\x43\x43\x01\xF8payload");
    }

    /// Reproduces the regression observed when ripping ANGEL_S1D1 title 1:
    /// a GOP header followed by a picture-start code (00 00 01 00) and
    /// then picture data must come through verbatim, even when the
    /// picture data starts with bytes that look like part of a start
    /// code prefix.
    #[test]
    fn picture_start_after_gop_passes_through() {
        // Real-disc byte pattern from ANGEL: GOP header then picture
        // start (00 00 01 00), then `00 0F FF F8` (which contains a
        // 0x00 that could confuse a naive matcher).
        let input = b"\
            \x00\x00\x01\xB3SEQ\
            \x00\x00\x01\xB8GOP\x83\xBF\x40\x40\
            \x00\x00\x01\x00\x00\x0F\xFF\xF8\
            \x00\x00\x01\xB5EXT_DATA\
            \x00\x00\x01\x00MORE_PIC";
        let (v, u, _) = run(input);
        // No user_data → user_data side empty; video side gets everything.
        assert!(u.is_empty(), "no user_data expected, got {} bytes", u.len());
        assert_eq!(v, input,
            "video output must match input verbatim when there's no user_data");
    }
}
