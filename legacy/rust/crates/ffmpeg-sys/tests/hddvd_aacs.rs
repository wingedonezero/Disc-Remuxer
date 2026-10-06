//! HD DVD AACS (`libavformat/hddvd_aacs.c` on our libaacs): keys from the
//! key files, title keys, navigation packs, decrypted packs, on built discs.
//! With `HDDVD_CORPUS` (a folder of HD DVD images) and `HDDVD_KEYDB` (key
//! files, comma-separated): the encrypted 300 images are opened, the
//! feature's first 4000 blocks decrypted (every E-AC-3 pack must hold a sync
//! word), and the same blocks decrypted again with a volume unique key
//! computed from a device key and the volume ID only.

mod common;

use std::os::raw::c_int;
use std::path::PathBuf;

use common::hddvd::{
    aes_g, data_pack, encrypt_pack, evob, hex, image, nav_pack, open, open_file, playlist, sha1, tkf, title_keys,
    tmap, vti, File,
};
use common::Img;

const INVALIDDATA: c_int = -0x4144_4E49;
const EACCES: c_int = -13;

const VUK: [u8; 16] = [7, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0x42];
const SEED: [u8; 12] = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc];

/// The EVOB's blocks, as plain packs and as the disc holds them.
struct Blocks {
    plain: Vec<Vec<u8>>,
    disc: Vec<Vec<u8>>,
}

/// nav pack (title key id 5 = slot 4), scrambled audio, clear video, private
/// stream 2 with scrambling bits, scrambled video.
fn blocks(key: &[u8; 16], mode_bits: u8) -> Blocks {
    let plain = vec![
        nav_pack(mode_bits, 5, &SEED),
        data_pack(0xbd, 1, 3),
        data_pack(0xe0, 0, 9),
        data_pack(0xbf, 1, 4),
        data_pack(0xe0, 2, 7),
    ];
    let disc = plain
        .iter()
        .map(|p| if p[17] != 0xbb && p[17] != 0xbf && p[20] & 0x30 != 0 { encrypt_pack(p, key, &SEED) } else { p.clone() })
        .collect();
    Blocks { plain, disc }
}

