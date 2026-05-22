//! Human-readable decoders for the bit-packed fields in libdvdread's
//! `video_attr_t` / `audio_attr_t` / `subp_attr_t` structures.
//!
//! The decode tables come from the DVD-Video specification (publicly
//! documented) and align with libdvdread's own printer in `ifo_print.c`.
//! All function names track the libdvdread field they decode so log lines
//! and CLI output stay greppable against the public C headers.

/// Decode `video_attr_t::mpeg_version` (2-bit field).
#[must_use]
pub fn video_mpeg_version(code: u8) -> &'static str {
    match code {
        0 => "mpeg1",
        1 => "mpeg2",
        _ => "reserved",
    }
}

/// Decode `video_attr_t::video_format` (2-bit field).
#[must_use]
pub fn video_format(code: u8) -> &'static str {
    match code {
        0 => "NTSC",
        1 => "PAL",
        _ => "reserved",
    }
}

/// Decode `video_attr_t::display_aspect_ratio` (2-bit field).
#[must_use]
pub fn video_aspect_ratio(code: u8) -> &'static str {
    match code {
        0 => "4:3",
        3 => "16:9",
        _ => "reserved",
    }
}

/// Decode `video_attr_t::permitted_df` (2-bit field — which display formats
/// the disc allows for 16:9 content on a 4:3 display).
#[must_use]
pub fn video_permitted_df(code: u8) -> &'static str {
    match code {
        0 => "pan&scan + letterboxed",
        1 => "pan&scan only",
        2 => "letterboxed only",
        3 => "not specified",
        _ => "reserved",
    }
}

/// Decode `video_attr_t::picture_size` into `(width, height)` in pixels.
/// `video_format == 0` (NTSC) gives 480-line heights; `1` (PAL) gives 576.
/// Returns `(0, 0)` for reserved size codes.
#[must_use]
pub fn video_picture_size(picture_size: u8, video_format: u8) -> (u16, u16) {
    let height: u16 = if video_format == 0 { 480 } else { 576 };
    match picture_size {
        0 => (720, height),
        1 => (704, height),
        2 => (352, height),
        3 => (352, height / 2),
        _ => (0, 0),
    }
}

/// Decode `audio_attr_t::audio_format` (3-bit field).
#[must_use]
pub fn audio_format(code: u8) -> &'static str {
    match code {
        0 => "ac3",
        2 => "mpeg1",
        3 => "mpeg2ext",
        4 => "lpcm",
        6 => "dts",
        _ => "reserved",
    }
}

/// Decode `audio_attr_t::sample_frequency` (2-bit field).
#[must_use]
pub fn audio_sample_frequency(code: u8) -> &'static str {
    match code {
        0 => "48 kHz",
        1 => "96 kHz",
        _ => "reserved",
    }
}

/// Decode `audio_attr_t::quantization` for LPCM (2-bit field, meaningful
/// only when `audio_format == 4`).
#[must_use]
pub fn audio_lpcm_quantization(code: u8) -> &'static str {
    match code {
        0 => "16-bit",
        1 => "20-bit",
        2 => "24-bit",
        _ => "reserved",
    }
}

/// Decode `audio_attr_t::quantization` for MPEG (2-bit field, meaningful
/// only when `audio_format == 2` or `3`). For MPEG this field carries
/// dynamic-range-control state, not bit depth.
#[must_use]
pub fn audio_mpeg_drc(code: u8) -> &'static str {
    match code {
        0 => "no drc",
        1 => "drc",
        _ => "reserved",
    }
}

/// Decode `audio_attr_t::application_mode` (2-bit field).
#[must_use]
pub fn audio_application_mode(code: u8) -> &'static str {
    match code {
        0 => "unspecified",
        1 => "karaoke",
        2 => "surround",
        _ => "reserved",
    }
}

/// Decode `audio_attr_t::lang_extension` (8-bit field) — the audio role.
#[must_use]
pub fn audio_lang_extension(code: u8) -> &'static str {
    match code {
        0 => "not specified",
        1 => "normal",
        2 => "visually-impaired",
        3 => "director's comments 1",
        4 => "director's comments 2",
        _ => "reserved",
    }
}

/// Decode `subp_attr_t::lang_extension` (8-bit field).
#[must_use]
pub fn subp_lang_extension(code: u8) -> &'static str {
    match code {
        0 => "not specified",
        1 => "caption (normal size)",
        2 => "caption (bigger size)",
        3 => "caption (children)",
        5 => "closed captions (normal)",
        6 => "closed captions (bigger)",
        7 => "closed captions (children)",
        9 => "forced caption",
        13 => "director's comments (normal)",
        14 => "director's comments (bigger)",
        15 => "director's comments (children)",
        _ => "reserved",
    }
}

/// Decode a packed 2-character ISO-639 language code from a `u16` (the
/// representation libdvdread uses in `audio_attr_t::lang_code` and
/// `subp_attr_t::lang_code`). Returns `None` if either byte is not an
/// ASCII letter (i.e. the slot was zeroed-out or contains garbage).
#[must_use]
pub fn lang_code(code: u16) -> Option<String> {
    let hi = (code >> 8) as u8;
    let lo = (code & 0xFF) as u8;
    if hi.is_ascii_alphabetic() && lo.is_ascii_alphabetic() {
        Some(format!("{}{}", hi as char, lo as char))
    } else {
        None
    }
}

