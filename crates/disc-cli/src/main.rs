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
//! Logging: controlled by `RUST_LOG` (env_logger-style syntax). Defaults
//! to `info` if unset. Set `RUST_LOG=debug` for IFO + sector-read
//! lifecycle traces, `=trace` for byte-level detail. Subcommands that
//! produce output may additionally write a duplicate log to
//! `<output>.log` next to the output file — set `--log-file` to opt in.

use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

mod dump_sectors;
mod info;
mod logging;

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
    }
}
