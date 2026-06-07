//! `info` operation — dump everything libdvdread tells us about a DVD.
//!
//! Output uses libdvdread field names exactly (`vmgi_mat::vmg_category`,
//! `tt_srpt::title_set_nr`, `pgc_t::nr_of_cells`, `audio_attr_t::audio_format`,
//! etc.) so each line can be grepped against the public headers under
//! `<dvdread/ifo_types.h>`. Bit-packed fields are decoded into
//! human-readable names via [`crate::decode`].
//!
//! Read-only: opens the IFOs (VMG + each referenced VTS) through the passed
//! [`crate::DvdReader`] and runs the libdvdcss [`crate::CssProbe`] over the
//! reader's path. Every line is written to the supplied `w` so the caller
//! controls the sink (stdout in the CLI, a buffer in tests / the facade).

use anyhow::{Context, Result};

use crate::css::ProbeMethod;
use crate::decode;
use crate::ifo::{
    audio_attr_t, cell_playback_t, format_dvd_time, pgc_t, ptt_info_t, subp_attr_t,
    title_info_t, video_attr_t, vtsi_mat_t, IfoHandle, IfoKind,
};
use crate::{CssProbe, DvdReader};

// The libdvdread structs are `#[repr(packed)]` (they mirror DVD on-disc
// layout — byte 1 = the first field, no padding). That means we cannot
// take references to individual fields. The `{ value.field }` block
// syntax used in the format args throughout creates an rvalue copy,
// which works around the alignment constraint.

pub fn run(reader: &DvdReader, w: &mut dyn std::io::Write) -> Result<()> {
    print_css_probe(reader, w)?;

    let vmg = IfoHandle::open(reader, IfoKind::Vmg)
        .context("opening VMG IFO (VIDEO_TS.IFO)")?;

    print_vmg_summary(&vmg, w)?;
    print_vmg_extras(&vmg, w)?;

    let titles = vmg.titles();
    if titles.is_empty() {
        writeln!(w)?;
        writeln!(w, "(no titles found in tt_srpt)")?;
        return Ok(());
    }

    writeln!(w)?;
    writeln!(w, "titles ({} entries in tt_srpt):", titles.len())?;

    for (idx, title) in titles.iter().enumerate() {
        let title_number = idx + 1;
        print_title_summary(title_number, title, w)?;

        let title_set_nr = { title.title_set_nr };
        let vts_ttn = { title.vts_ttn };
        let vts_kind = IfoKind::Vts(u32::from(title_set_nr));
        match IfoHandle::open(reader, vts_kind) {
            Ok(vts_ifo) => print_vts_detail(&vts_ifo, vts_ttn, w)?,
            Err(e) => {
                log::warn!(
                    "could not open VTS IFO {title_set_nr} for title {title_number}: {e}"
                );
                writeln!(w, "      (could not open VTS_{title_set_nr:02}_0.IFO: {e})")?;
            }
        }
    }

    Ok(())
}

// ---------------------------------------------------------------------------
// CSS probe (libdvdcss — libdvdread does not expose this)
// ---------------------------------------------------------------------------

