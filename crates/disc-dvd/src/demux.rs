//! MPEG-PS stream demultiplexer for DVD-Video — step 5a skeleton.
//!
//! Takes parsed [`PesPacket`]s from [`crate::mpegps`] and routes each
//! one's elementary bytes to a per-stream output file. Strips the
//! correct amount of DVD-specific framing per stream type:
//!
//! | stream                                  | bytes stripped beyond PES header |
//! |-----------------------------------------|----------------------------------|
//! | MPEG-2 video (`0xE0..=0xEF`)             | none — payload is raw MPEG-2 ES |
//! | MPEG audio (`0xC0..=0xDF`)               | none — payload is MPEG-1/2 audio |
//! | AC-3 (`0xBD` + substream `0x80..=0x87`)  | 3 (BD common: num_frames + first_access_unit_pointer) |
//! | DTS (`0xBD` + substream `0x88..=0x8F`)   | 3 (same BD common header) |
//! | LPCM (`0xBD` + substream `0xA0..=0xA7`)  | 6 (BD common + 3-byte LPCM frame header) |
//! | Subpicture (`0xBD` + substream `0x20..=0x3F`) | 0 — SPU payload starts immediately |
//!
//! Dropped (not emitted):
//!
//! | stream                            | reason |
//! |-----------------------------------|--------|
//! | `0xBB` system header              | container metadata |
//! | `0xBE` padding                    | encoder filler |
//! | `0xBF` NV_PCK (private_stream_2)  | DVD navigation, separate file format if we ever need it |
//!
//! What this module does NOT do yet (step 5b will):
//!
//! * Honor `first_access_unit_pointer` to skip partial leading frames
//!   on the first PES of a stream after a discontinuity.
//! * Walk AC-3 / DTS frame sync words to drop partial trailing frames
//!   at cell boundaries when `stc_discontinuity == true`.
//! * Reset per-stream state at cell boundaries.
//!
//! For the common case (single-cell title, or continuous stream
//! flagged `stc_discontinuity == false`), the naive concatenation in
//! this module already produces byte-identical elementary streams,
//! because PES payloads are just the original encoder bytes chunked at
//! arbitrary boundaries — concatenating them undoes the chunking.
//!
//! ## Output filename convention
//!
//! One file per emitted stream, under `out_dir`:
//!
//! ```text
//! out_dir/
//!   video.0xE0.m2v
//!   audio.ac3.0.ac3       (substream 0..=7)
//!   audio.dts.0.dts
//!   audio.lpcm.0.lpcm
//!   audio.mpeg.0xC0.mp2
//!   subpicture.0.sup      (substream 0..=31)
//! ```
//!
//! ## Per-stream sanity ("fields are correct" check)
//!
//! After the first emitted byte of each stream, the module records
//! whether the magic at byte 0 matches the codec the classifier said
//! it was:
//!
//! | stream         | expected first bytes              |
//! |----------------|-----------------------------------|
//! | MPEG-2 video   | `00 00 01 B3` (sequence_header)   |
//! | AC-3           | `0B 77` (syncword)                |
//! | DTS            | `7F FE 80 01` (core syncword)     |
//! | MPEG audio     | `FF Fx`/`FF Ex` (frame sync)      |
//! | LPCM           | no portable magic (raw samples)   |
//! | Subpicture     | SPU header (length + DCSQT offset), validated later |
//!
//! The result is one of [`MagicCheck::Pass`] / [`MagicCheck::Fail`] /
//! [`MagicCheck::Skipped`] in the per-stream stats. PASS/FAIL is also
//! logged through `disc_core::check` so it shows up in job logs.

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::PathBuf;

use disc_core::{check, DiscError};
use thiserror::Error;

use crate::mpegps::{
    scan_sector, stream_kind, MpegPsError, PesPacket, StreamKind, SECTOR_SIZE,
};

