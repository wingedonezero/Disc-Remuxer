//! `disc-remuxer info <path>` — dump everything libdvdread tells us about
//! a DVD.
//!
//! Output uses libdvdread field names exactly (`vmgi_mat::vmg_category`,
//! `tt_srpt::title_set_nr`, `pgc_t::nr_of_cells`, `audio_attr_t::audio_format`,
//! etc.) so each line can be grepped against the public headers under
//! `<dvdread/ifo_types.h>`. Bit-packed fields are decoded into
//! human-readable names via `disc_dvd::decode`.

use std::path::Path;

use anyhow::{anyhow, Context, Result};

use disc_core::{detect_disc_type, DiscType};
use disc_dvd::decode;
use disc_dvd::ifo::{
    audio_attr_t, cell_playback_t, format_dvd_time, pgc_t, ptt_info_t, subp_attr_t,
    title_info_t, video_attr_t, vtsi_mat_t, IfoHandle, IfoKind,
};
use disc_dvd::DvdSource;

// The libdvdread structs are `#[repr(packed)]` (they mirror DVD on-disc
// layout — byte 1 = the first field, no padding). That means we cannot
// take references to individual fields. The `{ value.field }` block
// syntax used in the format args throughout creates an rvalue copy,
// which works around the alignment constraint.

pub fn run(path: &Path) -> Result<()> {
    let disc_type = detect_disc_type(path).context("detect_disc_type")?;
    log::info!(
        "detected disc type: {} at {}",
        disc_type.as_str(),
        path.display()
    );

    match disc_type {
        DiscType::Dvd => print_dvd_info(path),
        DiscType::Bluray | DiscType::UltraHd => Err(anyhow!(
            "{} support not yet implemented",
            disc_type.as_str()
        )),
    }
}

// ---------------------------------------------------------------------------
// Top-level walk
// ---------------------------------------------------------------------------

fn print_dvd_info(path: &Path) -> Result<()> {
    let source = DvdSource::open(path).context("DvdSource::open")?;
    let vmg = IfoHandle::open(source.reader(), IfoKind::Vmg)
        .context("opening VMG IFO (VIDEO_TS.IFO)")?;

    print_vmg_summary(&vmg);

    let titles = vmg.titles();
    if titles.is_empty() {
        println!();
        println!("(no titles found in tt_srpt)");
        return Ok(());
    }

    println!();
    println!("titles ({} entries in tt_srpt):", titles.len());

    for (idx, title) in titles.iter().enumerate() {
        let title_number = idx + 1;
        print_title_summary(title_number, title);

        let title_set_nr = { title.title_set_nr };
        let vts_ttn = { title.vts_ttn };
        let vts_kind = IfoKind::Vts(u32::from(title_set_nr));
        match IfoHandle::open(source.reader(), vts_kind) {
            Ok(vts_ifo) => print_vts_detail(&vts_ifo, vts_ttn),
            Err(e) => {
                log::warn!(
                    "could not open VTS IFO {title_set_nr} for title {title_number}: {e}"
                );
                println!("      (could not open VTS_{title_set_nr:02}_0.IFO: {e})");
            }
        }
    }

    Ok(())
}

// ---------------------------------------------------------------------------
// VMG summary
// ---------------------------------------------------------------------------

fn print_vmg_summary(vmg: &IfoHandle<'_>) {
    let Some(vmgi_mat) = vmg.vmgi_mat() else {
        println!("VMG IFO opened but vmgi_mat was NULL (parse failure?)");
        return;
    };

    let id_bytes: &[u8] = unsafe {
        std::slice::from_raw_parts(
            vmgi_mat.vmg_identifier.as_ptr().cast::<u8>(),
            vmgi_mat.vmg_identifier.len(),
        )
    };
    let provider_bytes: &[u8] = unsafe {
        std::slice::from_raw_parts(
            vmgi_mat.provider_identifier.as_ptr().cast::<u8>(),
            vmgi_mat.provider_identifier.len(),
        )
    };
    let provider = String::from_utf8_lossy(provider_bytes);
    let provider = provider.trim_end_matches(['\0', ' ']);

    println!("VMG (VIDEO_TS.IFO):");
    println!("  vmg_identifier:           {:?}", String::from_utf8_lossy(id_bytes));
    println!("  specification_version:    0x{:02x}", { vmgi_mat.specification_version });
    println!("  vmg_category:             0x{:08x}", { vmgi_mat.vmg_category });
    println!("  vmg_nr_of_volumes:        {}", { vmgi_mat.vmg_nr_of_volumes });
    println!("  vmg_this_volume_nr:       {}", { vmgi_mat.vmg_this_volume_nr });
    println!("  disc_side:                {}", { vmgi_mat.disc_side });
    println!("  vmg_nr_of_title_sets:     {}", { vmgi_mat.vmg_nr_of_title_sets });
    println!("  vmg_last_sector:          {}", { vmgi_mat.vmg_last_sector });
    println!("  vmgi_last_sector:         {}", { vmgi_mat.vmgi_last_sector });
    println!("  vmgi_last_byte:           {}", { vmgi_mat.vmgi_last_byte });
    println!("  nr_of_vmgm_audio_streams: {}", { vmgi_mat.nr_of_vmgm_audio_streams });
    println!("  nr_of_vmgm_subp_streams:  {}", { vmgi_mat.nr_of_vmgm_subp_streams });
    println!("  provider_identifier:      {provider:?}");
}