fn print_css_probe(reader: &DvdReader, w: &mut dyn std::io::Write) -> Result<()> {
    writeln!(w, "CSS protection:")?;
    let probe = match CssProbe::open(reader.path()) {
        Ok(p) => p,
        Err(e) => {
            writeln!(w, "  (libdvdcss could not open the path: {e})")?;
            log::warn!("CssProbe failed: {e}");
            return Ok(());
        }
    };

    match probe.method {
        ProbeMethod::LibdvdcssIoctl => {
            let verdict = if probe.is_scrambled {
                "SCRAMBLED  (libdvdcss will decrypt sectors transparently)"
            } else {
                "not scrambled  (sectors readable as plaintext)"
            };
            writeln!(w, "  probe method:  block device (libdvdcss DVD ioctls, authoritative)")?;
            writeln!(w, "  is_scrambled:  {} -> {verdict}", probe.is_scrambled)?;
        }
        ProbeMethod::IsoUdfSector => {
            let verdict = if probe.is_scrambled {
                "SCRAMBLED  (raw VOB sector does not start with MPEG-PS pack)"
            } else {
                "not scrambled  (raw VOB sector starts with MPEG-PS pack 00 00 01 ba)"
            };
            writeln!(w, "  probe method:  ISO file (libdvdread UDFFindFile -> raw block read)")?;
            if let Some(loc) = &probe.probed_location {
                writeln!(w, "  probed at:     {loc}")?;
            }
            writeln!(
                w,
                "  first 4 bytes: {:02x} {:02x} {:02x} {:02x}",
                probe.first_bytes[0],
                probe.first_bytes[1],
                probe.first_bytes[2],
                probe.first_bytes[3],
            )?;
            writeln!(w, "  is_scrambled:  {} -> {verdict}", probe.is_scrambled)?;
        }
        ProbeMethod::VobFile => {
            let verdict = if probe.is_scrambled {
                "SCRAMBLED  (VOB sector does not start with MPEG-PS pack — unusual for a directory rip)"
            } else {
                "not scrambled  (VOB sector starts with MPEG-PS pack 00 00 01 ba)"
            };
            writeln!(w, "  probe method:  directory rip (VOB file MPEG-PS magic check)")?;
            if let Some(loc) = &probe.probed_location {
                writeln!(w, "  probed VOB:    {loc}")?;
            }
            writeln!(
                w,
                "  first 4 bytes: {:02x} {:02x} {:02x} {:02x}",
                probe.first_bytes[0],
                probe.first_bytes[1],
                probe.first_bytes[2],
                probe.first_bytes[3],
            )?;
            writeln!(w, "  is_scrambled:  {} -> {verdict}", probe.is_scrambled)?;
        }
        ProbeMethod::Inconclusive => {
            writeln!(w, "  probe method:  inconclusive")?;
            writeln!(w, "  is_scrambled:  UNKNOWN (could not sample a VOB sector)")?;
        }
    }

    if let Some(err) = &probe.last_error {
        writeln!(w, "  libdvdcss last_error: {err:?}")?;
    }

    Ok(())
}

// ---------------------------------------------------------------------------
// VMG summary
// ---------------------------------------------------------------------------

