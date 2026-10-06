//! Compiles our copy of expat (`libs/expat`) into a static library with the
//! `cc` crate.
//!
//! `expat_config.h` is generated into `OUT_DIR` with the values expat's own
//! `configure` gives on Linux / glibc (the defaults: `XML_DTD`, `XML_GE`,
//! `XML_NS`, `XML_CONTEXT_BYTES` 1024; the randomness sources glibc provides).

use std::env;
use std::fs;
use std::path::PathBuf;

const VERSION: (u32, u32, u32) = (2, 9, 0);

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    assert!(
        target_os == "linux",
        "libexpat-sys: only Linux is configured so far (target_os = {target_os})"
    );

    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.join("../../libs/expat");
    let lib = root.join("lib");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let gen = out.join("include");
    fs::create_dir_all(&gen).unwrap();

    let (major, minor, micro) = VERSION;
    let version = format!("{major}.{minor}.{micro}");

    fs::write(
        gen.join("expat_config.h"),
        format!(
            "#ifndef EXPAT_CONFIG_H\n\
             #define EXPAT_CONFIG_H 1\n\
             #define BYTEORDER 1234\n\
             #define HAVE_ARC4RANDOM 1\n\
             #define HAVE_ARC4RANDOM_BUF 1\n\
             #define HAVE_DLFCN_H 1\n\
             #define HAVE_FCNTL_H 1\n\
             #define HAVE_GETENTROPY 1\n\
             #define HAVE_GETPAGESIZE 1\n\
             #define HAVE_GETRANDOM 1\n\
             #define HAVE_INTTYPES_H 1\n\
             #define HAVE_MMAP 1\n\
             #define HAVE_STDINT_H 1\n\
             #define HAVE_STDIO_H 1\n\
             #define HAVE_STDLIB_H 1\n\
             #define HAVE_STRINGS_H 1\n\
             #define HAVE_STRING_H 1\n\
             #define HAVE_SYSCALL_GETRANDOM 1\n\
             #define HAVE_SYS_PARAM_H 1\n\
             #define HAVE_SYS_STAT_H 1\n\
             #define HAVE_SYS_TYPES_H 1\n\
             #define HAVE_UNISTD_H 1\n\
             #define PACKAGE \"expat\"\n\
             #define PACKAGE_NAME \"expat\"\n\
             #define PACKAGE_STRING \"expat {version}\"\n\
             #define PACKAGE_TARNAME \"expat\"\n\
             #define PACKAGE_VERSION \"{version}\"\n\
             #define STDC_HEADERS 1\n\
             #define VERSION \"{version}\"\n\
             #define XML_CONTEXT_BYTES 1024\n\
             #define XML_DTD 1\n\
             #define XML_GE 1\n\
             #define XML_NS 1\n\
             #endif\n"
        ),
    )
    .unwrap();

    // lib/Makefile.am on Linux: the core files plus every randomness source
    // configure enables there (arc4random, arc4random_buf, /dev/urandom,
    // getentropy, getrandom).
    let files = [
        "xcs.c",
        "xmlparse.c",
        "xmltok.c",
        "xmlrole.c",
        "random_arc4random.c",
        "random_arc4random_buf.c",
        "random_dev_urandom.c",
        "random_getentropy.c",
        "random_getrandom.c",
    ];
    let mut build = cc::Build::new();
    build.include(&gen).include(&lib).warnings(false);
    for f in files {
        build.file(lib.join(f));
    }
    build.compile("expat");

    // Headers and library folder for dependent -sys crates
    // (DEP_EXPAT_INCLUDE, DEP_EXPAT_ROOT, DEP_EXPAT_VERSION).
    println!("cargo:include={}", lib.display());
    println!("cargo:root={}", out.display());
    println!("cargo:version={version}");
    println!("cargo:rerun-if-changed={}", root.display());
}