// ---------------------------------------------------------------------------
// Title summary (from VMG's tt_srpt)
// ---------------------------------------------------------------------------

fn print_title_summary(title_number: usize, title: &title_info_t) {
    println!();
    println!("  Title {title_number} (tt_srpt[{}]):", title_number - 1);
    println!("    title_set_nr:       {}  (VTS that owns this title)", { title.title_set_nr });
    println!("    vts_ttn:            {}  (title number within VTS)", { title.vts_ttn });
    println!("    nr_of_ptts:         {}  (chapter count)", { title.nr_of_ptts });
    println!("    nr_of_angles:       {}  (angle count)", { title.nr_of_angles });
    println!("    parental_id:        0x{:04x}", { title.parental_id });
    println!("    title_set_sector:   {}  (where the VTS starts on disc)", { title.title_set_sector });
}

// ---------------------------------------------------------------------------
// Per-VTS detail (vtsi_mat + stream attribute tables + PGC + chapters)
// ---------------------------------------------------------------------------

fn print_vts_detail(vts_ifo: &IfoHandle<'_>, target_vts_ttn: u8) {
    let Some(vtsi_mat) = vts_ifo.vtsi_mat() else {
        println!("    (vtsi_mat NULL — VTS parse failure)");
        return;
    };

    print_vtsi_summary(vtsi_mat);
    print_video_attr(vtsi_mat);
    print_audio_streams(vtsi_mat);
    print_subp_streams(vtsi_mat);
    print_chapter_list(vts_ifo, target_vts_ttn);
    print_pgc_detail(vts_ifo, vtsi_mat, target_vts_ttn);
}

fn print_vtsi_summary(vtsi_mat: &vtsi_mat_t) {
    println!("    vtsi_mat:");
    println!("      vts_category:             0x{:08x}", { vtsi_mat.vts_category });
    println!("      specification_version:    0x{:02x}", { vtsi_mat.specification_version });
    println!("      vts_last_sector:          {}", { vtsi_mat.vts_last_sector });
    println!("      vtsi_last_sector:         {}", { vtsi_mat.vtsi_last_sector });
    println!("      vtsi_last_byte:           {}", { vtsi_mat.vtsi_last_byte });
}

// --- Video attributes (vtsi_mat::vts_video_attr) ---

fn print_video_attr(vtsi_mat: &vtsi_mat_t) {
    let attr: video_attr_t = { vtsi_mat.vts_video_attr };
    let mpeg = attr.mpeg_version();
    let vfmt = attr.video_format();
    let aspect = attr.display_aspect_ratio();
    let permitted_df = attr.permitted_df();
    let picture_size = attr.picture_size();
    let letterboxed = attr.letterboxed();
    let film_mode = attr.film_mode();
    let line21_cc_1 = attr.line21_cc_1();
    let line21_cc_2 = attr.line21_cc_2();
    let bit_rate = attr.bit_rate();
    let (w, h) = decode::video_picture_size(picture_size, vfmt);

    println!("    vts_video_attr:");
    println!("      mpeg_version:          {} ({})", mpeg, decode::video_mpeg_version(mpeg));
    println!("      video_format:          {} ({})", vfmt, decode::video_format(vfmt));
    println!("      display_aspect_ratio:  {} ({})", aspect, decode::video_aspect_ratio(aspect));
    println!("      permitted_df:          {} ({})", permitted_df, decode::video_permitted_df(permitted_df));
    println!("      picture_size:          {picture_size} => {w}x{h}");
    println!("      letterboxed:           {letterboxed}");
    println!("      film_mode:             {film_mode}  (0=video, 1=film/telecine)");
    println!("      line21_cc_1:           {line21_cc_1}  (NTSC line-21 Closed Captions field 1)");
    println!("      line21_cc_2:           {line21_cc_2}  (NTSC line-21 Closed Captions field 2)");
    println!("      bit_rate:              {bit_rate}  (0=variable, 1=constant)");
}

