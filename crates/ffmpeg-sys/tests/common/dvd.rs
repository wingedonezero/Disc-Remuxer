//! DVD-Video discs built byte by byte for the tests (DVD-Video Book layout,
//! field names as libdvdread's `ifo_types.h` / `nav_types.h`): a VMG with a
//! title search table, title sets with program chains, cells, chapters and a
//! VOBU address map, and title VOBs whose blocks hold NAV packs. Written as a
//! disc folder.

#![allow(clippy::struct_field_names, reason = "fields keep libdvdread's names")]

use std::path::Path;

use super::S;

fn be16(d: &mut [u8], at: usize, v: u16) {
    d[at..at + 2].copy_from_slice(&v.to_be_bytes());
}
fn be32(d: &mut [u8], at: usize, v: u32) {
    d[at..at + 4].copy_from_slice(&v.to_be_bytes());
}
fn sectors_for(bytes: usize) -> usize {
    bytes.div_ceil(S).max(1)
}
fn small(v: usize) -> u32 {
    u32::try_from(v).unwrap()
}

/// `dvd_time_t` for `secs` seconds at 25 frames per second (BCD, frame byte
/// 0x40 | frames).
#[must_use]
pub fn time(secs: u32) -> [u8; 4] {
    let bcd = |v: u32| u8::try_from((v / 10) << 4 | (v % 10)).unwrap();
    [bcd(secs / 3600), bcd(secs / 60 % 60), bcd(secs % 60), 0x40]
}

/// `cell_playback_t` byte 0.
pub mod flags {
    pub const SEAMLESS_ANGLE: u8 = 0x01;
    pub const STC_DISCONTINUITY: u8 = 0x02;
    pub const INTERLEAVED: u8 = 0x04;
    pub const SEAMLESS_PLAY: u8 = 0x08;
    /// block type 1: angle block
    pub const ANGLE_BLOCK: u8 = 0x10;
    /// block mode 1 / 2 / 3: first / middle / last cell of a block
    pub const FIRST_IN_BLOCK: u8 = 0x40;
    pub const IN_BLOCK: u8 = 0x80;
    pub const LAST_IN_BLOCK: u8 = 0xc0;
}

/// One cell (`cell_playback_t`): title-VOB sectors and the rest.
#[derive(Debug, Clone, Copy)]
pub struct Cell {
    pub flags: u8,
    pub still_time: u8,
    pub cell_cmd_nr: u8,
    pub playback_time: [u8; 4],
    pub first_sector: u32,
    pub first_ilvu_end_sector: u32,
    pub last_vobu_start_sector: u32,
    pub last_sector: u32,
}

impl Cell {
    #[must_use]
    pub fn new(first: u32, last_vobu: u32, last: u32, secs: u32) -> Self {
        Cell {
            flags: 0,
            still_time: 0,
            cell_cmd_nr: 0,
            playback_time: time(secs),
            first_sector: first,
            first_ilvu_end_sector: 0,
            last_vobu_start_sector: last_vobu,
            last_sector: last,
        }
    }
    #[must_use]
    pub fn flags(mut self, f: u8) -> Self {
        self.flags = f;
        self
    }
}

/// One program chain.
#[derive(Debug, Clone, Default)]
pub struct Pgc {
    pub cells: Vec<Cell>,
    /// The first cell of each program (1-based); empty = one program at cell 1.
    pub programs: Vec<u8>,
    /// Empty = the sum of the cells' times.
    pub playback_time: Option<[u8; 4]>,
    pub prohibited_ops: u32,
    pub pg_playback_mode: u8,
    pub pre: Vec<[u8; 8]>,
    pub post: Vec<[u8; 8]>,
    pub cell_cmds: Vec<[u8; 8]>,
    /// The search pointer's entry id (`0x80 | title` for a title's entry PGC).
    pub entry_id: u8,
}

impl Pgc {
    #[must_use]
    pub fn new(cells: Vec<Cell>) -> Self {
        Pgc { cells, ..Pgc::default() }
    }
}

/// A title set: its program chains, the parts of title of each of its titles
/// as (pgcn, pgn), the VOBU address map and the size of its title VOBs.
#[derive(Debug, Clone, Default)]
pub struct Vts {
    pub pgcs: Vec<Pgc>,
    pub ptts: Vec<Vec<(u16, u16)>>,
    pub vobus: Vec<u32>,
    pub vob_sectors: u32,
}

