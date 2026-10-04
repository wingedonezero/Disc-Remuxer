//! Compiles the our copy of libdvdnav (`libs/libdvdnav`) into a
//! static library with the `cc` crate, against our libdvdread.
//!
//! `config.h` and `dvdnav/version.h` are generated into `OUT_DIR`, mirroring
//! what upstream's `meson.build` would produce on Linux.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

const VERSION: (u32, u32, u32) = (7, 0, 0);

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    assert!(
        target_os == "linux",
        "libdvdnav-sys: only Linux is configured so far (target_os = {target_os})"
    );

    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.join("../../libs/libdvdnav");
    let src = root.join("src");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let gen = out.join("include");
    fs::create_dir_all(gen.join("dvdnav")).unwrap();

    let (major, minor, micro) = VERSION;
    let version = format!("{major}.{minor}.{micro}");

    // Same values meson's header checks give on a Linux/glibc host.
    fs::write(
        gen.join("config.h"),
        format!(
            "#define VERSION \"{version}\"\n\
             #define HAVE_DLFCN_H 1\n\
             #define HAVE_INTTYPES_H 1\n\
             #define HAVE_MEMORY_H 1\n\
             #define HAVE_STDINT_H 1\n\
             #define HAVE_STDLIB_H 1\n\
             #define HAVE_STRINGS_H 1\n\
             #define HAVE_STRING_H 1\n\
             #define HAVE_SYS_STAT_H 1\n\
             #define HAVE_SYS_TYPES_H 1\n\
             #define HAVE_UNISTD_H 1\n\
             #define HAVE_GETTIMEOFDAY 1\n"
        ),
    )
    .unwrap();

    configure_file(
        &src.join("dvdnav/version.h.in"),
        &gen.join("dvdnav/version.h"),
        &[
            ("@DVDNAV_MAJOR@", major),
            ("@DVDNAV_MINOR@", minor),
            ("@DVDNAV_SUB@", micro),
        ],
    );

    let files = [
        "dvdnav.c",
        "highlight.c",
        "logger.c",
        "navigation.c",
        "read_cache.c",
        "searching.c",
        "settings.c",
        "vm/decoder.c",
        "vm/getset.c",
        "vm/play.c",
        "vm/vm.c",
        "vm/vmcmd.c",
        "vm/vmget.c",
    ];
    let mut build = cc::Build::new();
    build
        .include(&gen)
        .include(&root)
        .include(&src)
        .include(src.join("dvdnav"))
        .include(src.join("vm"))
        .define("HAVE_CONFIG_H", None)
        .define("_DEFAULT_SOURCE", None)
        .flag_if_supported("-std=c17")
        .flag_if_supported("-mno-ms-bitfields")
        .warnings(false);
    for dir in env::split_paths(&env::var_os("DEP_DVDREAD_INCLUDE").expect(
        "DEP_DVDREAD_INCLUDE not set: libdvdnav-sys must depend on libdvdread-sys",
    )) {
        build.include(dir);
    }
    for f in files {
        build.file(src.join(f));
    }
    // Hooks for this crate's tests (glue/selftest.c); unused otherwise.
    build.file(manifest.join("glue/selftest.c"));
    build.compile("dvdnav");
    println!("cargo:rerun-if-changed={}", manifest.join("glue").display());
    // libdvdnav uses pthread mutexes.
    println!("cargo:rustc-link-lib=pthread");

    // Headers and library folder for dependent -sys crates
    // (DEP_DVDNAV_INCLUDE, DEP_DVDNAV_ROOT).
    println!(
        "cargo:include={}",
        env::join_paths([gen.as_path(), src.as_path()])
            .unwrap()
            .to_str()
            .unwrap()
    );
    println!("cargo:root={}", out.display());
    println!("cargo:version={version}");
    println!("cargo:rerun-if-changed={}", root.display());
}

/// Replaces `@NAME@` placeholders the way meson's `configure_file` does and
/// fails the build if any placeholder is left.
fn configure_file(input: &Path, output: &Path, values: &[(&str, u32)]) {
    let mut text = fs::read_to_string(input)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", input.display()));
    for (name, value) in values {
        text = text.replace(name, &value.to_string());
    }
    assert!(!text.contains('@'), "unsubstituted placeholder in {}", input.display());
    fs::write(output, text).unwrap();
}
