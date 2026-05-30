//! `disc-remuxer rip-title --disc <path> --title N --out-dir <dir>` —
//! produce MakeMKV-style per-track outputs for one DVD title.
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

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufWriter, Cursor, Write};
use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use clap::Args;
use disc_core::{check, check_eq, detect_disc_type, DiscType};
use disc_dvd::chapters::write_chapters_xml;
use disc_dvd::ifo::{audio_attr_t, subp_attr_t, IfoHandle, IfoKind};
use disc_dvd::mpegps::{scan_sector, stream_kind, PesPacket, StreamKind, SECTOR_SIZE};
use disc_dvd::nav::{DvdNav, NavEvent};
use disc_dvd::nav_cells::CellLookup;
use disc_dvd::video_es::UserDataFilter;
use disc_dvd::vobsub::{write_idx_file, SubWriter, VobSubEntry};
use disc_dvd::DvdSource;

const STILL_LENGTH_INFINITE: u8 = 0xFF;

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

/// Decode a 16-bit ISO-639 language code (two ASCII bytes packed
/// big-endian) into a 3-letter lowercase string. We map the common
/// 2-letter codes to their 3-letter equivalents; unknown codes return
/// "und" (undetermined).
fn decode_language_code(raw: u16) -> String {
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
    // Common 2-letter → 3-letter mapping (BCP47 / ISO 639-1 → 639-2/T).
    // Not exhaustive; falls back to a duplicated two-letter when
    // unknown.
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

#[derive(Debug, Clone, Copy)]
enum AudioCodec {
    Ac3,
    Dts,
    Lpcm,
    MpegAudio,
}

impl AudioCodec {
    fn file_ext(self) -> &'static str {
        match self {
            Self::Ac3 => "ac3",
            Self::Dts => "dts",
            Self::Lpcm => "wav",
            Self::MpegAudio => "mp2",
        }
    }
}

struct AudioHandler {
    codec: AudioCodec,
    track_number: u8,
    language: String,
    /// PRE-rename path. Final filename gets the delay value appended
    /// when we close out the rip.
    temp_path: PathBuf,
    writer: BufWriter<File>,
    first_pts: Option<u64>,
    bytes_written: u64,
    payload_strip: usize,
}

struct SubpictureHandler {
    track_number: u8,
    language: String,
    sub_path: PathBuf,
    idx_path: PathBuf,
    writer: SubWriter<File>,
}

struct VideoHandler {
    track_number: u8,
    language: String,
    video_path: PathBuf,
    cc_path: PathBuf,
    video_writer: BufWriter<File>,
    cc_writer: BufWriter<File>,
    filter: UserDataFilter,
    first_pts: Option<u64>,
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
    std::fs::create_dir_all(&args.out_dir)
        .with_context(|| format!("creating {}", args.out_dir.display()))?;

    let title_prefix = format!("t{:02}", args.title);
    log::info!(
        "rip-title disc={} title={} (prefix={title_prefix}) out_dir={}",
        args.disc.display(),
        args.title,
        args.out_dir.display(),
    );

    // 1) Resolve title metadata: VTS number, language codes, palette.
    let metadata = {
        let source = DvdSource::open(&args.disc).context("DvdSource::open (metadata)")?;
        TitleMetadata::resolve(source.reader(), args.title)
            .context("resolving title metadata")?
    };
    log::info!(
        "title {}: VTS={} #audio={} #subp={} #chapters={}",
        args.title,
        metadata.vts_nr,
        metadata.audio_attrs.len(),
        metadata.subp_attrs.len(),
        metadata.chapter_count,
    );

    // 2) Build CellLookup (used by demux for stc_discontinuity log).
    let lookup = {
        let source = DvdSource::open(&args.disc).context("DvdSource::open (lookup)")?;
        CellLookup::build(source.reader()).context("CellLookup::build")?
    };

    // 3) Open the nav VM.
    let mut nav = DvdNav::open(&args.disc).context("DvdNav::open")?;
    nav.set_readahead(false).context("disable readahead")?;
    nav.set_pgc_positioning(true)
        .context("enable PGC positioning")?;
    let n_titles = nav.num_titles().context("num_titles")?;
    if i32::from(args.title) > n_titles {
        return Err(anyhow!(
            "--title {} out of range (disc has {n_titles} titles per dvdnav)",
            args.title
        ));
    }
    nav.title_play(i32::from(args.title)).context("dvdnav_title_play")?;

