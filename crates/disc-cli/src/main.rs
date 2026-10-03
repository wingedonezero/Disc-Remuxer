//! `disc-remuxer`: the command-line front end.
//!
//! Skeleton stage: `version`, and `probe`, which runs FFmpeg's DVD-Video
//! demuxer unchanged on one title so its behaviour can be checked on real
//! discs before anything is changed.

mod ffmpeg;
mod logger;

use anyhow::Result;
use clap::{Parser, Subcommand};
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "disc-remuxer", version, about = "Rips DVD, Blu-ray and HD DVD sources")]
struct Cli {
    /// More log detail: -v debug, -vv trace.
    #[arg(short, long, global = true, action = clap::ArgAction::Count)]
    verbose: u8,
    /// Errors only.
    #[arg(short, long, global = true, conflicts_with = "verbose")]
    quiet: bool,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Program version and the versions of the bundled libraries.
    Version,
    /// Open one DVD title with FFmpeg's DVD-Video demuxer as it is and print
    /// the streams it finds.
    Probe {
        /// DVD folder (holding `VIDEO_TS`) or image file.
        source: PathBuf,
        /// Title number (1-based; 0 = the demuxer's own choice).
        #[arg(long, default_value_t = 1)]
        title: i32,
    },
}

fn main() {
    let cli = Cli::parse();
    let level = if cli.quiet {
        log::LevelFilter::Error
    } else {
        match cli.verbose {
            0 => log::LevelFilter::Info,
            1 => log::LevelFilter::Debug,
            _ => log::LevelFilter::Trace,
        }
    };
    logger::init(level);
    ffmpeg::route_log(level);

    if let Err(e) = run(cli.command) {
        log::error!("{e:#}");
        std::process::exit(1);
    }
}

fn run(command: Command) -> Result<()> {
    match command {
        Command::Version => {
            print_versions();
            Ok(())
        }
        Command::Probe { source, title } => ffmpeg::probe_dvdvideo(&source, title),
    }
}

fn print_versions() {
    println!("disc-remuxer {}", env!("CARGO_PKG_VERSION"));
    println!("  FFmpeg        {}", ffmpeg::version());
    for (name, v) in ffmpeg::library_versions() {
        println!("    {name:<12}{}.{}.{}", v.0, v.1, v.2);
    }
    println!("  libdvdread    {}", libdvdread_sys::VERSION);
    println!("  libdvdnav     {}", libdvdnav_sys::VERSION);
    println!("  libdvdcss     {}", libdvdcss_sys::VERSION);
}
