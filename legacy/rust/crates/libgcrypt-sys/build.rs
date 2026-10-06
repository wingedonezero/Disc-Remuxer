//! Builds our copy of libgcrypt (`libs/libgcrypt`) with its own
//! `configure` and `make`, out of tree in `OUT_DIR`, as a static library on
//! our libgpg-error (the cryptography libaacs uses).

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    assert!(target_os == "linux", "libgcrypt-sys: only Linux is configured so far (target_os = {target_os})");
    #[allow(unused_variables, reason = "not every library depends on another")]
    let dep = |name: &str| env::var(name).unwrap_or_else(|_| panic!("{name} not set"));
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let src = manifest.join("../../libs/libgcrypt").canonicalize().unwrap();
    let args: Vec<String> = vec![
        "--enable-static".into(),
        "--disable-shared".into(),
        "--with-pic".into(),
        "--disable-doc".into(),
        format!("--with-libgpg-error-prefix={}", dep("DEP_GPG_ERROR_ROOT")),
    ];
    let env_vars: Vec<(&str, String)> = vec![
        ("PATH", format!("{}/bin:{}", dep("DEP_GPG_ERROR_ROOT"), env::var("PATH").unwrap_or_default())),

    ];
    let prefix = configure_make(&src, &args, &env_vars);

    println!("cargo:rustc-link-search=native={}", prefix.join("lib").display());
    println!("cargo:rustc-link-lib=static=gcrypt");
    // Headers, library and pkg-config files for dependent -sys crates
    // (DEP_*_ROOT = the install prefix, DEP_*_INCLUDE, DEP_*_VERSION).
    println!("cargo:root={}", prefix.display());
    println!("cargo:include={}", prefix.join("include").display());
    println!("cargo:version=1.12.4");
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed=build.rs");
}

/// Runs `configure` (when its arguments changed) and `make install` for the
/// library at `src` out of tree in `OUT_DIR/build`, installing into
/// `OUT_DIR/install`, which is returned. The autotools are never run again
/// (their outputs ship with the release).
fn configure_make(src: &Path, args: &[String], env: &[(&str, String)]) -> PathBuf {
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let build = out.join("build");
    let prefix = out.join("install");
    fs::create_dir_all(&build).unwrap();
    let mut all = vec![format!("--prefix={}", prefix.display())];
    all.extend_from_slice(args);
    let stamp = build.join("disc-remuxer-configure-args");
    let wanted = all.join("\n");
    let configured = build.join("Makefile").exists() && fs::read_to_string(&stamp).is_ok_and(|s| s == wanted);
    if !configured {
        let mut c = Command::new(src.join("configure"));
        c.args(&all).current_dir(&build);
        for (k, v) in env {
            c.env(k, v);
        }
        run(&mut c, "configure");
        fs::write(&stamp, &wanted).unwrap();
    }
    let jobs = env::var("NUM_JOBS").unwrap_or_else(|_| "1".into());
    let no_autotools = ["ACLOCAL=true", "AUTOCONF=true", "AUTOMAKE=true", "AUTOHEADER=true", "MAKEINFO=true"];
    run(
        Command::new("make").arg(format!("-j{jobs}")).args(no_autotools).arg("install").current_dir(&build),
        "make install",
    );
    prefix
}

fn run(command: &mut Command, what: &str) {
    let status = command.status().unwrap_or_else(|e| panic!("cannot start {what}: {e}"));
    assert!(status.success(), "{what} failed ({status})");
}
