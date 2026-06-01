//! `DvdBackend` — the DVD implementation of [`disc_core::DiscBackend`].
//!
//! Holds an open [`DvdSource`] and enumerates the disc's `tt_srpt` titles
//! into the format-agnostic [`TitleCollection`]: per title, the duration
//! (PGC `playback_time`), chapter count, and the track list (one video
//! track, then the VTS audio streams, then the subpicture streams). All
//! tracks start `enabled`; the selection engine narrows from there.
//!
//! A title that can't be resolved (malformed VTS / PGC) is logged and
//! skipped rather than aborting the whole enumeration — same policy as
//! [`crate::nav_cells::CellLookup`].

use std::path::Path;
use std::time::Duration;

use anyhow::{anyhow, Context};
use disc_core::model::{Title, TitleCollection, Track, TrackKind};
use disc_core::{DiscBackend, DiscError, DiscType};

use crate::cell::dvd_time_seconds;
use crate::ifo::{audio_attr_t, subp_attr_t, title_info_t, IfoHandle, IfoKind};
use crate::{DvdError, DvdReader, DvdSource};

/// DVD-Video backend over an open [`DvdSource`].
pub struct DvdBackend {
    source: DvdSource,
}

impl DvdBackend {
    /// Open a DVD at the given path (directory / ISO / device).
    pub fn open(path: &Path) -> Result<Self, DvdError> {
        Ok(Self {
            source: DvdSource::open(path)?,
        })
    }

    /// Borrow the underlying [`DvdReader`] — needed by the rip ops, which
    /// drive libdvdnav and read sectors from the same disc.
    #[must_use]
    pub fn reader(&self) -> &DvdReader {
        self.source.reader()
    }
}

impl DiscBackend for DvdBackend {
    fn disc_type(&self) -> DiscType {
        DiscType::Dvd
    }

    fn path(&self) -> &Path {
        self.source.reader().path()
    }

    fn enumerate(&self) -> Result<TitleCollection, DiscError> {
        enumerate_titles(self.reader())
    }
}

/// Walk VMG `tt_srpt` and build the uniform title tree.
pub fn enumerate_titles(reader: &DvdReader) -> Result<TitleCollection, DiscError> {
    // VMG open failure is fatal (whole disc unreadable). `?` converts the
    // DvdError into DiscError::Backend at the boundary.
    let vmg = IfoHandle::open(reader, IfoKind::Vmg)?;
    let titles = vmg.titles();

    let mut out = Vec::with_capacity(titles.len());
    for (idx, t) in titles.iter().enumerate() {
        match build_title(reader, idx, t) {
            Ok(title) => out.push(title),
            Err(e) => log::warn!(
                "enumerate: skipping title {} (tt_srpt[{idx}]): {e:#}",
                idx + 1
            ),
        }
    }
    log::info!(
        "enumerate: {} of {} tt_srpt titles resolved",
        out.len(),
        titles.len()
    );
    Ok(TitleCollection { titles: out })
}

