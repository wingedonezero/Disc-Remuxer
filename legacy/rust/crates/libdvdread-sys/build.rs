//! Compiles the our copy of libdvdread (`libs/libdvdread`) into a
//! static library with the `cc` crate, linked directly against our libdvdcss
//! (no runtime `dlopen`).
//!
//! `config.h` and `dvdread/version.h` are generated into `OUT_DIR`, mirroring
//! what upstream's `meson.build` would produce on Linux with
//! `-Dlibdvdcss=enabled`.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

const VERSION: (u32, u32, u32) = (7, 1, 1);

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    assert!(
        target_os == "linux",
        "libdvdread-sys: only Linux is configured so far (target_os = {target_os})"
    );

    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.join("../../libs/libdvdread");
    let src = root.join("src");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let gen = out.join("include");
    fs::create_dir_all(gen.join("dvdread")).unwrap();

    let (major, minor, micro) = VERSION;
    let version = format!("{major}.{minor}.{micro}");

    // Same values meson gives on a Linux/glibc host, plus STRERROR_R_CHAR_P:
    // with _DEFAULT_SOURCE glibc declares the GNU strerror_r (returns char *),
    // which is the branch dvd_input.c selects with that macro.
    fs::write(
        gen.join("config.h"),
        format!(
            "#define PACKAGE_VERSION \"{version}\"\n\
             #define HAVE_SYS_PARAM_H 1\n\
             #define HAVE_LIMITS_H 1\n\
             #define HAVE_DIRENT_H 1\n\
             #define UNUSED __attribute__((unused))\n\
             #define HAVE_GETMNTENT_R 1\n\
             #define HAVE_STRERROR_R 1\n\
             #define HAVE_DECL_STRERROR_R 1\n\
             #define STRERROR_R_CHAR_P 1\n\
             #define HAVE_STATIC_ASSERT 1\n\
             #define HAVE_DVDCSS_DVDCSS_H 1\n\
             #define HAVE_DVDCSS_DVDCPXM_H 1\n"
        ),
    )
    .unwrap();

    configure_file(
        &src.join("dvdread/version.h.in"),
        &gen.join("dvdread/version.h"),
        &[
            ("@DVDREAD_VERSION_MAJOR@", major),
            ("@DVDREAD_VERSION_MINOR@", minor),
            ("@DVDREAD_VERSION_MICRO@", micro),
        ],
    );

    let files = [
        "bitreader.c",
        "dvd_input.c",
        "dvd_reader.c",
        "dvd_udf.c",
        "ifo_print.c",
        "ifo_read.c",
        "logger.c",
        "md5.c",
        "nav_print.c",
        "nav_read.c",
        "file/file_posix.c",
    ];
    let mut build = cc::Build::new();
    build
        .include(&gen)
        .include(&root)
        .include(&src)
        .include(src.join("dvdread"))
        .define("_DEFAULT_SOURCE", None)
        .define("_LARGEFILE64_SOURCE", None)
        .flag_if_supported("-std=c11")
        .warnings(false);
    for dir in env::split_paths(&env::var_os("DEP_DVDCSS_INCLUDE").expect(
        "DEP_DVDCSS_INCLUDE not set: libdvdread-sys must depend on libdvdcss-sys",
    )) {
        build.include(dir);
    }
    for f in files {
        build.file(src.join(f));
    }
    build.compile("dvdread");

    // Headers and library folder for dependent -sys crates
    // (DEP_DVDREAD_INCLUDE, DEP_DVDREAD_ROOT).
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