/// `VTS_nn_0.IFO`: `VTSI_MAT` in sector 0, then `VTS_PTT_SRPT`, `VTS_PGCIT`,
/// `VTS_C_ADT` and `VTS_VOBU_ADMAP` each from a sector of its own.
#[must_use]
pub fn vts_ifo(v: &Vts) -> Vec<u8> {
    // VTS_PTT_SRPT
    let n_titles = v.ptts.len();
    let entries: usize = v.ptts.iter().map(Vec::len).sum();
    let mut ptt = vec![0u8; 8 + 4 * n_titles + 4 * entries];
    let mut at = 8 + 4 * n_titles;
    be16(&mut ptt, 0, u16::try_from(n_titles).unwrap());
    for (t, title) in v.ptts.iter().enumerate() {
        be32(&mut ptt, 8 + 4 * t, small(at));
        for &(pgcn, pgn) in title {
            be16(&mut ptt, at, pgcn);
            be16(&mut ptt, at + 2, pgn);
            at += 4;
        }
    }
    let last = small(ptt.len() - 1);
    be32(&mut ptt, 4, last);

    // VTS_PGCIT
    let n = v.pgcs.len();
    let mut pgcit = vec![0u8; 8 + 8 * n];
    be16(&mut pgcit, 0, u16::try_from(n).unwrap());
    for (k, p) in v.pgcs.iter().enumerate() {
        let start = pgcit.len();
        pgcit[8 + 8 * k] = p.entry_id;
        be32(&mut pgcit, 8 + 8 * k + 4, small(start));
        pgcit.extend(pgc_bytes(p));
    }
    let last = small(pgcit.len() - 1);
    be32(&mut pgcit, 4, last);

    // VTS_C_ADT: one entry per cell of the first PGC that names it (VOB id 1)
    let mut cells: Vec<(u8, u32, u32)> = Vec::new();
    for p in &v.pgcs {
        for (k, c) in p.cells.iter().enumerate() {
            let id = u8::try_from(k + 1).unwrap();
            if !cells.iter().any(|&(i, f, _)| i == id && f == c.first_sector) {
                cells.push((id, c.first_sector, c.last_sector));
            }
        }
    }
    let mut cadt = vec![0u8; 8 + 12 * cells.len()];
    be16(&mut cadt, 0, 1);
    let last = small(cadt.len() - 1);
    be32(&mut cadt, 4, last);
    for (k, &(id, first, last)) in cells.iter().enumerate() {
        let e = 8 + 12 * k;
        be16(&mut cadt, e, 1);
        cadt[e + 2] = id;
        be32(&mut cadt, e + 4, first);
        be32(&mut cadt, e + 8, last);
    }

    // VTS_VOBU_ADMAP
    let mut map = vec![0u8; 4 + 4 * v.vobus.len()];
    let last = small(map.len() - 1);
    be32(&mut map, 0, last);
    for (k, s) in v.vobus.iter().enumerate() {
        be32(&mut map, 4 + 4 * k, *s);
    }

    let tables = [ptt, pgcit, cadt, map];
    let mut d = vec![0u8; S];
    let mut places = Vec::new();
    for t in &tables {
        places.push(small(d.len() / S));
        let mut t = t.clone();
        t.resize(sectors_for(t.len()) * S, 0);
        d.extend(t);
    }
    let ifo_sectors = small(d.len() / S);
    d[..12].copy_from_slice(b"DVDVIDEO-VTS");
    be32(&mut d, 0x0c, 2 * ifo_sectors + v.vob_sectors - 1); // vts_last_sector: IFO, title VOBs, BUP
    be32(&mut d, 0x1c, ifo_sectors - 1); // vtsi_last_sector
    be16(&mut d, 0x20, 0x0010); // specification version
    be32(&mut d, 0x80, 0x3ff); // vtsi_last_byte
    be32(&mut d, 0xc4, ifo_sectors); // vtstt_vobs
    be32(&mut d, 0xc8, places[0]); // vts_ptt_srpt
    be32(&mut d, 0xcc, places[1]); // vts_pgcit
    be32(&mut d, 0xe0, places[2]); // vts_c_adt
    be32(&mut d, 0xe4, places[3]); // vts_vobu_admap
    d
}

