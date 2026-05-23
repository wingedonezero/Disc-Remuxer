//! VobSub (`.idx` + `.sub`) output for DVD subpicture streams.
//!
//! VobSub is the de-facto subtitle format for DVD rips. The `.sub`
//! file is an MPEG-PS sector stream containing private_stream_1 PES
//! packets that wrap subpicture (SPU) bytes; each subtitle starts at
//! a 2048-byte sector boundary so `.idx` can point at it with a
//! sector-aligned `filepos`. The `.idx` is a small text file with the
//! palette, language, and a `timestamp: ... filepos: ...` line per
//! subtitle.
//!
//! ## What we generate
//!
//! Every input PES becomes one output 2048-byte sector. For each
//! subpicture PES observed during demux:
//!
//! 1. If the PES carries a PTS, it starts a new SPU. Record the new
//!    sector-aligned offset and the PES's PTS in the `.idx` index,
//!    and write a sector with a `private_stream_1` PES whose PES
//!    header carries that PTS.
//! 2. If the PES has no PTS, it's a continuation of the current SPU.
//!    Write a sector with a `private_stream_1` PES whose PES header
//!    carries no PTS, using the previous SPU's PTS as the SCR in the
//!    pack header.
//!
//! Each sector contains, in order:
//!
//! * a 14-byte MPEG-2 pack header (`00 00 01 BA …`) with the SPU's
//!   PTS encoded as SCR (decoders don't validate this for VobSub
//!   playback — but real DVDs always carry one so we do too),
//! * a `private_stream_1` PES (`00 00 01 BD …`) with `packet_length`
//!   sized to fit the SPU payload, substream_id, and the SPU bytes,
//! * a `pad_stream` PES (`00 00 01 BE …`) of `0xFF` bytes sized to
//!   pad the sector to exactly 2048 bytes.
//!
//! Multi-PES SPUs (where the SPU's encoded length is too large for a
//! single sector) on the disc are emitted as one output sector per
//! input PES — the spec is permissive about where the split lands.
//!
//! The `.idx` is written once at the end with the palette and the
//! per-subtitle index.

use std::io::{self, Seek, SeekFrom, Write};

use crate::ifo::dvd_time_t;

/// DVD sector size — also the alignment quantum for VobSub `.sub`
/// entries.
pub const SECTOR_SIZE: u64 = 2048;

/// One subtitle as it lives in the `.idx` file's index.
#[derive(Debug, Clone, Copy)]
pub struct VobSubEntry {
    /// PTS in 90 kHz units (33-bit value).
    pub pts_90khz: u64,
    /// Byte offset of the subtitle's pack header in the `.sub` file.
    /// Always a multiple of [`SECTOR_SIZE`].
    pub filepos: u64,
}

/// Streaming `.sub` writer. Emits exactly one 2048-byte sector per
/// input PES.
pub struct SubWriter<W: Write + Seek> {
    out: W,
    bytes_written: u64,
    pub entries: Vec<VobSubEntry>,
    /// Stream's substream_id byte (0x20..=0x3F).
    substream_id: u8,
    /// PTS of the most recently-started SPU. Used as the SCR encoded
    /// in continuation sectors so all sectors of one SPU share a
    /// consistent time reference.
    current_pts: Option<u64>,
}

/// Maximum SPU payload bytes that fit in one 2048-byte sector when
/// the PES carries a PTS (sector = 14 pack + 15 PES header + N SPU +
/// pad_stream). Computed as `2048 - 14 - 15 = 2019`.
const MAX_SPU_WITH_PTS: usize = 2048 - 14 - 15;
/// Same, for continuation sectors whose PES omits the 5-byte PTS
/// field (`2048 - 14 - 10 = 2024`).
const MAX_SPU_NO_PTS: usize = 2048 - 14 - 10;

impl<W: Write + Seek> SubWriter<W> {
    pub fn new(out: W, substream_id: u8) -> Self {
        Self {
            out,
            bytes_written: 0,
            entries: Vec::new(),
            substream_id,
            current_pts: None,
        }
    }

