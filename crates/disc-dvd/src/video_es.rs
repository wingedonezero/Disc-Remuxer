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
//! Spans across input-chunk boundaries need a small tail buffer (up
//! to 3 bytes of "maybe start code prefix") so we don't miss start
//! codes that straddle two `feed()` calls.

use std::io::{self, Write};

/// Streaming filter that splits an MPEG-2 video byte stream into a
/// "video without user_data" stream and a "user_data only" stream.
///
/// Doesn't own the writers — caller provides them as `&mut impl Write`
/// on each `feed()` call. This keeps the filter testable against
/// byte buffers and lets the caller pick BufWriter / Vec / etc.
pub struct UserDataFilter {
    state: State,
    /// 0-3 bytes of the *most recent* tail of input. We hold these
    /// back from the writer until we see the 4th byte of a potential
    /// start code, so we can correctly classify whether the start
    /// code belongs to the "normal" or "user_data" side.
    ///
    /// Invariant: at most 3 bytes; first byte (if present) is `0x00`,
    /// second `0x00`, third `0x01`. (We only buffer partial start-code
    /// prefixes, never arbitrary bytes.)
    tail: Vec<u8>,
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
            video_bytes: 0,
            user_data_bytes: 0,
            user_data_blocks: 0,
        }
    }

    /// Feed `chunk` through the filter. Emits non-user_data bytes to
    /// `video_w` and user_data bytes (including the `00 00 01 B2` start
    /// code prefix) to `user_data_w`.
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
            // Look for the next `00 00 01 XX` start code starting at
            // position i. `memchr` would be faster; for clarity we
            // do a simple byte loop.
            if let Some(sc_pos) = find_start_code(&combined[i..]) {
                let abs = i + sc_pos;
                // Bytes BEFORE the start code go to the current
                // state's destination.
                let prefix = &combined[i..abs];
                self.write_state_bytes(prefix, video_w, user_data_w)?;

                // We need at least 4 bytes from `abs` to know the
                // start-code value. If not yet available, hold the
                // remainder back into `tail` for next call.
                let remaining = combined.len() - abs;
                if remaining < 4 {
                    self.tail.extend_from_slice(&combined[abs..]);
                    return Ok(());
                }
                let sc_val = combined[abs + 3];
                let was_user_data = self.state == State::InUserData;
                let starts_user_data = sc_val == 0xB2;

                if was_user_data && !starts_user_data {
                    // We were inside a user_data block and this is a
                    // non-user_data start code, so the user_data block
                    // ENDS just before this start code. The new start
                    // code itself goes to video, NOT user_data.
                    self.user_data_blocks += 1;
                    self.state = State::Normal;
                    // Write the 4 start-code bytes to video.
                    video_w.write_all(&combined[abs..abs + 4])?;
                    self.video_bytes += 4;
                } else if !was_user_data && starts_user_data {
                    // Entering user_data. The 4 start-code bytes go to
                    // the user_data sidecar.
                    self.state = State::InUserData;
                    user_data_w.write_all(&combined[abs..abs + 4])?;
                    self.user_data_bytes += 4;
                } else if was_user_data && starts_user_data {
                    // Consecutive user_data blocks (e.g. ATSC GA94 then
                    // DTG1). Count the previous block; stay in
                    // InUserData; write the new 4 bytes to user_data.
                    self.user_data_blocks += 1;
                    user_data_w.write_all(&combined[abs..abs + 4])?;
                    self.user_data_bytes += 4;
                } else {
                    // Normal -> Normal, just pass through.
                    video_w.write_all(&combined[abs..abs + 4])?;
                    self.video_bytes += 4;
                }
                i = abs + 4;
            } else {
                // No more start codes in this chunk. Most of the
                // remainder goes to the current state's destination,
                // EXCEPT for the last 3 bytes which might be the
                // partial prefix of a future start code — hold those
                // back.
                let rem = &combined[i..];
                if rem.len() <= 3 {
                    // Save the whole thing for next call.
                    self.tail.extend_from_slice(rem);
                } else {
                    let split = rem.len() - 3;
                    self.write_state_bytes(&rem[..split], video_w, user_data_w)?;
                    self.tail.extend_from_slice(&rem[split..]);
                }
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
            }
            State::InUserData => {
                user_data_w.write_all(bytes)?;
                self.user_data_bytes += bytes.len() as u64;
            }
        }
        Ok(())
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

#[cfg(test)]
mod tests {
    use super::*;

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
