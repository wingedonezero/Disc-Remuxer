//! `dump-sectors` operation — read a raw sector range from a DVD VOB
//! stream and write it to a file, with SHA-256 for external byte-compare.
//!
//! On a CSS-protected disc the bytes here are cleartext (libdvdread
//! decrypts via libdvdcss transparently).

use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use disc_core::{check, check_eq};
use sha2::{Digest, Sha256};

use crate::{DvdFile, DvdReader, ReadDomain, BLOCK_SIZE};

/// Largest count we'll read in one call. 65536 blocks = 128 MiB; bigger
/// requests should iterate in chunks. We refuse outright rather than
/// silently allocate gigabytes.
pub const MAX_BLOCKS_PER_CALL: u32 = 65_536;

#[derive(Debug)]
pub struct Params {
    pub vts: u32,
    pub domain: ReadDomain,
    pub offset: u32,
    pub count: u32,
    pub out: PathBuf,
}

#[derive(Debug)]
pub struct Report {
    pub vts_nr: u32,
    pub domain: ReadDomain,
    pub block_count: u32,
    pub byte_size: u64,
    pub bytes_written: u64,
    pub sha256: String,
    pub out: PathBuf,
}

pub fn run(reader: &DvdReader, params: Params) -> Result<Report> {
    if params.count == 0 {
        bail!("--count must be > 0");
    }
    if params.count > MAX_BLOCKS_PER_CALL {
        bail!(
            "--count {} exceeds MAX_BLOCKS_PER_CALL={MAX_BLOCKS_PER_CALL} (128 MiB)",
            params.count,
        );
    }

    let dvd_file =
        DvdFile::open(reader, params.vts, params.domain).context("DvdFile::open")?;
    log::info!(
        "opened: vts={} domain={:?} block_count={} byte_size={}",
        dvd_file.vts_nr(),
        dvd_file.domain(),
        dvd_file.block_count(),
        dvd_file.byte_size(),
    );

    // Verifications before reading: the user's range fits the file.
    let end = params.offset.saturating_add(params.count);
    check(
        "requested range fits within file",
        &format!(
            "offset+count={} <= block_count={}",
            end,
            dvd_file.block_count()
        ),
        || end <= dvd_file.block_count(),
    );

    let buf = dvd_file
        .read_blocks(params.offset, params.count)
        .context("DvdFile::read_blocks")?;

    // Verification: read returned the expected byte count.
    let expected_bytes = (params.count as usize) * (BLOCK_SIZE as usize);
    check_eq("read returned expected byte count", buf.len(), expected_bytes);

    // First-sector MPEG-PS magic check (only meaningful when reading from
    // offset 0 of a VOB stream and the disc is supposed to be playable).
    if params.offset == 0 && buf.len() >= 4 {
        let head = [buf[0], buf[1], buf[2], buf[3]];
        check(
            "first sector starts with MPEG-PS pack-start code",
            "00 00 01 BA",
            || head == [0x00, 0x00, 0x01, 0xBA],
        );
        log::info!(
            "first 16 bytes of sector 0: {:02x?}",
            &buf[..buf.len().min(16)]
        );
    }

    log::info!("writing {} bytes to {}", buf.len(), params.out.display());
    write_atomically(&params.out, &buf)
        .with_context(|| format!("writing {}", params.out.display()))?;

    let digest = Sha256::digest(&buf);
    let sha256 = format!("{digest:x}");
    log::info!("sha256 = {sha256}");

    Ok(Report {
        vts_nr: dvd_file.vts_nr(),
        domain: dvd_file.domain(),
        block_count: dvd_file.block_count(),
        byte_size: dvd_file.byte_size(),
        bytes_written: buf.len() as u64,
        sha256,
        out: params.out,
    })
}

/// Write `data` to `path`, replacing whatever was there. Uses a
/// temp-file-and-rename so a partial write doesn't corrupt the target.
fn write_atomically(path: &Path, data: &[u8]) -> std::io::Result<()> {
    let tmp = path.with_extension("partial");
    {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(data)?;
        f.sync_all()?;
    }
    std::fs::rename(&tmp, path)?;
    Ok(())
}
