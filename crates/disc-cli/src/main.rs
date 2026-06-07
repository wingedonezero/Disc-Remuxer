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
//! These are **not** the rip pipeline; their source lives under `src/dvd/`,
//! out of the base commands. See [`dvd`] for what each isolates.
//!
//! Logging: controlled by `RUST_LOG` (env_logger-style). Defaults to `info`.
//! `RUST_LOG=debug` for IFO + sector-read traces, `=trace` for byte detail.
//! `--log-file <path>` mirrors the log to a file.

use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

mod dvd;
mod info;
mod list;
mod logging;
mod rip;

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
        tool: dvd::DvdTool,
    },
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
        Command::Dvd { tool } => dvd::run(tool),
    }
}