/// `pgc_t` with its command table, program map, cell playback and cell
/// position tables.
fn pgc_bytes(p: &Pgc) -> Vec<u8> {
    let n = p.cells.len();
    let mut d = vec![0u8; 0xec];
    d[2] = if n == 0 {
        0
    } else if p.programs.is_empty() {
        1
    } else {
        u8::try_from(p.programs.len()).unwrap()
    };
    d[3] = u8::try_from(n).unwrap();
    let total: u32 = p.cells.iter().map(|c| bcd_secs(c.playback_time)).sum();
    d[4..8].copy_from_slice(&p.playback_time.unwrap_or_else(|| time(total)));
    be32(&mut d, 8, p.prohibited_ops);
    d[0xa2] = p.pg_playback_mode;
    if !(p.pre.is_empty() && p.post.is_empty() && p.cell_cmds.is_empty()) {
        let at = d.len();
        be16(&mut d, 0xe4, u16::try_from(at).unwrap());
        let cmds: Vec<[u8; 8]> = p.pre.iter().chain(&p.post).chain(&p.cell_cmds).copied().collect();
        let mut t = vec![0u8; 8];
        be16(&mut t, 0, u16::try_from(p.pre.len()).unwrap());
        be16(&mut t, 2, u16::try_from(p.post.len()).unwrap());
        be16(&mut t, 4, u16::try_from(p.cell_cmds.len()).unwrap());
        be16(&mut t, 6, u16::try_from(8 + 8 * cmds.len() - 1).unwrap());
        for c in cmds {
            t.extend(c);
        }
        d.extend(t);
    }
    if n > 0 {
        let at = d.len();
        be16(&mut d, 0xe6, u16::try_from(at).unwrap());
        if p.programs.is_empty() {
            d.push(1);
        } else {
            d.extend(&p.programs);
        }
        if d.len() % 2 == 1 {
            d.push(0);
        }
        let at = d.len();
        be16(&mut d, 0xe8, u16::try_from(at).unwrap());
        for c in &p.cells {
            let mut b = [0u8; 24];
            b[0] = c.flags;
            b[2] = c.still_time;
            b[3] = c.cell_cmd_nr;
            b[4..8].copy_from_slice(&c.playback_time);
            be32(&mut b, 8, c.first_sector);
            be32(&mut b, 12, c.first_ilvu_end_sector);
            be32(&mut b, 16, c.last_vobu_start_sector);
            be32(&mut b, 20, c.last_sector);
            d.extend(b);
        }
        let at = d.len();
        be16(&mut d, 0xea, u16::try_from(at).unwrap());
        for k in 0..n {
            d.extend([0, 1, 0, u8::try_from(k + 1).unwrap()]);
        }
    }
    d
}

/// Seconds of a `dvd_time_t` (frames dropped).
#[must_use]
pub fn bcd_secs(t: [u8; 4]) -> u32 {
    let v = |b: u8| u32::from(b >> 4) * 10 + u32::from(b & 0xf);
    v(t[0]) * 3600 + v(t[1]) * 60 + v(t[2])
}

/// One entry of the title search table.
#[derive(Debug, Clone, Copy)]
pub struct Title {
    pub title_set_nr: u8,
    pub vts_ttn: u8,
    pub nr_of_ptts: u16,
    pub nr_of_angles: u8,
    pub pb_ty: u8,
    pub title_set_starting_sector: u32,
}

impl Title {
    #[must_use]
    pub fn new(vts: u8, ttn: u8, ptts: u16) -> Self {
        Title { title_set_nr: vts, vts_ttn: ttn, nr_of_ptts: ptts, nr_of_angles: 1, pb_ty: 0, title_set_starting_sector: 0 }
    }
}

