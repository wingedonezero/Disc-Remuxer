//! `disc-remuxer demux-title-nav --disc <path> --title N --out-dir <dir>` —
//! the libdvdnav-driven demuxer. Same per-stream output as `demux-title`
//! (the manual cell walk) but with libdvdnav executing the disc's
//! authored PGC playback path.
//!
//! Flow:
//!
//! 1. Build a [`CellLookup`] from VMG + every VTS IFO. This is a flat
//!    `(title, pgcn, cellN) → CellInfo` table covering every cell on
//!    the disc.
//! 2. Open a [`DvdNav`], disable readahead, enable PGC positioning.
//! 3. Call `dvdnav_title_play(title)`.
//! 4. Loop on `next_block`:
//!    * `Block` → feed to demuxer.
//!    * `CellChange` → query `dvdnav_current_title_program` for the
//!      current `(title, pgcn)`, look up the cell in the table via
//!      `(title, pgcn, cellN)`, call `Demuxer::begin_cell` with the
//!      cell's `stc_discontinuity` flag.
//!    * `StillFrame` / `Wait` → acknowledge immediately.
//!    * `Stop` → break.
//!    * Other events are counted and ignored.
//! 5. Drop the demuxer to flush output files; print accounting.

use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_core::{check, check_eq, detect_disc_type, DiscType};
use disc_dvd::demux::{Demuxer, MagicCheck};
use disc_dvd::nav::{DvdNav, NavEvent};
use disc_dvd::nav_cells::CellLookup;
use disc_dvd::DvdSource;

const STILL_LENGTH_INFINITE: u8 = 0xFF;

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
    /// Cells the lookup table didn't have an entry for. Should stay 0
    /// on well-formed discs.
    unresolved_cells: u64,
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
    std::fs::create_dir_all(&args.out_dir)
        .with_context(|| format!("creating {}", args.out_dir.display()))?;

    log::info!(
        "demux-title-nav disc={} title={} out_dir={}",
        args.disc.display(),
        args.title,
        args.out_dir.display(),
    );

    // 1) Build the (title, pgcn, cellN) → CellInfo lookup once.
    let lookup = {
        let source = DvdSource::open(&args.disc).context("DvdSource::open (lookup)")?;
        CellLookup::build(source.reader()).context("CellLookup::build")?
    };
    log::info!(
        "cell lookup ready: {} entries (VTS for title {} = {:?})",
        lookup.len(),
        args.title,
        lookup.vts_for_title(args.title as u32),
    );

    // 2) Open the nav VM. Note: DvdNav opens its own libdvdread under
    // the hood; we don't share the source from step (1).
    let mut nav = DvdNav::open(&args.disc).context("DvdNav::open")?;
    nav.set_readahead(false).context("disable readahead")?;
    nav.set_pgc_positioning(true)
        .context("enable PGC positioning")?;

    let n_titles = nav.num_titles().context("num_titles")?;
    if args.title > n_titles {
        return Err(anyhow!(
            "--title {} out of range (disc has {n_titles} titles per dvdnav)",
            args.title
        ));
    }

    nav.title_play(args.title).context("dvdnav_title_play")?;

    let mut demuxer = Demuxer::new(&args.out_dir);
    let mut events = EventCounts::default();
    let mut left_title = false;
    let mut last_logged_cell: Option<(i32, i32)> = None; // (pgcn, cellN)

    for event_idx in 0..args.max_events {
        let evt = nav.next_block().with_context(|| {
            format!("dvdnav_get_next_block at event {event_idx}")
        })?;
        match evt {
            NavEvent::Block { sector } => {
                events.blocks += 1;
                let label = format!("nav block {}", events.blocks);
                demuxer
                    .process_sector(sector, &label)
                    .with_context(|| format!("demuxing block {}", events.blocks))?;
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
            NavEvent::CellChange { cell_nr, program_nr, .. } => {
                events.cell_changes += 1;
                // Query current title/pgcn so we can index into the
                // lookup. cellN comes from the event payload.
                let (cur_title, cur_pgcn, _cur_pgn) =
                    nav.current_title_program()
                        .context("dvdnav_current_title_program")?;
                if (cur_pgcn, cell_nr) != last_logged_cell.unwrap_or((-1, -1)) {
                    log::info!(
                        "cell change: title={cur_title} pgcn={cur_pgcn} cellN={cell_nr} (program {program_nr})"
                    );
                    last_logged_cell = Some((cur_pgcn, cell_nr));
                }
                let title_u = u32::try_from(cur_title).unwrap_or(0);
                let pgcn_u = u16::try_from(cur_pgcn).unwrap_or(0);
                let cell_u = u8::try_from(cell_nr).unwrap_or(0);
                match lookup.get(title_u, pgcn_u, cell_u) {
                    Some(cell) => {
                        log::debug!(
                            "lookup hit: vob_id_nr={} cell_nr={} stc_disc={} first_sector={} last_sector={}",
                            cell.vob_id_nr, cell.cell_nr,
                            cell.stc_discontinuity,
                            cell.first_sector, cell.last_sector,
                        );
                        demuxer.begin_cell(cell.stc_discontinuity);
                    }
                    None => {
                        events.unresolved_cells += 1;
                        log::warn!(
                            "no cell lookup for (title={cur_title}, pgcn={cur_pgcn}, cellN={cell_nr}) — treating as no-discontinuity"
                        );
                        // Safe default: assume no discontinuity. Audio
                        // stream resync won't fire, which matches the
                        // 5a behavior for this boundary.
                        demuxer.begin_cell(false);
                    }
                }
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
                log::info!("hop channel");
            }
            NavEvent::Wait => {
                events.waits += 1;
                nav.wait_skip().context("dvdnav_wait_skip")?;
            }
            NavEvent::Other(code) => {
                events.others += 1;
                log::warn!("unknown nav event code {code}");
            }
        }

        // After processing the event, check whether we've left the
        // requested title — same stop condition as dump-title-nav.
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
            Ok(_) | Err(_) => {}
        }
        if left_title {
            break;
        }
    }

    let summary = demuxer.finish().context("finalizing demuxer")?;

    let accounted = summary.pack_header_bytes
        + summary.stripped_header_bytes
        + summary.elementary_emitted_bytes
        + summary.dropped_pes_bytes;
    check_eq(
        "demux-title-nav: byte accounting closes",
        accounted,
        summary.input_bytes,
    );
    check(
        "demux-title-nav: no unresolved cells",
        &format!("unresolved_cells={}", events.unresolved_cells),
        || events.unresolved_cells == 0,
    );

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