    // 4) Set up per-stream handlers.
    let video = open_video_handler(&args.out_dir, &title_prefix, 1)?;
    let mut audio_handlers: BTreeMap<u8, AudioHandler> = BTreeMap::new();
    let mut subp_handlers: BTreeMap<u8, SubpictureHandler> = BTreeMap::new();
    let mut next_track_number: u8 = 2;

    // Build audio handlers in order matching the IFO's audio_attr
    // table (stream index 0..nr_of_vts_audio_streams). We assign
    // each AC-3 substream the IFO's index 0, 1, 2, ... — matching
    // libdvdread's `audio_format` ordering.
    for (idx, attr) in metadata.audio_attrs.iter().enumerate() {
        let codec = audio_codec_from_attr(attr);
        let lang_code: u16 = { attr.lang_code };
        let language = decode_language_code(lang_code);
        let track_number = next_track_number;
        next_track_number += 1;
        let stream_n = u8::try_from(idx).unwrap_or(7);
        let h = open_audio_handler(
            &args.out_dir,
            &title_prefix,
            track_number,
            &language,
            codec,
        )?;
        audio_handlers.insert(stream_n, h);
    }
    for (idx, attr) in metadata.subp_attrs.iter().enumerate() {
        let lang_code: u16 = { attr.lang_code };
        let language = decode_language_code(lang_code);
        let track_number = next_track_number;
        next_track_number += 1;
        let stream_n = u8::try_from(idx).unwrap_or(31);
        let h = open_subpicture_handler(
            &args.out_dir,
            &title_prefix,
            track_number,
            &language,
        )?;
        subp_handlers.insert(stream_n, h);
    }

    // 5) Run the libdvdnav walk.
    let mut video = video;
    let mut sectors_processed: u64 = 0;
    let mut cell_changes: u64 = 0;
    let mut stc_disc_boundaries: u64 = 0;
    let mut left_title = false;
    let mut last_cell: Option<(i32, i32)> = None;
    // Title-relative subtitle timeline. The DVD presentation clock
    // resets at each cell with stc_discontinuity, so we accumulate the
    // inter-VOBU gap from the NAV-pack VOBU presentation times
    // (vobu_s_ptm/vobu_e_ptm) and add it to subtitle PTS to keep the
    // .idx/.sub timeline continuous across the title.
    let mut sub_pts_offset: i64 = 0;
    let mut prev_vobu_e_ptm: u32 = 0;

    for event_idx in 0..args.max_events {
        let evt = nav.next_block().with_context(|| {
            format!("dvdnav_get_next_block at event {event_idx}")
        })?;
        match evt {
            NavEvent::Block { sector } => {
                let label = format!("nav sector {sectors_processed}");
                let contents = scan_sector(sector, &label).with_context(|| {
                    format!("parsing nav block {sectors_processed}")
                })?;
                for pes in &contents.pes_packets {
                    dispatch_pes(
                        pes,
                        &mut video,
                        &mut audio_handlers,
                        &mut subp_handlers,
                        sub_pts_offset,
                    )?;
                }
                sectors_processed += 1;
                if sectors_processed % 100_000 == 0 {
                    log::info!("ripped {sectors_processed} sectors");
                }
            }
            NavEvent::Nop => {}
            NavEvent::StillFrame { length } => {
                if length == STILL_LENGTH_INFINITE {
                    log::debug!("still frame: indefinite — skipping");
                }
                nav.still_skip().context("dvdnav_still_skip")?;
            }
            NavEvent::SpuStreamChange | NavEvent::AudioStreamChange => {}
            NavEvent::VtsChange => log::info!("VTS change"),
            NavEvent::CellChange { cell_nr, .. } => {
                cell_changes += 1;
                let (cur_title, cur_pgcn, _) =
                    nav.current_title_program()
                        .context("dvdnav_current_title_program")?;
                if (cur_pgcn, cell_nr) != last_cell.unwrap_or((-1, -1)) {
                    log::debug!(
                        "cell change: title={cur_title} pgcn={cur_pgcn} cellN={cell_nr}"
                    );
                    last_cell = Some((cur_pgcn, cell_nr));
                }
                let title_u = u32::try_from(cur_title).unwrap_or(0);
                let pgcn_u = u16::try_from(cur_pgcn).unwrap_or(0);
                let cell_u = u8::try_from(cell_nr).unwrap_or(0);
                if let Some(cell) = lookup.get(title_u, pgcn_u, cell_u) {
                    if cell.stc_discontinuity {
                        stc_disc_boundaries += 1;
                    }
                }
            }
            NavEvent::NavPacket => {
                // VOBU presentation times from the NAV-pack PCI. When the
                // previous VOBU's end PTM exceeds this VOBU's start PTM the
                // clock jumped backward (cell stc_discontinuity), so add
                // the gap to the running offset to keep the title timeline
                // monotonic.
                if let Some((s_ptm, e_ptm)) = nav.current_vobu_ptm() {
                    if prev_vobu_e_ptm > s_ptm {
                        sub_pts_offset +=
                            i64::from(prev_vobu_e_ptm) - i64::from(s_ptm);
                    }
                    prev_vobu_e_ptm = e_ptm;
                }
            }
            NavEvent::Highlight
            | NavEvent::SpuClutChange
            | NavEvent::HopChannel
            | NavEvent::Other(_) => {}
            NavEvent::Stop => {
                log::info!("DVDNAV_STOP at event {event_idx}");
                break;
            }
            NavEvent::Wait => {
                nav.wait_skip().context("dvdnav_wait_skip")?;
            }
        }
        if !left_title {
            if let Ok((t, _)) = nav.current_title_part() {
                if t != i32::from(args.title) {
                    log::info!(
                        "left title {} -> currently in title {}; stopping",
                        args.title, t
                    );
                    left_title = true;
                }
            }
        }
        if left_title {
            break;
        }
    }

