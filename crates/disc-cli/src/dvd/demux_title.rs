//! `disc-remuxer demux-title --disc <path> --title N --out-dir <dir>` —
//! thin CLI shell over [`disc_dvd::ops::demux_title`].
//!
//! Walks a title's PGC cells directly from disc and demultiplexes each
//! cell's MPEG-PS sectors into per-stream files, wiring cell metadata
//! (especially `stc_discontinuity`) through to the demuxer so step-5b's
//! FAP-based audio resync fires at the right boundaries.
//!
//! This is the rip path that doesn't go through the intermediate VOB
//! file. Title resolution mirrors `dump-title`: VMG `tt_srpt[N-1]` →
//! `(title_set_nr, vts_ttn)` → VTS PTT chapter 1's `pgcn` → PGC →
//! `cell_playback[0..nr_of_cells]`. For each cell:
//!
//! 1. `Demuxer::begin_cell(cell.stc_discontinuity)` — signals the
//!    demuxer to mark known AC-3/DTS streams for FAP-based resync.
//! 2. Read each sector of the cell via `DvdFile::read_blocks`, fed
//!    1 MiB at a time to keep peak memory bounded.
//! 3. `Demuxer::process_sector` per sector — routes PES packets to
//!    output writers.
//!
//! After the walk, accounting and per-stream magic results are logged
//! the same way `demux-vob` does, plus the discontinuity / FAP-skip
//! counters from the demuxer.

use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_core::{detect_disc_type, DiscType};
use disc_dvd::demux::MagicCheck;
use disc_dvd::ops::demux_title as op;
use disc_dvd::DvdSource;

#[derive(Args, Debug)]
pub struct DemuxTitleArgs {
    /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
    #[arg(long = "disc")]
    pub disc: PathBuf,

    /// 1-based title number to demux (as it appears in `tt_srpt`).
    #[arg(long)]
    pub title: u8,

    /// Directory to write per-stream output files into. Created if it
    /// doesn't exist.
    #[arg(long)]
    pub out_dir: PathBuf,
}

pub fn run(args: DemuxTitleArgs) -> Result<()> {
    if args.title == 0 {
        return Err(anyhow!("--title must be >= 1 (1-based per tt_srpt)"));
    }
    let disc_type = detect_disc_type(&args.disc).context("detect_disc_type")?;
    if !matches!(disc_type, DiscType::Dvd) {
        return Err(anyhow!(
            "demux-title currently supports DVD only; detected {}",
            disc_type.as_str()
        ));
    }

    let source = DvdSource::open(&args.disc).context("DvdSource::open")?;
    let report = op::run(
        source.reader(),
        op::Params {
            title: args.title,
            out_dir: args.out_dir.clone(),
        },
    )?;

    let summary = &report.summary;
    let cells_count = report.cells as u64;
    let accounted = summary.pack_header_bytes
        + summary.stripped_header_bytes
        + summary.elementary_emitted_bytes
        + summary.dropped_pes_bytes;

    println!();
    println!(
        "demux-title summary (title {}, {} cells -> {}):",
        args.title,
        cells_count,
        args.out_dir.display()
    );
    println!("  sectors processed:       {}", summary.sectors_processed);
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
    println!(
        "  stc_discontinuity cells: {}",
        summary.discontinuity_boundaries
    );

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
