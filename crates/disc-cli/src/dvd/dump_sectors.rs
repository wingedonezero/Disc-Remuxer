//! `disc-remuxer dump-sectors` — thin CLI shell over
//! [`disc_dvd::ops::dump_sectors`].
//!
//! Primarily a verification tool: dump a known range and hash it, then
//! compare against the same range read via another path (e.g. `dd` of an
//! ISO) to confirm the sector-read layer is wired up correctly.

use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::Args;
use disc_dvd::ops::dump_sectors as op;
use disc_dvd::{DvdSource, ReadDomain};

#[derive(Args, Debug)]
pub struct DumpSectorsArgs {
    /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
    pub path: PathBuf,

    /// Video Title Set number (1..=99 for VTS files). 0 means the
    /// disc-wide `VIDEO_TS.VOB` (when used with `--domain menu`).
    #[arg(long)]
    pub vts: u32,

    /// Which file domain to read from.
    #[arg(long, default_value = "title")]
    pub domain: DomainArg,

    /// Starting block offset within the file.
    #[arg(long, default_value_t = 0)]
    pub offset: u32,

    /// Number of 2048-byte blocks to read.
    #[arg(long, default_value_t = 16)]
    pub count: u32,

    /// File path to write the raw sectors to.
    #[arg(long)]
    pub out: PathBuf,
}

#[derive(clap::ValueEnum, Debug, Clone, Copy)]
pub enum DomainArg {
    /// `VTS_NN_[1-9].VOB` concatenated — the actual title content.
    Title,
    /// `VIDEO_TS.VOB` / `VTS_NN_0.VOB` — menu content.
    Menu,
}

impl From<DomainArg> for ReadDomain {
    fn from(d: DomainArg) -> Self {
        match d {
            DomainArg::Title => ReadDomain::TitleVobs,
            DomainArg::Menu => ReadDomain::MenuVobs,
        }
    }
}

pub fn run(args: DumpSectorsArgs) -> Result<()> {
    log::info!(
        "dump-sectors path={} vts={} domain={:?} offset={} count={} out={}",
        args.path.display(),
        args.vts,
        args.domain,
        args.offset,
        args.count,
        args.out.display(),
    );

    let source = DvdSource::open(&args.path).context("DvdSource::open")?;
    let report = op::run(
        source.reader(),
        op::Params {
            vts: args.vts,
            domain: args.domain.into(),
            offset: args.offset,
            count: args.count,
            out: args.out.clone(),
        },
    )?;

    println!("wrote {} bytes to {}", report.bytes_written, report.out.display());
    println!("sha256: {}", report.sha256);
    Ok(())
}