fn build_title(
    reader: &DvdReader,
    index: usize,
    t: &title_info_t,
) -> anyhow::Result<Title> {
    let backend_title_id = u32::try_from(index + 1).unwrap_or(u32::MAX);
    let title_set_nr: u8 = { t.title_set_nr };
    let vts_ttn: u8 = { t.vts_ttn };

    let vts = IfoHandle::open(reader, IfoKind::Vts(u32::from(title_set_nr)))
        .with_context(|| format!("opening VTS_{title_set_nr:02}_0.IFO"))?;

    let chapters = vts.chapters_for(vts_ttn);
    let chapter_count = chapters.len();
    let first_chapter = chapters
        .first()
        .ok_or_else(|| anyhow!("no PTT chapter entries (vts_ttn={vts_ttn})"))?;
    let pgcn: u16 = { first_chapter.pgcn };

    let pgcs = vts.pgcs();
    let srp = pgcs
        .get(usize::from(pgcn).saturating_sub(1))
        .ok_or_else(|| anyhow!("pgcn {pgcn} out of range ({} PGCs)", pgcs.len()))?;
    let pgc_ptr = { srp.pgc };
    // SAFETY: libdvdread populates the PGC pointer when the IFO parses; a
    // NULL means a malformed slot — surfaced as an error and skipped.
    let pgc = unsafe { pgc_ptr.as_ref() }.ok_or_else(|| anyhow!("PGC pointer NULL"))?;
    let playback_time = { pgc.playback_time };
    let duration = Duration::from_secs(u64::from(dvd_time_seconds(&playback_time)));

    let vtsi_mat = vts.vtsi_mat().ok_or_else(|| anyhow!("vtsi_mat NULL"))?;
    let nr_audio: u8 = { vtsi_mat.nr_of_vts_audio_streams };
    let nr_subp: u8 = { vtsi_mat.nr_of_vts_subp_streams };
    let audio_arr: [audio_attr_t; 8] = { vtsi_mat.vts_audio_attr };
    let subp_arr: [subp_attr_t; 32] = { vtsi_mat.vts_subp_attr };

    let mut tracks = Vec::new();
    let mut order = 1u32;

    // One video track. DVD-Video video carries no language code.
    tracks.push(Track {
        kind: TrackKind::Video,
        order,
        backend_stream_id: 0,
        codec: "mpeg2".into(),
        language: "und".into(),
        channels: 0,
        enabled: true,
    });
    order += 1;

    for (i, attr) in audio_arr.iter().take(usize::from(nr_audio)).enumerate() {
        let lang_code: u16 = { attr.lang_code };
        let channels = audio_channels(attr);
        tracks.push(Track {
            kind: TrackKind::Audio,
            order,
            backend_stream_id: u32::try_from(i).unwrap_or(u32::MAX),
            codec: audio_codec_str(attr.audio_format()).into(),
            language: iso639_3(lang_code),
            channels,
            enabled: true,
        });
        order += 1;
    }

    for (i, attr) in subp_arr.iter().take(usize::from(nr_subp)).enumerate() {
        let lang_code: u16 = { attr.lang_code };
        tracks.push(Track {
            kind: TrackKind::Subtitle,
            order,
            backend_stream_id: u32::try_from(i).unwrap_or(u32::MAX),
            codec: "vobsub".into(),
            language: iso639_3(lang_code),
            channels: 0,
            enabled: true,
        });
        order += 1;
    }

    Ok(Title {
        index,
        backend_title_id,
        duration,
        chapter_count,
        tracks,
        enabled: true,
        skip_reason: None,
    })
}

/// Audio channel count from the packed `audio_attr_t` bitfield (the wire
/// format stores `channels - 1` at bit offset 13, width 3).
fn audio_channels(attr: &audio_attr_t) -> u8 {
    (attr._bitfield_1.get(13, 3) as u8) + 1
}

/// Map the DVD `audio_format` code to a short codec id. Mirrors the
/// mapping in `ops::rip_title`.
fn audio_codec_str(fmt: u8) -> &'static str {
    match fmt {
        0 => "ac3",
        2 | 3 => "mp2",
        4 => "lpcm",
        6 => "dts",
        _ => "ac3",
    }
}

/// Decode a 16-bit packed ISO-639 language code into a 3-letter lowercase
/// string (`"und"` when absent / non-alphabetic). Common 2-letter codes are
/// mapped to their 3-letter (639-2/T) equivalents. Mirrors
/// `ops::rip_title::decode_language_code` (shared home is a future cleanup).
fn iso639_3(raw: u16) -> String {
    if raw == 0 {
        return "und".into();
    }
    let lo = (raw & 0xFF) as u8;
    let hi = (raw >> 8) as u8;
    if !hi.is_ascii_alphabetic() || !lo.is_ascii_alphabetic() {
        return "und".into();
    }
    let two = [hi.to_ascii_lowercase(), lo.to_ascii_lowercase()];
    let two_str = std::str::from_utf8(&two).unwrap_or("un");
    match two_str {
        "en" => "eng".into(),
        "fr" => "fre".into(),
        "es" => "spa".into(),
        "de" => "ger".into(),
        "it" => "ita".into(),
        "ja" => "jpn".into(),
        "zh" => "chi".into(),
        "ko" => "kor".into(),
        "pt" => "por".into(),
        "ru" => "rus".into(),
        "nl" => "dut".into(),
        "sv" => "swe".into(),
        "fi" => "fin".into(),
        "no" => "nor".into(),
        "da" => "dan".into(),
        "pl" => "pol".into(),
        "cs" => "cze".into(),
        "ar" => "ara".into(),
        "he" => "heb".into(),
        "hi" => "hin".into(),
        "tr" => "tur".into(),
        "el" => "gre".into(),
        "hu" => "hun".into(),
        _ => two_str.into(),
    }
}
