//! `disc-remuxer demux-vob <vob> --out-dir <dir>` — feed an MPEG-PS
//! sector stream through [`disc_dvd::Demuxer`] and emit one file per
//! elementary stream into `out-dir`.
//!
//! Step-5a milestone: byte routing only. No frame-boundary handling
//! across cells yet. The reported summary checks the accounting
//! invariant
//!
//!   input_bytes == pack_header_bytes + stripped_header_bytes
//!                + elementary_emitted_bytes + dropped_pes_bytes
//!
//! and the per-stream first-byte magic against the codec syncword.

use std::fs::File;
use std::io::{BufReader, Read};
use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_core::{check, check_eq};
use disc_dvd::demux::{Demuxer, MagicCheck};
use disc_dvd::mpegps::SECTOR_SIZE;

#[derive(Args, Debug)]
pub struct DemuxVobArgs {
    /// Path to a 2048-byte-sector MPEG-PS file (typically the output
    /// of `dump-title`).
    pub path: PathBuf,

    /// Directory to write per-stream output files into. Created if it
    /// doesn't exist.
    #[arg(long)]
    pub out_dir: PathBuf,

    /// Stop after this many sectors. `0` = no limit.
    #[arg(long, default_value_t = 0)]
    pub max_sectors: u64,
}

pub fn run(args: DemuxVobArgs) -> Result<()> {
    log::info!(
        "demux-vob path={} out_dir={} max_sectors={}",
        args.path.display(),
        args.out_dir.display(),
        args.max_sectors,
    );

    std::fs::create_dir_all(&args.out_dir)
        .with_context(|| format!("creating {}", args.out_dir.display()))?;

    let file = File::open(&args.path)
        .with_context(|| format!("opening {}", args.path.display()))?;
    let metadata = file.metadata().context("stat input")?;
    let input_size = metadata.len();
    if input_size % SECTOR_SIZE as u64 != 0 {
        log::warn!(
            "input size {input_size} is not a multiple of 2048; trailing {} bytes will be ignored",
            input_size % SECTOR_SIZE as u64,
        );
    }
    let sector_count = input_size / SECTOR_SIZE as u64;
    log::info!("input: {input_size} bytes -> {sector_count} sectors");

    let mut reader = BufReader::with_capacity(SECTOR_SIZE * 64, file);
    let mut sector_buf = vec![0u8; SECTOR_SIZE];

    let mut demuxer = Demuxer::new(&args.out_dir);
    let mut sectors_read: u64 = 0;
    loop {
        if args.max_sectors > 0 && sectors_read >= args.max_sectors {
            log::info!("stopping at --max-sectors={}", args.max_sectors);
            break;
        }
        match reader.read_exact(&mut sector_buf) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(e) => {
                return Err(anyhow!(e))
                    .with_context(|| format!("reading sector {sectors_read}"));
            }
        }
        let label = format!("sector {sectors_read}");
        demuxer
            .process_sector(&sector_buf, &label)
            .with_context(|| format!("demuxing sector {sectors_read}"))?;
        sectors_read += 1;
        if sectors_read % 100_000 == 0 {
            log::info!("demuxed {sectors_read} sectors so far");
        }
    }

    let summary = demuxer.finish().context("finalizing demuxer")?;

    // Accounting invariant:
    //   input == pack_header + stripped_header + emitted + dropped
    let accounted = summary.pack_header_bytes
        + summary.stripped_header_bytes
        + summary.elementary_emitted_bytes
        + summary.dropped_pes_bytes;
    check_eq(
        "demux: byte accounting closes",
        accounted,
        summary.input_bytes,
    );
    check(
        "demux: all sectors processed without error",
        &format!("sectors_processed == sectors_read ({sectors_read})"),
        || summary.sectors_processed == sectors_read,
    );

    println!();
    println!("demux-vob summary for {}:", args.path.display());
    println!("  sectors processed:       {}", summary.sectors_processed);
    println!("  PES packets seen:        {}", summary.pes_total);
    println!("  input bytes:             {}", summary.input_bytes);
    println!("  pack header bytes:       {}", summary.pack_header_bytes);
    println!("  PES/BD header bytes:     {}", summary.stripped_header_bytes);
    println!("  elementary bytes out:    {}", summary.elementary_emitted_bytes);
    println!("  dropped bytes (NV/pad/sys): {}", summary.dropped_pes_bytes);
    println!("  accounted total:         {accounted}");
    if accounted == summary.input_bytes {
        println!("  byte accounting:         PASS");
    } else {
        println!(
            "  byte accounting:         FAIL ({} byte difference)",
            (accounted as i128) - (summary.input_bytes as i128)
        );
    }

    println!();
    println!(
        "{:<32}  {:>10}  {:>14}  {:>14}  {:<12}  first bytes",
        "stream", "packets", "PES bytes", "emitted bytes", "magic check"
    );
    println!(
        "{:-<32}  {:->10}  {:->14}  {:->14}  {:-<12}  {:-<24}",
        "", "", "", "", "", ""
    );
    for (key, stats) in &summary.streams {
        let magic = match stats.magic_check {
            MagicCheck::Pass => "PASS",
            MagicCheck::Fail => "FAIL",
            MagicCheck::Skipped => "skipped",
            MagicCheck::Pending => "pending",
        };
        let first = if stats.first_bytes_set {
            stats
                .first_bytes
                .iter()
                .map(|b| format!("{b:02X}"))
                .collect::<Vec<_>>()
                .join(" ")
        } else {
            "(none)".into()
        };
        println!(
            "{:<32}  {:>10}  {:>14}  {:>14}  {:<12}  {first}",
            key.label(),
            stats.pes_count,
            stats.pes_bytes,
            stats.emitted_bytes,
            magic,
        );
    }
    println!();
    println!(
        "wrote {} per-stream files to {}",
        summary.streams.len(),
        args.out_dir.display()
    );

    Ok(())
}
