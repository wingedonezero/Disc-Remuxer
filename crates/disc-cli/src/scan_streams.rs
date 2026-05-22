//! `disc-remuxer scan-streams <vob_path>` — walk a `.vob` file (or any
//! file containing concatenated MPEG-PS sectors of 2048 bytes each),
//! parse each sector as one pack, and report per-stream packet + byte
//! counts.
//!
//! This is the read side of the future demuxer: it doesn't write
//! elementary streams yet, it just proves we can identify every byte in
//! every sector and classify it by stream. Failure modes show up as
//! either parse errors (logged + aborted) or as the
//! "trailing_unknown_bytes" counter going non-zero.

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_dvd::mpegps::{scan_sector, stream_kind, StreamKind, SECTOR_SIZE};

#[derive(Args, Debug)]
pub struct ScanStreamsArgs {
    /// Path to a .vob (or any file containing back-to-back 2048-byte
    /// MPEG-PS sectors — including the output of `dump-title`).
    pub path: PathBuf,

    /// Stop after parsing this many sectors. `0` = no limit.
    #[arg(long, default_value_t = 0)]
    pub max_sectors: u64,
}

#[derive(Default)]
struct StreamStats {
    packets: u64,
    payload_bytes: u64,
    packet_bytes: u64,
}

pub fn run(args: ScanStreamsArgs) -> Result<()> {
    log::info!(
        "scan-streams path={} max_sectors={}",
        args.path.display(),
        args.max_sectors
    );

    let file = File::open(&args.path)
        .with_context(|| format!("opening {}", args.path.display()))?;
    let metadata = file.metadata().context("stat input file")?;
    let total_bytes = metadata.len();
    let sector_count_from_size = total_bytes / SECTOR_SIZE as u64;
    if total_bytes % SECTOR_SIZE as u64 != 0 {
        log::warn!(
            "input size {total_bytes} is not a multiple of {SECTOR_SIZE}; trailing {} bytes will be ignored",
            total_bytes % SECTOR_SIZE as u64,
        );
    }
    log::info!(
        "input size={total_bytes} bytes -> {sector_count_from_size} sectors (max sectors to scan: {})",
        if args.max_sectors == 0 { "all" } else { "set" },
    );

    let mut reader = BufReader::with_capacity(SECTOR_SIZE * 64, file);
    let mut sector_buf = vec![0u8; SECTOR_SIZE];

    let mut stats: BTreeMap<String, StreamStats> = BTreeMap::new();
    let mut sectors_scanned: u64 = 0;
    let mut packets_total: u64 = 0;
    let mut bytes_in_padding: u64 = 0;
    let mut bytes_in_nav: u64 = 0;
    let mut bytes_in_system_header: u64 = 0;
    let mut bytes_in_elementary: u64 = 0;
    let mut bytes_in_unknown: u64 = 0;

    loop {
        if args.max_sectors > 0 && sectors_scanned >= args.max_sectors {
            log::info!("stopping at --max-sectors={}", args.max_sectors);
            break;
        }
        match reader.read_exact(&mut sector_buf) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(e) => {
                return Err(anyhow!(e))
                    .with_context(|| format!("reading sector {sectors_scanned}"));
            }
        }

        let label = format!("sector {sectors_scanned}");
        let contents = scan_sector(&sector_buf, &label).with_context(|| {
            format!("parsing sector {sectors_scanned} of {}", args.path.display())
        })?;

        for pes in &contents.pes_packets {
            let kind = stream_kind(pes.stream_id, pes.substream_id);
            let key = kind.label();
            let entry = stats.entry(key).or_default();
            entry.packets += 1;
            entry.payload_bytes += pes.payload.len() as u64;
            entry.packet_bytes += pes.total_size as u64;

            packets_total += 1;
            match kind {
                StreamKind::Padding => bytes_in_padding += pes.total_size as u64,
                StreamKind::NavPack => bytes_in_nav += pes.total_size as u64,
                StreamKind::SystemHeader => {
                    bytes_in_system_header += pes.total_size as u64
                }
                StreamKind::Unknown { .. } => {
                    bytes_in_unknown += pes.total_size as u64
                }
                _ => bytes_in_elementary += pes.total_size as u64,
            }
        }

        sectors_scanned += 1;
        if sectors_scanned % 100_000 == 0 {
            log::info!("scanned {sectors_scanned} sectors so far");
        }
    }

    let total_pes_bytes = bytes_in_padding
        + bytes_in_nav
        + bytes_in_system_header
        + bytes_in_elementary
        + bytes_in_unknown;
    log::info!(
        "scan complete: {sectors_scanned} sectors, {packets_total} PES packets, {total_pes_bytes} PES bytes",
    );

    println!();
    println!(
        "scan-streams summary for {}:",
        args.path.display()
    );
    println!("  sectors scanned:        {sectors_scanned}");
    println!("  total PES packets:      {packets_total}");
    println!("  bytes in elementary:    {bytes_in_elementary}");
    println!("  bytes in NV_PCK:        {bytes_in_nav}");
    println!("  bytes in padding:       {bytes_in_padding}");
    println!("  bytes in system_header: {bytes_in_system_header}");
    if bytes_in_unknown > 0 {
        println!("  bytes in unknown:       {bytes_in_unknown}  ⚠");
    }
    println!();
    println!(
        "{:<32}  {:>10}  {:>14}  {:>14}",
        "stream", "packets", "payload bytes", "packet bytes"
    );
    println!("{:-<32}  {:->10}  {:->14}  {:->14}", "", "", "", "");
    for (label, s) in &stats {
        println!(
            "{label:<32}  {:>10}  {:>14}  {:>14}",
            s.packets, s.payload_bytes, s.packet_bytes
        );
    }

    Ok(())
}
