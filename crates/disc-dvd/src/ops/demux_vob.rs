//! `demux-vob` operation — feed an MPEG-PS sector stream through
//! [`crate::Demuxer`] and emit one file per elementary stream into
//! `out-dir`. File-level (no disc handle).
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
use disc_core::{check, check_eq};

use crate::demux::Demuxer;
use crate::mpegps::SECTOR_SIZE;

#[derive(Debug)]
pub struct Params {
    pub path: PathBuf,
    pub out_dir: PathBuf,
    pub max_sectors: u64,
}

#[derive(Debug)]
pub struct Report {
    pub summary: crate::DemuxSummary,
}

pub fn run(params: Params) -> Result<Report> {
    log::info!(
        "demux-vob path={} out_dir={} max_sectors={}",
        params.path.display(),
        params.out_dir.display(),
        params.max_sectors,
    );

    std::fs::create_dir_all(&params.out_dir)
        .with_context(|| format!("creating {}", params.out_dir.display()))?;

    let file = File::open(&params.path)
        .with_context(|| format!("opening {}", params.path.display()))?;
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

    let mut demuxer = Demuxer::new(&params.out_dir);
    let mut sectors_read: u64 = 0;
    loop {
        if params.max_sectors > 0 && sectors_read >= params.max_sectors {
            log::info!("stopping at --max-sectors={}", params.max_sectors);
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

    Ok(Report { summary })
}