#[derive(Debug, Error)]
pub enum DemuxError {
    #[error("mpegps parse error: {0}")]
    Parse(#[from] MpegPsError),
    #[error("I/O error writing {file}: {source}")]
    Io {
        file: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error(transparent)]
    Core(#[from] DiscError),
}

/// Identity of one emitted elementary stream. Used as the key into the
/// demuxer's writer / stats tables. Mirrors [`StreamKind`] but drops
/// variants that don't get emitted (`SystemHeader`, `Padding`, `NavPack`,
/// `Unknown`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum StreamKey {
    /// MPEG-2 video, holds the raw `stream_id` byte (DVD permits only
    /// `0xE0` in practice but the parser accepts `0xE0..=0xEF`).
    Video(u8),
    /// MPEG-1/2 audio (rare on DVD), holds the raw `stream_id` byte.
    MpegAudio(u8),
    /// AC-3 audio. Holds the substream index (0..=7).
    Ac3(u8),
    /// DTS audio. Holds the substream index (0..=7).
    Dts(u8),
    /// LPCM audio. Holds the substream index (0..=7).
    Lpcm(u8),
    /// DVD subpicture. Holds the substream index (0..=31).
    Subpicture(u8),
}

impl StreamKey {
    /// Build the per-stream key from a classified [`StreamKind`].
    /// Returns `None` for kinds that aren't elementary-data streams
    /// (system header, padding, NV_PCK, unknown).
    #[must_use]
    pub fn from_stream_kind(kind: StreamKind) -> Option<Self> {
        match kind {
            StreamKind::Video(id) => Some(Self::Video(id)),
            StreamKind::MpegAudio(id) => Some(Self::MpegAudio(id)),
            StreamKind::Ac3(n) => Some(Self::Ac3(n)),
            StreamKind::Dts(n) => Some(Self::Dts(n)),
            StreamKind::Lpcm(n) => Some(Self::Lpcm(n)),
            StreamKind::Subpicture(n) => Some(Self::Subpicture(n)),
            _ => None,
        }
    }

    /// Filename for this stream under the demux output directory.
    #[must_use]
    pub fn filename(self) -> String {
        match self {
            Self::Video(id) => format!("video.0x{id:02X}.m2v"),
            Self::MpegAudio(id) => format!("audio.mpeg.0x{id:02X}.mp2"),
            Self::Ac3(n) => format!("audio.ac3.{n}.ac3"),
            Self::Dts(n) => format!("audio.dts.{n}.dts"),
            Self::Lpcm(n) => format!("audio.lpcm.{n}.lpcm"),
            Self::Subpicture(n) => format!("subpicture.{n}.sup"),
        }
    }

    /// Human-readable label (matches [`StreamKind::label`] output for
    /// the equivalent variants).
    #[must_use]
    pub fn label(self) -> String {
        match self {
            Self::Video(id) => format!("video MPEG-2 stream 0x{id:02X}"),
            Self::MpegAudio(id) => format!("MPEG audio stream 0x{id:02X}"),
            Self::Ac3(n) => format!("AC-3 audio stream {n}"),
            Self::Dts(n) => format!("DTS audio stream {n}"),
            Self::Lpcm(n) => format!("LPCM audio stream {n}"),
            Self::Subpicture(n) => format!("subpicture stream {n}"),
        }
    }

    /// How many bytes to skip at the start of [`PesPacket::payload`]
    /// before the elementary-stream bytes begin.
    ///
    /// `pes.payload` already excludes the PES header AND the
    /// `substream_id` byte (the parser strips both). What remains
    /// depends on the codec:
    ///
    /// * AC-3 / DTS: 3 bytes (BD common — `num_audio_frames` + 16-bit
    ///   `first_access_unit_pointer`).
    /// * LPCM: 6 bytes (BD common 3 + LPCM-specific 3-byte frame header
    ///   for emphasis / mute / sample-rate / channels / DRC).
    /// * Subpicture: 0 bytes (SPU starts immediately).
    /// * Video / MPEG audio: 0 bytes (not private_stream_1).
    #[must_use]
    pub fn payload_prefix_strip(self) -> usize {
        match self {
            Self::Ac3(_) | Self::Dts(_) => 3,
            Self::Lpcm(_) => 6,
            Self::Subpicture(_) | Self::Video(_) | Self::MpegAudio(_) => 0,
        }
    }
}

/// Result of the first-byte magic check applied to each stream.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum MagicCheck {
    /// First bytes matched the expected codec syncword.
    Pass,
    /// First bytes did NOT match — stream may be misclassified, or the
    /// first PES started mid-frame (a step-5b concern).
    Fail,
    /// Codec has no portable byte-0 syncword we can validate (LPCM,
    /// subpictures).
    Skipped,
    /// Not enough bytes yet to apply the check. Default for fresh
    /// stats.
    #[default]
    Pending,
}

/// Per-stream demux statistics.
#[derive(Debug, Default, Clone)]
pub struct StreamStats {
    /// Number of PES packets routed to this stream.
    pub pes_count: u64,
    /// Total `pes.total_size` (PES header + payload) routed here, in
    /// bytes — used for cross-checking against scan-streams totals.
    pub pes_bytes: u64,
    /// Bytes written to the output file after BD-substream stripping.
    pub emitted_bytes: u64,
    /// First-bytes magic check result for this stream's output.
    pub magic_check: MagicCheck,
    /// First 8 bytes the stream produced (for diagnostics on FAIL).
    pub first_bytes: [u8; 8],
    /// Whether `first_bytes` has been populated.
    pub first_bytes_set: bool,
}