/// The MPEG-PS stream identifier used to carry a DVD audio stream.
///
/// Returns `(main_stream_id, optional_substream_id)`:
///
/// * MPEG audio (formats 2 and 3) rides on its own MPEG-1 audio stream
///   IDs `0xC0..0xC7`; no substream.
/// * AC-3 / DTS / LPCM ride on the MPEG-2 PS "private stream 1" carrier
///   (main ID `0xBD`) and are demuxed via a substream byte in the PES
///   payload:
///   - AC-3:   `0x80 + index`
///   - DTS:    `0x88 + index`
///   - LPCM:   `0xA0 + index`
#[must_use]
pub fn audio_stream_id(audio_format: u8, index: u8) -> Option<(u8, Option<u8>)> {
    match audio_format {
        0 => Some((0xBD, Some(0x80 + index))), // AC-3
        2 | 3 => Some((0xC0 + index, None)),   // MPEG-1 / MPEG-2 ext
        4 => Some((0xBD, Some(0xA0 + index))), // LPCM
        6 => Some((0xBD, Some(0x88 + index))), // DTS
        _ => None,
    }
}

/// The MPEG-PS substream identifier for a DVD subpicture stream — always
/// the "private stream 1" main ID `0xBD` with substream byte `0x20 + index`.
#[must_use]
pub fn subp_stream_id(index: u8) -> (u8, u8) {
    (0xBD, 0x20 + index)
}

/// Decode a PGC `audio_control[i]` u16. Bit 15 is the "stream is used in
/// this PGC" flag; if unset, the slot is inactive. When set, bits 14..12
/// carry the logical stream number (which indexes into `vts_audio_attr[]`).
#[must_use]
pub const fn pgc_audio_control_available(control: u16) -> bool {
    (control & 0x8000) != 0
}

#[must_use]
pub const fn pgc_audio_control_stream_number(control: u16) -> u8 {
    ((control >> 8) & 0x07) as u8
}

/// Decode a PGC `subp_control[i]` u32. Bit 31 indicates the slot is used.
/// For 16:9 discs the other bytes give the *logical* stream number to use
/// in each of the four display modes (4:3 / wide / letterbox / pan&scan);
/// for 4:3 discs they're all the same.
#[must_use]
pub const fn pgc_subp_control_available(control: u32) -> bool {
    (control & 0x8000_0000) != 0
}

#[must_use]
pub const fn pgc_subp_control_stream_4_3(control: u32) -> u8 {
    ((control >> 24) & 0x1F) as u8
}

#[must_use]
pub const fn pgc_subp_control_stream_wide(control: u32) -> u8 {
    ((control >> 16) & 0x1F) as u8
}

#[must_use]
pub const fn pgc_subp_control_stream_letterbox(control: u32) -> u8 {
    ((control >> 8) & 0x1F) as u8
}

#[must_use]
pub const fn pgc_subp_control_stream_pan_scan(control: u32) -> u8 {
    (control & 0x1F) as u8
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lang_code_decodes_ascii_pairs() {
        // 'e' = 0x65, 'n' = 0x6e
        assert_eq!(lang_code(0x656e).as_deref(), Some("en"));
        assert_eq!(lang_code(0x6a61).as_deref(), Some("ja"));
        assert_eq!(lang_code(0x0000), None);
        assert_eq!(lang_code(0xffff), None);
    }

    #[test]
    fn audio_stream_id_examples() {
        // AC-3 stream 0 -> private stream 1 (0xBD), substream 0x80.
        assert_eq!(audio_stream_id(0, 0), Some((0xBD, Some(0x80))));
        // AC-3 stream 5 -> 0xBD / 0x85.
        assert_eq!(audio_stream_id(0, 5), Some((0xBD, Some(0x85))));
        // MPEG-1 audio stream 0 -> direct ID 0xC0.
        assert_eq!(audio_stream_id(2, 0), Some((0xC0, None)));
        // DTS stream 1 -> 0xBD / 0x89.
        assert_eq!(audio_stream_id(6, 1), Some((0xBD, Some(0x89))));
        // LPCM stream 2 -> 0xBD / 0xA2.
        assert_eq!(audio_stream_id(4, 2), Some((0xBD, Some(0xA2))));
        // Reserved.
        assert_eq!(audio_stream_id(7, 0), None);
    }

    #[test]
    fn subp_stream_id_examples() {
        assert_eq!(subp_stream_id(0), (0xBD, 0x20));
        assert_eq!(subp_stream_id(31), (0xBD, 0x3F));
    }

    #[test]
    fn picture_size_decodes() {
        assert_eq!(video_picture_size(0, 0), (720, 480)); // NTSC full
        assert_eq!(video_picture_size(0, 1), (720, 576)); // PAL full
        assert_eq!(video_picture_size(2, 0), (352, 480)); // NTSC half-h
        assert_eq!(video_picture_size(3, 1), (352, 288)); // PAL half-both
    }

    #[test]
    fn pgc_control_bit_decoding() {
        // Bit 15 set, stream number = 3 in bits 14..12.
        assert!(pgc_audio_control_available(0x8300));
        assert_eq!(pgc_audio_control_stream_number(0x8300), 3);
        // Bit 31 set, stream number 2 / 4 / 6 / 8 across the four bytes.
        let s = 0x82_04_06_08_u32;
        assert!(pgc_subp_control_available(s));
        assert_eq!(pgc_subp_control_stream_4_3(s), 2);
        assert_eq!(pgc_subp_control_stream_wide(s), 4);
        assert_eq!(pgc_subp_control_stream_letterbox(s), 6);
        assert_eq!(pgc_subp_control_stream_pan_scan(s), 8);
    }
}
