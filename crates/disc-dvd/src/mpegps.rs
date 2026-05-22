//! MPEG-2 Program Stream pack + PES parser, scoped to DVD-Video.
//!
//! A DVD VOB is a sequence of 2048-byte sectors, each containing exactly
//! one MPEG-PS pack (ISO/IEC 13818-1 §2.5). A pack consists of:
//!
//! ```text
//!   +----------------+-----------------+--------------------+----------+
//!   | Pack header    | System header   | PES packet(s)      | Padding  |
//!   | (14 bytes +    | (optional, on   | one or more, each  | (0..N    |
//!   |  0..7 stuffing)| VOBU starts)    | 00 00 01 <stream>  | bytes)   |
//!   +----------------+-----------------+--------------------+----------+
//!     starts with                       PES = Packetized
//!     00 00 01 BA                       Elementary Stream
//! ```
//!
//! DVD-Video stream-id usage (from the DVD-Video spec / ECMA-130):
//!
//! | stream_id   | meaning |
//! |-------------|--------|
//! | `0xBA`      | pack header (the start code itself) |
//! | `0xBB`      | system header (DVD: only on VOBU start) |
//! | `0xBC`      | program_stream_map (not used on DVD-Video) |
//! | `0xBD`      | private_stream_1 — audio (AC3/DTS/LPCM) + subpictures, substream ID in payload[0] |
//! | `0xBE`      | padding (filler to make the sector reach 2048 bytes) |
//! | `0xBF`      | private_stream_2 — NV_PCK navigation pack, stripped during demux |
//! | `0xC0–0xDF` | MPEG-1/2 audio (rare on DVD; most discs use AC3 in 0xBD) |
//! | `0xE0`      | MPEG-2 video (DVD permits only `0xE0`) |
//!
//! For `0xBD` (private_stream_1), DVD packs the substream identifier in
//! the first byte of the PES payload:
//!
//! | substream_id range | meaning |
//! |--------------------|---------|
//! | `0x20..=0x3F`     | subpicture stream 0..31 |
//! | `0x80..=0x87`     | AC-3 audio stream 0..7 |
//! | `0x88..=0x8F`     | DTS audio stream 0..7 |
//! | `0xA0..=0xA7`     | LPCM audio stream 0..7 |
//!
//! This module gives us:
//! * [`PackHeader::parse`] — decode the 14-byte (+ stuffing) pack header.
//! * [`pes_iter`] — walk PES packets within a sector body, yielding
//!   `Result<PesPacket, MpegPsError>` one at a time.
//! * [`scan_sector`] — convenience over the above for a whole 2048-byte
//!   sector; returns the pack header, the list of PES packets, and any
//!   trailing padding count.
//! * [`stream_kind`] — classify a `(stream_id, Option<substream_id>)`
//!   pair into [`StreamKind`] for the demuxer / scan tool.
//!
//! Per-pack invariants checked by [`scan_sector`]:
//!   - pack-start magic (`00 00 01 BA`) at offset 0
//!   - all four marker bits in the SCR field are 1
//!   - both mux-rate marker bits are 1
//!   - PES packets together with header + padding sum to exactly 2048 bytes
//!
//! Each invariant logs PASS/FAIL through `disc_core::check` so the job
//! log records the result; the parser still returns a structured
//! [`MpegPsError`] for hard failures (anything that prevents further
//! walking, like a missing pack-start code).

use disc_core::check;
use thiserror::Error;

/// Size of one DVD sector — also the size of one MPEG-PS pack on DVD.
pub const SECTOR_SIZE: usize = 2048;

/// Fixed size of the pack header before any stuffing bytes (14 bytes
/// for MPEG-2 program stream).
pub const PACK_HEADER_BASE_SIZE: usize = 14;

// --- stream_id constants (MPEG-2 / DVD-Video) ---