/// Overall summary returned by [`Demuxer::finish`].
#[derive(Debug, Clone)]
pub struct DemuxSummary {
    pub sectors_processed: u64,
    pub pes_total: u64,
    /// Bytes of input consumed across all sectors processed
    /// (`sectors_processed * 2048`).
    pub input_bytes: u64,
    /// Bytes that went into elementary-stream output files (post-
    /// header-stripping).
    pub elementary_emitted_bytes: u64,
    /// Bytes of NV_PCK / padding / system header / unknown that the
    /// demuxer dropped (whole PES total_size).
    pub dropped_pes_bytes: u64,
    /// Total bytes of PES headers and DVD BD-substream headers
    /// stripped. `input - elementary - dropped - pack_headers - strip
    /// = 0` is the accounting invariant.
    pub stripped_header_bytes: u64,
    /// `14 * sectors_processed` — the pack header on every sector.
    pub pack_header_bytes: u64,
    pub streams: BTreeMap<StreamKey, StreamStats>,
}

/// The demultiplexer itself.
pub struct Demuxer {
    out_dir: PathBuf,
    writers: BTreeMap<StreamKey, BufWriter<File>>,
    stats: BTreeMap<StreamKey, StreamStats>,
    sectors_processed: u64,
    pes_total: u64,
    elementary_emitted_bytes: u64,
    dropped_pes_bytes: u64,
    stripped_header_bytes: u64,
}

impl Demuxer {
    /// Create a new demuxer that writes per-stream files into
    /// `out_dir`. The directory must already exist; output files are
    /// created lazily on first packet per stream.
    pub fn new(out_dir: impl Into<PathBuf>) -> Self {
        Self {
            out_dir: out_dir.into(),
            writers: BTreeMap::new(),
            stats: BTreeMap::new(),
            sectors_processed: 0,
            pes_total: 0,
            elementary_emitted_bytes: 0,
            dropped_pes_bytes: 0,
            stripped_header_bytes: 0,
        }
    }

    /// Parse one 2048-byte sector and route its PES packets.
    pub fn process_sector(&mut self, sector: &[u8], label: &str) -> Result<(), DemuxError> {
        let contents = scan_sector(sector, label)?;
        for pes in &contents.pes_packets {
            self.process_pes(pes)?;
        }
        self.sectors_processed += 1;
        Ok(())
    }

    /// Route one already-parsed PES packet through the demuxer.
    pub fn process_pes(&mut self, pes: &PesPacket<'_>) -> Result<(), DemuxError> {
        self.pes_total += 1;
        let kind = stream_kind(pes.stream_id, pes.substream_id);
        let Some(key) = StreamKey::from_stream_kind(kind) else {
            // Dropped: NV_PCK / padding / system header / unknown.
            self.dropped_pes_bytes += pes.total_size as u64;
            return Ok(());
        };

        let prefix = key.payload_prefix_strip();
        // pes.header_size already accounts for PES header + substream_id;
        // anything else we strip (BD common 3 / LPCM extra 3) is part of
        // pes.payload here.
        let header_strip = pes.header_size + prefix;
        self.stripped_header_bytes += header_strip as u64;

        if pes.payload.len() <= prefix {
            // Payload is shorter than the expected BD/LPCM header — log
            // it and skip without writing anything. This is rare and
            // shouldn't happen on real DVD-Video data; per-stream stats
            // still see the PES count.
            log::warn!(
                "demux: PES at sector_offset={} has payload {} bytes but stream {:?} expects {prefix}-byte BD strip",
                pes.sector_offset,
                pes.payload.len(),
                key,
            );
            let stats = self.stats.entry(key).or_default();
            stats.pes_count += 1;
            stats.pes_bytes += pes.total_size as u64;
            return Ok(());
        }

        let bytes_to_emit = &pes.payload[prefix..];

        // Lazy-open the writer.
        let writer = match self.writers.get_mut(&key) {
            Some(w) => w,
            None => {
                let path = self.out_dir.join(key.filename());
                let file = File::create(&path).map_err(|e| DemuxError::Io {
                    file: path.clone(),
                    source: e,
                })?;
                log::info!("demux: opened output {} for {}", path.display(), key.label());
                self.writers.insert(key, BufWriter::with_capacity(64 * 1024, file));
                self.writers.get_mut(&key).expect("just inserted")
            }
        };

        writer.write_all(bytes_to_emit).map_err(|e| DemuxError::Io {
            file: self.out_dir.join(key.filename()),
            source: e,
        })?;

        // Update stats. Magic-check is applied once per stream, on the
        // first emit, against the first 8 bytes (or fewer if the first
        // emit is smaller).
        let stats = self.stats.entry(key).or_default();
        stats.pes_count += 1;
        stats.pes_bytes += pes.total_size as u64;
        stats.emitted_bytes += bytes_to_emit.len() as u64;
        self.elementary_emitted_bytes += bytes_to_emit.len() as u64;

        if !stats.first_bytes_set && !bytes_to_emit.is_empty() {
            let n = bytes_to_emit.len().min(stats.first_bytes.len());
            stats.first_bytes[..n].copy_from_slice(&bytes_to_emit[..n]);
            stats.first_bytes_set = true;
            stats.magic_check = run_magic_check(key, &stats.first_bytes[..n]);
            // Mirror PASS/FAIL into the disc_check log target.
            let label = format!("demux: first-byte magic for {}", key.label());
            match stats.magic_check {
                MagicCheck::Pass => {
                    check(&label, "matches codec syncword", || true);
                }
                MagicCheck::Fail => {
                    check(
                        &label,
                        &format!(
                            "expected codec syncword, got {:02X?}",
                            &stats.first_bytes[..n]
                        ),
                        || false,
                    );
                }
                MagicCheck::Skipped | MagicCheck::Pending => {}
            }
        }
        Ok(())
    }