    // 6) Close handlers and write sidecar files.
    let video_first_pts = video.first_pts;
    finish_video_handler(video, &mut audio_handlers, &mut subp_handlers)?;

    // For each audio track, compute delay = first_audio_pts -
    // first_video_pts in ms. PTS is in 90 kHz units, so 1 ms = 90 ticks.
    let mut audio_summaries: Vec<(u8, AudioCodec, String, u8, i64, u64, PathBuf)> = Vec::new();
    for (stream_n, mut h) in std::mem::take(&mut audio_handlers) {
        h.writer.flush().context("flushing audio writer")?;
        let delay_ms: i64 = match (video_first_pts, h.first_pts) {
            (Some(v), Some(a)) => ((a as i64) - (v as i64)) / 90,
            _ => 0,
        };
        // Rename to include the delay literal MakeMKV uses.
        let final_path = audio_final_path(
            &args.out_dir,
            &title_prefix,
            h.track_number,
            &h.language,
            h.codec,
            delay_ms,
        );
        std::fs::rename(&h.temp_path, &final_path).with_context(|| {
            format!("rename {} -> {}", h.temp_path.display(), final_path.display())
        })?;
        audio_summaries.push((
            stream_n,
            h.codec,
            h.language.clone(),
            h.track_number,
            delay_ms,
            h.bytes_written,
            final_path,
        ));
    }

    // Subpictures: close each SubWriter, write the .idx file beside it.
    let mut subp_summaries: Vec<(u8, String, u8, u64, PathBuf, PathBuf)> = Vec::new();
    for (stream_n, h) in std::mem::take(&mut subp_handlers) {
        let SubpictureHandler {
            track_number,
            language,
            sub_path,
            idx_path,
            writer,
        } = h;
        let (_file, entries) = writer.finish().context("finishing SubWriter")?;
        let sub_bytes = std::fs::metadata(&sub_path)
            .map(|m| m.len())
            .unwrap_or(0);
        // Write .idx
        let mut idx_file = std::fs::File::create(&idx_path)
            .with_context(|| format!("creating {}", idx_path.display()))?;
        write_idx_file(
            &mut idx_file,
            &metadata.palette,
            &language,
            metadata.video_width,
            metadata.video_height,
            &entries,
        )
        .with_context(|| format!("writing {}", idx_path.display()))?;
        subp_summaries.push((stream_n, language, track_number, sub_bytes, sub_path, idx_path));
    }

