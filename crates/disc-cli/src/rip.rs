//! `disc-remuxer rip --disc <path> --out-dir <dir> [--title …] [--audio …]
//! [--subtitle …]` — rip selected titles and tracks.
//!
//! The format-agnostic selection front-end: enumerate the disc into the
//! uniform title tree ([`disc_core::TitleCollection`]), apply the user's
//! selectors ([`disc_core::Selection`]), then drive the per-title rip op
//! for each enabled title. With no selectors it rips **every title and
//! every track** — matching makemkvcon's "rip all" default (the
//! min-length / duplicate filtering is a later phase).
//!
//! Track selectors are per kind and indexed within the kind:
//! `--audio 0` = the first audio track; `--audio eng` = English audio;
//! `--audio none` = drop audio; `--subtitle` uses the same grammar.

use std::path::PathBuf;

use anyhow::{anyhow, bail, Context, Result};
use clap::Args;
use disc_core::model::TrackKind;
use disc_core::selection::{Selection, TitleSelector, TrackSelector};
use disc_core::{detect_disc_type, DiscBackend, DiscType};
use disc_dvd::ops::rip_title as op;
use disc_dvd::DvdBackend;

#[derive(Args, Debug)]
pub struct RipArgs {
    /// Path to a disc, ISO image, VIDEO_TS directory, or device node.
    #[arg(long = "disc")]
    pub disc: PathBuf,

    /// Directory to write per-track output files into (created if missing).
    /// Files are prefixed by title number, so all titles share one dir.
    #[arg(long)]
    pub out_dir: PathBuf,

    /// Titles to rip: `all` (default) or a comma-separated list of 0-based
    /// collection indices, e.g. `0,2,5`.
    #[arg(long, default_value = "all")]
    pub title: String,

    /// Audio tracks: `all` (default), `none`, a comma-separated list of
    /// 0-based audio indices (`0,1`), or language codes (`eng,fre`).
    #[arg(long, default_value = "all")]
    pub audio: String,

    /// Subtitle tracks: same grammar as `--audio`.
    #[arg(long, default_value = "all")]
    pub subtitle: String,

    /// Minimum title length, in seconds. Shorter titles are *unselected by
    /// default* (still listed + selectable via --title); `0` = no minimum.
    /// Matches makemkvcon's default of 120.
    #[arg(long, default_value_t = 120)]
    pub min_length: u64,

    /// Safety cap on per-title event iterations.
    #[arg(long, default_value_t = 100_000_000)]
    pub max_events: u64,
}

pub fn run(args: RipArgs) -> Result<()> {
    let disc_type = detect_disc_type(&args.disc).context("detect_disc_type")?;
    if !matches!(disc_type, DiscType::Dvd) {
        bail!(
            "rip currently supports DVD only; detected {}",
            disc_type.as_str()
        );
    }

    let backend = DvdBackend::open(&args.disc).context("DvdBackend::open")?;
    let mut collection = backend.enumerate().context("enumerate titles")?;

    // Default filter: unselect (don't remove) titles below the threshold.
    if args.min_length > 0 {
        disc_core::mark_min_length(
            &mut collection,
            std::time::Duration::from_secs(args.min_length),
        );
    }

    let selection = Selection {
        titles: parse_title_selector(&args.title)?,
        audio: parse_track_selector(&args.audio)?,
        subtitle: parse_track_selector(&args.subtitle)?,
        video: TrackSelector::All,
    };
    selection.apply(&mut collection);

    let enabled: Vec<&disc_core::Title> =
        collection.titles.iter().filter(|t| t.enabled).collect();
    if enabled.is_empty() {
        bail!(
            "no titles selected (disc has {} title(s))",
            collection.len()
        );
    }

    log::info!(
        "rip: {} of {} title(s) selected (min-length {}s) -> {}",
        enabled.len(),
        collection.len(),
        args.min_length,
        args.out_dir.display(),
    );
    for t in &enabled {
        let a = count_enabled(t, TrackKind::Audio);
        let s = count_enabled(t, TrackKind::Subtitle);
        log::info!(
            "  title #{:02} (id {}) dur={:?}: {a} audio, {s} subtitle track(s)",
            t.index,
            t.backend_title_id,
            t.duration,
        );
    }

    std::fs::create_dir_all(&args.out_dir)
        .with_context(|| format!("creating {}", args.out_dir.display()))?;

    for t in &enabled {
        let title_id = u8::try_from(t.backend_title_id)
            .map_err(|_| anyhow!("title id {} out of range for rip", t.backend_title_id))?;
        let audio: Vec<u32> = enabled_stream_ids(t, TrackKind::Audio);
        let subp: Vec<u32> = enabled_stream_ids(t, TrackKind::Subtitle);

        log::info!("ripping title #{:02} (id {title_id})", t.index);
        let report = op::run(
            backend.reader(),
            op::Params {
                title: title_id,
                out_dir: args.out_dir.clone(),
                max_events: args.max_events,
                tracks: op::TrackFilter {
                    audio: Some(audio),
                    subp: Some(subp),
                },
            },
        )
        .with_context(|| format!("ripping title id {title_id}"))?;

        println!(
            "title #{:02} (id {title_id}): {} audio, {} subtitle track(s) -> {}",
            t.index,
            report.audio.len(),
            report.subpictures.len(),
            report.out_dir.display(),
        );
    }

    println!(
        "rip complete: {} title(s) -> {}",
        enabled.len(),
        args.out_dir.display()
    );
    Ok(())
}

fn count_enabled(title: &disc_core::Title, kind: TrackKind) -> usize {
    title
        .tracks
        .iter()
        .filter(|x| x.kind == kind && x.enabled)
        .count()
}

fn enabled_stream_ids(title: &disc_core::Title, kind: TrackKind) -> Vec<u32> {
    title
        .tracks
        .iter()
        .filter(|x| x.kind == kind && x.enabled)
        .map(|x| x.backend_stream_id)
        .collect()
}

fn parse_title_selector(s: &str) -> Result<TitleSelector> {
    if s.eq_ignore_ascii_case("all") {
        return Ok(TitleSelector::All);
    }
    let idxs = parse_indices(s).with_context(|| format!("invalid --title {s:?}"))?;
    Ok(TitleSelector::Indices(idxs))
}

fn parse_track_selector(s: &str) -> Result<TrackSelector> {
    if s.eq_ignore_ascii_case("all") {
        return Ok(TrackSelector::All);
    }
    if s.eq_ignore_ascii_case("none") {
        return Ok(TrackSelector::None);
    }
    // A list of integers → positional indices; otherwise language codes.
    let tokens: Vec<&str> = s.split(',').map(str::trim).filter(|t| !t.is_empty()).collect();
    if tokens.is_empty() {
        return Ok(TrackSelector::None);
    }
    if tokens.iter().all(|t| t.parse::<usize>().is_ok()) {
        Ok(TrackSelector::Indices(parse_indices(s)?))
    } else {
        Ok(TrackSelector::Languages(
            tokens.iter().map(|t| t.to_ascii_lowercase()).collect(),
        ))
    }
}

fn parse_indices(s: &str) -> Result<Vec<usize>> {
    s.split(',')
        .map(str::trim)
        .filter(|t| !t.is_empty())
        .map(|t| t.parse::<usize>().map_err(|_| anyhow!("not a number: {t:?}")))
        .collect()
}
