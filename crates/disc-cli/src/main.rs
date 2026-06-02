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
//! * `demux-vob <vob_path> --out-dir <dir>` — split an MPEG-PS sector
//!   stream into per-stream elementary files (`video.0xE0.m2v`,
//!   `audio.ac3.0.ac3`, `subpicture.0.sup`, …). Strips PES headers +
//!   DVD BD-substream headers and verifies first-byte codec magic per
//!   stream. Step-5a skeleton — no cross-cell frame alignment yet.
//!
//! * `demux-title --disc <path> --title N --out-dir <dir>` — same
//!   output as demux-vob but walks the title's PGC cells directly from
//!   disc, wiring cell metadata (especially stc_discontinuity) through
//!   to the demuxer so AC-3/DTS streams resync at chapter boundaries
//!   via first_access_unit_pointer. Step-5b — preferred path for ripping.
//!
//! * `dump-title-nav --disc <path> --title N --out file.vob` — like
//!   dump-title but drives the rip via libdvdnav (executes PGC
//!   commands, follows authored playback chain). Step-6 minimal
//!   integration — proves libdvdnav as a sector source produces the
//!   same bytes as our manual cell walk for simple titles.
//!
//! Logging: controlled by `RUST_LOG` (env_logger-style syntax). Defaults
//! to `info` if unset. Set `RUST_LOG=debug` for IFO + sector-read
//! lifecycle traces, `=trace` for byte-level detail. Subcommands that
//! produce output may additionally write a duplicate log to
//! `<output>.log` next to the output file — set `--log-file` to opt in.

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
mod rip_title;
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
    },

    /// Read raw sectors from a VOB stream and write them to disk.
    DumpSectors(dump_sectors::DumpSectorsArgs),

    /// Walk a title's PGC cells and dump the concatenated VOB stream.
    DumpTitle(dump_title::DumpTitleArgs),

    /// Scan an MPEG-PS sector stream and report per-stream byte counts.
    ScanStreams(scan_streams::ScanStreamsArgs),

    /// Demultiplex an MPEG-PS sector stream into per-stream files.
    DemuxVob(demux_vob::DemuxVobArgs),

    /// Demultiplex a title's PGC cells directly from disc, honoring
    /// stc_discontinuity for AC-3/DTS audio resync.
    DemuxTitle(demux_title::DemuxTitleArgs),

    /// Dump a title via libdvdnav (executes PGC commands).
    DumpTitleNav(dump_title_nav::DumpTitleNavArgs),

    /// Demultiplex a title via libdvdnav, looking up cell metadata
    /// on each CellChange so AC-3/DTS audio resyncs correctly.
    DemuxTitleNav(demux_title_nav::DemuxTitleNavArgs),

    /// Rip selected titles + tracks. Default = all titles + all tracks;
    /// narrow with --title / --audio / --subtitle (the makemkvcon-style
    /// "rip all" front-end over the uniform selection model).
    Rip(rip::RipArgs),

    /// Rip a single title to MakeMKV-style per-track outputs (.mpg + .ac3
    /// with DELAY suffix + VobSub .idx/.sub + chapters XML).
    RipTitle(rip_title::RipTitleArgs),
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
        Command::List { path } => {
            list::run(&path).with_context(|| format!("list {}", path.display()))
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
        Command::DemuxVob(args) => {
            let path_disp = args.path.display().to_string();
            demux_vob::run(args)
                .with_context(|| format!("demux-vob {path_disp}"))
        }
        Command::DemuxTitle(args) => {
            let path_disp = args.disc.display().to_string();
            let title = args.title;
            demux_title::run(args)
                .with_context(|| format!("demux-title {path_disp} title={title}"))
        }
        Command::DumpTitleNav(args) => {
            let path_disp = args.disc.display().to_string();
            let title = args.title;
            dump_title_nav::run(args).with_context(|| {
                format!("dump-title-nav {path_disp} title={title}")
            })
        }
        Command::DemuxTitleNav(args) => {
            let path_disp = args.disc.display().to_string();
            let title = args.title;
            demux_title_nav::run(args).with_context(|| {
                format!("demux-title-nav {path_disp} title={title}")
            })
        }
        Command::Rip(args) => {
            let path_disp = args.disc.display().to_string();
            rip::run(args).with_context(|| format!("rip {path_disp}"))
        }
        Command::RipTitle(args) => {
            let path_disp = args.disc.display().to_string();
            let title = args.title;
            rip_title::run(args).with_context(|| {
                format!("rip-title {path_disp} title={title}")
            })
        }
    }
}