pub const STREAM_ID_PACK_HEADER: u8 = 0xBA;
pub const STREAM_ID_SYSTEM_HEADER: u8 = 0xBB;
pub const STREAM_ID_PROGRAM_STREAM_MAP: u8 = 0xBC;
pub const STREAM_ID_PRIVATE_1: u8 = 0xBD;
pub const STREAM_ID_PADDING: u8 = 0xBE;
pub const STREAM_ID_PRIVATE_2: u8 = 0xBF;

/// `0xE0` is the only video stream-id permitted by the DVD-Video spec
/// (one MPEG-2 video stream per VOB).
pub const STREAM_ID_VIDEO_E0: u8 = 0xE0;

#[derive(Debug, Error)]
pub enum MpegPsError {
    #[error("buffer too short for {what}: need {need} bytes, have {have}")]
    TooShort {
        what: &'static str,
        need: usize,
        have: usize,
    },
    #[error("expected pack-start code 00 00 01 BA, got {actual:02X?}")]
    BadPackStart { actual: [u8; 4] },
    #[error("expected PES start code 00 00 01, got {actual:02X?} at offset {offset}")]
    BadPesStart { actual: [u8; 3], offset: usize },
    #[error("PES packet length {length} exceeds sector remaining {remaining} at offset {offset}")]
    OversizePes {
        length: usize,
        remaining: usize,
        offset: usize,
    },
}

/// Decoded MPEG-2 pack header.
///
/// `scr_base` is in 90 kHz units (system clock reference at video PTS
/// resolution). `scr_ext` is in 27 MHz / 300 units (0..299), so the
/// full 27 MHz reference is `scr_base * 300 + scr_ext`.
///
/// `program_mux_rate` is in units of 50 bytes per second per the
/// MPEG-2 spec.
#[derive(Debug, Clone, Copy)]
pub struct PackHeader {
    pub scr_base: u64,
    pub scr_ext: u16,
    pub program_mux_rate: u32,
    pub stuffing_length: u8,
    /// All four SCR marker bits + both mux_rate marker bits were 1.
    pub markers_ok: bool,
}