/// `VIDEO_TS.IFO`: `VMGI_MAT` in sector 0, `TT_SRPT` and `VTS_ATRT` after it;
/// no first-play PGC, no menus.
#[must_use]
pub fn vmg_ifo(titles: &[Title], nr_vts: u16) -> Vec<u8> {
    let mut tt = vec![0u8; 8 + 12 * titles.len()];
    be16(&mut tt, 0, u16::try_from(titles.len()).unwrap());
    let last = small(tt.len() - 1);
    be32(&mut tt, 4, last);
    for (k, t) in titles.iter().enumerate() {
        let e = 8 + 12 * k;
        tt[e] = t.pb_ty;
        tt[e + 1] = t.nr_of_angles;
        be16(&mut tt, e + 2, t.nr_of_ptts);
        tt[e + 6] = t.title_set_nr;
        tt[e + 7] = t.vts_ttn;
        be32(&mut tt, e + 8, t.title_set_starting_sector);
    }
    let n = usize::from(nr_vts);
    let mut atrt = vec![0u8; 8 + 4 * n + 542 * n];
    be16(&mut atrt, 0, nr_vts);
    let last = small(atrt.len() - 1);
    be32(&mut atrt, 4, last);
    for k in 0..n {
        let off = 8 + 4 * n + 542 * k;
        be32(&mut atrt, 8 + 4 * k, small(off));
        be32(&mut atrt, off, 541); // last_byte of the entry
    }
    let mut d = vec![0u8; S];
    let tt_at = small(d.len() / S);
    tt.resize(sectors_for(tt.len()) * S, 0);
    d.extend(tt);
    let atrt_at = small(d.len() / S);
    atrt.resize(sectors_for(atrt.len()) * S, 0);
    d.extend(atrt);
    let ifo_sectors = small(d.len() / S);
    d[..12].copy_from_slice(b"DVDVIDEO-VMG");
    be32(&mut d, 0x0c, 2 * ifo_sectors - 1); // vmg_last_sector: IFO + BUP
    be32(&mut d, 0x1c, ifo_sectors - 1); // vmgi_last_sector
    be16(&mut d, 0x20, 0x0010);
    be16(&mut d, 0x26, 1); // vmg_nr_of_volumes
    be16(&mut d, 0x28, 1); // vmg_this_volume_nr
    d[0x2a] = 1; // disc_side
    be16(&mut d, 0x3e, nr_vts);
    be32(&mut d, 0x80, 0x3ff); // vmgi_last_byte
    be32(&mut d, 0xc4, tt_at); // tt_srpt
    be32(&mut d, 0xd0, atrt_at); // vts_atrt
    d
}

/// A NAV pack (pack header, system header, PCI and DSI packets).
#[derive(Debug, Clone, Copy, Default)]
pub struct Nav {
    /// `nv_pck_lbn` of the PCI and the DSI.
    pub lbn: u32,
    pub vobu_s_ptm: u32,
    pub vobu_e_ptm: u32,
    /// `dsi_gi.vobu_ea`
    pub vobu_ea: u32,
    /// `sml_pbi.category`
    pub category: u16,
    /// `sml_pbi.ilvu_ea`
    pub ilvu_ea: u32,
    /// `vobu_sri.next_vobu` (0x3fffffff = end of cell; bit 31 = a video VOBU)
    pub next_vobu: u32,
}

/// `vobu_sri.next_vobu` value for "the last VOBU of its cell".
pub const END_OF_CELL: u32 = 0x3fff_ffff;

impl Nav {
    #[must_use]
    pub fn block(&self) -> Vec<u8> {
        let mut s = vec![0u8; S];
        s[..5].copy_from_slice(&[0, 0, 1, 0xba, 0x44]);
        s[0x0e..0x14].copy_from_slice(&[0, 0, 1, 0xbb, 0, 0x12]);
        s[0x26..0x2d].copy_from_slice(&[0, 0, 1, 0xbf, 0x03, 0xd4, 0]);
        s[0x400..0x407].copy_from_slice(&[0, 0, 1, 0xbf, 0x03, 0xfa, 1]);
        be32(&mut s, 0x2d, self.lbn);
        be32(&mut s, 0x39, self.vobu_s_ptm);
        be32(&mut s, 0x3d, self.vobu_e_ptm);
        be32(&mut s, 0x40b, self.lbn);
        be32(&mut s, 0x40f, self.vobu_ea);
        be16(&mut s, 0x427, self.category);
        be32(&mut s, 0x429, self.ilvu_ea);
        be32(&mut s, 0x541, self.next_vobu);
        s
    }
}

/// Title VOBs of `n` blocks: zeros, with `navs` at their blocks.
#[must_use]
pub fn title_vobs(n: u32, navs: &[Nav]) -> Vec<u8> {
    let mut d = vec![0u8; n as usize * S];
    for nav in navs {
        let at = nav.lbn as usize * S;
        d[at..at + S].copy_from_slice(&nav.block());
    }
    d
}

