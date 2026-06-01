//! `disc-remuxer rip-title --disc <path> --title N --out-dir <dir>` —
//! thin CLI shell over [`disc_dvd::ops::rip_title`].
//!
//! Files produced (matching MakeMKV's mkvextract output conventions):
//!
//! ```text
//! out-dir/
//!   t{NN}_track1_[{lang}].mpg              video (MPEG-2 ES, user_data stripped)
//!   t{NN}_track1_[{lang}].cc.bin           raw EIA-608 user_data captured from video
//!   t{NN}_track{N}_[{lang}]_DELAY {ms}ms.ac3   AC-3 audio (or .dts / .wav for LPCM)
//!   t{NN}_track{N}_[{lang}].idx            VobSub index for subpicture stream
//!   t{NN}_track{N}_[{lang}].sub            VobSub data
//!   t{NN}_chapters.xml                     MKV chapter XML
//! ```
//!
//! The pipeline mirrors `demux-title-nav`: libdvdnav drives playback,
//! `CellLookup` resolves cell metadata, and a richer per-stream
//! handler set lives on top of the existing `Demuxer`-style sector
//! routing. Audio outputs are byte-identical to MakeMKV's mkvextract
//! output (verified on ANGEL_S1D1 title 1). Video output has
//! `user_data` blocks stripped to a sidecar so the elementary stream
//! matches MakeMKV's `.mpg` byte-for-byte.

use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_core::{detect_disc_type, DiscType};
use disc_dvd::ops::rip_title::AudioCodec;
use disc_dvd::DvdSource;

#[derive(Args, Debug)]
pub struct RipTitleArgs {
    /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
    #[arg(long = "disc")]
    pub disc: PathBuf,

    /// 1-based title number per libdvdnav.
    #[arg(long)]
    pub title: u8,

    /// Directory to write per-track output files into. Created if
    /// missing.
    #[arg(long)]
    pub out_dir: PathBuf,

    /// Safety cap on event iterations.
    #[arg(long, default_value_t = 100_000_000)]
    pub max_events: u64,
}

pub fn run(args: RipTitleArgs) -> Result<()> {
    if args.title == 0 {
        return Err(anyhow!("--title must be >= 1"));
    }
    let disc_type = detect_disc_type(&args.disc).context("detect_disc_type")?;
    if !matches!(disc_type, DiscType::Dvd) {
        return Err(anyhow!(
            "rip-title currently supports DVD only; detected {}",
            disc_type.as_str()
        ));
    }

    let source = DvdSource::open(&args.disc).context("DvdSource::open")?;
    use disc_dvd::ops::rip_title as op;
    let report = op::run(
        source.reader(),
        op::Params {
            title: args.title,
            out_dir: args.out_dir.clone(),
            max_events: args.max_events,
            tracks: op::TrackFilter::default(),
        },
    )?;

    println!();
    println!("rip-title summary (title {}):", report.title);
    println!("  out dir:            {}", report.out_dir.display());
    println!("  sectors processed:  {}", report.sectors_processed);
    println!(
        "  cell changes:       {} ({} with stc_discontinuity)",
        report.cell_changes, report.stc_disc_boundaries
    );
    println!();
    println!("video track 1 ({}):", report.video_language);
    println!("  {}", report.video_path.display());
    println!();
    println!("audio tracks:");
    for t in &report.audio {
        let codec: AudioCodec = t.codec;
        println!(
            "  track {} substream {} {:?} [{}] delay {}ms {} bytes",
            t.track_number, t.stream_n, codec, t.language, t.delay_ms, t.bytes
        );
        println!("    {}", t.path.display());
    }
    println!();
    println!("subpicture tracks:");
    for t in &report.subpictures {
        println!(
            "  track {} substream {} [{}] {} bytes",
            t.track_number, t.stream_n, t.language, t.bytes,
        );
        println!("    {}", t.sub_path.display());
        println!("    {}", t.idx_path.display());
    }
    println!();
    println!("chapters: {}", report.chapters_path.display());

    Ok(())
}
