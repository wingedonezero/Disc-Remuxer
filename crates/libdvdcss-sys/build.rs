//! Build script for libdvdcss-sys.
//!
//! 1. Invokes meson + ninja against `vendor/libdvdcss/` to build a shared
//!    libdvdcss.so into `$OUT_DIR/install/`.
//! 2. Generates Rust FFI bindings from `<dvdcss/dvdcss.h>` via bindgen.
//! 3. Tells cargo to link `dvdcss` and bakes the install lib dir into rpath
//!    so `cargo test` / `cargo run` in dev finds the .so without bundling.
//! 4. Exposes the include / lib paths to dependent -sys crates via the
//!    `cargo:KEY=VALUE` mechanism (becomes `DEP_DVDCSS_KEY` in their build
//!    scripts).

use std::env;
use std::path::PathBuf;
use std::process::Command;

const LIB_NAME: &str = "dvdcss";

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    let src_dir = manifest_dir
        .join("..")
        .join("..")
        .join("vendor")
        .join("libdvdcss")
        .canonicalize()
        .expect("vendor/libdvdcss not found — run `git submodule update --init`");

    let build_dir = out_dir.join("build");
    let install_dir = out_dir.join("install");
    let lib_dir = install_dir.join("lib");
    let include_dir = install_dir.join("include");

    run_meson_build(&src_dir, &build_dir, &install_dir, &[]);

    // Linkage: tell rustc to link dvdcss and where to find it.
    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=dylib={}", LIB_NAME);

    // Bake the install lib dir into rpath for dev (cargo test / cargo run).
    // Distribution sets rpath=$ORIGIN via .cargo/config.toml and copies the
    // .so files next to the binary, which takes precedence at runtime when
    // libs are co-located. Both rpaths in the binary is fine.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());

    // Expose paths to dependent -sys crates via DEP_DVDCSS_*.
    println!("cargo:include={}", include_dir.display());
    println!("cargo:lib_dir={}", lib_dir.display());
    println!("cargo:pkg_config_dir={}", lib_dir.join("pkgconfig").display());

    generate_bindings(&manifest_dir, &out_dir, &include_dir);

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=wrapper.h");
}

fn run_meson_build(src_dir: &PathBuf, build_dir: &PathBuf, install_dir: &PathBuf, extra_opts: &[&str]) {
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
        // Only emit the public dvdcss_* / DVDCSS_* surface.
        .allowlist_function("dvdcss_.*")
        .allowlist_type("dvdcss_.*")
        .allowlist_var("DVDCSS_.*")
        // Map size_t et al. to libc types instead of bindgen's own ::std::os::raw::*.
        .size_t_is_usize(true)
        .generate()
        .expect("bindgen failed to generate libdvdcss bindings");

    bindings
        .write_to_file(out_dir.join("bindings.rs"))
        .expect("failed to write libdvdcss bindings.rs");
}
