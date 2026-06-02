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

use std::time::Duration;

use crate::model::{SkipReason, Title, TitleCollection, Track, TrackKind};

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
    fn selects(&self, position: usize, language: Option<&str>) -> bool {
        match self {
            Self::All => true,
            Self::None => false,
            Self::Indices(idxs) => idxs.contains(&position),
            Self::Languages(langs) => language
                .is_some_and(|l| langs.iter().any(|x| x.eq_ignore_ascii_case(l))),
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
                // `All` = every title that survived the default filters
                // (min-length etc.) — i.e. not flagged with a skip_reason.
                TitleSelector::All => title.skip_reason.is_none(),
                // An explicit index list overrides the default filters: the
                // user asked for these titles by number, skipped or not.
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
///
/// Language include-only is *lenient*: if a `Languages` selector matches no
/// track of a kind the title actually has (e.g. an untagged disc where
/// `--audio eng` matches nothing), fall back to enabling all of that kind
/// rather than silently dropping it. Index selection is the precise path.
fn apply_kind(tracks: &mut [Track], kind: TrackKind, sel: &TrackSelector) {
    if let TrackSelector::Languages(langs) = sel {
        let present = tracks.iter().any(|t| t.kind == kind);
        let any_match = tracks.iter().any(|t| {
            t.kind == kind
                && t.language
                    .as_deref()
                    .is_some_and(|l| langs.iter().any(|x| x.eq_ignore_ascii_case(l)))
        });
        if present && !any_match {
            log::warn!(
                target: "disc_check",
                "selection: {} language filter {langs:?} matched no track — including all {}(s)",
                kind.as_str(),
                kind.as_str(),
            );
            for track in tracks.iter_mut().filter(|t| t.kind == kind) {
                track.enabled = true;
            }
            return;
        }
    }

    let mut position = 0usize;
    for track in tracks.iter_mut() {
        if track.kind != kind {
            continue;
        }
        track.enabled = sel.selects(position, track.language.as_deref());
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

/// Default filter: flag every title shorter than `min` with
/// [`SkipReason::MinLength`].
///
/// This *unselects by default* — under [`TitleSelector::All`] a flagged
/// title is not enabled — but it never **removes** anything: the title
/// stays in the collection (visible in `list`) and an explicit
/// [`TitleSelector::Indices`] still selects it. Only titles not already
/// flagged (for another reason) are marked.
pub fn mark_min_length(collection: &mut TitleCollection, min: Duration) {
    for title in &mut collection.titles {
        if title.skip_reason.is_none() && title.duration < min {
            title.skip_reason = Some(SkipReason::MinLength);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{SkipReason, Title, Track, TrackKind};
    use std::time::Duration;

    fn track(kind: TrackKind, order: u32, lang: Option<&str>) -> Track {
        Track {
            kind,
            order,
            backend_stream_id: order,
            codec: "x".into(),
            language: lang.map(str::to_string),
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
                track(TrackKind::Video, 1, None),
                track(TrackKind::Audio, 2, Some("eng")),
                track(TrackKind::Audio, 3, Some("fre")),
                track(TrackKind::Subtitle, 4, Some("eng")),
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

    #[test]
    fn audio_language_no_match_falls_back_to_all() {
        let mut c = sample();
        let sel = Selection {
            // no Spanish audio on the sample → lenient fallback to all audio
            audio: TrackSelector::Languages(vec!["spa".into()]),
            ..Default::default()
        };
        sel.apply(&mut c);
        let enabled_audio = c.titles[0]
            .tracks
            .iter()
            .filter(|t| t.kind == TrackKind::Audio && t.enabled)
            .count();
        assert_eq!(enabled_audio, 2);
    }

    #[test]
    fn untagged_audio_not_matched_by_named_language() {
        // A title whose only audio is untagged (None): a named-language
        // filter matches nothing, so the fallback keeps it.
        let mut c = TitleCollection {
            titles: vec![Title {
                index: 0,
                backend_title_id: 1,
                duration: Duration::from_secs(60),
                chapter_count: 1,
                tracks: vec![
                    track(TrackKind::Video, 1, None),
                    track(TrackKind::Audio, 2, None),
                ],
                enabled: false,
                skip_reason: None,
            }],
        };
        let sel = Selection {
            audio: TrackSelector::Languages(vec!["eng".into()]),
            ..Default::default()
        };
        sel.apply(&mut c);
        assert_eq!(
            c.titles[0]
                .tracks
                .iter()
                .filter(|t| t.kind == TrackKind::Audio && t.enabled)
                .count(),
            1
        );
    }

    #[test]
    fn min_length_unselects_but_keeps_titles() {
        let mut c = sample(); // 3 titles, 60s each
        mark_min_length(&mut c, Duration::from_secs(120));
        // flagged MinLength, but still present (never removed)
        assert_eq!(c.len(), 3);
        assert!(c
            .titles
            .iter()
            .all(|t| t.skip_reason == Some(SkipReason::MinLength)));
        // default (All) selects none of the short titles
        Selection::default().apply(&mut c);
        assert_eq!(c.enabled_titles().count(), 0);
        // but an explicit index overrides the min-length default
        let sel = Selection {
            titles: TitleSelector::Indices(vec![1]),
            ..Default::default()
        };
        sel.apply(&mut c);
        assert_eq!(
            c.enabled_titles().map(|t| t.index).collect::<Vec<_>>(),
            vec![1]
        );
    }
}
