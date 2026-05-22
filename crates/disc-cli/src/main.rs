//! `disc-remuxer` CLI entry point.
//!
//! Current commands:
//!
//! * `info <path>` — open a disc and dump everything we can read from the
//!   IFOs: disc metadata, title list, per-VTS PGC counts. Equivalent in
//!   spirit to `lsdvd`, with libdvdread-style field names.
//!
//! * `dump-sectors <path> --vts N --offset M --count K --out file.bin`
//!   — read raw sectors from a VOB stream and write them to disk, for
//!   verification against external tools (`dd`, hash-compare, etc.).
//!
//! * `dump-title <path> --title N --out file.vob` — walk the cells of
//!   title `N`'s PGC in IFO order, concatenate the `TITLE_VOBS` sector
//!   ranges into the output file. Logs per-cell checks and a final
//!   SHA-256 for byte-compare against external dumps.
//!
//! * `scan-streams <vob_path>` — walk an MPEG-PS sector stream (e.g.
//!   the output of `dump-title`) and report per-stream packet + byte
//!   counts. Verifies the MPEG-PS pack/PES parser before demux.
//!
//! Logging: controlled by `RUST_LOG` (env_logger-style syntax). Defaults
//! to `info` if unset. Set `RUST_LOG=debug` for IFO + sector-read
//! lifecycle traces, `=trace` for byte-level detail. Subcommands that
//! produce output may additionally write a duplicate log to
//! `<output>.log` next to the output file — set `--log-file` to opt in.

use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

mod dump_sectors;
mod dump_title;
mod info;
mod logging;
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

    /// Read raw sectors from a VOB stream and write them to disk.
    DumpSectors(dump_sectors::DumpSectorsArgs),

    /// Walk a title's PGC cells and dump the concatenated VOB stream.
    DumpTitle(dump_title::DumpTitleArgs),

    /// Scan an MPEG-PS sector stream and report per-stream byte counts.
    ScanStreams(scan_streams::ScanStreamsArgs),
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
        Command::DumpSectors(args) => {
            let path_disp = args.path.display().to_string();
            dump_sectors::run(args)
                .with_context(|| format!("dump-sectors {path_disp}"))
        }
        Command::DumpTitle(args) => {
            let path_disp = args.path.display().to_string();
            let title = args.title;
            dump_title::run(args)
                .with_context(|| format!("dump-title {path_disp} title={title}"))
        }
        Command::ScanStreams(args) => {
            let path_disp = args.path.display().to_string();
            scan_streams::run(args)
                .with_context(|| format!("scan-streams {path_disp}"))
        }
    }
}