    // Chapters XML.
    let chapters_path = args.out_dir.join(format!("{title_prefix}_chapters.xml"));
    {
        let mut f = File::create(&chapters_path)
            .with_context(|| format!("creating {}", chapters_path.display()))?;
        // Need to fetch the PGC + chapters via libdvdread (we don't
        // keep it alive across the rip — re-open just for this).
        let source = DvdSource::open(&args.disc).context("DvdSource::open (chapters)")?;
        let vmg = IfoHandle::open(source.reader(), IfoKind::Vmg).context("VMG IFO")?;
        let titles = vmg.titles();
        let title = titles
            .get(usize::from(args.title - 1))
            .ok_or_else(|| anyhow!("title {} out of range", args.title))?;
        let title_set_nr: u8 = { title.title_set_nr };
        let vts_ttn: u8 = { title.vts_ttn };
        let vts =
            IfoHandle::open(source.reader(), IfoKind::Vts(u32::from(title_set_nr)))
                .context("VTS IFO")?;
        let chapters_for = vts.chapters_for(vts_ttn);
        let chapters = chapters_for;
        let first_chapter = chapters
            .first()
            .ok_or_else(|| anyhow!("title has no chapters"))?;
        let pgcn: u16 = { first_chapter.pgcn };
        let pgcs = vts.pgcs();
        let srp = pgcs
            .get(usize::from(pgcn).saturating_sub(1))
            .ok_or_else(|| anyhow!("pgcn {pgcn} out of range"))?;
        let pgc_ptr = { srp.pgc };
        // SAFETY: see dump_title.rs — libdvdread populates the PGC
        // pointer when the IFO parses successfully.
        let pgc = unsafe { pgc_ptr.as_ref() }.ok_or_else(|| anyhow!("PGC ptr NULL"))?;
        write_chapters_xml(&mut f, pgc, chapters, u32::from(args.title), "eng")
            .context("writing chapters XML")?;
    }

    // 7) Stats + summary
    let cell_diag = cell_changes;
    check_eq(
        "rip-title: sectors processed > 0",
        sectors_processed > 0,
        true,
    );

    println!();
    println!("rip-title summary (title {}):", args.title);
    println!("  out dir:            {}", args.out_dir.display());
    println!("  sectors processed:  {sectors_processed}");
    println!("  cell changes:       {cell_diag} ({stc_disc_boundaries} with stc_discontinuity)");
    println!();
    println!("video track 1 ({}):", "eng");
    println!("  {}", video_path_for(&args.out_dir, &title_prefix, 1, "eng").display());
    println!();
    println!("audio tracks:");
    for (stream_n, codec, lang, track_n, delay_ms, bytes, path) in &audio_summaries {
        println!(
            "  track {track_n} substream {stream_n} {:?} [{lang}] delay {delay_ms}ms {bytes} bytes",
            codec
        );
        println!("    {}", path.display());
    }
    println!();
    println!("subpicture tracks:");
    for (stream_n, lang, track_n, bytes, sub_path, idx_path) in &subp_summaries {
        println!(
            "  track {track_n} substream {stream_n} [{lang}] {bytes} bytes",
        );
        println!("    {}", sub_path.display());
        println!("    {}", idx_path.display());
    }
    println!();
    println!("chapters: {}", chapters_path.display());

    Ok(())
}

// ============================================================================
// Helpers
// ============================================================================

struct TitleMetadata {
    vts_nr: u8,
    audio_attrs: Vec<audio_attr_t>,
    subp_attrs: Vec<subp_attr_t>,
    palette: [u32; 16],
    video_width: u32,
    video_height: u32,
    chapter_count: usize,
}

impl TitleMetadata {
    fn resolve(reader: &disc_dvd::DvdReader, title: u8) -> Result<Self> {
        let vmg = IfoHandle::open(reader, IfoKind::Vmg).context("VMG IFO")?;
        let titles = vmg.titles();
        let title_info = titles
            .get(usize::from(title - 1))
            .ok_or_else(|| anyhow!("title {} out of range", title))?;
        let vts_nr: u8 = { title_info.title_set_nr };
        let vts_ttn: u8 = { title_info.vts_ttn };

        let vts =
            IfoHandle::open(reader, IfoKind::Vts(u32::from(vts_nr))).context("VTS IFO")?;
        let chapters = vts.chapters_for(vts_ttn);
        let chapter_count = chapters.len();
        let first_chapter = chapters
            .first()
            .ok_or_else(|| anyhow!("title has no chapters"))?;
        let pgcn: u16 = { first_chapter.pgcn };
        let pgcs = vts.pgcs();
        let srp = pgcs
            .get(usize::from(pgcn).saturating_sub(1))
            .ok_or_else(|| anyhow!("pgcn {pgcn} out of range"))?;
        let pgc_ptr = { srp.pgc };
        let pgc = unsafe { pgc_ptr.as_ref() }.ok_or_else(|| anyhow!("PGC ptr NULL"))?;
        let palette: [u32; 16] = { pgc.palette };

        let vtsi_mat = vts.vtsi_mat().ok_or_else(|| anyhow!("vtsi_mat NULL"))?;
        let nr_audio: u8 = { vtsi_mat.nr_of_vts_audio_streams };
        let nr_subp: u8 = { vtsi_mat.nr_of_vts_subp_streams };
        let audio_arr: [audio_attr_t; 8] = { vtsi_mat.vts_audio_attr };
        let subp_arr: [subp_attr_t; 32] = { vtsi_mat.vts_subp_attr };
        let audio_attrs: Vec<audio_attr_t> = audio_arr
            .iter()
            .take(usize::from(nr_audio))
            .copied()
            .collect();
        let subp_attrs: Vec<subp_attr_t> = subp_arr
            .iter()
            .take(usize::from(nr_subp))
            .copied()
            .collect();

        let video_attr: disc_dvd::ifo::video_attr_t = { vtsi_mat.vts_video_attr };
        let picture_size = video_attr.picture_size();
        let video_format = video_attr.video_format();
        let (w, h) = disc_dvd::decode::video_picture_size(picture_size, video_format);
        let video_width = u32::from(w);
        let video_height = u32::from(h);

        Ok(Self {
            vts_nr,
            audio_attrs,
            subp_attrs,
            palette,
            video_width,
            video_height,
            chapter_count,
        })
    }
}

