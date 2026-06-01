//! Format-agnostic title / track model.
//!
//! This is the uniform tree every backend (`disc-dvd` now; future
//! `disc-bd` / `disc-uhd` / `disc-hddvd`) populates, and that the CLI and
//! a future UI present + select from. It mirrors MakeMKV's `AP_UiItem`
//! tree: a collection of titles, each with tracks, and **every title and
//! track carries an `enabled` flag** — the equivalent of MakeMKV's
//! `set_Enabled` checkbox that drives `SaveAllSelectedTitlesToMkv`.
//!
//! The model is plain owned data (no FFI handles, no lifetimes) so it can
//! outlive the backend that produced it and cross into the future safe
//! bindings unchanged.

use std::time::Duration;

/// The kind of an elementary track within a title.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrackKind {
    Video,
    Audio,
    Subtitle,
}

impl TrackKind {
    /// Short lowercase name (`"video"` / `"audio"` / `"subtitle"`).
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Video => "video",
            Self::Audio => "audio",
            Self::Subtitle => "subtitle",
        }
    }
}

/// One elementary stream within a title (video, audio, or subtitle).
#[derive(Debug, Clone)]
pub struct Track {
    pub kind: TrackKind,
    /// 1-based track number within the title as presented to the user
    /// (video first), matching the output filename scheme.
    pub order: u32,
    /// Backend-private stream identifier used to route bytes during a rip.
    /// For DVD this is the substream index within the IFO audio / subp
    /// attribute table (and `0` for the single video track).
    pub backend_stream_id: u32,
    /// Short codec id, e.g. `"ac3"`, `"dts"`, `"lpcm"`, `"mpeg2"`,
    /// `"vobsub"`.
    pub codec: String,
    /// 3-letter ISO-639 language code (`"und"` when unknown / not
    /// applicable, e.g. DVD video).
    pub language: String,
    /// Channel count for audio tracks; `0` for video / subtitle.
    pub channels: u8,
    /// Whether this track is selected for output.
    pub enabled: bool,
}

/// Why a title was excluded from the default selection.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SkipReason {
    /// Shorter than the configured minimum title length.
    MinLength,
    /// Identical content to another title (its 0-based collection index).
    DuplicateOf(usize),
    /// No deliverable content.
    Empty,
}

/// One playable title — a DVD PGC / `tt_srpt` entry today; a Blu-ray
/// playlist later.
#[derive(Debug, Clone)]
pub struct Title {
    /// 0-based position within the [`TitleCollection`]; this is the index
    /// the user selects by and the CLI prints.
    pub index: usize,
    /// Backend-private title identifier used to drive a rip. For DVD this
    /// is the 1-based libdvdnav / `tt_srpt` title number.
    pub backend_title_id: u32,
    /// Total playback duration.
    pub duration: Duration,
    /// Chapter count.
    pub chapter_count: usize,
    /// The title's tracks (video, then audio, then subtitle).
    pub tracks: Vec<Track>,
    /// Whether this title is selected for output.
    pub enabled: bool,
    /// If a default filter excluded it, why (informational; the title is
    /// still present in the collection).
    pub skip_reason: Option<SkipReason>,
}

impl Title {
    /// Iterate the title's currently-enabled tracks.
    pub fn enabled_tracks(&self) -> impl Iterator<Item = &Track> {
        self.tracks.iter().filter(|t| t.enabled)
    }
}

/// The uniform title tree for an opened disc.
#[derive(Debug, Clone, Default)]
pub struct TitleCollection {
    pub titles: Vec<Title>,
}

impl TitleCollection {
    /// Iterate the currently-enabled titles.
    pub fn enabled_titles(&self) -> impl Iterator<Item = &Title> {
        self.titles.iter().filter(|t| t.enabled)
    }

    /// Number of titles in the collection (including skipped ones).
    #[must_use]
    pub fn len(&self) -> usize {
        self.titles.len()
    }

    /// Whether the collection has no titles.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.titles.is_empty()
    }
}
