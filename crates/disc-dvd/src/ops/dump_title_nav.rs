//! `dump-title-nav` operation — like `dump-title` but drives the rip via
//! libdvdnav instead of the manual `cell_playback[0..]` walk.
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
use disc_core::check_eq;
use sha2::{Digest, Sha256};

use crate::nav::{DvdNav, NavEvent};
use crate::DvdReader;

/// libdvdnav's still-frame `length` byte. `0xFF` means "indefinite";
/// for ripping we always immediately advance past stills, indefinite
/// or not.
const STILL_LENGTH_INFINITE: u8 = 0xFF;

#[derive(Debug)]
pub struct Params {
    pub title: i32,
    pub out: PathBuf,
    pub max_events: u64,
}

#[derive(Debug)]
pub struct Report {
    pub title: i32,
    pub total_blocks: u64,
    pub total_bytes: u64,
    pub out: PathBuf,
    pub sha256: String,
    pub events: EventCounts,
}

#[derive(Default, Debug)]
pub struct EventCounts {
    pub blocks: u64,
    pub nops: u64,
    pub still_frames: u64,
    pub spu_stream_changes: u64,
    pub audio_stream_changes: u64,
    pub vts_changes: u64,
    pub cell_changes: u64,
    pub nav_packets: u64,
    pub highlights: u64,
    pub spu_clut_changes: u64,
    pub hop_channels: u64,
    pub waits: u64,
    pub others: u64,
}

pub fn run(reader: &DvdReader, params: Params) -> Result<Report> {
    let mut nav = DvdNav::open(reader.path()).context("DvdNav::open")?;
    nav.set_readahead(false).context("disable readahead")?;
    nav.set_pgc_positioning(true)
        .context("enable PGC positioning")?;

    let n_titles = nav.num_titles().context("num_titles")?;
    log::info!("dvdnav reports {n_titles} titles");
    if params.title > n_titles {
        return Err(anyhow!(
            "--title {} out of range (disc has {n_titles} titles per dvdnav)",
            params.title
        ));
    }
    if let Ok(n_parts) = nav.num_parts(params.title) {
        log::info!("title {} has {n_parts} parts (chapters)", params.title);
    }

    nav.title_play(params.title).context("dvdnav_title_play")?;

    let out_file = File::create(&params.out)
        .with_context(|| format!("creating {}", params.out.display()))?;
    let mut writer = BufWriter::with_capacity(64 * 1024, out_file);
    let mut hasher = Sha256::new();
    let mut total_bytes: u64 = 0;
    let mut total_blocks: u64 = 0;
    let mut events = EventCounts::default();
    let mut last_title_logged: i32 = params.title;
    let mut left_title = false;

    for event_idx in 0..params.max_events {
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
            Ok((t, _)) if t != params.title => {
                if !left_title {
                    log::info!(
                        "left title {} -> currently in title {}; stopping",
                        params.title, t
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
        params.out.display()
    );

    let sha256 = format!("{digest:x}");

    Ok(Report {
        title: params.title,
        total_blocks,
        total_bytes,
        out: params.out,
        sha256,
        events,
    })
}