impl PackHeader {
    /// Parse the pack header at the start of `buf`. Requires
    /// `buf.len() >= PACK_HEADER_BASE_SIZE + stuffing_length`.
    pub fn parse(buf: &[u8]) -> Result<Self, MpegPsError> {
        if buf.len() < PACK_HEADER_BASE_SIZE {
            return Err(MpegPsError::TooShort {
                what: "pack header",
                need: PACK_HEADER_BASE_SIZE,
                have: buf.len(),
            });
        }
        let head = [buf[0], buf[1], buf[2], buf[3]];
        if head != [0x00, 0x00, 0x01, STREAM_ID_PACK_HEADER] {
            return Err(MpegPsError::BadPackStart { actual: head });
        }

        // SCR + ext + markers (6 bytes, offsets 4..10)
        // Layout (per ISO/IEC 13818-1 Table 2-33):
        //   byte 4:  '01'(2) | SCR[32:30](3) | M(1) | SCR[29:28](2)
        //   byte 5:  SCR[27:20](8)
        //   byte 6:  SCR[19:15](5) | M(1) | SCR[14:13](2)
        //   byte 7:  SCR[12:5](8)
        //   byte 8:  SCR[4:0](5)   | M(1) | SCR_ext[8:7](2)
        //   byte 9:  SCR_ext[6:0](7) | M(1)
        let b4 = u64::from(buf[4]);
        let b5 = u64::from(buf[5]);
        let b6 = u64::from(buf[6]);
        let b7 = u64::from(buf[7]);
        let b8 = u64::from(buf[8]);
        let b9 = u64::from(buf[9]);

        let scr_32_30 = (b4 >> 3) & 0b111;
        let m1 = (b4 >> 2) & 0b1;
        let scr_29_28 = b4 & 0b11;
        let scr_27_20 = b5;
        let scr_19_15 = (b6 >> 3) & 0b1_1111;
        let m2 = (b6 >> 2) & 0b1;
        let scr_14_13 = b6 & 0b11;
        let scr_12_5 = b7;
        let scr_4_0 = (b8 >> 3) & 0b1_1111;
        let m3 = (b8 >> 2) & 0b1;
        let scr_ext_8_7 = b8 & 0b11;
        let scr_ext_6_0 = (b9 >> 1) & 0b111_1111;
        let m4 = b9 & 0b1;

        let scr_base = (scr_32_30 << 30)
            | (scr_29_28 << 28)
            | (scr_27_20 << 20)
            | (scr_19_15 << 15)
            | (scr_14_13 << 13)
            | (scr_12_5 << 5)
            | scr_4_0;
        let scr_ext_u64 = (scr_ext_8_7 << 7) | scr_ext_6_0;

        // mux_rate (22 bits in bytes 10..12) + 2 marker bits at the tail
        // of byte 12.
        //   byte 10: rate[21:14](8)
        //   byte 11: rate[13:6](8)
        //   byte 12: rate[5:0](6) | M(1) | M(1)
        let b10 = u32::from(buf[10]);
        let b11 = u32::from(buf[11]);
        let b12 = u32::from(buf[12]);
        let rate_5_0 = (b12 >> 2) & 0b11_1111;
        let m5 = (b12 >> 1) & 0b1;
        let m6 = b12 & 0b1;
        let program_mux_rate = (b10 << 14) | (b11 << 6) | rate_5_0;

        // stuffing_length is the low 3 bits of byte 13.
        let stuffing_length = buf[13] & 0b111;

        let markers_ok =
            m1 == 1 && m2 == 1 && m3 == 1 && m4 == 1 && m5 == 1 && m6 == 1;

        Ok(Self {
            scr_base,
            scr_ext: u16::try_from(scr_ext_u64).unwrap_or(0),
            program_mux_rate,
            stuffing_length,
            markers_ok,
        })
    }

    /// Total on-wire size of this pack header including any stuffing.
    #[must_use]
    pub fn total_size(self) -> usize {
        PACK_HEADER_BASE_SIZE + usize::from(self.stuffing_length)
    }

    /// 27 MHz system clock reference reconstructed from `scr_base * 300
    /// + scr_ext`. Convenient for cross-stream timing log lines.
    #[must_use]
    pub fn scr_27mhz(self) -> u64 {
        self.scr_base * 300 + u64::from(self.scr_ext)
    }
}

/// A single PES (Packetized Elementary Stream) packet observed inside a
/// pack. `raw` covers the full PES bytes from `00 00 01 <stream_id>`
/// through the end of the packet's declared length; `payload` is the
/// post-PES-header elementary bytes (excluding the substream-ID byte
/// for `0xBD` / `0xBF`).
#[derive(Debug, Clone, Copy)]
pub struct PesPacket<'a> {
    /// `0xBB..=0xEF` per MPEG-PS.
    pub stream_id: u8,
    /// First byte of the PES payload for `private_stream_1` (`0xBD`)
    /// and `private_stream_2` (`0xBF`); `None` for any other stream_id.
    pub substream_id: Option<u8>,
    /// Byte offset of `00 00 01` within the parent sector.
    pub sector_offset: usize,
    /// On-wire size of the PES packet (header + payload + any tail
    /// padding the encoder added — i.e. `6 + PES_packet_length`).
    pub total_size: usize,
    /// Bytes of `raw` consumed by the PES header (stream_id, length,
    /// flags, header-data-length, optional PTS/DTS, substream byte for
    /// `0xBD`/`0xBF`). Payload begins at `raw[header_size..]`.
    pub header_size: usize,
    pub raw: &'a [u8],
    pub payload: &'a [u8],
}

