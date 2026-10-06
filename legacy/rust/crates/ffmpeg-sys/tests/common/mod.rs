//! Disc images built byte by byte for the tests: ISO 9660 / Joliet trees
//! (ECMA-119) and a UDF 1.02 volume (ECMA-167 / OSTA UDF) over the same
//! sectors, as on DVD-Video bridge discs.

#![allow(dead_code, reason = "each test file uses part of the builders")]
#![allow(clippy::many_single_char_names, reason = "one short name per descriptor being built")]

pub mod discrip;
pub mod dvd;
pub mod hddvd;

pub const S: usize = 2048;

// ---- little helpers ----

pub fn put16(b: &mut [u8], at: usize, v: u16) {
    b[at..at + 2].copy_from_slice(&v.to_le_bytes());
}
pub fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
pub fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}
/// ECMA-119 both-byte-order 32-bit field.
pub fn both32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
    b[at + 4..at + 8].copy_from_slice(&v.to_be_bytes());
}
pub fn ucs2(s: &str) -> Vec<u8> {
    s.encode_utf16().flat_map(u16::to_be_bytes).collect()
}

// ---- ISO 9660 / Joliet ----

/// A directory record: identifier bytes, flags, extent, size.
pub fn record(id: &[u8], flags: u8, extent: u32, size: u32) -> Vec<u8> {
    let len = (33 + id.len() + 1) & !1;
    let mut r = vec![0u8; len];
    r[0] = u8::try_from(len).unwrap();
    both32(&mut r, 2, extent);
    both32(&mut r, 10, size);
    r[25] = flags;
    r[32] = u8::try_from(id.len()).unwrap();
    r[33..33 + id.len()].copy_from_slice(id);
    r
}