    /// Start a new SPU. Writes a single 2048-byte sector whose PES
    /// carries the supplied PTS and the first portion of the SPU.
    pub fn write_subtitle(&mut self, pts_90khz: u64, spu: &[u8]) -> io::Result<()> {
        if spu.len() > MAX_SPU_WITH_PTS {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "subpicture PES payload {} bytes exceeds sector capacity {}",
                    spu.len(),
                    MAX_SPU_WITH_PTS
                ),
            ));
        }
        let filepos = self.bytes_written;
        self.entries.push(VobSubEntry {
            pts_90khz,
            filepos,
        });
        self.current_pts = Some(pts_90khz);
        self.write_sector(pts_90khz, Some(pts_90khz), spu)
    }

    /// Continue an SPU started by a prior `write_subtitle` call.
    /// Writes a single 2048-byte sector whose PES carries no PTS and
    /// the next portion of the SPU.
    pub fn write_continuation(&mut self, spu: &[u8]) -> io::Result<()> {
        if spu.len() > MAX_SPU_NO_PTS {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "subpicture continuation PES payload {} bytes exceeds sector capacity {}",
                    spu.len(),
                    MAX_SPU_NO_PTS
                ),
            ));
        }
        let scr = self.current_pts.unwrap_or(0);
        self.write_sector(scr, None, spu)
    }

    /// Internal: write one complete 2048-byte sector. `scr_90khz`
    /// goes into the pack header; `pts_90khz`, if `Some`, goes into
    /// the inner PES header; `spu` is the SPU payload.
    fn write_sector(
        &mut self,
        scr_90khz: u64,
        pts_90khz: Option<u64>,
        spu: &[u8],
    ) -> io::Result<()> {
        let start = self.bytes_written;
        // 1) Pack header (14 bytes).
        self.out.write_all(&pack_header_bytes(scr_90khz))?;
        self.bytes_written += 14;
        // 2) private_stream_1 PES carrying the SPU.
        //
        // PES bytes inside `packet_length`:
        //   1 flags1 (0x81 = '10' markers + data_alignment_indicator)
        //   1 flags2 (0x80 = PTS-only, 0x00 = no PTS)
        //   1 PES_header_data_length (5 for PTS, 0 for no-PTS)
        //   5 PTS field (only if PTS)
        //   1 substream_id
        //   N SPU bytes
        let (flags2, hdr_data_len, pts_field_len) = match pts_90khz {
            Some(_) => (0x80u8, 0x05u8, 5),
            None => (0x00u8, 0x00u8, 0),
        };
        let pes_pkt_len = u16::try_from(3 + pts_field_len + 1 + spu.len())
            .expect("PES packet length fits in u16 by construction");
        self.out.write_all(&[0x00, 0x00, 0x01, 0xBD])?;
        self.out.write_all(&pes_pkt_len.to_be_bytes())?;
        self.out.write_all(&[0x81, flags2, hdr_data_len])?;
        self.bytes_written += 9;
        if let Some(pts) = pts_90khz {
            self.out.write_all(&encode_pts_field(0b0010, pts))?;
            self.bytes_written += 5;
        }
        self.out.write_all(&[self.substream_id])?;
        self.bytes_written += 1;
        self.out.write_all(spu)?;
        self.bytes_written += spu.len() as u64;
        // 3) Pad to the 2048-byte sector boundary with a pad_stream
        //    PES (0x00 0x00 0x01 0xBE + length + 0xFF…). This matches
        //    real DVD VOBs.
        let written_in_sector = (self.bytes_written - start) as usize;
        let remaining = (SECTOR_SIZE as usize)
            .checked_sub(written_in_sector)
            .expect("sector overflow — caller must respect MAX_SPU_*");
        self.write_pad_stream(remaining)?;
        debug_assert_eq!(self.bytes_written - start, SECTOR_SIZE);
        Ok(())
    }

    /// Emit exactly `total` bytes of pad_stream (`00 00 01 BE` PES
    /// with `0xFF` content). If `total` is 0, nothing is written;
    /// if `total < 6` we cannot fit a pad_stream and fall back to
    /// zero-fill (does not occur for any DVD-spec-conforming SPU
    /// payload size).
    fn write_pad_stream(&mut self, total: usize) -> io::Result<()> {
        if total == 0 {
            return Ok(());
        }
        if total < 6 {
            let zeros = vec![0u8; total];
            self.out.write_all(&zeros)?;
            self.bytes_written += total as u64;
            return Ok(());
        }
        let payload_len = total - 6;
        let pkt_len = u16::try_from(payload_len)
            .expect("pad_stream payload_len fits in u16 for sector-bounded total");
        self.out.write_all(&[0x00, 0x00, 0x01, 0xBE])?;
        self.out.write_all(&pkt_len.to_be_bytes())?;
        let chunk = [0xFFu8; 512];
        let mut remaining = payload_len;
        while remaining > 0 {
            let n = std::cmp::min(remaining, chunk.len());
            self.out.write_all(&chunk[..n])?;
            remaining -= n;
        }
        self.bytes_written += total as u64;
        Ok(())
    }

    /// Close the writer. Each `write_subtitle` / `write_continuation`
    /// already produces a sector-aligned 2048-byte sector, so no
    /// trailing padding is required.
    pub fn finish(mut self) -> io::Result<(W, Vec<VobSubEntry>)> {
        debug_assert_eq!(self.bytes_written % SECTOR_SIZE, 0);
        let _ = self.out.seek(SeekFrom::Start(0))?;
        Ok((self.out, self.entries))
    }
}

