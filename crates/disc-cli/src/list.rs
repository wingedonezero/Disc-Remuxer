//! `disc-remuxer list <path>` — print the uniform title/track tree that the
//! `rip` selectors operate on.
//!
//! This is the index map: each title's 0-based collection index (what
//! `--title` takes), its duration / chapter count, and per-kind tracks with
//! the within-kind index (what `--audio` / `--subtitle` take), codec, and
//! language (`—` = untagged). The future UI mirrors this exact tree, so
//! "audio 0 / audio 1" here is precisely what a checkmark selects — stable
//! regardless of whether a track is language-tagged.
//!
//! Goes through the generic [`disc_core::Session`] / [`DiscBackend`] facade,
//! so it works the same way for every backend once they exist.

use std::path::Path;

use anyhow::{bail, Context, Result};
use disc_core::model::TrackKind;
use disc_core::{detect_disc_type, DiscBackend, DiscType, Session, Title};
use disc_dvd::DvdBackend;

pub fn run(path: &Path) -> Result<()> {
    let disc_type = detect_disc_type(path).context("detect_disc_type")?;
    let backend: Box<dyn DiscBackend> = match disc_type {
        DiscType::Dvd => {
            Box::new(DvdBackend::open(path).context("DvdBackend::open")?)
        }
        other => bail!("`list` does not support {} yet", other.as_str()),
    };

    let session = Session::new(backend).context("enumerating titles")?;
    let collection = session.collection();

    println!(
        "{} — {} title(s)",
        session.disc_type().as_str(),
        collection.len()
    );
    for t in &collection.titles {
        let skip = t
            .skip_reason
            .as_ref()
            .map(|r| format!("  [skipped: {r:?}]"))
            .unwrap_or_default();
        println!();
        println!(
            "title #{:02}  (id {})  {}  {} chapter(s){skip}",
            t.index,
            t.backend_title_id,
            fmt_duration(t.duration),
            t.chapter_count,
        );
        print_kind(t, TrackKind::Video);
        print_kind(t, TrackKind::Audio);
        print_kind(t, TrackKind::Subtitle);
    }
    Ok(())
}

/// Print each track of `kind` with its within-kind selection index.
fn print_kind(title: &Title, kind: TrackKind) {
    for (pos, tr) in title
        .tracks
        .iter()
        .filter(|x| x.kind == kind)
        .enumerate()
    {
        let lang = tr.language.as_deref().unwrap_or("—");
        let ch = if tr.channels > 0 {
            format!("  {}ch", tr.channels)
        } else {
            String::new()
        };
        println!("    {} {pos}  {:<6}  {lang}{ch}", kind.as_str(), tr.codec);
    }
}

fn fmt_duration(d: std::time::Duration) -> String {
    let s = d.as_secs();
    format!("{:02}:{:02}:{:02}", s / 3600, (s % 3600) / 60, s % 60)
}
