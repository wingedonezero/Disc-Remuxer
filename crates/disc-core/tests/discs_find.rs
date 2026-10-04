//! Finding discs under a folder and choosing job folders.

use std::fs;
use std::path::{Path, PathBuf};

use disc_core::discs::{find, job_folder, safe_name, Layout};

fn tree(test: &str) -> PathBuf {
    let root = std::env::temp_dir().join(format!("disc-core-discs-{test}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&root);
    let lib = root.join("Library");
    for d in ["Show/Vol 1/VIDEO_TS", "Show/Vol 2/BDMV", "Bare", "deep/1/2/3/4/5/6/VIDEO_TS", "Empty"] {
        fs::create_dir_all(lib.join(d)).unwrap();
    }
    fs::write(lib.join("Bare/VIDEO_TS.IFO"), b"").unwrap();
    fs::write(lib.join("Show/Extras.ISO"), b"").unwrap();
    fs::write(lib.join("notes.txt"), b"").unwrap();
    lib
}

#[test]
fn finds_every_kind_in_name_order_within_the_depth() {
    let lib = tree("find");
    let (discs, skipped) = find(&lib, 5);
    assert!(skipped.is_empty());
    let got: Vec<(String, Layout, PathBuf)> = discs.iter().map(|d| (d.name.clone(), d.layout, d.relative.clone())).collect();
    assert_eq!(
        got,
        [
            ("Bare".into(), Layout::DvdFilesFolder, PathBuf::from("Bare")),
            ("Extras".into(), Layout::Image, PathBuf::from("Show/Extras.ISO")),
            ("Vol 1".into(), Layout::DvdFolder, PathBuf::from("Show/Vol 1")),
            ("Vol 2".into(), Layout::BlurayFolder, PathBuf::from("Show/Vol 2")),
        ]
    );
    // The disc 7 levels down is found once the depth allows it.
    assert_eq!(find(&lib, 7).0.len(), 5);
    // A disc given directly.
    let (one, _) = find(&lib.join("Show/Vol 1"), 0);
    assert_eq!((one[0].relative.as_path(), one[0].layout), (Path::new("."), Layout::DvdFolder));
}

#[test]
fn job_folders_keep_the_structure_and_never_reuse_a_folder() {
    let lib = tree("folders");
    let out = lib.parent().unwrap().join("out");
    let (discs, _) = find(&lib, 5);
    let vol1 = discs.iter().find(|d| d.name == "Vol 1").unwrap();
    let iso = discs.iter().find(|d| d.name == "Extras").unwrap();
    assert_eq!(job_folder(vol1, &lib, &out, true), out.join("Library/Show/Vol 1"));
    assert_eq!(job_folder(iso, &lib, &out, true), out.join("Library/Show/Extras"));
    assert_eq!(job_folder(vol1, &lib, &out, false), out.join("Vol 1"));
    fs::create_dir_all(out.join("Vol 1")).unwrap();
    fs::create_dir_all(out.join("Vol 1_001")).unwrap();
    assert_eq!(job_folder(vol1, &lib, &out, false), out.join("Vol 1_002"));
}

#[test]
fn safe_names() {
    assert_eq!(safe_name("A: B/C*?\"<>|  D"), "A B C D");
    assert_eq!(safe_name("  //  "), "Unnamed");
    assert_eq!(safe_name("ドラゴノーツ [R2J]"), "ドラゴノーツ [R2J]");
}