// --- Audio streams (vtsi_mat::vts_audio_attr[0..nr_of_vts_audio_streams]) ---

fn print_audio_streams(vtsi_mat: &vtsi_mat_t) {
    let nr_raw: u8 = vtsi_mat.nr_of_vts_audio_streams;
    let n = usize::from(nr_raw);
    let attrs: [audio_attr_t; 8] = { vtsi_mat.vts_audio_attr };
    println!("    vts_audio_attr ({n} active streams of 8 slots):");
    if n == 0 {
        println!("      (no audio streams)");
        return;
    }
    for (i, attr) in attrs.iter().take(n).enumerate() {
        print_audio_attr(i, attr);
    }
}

fn print_audio_attr(index: usize, attr: &audio_attr_t) {
    let fmt = attr.audio_format();
    let multich = attr.multichannel_extension();
    let lang_type = attr.lang_type();
    let app_mode = attr.application_mode();
    let quant = attr.quantization();
    let freq = attr.sample_frequency();
    let lang_code_raw: u16 = { attr.lang_code };
    let lang_ext: u8 = { attr.lang_extension };
    let code_ext: u8 = { attr.code_extension };

    // `channels` is "channels - 1" in the wire format (so 0 = mono,
    // 1 = stereo). bindgen doesn't always emit an accessor for it, so we
    // pull it from the bitfield by offset.
    let channels_minus_1 = read_audio_channels_minus_1(attr);

    let lang = decode::lang_code(lang_code_raw)
        .unwrap_or_else(|| format!("0x{lang_code_raw:04x} (invalid)"));

    let stream_id = decode::audio_stream_id(fmt, u8::try_from(index).unwrap_or(0));

    println!("      [{index}] audio_format={fmt} ({})", decode::audio_format(fmt));
    println!("          channels={} ({}ch)", channels_minus_1, channels_minus_1 + 1);
    println!("          sample_frequency={freq} ({})", decode::audio_sample_frequency(freq));
    let quant_str = match fmt {
        4 => decode::audio_lpcm_quantization(quant),
        2 | 3 => decode::audio_mpeg_drc(quant),
        _ => "n/a for this format",
    };
    println!("          quantization={quant} ({quant_str})");
    println!("          multichannel_extension={multich}");
    println!("          application_mode={app_mode} ({})", decode::audio_application_mode(app_mode));
    println!("          lang_type={lang_type} (1 = language code present)");
    println!("          lang_code=0x{lang_code_raw:04x} ({lang})");
    println!("          lang_extension={lang_ext} ({})", decode::audio_lang_extension(lang_ext));
    println!("          code_extension={code_ext}");
    match stream_id {
        Some((main, Some(sub))) => println!(
            "          PS stream id:           0x{main:02X} substream 0x{sub:02X}"
        ),
        Some((main, None)) => println!("          PS stream id:           0x{main:02X}"),
        None => println!("          PS stream id:           (unknown format)"),
    }
}

fn read_audio_channels_minus_1(attr: &audio_attr_t) -> u8 {
    // `audio_attr_t::_bitfield_1` packs (per the DVD-Video spec):
    //   audio_format (3) | multichannel_extension (1) | lang_type (2) |
    //   application_mode (2) | quantization (2) | sample_frequency (2) |
    //   unknown1 (1) | channels (3)
    // For total of 16 bits. The `channels` field starts at bit offset 13.
    attr._bitfield_1.get(13, 3) as u8
}

// --- Subpicture streams (vtsi_mat::vts_subp_attr[0..nr_of_vts_subp_streams]) ---