/// Classification of a `(stream_id, Option<substream_id>)` pair into
/// the elementary-stream categories the DVD-Video spec defines.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum StreamKind {
    /// `0xE0..=0xEF` MPEG-2 video; DVD permits only `0xE0`.
    Video(u8),
    /// `0xC0..=0xDF` MPEG-1/2 audio (rare on DVD).
    MpegAudio(u8),
    /// `0xBD` / `0x80..=0x87` AC-3 audio, low 3 bits = stream number.
    Ac3(u8),
    /// `0xBD` / `0x88..=0x8F` DTS audio.
    Dts(u8),
    /// `0xBD` / `0xA0..=0xA7` LPCM audio.
    Lpcm(u8),
    /// `0xBD` / `0x20..=0x3F` DVD subpicture.
    Subpicture(u8),
    /// `0xBB` System header.
    SystemHeader,
    /// `0xBE` Padding stream.
    Padding,
    /// `0xBF` Private stream 2 — DVD NV_PCK.
    NavPack,
    /// Anything we don't recognize, with both ids preserved for diag.
    Unknown { stream_id: u8, substream_id: Option<u8> },
}

impl StreamKind {
    /// Human-readable label for log lines and reports.
    #[must_use]
    pub fn label(self) -> String {
        match self {
            Self::Video(id) => format!("video MPEG-2 stream 0x{id:02X}"),
            Self::MpegAudio(id) => format!("MPEG audio stream 0x{id:02X}"),
            Self::Ac3(n) => format!("AC-3 audio stream {n}"),
            Self::Dts(n) => format!("DTS audio stream {n}"),
            Self::Lpcm(n) => format!("LPCM audio stream {n}"),
            Self::Subpicture(n) => format!("subpicture stream {n}"),
            Self::SystemHeader => "system header".into(),
            Self::Padding => "padding".into(),
            Self::NavPack => "NV_PCK (navigation)".into(),
            Self::Unknown {
                stream_id,
                substream_id,
            } => match substream_id {
                Some(sub) => format!("unknown stream 0x{stream_id:02X}/0x{sub:02X}"),
                None => format!("unknown stream 0x{stream_id:02X}"),
            },
        }
    }

    /// True if this packet kind carries elementary-stream bytes the
    /// demuxer should route to an output file. False for navigation,
    /// padding, and system-header packets which the demuxer drops.
    #[must_use]
    pub fn is_elementary_data(self) -> bool {
        matches!(
            self,
            Self::Video(_)
                | Self::MpegAudio(_)
                | Self::Ac3(_)
                | Self::Dts(_)
                | Self::Lpcm(_)
                | Self::Subpicture(_)
        )
    }
}

/// Classify a PES `(stream_id, optional substream_id)` pair.
#[must_use]
pub fn stream_kind(stream_id: u8, substream_id: Option<u8>) -> StreamKind {
    match stream_id {
        STREAM_ID_SYSTEM_HEADER => StreamKind::SystemHeader,
        STREAM_ID_PADDING => StreamKind::Padding,
        STREAM_ID_PRIVATE_2 => StreamKind::NavPack,
        STREAM_ID_PRIVATE_1 => match substream_id {
            Some(s) if (0x20..=0x3F).contains(&s) => StreamKind::Subpicture(s - 0x20),
            Some(s) if (0x80..=0x87).contains(&s) => StreamKind::Ac3(s - 0x80),
            Some(s) if (0x88..=0x8F).contains(&s) => StreamKind::Dts(s - 0x88),
            Some(s) if (0xA0..=0xA7).contains(&s) => StreamKind::Lpcm(s - 0xA0),
            other => StreamKind::Unknown {
                stream_id,
                substream_id: other,
            },
        },
        0xC0..=0xDF => StreamKind::MpegAudio(stream_id),
        0xE0..=0xEF => StreamKind::Video(stream_id),
        _ => StreamKind::Unknown {
            stream_id,
            substream_id,
        },
    }
}