fn audio_codec_from_attr(attr: &audio_attr_t) -> AudioCodec {
    match attr.audio_format() {
        0 => AudioCodec::Ac3,
        2 | 3 => AudioCodec::MpegAudio, // MPEG-1/2 audio
        4 => AudioCodec::Lpcm,
        6 => AudioCodec::Dts,
        _ => AudioCodec::Ac3, // default
    }
}

fn video_path_for(
    out_dir: &std::path::Path,
    title_prefix: &str,
    track_number: u8,
    language: &str,
) -> PathBuf {
    out_dir.join(format!("{title_prefix}_track{track_number}_[{language}].mpg"))
}

fn open_video_handler(
    out_dir: &std::path::Path,
    title_prefix: &str,
    track_number: u8,
) -> Result<VideoHandler> {
    let language = "eng".to_string(); // DVD-Video video doesn't carry a language code
    let video_path = video_path_for(out_dir, title_prefix, track_number, &language);
    let cc_path = out_dir.join(format!(
        "{title_prefix}_track{track_number}_[{language}].cc.bin"
    ));
    let video_writer = BufWriter::with_capacity(
        64 * 1024,
        File::create(&video_path)
            .with_context(|| format!("creating {}", video_path.display()))?,
    );
    let cc_writer = BufWriter::with_capacity(
        64 * 1024,
        File::create(&cc_path).with_context(|| format!("creating {}", cc_path.display()))?,
    );
    Ok(VideoHandler {
        track_number,
        language,
        video_path,
        cc_path,
        video_writer,
        cc_writer,
        filter: UserDataFilter::new(),
        first_pts: None,
    })
}

fn open_audio_handler(
    out_dir: &std::path::Path,
    title_prefix: &str,
    track_number: u8,
    language: &str,
    codec: AudioCodec,
) -> Result<AudioHandler> {
    // Use a temp filename with no DELAY suffix; we rename after we know
    // the delay value.
    let temp_path = out_dir.join(format!(
        "{title_prefix}_track{track_number}_[{language}].{}.tmp",
        codec.file_ext()
    ));
    let writer = BufWriter::with_capacity(
        64 * 1024,
        File::create(&temp_path)
            .with_context(|| format!("creating {}", temp_path.display()))?,
    );
    let payload_strip = match codec {
        AudioCodec::Ac3 | AudioCodec::Dts => 3, // BD common header
        AudioCodec::Lpcm => 6,                  // BD common + LPCM 3-byte
        AudioCodec::MpegAudio => 0,
    };
    Ok(AudioHandler {
        codec,
        track_number,
        language: language.to_string(),
        temp_path,
        writer,
        first_pts: None,
        bytes_written: 0,
        payload_strip,
    })
}

fn audio_final_path(
    out_dir: &std::path::Path,
    title_prefix: &str,
    track_number: u8,
    language: &str,
    codec: AudioCodec,
    delay_ms: i64,
) -> PathBuf {
    out_dir.join(format!(
        "{title_prefix}_track{track_number}_[{language}]_DELAY {delay_ms}ms.{}",
        codec.file_ext()
    ))
}