/// 14-byte MPEG-2 pack header with the supplied PTS encoded as the
/// SCR (we use PTS-as-SCR — decoders don't validate the SCR for
/// VobSub playback). `program_mux_rate` is set to 10 Mbps which is a
/// safe value for DVD-Video. Stuffing length = 0.
fn pack_header_bytes(scr_90khz: u64) -> [u8; 14] {
    let mut b = [0u8; 14];
    b[0] = 0x00;
    b[1] = 0x00;
    b[2] = 0x01;
    b[3] = 0xBA;
    // SCR base (33 bits) + SCR_ext (9 bits, set to 0). SCR_base ==
    // PTS for our synthetic stream.
    let scr_base = scr_90khz & ((1u64 << 33) - 1);
    let scr_ext: u64 = 0;
    let bits_32_30 = (scr_base >> 30) & 0b111;
    let bits_29_15 = (scr_base >> 15) & 0x7FFF;
    let bits_14_0 = scr_base & 0x7FFF;
    let bits_8_0 = scr_ext & 0x1FF;
    // byte 4: '01' | SCR[32:30](3) | M(1) | SCR[29:28](2)
    b[4] = 0b0100_0000 | ((bits_32_30 as u8) << 3) | 0b0000_0100 | ((bits_29_15 >> 13) & 0b11) as u8;
    // byte 5: SCR[27:20]
    b[5] = ((bits_29_15 >> 5) & 0xFF) as u8;
    // byte 6: SCR[19:15](5) | M(1) | SCR[14:13](2)
    b[6] = (((bits_29_15 & 0x1F) as u8) << 3) | 0b0000_0100 | ((bits_14_0 >> 13) & 0b11) as u8;
    // byte 7: SCR[12:5]
    b[7] = ((bits_14_0 >> 5) & 0xFF) as u8;
    // byte 8: SCR[4:0](5) | M(1) | SCR_ext[8:7](2)
    b[8] = (((bits_14_0 & 0x1F) as u8) << 3) | 0b0000_0100 | ((bits_8_0 >> 7) & 0b11) as u8;
    // byte 9: SCR_ext[6:0](7) | M(1)
    b[9] = (((bits_8_0 & 0x7F) as u8) << 1) | 0b1;
    // bytes 10..12: program_mux_rate(22) + 2 markers. Use 0x1A00 ~ 10 Mbps.
    let mux_rate: u32 = 0x1A_00_00 >> 2; // arbitrary; high enough for DVD
    b[10] = ((mux_rate >> 14) & 0xFF) as u8;
    b[11] = ((mux_rate >> 6) & 0xFF) as u8;
    b[12] = (((mux_rate as u8) & 0b11_1111) << 2) | 0b11; // 6-bit low + 2 markers
    // byte 13: 5 reserved bits | 3-bit pack_stuffing_length (0)
    b[13] = 0b1111_1000;
    b
}

/// Encode a 33-bit PTS or DTS into the 5-byte MPEG-PS field with the
/// given 4-bit tag (`0b0010` for PTS-only, `0b0011` for PTS+DTS PTS,
/// `0b0001` for DTS). Marker bits set to 1.
#[must_use]
fn encode_pts_field(tag: u8, value_33b: u64) -> [u8; 5] {
    let v = value_33b & ((1u64 << 33) - 1);
    let hi = ((v >> 30) & 0b111) as u8;
    let mid = ((v >> 15) & 0x7FFF) as u16;
    let lo = (v & 0x7FFF) as u16;
    [
        ((tag & 0x0F) << 4) | (hi << 1) | 0b1,
        ((mid >> 7) & 0xFF) as u8,
        (((mid & 0x7F) as u8) << 1) | 0b1,
        ((lo >> 7) & 0xFF) as u8,
        (((lo & 0x7F) as u8) << 1) | 0b1,
    ]
}

