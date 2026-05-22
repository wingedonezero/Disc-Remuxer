//! `disc-remuxer demux-title --disc <path> --title N --out-dir <dir>` —
//! walk a title's PGC cells directly from disc and demultiplex each
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
use disc_core::{check, check_eq, detect_disc_type, DiscType};
use disc_dvd::cell::{
    cells_in_pgc, check_cell_vs_c_adt, check_cell_walk, CellInfo,
};
use disc_dvd::demux::{Demuxer, MagicCheck};
use disc_dvd::ifo::{format_dvd_time, IfoHandle, IfoKind};
use disc_dvd::mpegps::SECTOR_SIZE;
use disc_dvd::{DvdFile, DvdSource, ReadDomain, BLOCK_SIZE};

/// Read 512 blocks (= 1 MiB) per `read_blocks` call. Same chunking as
/// `dump-title` so the two share the same I/O cadence.
const READ_CHUNK_BLOCKS: u32 = 512;

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

    std::fs::create_dir_all(&args.out_dir)
        .with_context(|| format!("creating {}", args.out_dir.display()))?;

    log::info!(
        "demux-title disc={} title={} out_dir={}",
        args.disc.display(),
        args.title,
        args.out_dir.display(),
    );

    let source = DvdSource::open(&args.disc).context("DvdSource::open")?;
    let vmg = IfoHandle::open(source.reader(), IfoKind::Vmg)
        .context("opening VMG IFO")?;

    let titles = vmg.titles();
    let title_idx = usize::from(args.title - 1);
    let title = titles.get(title_idx).ok_or_else(|| {
        anyhow!(
            "--title {} out of range (disc has {} titles in tt_srpt)",
            args.title,
            titles.len()
        )
    })?;
    let title_set_nr: u8 = { title.title_set_nr };
    let vts_ttn: u8 = { title.vts_ttn };
    let nr_of_ptts: u16 = { title.nr_of_ptts };
    let nr_of_angles: u8 = { title.nr_of_angles };
    log::info!(
        "title {}: title_set_nr={title_set_nr} vts_ttn={vts_ttn} nr_of_ptts={nr_of_ptts} nr_of_angles={nr_of_angles}",
        args.title,
    );
    if nr_of_angles > 1 {
        log::warn!(
            "title {} has nr_of_angles={nr_of_angles} — simple cell walk only emits IFO-order cells",
            args.title,
        );
    }

    let vts_ifo =
        IfoHandle::open(source.reader(), IfoKind::Vts(u32::from(title_set_nr)))
            .with_context(|| format!("opening VTS_{title_set_nr:02}_0.IFO"))?;

    let chapters = vts_ifo.chapters_for(vts_ttn);
    let first_chapter = chapters.first().ok_or_else(|| {
        anyhow!(
            "title {} has no PTT chapter entries (vts_ttn={vts_ttn})",
            args.title
        )
    })?;
    let pgcn: u16 = { first_chapter.pgcn };
    log::info!(
        "title {} -> chapter 1 -> pgcn={pgcn} ({} chapters total)",
        args.title,
        chapters.len(),
    );

    let pgcs = vts_ifo.pgcs();
    let srp = pgcs.get(usize::from(pgcn).saturating_sub(1)).ok_or_else(|| {
        anyhow!(
            "pgcn {pgcn} out of range (VTS {title_set_nr} has {} PGCs)",
            pgcs.len()
        )
    })?;
    let pgc_ptr = { srp.pgc };
    // SAFETY: see dump_title.rs — libdvdread populates the PGC pointer
    // when the IFO parses successfully.
    let pgc = unsafe { pgc_ptr.as_ref() }
        .ok_or_else(|| anyhow!("pgc pointer for pgcn {pgcn} is NULL"))?;

    let pgc_playback = { pgc.playback_time };
    log::info!(
        "PGC pgcn={pgcn}: nr_of_cells={} playback_time={}",
        { pgc.nr_of_cells },
        format_dvd_time(&pgc_playback),
    );

    let title_vobs = DvdFile::open(
        source.reader(),
        u32::from(title_set_nr),
        ReadDomain::TitleVobs,
    )
    .with_context(|| format!("opening TITLE_VOBS for VTS {title_set_nr}"))?;
    log::info!(
        "TITLE_VOBS vts={title_set_nr} block_count={} byte_size={}",
        title_vobs.block_count(),
        title_vobs.byte_size(),
    );

    let cells = cells_in_pgc(pgc);
    if cells.is_empty() {
        return Err(anyhow!("PGC for title {} has no cells", args.title));
    }
    log::info!("walking {} cells in IFO order", cells.len());

    let c_adt_rows = vts_ifo.cell_adr_table();
    log::info!(
        "vts_c_adt: {} cell-address entries available for cross-check",
        c_adt_rows.len()
    );

    let mut demuxer = Demuxer::new(&args.out_dir);
    let mut prev_cell: Option<CellInfo> = None;
    let mut total_blocks_read: u64 = 0;
    let title_file_blocks = title_vobs.block_count();

    for cell in &cells {
        log::info!(
            "cell[{}/{}] vob_id_nr={} cell_nr={} first_sector={} last_sector={} block_count={} stc_disc={} playback_time={}",
            cell.idx + 1,
            cells.len(),
            cell.vob_id_nr,
            cell.cell_nr,
            cell.first_sector,
            cell.last_sector,
            cell.block_count,
            cell.stc_discontinuity,
            format_dvd_time(&cell.playback_time),
        );

        let _walk_ok = check_cell_walk(cell, prev_cell.as_ref(), title_file_blocks);
        if !c_adt_rows.is_empty() {
            let _c_adt_ok = check_cell_vs_c_adt(cell, c_adt_rows);
        }

        // Tell the demuxer a new cell is starting BEFORE feeding any of
        // its sectors. On stc_discontinuity=true the demuxer will mark
        // known AC-3/DTS streams for FAP-based resync on their next
        // PES.
        demuxer.begin_cell(cell.stc_discontinuity);

        let mut blocks_remaining = cell.block_count;
        let mut offset = cell.first_sector;
        let mut sector_in_cell: u32 = 0;
        while blocks_remaining > 0 {
            let chunk = blocks_remaining.min(READ_CHUNK_BLOCKS);
            let buf = title_vobs.read_blocks(offset, chunk).with_context(|| {
                format!(
                    "reading cell[{}] offset={offset} count={chunk}",
                    cell.idx
                )
            })?;
            for sector_idx in 0..chunk {
                let start = (sector_idx as usize) * SECTOR_SIZE;
                let end = start + SECTOR_SIZE;
                let sector = &buf[start..end];
                let global_sector = total_blocks_read + u64::from(sector_in_cell);
                let label = format!(
                    "cell {} (lba {}, in-cell {})",
                    cell.idx + 1,
                    cell.first_sector + sector_in_cell,
                    sector_in_cell,
                );
                demuxer
                    .process_sector(sector, &label)
                    .with_context(|| format!("demuxing sector {global_sector}"))?;
                sector_in_cell += 1;
            }
            total_blocks_read += u64::from(chunk);
            offset = offset.saturating_add(chunk);
            blocks_remaining -= chunk;
        }

        prev_cell = Some(*cell);
    }

    let summary = demuxer.finish().context("finalizing demuxer")?;

    // Cross-check: blocks we fed match what we read.
    check_eq(
        "demux-title: blocks read == sectors processed",
        total_blocks_read,
        summary.sectors_processed,
    );

    let accounted = summary.pack_header_bytes
        + summary.stripped_header_bytes
        + summary.elementary_emitted_bytes
        + summary.dropped_pes_bytes;
    check_eq(
        "demux-title: byte accounting closes",
        accounted,
        summary.input_bytes,
    );

    let cells_count = cells.len() as u64;
    let expected_discontinuities =
        cells.iter().filter(|c| c.stc_discontinuity).count() as u64;
    check(
        "demux-title: observed discontinuities match cell flags",
        &format!(
            "summary.discontinuity_boundaries={} expected={}",
            summary.discontinuity_boundaries, expected_discontinuities
        ),
        || summary.discontinuity_boundaries == expected_discontinuities,
    );

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

    // Suppress unused-import warning when BLOCK_SIZE isn't used in some
    // build configs.
    let _ = BLOCK_SIZE;
    Ok(())
}