fn open_subpicture_handler(
    out_dir: &std::path::Path,
    title_prefix: &str,
    track_number: u8,
    language: &str,
) -> Result<SubpictureHandler> {
    let sub_path = out_dir.join(format!(
        "{title_prefix}_track{track_number}_[{language}].sub"
    ));
    let idx_path = out_dir.join(format!(
        "{title_prefix}_track{track_number}_[{language}].idx"
    ));
    let file = File::create(&sub_path)
        .with_context(|| format!("creating {}", sub_path.display()))?;
    // Each .sub is a standalone single-stream file, so the subpicture
    // substream id is always 0x20 (first of the DVD 0x20..=0x3F range),
    // regardless of the stream's IFO position — matching MakeMKV's
    // per-track extraction.
    let substream_id = 0x20u8;
    let writer = SubWriter::new(file, substream_id);
    Ok(SubpictureHandler {
        track_number,
        language: language.to_string(),
        sub_path,
        idx_path,
        writer,
    })
}

fn dispatch_pes(
    pes: &PesPacket<'_>,
    video: &mut VideoHandler,
    audio_handlers: &mut BTreeMap<u8, AudioHandler>,
    subp_handlers: &mut BTreeMap<u8, SubpictureHandler>,
    sub_pts_offset: i64,
) -> Result<()> {
    let kind = stream_kind(pes.stream_id, pes.substream_id);
    match kind {
        StreamKind::Video(_) => {
            if video.first_pts.is_none() {
                video.first_pts = pes.pts;
            }
            video
                .filter
                .feed(pes.payload, &mut video.video_writer, &mut video.cc_writer)
                .with_context(|| "video user_data filter")?;
        }
        StreamKind::Ac3(n) | StreamKind::Dts(n) | StreamKind::Lpcm(n) => {
            if let Some(h) = audio_handlers.get_mut(&n) {
                if h.first_pts.is_none() {
                    h.first_pts = pes.pts;
                }
                let bytes = if pes.payload.len() > h.payload_strip {
                    &pes.payload[h.payload_strip..]
                } else {
                    &[]
                };
                h.writer.write_all(bytes).with_context(|| {
                    format!("writing audio track {}", h.track_number)
                })?;
                h.bytes_written += bytes.len() as u64;
            }
        }
        StreamKind::MpegAudio(_) => {
            // MPEG audio uses stream_id index, not substream — defer
            // to future support if any test disc needs it.
        }
        StreamKind::Subpicture(n) => {
            if let Some(h) = subp_handlers.get_mut(&n) {
                // First PES of an SPU carries a PTS; later PESes of
                // a multi-PES SPU don't and are written as
                // continuation sectors.
                if let Some(pts) = pes.pts {
                    // Title-relative PTS (ISO/IEC 13818-1, 90 kHz):
                    // subtract the title's first video PTS, then add the
                    // accumulated NAV-pack offset so the subpicture
                    // timeline stays continuous across cell
                    // stc_discontinuity. Full precision — no container
                    // rounding. `first_pts` is set by the first video PES,
                    // which precedes any subpicture in a title.
                    let base = video.first_pts.unwrap_or(0);
                    let rel = ((pts as i64) - (base as i64) + sub_pts_offset)
                        .max(0) as u64;
                    h.writer
                        .write_subtitle(rel, pes.payload)
                        .with_context(|| {
                            format!("writing subpicture track {}", h.track_number)
                        })?;
                } else {
                    h.writer
                        .write_continuation(pes.payload)
                        .with_context(|| {
                            format!(
                                "writing subpicture continuation track {}",
                                h.track_number
                            )
                        })?;
                }
            }
        }
        StreamKind::SystemHeader
        | StreamKind::Padding
        | StreamKind::NavPack
        | StreamKind::Unknown { .. } => {
            // Dropped — same as the standard Demuxer behavior.
        }
    }
    Ok(())
}

fn finish_video_handler(
    video: VideoHandler,
    _audio_handlers: &mut BTreeMap<u8, AudioHandler>,
    _subp_handlers: &mut BTreeMap<u8, SubpictureHandler>,
) -> Result<()> {
    let VideoHandler {
        mut video_writer,
        mut cc_writer,
        filter,
        ..
    } = video;
    let stats = filter
        .finish(&mut video_writer, &mut cc_writer)
        .context("flushing user_data filter")?;
    video_writer.flush().context("flushing video writer")?;
    cc_writer.flush().context("flushing CC writer")?;
    check(
        "rip-title: video stream had at least one byte",
        &format!("video_bytes={} user_data_bytes={}", stats.video_bytes, stats.user_data_bytes),
        || stats.video_bytes > 0,
    );
    Ok(())
}
