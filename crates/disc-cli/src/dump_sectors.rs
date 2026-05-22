//! `disc-remuxer dump-sectors` — read raw sectors from a DVD VOB stream
//! and write them to a file.
//!
//! Primarily a verification tool: dump a known range and hash it, then
//! compare against the same range read via another path (e.g. `dd` of
//! an ISO) to confirm the sector-read layer is wired up correctly. On
//! a CSS-protected disc the bytes here should be cleartext (libdvdread
//! decrypts via libdvdcss transparently).

use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use clap::Args;
use disc_core::{check, check_eq};
use disc_dvd::{DvdFile, DvdSource, ReadDomain, BLOCK_SIZE};
use sha2::{Digest, Sha256};

/// Largest count we'll let the user request in one go. 65536 blocks =
/// 128 MiB; bigger requests should iterate in chunks. We refuse outright
/// rather than silently allocate gigabytes.
const MAX_BLOCKS_PER_CALL: u32 = 65_536;

#[derive(Args, Debug)]
pub struct DumpSectorsArgs {
    /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
    pub path: PathBuf,

    /// Video Title Set number (1..=99 for VTS files). 0 means the
    /// disc-wide `VIDEO_TS.VOB` (when used with `--domain menu`).
    #[arg(long)]
    pub vts: u32,

    /// Which file domain to read from.
    #[arg(long, default_value = "title")]
    pub domain: DomainArg,

    /// Starting block offset within the file.
    #[arg(long, default_value_t = 0)]
    pub offset: u32,

    /// Number of 2048-byte blocks to read.
    #[arg(long, default_value_t = 16)]
    pub count: u32,

    /// File path to write the raw sectors to.
    #[arg(long)]
    pub out: PathBuf,
}

#[derive(clap::ValueEnum, Debug, Clone, Copy)]
pub enum DomainArg {
    /// `VTS_NN_[1-9].VOB` concatenated — the actual title content.
    Title,
    /// `VIDEO_TS.VOB` / `VTS_NN_0.VOB` — menu content.
    Menu,
}

impl From<DomainArg> for ReadDomain {
    fn from(d: DomainArg) -> Self {
        match d {
            DomainArg::Title => ReadDomain::TitleVobs,
            DomainArg::Menu => ReadDomain::MenuVobs,
        }
    }
}

pub fn run(args: DumpSectorsArgs) -> Result<()> {
    if args.count == 0 {
        anyhow::bail!("--count must be > 0");
    }
    if args.count > MAX_BLOCKS_PER_CALL {
        anyhow::bail!(
            "--count {} exceeds MAX_BLOCKS_PER_CALL={MAX_BLOCKS_PER_CALL} (128 MiB)",
            args.count,
        );
    }

    log::info!(
        "dump-sectors path={} vts={} domain={:?} offset={} count={} out={}",
        args.path.display(),
        args.vts,
        args.domain,
        args.offset,
        args.count,
        args.out.display(),
    );

    let source = DvdSource::open(&args.path).context("DvdSource::open")?;
    let dvd_file = DvdFile::open(source.reader(), args.vts, args.domain.into())
        .context("DvdFile::open")?;

    log::info!(
        "opened: vts={} domain={:?} block_count={} byte_size={}",
        dvd_file.vts_nr(),
        dvd_file.domain(),
        dvd_file.block_count(),
        dvd_file.byte_size(),
    );

    // Verifications before reading: the user's range fits the file.
    let end = args.offset.saturating_add(args.count);
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
        .read_blocks(args.offset, args.count)
        .context("DvdFile::read_blocks")?;

    // Verification: read returned the expected byte count.
    let expected_bytes = (args.count as usize) * (BLOCK_SIZE as usize);
    check_eq("read returned expected byte count", buf.len(), expected_bytes);

    // First-sector MPEG-PS magic check (only meaningful when reading
    // from offset 0 of a VOB stream and the disc is supposed to be
    // playable). If this fails on what should be a cleartext sector we
    // probably have a CSS-authentication problem.
    if args.offset == 0 && buf.len() >= 4 {
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

    // Write to disk.
    log::info!("writing {} bytes to {}", buf.len(), args.out.display());
    write_atomically(&args.out, &buf)
        .with_context(|| format!("writing {}", args.out.display()))?;

    // SHA-256 for byte-compare verification with external tools.
    let digest = Sha256::digest(&buf);
    log::info!("sha256 = {digest:x}");

    println!("wrote {} bytes to {}", buf.len(), args.out.display());
    println!("sha256: {digest:x}");

    Ok(())
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
