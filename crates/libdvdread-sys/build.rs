//! Build script for libdvdread-sys.
//!
//! Builds the vendored libdvdread via meson with `-Dlibdvdcss=enabled`,
//! pointing meson's pkg-config search at libdvdcss-sys's install dir
//! (passed in via `DEP_DVDCSS_PKG_CONFIG_DIR`) so the resulting
//! libdvdread.so has libdvdcss.so as a regular DT_NEEDED dependency
//! rather than the legacy runtime dlopen() lookup.

use std::env;
use std::path::PathBuf;
use std::process::Command;

const LIB_NAME: &str = "dvdread";

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    let src_dir = manifest_dir
        .join("..")
        .join("..")
        .join("vendor")
        .join("libdvdread")
        .canonicalize()
        .expect("vendor/libdvdread not found — run `git submodule update --init`");

    let build_dir = out_dir.join("build");
    let install_dir = out_dir.join("install");
    let lib_dir = install_dir.join("lib");
    let include_dir = install_dir.join("include");

    // libdvdcss-sys exports its install paths via the `links = "dvdcss"` mechanism.
    let dvdcss_pkg_config_dir = env::var("DEP_DVDCSS_PKG_CONFIG_DIR")
        .expect("DEP_DVDCSS_PKG_CONFIG_DIR not set — does Cargo.toml depend on libdvdcss-sys?");
    let dvdcss_lib_dir = env::var("DEP_DVDCSS_LIB_DIR").unwrap();

    // meson's pkg-config probe for libdvdcss reads PKG_CONFIG_PATH from env.
    let pkg_config_path = match env::var("PKG_CONFIG_PATH") {
        Ok(existing) if !existing.is_empty() => format!("{}:{}", dvdcss_pkg_config_dir, existing),
        _ => dvdcss_pkg_config_dir.clone(),
    };

    run_meson_build(
        &src_dir,
        &build_dir,
        &install_dir,
        &["-Dlibdvdcss=enabled"],
        &[("PKG_CONFIG_PATH", &pkg_config_path)],
    );

    // Linkage.
    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-search=native={}", dvdcss_lib_dir);
    println!("cargo:rustc-link-lib=dylib={}", LIB_NAME);
    // libdvdcss is already linked transitively via DT_NEEDED in libdvdread.so,
    // but cargo's link-lib propagation from libdvdcss-sys handles the explicit
    // `-ldvdcss` flag we'd otherwise need.

    // Rpath for dev (cargo test / cargo run). Both lib dirs.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dvdcss_lib_dir);

    // Expose paths to dependent -sys crates via DEP_DVDREAD_*.
    println!("cargo:include={}", include_dir.display());
    println!("cargo:lib_dir={}", lib_dir.display());
    println!("cargo:pkg_config_dir={}", lib_dir.join("pkgconfig").display());

    generate_bindings(&manifest_dir, &out_dir, &include_dir);

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=wrapper.h");
    println!("cargo:rerun-if-env-changed=DEP_DVDCSS_PKG_CONFIG_DIR");
}

fn run_meson_build(
    src_dir: &PathBuf,
    build_dir: &PathBuf,
    install_dir: &PathBuf,
    extra_opts: &[&str],
    extra_env: &[(&str, &str)],
) {
    let needs_setup = !build_dir.join("build.ninja").exists();

    if needs_setup {
        let mut cmd = Command::new("meson");
        cmd.arg("setup")
            .arg("--buildtype=release")
            .arg("--default-library=shared")
            .arg("--libdir=lib")
            .arg(format!("--prefix={}", install_dir.display()))
            .arg("-Denable_docs=false");
        for opt in extra_opts {
            cmd.arg(opt);
        }
        cmd.arg(build_dir).arg(src_dir);
        for (k, v) in extra_env {
            cmd.env(k, v);
        }

        let status = cmd
            .status()
            .expect("failed to invoke `meson setup` — is meson installed?");
        assert!(status.success(), "meson setup failed for {}", LIB_NAME);
    }

    let status = Command::new("meson")
        .arg("compile")
        .arg("-C")
        .arg(build_dir)
        .status()
        .expect("failed to invoke `meson compile`");
    assert!(status.success(), "meson compile failed for {}", LIB_NAME);

    let status = Command::new("meson")
        .arg("install")
        .arg("--quiet")
        .arg("-C")
        .arg(build_dir)
        .status()
        .expect("failed to invoke `meson install`");
    assert!(status.success(), "meson install failed for {}", LIB_NAME);
}

fn generate_bindings(manifest_dir: &PathBuf, out_dir: &PathBuf, include_dir: &PathBuf) {
    let wrapper = manifest_dir.join("wrapper.h");

    let bindings = bindgen::Builder::default()
        .header(wrapper.to_str().unwrap())
        .clang_arg(format!("-I{}", include_dir.display()))
        // Emit the public libdvdread surface: function families + all IFO
        // struct types + the version macros. Field names follow libdvdread's
        // public C headers (`tt_srpt_t`, `pgc_t`, `cell_playback_t`, etc.).
        .allowlist_function("DVD[A-Z][a-zA-Z_]+")     // DVDOpen, DVDReadBlocks, ...
        .allowlist_function("ifo[A-Z][a-zA-Z_]+")     // ifoOpen, ifoClose, ...
        .allowlist_function("nav[A-Z][a-zA-Z_]+")     // navRead_PCI, navRead_DSI, ...
        .allowlist_function("UDF[A-Z][a-zA-Z_]+")     // UDFFindFile, ...
        .allowlist_type(".*_t")                       // pgc_t, vts_t, etc.
        .allowlist_type("[A-Za-z]+_(t|info_t|playback_t|position_t)")
        .allowlist_var("DVD_.*")
        .allowlist_var("VTS_.*")
        .size_t_is_usize(true)
        .generate()
        .expect("bindgen failed to generate libdvdread bindings");

    bindings
        .write_to_file(out_dir.join("bindings.rs"))
        .expect("failed to write libdvdread bindings.rs");
}
