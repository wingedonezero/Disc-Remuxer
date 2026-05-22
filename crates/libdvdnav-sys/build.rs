//! Build script for libdvdnav-sys.
//!
//! Builds the vendored libdvdnav via meson, pointing pkg-config at both the
//! bundled libdvdread and libdvdcss install dirs. libdvdnav links libdvdread
//! at link time; libdvdread already DT_NEEDED's libdvdcss, so both are pulled
//! in transitively at runtime.

use std::env;
use std::path::PathBuf;
use std::process::Command;

const LIB_NAME: &str = "dvdnav";

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    let src_dir = manifest_dir
        .join("..")
        .join("..")
        .join("vendor")
        .join("libdvdnav")
        .canonicalize()
        .expect("vendor/libdvdnav not found — run `git submodule update --init`");

    let build_dir = out_dir.join("build");
    let install_dir = out_dir.join("install");
    let lib_dir = install_dir.join("lib");
    let include_dir = install_dir.join("include");

    let dvdread_pkg_config_dir = env::var("DEP_DVDREAD_PKG_CONFIG_DIR")
        .expect("DEP_DVDREAD_PKG_CONFIG_DIR not set — does Cargo.toml depend on libdvdread-sys?");
    let dvdread_lib_dir = env::var("DEP_DVDREAD_LIB_DIR").unwrap();
    let dvdcss_pkg_config_dir = env::var("DEP_DVDCSS_PKG_CONFIG_DIR")
        .expect("DEP_DVDCSS_PKG_CONFIG_DIR not set — does Cargo.toml depend on libdvdcss-sys?");
    let dvdcss_lib_dir = env::var("DEP_DVDCSS_LIB_DIR").unwrap();

    let pkg_config_path = {
        let mut parts = vec![dvdread_pkg_config_dir.clone(), dvdcss_pkg_config_dir.clone()];
        if let Ok(existing) = env::var("PKG_CONFIG_PATH") {
            if !existing.is_empty() {
                parts.push(existing);
            }
        }
        parts.join(":")
    };

    run_meson_build(
        &src_dir,
        &build_dir,
        &install_dir,
        &[],
        &[("PKG_CONFIG_PATH", &pkg_config_path)],
    );

    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-search=native={}", dvdread_lib_dir);
    println!("cargo:rustc-link-search=native={}", dvdcss_lib_dir);
    println!("cargo:rustc-link-lib=dylib={}", LIB_NAME);

    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dvdread_lib_dir);
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dvdcss_lib_dir);

    println!("cargo:include={}", include_dir.display());
    println!("cargo:lib_dir={}", lib_dir.display());
    println!("cargo:pkg_config_dir={}", lib_dir.join("pkgconfig").display());

    generate_bindings(&manifest_dir, &out_dir, &include_dir);

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=wrapper.h");
    println!("cargo:rerun-if-env-changed=DEP_DVDREAD_PKG_CONFIG_DIR");
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
        .allowlist_function("dvdnav_.*")
        .allowlist_type("dvdnav_.*")
        .allowlist_type(".*event_t")
        .allowlist_var("DVDNAV_.*")
        .size_t_is_usize(true)
        .generate()
        .expect("bindgen failed to generate libdvdnav bindings");

    bindings
        .write_to_file(out_dir.join("bindings.rs"))
        .expect("failed to write libdvdnav bindings.rs");
}