    /// Flush + close all output files and return summary stats.
    pub fn finish(mut self) -> Result<DemuxSummary, DemuxError> {
        for (key, mut writer) in std::mem::take(&mut self.writers) {
            writer.flush().map_err(|e| DemuxError::Io {
                file: self.out_dir.join(key.filename()),
                source: e,
            })?;
        }
        let pack_header_bytes = self.sectors_processed * 14;
        Ok(DemuxSummary {
            sectors_processed: self.sectors_processed,
            pes_total: self.pes_total,
            input_bytes: self.sectors_processed * SECTOR_SIZE as u64,
            elementary_emitted_bytes: self.elementary_emitted_bytes,
            dropped_pes_bytes: self.dropped_pes_bytes,
            stripped_header_bytes: self.stripped_header_bytes,
            pack_header_bytes,
            streams: self.stats,
        })
    }
}

/// Apply the codec's expected first-byte magic to the captured prefix.
fn run_magic_check(key: StreamKey, first_bytes: &[u8]) -> MagicCheck {
    match key {
        StreamKey::Video(_) => {
            // MPEG-2 sequence_header_code = 0x000001B3
            if first_bytes.len() >= 4
                && first_bytes[..4] == [0x00, 0x00, 0x01, 0xB3]
            {
                MagicCheck::Pass
            } else {
                MagicCheck::Fail
            }
        }
        StreamKey::Ac3(_) => {
            // AC-3 syncword 0x0B77 at start.
            if first_bytes.len() >= 2 && first_bytes[..2] == [0x0B, 0x77] {
                MagicCheck::Pass
            } else {
                MagicCheck::Fail
            }
        }
        StreamKey::Dts(_) => {
            // DTS core syncword 0x7FFE8001.
            if first_bytes.len() >= 4
                && first_bytes[..4] == [0x7F, 0xFE, 0x80, 0x01]
            {
                MagicCheck::Pass
            } else {
                MagicCheck::Fail
            }
        }
        StreamKey::MpegAudio(_) => {
            // MPEG-1/2 audio frame sync is 11 bits of 1: 0xFFE..0xFFF
            // in big-endian.
            if first_bytes.len() >= 2
                && first_bytes[0] == 0xFF
                && (first_bytes[1] & 0xE0) == 0xE0
            {
                MagicCheck::Pass
            } else {
                MagicCheck::Fail
            }
        }
        StreamKey::Lpcm(_) | StreamKey::Subpicture(_) => MagicCheck::Skipped,
    }
}