fn print_subp_streams(vtsi_mat: &vtsi_mat_t) {
    let nr_raw: u8 = vtsi_mat.nr_of_vts_subp_streams;
    let n = usize::from(nr_raw);
    let attrs: [subp_attr_t; 32] = { vtsi_mat.vts_subp_attr };
    println!("    vts_subp_attr ({n} active streams of 32 slots):");
    if n == 0 {
        println!("      (no subpicture streams)");
        return;
    }
    for (i, attr) in attrs.iter().take(n).enumerate() {
        let code_mode = attr.code_mode();
        let ty = attr.type_();
        let lang_code_raw: u16 = { attr.lang_code };
        let lang_ext: u8 = { attr.lang_extension };
        let code_ext: u8 = { attr.code_extension };
        let lang = decode::lang_code(lang_code_raw)
            .unwrap_or_else(|| format!("0x{lang_code_raw:04x} (invalid)"));
        let (main, sub) = decode::subp_stream_id(u8::try_from(i).unwrap_or(0));

        println!("      [{i}] code_mode={code_mode}  type=0x{ty:02x}");
        println!("          lang_code=0x{lang_code_raw:04x} ({lang})");
        println!("          lang_extension={lang_ext} ({})", decode::subp_lang_extension(lang_ext));
        println!("          code_extension={code_ext}");
        println!("          PS stream id:           0x{main:02X} substream 0x{sub:02X}");
    }
}

// --- Chapter list (vts_ptt_srpt -> ttu[vts_ttn-1] -> ptt[]) ---

fn print_chapter_list(vts_ifo: &IfoHandle<'_>, vts_ttn: u8) {
    let chapters: &[ptt_info_t] = vts_ifo.chapters_for(vts_ttn);
    println!("    chapters for vts_ttn={vts_ttn} ({} entries):", chapters.len());
    if chapters.is_empty() {
        println!("      (no chapter entries)");
        return;
    }
    for (i, ptt) in chapters.iter().enumerate() {
        let pgcn = { ptt.pgcn };
        let pgn = { ptt.pgn };
        println!("      ch {:>2}: pgcn={pgcn:>3}  pgn={pgn:>3}", i + 1);
    }
}

// --- Per-PGC: cell summary + audio_control + subp_control ---

fn print_pgc_detail(vts_ifo: &IfoHandle<'_>, vtsi_mat: &vtsi_mat_t, target_vts_ttn: u8) {
    let pgcs = vts_ifo.pgcs();
    let pgcit_count: u16 = vts_ifo.vts_pgcit().map_or(0, |p| { p.nr_of_pgci_srp });
    println!("    vts_pgcit:");
    println!(
        "      nr_of_pgci_srp:   {pgcit_count}  ({} PGCs in this VTS)",
        pgcs.len()
    );

    // libdvdread's title-to-PGC mapping: the PGC playing this title is at
    // index `vts_ttn - 1` within the VTS's pgci_srp array.
    if let Some(srp) = pgcs.get(usize::from(target_vts_ttn.saturating_sub(1))) {
        let pgc_ptr = { srp.pgc };
        if let Some(pgc) = unsafe { pgc_ptr.as_ref() } {
            print_pgc_summary(pgc, vtsi_mat);
        } else {
            println!("    (pgc pointer for vts_ttn={target_vts_ttn} was NULL)");
        }
    }
}

fn print_pgc_summary(pgc: &pgc_t, vtsi_mat: &vtsi_mat_t) {
    println!("    pgc (this title's playback chain):");
    println!("      nr_of_programs:   {}  (chapter count from PGC side)", { pgc.nr_of_programs });
    println!("      nr_of_cells:      {}", { pgc.nr_of_cells });
    let playback_time = { pgc.playback_time };
    println!(
        "      playback_time:    {}  (HH:MM:SS.FF, BCD-encoded)",
        format_dvd_time(&playback_time)
    );
    println!("      still_time:       {}  (seconds; 0xff = infinite)", { pgc.still_time });

    print_pgc_audio_control(pgc, vtsi_mat);
    print_pgc_subp_control(pgc, vtsi_mat);
    print_pgc_cells(pgc);
}

