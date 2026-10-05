//! `libavformat/dvdvideo_scan.c`: the navigation scan of the DVD-Video
//! demuxer. Its random numbers are glibc's `rand()` sequence, produced on its
//! own (one sequence per scan, untouched by anything else calling `rand()`).
//! With `DVDVIDEO_SCAN_REFERENCE` (recorded results of corpus discs) the scan
//! of each disc gives them again; with `DVDVIDEO_SCAN_FOLDER` (a DVD folder) a
//! read past the end of the title VOBs fails the whole scan.

use std::os::raw::{c_int, c_uint};
use std::path::{Path, PathBuf};

use ffmpeg_sys::discio::ImageOptions;
use ffmpeg_sys::dvdvideo::{self, ff_dvdvideo_rand_init, ff_dvdvideo_rand_next, Rand};

extern "C" {
    fn srand(seed: c_uint);
    fn rand() -> c_int;
}

fn sequence(seed: u32, n: usize) -> Vec<c_int> {
    let mut r = Rand::default();
    // SAFETY: r is a valid generator state.
    unsafe { ff_dvdvideo_rand_init(&raw mut r, seed) };
    // SAFETY: as above.
    (0..n).map(|_| unsafe { ff_dvdvideo_rand_next((&raw mut r).cast()) }).collect()
}

#[test]
fn the_random_numbers_are_glibcs_rand_sequence() {
    // the first values of rand() in a fresh glibc process (seed 1)
    assert_eq!(sequence(1, 6), [1_804_289_383, 846_930_886, 1_681_692_777, 1_714_636_915, 1_957_747_793, 424_238_335]);
    // and the C library's own rand() after srand(seed), far into the sequence
    for seed in [1, 2, 12_345, 0x7fff_ffff, 0xffff_ffff] {
        let ours = sequence(seed, 100_000);
        // SAFETY: plain C library calls; no other test of this binary uses rand().
        let libc: Vec<c_int> = unsafe {
            srand(seed);
            (0..100_000).map(|_| rand()).collect()
        };
        assert!(ours == libc, "seed {seed}: the sequences differ");
    }
}

#[test]
fn seed_0_is_seed_1() {
    assert_eq!(sequence(0, 1000), sequence(1, 1000));
}

// ---- the scan on real discs ----

/// The scan's results as `debug dvd-scan` prints them.
fn scan_lines(path: &Path) -> Vec<String> {
    let scan = dvdvideo::scan(path, &ImageOptions::default(), 5, None).expect("the scan runs");
    let mut out = Vec::new();
    if let Some(f) = &scan.failure {
        out.push(format!("scan failed: {f}"));
    }
    for r in &scan.results {
        let cells: Vec<String> = r.cells.iter().map(ToString::to_string).collect();
        out.push(format!("result {} title {} pgc {} cells {}", r.name, r.title, r.pgcn, cells.join(",")));
    }
    out.extend(scan.entered.iter().filter(|&&t| t != 0).map(|t| format!("entered {t}")));
    out
}

/// With `DVDVIDEO_SCAN_REFERENCE=<file>`: every disc of the file scans to the
/// lines recorded for it. The file holds `disc <path>` lines, each followed
/// by its result lines.
#[test]
fn corpus_scans_give_the_recorded_results() {
    let Some(file) = std::env::var_os("DVDVIDEO_SCAN_REFERENCE") else { return };
    let text = std::fs::read_to_string(&file).expect("the reference file");
    let mut discs: Vec<(PathBuf, Vec<String>)> = Vec::new();
    for line in text.lines() {
        if let Some(p) = line.strip_prefix("disc ") {
            discs.push((PathBuf::from(p), Vec::new()));
        } else if let Some(d) = discs.last_mut() {
            d.1.push(line.to_string());
        }
    }
    assert!(!discs.is_empty(), "no disc in the reference file");
    let mut bad = Vec::new();
    for (path, want) in &discs {
        if scan_lines(path) != *want {
            bad.push(path.display().to_string());
        }
    }
    assert!(bad.is_empty(), "{} of {} discs scan differently: {bad:?}", bad.len(), discs.len());
}

/// With `DVDVIDEO_SCAN_FOLDER=<a DVD folder>`: a copy whose title VOBs end
/// after a few blocks makes the navigation read past their end; that read is
/// refused and the whole scan fails, with no results.
#[test]
fn a_refused_read_fails_the_scan() {
    let Some(src) = std::env::var_os("DVDVIDEO_SCAN_FOLDER") else { return };
    let src = PathBuf::from(src);
    let video_ts = if src.join("VIDEO_TS").is_dir() { src.join("VIDEO_TS") } else { src.clone() };
    let tmp = std::env::temp_dir().join(format!("dvdvideo_scan_cut_{}", std::process::id()));
    let dst = tmp.join("VIDEO_TS");
    std::fs::create_dir_all(&dst).unwrap();
    for e in std::fs::read_dir(&video_ts).unwrap() {
        let e = e.unwrap();
        let name = e.file_name().to_string_lossy().to_uppercase();
        let to = dst.join(&name);
        // VTS_nn_k.VOB with k > 0: a title VOB
        let title_vob = name.len() == 12 && name.starts_with("VTS_") && name.get(6..8).is_some_and(|k| k != "_0")
            && name.get(8..) == Some(".VOB");
        if !title_vob {
            std::os::unix::fs::symlink(e.path(), &to).unwrap();
        } else if name.get(6..8) == Some("_1") {
            // the first title VOB: its first 16 blocks (at most) only; the
            // other title VOBs are left out, so the title VOBs end there
            let mut data = Vec::new();
            let f = std::fs::File::open(e.path()).unwrap();
            std::io::Read::read_to_end(&mut std::io::Read::take(f, 16 * 2048), &mut data).unwrap();
            std::fs::write(&to, data).unwrap();
        }
    }
    let scan = dvdvideo::scan(&tmp, &ImageOptions::default(), 5, None).expect("the scan runs");
    std::fs::remove_dir_all(&tmp).unwrap();
    let failure = scan.failure.expect("the scan failed");
    assert!(failure.contains("past the end of the VOB"), "{failure}");
    assert!(scan.results.is_empty());
}