/// Writes a disc folder: `VIDEO_TS/VIDEO_TS.IFO` (+ `.BUP`) and per title set
/// `VTS_nn_0.IFO` (+ `.BUP`) and `VTS_nn_1.VOB`.
pub fn write_disc(dir: &Path, vmg: &[u8], title_sets: &[(Vec<u8>, Vec<u8>)]) {
    let v = dir.join("VIDEO_TS");
    let _ = std::fs::remove_dir_all(dir);
    std::fs::create_dir_all(&v).unwrap();
    std::fs::write(v.join("VIDEO_TS.IFO"), vmg).unwrap();
    std::fs::write(v.join("VIDEO_TS.BUP"), vmg).unwrap();
    for (k, (ifo, vob)) in title_sets.iter().enumerate() {
        std::fs::write(v.join(format!("VTS_{:02}_0.IFO", k + 1)), ifo).unwrap();
        std::fs::write(v.join(format!("VTS_{:02}_0.BUP", k + 1)), ifo).unwrap();
        std::fs::write(v.join(format!("VTS_{:02}_1.VOB", k + 1)), vob).unwrap();
    }
}

/// A built disc opened as the DVD-Video demuxer opens it (`DVDVideoDisc`).
pub struct OpenDisc {
    pub src: *mut ffmpeg_sys::dvdvideo::Source,
    pub disc: *mut ffmpeg_sys::dvdvideo::Disc,
}

impl OpenDisc {
    /// Opens the disc folder `dir`.
    #[must_use]
    pub fn open(dir: &Path) -> Self {
        use ffmpeg_sys::dvdvideo::{ff_dvdvideo_disc_open, ff_dvdvideo_source_open};
        let c = std::ffi::CString::new(dir.to_str().unwrap()).unwrap();
        let (mut src, mut disc) = (std::ptr::null_mut(), std::ptr::null_mut());
        let opts = ffmpeg_sys::discio::ImageOptions::default();
        // SAFETY: valid path, options and out pointers.
        unsafe {
            assert!(ff_dvdvideo_source_open(std::ptr::null_mut(), c.as_ptr(), &raw const opts, 1, &raw mut src) >= 0);
            assert!(ff_dvdvideo_disc_open(std::ptr::null_mut(), src, &raw mut disc) >= 0, "the built disc opens");
        }
        OpenDisc { src, disc }
    }

    /// Writes a disc folder for test `test` (VMG with `titles`, the title
    /// sets with their NAV packs) and opens it.
    #[must_use]
    pub fn build(test: &str, titles: &[Title], title_sets: &[(Vts, Vec<Nav>)]) -> Self {
        let dir = std::env::temp_dir().join(format!("dvdvideo-{test}-{}", std::process::id()));
        let vmg = vmg_ifo(titles, u16::try_from(title_sets.len()).unwrap());
        let files: Vec<(Vec<u8>, Vec<u8>)> =
            title_sets.iter().map(|(v, navs)| (vts_ifo(v), title_vobs(v.vob_sectors, navs))).collect();
        write_disc(&dir, &vmg, &files);
        Self::open(&dir)
    }
}

impl Drop for OpenDisc {
    fn drop(&mut self) {
        // SAFETY: opened in `open`, closed once, the disc first.
        unsafe {
            ffmpeg_sys::dvdvideo::ff_dvdvideo_disc_close(&raw mut self.disc);
            ffmpeg_sys::dvdvideo::ff_dvdvideo_source_close(&raw mut self.src);
        }
    }
}

/// A NAV pack at `lbn` of a VOBU of `len` blocks followed by the next one
/// (`next` = None: the last VOBU of its cell), times in 90 kHz ticks.
#[must_use]
pub fn nav(lbn: u32, len: u32, next: Option<u32>) -> Nav {
    Nav { lbn, vobu_ea: len - 1, next_vobu: next.map_or(END_OF_CELL, |n| n - lbn), ..Nav::default() }
}

/// NAV packs for the VOBUs `starts` (sorted) of a cell ending before block
/// `end`, `ticks` apart (0.5 s each by default), times from `t0`.
#[must_use]
pub fn cell_navs(starts: &[u32], end: u32, t0: u32) -> Vec<Nav> {
    starts
        .iter()
        .enumerate()
        .map(|(k, &s)| {
            let next = starts.get(k + 1).copied();
            let len = next.unwrap_or(end) - s;
            let t = t0 + 45_000 * u32::try_from(k).unwrap();
            Nav { vobu_s_ptm: t, vobu_e_ptm: t + 45_000, ..nav(s, len, next) }
        })
        .collect()
}