/// Write a `.idx` file with palette + per-subtitle index.
///
/// `palette_pgc` is the 16-entry `pgc.palette` field (each `u32` is
/// `0x00 Y Cr Cb` with 8-bit components). We convert each entry to an
/// RGB hex string for the VobSub palette directive.
pub fn write_idx_file<W: Write>(
    w: &mut W,
    palette_pgc: &[u32; 16],
    lang_2letter: &str,
    width: u32,
    height: u32,
    entries: &[VobSubEntry],
) -> io::Result<()> {
    writeln!(w, "# VobSub index file, v7 (do not modify this line!)")?;
    writeln!(w, "# ")?;
    writeln!(w, "# This index block was generated by disc-remuxer")?;
    writeln!(w, "# ")?;
    writeln!(w, "size: {width}x{height}")?;
    writeln!(w, "org: 0, 0")?;
    writeln!(w, "alpha: 100%")?;
    writeln!(w, "smooth: OFF")?;
    writeln!(w, "fadein/out: 50, 50")?;
    writeln!(w, "align: OFF at LEFT TOP")?;
    writeln!(w, "time offset: 0")?;
    writeln!(w, "forced subs: OFF")?;
    writeln!(w, "langidx: 0")?;
    write!(w, "palette: ")?;
    for (i, &entry) in palette_pgc.iter().enumerate() {
        if i > 0 {
            write!(w, ", ")?;
        }
        let (r, g, b) = ycrcb_u32_to_rgb(entry);
        write!(w, "{r:02x}{g:02x}{b:02x}")?;
    }
    writeln!(w)?;
    writeln!(w, "# ")?;
    writeln!(w, "# end")?;
    writeln!(w)?;
    writeln!(w, "id: {lang_2letter}, index: 0")?;
    for e in entries {
        // Timestamp format in .idx is HH:MM:SS:MMM (millisecond
        // colon-separated). filepos is 9 hex digits.
        let total_ms = e.pts_90khz / 90;
        let h = total_ms / 3_600_000;
        let m = (total_ms / 60_000) % 60;
        let s = (total_ms / 1_000) % 60;
        let ms = total_ms % 1_000;
        writeln!(
            w,
            "timestamp: {h:02}:{m:02}:{s:02}:{ms:03}, filepos: {:09x}",
            e.filepos
        )?;
    }
    Ok(())
}

/// Convert one `pgc.palette[i]` entry (`0x00 Y Cr Cb`) into a 24-bit
/// `(R, G, B)` triple using ITU-R BT.601 limited-range matrix
/// coefficients. The conversion mirrors what most VobSub viewers and
/// MakeMKV's own palette emission do.
#[must_use]
pub fn ycrcb_u32_to_rgb(ycrcb: u32) -> (u8, u8, u8) {
    let y = ((ycrcb >> 16) & 0xFF) as f64;
    let cr = ((ycrcb >> 8) & 0xFF) as f64;
    let cb = (ycrcb & 0xFF) as f64;
    let r = y + 1.402 * (cr - 128.0);
    let g = y - 0.344_136 * (cb - 128.0) - 0.714_136 * (cr - 128.0);
    let b = y + 1.772 * (cb - 128.0);
    let clip = |v: f64| -> u8 { v.clamp(0.0, 255.0).round() as u8 };
    (clip(r), clip(g), clip(b))
}

