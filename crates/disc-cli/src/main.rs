//! `disc-remuxer` CLI entry point.
//!
//! Current commands:
//!
//! * `info <path>` — open a disc and dump everything we can read from the
//!   IFOs: disc metadata, title list, per-VTS PGC counts. Equivalent in
//!   spirit to `lsdvd`, with libdvdread-style field names.
//!
//! Logging: controlled by `RUST_LOG`. Defaults to `info` if unset. Set
//! `RUST_LOG=debug` for IFO open/close traces, `=trace` for byte-level
//! detail (once we add demuxing).

use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

mod info;

#[derive(Parser)]
#[command(name = "disc-remuxer", version, about, long_about = None)]
struct Cli {
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
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp(None)
        .init();

    let cli = Cli::parse();

    match cli.command {
        Command::Info { path } => info::run(&path)
            .with_context(|| format!("info {}", path.display())),
    }
}