/// A file in a built tree: name, first sector, size in bytes.
pub type F = (&'static str, u32, u32);
/// A directory of the root with its files.
pub type D = (&'static str, Vec<F>);

/// Sectors of the ISO 9660 tree (root, then one per directory) and of the
/// Joliet tree.
pub const ISO_DIRS: u32 = 24;
pub const JOLIET_DIRS: u32 = 26;
/// UDF partition: start sector and length.
pub const PART_START: u32 = 300;
pub const PART_LEN: u32 = 200;
/// VMG sectors used by the DVD trees (absolute sectors, inside the partition
/// so both file systems can point at them).
pub const VMG: u32 = PART_START + 30;
pub const BUP: u32 = PART_START + 32;
pub const VTS1: u32 = PART_START + 34;
/// A sector that holds no VMG.
pub const NO_VMG: u32 = PART_START + 40;

pub struct Img {
    pub d: Vec<u8>,
    pub bad: Vec<u32>,
}

impl Img {
    pub fn new() -> Self {
        Img { d: vec![0u8; 520 * S], bad: Vec::new() }
    }
    pub fn sector(&mut self, n: u32) -> &mut [u8] {
        let at = n as usize * S;
        &mut self.d[at..at + S]
    }
    pub fn put(&mut self, n: u32, bytes: &[u8]) {
        let at = n as usize * S;
        self.d[at..at + bytes.len()].copy_from_slice(bytes);
    }
    /// A VMG sector: identifier and number of title sets.
    pub fn vmg(&mut self, n: u32, id: &[u8; 12], title_sets: u16) {
        let v = self.sector(n);
        v[..12].copy_from_slice(id);
        v[0x3e..0x40].copy_from_slice(&title_sets.to_be_bytes());
    }
    pub fn volume_descriptor(&mut self, n: u32, kind: u8, label: &[u8], root: u32) {
        let v = self.sector(n);
        v[0] = kind;
        v[1..6].copy_from_slice(b"CD001");
        v[6] = 1;
        v[40..72].fill(if kind == 2 { 0 } else { b' ' });
        v[40..40 + label.len()].copy_from_slice(label);
        v[128..130].copy_from_slice(&2048u16.to_le_bytes());
        let r = record(&[0], 2, root, 2048);
        v[156..156 + r.len()].copy_from_slice(&r);
    }
    /// One tree: root at `base`, directory k at `base + 1 + k`.
    pub fn tree(&mut self, base: u32, dirs: &[D], name: impl Fn(&str) -> Vec<u8>) {
        let mut root = record(&[0], 2, base, 2048);
        root.extend(record(&[1], 2, base, 2048));
        for (k, (dir, files)) in dirs.iter().enumerate() {
            let at = base + 1 + u32::try_from(k).unwrap();
            root.extend(record(&name(dir), 2, at, 2048));
            let mut d = record(&[0], 2, at, 2048);
            d.extend(record(&[1], 2, base, 2048));
            for (file, sector, size) in files {
                d.extend(record(&name(&format!("{file};1")), 0, *sector, *size));
            }
            self.put(at, &d);
        }
        self.put(base, &root);
    }
}

/// The sector after the ISO 9660 descriptors (PVD at 16, SVD at 17 with
/// Joliet, then the terminator).
pub fn iso(im: &mut Img, label: &str, dirs: &[D], joliet: Option<(&str, &[D])>) -> u32 {
    im.volume_descriptor(16, 1, label.as_bytes(), ISO_DIRS);
    im.tree(ISO_DIRS, dirs, |s| s.as_bytes().to_vec());
    let mut n = 17;
    if let Some((jlabel, jdirs)) = joliet {
        im.volume_descriptor(17, 2, &ucs2(jlabel), JOLIET_DIRS);
        im.tree(JOLIET_DIRS, jdirs, ucs2);
        n = 18;
    }
    let t = im.sector(n);
    t[0] = 0xff;
    t[1..6].copy_from_slice(b"CD001");
    t[6] = 1;
    n + 1
}

// ---- UDF ----

pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for &b in data {
        crc ^= u16::from(b) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}

/// A descriptor tag: id, version 2, location, CRC over `crclen` bytes.
pub fn tag(b: &mut [u8], id: u16, loc: u32, crclen: u16) {
    put16(b, 0, id);
    put16(b, 2, 2);
    put32(b, 12, loc);
    put16(b, 10, crclen);
    let crc = crc16(&b[16..16 + usize::from(crclen)]);
    put16(b, 8, crc);
    b[4] = 0;
    b[4] = b[..16].iter().enumerate().filter(|&(i, _)| i != 4).fold(0u8, |s, (_, &x)| s.wrapping_add(x));
}

pub fn dstring(b: &mut [u8], at: usize, size: usize, text: &str) {
    b[at] = 8;
    b[at + 1..at + 1 + text.len()].copy_from_slice(text.as_bytes());
    b[at + size - 1] = u8::try_from(1 + text.len()).unwrap();
}

pub fn osta(b: &mut [u8], at: usize) {
    b[at] = 0;
    b[at + 1..at + 24].copy_from_slice(b"OSTA Compressed Unicode");
}

/// What the UDF volume of a built image records.
pub struct Udf {
    pub label: &'static str,
    pub revision: u16,
    pub year: u16,
    /// Files of `VIDEO_TS` (absolute sectors, inside the partition).
    pub files: Vec<F>,
    /// Files of `VIDEO_TS` recorded in several pieces: (name, [(absolute
    /// sector, bytes)]).
    pub pieces: Vec<(&'static str, Vec<(u32, u32)>)>,
}

/// A file entry at partition block `lbn` with short allocation descriptors
/// (bytes, partition block).
pub fn file_entry_ads(im: &mut Img, lbn: u32, file_type: u8, size: u64, ads: &[(u32, u32)]) {
    let mut b = vec![0u8; S];
    put16(&mut b, 0x14, 4);
    b[0x1b] = file_type;
    put16(&mut b, 0x30, 1); // link count
    put64(&mut b, 0x38, size);
    for (i, &(len, at)) in ads.iter().enumerate() {
        put32(&mut b, 0xb0 + 8 * i, len);
        put32(&mut b, 0xb4 + 8 * i, at);
    }
    put32(&mut b, 0xac, u32::try_from(8 * ads.len()).unwrap());
    tag(&mut b, 0x105, lbn, u16::try_from(176 + 8 * ads.len() - 16).unwrap());
    im.put(PART_START + lbn, &b);
}

/// A file entry at partition block `lbn` with one short allocation
/// descriptor.
pub fn file_entry(im: &mut Img, lbn: u32, file_type: u8, size: u64, data_lbn: u32) {
    file_entry_ads(im, lbn, file_type, size, &[(u32::try_from(size).unwrap(), data_lbn)]);
}

/// Directory data at partition block `lbn`: the parent entry, then
/// (name, characteristics, file entry block); returns its length.
pub fn directory(im: &mut Img, lbn: u32, parent: u32, entries: &[(&str, u8, u32)]) -> u64 {
    let mut out = Vec::new();
    let mut all = vec![(String::new(), 0x0a_u8, parent)];
    all.extend(entries.iter().map(|&(n, c, l)| (n.to_owned(), c, l)));
    for (name, chars, icb) in all {
        let raw: Vec<u8> = if name.is_empty() { Vec::new() } else { std::iter::once(8).chain(name.bytes()).collect() };
        let len = (38 + raw.len() + 3) & !3;
        let mut b = vec![0u8; len];
        put16(&mut b, 0x10, 1);
        b[0x12] = chars;
        b[0x13] = u8::try_from(raw.len()).unwrap();
        put32(&mut b, 0x14, 2048);
        put32(&mut b, 0x18, icb);
        b[0x26..0x26 + raw.len()].copy_from_slice(&raw);
        tag(&mut b, 0x101, lbn, u16::try_from(len - 16).unwrap());
        out.extend_from_slice(&b);
    }
    im.put(PART_START + lbn, &out);
    out.len() as u64
}

/// Volume recognition sequence from sector `vrs`, anchor at 256, main volume
/// descriptor sequence at 32, reserve at 48, integrity at 64; file set at
/// block 0, root (`VIDEO_TS` only) at 1-2, `VIDEO_TS` at 3 and 5, file
/// entries from block 20.
pub fn udf(im: &mut Img, vrs: u32, u: &Udf) {
    for (i, id) in [b"BEA01", b"NSR02", b"TEA01"].iter().enumerate() {
        let v = im.sector(vrs + u32::try_from(i).unwrap());
        v[1..6].copy_from_slice(*id);
        v[6] = 1;
    }
    let a = im.sector(256);
    put32(a, 0x10, 16 * 2048);
    put32(a, 0x14, 32);
    put32(a, 0x18, 16 * 2048);
    put32(a, 0x1c, 48);
    tag(a, 2, 256, 496);
    let p = im.sector(32);
    put16(p, 0x178 + 2, u.year);
    tag(p, 1, 32, 496);
    let iu = im.sector(33);
    iu[0x15..0x21].copy_from_slice(b"*UDF LV Info");
    put16(iu, 0x2c, u.revision);
    tag(iu, 4, 33, 496);
    let pd = im.sector(34);
    pd[0x14] = 1;
    pd[0x19..0x1f].copy_from_slice(b"+NSR02");
    put32(pd, 0xb8, 1); // read-only access
    put32(pd, 0xbc, PART_START);
    put32(pd, 0xc0, PART_LEN);
    tag(pd, 5, 34, 496);
    let l = im.sector(35);
    put32(l, 0x10, 1);
    osta(l, 0x14);
    dstring(l, 0x54, 128, u.label);
    put32(l, 0xd4, 2048);
    l[0xd9..0xd9 + 19].copy_from_slice(b"*OSTA UDF Compliant");
    put32(l, 0xf8, 2048);
    put32(l, 0x108, 6);
    put32(l, 0x10c, 1);
    put32(l, 0x1b0, 2048);
    put32(l, 0x1b4, 64);
    l[0x1b8] = 1;
    l[0x1b9] = 6;
    tag(l, 6, 35, 446 - 16);
    tag(im.sector(36), 8, 36, 0);
    for s in 32..37 {
        let mut d = im.sector(s).to_vec();
        let id = u16::from_le_bytes([d[0], d[1]]);
        let crclen = u16::from_le_bytes([d[10], d[11]]);
        tag(&mut d, id, s + 16, crclen);
        im.sector(s + 16).copy_from_slice(&d);
    }
    let v = im.sector(64);
    put32(v, 0x1c, 1);
    put32(v, 0x48, 1);
    put32(v, 0x4c, 46);
    put16(v, 80 + 8 + 40, 0x0102);
    tag(v, 9, 64, 80 + 8 + 46 - 16);
    tag(im.sector(65), 8, 65, 0);

    let mut f = vec![0u8; S];
    put32(&mut f, 0x190, 2048);
    put32(&mut f, 0x194, 1);
    f[0x1a1..0x1a1 + 19].copy_from_slice(b"*OSTA UDF Compliant");
    tag(&mut f, 0x100, 0, 496);
    im.put(PART_START, &f);
    let root = directory(im, 2, 1, &[("VIDEO_TS", 0x02, 3)]);
    file_entry(im, 1, 4, root, 2);
    let names = u.files.iter().map(|f| f.0).chain(u.pieces.iter().map(|p| p.0));
    let entries: Vec<(&str, u8, u32)> = names.enumerate().map(|(k, n)| (n, 0, 20 + u32::try_from(k).unwrap())).collect();
    let vt = directory(im, 5, 1, &entries);
    file_entry(im, 3, 4, vt, 5);
    for (k, &(_, sector, size)) in u.files.iter().enumerate() {
        file_entry(im, 20 + u32::try_from(k).unwrap(), 5, u64::from(size), sector - PART_START);
    }
    for (k, (_, pieces)) in u.pieces.iter().enumerate() {
        let lbn = 20 + u32::try_from(u.files.len() + k).unwrap();
        let size = pieces.iter().map(|p| u64::from(p.1)).sum();
        let ads: Vec<(u32, u32)> = pieces.iter().map(|&(sector, bytes)| (bytes, sector - PART_START)).collect();
        file_entry_ads(im, lbn, 5, size, &ads);
    }
}

// ---- the DVD-Video trees ----

/// `VIDEO_TS` of a valid DVD-Video volume.
pub fn dvd_files() -> Vec<F> {
    vec![("VIDEO_TS.IFO", VMG, 4096), ("VIDEO_TS.BUP", BUP, 4096), ("VTS_01_0.IFO", VTS1, 4096)]
}

/// `VIDEO_TS` whose IFO points at a sector without a VMG.
pub fn broken_files() -> Vec<F> {
    vec![("VIDEO_TS.IFO", NO_VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096)]
}

/// A bridge image: the UDF volume `u` (if any), an ISO 9660 tree with
/// `iso_files` labelled `ISO_LABEL`, and a Joliet tree with `joliet_files`
/// labelled `JOLIET_LABEL` (if given). The VMG sectors are valid.
pub fn bridge(u: Option<&Udf>, iso_files: Vec<F>, joliet_files: Option<Vec<F>>) -> Img {
    let mut im = Img::new();
    let dirs = vec![("VIDEO_TS", iso_files)];
    let jdirs = joliet_files.map(|f| vec![("VIDEO_TS", f)]);
    let next = iso(&mut im, "ISO_LABEL", &dirs, jdirs.as_deref().map(|d| ("JOLIET_LABEL", d)));
    if let Some(u) = u {
        udf(&mut im, next, u);
    }
    im.vmg(VMG, b"DVDVIDEO-VMG", 1);
    im.vmg(BUP, b"DVDVIDEO-VMG", 1);
    im
}

pub fn udf102(year: u16, files: Vec<F>) -> Udf {
    Udf { label: "UDF_DISC_LABEL", revision: 0x0102, year, files, pieces: Vec::new() }
}

