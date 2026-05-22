//! `disc-remuxer dump-title <path> --title N --out file.vob` — walk the
//! cells of a DVD title's PGC in playback order and write the
//! concatenated `TITLE_VOBS` sectors out as a single file.
//!
//! Title resolution mirrors the libdvdread convention:
//!
//! 1. VMG `tt_srpt[--title - 1]` → `(title_set_nr, vts_ttn)` for the
//!    title number the user passed.
//! 2. VTS IFO `vts_ptt_srpt.title[vts_ttn - 1]` → the title's chapter
//!    (PTT) array; chapter 1's `pgcn` selects the title's main PGC.
//! 3. VTS `vts_pgcit.pgci_srp[pgcn - 1].pgc` → the `pgc_t` itself.
//! 4. Walk `pgc.cell_playback[0..nr_of_cells]` via [`disc_dvd::cells_in_pgc`].
//!
//! For each cell the command runs the per-cell invariant checks from
//! [`disc_dvd::check_cell_walk`], reads `cell.first_sector..=cell.last_sector`
//! from the title's `TITLE_VOBS` stream via [`DvdFile::read_blocks`], and
//! writes the bytes to the output file. A SHA-256 of the concatenated
//! stream is logged at the end so the result can be byte-compared against
//! external dumps.
//!
//! This is the simple-linear walk: it follows IFO order regardless of
//! cell `block_type` / `seamless_angle` flags. Multi-angle and branching
//! titles will need the libdvdnav VM to drive cell selection — that
//! sits later on the roadmap (v1 milestone). The per-cell checks log a
//! warning when they detect non-monotonic sector ranges so the case is
//! visible in the job log.

use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_core::{check, check_eq, detect_disc_type, DiscType};
use disc_dvd::cell::{cells_in_pgc, check_cell_walk, dvd_time_seconds, CellInfo};
use disc_dvd::ifo::{format_dvd_time, IfoHandle, IfoKind};
use disc_dvd::{DvdFile, DvdSource, ReadDomain, BLOCK_SIZE};
use sha2::{Digest, Sha256};

/// Cap on a single `DVDReadBlocks` call. 512 blocks = 1 MiB; small
/// enough to keep peak memory low even for large cells, big enough to
/// amortise per-call overhead.
const READ_CHUNK_BLOCKS: u32 = 512;

#[derive(Args, Debug)]
pub struct DumpTitleArgs {
    /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
    pub path: PathBuf,

    /// 1-based title number to dump (as it appears in `tt_srpt`).
    #[arg(long)]
    pub title: u8,

    /// Output file path. Existing files are overwritten.
    #[arg(long)]
    pub out: PathBuf,
}