fn disc_with(blocks: &[Vec<u8>], aacs: Vec<File>, playlists: usize) -> Img {
    let xpl = playlist("", r#"<Title><PrimaryAudioVideoClip src="file:///dvddisc/HVDVD_TS/A.MAP"/></Title>"#);
    let mut files: Vec<File> = vec![("ADV_OBJ", "VPLST000.XPL", xpl.clone())];
    if playlists > 1 {
        files.push(("ADV_OBJ", "VPLST001.XPL", xpl));
    }
    files.push(("HVDVD_TS", "HVA00001.VTI", vti(&[evob("A.EVO", 1, 10)])));
    files.push(("HVDVD_TS", "A.MAP", tmap(&[u16::try_from(blocks.len()).unwrap()], 0, 0, false)));
    files.push(("HVDVD_TS", "A.EVO", blocks.concat()));
    files.extend(aacs);
    image(&files)
}

fn key_file(name: &str, text: &str) -> String {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("hddvd_aacs");
    std::fs::create_dir_all(&dir).unwrap();
    let p = dir.join(name);
    std::fs::write(&p, text).unwrap();
    p.to_str().unwrap().to_owned()
}

fn cleared(p: &[u8]) -> Vec<u8> {
    let mut c = p.to_vec();
    c[20] &= 0xcf;
    c
}

fn standard() -> (Blocks, Vec<File>, String) {
    let keys = title_keys();
    let b = blocks(&keys[4], 2);
    let t = tkf(&VUK, &keys);
    let id = hex(&sha1(&t));
    (b, vec![("AACS", "VTKF000.AACS", t)], id)
}

#[test]
fn a_scrambled_pack_is_decrypted_with_the_title_key_its_navigation_pack_names() {
    let (b, aacs, id) = standard();
    // the HD DVD key file dialect: no "0x"
    let kf = key_file("bare.cfg", &format!("{id} = Test disc | V | {}\n", hex(&VUK)));
    let d = open(&disc_with(&b.disc, aacs, 1), &[kf]).unwrap();
    for (i, p) in b.plain.iter().enumerate() {
        let (r, got) = d.block(1, u32::try_from(i).unwrap());
        assert_eq!(r, 1, "block {i}");
        let want = if p[17] != 0xbb && p[17] != 0xbf && p[20] & 0x30 != 0 { cleared(p) } else { p.clone() };
        assert!(got == want, "block {i}");
    }
}

#[test]
fn the_vuk_comes_from_the_media_key_and_volume_id_too() {
    let keys = title_keys();
    let (mk, vid) = ([0x31u8; 16], [0x40u8, 0, 0x15, 7, 3, 6, 0x19, 0x13, b'W', b'G', b'H', b'D', b'V', b'M', 0, 0]);
    let vuk = aes_g(&mk, &vid);
    let b = blocks(&keys[4], 2);
    let t = tkf(&vuk, &keys);
    let kf = key_file(
        "mk_vid.cfg",
        &format!("; comment\n| PK | 0x{}\n0x{} = Test | D | 2007-01-01 | M | 0x{} | I | 0x{} ; note\n", hex(&[9; 16]), hex(&sha1(&t)), hex(&mk), hex(&vid)),
    );
    let d = open(&disc_with(&b.disc, vec![("AACS", "VTKF000.AACS", t)], 1), &[kf]).unwrap();
    assert!(d.block(1, 1).1 == cleared(&b.plain[1]));
}

#[test]
fn the_first_key_file_with_a_vuk_wins() {
    let (b, aacs, id) = standard();
    let right = key_file("right.cfg", &format!("{id} = Test | V | {}\n", hex(&VUK)));
    let wrong = key_file("wrong.cfg", &format!("{id} = Test | V | {}\n", hex(&[0xee; 16])));
    let d = open(&disc_with(&b.disc, aacs.clone(), 1), &[right.clone(), wrong.clone()]).unwrap();
    assert!(d.block(1, 1).1 == cleared(&b.plain[1]));
    let d = open(&disc_with(&b.disc, aacs, 1), &[wrong, right]).unwrap();
    assert!(d.block(1, 1).1 != cleared(&b.plain[1]), "the wrong key decrypts to something else");
}

#[test]
fn without_a_vuk_or_a_volume_id_the_disc_cannot_be_opened() {
    let (b, aacs, id) = standard();
    let none = key_file("none.cfg", "0123456789012345678901234567890123456789 = Other | V | 00112233445566778899AABBCCDDEEFF\n");
    assert_eq!(open(&disc_with(&b.disc, aacs.clone(), 1), &[none]).err(), Some(EACCES));
    // a volume ID but no media key and no key that gives one (no MKBROM.AACS either)
    let vid_only = key_file("vid_only.cfg", &format!("{id} = Test | I | {}\n", hex(&[1; 16])));
    assert_eq!(open(&disc_with(&b.disc, aacs.clone(), 1), &[vid_only]).err(), Some(EACCES));
    assert_eq!(open(&disc_with(&b.disc, aacs, 1), &[]).err(), Some(EACCES));
}

#[test]
fn the_aacs_files_are_read_from_aacs_bak_when_aacs_lacks_them() {
    let (b, mut aacs, id) = standard();
    aacs[0].0 = "AACS_BAK";
    aacs.push(("AACS", "DKF.AACS", vec![0u8; 64]));
    let kf = key_file("bak.cfg", &format!("{id} = Test | V | {}\n", hex(&VUK)));
    let d = open(&disc_with(&b.disc, aacs, 1), &[kf]).unwrap();
    assert!(d.block(1, 1).1 == cleared(&b.plain[1]));
}

#[test]
fn a_bad_title_key_file_fails_the_open() {
    let (b, mut aacs, _) = standard();
    aacs[0].2[..12].copy_from_slice(b"DVD_HD_V_TKX");
    assert_eq!(open(&disc_with(&b.disc, aacs.clone(), 1), &[]).err(), Some(INVALIDDATA));
    aacs[0].2[..12].copy_from_slice(b"DVD_HD_V_TKF");
    aacs[0].2[12..16].copy_from_slice(&0x9b1u32.to_be_bytes());
    assert_eq!(open(&disc_with(&b.disc, aacs, 1), &[]).err(), Some(INVALIDDATA));
    // no VTKF000.AACS: no disc identifier
    let aacs = vec![("AACS", "DKF.AACS", vec![0u8; 64])];
    assert_eq!(open(&disc_with(&b.disc, aacs, 1), &[]).err(), Some(INVALIDDATA));
}

#[test]
fn without_an_aacs_directory_scrambled_packs_cannot_be_used() {
    let (b, _, _) = standard();
    let d = open(&disc_with(&b.disc, vec![], 1), &[]).unwrap();
    assert_eq!(d.block(1, 1).0, 0);
    assert_eq!(d.block(1, 2).0, 1);
}

#[test]
fn the_navigation_pack_is_looked_for_back_from_the_pack() {
    let keys = title_keys();
    let t = tkf(&VUK, &keys);
    let kf = key_file("back.cfg", &format!("{} = Test | V | {}\n", hex(&sha1(&t)), hex(&VUK)));
    // the extent's first block is no navigation pack; the one two blocks back is
    let plain = [data_pack(0xe0, 0, 1), nav_pack(2, 5, &SEED), data_pack(0xe0, 0, 2), data_pack(0xbd, 1, 5)];
    let mut disc = plain.to_vec();
    disc[3] = encrypt_pack(&plain[3], &keys[4], &SEED);
    let d = open(&disc_with(&disc, vec![("AACS", "VTKF000.AACS", t.clone())], 1), std::slice::from_ref(&kf)).unwrap();
    assert!(d.block(1, 3).1 == cleared(&plain[3]));
    // no navigation pack at or before it: not usable
    let plain = [data_pack(0xe0, 0, 1), data_pack(0xbd, 1, 5)];
    let disc = [plain[0].clone(), encrypt_pack(&plain[1], &keys[4], &SEED)];
    let d = open(&disc_with(&disc, vec![("AACS", "VTKF000.AACS", t)], 1), &[kf]).unwrap();
    assert_eq!(d.block(1, 1).0, 0);
}

#[test]
fn only_key_mode_1_decrypts() {
    let keys = title_keys();
    let t = tkf(&VUK, &keys);
    let kf = key_file("mode.cfg", &format!("{} = Test | V | {}\n", hex(&sha1(&t)), hex(&VUK)));
    // mode bits 0 -> mode 0, 1 -> mode 2, 3 -> invalid
    for bits in [0u8, 1, 3] {
        let b = blocks(&keys[4], bits);
        let d = open(&disc_with(&b.disc, vec![("AACS", "VTKF000.AACS", t.clone())], 1), std::slice::from_ref(&kf)).unwrap();
        assert_eq!(d.block(1, 1).0, 0, "mode bits {bits}");
        assert_eq!(d.block(1, 2).0, 1, "mode bits {bits}: a clear pack stays usable");
    }
}

#[test]
fn title_key_ids_start_at_the_highest_playlist_naming_the_evob_times_256() {
    // both playlists name A.EVO: its title keys come from VTKF001.AACS (ids 0x101..)
    let keys0 = title_keys();
    let mut keys1 = title_keys();
    keys1[4] = [0x77; 16];
    let t0 = tkf(&VUK, &keys0);
    let kf = key_file("pl.cfg", &format!("{} = Test | V | {}\n", hex(&sha1(&t0)), hex(&VUK)));
    let b = blocks(&keys1[4], 2);
    let aacs = vec![("AACS", "VTKF000.AACS", t0), ("AACS", "VTKF001.AACS", tkf(&VUK, &keys1))];
    let d = open(&disc_with(&b.disc, aacs, 2), &[kf]).unwrap();
    assert!(d.block(1, 1).1 == cleared(&b.plain[1]));
}

// ---- corpus ----

fn keydb() -> Option<Vec<String>> {
    std::env::var("HDDVD_KEYDB").ok().map(|s| s.split(',').map(str::to_owned).collect())
}

/// Decrypt 4000 blocks of the 300's feature (slot 7): (E-AC-3 packs, with a
/// sync word, a hash of every block).
fn feature_blocks(d: &common::hddvd::Opened) -> (usize, usize, [u8; 20]) {
    let (mut eac3, mut synced, mut all) = (0, 0, Vec::new());
    for blk in 0..4000 {
        let (r, s) = d.block(7, blk);
        assert_eq!(r, 1, "block {blk}");
        let h = 14 + if s[0xd] & 7 != 0 && s[0xe] != 0 { usize::from(s[0xd] & 7) } else { 0 };
        if s[h + 3] == 0xbd {
            let payload = &s[h + 9 + usize::from(s[h + 8])..];
            if (0xc0..=0xcf).contains(&payload[0]) {
                eac3 += 1;
                synced += usize::from(payload.windows(2).any(|w| w == [0x0b, 0x77]));
            }
        }
        all.extend_from_slice(&sha1(&s));
    }
    (eac3, synced, sha1(&all))
}

#[test]
fn corpus_the_encrypted_discs_decrypt_with_the_key_files() {
    let (Ok(dir), Some(kf)) = (std::env::var("HDDVD_CORPUS"), keydb()) else {
        eprintln!("HDDVD_CORPUS / HDDVD_KEYDB not set: corpus check skipped");
        return;
    };
    let combo = PathBuf::from(&dir).join("300 (HD-DVD Combo, HD-DVD Side).iso");
    let d = open_file(&combo, &kf).unwrap();
    let (eac3, synced, digest) = feature_blocks(&d);
    eprintln!("300 Combo: {eac3} E-AC-3 packs, {synced} with a sync word");
    assert!(eac3 > 100);
    assert_eq!(eac3, synced);
    drop(d);

    // the same blocks with a volume unique key from a device key and the volume ID only
    let full = std::fs::read_to_string(kf.iter().find(|f| f.ends_with("keydb.cfg")).expect("keydb.cfg in HDDVD_KEYDB")).unwrap();
    let dks: String = full.lines().filter(|l| l.starts_with("| DK |")).flat_map(|l| [l, "\n"]).collect();
    let entry = full.lines().find(|l| l.starts_with("0xA41555566FDA15BAC3C9DD9E52178D388787B67C")).unwrap();
    let vid = entry.split("| I | ").nth(1).unwrap().split_whitespace().next().unwrap();
    let only = key_file("dk_vid.cfg", &format!("{dks}0xA41555566FDA15BAC3C9DD9E52178D388787B67C = 300 | I | {vid}\n"));
    let d = open_file(&combo, &[only]).unwrap();
    assert_eq!(feature_blocks(&d).2, digest, "same blocks with a computed VUK");

    // the Standalone disc: no key for it in the key files
    let standalone = PathBuf::from(&dir).join("300 (Standalone HD-DVD).iso");
    assert_eq!(open_file(&standalone, &kf).err(), Some(EACCES));
}
