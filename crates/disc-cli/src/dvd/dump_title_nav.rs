//! `disc-remuxer dump-title-nav --disc <path> --title N --out file.vob`
//! — thin CLI shell over [`disc_dvd::ops::dump_title_nav`].
//!
//! Like `dump-title` but drives the rip via libdvdnav instead of the
//! manual `cell_playback[0..]` walk.
//!
//! This is the step-6 minimal integration: it proves the libdvdnav
//! sector source produces the same bytes our manual cell walk does for
//! simple titles (no multi-angle, no protection traps). Once that
//! equivalence is shown, future commits will pipe libdvdnav into the
//! demuxer (so we get the protection-bypass benefits on real-world
//! discs) and add cell-info lookups so we can drive
//! `Demuxer::begin_cell` correctly from nav events.

use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_dvd::ops::dump_title_nav as op;
use disc_dvd::DvdSource;

#[derive(Args, Debug)]
pub struct DumpTitleNavArgs {
    /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
    #[arg(long = "disc")]
    pub disc: PathBuf,

    /// 1-based title number per libdvdnav's `dvdnav_get_number_of_titles`.
    /// (Usually matches libdvdread's `tt_srpt` index but the mapping
    /// can differ on discs with first-play / VMG-menu titles.)
    #[arg(long)]
    pub title: i32,

    /// Output file path. Existing files are overwritten.
    #[arg(long)]
    pub out: PathBuf,

    /// Safety cap on iteration. Default = 100M events (more than any
    /// real DVD would produce). Set lower to bound runaway VM loops.
    #[arg(long, default_value_t = 100_000_000)]
    pub max_events: u64,
}

pub fn run(args: DumpTitleNavArgs) -> Result<()> {
    if args.title < 1 {
        return Err(anyhow!("--title must be >= 1"));
    }
    log::info!(
        "dump-title-nav disc={} title={} out={}",
        args.disc.display(),
        args.title,
        args.out.display(),
    );

    let source = DvdSource::open(&args.disc).context("DvdSource::open")?;
    let report = op::run(
        source.reader(),
        op::Params {
            title: args.title,
            out: args.out.clone(),
            max_events: args.max_events,
        },
    )?;

    println!();
    println!(
        "dump-title-nav (title {}, libdvdnav driven):",
        report.title
    );
    println!("  sectors written:         {}", report.total_blocks);
    println!("  bytes written:           {}", report.total_bytes);
    println!("  output:                  {}", report.out.display());
    println!("  sha256:                  {}", report.sha256);
    println!();
    println!("nav event counts:");
    println!("  BLOCK_OK              {:>10}", report.events.blocks);
    println!("  NOP                   {:>10}", report.events.nops);
    println!("  STILL_FRAME           {:>10}", report.events.still_frames);
    println!("  SPU_STREAM_CHANGE     {:>10}", report.events.spu_stream_changes);
    println!("  AUDIO_STREAM_CHANGE   {:>10}", report.events.audio_stream_changes);
    println!("  VTS_CHANGE            {:>10}", report.events.vts_changes);
    println!("  CELL_CHANGE           {:>10}", report.events.cell_changes);
    println!("  NAV_PACKET            {:>10}", report.events.nav_packets);
    println!("  HIGHLIGHT             {:>10}", report.events.highlights);
    println!("  SPU_CLUT_CHANGE       {:>10}", report.events.spu_clut_changes);
    println!("  HOP_CHANNEL           {:>10}", report.events.hop_channels);
    println!("  WAIT                  {:>10}", report.events.waits);
    println!("  (unknown)             {:>10}", report.events.others);

    Ok(())
}