fn print_pgc_audio_control(pgc: &pgc_t, vtsi_mat: &vtsi_mat_t) {
    let ctrl: [u16; 8] = { pgc.audio_control };
    let attrs: [audio_attr_t; 8] = { vtsi_mat.vts_audio_attr };
    let nr = { vtsi_mat.nr_of_vts_audio_streams };

    let active: Vec<(usize, u16)> = ctrl
        .iter()
        .enumerate()
        .filter(|(_, &c)| decode::pgc_audio_control_available(c))
        .map(|(i, &c)| (i, c))
        .collect();

    println!("      audio_control (active in PGC: {}):", active.len());
    if active.is_empty() {
        println!("        (no audio streams enabled in this PGC)");
        return;
    }
    for (slot, ctrl_word) in active {
        let stream_num = decode::pgc_audio_control_stream_number(ctrl_word);
        let attr = attrs.get(usize::from(stream_num));
        let fmt = attr.map_or(0, audio_attr_t::audio_format);
        let lang = attr
            .and_then(|a| {
                let raw = { a.lang_code };
                decode::lang_code(raw)
            })
            .unwrap_or_default();
        let stream_id = decode::audio_stream_id(fmt, stream_num);
        let id_str = match stream_id {
            Some((main, Some(sub))) => format!("PS 0x{main:02X}/sub 0x{sub:02X}"),
            Some((main, None)) => format!("PS 0x{main:02X}"),
            None => "PS unknown".into(),
        };
        let warn = if usize::from(stream_num) < usize::from(nr) {
            ""
        } else {
            "  WARN: stream_num >= nr_of_vts_audio_streams"
        };
        println!(
            "        slot {slot} -> stream_num={stream_num}  ctrl=0x{ctrl_word:04x}  {}  {lang:<2}  {id_str}{warn}",
            decode::audio_format(fmt),
        );
    }
}

fn print_pgc_subp_control(pgc: &pgc_t, vtsi_mat: &vtsi_mat_t) {
    let ctrl: [u32; 32] = { pgc.subp_control };
    let attrs: [subp_attr_t; 32] = { vtsi_mat.vts_subp_attr };
    let nr = { vtsi_mat.nr_of_vts_subp_streams };

    let active: Vec<(usize, u32)> = ctrl
        .iter()
        .enumerate()
        .filter(|(_, &c)| decode::pgc_subp_control_available(c))
        .map(|(i, &c)| (i, c))
        .collect();

    println!("      subp_control (active in PGC: {}):", active.len());
    if active.is_empty() {
        println!("        (no subpicture streams enabled in this PGC)");
        return;
    }
    for (slot, ctrl_word) in active {
        let s43 = decode::pgc_subp_control_stream_4_3(ctrl_word);
        let sw = decode::pgc_subp_control_stream_wide(ctrl_word);
        let sl = decode::pgc_subp_control_stream_letterbox(ctrl_word);
        let sp = decode::pgc_subp_control_stream_pan_scan(ctrl_word);
        let attr = attrs.get(usize::from(s43));
        let lang = attr
            .and_then(|a| {
                let raw = { a.lang_code };
                decode::lang_code(raw)
            })
            .unwrap_or_default();
        let (main, sub) = decode::subp_stream_id(s43);
        let warn = if usize::from(s43) < usize::from(nr) {
            ""
        } else {
            "  WARN: stream_num >= nr_of_vts_subp_streams"
        };
        println!(
            "        slot {slot} -> ctrl=0x{ctrl_word:08x}  streams[4:3={s43} wide={sw} letterbox={sl} pan&scan={sp}]  {lang:<2}  PS 0x{main:02X}/sub 0x{sub:02X}{warn}"
        );
    }
}

fn print_pgc_cells(pgc: &pgc_t) {
    let cell_playback_ptr = { pgc.cell_playback };
    let nr_of_cells = { pgc.nr_of_cells };
    if cell_playback_ptr.is_null() || nr_of_cells == 0 {
        return;
    }
    let cells: &[cell_playback_t] = unsafe {
        std::slice::from_raw_parts(cell_playback_ptr, usize::from(nr_of_cells))
    };
    let preview_len = cells.len().min(3);
    println!("      cells (first {preview_len} of {}):", cells.len());
    for (i, cell) in cells.iter().take(preview_len).enumerate() {
        let first = { cell.first_sector };
        let last = { cell.last_sector };
        let sectors = last.saturating_sub(first);
        let ptime = { cell.playback_time };
        println!(
            "        [{i}] sectors {first}..{last} ({sectors:>7} blocks), playback_time={}",
            format_dvd_time(&ptime),
        );
    }
    if cells.len() > preview_len {
        println!("        ... ({} more cells)", cells.len() - preview_len);
    }
}
