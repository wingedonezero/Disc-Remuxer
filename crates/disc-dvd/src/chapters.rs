//! Chapter timing extraction + MKV-XML emission.
//!
//! DVD chapters live in the PGC's PTT (Part Of Title) table — each
//! `ptt_info_t` entry points to a `(pgcn, pgn)` pair, and the PGC's
//! `program_map` resolves the program number to the starting cell
//! within `cell_playback`. The chapter's start time is the sum of the
//! playback times of all cells preceding the start cell.
//!
//! ## Output format
//!
//! MKV chapter XML (matroska's `<Chapters>` element), formatted to
//! match the shape of MakeMKV's `*_chapters.xml` output. Chapter UIDs
//! are deterministic but distinct per-chapter (derived from a stable
//! seed so re-running our rip yields identical XML).
//!
//! ## Frame-rate handling
//!
//! Cell `playback_time` is BCD-encoded `HH:MM:SS.FF` where `FF` is the
//! frame index (0-based, 0..=29 for NTSC, 0..=24 for PAL). The top
//! two bits of `frame_u` encode the framerate:
//!
//! | top 2 bits | framerate |
//! |---|---|
//! | `01`       | 25 fps (PAL) |
//! | `11`       | 30000/1001 fps (NTSC) |
//! | `00`/`10`  | reserved / not specified |
//!
//! We use these to convert FF into nanoseconds precisely.

use std::io::{self, Write};

use crate::ifo::{pgc_t, ptt_info_t};

/// Compute the cumulative cell-end times in nanoseconds for every cell
/// in the PGC. `result[i]` is the time at which cell `i` ends (= time
/// at which cell `i+1` starts). `result[0]` is the duration of cell 0.
#[must_use]
pub fn cumulative_cell_end_nanos(pgc: &pgc_t) -> Vec<u64> {
    let nr = { pgc.nr_of_cells };
    let cp_ptr = { pgc.cell_playback };
    if cp_ptr.is_null() || nr == 0 {
        return Vec::new();
    }
    // SAFETY: libdvdread guarantees cell_playback[0..nr] is initialized
    // when the pointer is non-null.
    let cells: &[libdvdread_sys::cell_playback_t] =
        unsafe { std::slice::from_raw_parts(cp_ptr, usize::from(nr)) };
    let mut acc: u64 = 0;
    let mut out = Vec::with_capacity(cells.len());
    for c in cells {
        let pt = { c.playback_time };
        acc += dvd_time_to_nanos(&pt);
        out.push(acc);
    }
    out
}

/// Convert one `dvd_time_t` (BCD H/M/S + frame index with framerate
/// flag) into a duration in nanoseconds.
#[must_use]
pub fn dvd_time_to_nanos(t: &libdvdread_sys::dvd_time_t) -> u64 {
    let h = bcd_to_u8(t.hour) as u64;
    let m = bcd_to_u8(t.minute) as u64;
    let s = bcd_to_u8(t.second) as u64;
    let frame_byte = t.frame_u;
    let frames = u64::from(frame_byte & 0x3F);
    let framerate_code = (frame_byte >> 6) & 0b11;
    // Nanoseconds per frame:
    //   NTSC: 1001 / 30000 sec = 1001 * 1_000_000_000 / 30000 ns
    //                          = 33_366_666 + 2/3 ns  (use exact ratio)
    //   PAL:  1/25 sec = 40_000_000 ns
    let frame_ns = match framerate_code {
        0b01 => frames * 40_000_000,
        0b11 => frames * 1001 * 1_000_000_000 / 30000,
        _ => 0, // reserved / unspecified — frames contribute 0
    };
    (h * 3600 + m * 60 + s) * 1_000_000_000 + frame_ns
}

const fn bcd_to_u8(bcd: u8) -> u8 {
    ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F)
}

/// Resolve a 1-based program number to the 1-based cell number that
/// it starts at via `pgc.program_map`. Returns `None` if `pgn` is out
/// of range or the `program_map` pointer is NULL.
#[must_use]
pub fn program_start_cell(pgc: &pgc_t, pgn: u16) -> Option<u8> {
    let nr_programs = { pgc.nr_of_programs };
    if pgn == 0 || pgn > u16::from(nr_programs) {
        return None;
    }
    let pm_ptr = { pgc.program_map };
    if pm_ptr.is_null() {
        return None;
    }
    // SAFETY: nr_of_programs entries are initialized when pm_ptr is
    // non-null.
    let pm: &[u8] =
        unsafe { std::slice::from_raw_parts(pm_ptr, usize::from(nr_programs)) };
    pm.get(usize::from(pgn) - 1).copied()
}

/// Compute the start time (nanoseconds) of each chapter in the title.
///
/// For chapter `i`, the start time is the sum of all cell durations
/// that precede `program_map[chapter[i].pgn - 1]`. Chapter 0's start
/// is always 0. Chapter `chapters.len()`'s start = PGC total duration.
#[must_use]
pub fn chapter_start_nanos(pgc: &pgc_t, chapters: &[ptt_info_t]) -> Vec<u64> {
    let cum_ends = cumulative_cell_end_nanos(pgc);
    let mut out = Vec::with_capacity(chapters.len());
    for c in chapters {
        let pgn = { c.pgn };
        let Some(start_cell_1b) = program_start_cell(pgc, pgn) else {
            // Malformed chapter — emit 0 so XML still parses.
            out.push(0);
            continue;
        };
        // start_cell_1b is the 1-based cell number. Its START time is
        // the END time of the PREVIOUS cell (0-based index
        // start_cell_1b - 2), or 0 if it's cell 1.
        let cell_idx_0b = usize::from(start_cell_1b).saturating_sub(1);
        let start_ns = if cell_idx_0b == 0 {
            0
        } else {
            *cum_ends.get(cell_idx_0b - 1).unwrap_or(&0)
        };
        out.push(start_ns);
    }
    out
}

