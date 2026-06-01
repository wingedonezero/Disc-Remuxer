//! `scan-streams` operation — walk a file of back-to-back 2048-byte
//! MPEG-PS sectors, parse each as one pack, and tally per-stream packet +
//! byte counts. The read side of the demuxer: proves every byte in every
//! sector is classified by stream. File-level (no disc handle).

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};

use crate::mpegps::{scan_sector, stream_kind, StreamKind, SECTOR_SIZE};

#[derive(Debug)]
pub struct Params {
    pub path: PathBuf,
    pub max_sectors: u64,
}

#[derive(Default, Debug)]
pub struct StreamTally {
    pub packets: u64,
    pub payload_bytes: u64,
    pub packet_bytes: u64,
}

#[derive(Debug)]
pub struct Report {
    pub sectors_scanned: u64,
    pub packets_total: u64,
    pub bytes_in_elementary: u64,
    pub bytes_in_nav: u64,
    pub bytes_in_padding: u64,
    pub bytes_in_system_header: u64,
    pub bytes_in_unknown: u64,
    pub streams: BTreeMap<String, StreamTally>,
}

pub fn run(params: Params) -> Result<Report> {
    log::info!(
        "scan-streams path={} max_sectors={}",
        params.path.display(),
        params.max_sectors
    );

    let file = File::open(&params.path)
        .with_context(|| format!("opening {}", params.path.display()))?;
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
        if params.max_sectors == 0 { "all" } else { "set" },
    );

    let mut reader = BufReader::with_capacity(SECTOR_SIZE * 64, file);
    let mut sector_buf = vec![0u8; SECTOR_SIZE];

    let mut streams: BTreeMap<String, StreamTally> = BTreeMap::new();
    let mut sectors_scanned: u64 = 0;
    let mut packets_total: u64 = 0;
    let mut bytes_in_padding: u64 = 0;
    let mut bytes_in_nav: u64 = 0;
    let mut bytes_in_system_header: u64 = 0;
    let mut bytes_in_elementary: u64 = 0;
    let mut bytes_in_unknown: u64 = 0;

    loop {
        if params.max_sectors > 0 && sectors_scanned >= params.max_sectors {
            log::info!("stopping at --max-sectors={}", params.max_sectors);
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
            format!("parsing sector {sectors_scanned} of {}", params.path.display())
        })?;

        for pes in &contents.pes_packets {
            let kind = stream_kind(pes.stream_id, pes.substream_id);
            let key = kind.label();
            let entry = streams.entry(key).or_default();
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

    Ok(Report {
        sectors_scanned,
        packets_total,
        bytes_in_elementary,
        bytes_in_nav,
        bytes_in_padding,
        bytes_in_system_header,
        bytes_in_unknown,
        streams,
    })
}