// Notes for future steps (5b/c):
//
// We do NOT clear `first_bytes_set` on cell boundaries or
// stc_discontinuities yet. Step 5b should:
//   * Reset per-stream state when the upstream caller marks a cell
//     boundary with stc_discontinuity=true on the cell metadata.
//   * Drop the leading partial frame on the first PES after such a
//     reset using `first_access_unit_pointer` from the BD common
//     header (the 2 bytes at offset 1..3 of payload before we strip).
//
// Right now `pes.payload[..3]` for AC-3/DTS contains:
//   [0] = num_audio_frames    (informational; how many AC-3 frames
//                              first-start within this PES)
//   [1] = first_access_unit_pointer hi
//   [2] = first_access_unit_pointer lo  (16-bit BE offset to the
//                              first complete frame, measured from the
//                              END of this 3-byte header — i.e. from
//                              what we currently call payload[3..])
// For LPCM the same 3-byte BD common header is present, then 3 LPCM
// codec bytes (offsets [3]..[5] of payload) before the audio samples.

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mpegps::StreamKind;

    #[test]
    fn stream_key_from_kind_filters_non_elementary() {
        assert_eq!(
            StreamKey::from_stream_kind(StreamKind::Ac3(2)),
            Some(StreamKey::Ac3(2))
        );
        assert_eq!(
            StreamKey::from_stream_kind(StreamKind::Video(0xE0)),
            Some(StreamKey::Video(0xE0))
        );
        assert_eq!(StreamKey::from_stream_kind(StreamKind::NavPack), None);
        assert_eq!(StreamKey::from_stream_kind(StreamKind::Padding), None);
        assert_eq!(StreamKey::from_stream_kind(StreamKind::SystemHeader), None);
        assert_eq!(
            StreamKey::from_stream_kind(StreamKind::Unknown {
                stream_id: 0x99,
                substream_id: None
            }),
            None
        );
    }

    #[test]
    fn stream_key_filenames_are_disjoint() {
        let keys = [
            StreamKey::Video(0xE0),
            StreamKey::MpegAudio(0xC0),
            StreamKey::Ac3(0),
            StreamKey::Ac3(7),
            StreamKey::Dts(0),
            StreamKey::Lpcm(0),
            StreamKey::Subpicture(0),
            StreamKey::Subpicture(31),
        ];
        let mut seen = std::collections::HashSet::new();
        for k in keys {
            assert!(seen.insert(k.filename()), "duplicate filename for {k:?}");
        }
    }

    #[test]
    fn payload_prefix_strip_matches_spec() {
        assert_eq!(StreamKey::Ac3(0).payload_prefix_strip(), 3);
        assert_eq!(StreamKey::Dts(0).payload_prefix_strip(), 3);
        assert_eq!(StreamKey::Lpcm(0).payload_prefix_strip(), 6);
        assert_eq!(StreamKey::Subpicture(0).payload_prefix_strip(), 0);
        assert_eq!(StreamKey::Video(0xE0).payload_prefix_strip(), 0);
        assert_eq!(StreamKey::MpegAudio(0xC0).payload_prefix_strip(), 0);
    }

    #[test]
    fn magic_check_video_sequence_header_passes() {
        assert_eq!(
            run_magic_check(StreamKey::Video(0xE0), &[0x00, 0x00, 0x01, 0xB3, 0xFF]),
            MagicCheck::Pass,
        );
    }

    #[test]
    fn magic_check_video_wrong_magic_fails() {
        assert_eq!(
            run_magic_check(StreamKey::Video(0xE0), &[0xDE, 0xAD, 0xBE, 0xEF]),
            MagicCheck::Fail,
        );
    }

    #[test]
    fn magic_check_ac3_syncword() {
        assert_eq!(
            run_magic_check(StreamKey::Ac3(0), &[0x0B, 0x77, 0x00, 0x00]),
            MagicCheck::Pass,
        );
        assert_eq!(
            run_magic_check(StreamKey::Ac3(0), &[0x0A, 0x77]),
            MagicCheck::Fail,
        );
    }

    #[test]
    fn magic_check_dts_syncword() {
        assert_eq!(
            run_magic_check(StreamKey::Dts(0), &[0x7F, 0xFE, 0x80, 0x01, 0x00]),
            MagicCheck::Pass,
        );
        assert_eq!(
            run_magic_check(StreamKey::Dts(0), &[0x7F, 0xFE, 0x80, 0x00]),
            MagicCheck::Fail,
        );
    }

    #[test]
    fn magic_check_mpeg_audio_frame_sync() {
        // MPEG-1 layer III, valid sync.
        assert_eq!(
            run_magic_check(StreamKey::MpegAudio(0xC0), &[0xFF, 0xFB, 0x90, 0x00]),
            MagicCheck::Pass,
        );
        // No frame sync.
        assert_eq!(
            run_magic_check(StreamKey::MpegAudio(0xC0), &[0xFF, 0x00]),
            MagicCheck::Fail,
        );
    }

    #[test]
    fn magic_check_skipped_for_lpcm_and_subpicture() {
        assert_eq!(
            run_magic_check(StreamKey::Lpcm(0), &[0; 8]),
            MagicCheck::Skipped
        );
        assert_eq!(
            run_magic_check(StreamKey::Subpicture(0), &[0; 8]),
            MagicCheck::Skipped
        );
    }

}