/// Format `nanos` as `HH:MM:SS.NNNNNNNNN`.
#[must_use]
pub fn format_timecode(nanos: u64) -> String {
    let total_s = nanos / 1_000_000_000;
    let nanos_in_s = nanos % 1_000_000_000;
    let h = total_s / 3600;
    let m = (total_s % 3600) / 60;
    let s = total_s % 60;
    format!("{h:02}:{m:02}:{s:02}.{nanos_in_s:09}")
}

/// Deterministic 64-bit ID derived from a small seed. Used for
/// ChapterUID / EditionUID so two rips of the same title produce
/// byte-identical chapter XML. The hash is splitmix64 — fast, no
/// dependency, well-distributed.
#[must_use]
pub fn stable_uid(seed: u64) -> u64 {
    let mut z = seed.wrapping_add(0x9E37_79B9_7F4A_7C15);
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

/// Write the chapter XML for one title.
///
/// `title_number` is the 1-based libdvdnav title number (used as the
/// UID seed so the output is stable across reruns).
/// `language` is a 3-letter BCP-47 code, default "eng".
pub fn write_chapters_xml<W: Write>(
    w: &mut W,
    pgc: &pgc_t,
    chapters: &[ptt_info_t],
    title_number: u32,
    language: &str,
) -> io::Result<()> {
    let starts = chapter_start_nanos(pgc, chapters);
    let cum_ends = cumulative_cell_end_nanos(pgc);
    let total_ns = cum_ends.last().copied().unwrap_or(0);

    // UTF-8 BOM is what MakeMKV writes; keep it for visual parity.
    w.write_all("\u{FEFF}".as_bytes())?;
    writeln!(w, "<?xml version=\"1.0\"?>")?;
    writeln!(w, "<!-- <!DOCTYPE Chapters SYSTEM \"matroskachapters.dtd\"> -->")?;
    writeln!(w, "<Chapters>")?;
    writeln!(w, "  <EditionEntry>")?;
    writeln!(w, "    <EditionFlagHidden>0</EditionFlagHidden>")?;
    writeln!(w, "    <EditionFlagDefault>1</EditionFlagDefault>")?;
    let edition_uid = stable_uid(u64::from(title_number) * 0x1_0000);
    writeln!(w, "    <EditionUID>{edition_uid}</EditionUID>")?;

    for (i, &start_ns) in starts.iter().enumerate() {
        let end_ns = starts.get(i + 1).copied().unwrap_or(total_ns);
        let chapter_uid = stable_uid(u64::from(title_number) * 0x1_0000 + (i as u64) + 1);
        let chapter_num_1b = i + 1;
        writeln!(w, "    <ChapterAtom>")?;
        writeln!(w, "      <ChapterUID>{chapter_uid}</ChapterUID>")?;
        writeln!(
            w,
            "      <ChapterTimeStart>{}</ChapterTimeStart>",
            format_timecode(start_ns)
        )?;
        writeln!(w, "      <ChapterFlagHidden>0</ChapterFlagHidden>")?;
        writeln!(w, "      <ChapterFlagEnabled>1</ChapterFlagEnabled>")?;
        writeln!(
            w,
            "      <ChapterTimeEnd>{}</ChapterTimeEnd>",
            format_timecode(end_ns)
        )?;
        writeln!(w, "      <ChapterDisplay>")?;
        writeln!(
            w,
            "        <ChapterString>Chapter {chapter_num_1b:02}</ChapterString>"
        )?;
        writeln!(
            w,
            "        <ChapterLanguage>{language}</ChapterLanguage>"
        )?;
        writeln!(w, "      </ChapterDisplay>")?;
        writeln!(w, "    </ChapterAtom>")?;
    }

    writeln!(w, "  </EditionEntry>")?;
    writeln!(w, "</Chapters>")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dvd_time_zero_is_zero() {
        let t = libdvdread_sys::dvd_time_t {
            hour: 0,
            minute: 0,
            second: 0,
            frame_u: 0,
        };
        assert_eq!(dvd_time_to_nanos(&t), 0);
    }

    #[test]
    fn dvd_time_ntsc_frame_arithmetic() {
        // 15 frames at NTSC = 15 * 1001 / 30000 sec = 0.5005 sec
        let t = libdvdread_sys::dvd_time_t {
            hour: 0,
            minute: 0,
            second: 0,
            frame_u: 15 | (0b11 << 6),
        };
        assert_eq!(dvd_time_to_nanos(&t), 15 * 1001 * 1_000_000_000 / 30000);
    }

    #[test]
    fn dvd_time_pal_frame_arithmetic() {
        // 5 frames at PAL = 5 * 40ms = 200_000_000 ns
        let t = libdvdread_sys::dvd_time_t {
            hour: 0,
            minute: 0,
            second: 0,
            frame_u: 5 | (0b01 << 6),
        };
        assert_eq!(dvd_time_to_nanos(&t), 200_000_000);
    }

    #[test]
    fn format_timecode_examples() {
        assert_eq!(format_timecode(0), "00:00:00.000000000");
        assert_eq!(format_timecode(1_000_000_000), "00:00:01.000000000");
        assert_eq!(
            format_timecode(3 * 3600 * 1_000_000_000 + 4 * 60 * 1_000_000_000 + 5 * 1_000_000_000 + 123_456_789),
            "03:04:05.123456789"
        );
    }

    #[test]
    fn stable_uid_is_deterministic_and_distinct() {
        assert_eq!(stable_uid(1), stable_uid(1));
        assert_ne!(stable_uid(1), stable_uid(2));
        assert_ne!(stable_uid(0), 0);
    }
}
