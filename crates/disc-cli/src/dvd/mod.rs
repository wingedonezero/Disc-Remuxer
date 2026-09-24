//! `dvd <tool>` — DVD low-level diagnostics.
//!
//! NONE of these is the rip pipeline or a user-facing command. The real
//! pipeline is [`crate::rip`] → `disc_dvd::ops::rip_title`: a libdvdnav
//! traversal plus the faithful per-stream handlers (user_data strip, VobSub,
//! delay, chapters). The tools grouped here isolate *individual stages* —
//! raw sector read, the MPEG-PS parser, the generic `Demuxer` — and the
//! *older manual cell-walk*, so each can be exercised on its own when
//! debugging or byte-verifying. They're always callable (`disc-remuxer dvd
//! <tool>`), but they live here, out of the base commands, because you only
//! reach for them when there's an issue to chase. A future BD backend would
//! get its own `bd` group the same way.

use anyhow::{Context, Result};
use clap::Subcommand;

pub mod demux_title;
pub mod demux_title_nav;
pub mod demux_vob;
pub mod dump_sectors;
pub mod dump_title;
pub mod dump_title_nav;
pub mod scan_streams;

/// The `dvd <tool>` diagnostics. See the module docs for what each isolates.
#[derive(Subcommand)]
pub enum DvdTool {
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

/// Dispatch a `dvd <tool>` invocation. Each tool is a thin shell over a
/// `disc_dvd::ops::*` function.
pub fn run(tool: DvdTool) -> Result<()> {
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