/// Result of [`scan_sector`].
#[derive(Debug)]
pub struct SectorContents<'a> {
    pub pack_header: PackHeader,
    pub pes_packets: Vec<PesPacket<'a>>,
    /// On a typical DVD sector this is 0 (the encoder uses a padding
    /// PES packet, accounted for inside `pes_packets`, to absorb
    /// slack). Non-zero means the encoder left trailing bytes the
    /// parser didn't classify — flagged by a check.
    pub trailing_unknown_bytes: usize,
}

/// Parse one 2048-byte DVD sector: pack header + PES walk + invariant
/// checks. Returns a [`SectorContents`] borrowing into the input slice.
///
/// `sector_label` is a caller-supplied identifier (e.g.
/// `"sector 12345"`) used only in the PASS/FAIL log lines.
pub fn scan_sector<'a>(
    sector: &'a [u8],
    sector_label: &str,
) -> Result<SectorContents<'a>, MpegPsError> {
    if sector.len() < SECTOR_SIZE {
        return Err(MpegPsError::TooShort {
            what: "DVD sector",
            need: SECTOR_SIZE,
            have: sector.len(),
        });
    }
    let buf = &sector[..SECTOR_SIZE];

    let pack = PackHeader::parse(buf)?;
    check(
        &format!("{sector_label}: pack-header marker bits"),
        "all four SCR markers + both mux-rate markers are 1",
        || pack.markers_ok,
    );

    let mut offset = pack.total_size();
    let mut pes_packets = Vec::with_capacity(4);
    while offset + 6 <= SECTOR_SIZE {
        // PES packet starts at `00 00 01`. If we don't see that, the
        // rest of the sector is post-PES padding the encoder left
        // unaccounted for (rare on real DVDs); stop walking.
        if buf[offset] != 0 || buf[offset + 1] != 0 || buf[offset + 2] != 1 {
            break;
        }
        let pes = parse_pes(buf, offset)?;
        offset += pes.total_size;
        pes_packets.push(pes);
    }

    let trailing_unknown_bytes = SECTOR_SIZE.saturating_sub(offset);
    check(
        &format!("{sector_label}: sector fully accounted for"),
        "header + PES packets sum to 2048 with no trailing bytes",
        || trailing_unknown_bytes == 0,
    );

    Ok(SectorContents {
        pack_header: pack,
        pes_packets,
        trailing_unknown_bytes,
    })
}

