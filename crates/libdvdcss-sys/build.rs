//! Compiles the our copy of libdvdcss (`libs/libdvdcss`) into a
//! static library with the `cc` crate. No meson, ninja or pkg-config needed.
//!
//! `config.h` and `dvdcss/version.h` are generated into `OUT_DIR`, mirroring
//! what upstream's `meson.build` would produce on Linux.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

const VERSION: (u32, u32, u32) = (1, 6, 0);

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    assert!(
        target_os == "linux",
        "libdvdcss-sys: only Linux is configured so far (target_os = {target_os})"
    );

    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.join("../../libs/libdvdcss");
    let src = root.join("src");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let gen = out.join("include");
    fs::create_dir_all(gen.join("dvdcss")).unwrap();

    let (major, minor, micro) = VERSION;
    let version = format!("{major}.{minor}.{micro}");

    // Same values meson's header checks give on a Linux/glibc host.
    fs::write(
        gen.join("config.h"),
        format!(
            "#define PACKAGE_VERSION \"{version}\"\n\
             #define HAVE_ERRNO_H 1\n\
             #define HAVE_FCNTL_H 1\n\
             #define HAVE_PWD_H 1\n\
             #define HAVE_SCSI_SG_H 1\n\
             #define HAVE_SYS_IOCTL_H 1\n\
             #define HAVE_SYS_PARAM_H 1\n\
             #define HAVE_SYS_STAT_H 1\n\
             #define HAVE_SYS_TYPES_H 1\n\
             #define HAVE_SYS_UIO_H 1\n\
             #define HAVE_UNISTD_H 1\n\
             #define DVD_STRUCT_IN_LINUX_CDROM_H 1\n\
             #define HAVE_LINUX_DVD_STRUCT 1\n\
             #define SUPPORT_ATTRIBUTE_VISIBILITY_DEFAULT 1\n"
        ),
    )
    .unwrap();

    configure_file(
        &src.join("dvdcss/version.h.in"),
        &gen.join("dvdcss/version.h"),
        &[
            ("@DVDCSS_VERSION_MAJOR@", major),
            ("@DVDCSS_VERSION_MINOR@", minor),
            ("@DVDCSS_VERSION_MICRO@", micro),
        ],
    );

    let files = [
        "cpxm.c",
        "css.c",
        "device.c",
        "error.c",
        "ioctl.c",
        "libdvdcpxm.c",
        "libdvdcss.c",
    ];
    let mut build = cc::Build::new();
    build
        .include(&gen)
        .include(&root)
        .include(&src)
        .include(src.join("dvdcss"))
        .define("_DEFAULT_SOURCE", None)
        .flag_if_supported("-std=c17")
        .warnings(false);
    for f in files {
        build.file(src.join(f));
    }
    build.compile("dvdcss");

    // Headers and library folder for dependent -sys crates
    // (DEP_DVDCSS_INCLUDE, DEP_DVDCSS_ROOT).
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