/// Convert one PGC palette entry expressed as a packed `dvd_time_t`-
/// adjacent `u32` ... no, ignore — included only so the public type
/// re-export stays useful in callers that don't already pull in `ifo`.
#[doc(hidden)]
pub fn _re_export_anchor(_t: dvd_time_t) {}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn encode_pts_field_round_trips() {
        // Use our parser to round-trip a value through both directions.
        let pts: u64 = 0x0_1234_5678;
        let bytes = encode_pts_field(0b0010, pts);
        // Recover the 33-bit value the same way the parser does.
        let hi = u64::from((bytes[0] >> 1) & 0b111);
        let mid = (u64::from(bytes[1]) << 7) | u64::from((bytes[2] >> 1) & 0x7F);
        let lo = (u64::from(bytes[3]) << 7) | u64::from((bytes[4] >> 1) & 0x7F);
        let recovered = (hi << 30) | (mid << 15) | lo;
        assert_eq!(recovered, pts);
    }

    #[test]
    fn ycrcb_zero_chroma_is_grey() {
        let (r, g, b) = ycrcb_u32_to_rgb(0x00_80_80_80); // Y=0x80, Cr=Cb=0x80
        // Y=128, neutral chroma → neutral grey ~128
        assert!(r.abs_diff(g) <= 1 && g.abs_diff(b) <= 1);
        assert_eq!(r, 128);
    }

    #[test]
    fn sub_writer_aligns_to_sector() {
        let buf = Cursor::new(Vec::new());
        let mut sw = SubWriter::new(buf, 0x20);
        sw.write_subtitle(90_000, &[0x00, 0x01, 0x02, 0x03]).unwrap();
        sw.write_subtitle(180_000, &[0x10, 0x11, 0x12, 0x13]).unwrap();
        sw.write_subtitle(270_000, &[0x20, 0x21, 0x22, 0x23]).unwrap();
        let (cur, entries) = sw.finish().unwrap();
        let data = cur.into_inner();
        assert_eq!(data.len(), 3 * 2048);
        assert_eq!(entries.len(), 3);
        assert_eq!(entries[0].filepos, 0);
        assert_eq!(entries[1].filepos, 2048);
        assert_eq!(entries[2].filepos, 4096);
        for e in &entries {
            assert_eq!(
                &data[e.filepos as usize..e.filepos as usize + 4],
                &[0x00, 0x00, 0x01, 0xBA]
            );
        }
    }

    #[test]
    fn sub_writer_emits_pad_stream() {
        let buf = Cursor::new(Vec::new());
        let mut sw = SubWriter::new(buf, 0x20);
        let spu = [0xAAu8; 64];
        sw.write_subtitle(90_000, &spu).unwrap();
        let (cur, _) = sw.finish().unwrap();
        let data = cur.into_inner();
        // Find the pad_stream start code.
        let pad_off = data
            .windows(4)
            .position(|w| w == [0x00, 0x00, 0x01, 0xBE])
            .expect("pad_stream PES should be present");
        // After pad_stream's 2-byte length field comes 0xFF content.
        let pkt_len = ((data[pad_off + 4] as usize) << 8) | data[pad_off + 5] as usize;
        let content = &data[pad_off + 6..pad_off + 6 + pkt_len];
        assert!(content.iter().all(|&b| b == 0xFF), "pad content must be 0xFF");
        // Sector should be exactly 2048 bytes.
        assert_eq!(data.len(), 2048);
    }

    #[test]
    fn sub_writer_continuation_no_pts() {
        let buf = Cursor::new(Vec::new());
        let mut sw = SubWriter::new(buf, 0x20);
        sw.write_subtitle(90_000, &[0xAA; 32]).unwrap();
        sw.write_continuation(&[0xBB; 16]).unwrap();
        // Next SPU starts a fresh sector.
        sw.write_subtitle(180_000, &[0xCC; 32]).unwrap();
        let (cur, entries) = sw.finish().unwrap();
        let data = cur.into_inner();
        assert_eq!(data.len(), 3 * 2048);
        // Only 2 .idx entries (the continuation doesn't add one).
        assert_eq!(entries.len(), 2);
        assert_eq!(entries[0].filepos, 0);
        assert_eq!(entries[1].filepos, 4096);

        // First sector: PES with PTS (flags2 == 0x80).
        // PES starts at offset 14 (pack header is 14 bytes).
        assert_eq!(&data[14..18], &[0x00, 0x00, 0x01, 0xBD]);
        assert_eq!(data[14 + 6], 0x81); // flags1
        assert_eq!(data[14 + 7], 0x80); // flags2 = PTS only
        assert_eq!(data[14 + 8], 0x05); // PES_header_data_length

        // Continuation sector at offset 2048: PES with no PTS.
        let cont = 2048;
        assert_eq!(&data[cont + 14..cont + 18], &[0x00, 0x00, 0x01, 0xBD]);
        assert_eq!(data[cont + 14 + 6], 0x81); // flags1
        assert_eq!(data[cont + 14 + 7], 0x00); // flags2 = no PTS
        assert_eq!(data[cont + 14 + 8], 0x00); // PES_header_data_length = 0
        // substream_id follows the (empty) PES header data.
        assert_eq!(data[cont + 14 + 9], 0x20);
    }

    #[test]
    fn sub_writer_substream_id_in_pes() {
        let buf = Cursor::new(Vec::new());
        let mut sw = SubWriter::new(buf, 0x23);
        sw.write_subtitle(90_000, &[0xAA; 8]).unwrap();
        let (cur, _) = sw.finish().unwrap();
        let data = cur.into_inner();
        // substream_id sits right after the 5-byte PTS field in the
        // PES header (PES preamble 6 + flags+len 3 + PTS 5 = 14
        // bytes inside the PES, so at offset pack(14) + 14 = 28).
        assert_eq!(data[28], 0x23);
    }

    #[test]
    fn sub_writer_rejects_oversized_payload() {
        let buf = Cursor::new(Vec::new());
        let mut sw = SubWriter::new(buf, 0x20);
        let oversized = vec![0xAAu8; MAX_SPU_WITH_PTS + 1];
        let err = sw.write_subtitle(90_000, &oversized).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
    }
}
