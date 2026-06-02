//! `disc-remuxer` CLI entry point.
//!
//! User commands (format-agnostic — they detect the disc type and delegate
//! to the right backend):
//!
//! * `info <path>` — dump the disc's metadata + title list.
//! * `list <path>` — print the uniform title/track tree the `rip` selectors
//!   operate on (per-title index, per-track audio/subtitle index, codec,
//!   language). The index map the future UI mirrors.
//! * `rip --disc <path> --out-dir <dir> [--title …] [--audio …]
//!   [--subtitle …] [--min-length N]` — rip selected titles + tracks
//!   (default: every title ≥ min-length, all tracks, in order).
//!
//! `dvd <tool>` — DVD-specific low-level tools for debugging / verification.
//! These are **not** the rip pipeline: `rip` uses libdvdnav plus its own
//! per-stream handlers; these expose individual layers (and the older manual
//! cell-walk) so each can be tested in isolation.
//!
//! Logging: controlled by `RUST_LOG` (env_logger-style). Defaults to `info`.
//! `RUST_LOG=debug` for IFO + sector-read traces, `=trace` for byte detail.
//! `--log-file <path>` mirrors the log to a file.

use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

mod demux_title;
mod demux_title_nav;
mod demux_vob;
mod dump_sectors;
mod dump_title;
mod dump_title_nav;
mod info;
mod list;
mod logging;
mod rip;
mod scan_streams;

#[derive(Parser)]
#[command(name = "disc-remuxer", version, about, long_about = None)]
struct Cli {
    /// Write a duplicate of the log to this file (in addition to stderr).
    /// Useful when shipping a job archive back for debugging.
    #[arg(long, global = true)]
    log_file: Option<PathBuf>,

    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Open a disc and print its metadata + title list.
    Info {
        /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
        path: PathBuf,
    },

    /// Print the uniform title/track tree the `rip` selectors operate on
    /// (the index map: per-title index, per-track audio/subtitle indices,
    /// codec, language). What the future UI displays + checkmarks.
    List {
        /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
        path: PathBuf,

        /// Minimum title length, seconds — titles below this are shown
        /// as `[skipped: MinLength]`. `0` = mark none. Default matches
        /// makemkvcon (120).
        #[arg(long, default_value_t = 120)]
        min_length: u64,
    },

    /// Rip selected titles + tracks. Default = all titles ≥ --min-length,
    /// all tracks, in order; narrow with --title / --audio / --subtitle.
    Rip(rip::RipArgs),

    /// DVD-specific low-level tools (debugging / verification). NOT the rip
    /// pipeline — these expose individual layers + the manual cell-walk.
    Dvd {
        #[command(subcommand)]
        tool: DvdTool,
    },
}

/// Low-level DVD diagnostics, grouped under `dvd` so they don't clutter the
/// base commands. `rip` is the real pipeline; these isolate single stages.
#[derive(Subcommand)]
enum DvdTool {
    /// Read raw sectors from a VOB stream and write them to disk (+ CSS).
    DumpSectors(dump_sectors::DumpSectorsArgs),

    /// Scan an MPEG-PS sector file and report per-stream byte counts.
    ScanStreams(scan_streams::ScanStreamsArgs),

    /// Demultiplex an MPEG-PS sector file (generic Demuxer) into streams.
    DemuxVob(demux_vob::DemuxVobArgs),

    /// Walk a title's PGC cells (manual) and dump the concatenated VOB.
    DumpTitle(dump_title::DumpTitleArgs),

    /// Demultiplex a title's PGC cells (manual walk) into per-stream files.
    DemuxTitle(demux_title::DemuxTitleArgs),

    /// Dump a title via libdvdnav (executes PGC commands).
    DumpTitleNav(dump_title_nav::DumpTitleNavArgs),

    /// Demultiplex a title via libdvdnav (per-CellChange metadata lookup).
    DemuxTitleNav(demux_title_nav::DemuxTitleNavArgs),
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    // `_logger_handle` must live for the rest of `main` — dropping it
    // shuts down flexi_logger's background flush.
    let _logger_handle = logging::init(cli.log_file.as_deref())?;

    match cli.command {
        Command::Info { path } => {
            info::run(&path).with_context(|| format!("info {}", path.display()))
        }
        Command::List { path, min_length } => {
            list::run(&path, min_length)
                .with_context(|| format!("list {}", path.display()))
        }
        Command::Rip(args) => {
            let path_disp = args.disc.display().to_string();
            rip::run(args).with_context(|| format!("rip {path_disp}"))
        }
        Command::Dvd { tool } => run_dvd_tool(tool),
    }
}

/// Dispatch the `dvd <tool>` diagnostics. Each is a thin shell over a
/// `disc_dvd::ops::*` function; see the per-module docs for what it isolates.
fn run_dvd_tool(tool: DvdTool) -> Result<()> {
    match tool {
        DvdTool::DumpSectors(args) => {
            let path_disp = args.path.display().to_string();
            dump_sectors::run(args)
                .with_context(|| format!("dvd dump-sectors {path_disp}"))
        }
        DvdTool::ScanStreams(args) => {
            let path_disp = args.path.display().to_string();
            scan_streams::run(args)
                .with_context(|| format!("dvd scan-streams {path_disp}"))
        }
        DvdTool::DemuxVob(args) => {
            let path_disp = args.path.display().to_string();
            demux_vob::run(args)
                .with_context(|| format!("dvd demux-vob {path_disp}"))
        }
        DvdTool::DumpTitle(args) => {
            let path_disp = args.path.display().to_string();
            let title = args.title;
            dump_title::run(args)
                .with_context(|| format!("dvd dump-title {path_disp} title={title}"))
        }
        DvdTool::DemuxTitle(args) => {
            let path_disp = args.disc.display().to_string();
            let title = args.title;
            demux_title::run(args)
                .with_context(|| format!("dvd demux-title {path_disp} title={title}"))
        }
        DvdTool::DumpTitleNav(args) => {
            let path_disp = args.disc.display().to_string();
            let title = args.title;
            dump_title_nav::run(args).with_context(|| {
                format!("dvd dump-title-nav {path_disp} title={title}")
            })
        }
        DvdTool::DemuxTitleNav(args) => {
            let path_disp = args.disc.display().to_string();
            let title = args.title;
            demux_title_nav::run(args).with_context(|| {
                format!("dvd demux-title-nav {path_disp} title={title}")
            })
        }
    }
}
