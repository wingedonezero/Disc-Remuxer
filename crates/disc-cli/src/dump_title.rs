//! `disc-remuxer dump-title <path> --title N --out file.vob` — thin CLI
//! shell over [`disc_dvd::ops::dump_title`].
//!
//! Walks the cells of a DVD title's PGC in playback order and writes the
//! concatenated `TITLE_VOBS` sectors out as a single file. Title
//! resolution, the cell walk, the per-cell invariant checks, and the
//! SHA-256 all live in the op module; this shell only validates inputs,
//! guards the disc type, opens the disc, and prints the report.

use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_core::{detect_disc_type, DiscType};
use disc_dvd::ops::dump_title as op;
use disc_dvd::DvdSource;

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
    let report = op::run(
        source.reader(),
        op::Params {
            title: args.title,
            out: args.out.clone(),
        },
    )?;

    println!(
        "title {}: {} cells, {} bytes ({} blocks) -> {}",
        args.title,
        report.cells,
        report.total_bytes,
        report.total_blocks,
        args.out.display(),
    );
    println!("sha256: {}", report.sha256);

    Ok(())
}
