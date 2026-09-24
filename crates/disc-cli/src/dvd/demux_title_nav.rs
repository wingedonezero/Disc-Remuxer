//! `disc-remuxer demux-title-nav --disc <path> --title N --out-dir <dir>` —
//! thin CLI shell over [`disc_dvd::ops::demux_title_nav`].
//!
//! The libdvdnav-driven demuxer. Same per-stream output as `demux-title`
//! (the manual cell walk) but with libdvdnav executing the disc's
//! authored PGC playback path. See the op module for the full flow.

use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_core::{detect_disc_type, DiscType};
use disc_dvd::demux::MagicCheck;
use disc_dvd::ops::demux_title_nav as op;
use disc_dvd::DvdSource;

#[derive(Args, Debug)]
pub struct DemuxTitleNavArgs {
    /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
    #[arg(long = "disc")]
    pub disc: PathBuf,

    /// 1-based title number per libdvdnav.
    #[arg(long)]
    pub title: i32,

    /// Directory to write per-stream output files into.
    #[arg(long)]
    pub out_dir: PathBuf,

    /// Safety cap on event iterations.
    #[arg(long, default_value_t = 100_000_000)]
    pub max_events: u64,
}

pub fn run(args: DemuxTitleNavArgs) -> Result<()> {
    if args.title < 1 {
        return Err(anyhow!("--title must be >= 1"));
    }
    let disc_type = detect_disc_type(&args.disc).context("detect_disc_type")?;
    if !matches!(disc_type, DiscType::Dvd) {
        return Err(anyhow!(
            "demux-title-nav currently supports DVD only; detected {}",
            disc_type.as_str()
        ));
    }

    let source = DvdSource::open(&args.disc).context("DvdSource::open")?;
    let report = op::run(
        source.reader(),
        op::Params {
            title: args.title,
            out_dir: args.out_dir.clone(),
            max_events: args.max_events,
        },
    )?;

    let summary = &report.summary;
    let events = &report.events;
    let accounted = summary.pack_header_bytes
        + summary.stripped_header_bytes
        + summary.elementary_emitted_bytes
        + summary.dropped_pes_bytes;

    println!();
    println!(
        "demux-title-nav summary (title {}, libdvdnav driven -> {}):",
        args.title,
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
        "  cell changes:            {} (stc_discontinuity: {}, unresolved: {})",
        events.cell_changes,
        summary.discontinuity_boundaries,
        events.unresolved_cells,
    );

    println!();
    println!("nav event counts:");
    println!("  BLOCK_OK              {:>10}", events.blocks);
    println!("  NOP                   {:>10}", events.nops);
    println!("  STILL_FRAME           {:>10}", events.still_frames);
    println!("  SPU_STREAM_CHANGE     {:>10}", events.spu_stream_changes);
    println!("  AUDIO_STREAM_CHANGE   {:>10}", events.audio_stream_changes);
    println!("  VTS_CHANGE            {:>10}", events.vts_changes);
    println!("  CELL_CHANGE           {:>10}", events.cell_changes);
    println!("  NAV_PACKET            {:>10}", events.nav_packets);
    println!("  HIGHLIGHT             {:>10}", events.highlights);
    println!("  SPU_CLUT_CHANGE       {:>10}", events.spu_clut_changes);
    println!("  HOP_CHANNEL           {:>10}", events.hop_channels);
    println!("  WAIT                  {:>10}", events.waits);
    println!("  (unknown)             {:>10}", events.others);

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
