//! `disc-remuxer demux-vob <vob> --out-dir <dir>` — thin CLI shell over
//! [`disc_dvd::ops::demux_vob`]. Feeds an MPEG-PS sector stream through
//! the demuxer and emits one file per elementary stream into `out-dir`.
//!
//! Step-5a milestone: byte routing only. No frame-boundary handling
//! across cells yet. The reported summary checks the accounting
//! invariant
//!
//!   input_bytes == pack_header_bytes + stripped_header_bytes
//!                + elementary_emitted_bytes + dropped_pes_bytes
//!
//! and the per-stream first-byte magic against the codec syncword.

use std::path::PathBuf;

use anyhow::Result;
use clap::Args;
use disc_dvd::demux::MagicCheck;
use disc_dvd::ops::demux_vob as op;

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
    let report = op::run(op::Params {
        path: args.path.clone(),
        out_dir: args.out_dir.clone(),
        max_sectors: args.max_sectors,
    })?;
    let summary = &report.summary;

    let accounted = summary.pack_header_bytes
        + summary.stripped_header_bytes
        + summary.elementary_emitted_bytes
        + summary.dropped_pes_bytes;

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
