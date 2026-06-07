//! `disc-remuxer scan-streams <vob_path>` — thin CLI shell over
//! [`disc_dvd::ops::scan_streams`]. Walks a file of back-to-back 2048-byte
//! MPEG-PS sectors and reports per-stream packet + byte counts.

use std::path::PathBuf;

use anyhow::Result;
use clap::Args;
use disc_dvd::ops::scan_streams as op;

#[derive(Args, Debug)]
pub struct ScanStreamsArgs {
    /// Path to a .vob (or any file containing back-to-back 2048-byte
    /// MPEG-PS sectors — including the output of `dump-title`).
    pub path: PathBuf,

    /// Stop after parsing this many sectors. `0` = no limit.
    #[arg(long, default_value_t = 0)]
    pub max_sectors: u64,
}

pub fn run(args: ScanStreamsArgs) -> Result<()> {
    let report = op::run(op::Params {
        path: args.path.clone(),
        max_sectors: args.max_sectors,
    })?;

    println!();
    println!("scan-streams summary for {}:", args.path.display());
    println!("  sectors scanned:        {}", report.sectors_scanned);
    println!("  total PES packets:      {}", report.packets_total);
    println!("  bytes in elementary:    {}", report.bytes_in_elementary);
    println!("  bytes in NV_PCK:        {}", report.bytes_in_nav);
    println!("  bytes in padding:       {}", report.bytes_in_padding);
    println!("  bytes in system_header: {}", report.bytes_in_system_header);
    if report.bytes_in_unknown > 0 {
        println!("  bytes in unknown:       {}  ⚠", report.bytes_in_unknown);
    }
    println!();
    println!(
        "{:<32}  {:>10}  {:>14}  {:>14}",
        "stream", "packets", "payload bytes", "packet bytes"
    );
    println!("{:-<32}  {:->10}  {:->14}  {:->14}", "", "", "", "");
    for (label, s) in &report.streams {
        println!(
            "{label:<32}  {:>10}  {:>14}  {:>14}",
            s.packets, s.payload_bytes, s.packet_bytes
        );
    }

    Ok(())
}
