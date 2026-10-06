//! Builds our copy of `CCExtractor` (`libs/ccextractor`) with its own `CMake`,
//! C only (`WITHOUT_RUST=ON`; no OCR, FFmpeg or sharing), and copies the
//! `ccextractor` program to `target/<profile>/`, next to ours. Its `CMake`
//! writes a header into the source tree, so it runs on a copy of `src` in
//! `OUT_DIR` (files copied only when their bytes changed, so `make` stays
//! incremental).

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    assert!(target_os == "linux", "ccextractor-sys: only Linux is configured so far (target_os = {target_os})");
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let src = manifest.join("../../libs/ccextractor/src").canonicalize().unwrap();
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let copy = out.join("src");
    let build = out.join("build");
    sync_tree(&src, &copy);
    fs::create_dir_all(&build).unwrap();

    let args = ["-DWITHOUT_RUST=ON", "-DWITH_OCR=OFF", "-DWITH_FFMPEG=OFF", "-DWITH_SHARING=OFF", "-DWITH_HARDSUBX=OFF",
        "-DCMAKE_BUILD_TYPE=Release"];
    let stamp = build.join("disc-remuxer-cmake-args");
    let wanted = args.join("\n");
    let configured = build.join("Makefile").exists() && fs::read_to_string(&stamp).is_ok_and(|s| s == wanted);
    if !configured {
        run(Command::new("cmake").arg(&copy).args(args).current_dir(&build), "cmake");
        fs::write(&stamp, &wanted).unwrap();
    }
    let jobs = env::var("NUM_JOBS").unwrap_or_else(|_| "1".into());
    run(Command::new("make").arg(format!("-j{jobs}")).arg("ccextractor").current_dir(&build), "make ccextractor");

    let profile_dir = out.ancestors().nth(3).unwrap();
    let from = build.join("ccextractor");
    let to = profile_dir.join("ccextractor");
    fs::copy(&from, &to).unwrap_or_else(|e| panic!("cannot copy {} to {}: {e}", from.display(), to.display()));
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed=build.rs");
}

/// Copies the tree at `from` to `to`, writing only files whose bytes differ.
fn sync_tree(from: &Path, to: &Path) {
    fs::create_dir_all(to).unwrap();
    for entry in fs::read_dir(from).unwrap() {
        let entry = entry.unwrap();
        let (src, dst) = (entry.path(), to.join(entry.file_name()));
        if entry.file_type().unwrap().is_dir() {
            sync_tree(&src, &dst);
        } else {
            let bytes = fs::read(&src).unwrap();
            if fs::read(&dst).ok().as_deref() != Some(bytes.as_slice()) {
                fs::write(&dst, &bytes).unwrap();
            }
        }
    }
}

fn run(command: &mut Command, what: &str) {
    let status = command.status().unwrap_or_else(|e| panic!("cannot start {what}: {e}"));
    assert!(status.success(), "{what} failed ({status})");
}
