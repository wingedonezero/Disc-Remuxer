//! Manual title / track selection.
//!
//! A [`Selection`] sets the `enabled` flags on a [`TitleCollection`] from
//! explicit user selectors — the format-agnostic "what to rip" layer. This
//! is the *manual* override; MakeMKV's auto-defaults (min-title-length,
//! duplicate-skip, favourite-language / forced-subtitle picking) compose on
//! top of this in a later phase.
//!
//! Track selectors are applied **per kind**, indexed within that kind: e.g.
//! `--audio 0` selects the first audio track regardless of how many video
//! or subtitle tracks precede it in [`Title::tracks`].

use crate::model::{Title, TitleCollection, Track, TrackKind};

/// Which titles to select.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub enum TitleSelector {
    /// All titles (the default).
    #[default]
    All,
    /// A specific set of 0-based collection indices.
    Indices(Vec<usize>),
}

/// Which tracks of a given kind to select.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub enum TrackSelector {
    /// All tracks of this kind (the default).
    #[default]
    All,
    /// No tracks of this kind.
    None,
    /// Tracks at these 0-based positions among this kind's tracks.
    Indices(Vec<usize>),
    /// Tracks whose language matches one of these (case-insensitive)
    /// 3-letter codes.
    Languages(Vec<String>),
}

impl TrackSelector {
    fn selects(&self, position: usize, language: &str) -> bool {
        match self {
            Self::All => true,
            Self::None => false,
            Self::Indices(idxs) => idxs.contains(&position),
            Self::Languages(langs) => {
                langs.iter().any(|l| l.eq_ignore_ascii_case(language))
            }
        }
    }
}

/// A complete selection request: which titles, and per-kind which tracks.
#[derive(Debug, Clone, Default)]
pub struct Selection {
    pub titles: TitleSelector,
    pub video: TrackSelector,
    pub audio: TrackSelector,
    pub subtitle: TrackSelector,
}

impl Selection {
    /// Apply the selectors to a collection, setting every title's and
    /// track's `enabled` flag. Titles excluded by the title selector are
    /// disabled; within enabled titles, each kind's tracks are enabled per
    /// that kind's selector.
    pub fn apply(&self, collection: &mut TitleCollection) {
        for title in &mut collection.titles {
            let title_enabled = match &self.titles {
                TitleSelector::All => true,
                TitleSelector::Indices(idxs) => idxs.contains(&title.index),
            };
            title.enabled = title_enabled;
            apply_kind(&mut title.tracks, TrackKind::Video, &self.video);
            apply_kind(&mut title.tracks, TrackKind::Audio, &self.audio);
            apply_kind(&mut title.tracks, TrackKind::Subtitle, &self.subtitle);
        }
    }

    /// The per-kind track selector for a given kind.
    #[must_use]
    pub fn for_kind(&self, kind: TrackKind) -> &TrackSelector {
        match kind {
            TrackKind::Video => &self.video,
            TrackKind::Audio => &self.audio,
            TrackKind::Subtitle => &self.subtitle,
        }
    }
}

/// Set the `enabled` flag on every track of `kind` per `sel`, counting
/// positions within the kind (0-based).
fn apply_kind(tracks: &mut [Track], kind: TrackKind, sel: &TrackSelector) {
    let mut position = 0usize;
    for track in tracks.iter_mut() {
        if track.kind != kind {
            continue;
        }
        track.enabled = sel.selects(position, &track.language);
        position += 1;
    }
}

/// Convenience: leave a `Title` fully enabled (title + all tracks). Used by
/// backends to set the default state before any selector is applied.
pub fn enable_all(title: &mut Title) {
    title.enabled = true;
    for t in &mut title.tracks {
        t.enabled = true;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Title, Track, TrackKind};
    use std::time::Duration;

    fn track(kind: TrackKind, order: u32, lang: &str) -> Track {
        Track {
            kind,
            order,
            backend_stream_id: order,
            codec: "x".into(),
            language: lang.into(),
            channels: 0,
            enabled: false,
        }
    }

    fn sample() -> TitleCollection {
        let mk = |index: usize| Title {
            index,
            backend_title_id: index as u32 + 1,
            duration: Duration::from_secs(60),
            chapter_count: 1,
            tracks: vec![
                track(TrackKind::Video, 1, "und"),
                track(TrackKind::Audio, 2, "eng"),
                track(TrackKind::Audio, 3, "fre"),
                track(TrackKind::Subtitle, 4, "eng"),
            ],
            enabled: false,
            skip_reason: None,
        };
        TitleCollection {
            titles: vec![mk(0), mk(1), mk(2)],
        }
    }

    #[test]
    fn default_selects_everything() {
        let mut c = sample();
        Selection::default().apply(&mut c);
        assert_eq!(c.enabled_titles().count(), 3);
        for t in &c.titles {
            assert_eq!(t.enabled_tracks().count(), 4);
        }
    }

    #[test]
    fn title_indices_filter() {
        let mut c = sample();
        let sel = Selection {
            titles: TitleSelector::Indices(vec![1]),
            ..Default::default()
        };
        sel.apply(&mut c);
        let enabled: Vec<usize> = c.enabled_titles().map(|t| t.index).collect();
        assert_eq!(enabled, vec![1]);
    }

    #[test]
    fn audio_index_selects_within_kind() {
        let mut c = sample();
        let sel = Selection {
            audio: TrackSelector::Indices(vec![0]),
            ..Default::default()
        };
        sel.apply(&mut c);
        let t = &c.titles[0];
        // first audio (order 2) enabled, second audio (order 3) not
        let enabled_audio: Vec<u32> = t
            .tracks
            .iter()
            .filter(|tr| tr.kind == TrackKind::Audio && tr.enabled)
            .map(|tr| tr.order)
            .collect();
        assert_eq!(enabled_audio, vec![2]);
    }

    #[test]
    fn subtitle_none_and_audio_language() {
        let mut c = sample();
        let sel = Selection {
            audio: TrackSelector::Languages(vec!["fre".into()]),
            subtitle: TrackSelector::None,
            ..Default::default()
        };
        sel.apply(&mut c);
        let t = &c.titles[0];
        let aud: Vec<u32> = t
            .tracks
            .iter()
            .filter(|tr| tr.kind == TrackKind::Audio && tr.enabled)
            .map(|tr| tr.order)
            .collect();
        assert_eq!(aud, vec![3]); // only French audio
        assert_eq!(
            t.tracks
                .iter()
                .filter(|tr| tr.kind == TrackKind::Subtitle && tr.enabled)
                .count(),
            0
        );
    }
}
