//! `disc-remuxer info <path>` — print everything we can read about a disc.
//!
//! Output uses libdvdread / libdvdnav field names exactly. The intent is
//! that anyone reading the output can grep for a field name in libdvdread's
//! public headers (`<dvdread/ifo_types.h>` etc.) and find the matching
//! struct member.

use std::path::Path;

use anyhow::{anyhow, Context, Result};

use disc_core::{detect_disc_type, DiscType};
use disc_dvd::ifo::{
    cell_playback_t, format_dvd_time, pgc_t, title_info_t, IfoHandle, IfoKind,
};
use disc_dvd::DvdSource;

// The libdvdread structs are `#[repr(packed)]` (they mirror DVD on-disc
// layout — byte 1 = the first field, no padding). That means we cannot take
// references to individual fields, so copying through a temporary is
// required. The `{ value.field }` block syntax used in the format args below
// creates an rvalue copy, which works around the alignment constraint.

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

        // Open the VTS IFO so we can report PGC counts + a representative
        // PGC's playback time / cell count. libdvdread builds the VTS-IFO
        // cache on `ifoOpen`; subsequent reads are fast.
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

fn print_vmg_summary(vmg: &IfoHandle<'_>) {
    let Some(vmgi_mat) = vmg.vmgi_mat() else {
        println!("VMG IFO opened but vmgi_mat was NULL (parse failure?)");
        return;
    };

    // `vmg_identifier` is "DVDVIDEO-VMG" in valid discs.
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

fn print_vts_detail(vts_ifo: &IfoHandle<'_>, target_vts_ttn: u8) {
    let Some(vtsi_mat) = vts_ifo.vtsi_mat() else {
        println!("    (vtsi_mat NULL — VTS parse failure)");
        return;
    };

    println!("    vtsi_mat:");
    println!("      vts_category:             0x{:08x}", { vtsi_mat.vts_category });
    println!("      specification_version:    0x{:02x}", { vtsi_mat.specification_version });
    println!("      vts_last_sector:          {}", { vtsi_mat.vts_last_sector });
    println!("      vtsi_last_sector:         {}", { vtsi_mat.vtsi_last_sector });
    println!("      vtsi_last_byte:           {}", { vtsi_mat.vtsi_last_byte });
    println!("      nr_of_vtsm_audio_streams: {}", { vtsi_mat.nr_of_vtsm_audio_streams });
    println!("      nr_of_vtsm_subp_streams:  {}", { vtsi_mat.nr_of_vtsm_subp_streams });

    let pgcs = vts_ifo.pgcs();
    let pgcit_count: u16 = vts_ifo.vts_pgcit().map_or(0, |p| { p.nr_of_pgci_srp });
    println!("    vts_pgcit:");
    println!(
        "      nr_of_pgci_srp:   {pgcit_count}  ({} PGCs in this VTS)",
        pgcs.len()
    );

    // The PGC that plays this title sits at index `vts_ttn - 1` within the
    // VTS's pgci_srp array (libdvdread title-to-PGC mapping convention).
    if let Some(srp) = pgcs.get(usize::from(target_vts_ttn.saturating_sub(1))) {
        let pgc_ptr = { srp.pgc };
        if let Some(pgc) = unsafe { pgc_ptr.as_ref() } {
            print_pgc_summary(pgc);
        } else {
            println!("    (pgc pointer for vts_ttn={target_vts_ttn} was NULL)");
        }
    }
}

fn print_pgc_summary(pgc: &pgc_t) {
    println!("    pgc (this title's playback chain):");
    println!("      nr_of_programs:   {}  (chapter count from PGC side)", { pgc.nr_of_programs });
    println!("      nr_of_cells:      {}", { pgc.nr_of_cells });
    let playback_time = { pgc.playback_time };
    println!(
        "      playback_time:    {}  (HH:MM:SS.FF, BCD-encoded)",
        format_dvd_time(&playback_time)
    );
    println!("      still_time:       {}  (seconds; 0xff = infinite)", { pgc.still_time });

    // Cell array — walk a few cells so we can spot suspicious patterns
    // (zero-length cells, weird sector ranges — these are hooks for the
    // RipGuard/ARccOS detection logic we'll add later).
    let cell_playback_ptr = { pgc.cell_playback };
    let nr_of_cells = { pgc.nr_of_cells };
    if !cell_playback_ptr.is_null() && nr_of_cells > 0 {
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
}
