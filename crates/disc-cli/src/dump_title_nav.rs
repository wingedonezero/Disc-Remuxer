//! `disc-remuxer dump-title-nav --disc <path> --title N --out file.vob`
//! — like `dump-title` but drives the rip via libdvdnav instead of the
//! manual `cell_playback[0..]` walk.
//!
//! This is the step-6 minimal integration: it proves the libdvdnav
//! sector source produces the same bytes our manual cell walk does for
//! simple titles (no multi-angle, no protection traps). Once that
//! equivalence is shown, future commits will pipe libdvdnav into the
//! demuxer (so we get the protection-bypass benefits on real-world
//! discs) and add cell-info lookups so we can drive
//! `Demuxer::begin_cell` correctly from nav events.

use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_core::check_eq;
use disc_dvd::nav::{DvdNav, NavEvent};
use sha2::{Digest, Sha256};

/// libdvdnav's still-frame `length` byte. `0xFF` means "indefinite";
/// for ripping we always immediately advance past stills, indefinite
/// or not.
const STILL_LENGTH_INFINITE: u8 = 0xFF;

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

#[derive(Default)]
struct EventCounts {
    blocks: u64,
    nops: u64,
    still_frames: u64,
    spu_stream_changes: u64,
    audio_stream_changes: u64,
    vts_changes: u64,
    cell_changes: u64,
    nav_packets: u64,
    highlights: u64,
    spu_clut_changes: u64,
    hop_channels: u64,
    waits: u64,
    others: u64,
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

    let mut nav = DvdNav::open(&args.disc).context("DvdNav::open")?;
    nav.set_readahead(false).context("disable readahead")?;
    nav.set_pgc_positioning(true)
        .context("enable PGC positioning")?;

    let n_titles = nav.num_titles().context("num_titles")?;
    log::info!("dvdnav reports {n_titles} titles");
    if args.title > n_titles {
        return Err(anyhow!(
            "--title {} out of range (disc has {n_titles} titles per dvdnav)",
            args.title
        ));
    }
    if let Ok(n_parts) = nav.num_parts(args.title) {
        log::info!("title {} has {n_parts} parts (chapters)", args.title);
    }

    nav.title_play(args.title).context("dvdnav_title_play")?;

    let out_file = File::create(&args.out)
        .with_context(|| format!("creating {}", args.out.display()))?;
    let mut writer = BufWriter::with_capacity(64 * 1024, out_file);
    let mut hasher = Sha256::new();
    let mut total_bytes: u64 = 0;
    let mut total_blocks: u64 = 0;
    let mut events = EventCounts::default();
    let mut last_title_logged: i32 = args.title;
    let mut left_title = false;

    for event_idx in 0..args.max_events {
        let evt = nav.next_block().with_context(|| {
            format!("dvdnav_get_next_block at event {event_idx}")
        })?;

        match evt {
            NavEvent::Block { sector } => {
                events.blocks += 1;
                hasher.update(sector);
                writer.write_all(sector).context("writing sector")?;
                total_bytes += sector.len() as u64;
                total_blocks += 1;
            }
            NavEvent::Nop => events.nops += 1,
            NavEvent::StillFrame { length } => {
                events.still_frames += 1;
                if length == STILL_LENGTH_INFINITE {
                    log::debug!("still frame: indefinite — skipping");
                } else {
                    log::debug!("still frame: {length}s — skipping");
                }
                nav.still_skip().context("dvdnav_still_skip")?;
            }
            NavEvent::SpuStreamChange => events.spu_stream_changes += 1,
            NavEvent::AudioStreamChange => events.audio_stream_changes += 1,
            NavEvent::VtsChange => {
                events.vts_changes += 1;
                log::info!("VTS change");
            }
            NavEvent::CellChange {
                cell_nr,
                program_nr,
                cell_length_pts,
                program_length_pts,
                pgc_length_pts,
                ..
            } => {
                events.cell_changes += 1;
                log::info!(
                    "cell change: cellN={cell_nr} pgN={program_nr} cell_len_pts={cell_length_pts} pg_len_pts={program_length_pts} pgc_len_pts={pgc_length_pts}"
                );
            }
            NavEvent::NavPacket => events.nav_packets += 1,
            NavEvent::Stop => {
                log::info!("DVDNAV_STOP at event {event_idx}");
                break;
            }
            NavEvent::Highlight => events.highlights += 1,
            NavEvent::SpuClutChange => events.spu_clut_changes += 1,
            NavEvent::HopChannel => {
                events.hop_channels += 1;
                log::info!("hop channel — VM jumped");
            }
            NavEvent::Wait => {
                events.waits += 1;
                log::debug!("wait — acknowledging");
                nav.wait_skip().context("dvdnav_wait_skip")?;
            }
            NavEvent::Other(code) => {
                events.others += 1;
                log::warn!("unknown nav event code {code}");
            }
        }

        // After processing the event, check whether libdvdnav has
        // moved us out of the requested title (e.g. rolled into the
        // next title or back to first-play). If so, stop.
        match nav.current_title_part() {
            Ok((t, _)) if t != args.title => {
                if !left_title {
                    log::info!(
                        "left title {} -> currently in title {}; stopping",
                        args.title, t
                    );
                    left_title = true;
                }
            }
            Ok((t, _)) => {
                if t != last_title_logged {
                    log::debug!("now in title {t}");
                    last_title_logged = t;
                }
            }
            Err(_) => {}
        }
        if left_title {
            break;
        }
    }

    writer.flush().context("flushing output")?;
    drop(writer);

    // Sanity check: blocks counted == bytes / 2048.
    check_eq(
        "dump-title-nav: blocks == bytes / 2048",
        total_blocks,
        total_bytes / 2048,
    );

    let digest = hasher.finalize();
    log::info!("sha256 = {digest:x}");
    log::info!(
        "wrote {total_bytes} bytes ({total_blocks} sectors) to {}",
        args.out.display()
    );

    println!();
    println!(
        "dump-title-nav (title {}, libdvdnav driven):",
        args.title
    );
    println!("  sectors written:         {total_blocks}");
    println!("  bytes written:           {total_bytes}");
    println!("  output:                  {}", args.out.display());
    println!("  sha256:                  {digest:x}");
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

    Ok(())
}