pub fn run(args: DumpTitleArgs) -> Result<()> {
    if args.title == 0 {
        return Err(anyhow!("--title must be >= 1 (1-based per tt_srpt)"));
    }

    let disc_type = detect_disc_type(&args.path).context("detect_disc_type")?;
    if !matches!(disc_type, DiscType::Dvd) {
        return Err(anyhow!(
            "dump-title currently supports DVD only; detected {}",
            disc_type.as_str()
        ));
    }

    log::info!(
        "dump-title path={} title={} out={}",
        args.path.display(),
        args.title,
        args.out.display(),
    );

    let source = DvdSource::open(&args.path).context("DvdSource::open")?;
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
            "title {} has nr_of_angles={nr_of_angles} — the simple cell walk only emits the IFO-order cells; multi-angle titles need libdvdnav navigation (roadmap)",
            args.title,
        );
    }

    let vts_ifo =
        IfoHandle::open(source.reader(), IfoKind::Vts(u32::from(title_set_nr)))
            .with_context(|| format!("opening VTS_{title_set_nr:02}_0.IFO"))?;

    // PTT chapter table → first chapter's pgcn picks the main PGC.
    let chapters = vts_ifo.chapters_for(vts_ttn);
    let first_chapter = chapters.first().ok_or_else(|| {
        anyhow!(
            "title {} has no PTT chapter entries (vts_ttn={vts_ttn}, VTS {title_set_nr})",
            args.title,
        )
    })?;
    let pgcn: u16 = { first_chapter.pgcn };
    let pgn: u16 = { first_chapter.pgn };
    log::info!(
        "title {} -> chapter 1 -> pgcn={pgcn} pgn={pgn} ({} chapters total)",
        args.title,
        chapters.len(),
    );

    let pgcs = vts_ifo.pgcs();
    let srp = pgcs.get(usize::from(pgcn).saturating_sub(1)).ok_or_else(|| {
        anyhow!(
            "pgcn {pgcn} out of range (VTS {title_set_nr} has {} PGCs in pgcit)",
            pgcs.len()
        )
    })?;
    let pgc_ptr = { srp.pgc };
    // SAFETY: libdvdread populates the PGC pointer when the IFO parses
    // successfully; NULL means the IFO is malformed for this slot.
    let pgc = unsafe { pgc_ptr.as_ref() }.ok_or_else(|| {
        anyhow!("pgc pointer for pgcn {pgcn} (VTS {title_set_nr}) is NULL")
    })?;

    let nr_of_cells: u8 = { pgc.nr_of_cells };
    let nr_of_programs: u8 = { pgc.nr_of_programs };
    let pgc_playback = { pgc.playback_time };
    log::info!(
        "PGC pgcn={pgcn}: nr_of_programs={nr_of_programs} nr_of_cells={nr_of_cells} playback_time={}",
        format_dvd_time(&pgc_playback),
    );

    // TITLE_VOBS holds the title's MPEG-PS sectors. `vts_nr` is the
    // VTS that owns the title (per `title_set_nr`).
    let title_vobs = DvdFile::open(
        source.reader(),
        u32::from(title_set_nr),
        ReadDomain::TitleVobs,
    )
    .with_context(|| {
        format!("opening TITLE_VOBS for VTS {title_set_nr}")
    })?;
    log::info!(
        "TITLE_VOBS vts={title_set_nr} block_count={} byte_size={}",
        title_vobs.block_count(),
        title_vobs.byte_size(),
    );

    let cells = cells_in_pgc(pgc);
    if cells.is_empty() {
        return Err(anyhow!(
            "PGC for title {} has no cells (nr_of_cells={nr_of_cells}, cell_playback={:p})",
            args.title,
            { pgc.cell_playback },
        ));
    }
    log::info!("walking {} cells in IFO order", cells.len());

    let mut writer = BufWriter::new(
        File::create(&args.out)
            .with_context(|| format!("creating {}", args.out.display()))?,
    );

    let mut hasher = Sha256::new();
    let mut total_bytes: u64 = 0;
    let mut total_blocks: u64 = 0;
    let mut sum_cell_seconds: u32 = 0;
    let mut prev_cell: Option<CellInfo> = None;
    let title_file_blocks = title_vobs.block_count();

    for cell in &cells {
        log::info!(
            "cell[{}/{}] first_sector={} last_sector={} block_count={} block_type={} block_mode={} interleaved={} stc_disc={} playback_time={}",
            cell.idx + 1,
            cells.len(),
            cell.first_sector,
            cell.last_sector,
            cell.block_count,
            cell.block_type,
            cell.block_mode,
            cell.interleaved,
            cell.stc_discontinuity,
            format_dvd_time(&cell.playback_time),
        );

        let _walk_ok = check_cell_walk(cell, prev_cell.as_ref(), title_file_blocks);

        // Read the cell in `READ_CHUNK_BLOCKS`-sized pieces. Doing this
        // in chunks keeps peak memory bounded for cells that span tens
        // of thousands of sectors (a single feature-length cell on a
        // DVD-9 can exceed 1 GiB).
        let mut blocks_remaining = cell.block_count;
        let mut offset = cell.first_sector;
        let mut cell_bytes: u64 = 0;
        let mut first_chunk_of_cell = true;
        while blocks_remaining > 0 {
            let chunk = blocks_remaining.min(READ_CHUNK_BLOCKS);
            let buf = title_vobs
                .read_blocks(offset, chunk)
                .with_context(|| {
                    format!(
                        "reading cell[{}] offset={offset} count={chunk}",
                        cell.idx,
                    )
                })?;

            // DVD-Video spec: every VOB sector starts with an MPEG-PS
            // pack-start code (00 00 01 BA), because each sector
            // contains exactly one MPEG-PS pack. Check the first
            // sector of each cell — if this fails on what should be a
            // cleartext disc, the cell-walk lookup is pointing into
            // the wrong byte range.
            if first_chunk_of_cell && buf.len() >= 4 {
                let head = [buf[0], buf[1], buf[2], buf[3]];
                check(
                    &format!("cell[{}] first sector starts with MPEG-PS pack-start", cell.idx),
                    "00 00 01 BA",
                    || head == [0x00, 0x00, 0x01, 0xBA],
                );
                first_chunk_of_cell = false;
            }

            hasher.update(&buf);
            writer.write_all(&buf).with_context(|| {
                format!("writing cell[{}] chunk to {}", cell.idx, args.out.display())
            })?;

            let n = buf.len() as u64;
            total_bytes += n;
            cell_bytes += n;
            total_blocks += u64::from(chunk);
            offset = offset.saturating_add(chunk);
            blocks_remaining -= chunk;
        }

        // Per-cell byte-count check: the cell's contribution to the file
        // is exactly its block_count * BLOCK_SIZE bytes.
        check_eq(
            &format!("cell[{}] bytes written == block_count * BLOCK_SIZE", cell.idx),
            cell_bytes,
            u64::from(cell.block_count) * u64::from(BLOCK_SIZE),
        );

        sum_cell_seconds = sum_cell_seconds.saturating_add(cell.playback_seconds());
        prev_cell = Some(*cell);
    }

    writer.flush().context("flushing output")?;
    drop(writer);

    // Aggregate post-walk checks.
    let expected_blocks: u64 = cells.iter().map(|c| u64::from(c.block_count)).sum();
    check_eq(
        "total_blocks == sum(cell.block_count)",
        total_blocks,
        expected_blocks,
    );
    check_eq(
        "total_bytes == total_blocks * BLOCK_SIZE",
        total_bytes,
        total_blocks * u64::from(BLOCK_SIZE),
    );

    // Duration cross-check (soft): sum-of-cells should approximate the
    // PGC's playback_time. BCD truncates frames, so tolerate a few
    // seconds of rounding (each cell can lose <1 s of frames).
    let pgc_seconds = dvd_time_seconds(&pgc_playback);
    let diff = sum_cell_seconds.abs_diff(pgc_seconds);
    let tol = u32::try_from(cells.len()).unwrap_or(u32::MAX).max(2);
    check(
        "sum(cell.playback_seconds) ~= pgc.playback_time",
        &format!(
            "sum_cells={sum_cell_seconds}s pgc={pgc_seconds}s diff={diff}s tol={tol}s"
        ),
        || diff <= tol,
    );

    let digest = hasher.finalize();
    log::info!("sha256 = {digest:x}");
    log::info!(
        "wrote {total_bytes} bytes ({total_blocks} blocks, {} cells) to {}",
        cells.len(),
        args.out.display(),
    );

    println!(
        "title {}: {} cells, {total_bytes} bytes ({total_blocks} blocks) -> {}",
        args.title,
        cells.len(),
        args.out.display(),
    );
    println!("sha256: {digest:x}");

    Ok(())
}