/// Parse one PES packet starting at `buf[offset]`. Does NOT validate
/// markers inside PTS/DTS optional fields (that's a step-5 demuxer
/// concern when we actually decode timing).
fn parse_pes(buf: &[u8], offset: usize) -> Result<PesPacket<'_>, MpegPsError> {
    if offset + 6 > buf.len() {
        return Err(MpegPsError::TooShort {
            what: "PES header (6 bytes)",
            need: 6,
            have: buf.len() - offset,
        });
    }
    let start = [buf[offset], buf[offset + 1], buf[offset + 2]];
    if start != [0x00, 0x00, 0x01] {
        return Err(MpegPsError::BadPesStart {
            actual: start,
            offset,
        });
    }
    let stream_id = buf[offset + 3];
    let packet_length =
        (usize::from(buf[offset + 4]) << 8) | usize::from(buf[offset + 5]);
    let total_size = 6 + packet_length;
    let remaining = buf.len() - offset;
    if total_size > remaining {
        return Err(MpegPsError::OversizePes {
            length: packet_length,
            remaining,
            offset,
        });
    }
    let raw = &buf[offset..offset + total_size];

    // Header size depends on stream_id:
    // * 0xBB system_header → 6 bytes + variable; we don't fully parse,
    //   just treat the whole packet as opaque header.
    // * 0xBE padding → 6 bytes header + (packet_length) bytes filler.
    // * 0xBC..=0xFF MPEG-1/2: 6-byte PES header + 1 flags-byte + 1
    //   flags-byte + 1 header_data_length + N optional fields, then
    //   payload. For DVD-Video this is the common case.
    // * Less common: legacy MPEG-1 PES with stuffing-FFs before flags
    //   byte. DVD streams shouldn't hit that.
    let (header_size, substream_id) = match stream_id {
        STREAM_ID_SYSTEM_HEADER | STREAM_ID_PADDING | STREAM_ID_PROGRAM_STREAM_MAP => {
            (total_size, None)
        }
        _ => {
            if raw.len() < 9 {
                return Err(MpegPsError::TooShort {
                    what: "extended PES header",
                    need: 9,
                    have: raw.len(),
                });
            }
            let pes_header_data_length = usize::from(raw[8]);
            let mut hdr = 9 + pes_header_data_length;
            // For DVD private streams, payload[0] is the substream id.
            let substream_id = if matches!(stream_id, STREAM_ID_PRIVATE_1 | STREAM_ID_PRIVATE_2) {
                if hdr < raw.len() {
                    let id = raw[hdr];
                    hdr += 1;
                    Some(id)
                } else {
                    None
                }
            } else {
                None
            };
            (hdr.min(raw.len()), substream_id)
        }
    };

    let payload = &raw[header_size..];

    Ok(PesPacket {
        stream_id,
        substream_id,
        sector_offset: offset,
        total_size,
        header_size,
        raw,
        payload,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a synthetic pack header with SCR=0, mux_rate=0x1A00 (= 10 Mbps).
    fn pack_header_bytes() -> [u8; 14] {
        let mut b = [0u8; 14];
        b[0] = 0x00;
        b[1] = 0x00;
        b[2] = 0x01;
        b[3] = 0xBA;
        // SCR fields are 0 in this fixture; just set the 4 marker bits
        // (m1@byte4 bit2, m2@byte6 bit2, m3@byte8 bit2, m4@byte9 bit0)
        // and the leading '01' in byte 4.
        b[4] = 0b0100_0100; // '01' + scr=0 + marker=1 + scr=0
        b[6] = 0b0000_0100;
        b[8] = 0b0000_0100;
        b[9] = 0b0000_0001;
        // mux_rate = 0x1A00 (low 22 bits all zero except higher bits) +
        // two marker bits at the bottom of byte 12.
        b[10] = 0x00;
        b[11] = 0x68; // (0x1A00 >> 6) & 0xFF
        b[12] = 0b00_000011; // low 6 bits of rate=0 + two markers
        b[13] = 0; // no stuffing
        b
    }

    #[test]
    fn pack_header_round_trips_markers() {
        let bytes = pack_header_bytes();
        let p = PackHeader::parse(&bytes).expect("parse");
        assert!(p.markers_ok);
        assert_eq!(p.stuffing_length, 0);
        assert_eq!(p.scr_base, 0);
    }

    #[test]
    fn pack_header_rejects_bad_magic() {
        let mut bytes = pack_header_bytes();
        bytes[3] = 0xBB;
        assert!(matches!(
            PackHeader::parse(&bytes),
            Err(MpegPsError::BadPackStart { .. })
        ));
    }

    #[test]
    fn pack_header_rejects_short_buffer() {
        let buf = [0u8; 10];
        assert!(matches!(
            PackHeader::parse(&buf),
            Err(MpegPsError::TooShort { .. })
        ));
    }

    #[test]
    fn stream_kind_classifies_dvd_substreams() {
        // AC3 stream 0
        assert_eq!(
            stream_kind(STREAM_ID_PRIVATE_1, Some(0x80)),
            StreamKind::Ac3(0)
        );
        // DTS stream 1
        assert_eq!(
            stream_kind(STREAM_ID_PRIVATE_1, Some(0x89)),
            StreamKind::Dts(1)
        );
        // LPCM stream 2
        assert_eq!(
            stream_kind(STREAM_ID_PRIVATE_1, Some(0xA2)),
            StreamKind::Lpcm(2)
        );
        // Subpicture 5
        assert_eq!(
            stream_kind(STREAM_ID_PRIVATE_1, Some(0x25)),
            StreamKind::Subpicture(5)
        );
        // NV_PCK
        assert_eq!(stream_kind(STREAM_ID_PRIVATE_2, Some(0x00)), StreamKind::NavPack);
        // MPEG-2 video E0
        assert_eq!(stream_kind(0xE0, None), StreamKind::Video(0xE0));
        // Padding
        assert_eq!(stream_kind(STREAM_ID_PADDING, None), StreamKind::Padding);
        // System header
        assert_eq!(stream_kind(STREAM_ID_SYSTEM_HEADER, None), StreamKind::SystemHeader);
    }

    #[test]
    fn stream_kind_unknown_preserves_ids() {
        let k = stream_kind(0xC0, None);
        assert_eq!(k, StreamKind::MpegAudio(0xC0));
        let k = stream_kind(0xBD, Some(0x10)); // unknown private substream
        assert_eq!(
            k,
            StreamKind::Unknown {
                stream_id: 0xBD,
                substream_id: Some(0x10)
            }
        );
    }

    #[test]
    fn is_elementary_data_flags_correctly() {
        assert!(StreamKind::Video(0xE0).is_elementary_data());
        assert!(StreamKind::Ac3(0).is_elementary_data());
        assert!(StreamKind::Subpicture(0).is_elementary_data());
        assert!(!StreamKind::NavPack.is_elementary_data());
        assert!(!StreamKind::Padding.is_elementary_data());
        assert!(!StreamKind::SystemHeader.is_elementary_data());
    }

    fn build_sector_with_padding(payload_size: usize) -> Vec<u8> {
        let mut sector = vec![0u8; SECTOR_SIZE];
        sector[..14].copy_from_slice(&pack_header_bytes());
        // Padding PES at offset 14: 00 00 01 BE + length (big-endian) +
        // 0xFF bytes filling to the declared length.
        sector[14] = 0x00;
        sector[15] = 0x00;
        sector[16] = 0x01;
        sector[17] = STREAM_ID_PADDING;
        let len = SECTOR_SIZE - 14 - 6;
        sector[18] = (len >> 8) as u8;
        sector[19] = (len & 0xFF) as u8;
        for b in sector[20..20 + payload_size].iter_mut() {
            *b = 0xFF;
        }
        sector
    }

    #[test]
    fn scan_sector_full_padding_packet() {
        let sector = build_sector_with_padding(SECTOR_SIZE - 14 - 6);
        let contents = scan_sector(&sector, "test").expect("scan");
        assert_eq!(contents.pes_packets.len(), 1);
        let p = &contents.pes_packets[0];
        assert_eq!(p.stream_id, STREAM_ID_PADDING);
        assert_eq!(p.sector_offset, 14);
        assert_eq!(p.total_size, SECTOR_SIZE - 14);
        assert_eq!(contents.trailing_unknown_bytes, 0);
    }

    #[test]
    fn scan_sector_rejects_short_input() {
        let short = vec![0u8; 1024];
        assert!(matches!(
            scan_sector(&short, "x"),
            Err(MpegPsError::TooShort { .. })
        ));
    }

    #[test]
    fn pes_oversize_packet_errors() {
        // Build a "PES" packet whose declared length runs past the
        // sector. We craft just enough to trigger the size check.
        let mut sector = vec![0u8; SECTOR_SIZE];
        sector[..14].copy_from_slice(&pack_header_bytes());
        sector[14] = 0x00;
        sector[15] = 0x00;
        sector[16] = 0x01;
        sector[17] = STREAM_ID_PRIVATE_1;
        sector[18] = 0xFF; // length = 0xFFFF, way past sector
        sector[19] = 0xFF;
        let err = scan_sector(&sector, "x").expect_err("should fail");
        assert!(matches!(err, MpegPsError::OversizePes { .. }));
    }
}