fn print_vmg_summary(vmg: &IfoHandle<'_>, w: &mut dyn std::io::Write) -> Result<()> {
    let Some(vmgi_mat) = vmg.vmgi_mat() else {
        writeln!(w, "VMG IFO opened but vmgi_mat was NULL (parse failure?)")?;
        return Ok(());
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

    writeln!(w, "VMG (VIDEO_TS.IFO):")?;
    writeln!(w, "  vmg_identifier:           {:?}", String::from_utf8_lossy(id_bytes))?;
    writeln!(w, "  specification_version:    0x{:02x}", { vmgi_mat.specification_version })?;
    writeln!(w, "  vmg_category:             0x{:08x}", { vmgi_mat.vmg_category })?;
    writeln!(w, "  vmg_nr_of_volumes:        {}", { vmgi_mat.vmg_nr_of_volumes })?;
    writeln!(w, "  vmg_this_volume_nr:       {}", { vmgi_mat.vmg_this_volume_nr })?;
    writeln!(w, "  disc_side:                {}", { vmgi_mat.disc_side })?;
    writeln!(w, "  vmg_nr_of_title_sets:     {}", { vmgi_mat.vmg_nr_of_title_sets })?;
    writeln!(w, "  vmg_last_sector:          {}", { vmgi_mat.vmg_last_sector })?;
    writeln!(w, "  vmgi_last_sector:         {}", { vmgi_mat.vmgi_last_sector })?;
    writeln!(w, "  vmgi_last_byte:           {}", { vmgi_mat.vmgi_last_byte })?;
    writeln!(w, "  nr_of_vmgm_audio_streams: {}", { vmgi_mat.nr_of_vmgm_audio_streams })?;
    writeln!(w, "  nr_of_vmgm_subp_streams:  {}", { vmgi_mat.nr_of_vmgm_subp_streams })?;
    writeln!(w, "  provider_identifier:      {provider:?}")?;

    Ok(())
}

// --- VMG-side ancillary tables (first_play_pgc, parental, vts_atrt) ---

fn print_vmg_extras(vmg: &IfoHandle<'_>, w: &mut dyn std::io::Write) -> Result<()> {
    writeln!(w)?;
    writeln!(w, "VMG ancillary tables:")?;

    match vmg.first_play_pgc() {
        Some(pgc) => {
            let nr_of_cells: u8 = { pgc.nr_of_cells };
            let nr_of_programs: u8 = { pgc.nr_of_programs };
            let pt = { pgc.playback_time };
            writeln!(
                w,
                "  first_play_pgc:          present (nr_of_programs={nr_of_programs} nr_of_cells={nr_of_cells} playback_time={})",
                crate::ifo::format_dvd_time(&pt),
            )?;
        }
        None => writeln!(w, "  first_play_pgc:          (none)")?,
    }

    match vmg.ptl_mait() {
        Some(p) => {
            let nr_countries: u16 = { p.nr_of_countries };
            let nr_vtss: u16 = { p.nr_of_vtss };
            writeln!(
                w,
                "  ptl_mait:                nr_of_countries={nr_countries} nr_of_vtss={nr_vtss}"
            )?;
        }
        None => writeln!(w, "  ptl_mait:                (no parental management table)")?,
    }

    match vmg.vts_atrt() {
        Some(a) => {
            let nr_vtss: u16 = { a.nr_of_vtss };
            writeln!(w, "  vts_atrt:                nr_of_vtss={nr_vtss}")?;
        }
        None => writeln!(w, "  vts_atrt:                (no VTS attribute table)")?,
    }

    Ok(())
}

// ---------------------------------------------------------------------------
// Title summary (from VMG's tt_srpt)
// ---------------------------------------------------------------------------

fn print_title_summary(
    title_number: usize,
    title: &title_info_t,
    w: &mut dyn std::io::Write,
) -> Result<()> {
    writeln!(w)?;
    writeln!(w, "  Title {title_number} (tt_srpt[{}]):", title_number - 1)?;
    writeln!(w, "    title_set_nr:       {}  (VTS that owns this title)", { title.title_set_nr })?;
    writeln!(w, "    vts_ttn:            {}  (title number within VTS)", { title.vts_ttn })?;
    writeln!(w, "    nr_of_ptts:         {}  (chapter count)", { title.nr_of_ptts })?;
    writeln!(w, "    nr_of_angles:       {}  (angle count)", { title.nr_of_angles })?;
    writeln!(w, "    parental_id:        0x{:04x}", { title.parental_id })?;
    writeln!(w, "    title_set_sector:   {}  (where the VTS starts on disc)", { title.title_set_sector })?;

    Ok(())
}

// ---------------------------------------------------------------------------
// Per-VTS detail (vtsi_mat + stream attribute tables + PGC + chapters)
// ---------------------------------------------------------------------------

fn print_vts_detail(
    vts_ifo: &IfoHandle<'_>,
    target_vts_ttn: u8,
    w: &mut dyn std::io::Write,
) -> Result<()> {
    let Some(vtsi_mat) = vts_ifo.vtsi_mat() else {
        writeln!(w, "    (vtsi_mat NULL — VTS parse failure)")?;
        return Ok(());
    };

    print_vtsi_summary(vtsi_mat, w)?;
    print_video_attr(vtsi_mat, w)?;
    print_audio_streams(vtsi_mat, w)?;
    print_subp_streams(vtsi_mat, w)?;
    print_chapter_list(vts_ifo, target_vts_ttn, w)?;
    print_vts_address_tables(vts_ifo, w)?;
    print_pgc_detail(vts_ifo, vtsi_mat, target_vts_ttn, w)?;

    Ok(())
}

fn print_vtsi_summary(vtsi_mat: &vtsi_mat_t, w: &mut dyn std::io::Write) -> Result<()> {
    writeln!(w, "    vtsi_mat:")?;
    writeln!(w, "      vts_category:             0x{:08x}", { vtsi_mat.vts_category })?;
    writeln!(w, "      specification_version:    0x{:02x}", { vtsi_mat.specification_version })?;
    writeln!(w, "      vts_last_sector:          {}", { vtsi_mat.vts_last_sector })?;
    writeln!(w, "      vtsi_last_sector:         {}", { vtsi_mat.vtsi_last_sector })?;
    writeln!(w, "      vtsi_last_byte:           {}", { vtsi_mat.vtsi_last_byte })?;

    Ok(())
}

// --- Video attributes (vtsi_mat::vts_video_attr) ---

fn print_video_attr(vtsi_mat: &vtsi_mat_t, w: &mut dyn std::io::Write) -> Result<()> {
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
    let (width, height) = decode::video_picture_size(picture_size, vfmt);

    writeln!(w, "    vts_video_attr:")?;
    writeln!(w, "      mpeg_version:          {} ({})", mpeg, decode::video_mpeg_version(mpeg))?;
    writeln!(w, "      video_format:          {} ({})", vfmt, decode::video_format(vfmt))?;
    writeln!(w, "      display_aspect_ratio:  {} ({})", aspect, decode::video_aspect_ratio(aspect))?;
    writeln!(w, "      permitted_df:          {} ({})", permitted_df, decode::video_permitted_df(permitted_df))?;
    writeln!(w, "      picture_size:          {picture_size} => {width}x{height}")?;
    writeln!(w, "      letterboxed:           {letterboxed}")?;
    writeln!(w, "      film_mode:             {film_mode}  (0=video, 1=film/telecine)")?;
    writeln!(w, "      line21_cc_1:           {line21_cc_1}  (NTSC line-21 Closed Captions field 1)")?;
    writeln!(w, "      line21_cc_2:           {line21_cc_2}  (NTSC line-21 Closed Captions field 2)")?;
    writeln!(w, "      bit_rate:              {bit_rate}  (0=variable, 1=constant)")?;

    Ok(())
}

// --- Audio streams (vtsi_mat::vts_audio_attr[0..nr_of_vts_audio_streams]) ---

fn print_audio_streams(vtsi_mat: &vtsi_mat_t, w: &mut dyn std::io::Write) -> Result<()> {
    let nr_raw: u8 = vtsi_mat.nr_of_vts_audio_streams;
    let n = usize::from(nr_raw);
    let attrs: [audio_attr_t; 8] = { vtsi_mat.vts_audio_attr };
    writeln!(w, "    vts_audio_attr ({n} active streams of 8 slots):")?;
    if n == 0 {
        writeln!(w, "      (no audio streams)")?;
        return Ok(());
    }
    for (i, attr) in attrs.iter().take(n).enumerate() {
        print_audio_attr(i, attr, w)?;
    }

    Ok(())
}

fn print_audio_attr(
    index: usize,
    attr: &audio_attr_t,
    w: &mut dyn std::io::Write,
) -> Result<()> {
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

    writeln!(w, "      [{index}] audio_format={fmt} ({})", decode::audio_format(fmt))?;
    writeln!(w, "          channels={} ({}ch)", channels_minus_1, channels_minus_1 + 1)?;
    writeln!(w, "          sample_frequency={freq} ({})", decode::audio_sample_frequency(freq))?;
    let quant_str = match fmt {
        4 => decode::audio_lpcm_quantization(quant),
        2 | 3 => decode::audio_mpeg_drc(quant),
        _ => "n/a for this format",
    };
    writeln!(w, "          quantization={quant} ({quant_str})")?;
    writeln!(w, "          multichannel_extension={multich}")?;
    writeln!(w, "          application_mode={app_mode} ({})", decode::audio_application_mode(app_mode))?;
    writeln!(w, "          lang_type={lang_type} (1 = language code present)")?;
    writeln!(w, "          lang_code=0x{lang_code_raw:04x} ({lang})")?;
    writeln!(w, "          lang_extension={lang_ext} ({})", decode::audio_lang_extension(lang_ext))?;
    writeln!(w, "          code_extension={code_ext}")?;
    match stream_id {
        Some((main, Some(sub))) => writeln!(
            w,
            "          PS stream id:           0x{main:02X} substream 0x{sub:02X}"
        )?,
        Some((main, None)) => writeln!(w, "          PS stream id:           0x{main:02X}")?,
        None => writeln!(w, "          PS stream id:           (unknown format)")?,
    }

    Ok(())
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

fn print_subp_streams(vtsi_mat: &vtsi_mat_t, w: &mut dyn std::io::Write) -> Result<()> {
    let nr_raw: u8 = vtsi_mat.nr_of_vts_subp_streams;
    let n = usize::from(nr_raw);
    let attrs: [subp_attr_t; 32] = { vtsi_mat.vts_subp_attr };
    writeln!(w, "    vts_subp_attr ({n} active streams of 32 slots):")?;
    if n == 0 {
        writeln!(w, "      (no subpicture streams)")?;
        return Ok(());
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

        writeln!(w, "      [{i}] code_mode={code_mode}  type=0x{ty:02x}")?;
        writeln!(w, "          lang_code=0x{lang_code_raw:04x} ({lang})")?;
        writeln!(w, "          lang_extension={lang_ext} ({})", decode::subp_lang_extension(lang_ext))?;
        writeln!(w, "          code_extension={code_ext}")?;
        writeln!(w, "          PS stream id:           0x{main:02X} substream 0x{sub:02X}")?;
    }

    Ok(())
}

// --- VTS address tables (vts_c_adt + vts_vobu_admap + vts_tmapt) ---

fn print_vts_address_tables(vts_ifo: &IfoHandle<'_>, w: &mut dyn std::io::Write) -> Result<()> {
    let c_adt_rows = vts_ifo.cell_adr_table();
    let vobu_starts = vts_ifo.vobu_start_sectors();
    let tmaps = vts_ifo.tmaps();
    let nr_of_vobs: u16 = vts_ifo.vts_c_adt().map_or(0, |c| { c.nr_of_vobs });

    writeln!(
        w,
        "    vts_c_adt:        nr_of_vobs={nr_of_vobs}  cell_adr_table_entries={}",
        c_adt_rows.len(),
    )?;
    if !c_adt_rows.is_empty() {
        let preview = c_adt_rows.len().min(3);
        for entry in c_adt_rows.iter().take(preview) {
            let vob_id: u16 = { entry.vob_id };
            let cell_id: u8 = { entry.cell_id };
            let start: u32 = { entry.start_sector };
            let last: u32 = { entry.last_sector };
            writeln!(
                w,
                "      vob_id={vob_id:>3} cell_id={cell_id:>3} start={start} last={last}",
            )?;
        }
        if c_adt_rows.len() > preview {
            writeln!(w, "      ... ({} more)", c_adt_rows.len() - preview)?;
        }
    }

    writeln!(
        w,
        "    vts_vobu_admap:   vobu_start_sectors_entries={}{}",
        vobu_starts.len(),
        if vobu_starts.is_empty() {
            ""
        } else {
            "  (first/last VOBU starts logged below)"
        },
    )?;
    if let (Some(first), Some(last)) = (vobu_starts.first(), vobu_starts.last()) {
        writeln!(w, "      first VOBU start sector: {first}")?;
        writeln!(w, "      last  VOBU start sector: {last}")?;
    }

    writeln!(w, "    vts_tmapt:        nr_of_tmaps={}", tmaps.len())?;
    for (i, tmap) in tmaps.iter().enumerate() {
        let tmu: u8 = { tmap.tmu };
        let nr_entries: u16 = { tmap.nr_of_entries };
        writeln!(
            w,
            "      tmap[{i}]: tmu={tmu}s nr_of_entries={nr_entries}",
        )?;
    }

    Ok(())
}

// --- Chapter list (vts_ptt_srpt -> ttu[vts_ttn-1] -> ptt[]) ---

fn print_chapter_list(
    vts_ifo: &IfoHandle<'_>,
    vts_ttn: u8,
    w: &mut dyn std::io::Write,
) -> Result<()> {
    let chapters: &[ptt_info_t] = vts_ifo.chapters_for(vts_ttn);
    writeln!(w, "    chapters for vts_ttn={vts_ttn} ({} entries):", chapters.len())?;
    if chapters.is_empty() {
        writeln!(w, "      (no chapter entries)")?;
        return Ok(());
    }
    for (i, ptt) in chapters.iter().enumerate() {
        let pgcn = { ptt.pgcn };
        let pgn = { ptt.pgn };
        writeln!(w, "      ch {:>2}: pgcn={pgcn:>3}  pgn={pgn:>3}", i + 1)?;
    }

    Ok(())
}

// --- Per-PGC: cell summary + audio_control + subp_control ---

fn print_pgc_detail(
    vts_ifo: &IfoHandle<'_>,
    vtsi_mat: &vtsi_mat_t,
    target_vts_ttn: u8,
    w: &mut dyn std::io::Write,
) -> Result<()> {
    let pgcs = vts_ifo.pgcs();
    let pgcit_count: u16 = vts_ifo.vts_pgcit().map_or(0, |p| { p.nr_of_pgci_srp });
    writeln!(w, "    vts_pgcit:")?;
    writeln!(
        w,
        "      nr_of_pgci_srp:   {pgcit_count}  ({} PGCs in this VTS)",
        pgcs.len()
    )?;

    // libdvdread's title-to-PGC mapping: the PGC playing this title is at
    // index `vts_ttn - 1` within the VTS's pgci_srp array.
    if let Some(srp) = pgcs.get(usize::from(target_vts_ttn.saturating_sub(1))) {
        let pgc_ptr = { srp.pgc };
        if let Some(pgc) = unsafe { pgc_ptr.as_ref() } {
            print_pgc_summary(pgc, vtsi_mat, w)?;
        } else {
            writeln!(w, "    (pgc pointer for vts_ttn={target_vts_ttn} was NULL)")?;
        }
    }

    Ok(())
}

fn print_pgc_summary(
    pgc: &pgc_t,
    vtsi_mat: &vtsi_mat_t,
    w: &mut dyn std::io::Write,
) -> Result<()> {
    writeln!(w, "    pgc (this title's playback chain):")?;
    writeln!(w, "      nr_of_programs:   {}  (chapter count from PGC side)", { pgc.nr_of_programs })?;
    writeln!(w, "      nr_of_cells:      {}", { pgc.nr_of_cells })?;
    let playback_time = { pgc.playback_time };
    writeln!(
        w,
        "      playback_time:    {}  (HH:MM:SS.FF, BCD-encoded)",
        format_dvd_time(&playback_time)
    )?;
    writeln!(w, "      still_time:       {}  (seconds; 0xff = infinite)", { pgc.still_time })?;

    print_pgc_audio_control(pgc, vtsi_mat, w)?;
    print_pgc_subp_control(pgc, vtsi_mat, w)?;
    print_pgc_palette(pgc, w)?;
    print_pgc_cells(pgc, w)?;

    Ok(())
}

/// Dump the 16-entry PGC color lookup table. Each entry is packed
/// `0x00 Y Cr Cb` (8-bit components) per the DVD-Video spec; subpicture
/// pixels index into this CLUT and it feeds the VobSub `.idx` palette.
fn print_pgc_palette(pgc: &pgc_t, w: &mut dyn std::io::Write) -> Result<()> {
    let palette: [u32; 16] = { pgc.palette };
    write!(w, "      palette (YCrCb x16):")?;
    for &e in &palette {
        write!(w, " {:06x}", e & 0x00FF_FFFF)?;
    }
    writeln!(w)?;

    Ok(())
}

fn print_pgc_audio_control(
    pgc: &pgc_t,
    vtsi_mat: &vtsi_mat_t,
    w: &mut dyn std::io::Write,
) -> Result<()> {
    let ctrl: [u16; 8] = { pgc.audio_control };
    let attrs: [audio_attr_t; 8] = { vtsi_mat.vts_audio_attr };
    let nr = { vtsi_mat.nr_of_vts_audio_streams };

    let active: Vec<(usize, u16)> = ctrl
        .iter()
        .enumerate()
        .filter(|(_, &c)| decode::pgc_audio_control_available(c))
        .map(|(i, &c)| (i, c))
        .collect();

    writeln!(w, "      audio_control (active in PGC: {}):", active.len())?;
    if active.is_empty() {
        writeln!(w, "        (no audio streams enabled in this PGC)")?;
        return Ok(());
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
        writeln!(
            w,
            "        slot {slot} -> stream_num={stream_num}  ctrl=0x{ctrl_word:04x}  {}  {lang:<2}  {id_str}{warn}",
            decode::audio_format(fmt),
        )?;
    }

    Ok(())
}

fn print_pgc_subp_control(
    pgc: &pgc_t,
    vtsi_mat: &vtsi_mat_t,
    w: &mut dyn std::io::Write,
) -> Result<()> {
    let ctrl: [u32; 32] = { pgc.subp_control };
    let attrs: [subp_attr_t; 32] = { vtsi_mat.vts_subp_attr };
    let nr = { vtsi_mat.nr_of_vts_subp_streams };

    let active: Vec<(usize, u32)> = ctrl
        .iter()
        .enumerate()
        .filter(|(_, &c)| decode::pgc_subp_control_available(c))
        .map(|(i, &c)| (i, c))
        .collect();

    writeln!(w, "      subp_control (active in PGC: {}):", active.len())?;
    if active.is_empty() {
        writeln!(w, "        (no subpicture streams enabled in this PGC)")?;
        return Ok(());
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
        writeln!(
            w,
            "        slot {slot} -> ctrl=0x{ctrl_word:08x}  streams[4:3={s43} wide={sw} letterbox={sl} pan&scan={sp}]  {lang:<2}  PS 0x{main:02X}/sub 0x{sub:02X}{warn}"
        )?;
    }

    Ok(())
}

fn print_pgc_cells(pgc: &pgc_t, w: &mut dyn std::io::Write) -> Result<()> {
    let cell_playback_ptr = { pgc.cell_playback };
    let nr_of_cells = { pgc.nr_of_cells };
    if cell_playback_ptr.is_null() || nr_of_cells == 0 {
        return Ok(());
    }
    let cells: &[cell_playback_t] = unsafe {
        std::slice::from_raw_parts(cell_playback_ptr, usize::from(nr_of_cells))
    };
    let preview_len = cells.len().min(3);
    writeln!(w, "      cells (first {preview_len} of {}):", cells.len())?;
    for (i, cell) in cells.iter().take(preview_len).enumerate() {
        let first = { cell.first_sector };
        let last = { cell.last_sector };
        let sectors = last.saturating_sub(first);
        let ptime = { cell.playback_time };
        writeln!(
            w,
            "        [{i}] sectors {first}..{last} ({sectors:>7} blocks), playback_time={}",
            format_dvd_time(&ptime),
        )?;
    }
    if cells.len() > preview_len {
        writeln!(w, "        ... ({} more cells)", cells.len() - preview_len)?;
    }

    Ok(())
}
