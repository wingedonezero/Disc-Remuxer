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
//! For each subpicture PES observed during demux, we:
//!
//! 1. Pad the `.sub` writer to the next 2048-byte sector boundary
//!    (zeros — the existing pack at the previous offset is "complete"
//!    because its declared PES packet_length plus the pack header
//!    plus this padding sum to 2048).
//! 2. Record the new sector-aligned offset and the PES's PTS.
//! 3. Write a synthetic 14-byte pack header with the PTS encoded as
//!    SCR (the SCR value doesn't change which subpicture renders —
//!    decoders use the PTS in the inner PES — but real DVDs always
//!    carry one so we do too).
//! 4. Write a private_stream_1 PES with PTS and substream_id, then
//!    the SPU bytes.
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

/// Streaming `.sub` writer. Tracks the byte offset itself so it can
/// align each new SPU to a 2048-byte boundary.
pub struct SubWriter<W: Write + Seek> {
    out: W,
    bytes_written: u64,
    pub entries: Vec<VobSubEntry>,
    /// Stream's substream_id byte (0x20..=0x3F).
    substream_id: u8,
}

impl<W: Write + Seek> SubWriter<W> {
    pub fn new(out: W, substream_id: u8) -> Self {
        Self {
            out,
            bytes_written: 0,
            entries: Vec::new(),
            substream_id,
        }
    }

    /// Write one subtitle (SPU bytes + PTS) into the `.sub` file.
    pub fn write_subtitle(&mut self, pts_90khz: u64, spu: &[u8]) -> io::Result<()> {
        // Align to next 2048-byte boundary if we're not already there.
        let pos = self.bytes_written;
        if pos % SECTOR_SIZE != 0 {
            let pad = SECTOR_SIZE - (pos % SECTOR_SIZE);
            self.write_padding(pad)?;
        }
        // Record the aligned filepos before writing this subtitle.
        let filepos = self.bytes_written;
        self.entries.push(VobSubEntry {
            pts_90khz,
            filepos,
        });

        // Write pack header (14 bytes), PES header (16 bytes), then
        // the SPU payload.
        let pack = pack_header_bytes(pts_90khz);
        self.out.write_all(&pack)?;
        self.bytes_written += 14;

        // PES layout:
        //   6 bytes: 00 00 01 BD + length(2)
        //   1 byte:  flags1 (0x81 = '10' marker + data_alignment)
        //   1 byte:  flags2 (0x80 = PTS_DTS_flags '10')
        //   1 byte:  PES_header_data_length = 5
        //   5 bytes: PTS field
        //   1 byte:  substream_id
        //   N bytes: SPU
        // Total PES bytes = 15 + N. packet_length field excludes the
        // first 6 bytes (start code + stream_id + length itself).
        let pes_packet_length = u16::try_from(9 + 1 + spu.len()).unwrap_or(u16::MAX);
        let mut pes = Vec::with_capacity(15);
        pes.extend_from_slice(&[0x00, 0x00, 0x01, 0xBD]);
        pes.extend_from_slice(&pes_packet_length.to_be_bytes());
        pes.push(0x81); // flags1
        pes.push(0x80); // flags2
        pes.push(0x05); // PES_header_data_length
        pes.extend_from_slice(&encode_pts_field(0b0010, pts_90khz));
        pes.push(self.substream_id);
        debug_assert_eq!(pes.len(), 15);
        self.out.write_all(&pes)?;
        self.bytes_written += 15;

        self.out.write_all(spu)?;
        self.bytes_written += spu.len() as u64;
        Ok(())
    }

    /// Write `count` bytes of zero padding.
    fn write_padding(&mut self, count: u64) -> io::Result<()> {
        let mut remaining = count;
        let zeros = [0u8; 256];
        while remaining > 0 {
            let n = std::cmp::min(remaining as usize, zeros.len());
            self.out.write_all(&zeros[..n])?;
            self.bytes_written += n as u64;
            remaining -= n as u64;
        }
        Ok(())
    }

    /// Pad the final entry out to a sector boundary so the file
    /// length is a multiple of 2048. Returns the writer.
    pub fn finish(mut self) -> io::Result<(W, Vec<VobSubEntry>)> {
        let pos = self.bytes_written;
        if pos % SECTOR_SIZE != 0 {
            let pad = SECTOR_SIZE - (pos % SECTOR_SIZE);
            self.write_padding(pad)?;
        }
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
        // Write 3 tiny subtitles. Each gets its own 2048-byte sector.
        sw.write_subtitle(90_000, &[0x00, 0x01, 0x02, 0x03]).unwrap();
        sw.write_subtitle(180_000, &[0x10, 0x11, 0x12, 0x13]).unwrap();
        sw.write_subtitle(270_000, &[0x20, 0x21, 0x22, 0x23]).unwrap();
        let (cur, entries) = sw.finish().unwrap();
        let data = cur.into_inner();
        // 3 entries * 2048 byte sectors = 6144 bytes.
        assert_eq!(data.len(), 3 * 2048);
        assert_eq!(entries.len(), 3);
        assert_eq!(entries[0].filepos, 0);
        assert_eq!(entries[1].filepos, 2048);
        assert_eq!(entries[2].filepos, 4096);
        // Each entry starts with the pack-header magic.
        for e in &entries {
            assert_eq!(
                &data[e.filepos as usize..e.filepos as usize + 4],
                &[0x00, 0x00, 0x01, 0xBA]
            );
        }
    }
}
