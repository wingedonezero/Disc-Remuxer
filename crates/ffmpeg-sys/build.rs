//! Builds our copy of FFmpeg (`libs/ffmpeg`) with its own `configure` and
//! `make`, out of tree in `OUT_DIR`, as static libraries linked into the
//! workspace, plus the `ffmpeg` and `ffprobe` programs, which are copied next
//! to our binary in `target/<profile>/` for testing.
//!
//! FFmpeg finds libdvdread / libdvdnav / expat through pkg-config; the `.pc`
//! files are written here and point at the static libraries the
//! `libdvd*-sys` / `libexpat-sys` crates built. Autodetection is off, so the
//! build never picks up whatever happens to be installed on the host: every
//! external library is enabled by name.
//!
//! `configure` runs again only when its arguments change; `make` itself
//! rebuilds only what changed.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

/// FFmpeg libraries in link order (dependents first).
const LIBS: [&str; 7] = [
    "avdevice",
    "avfilter",
    "avformat",
    "avcodec",
    "swresample",
    "swscale",
    "avutil",
];

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    assert!(
        target_os == "linux",
        "ffmpeg-sys: only Linux is configured so far (target_os = {target_os})"
    );

    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let src = manifest.join("../../libs/ffmpeg").canonicalize().unwrap();
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let build_dir = out.join("build");
    let prefix = out.join("install");
    let pc_dir = out.join("pkgconfig");
    fs::create_dir_all(&build_dir).unwrap();
    fs::create_dir_all(&pc_dir).unwrap();

    write_dvd_pc_files(&pc_dir);
    write_expat_pc_file(&pc_dir);

    let args = [
        format!("--prefix={}", prefix.display()),
        "--enable-gpl".into(),
        "--enable-libdvdnav".into(),
        "--enable-libdvdread".into(),
        "--enable-libexpat".into(),
        "--disable-autodetect".into(),
        "--enable-static".into(),
        "--disable-shared".into(),
        "--enable-pic".into(),
        "--disable-doc".into(),
        "--disable-ffplay".into(),
        "--pkg-config=pkg-config".into(),
        "--pkg-config-flags=--static".into(),
    ];
    let stamp = build_dir.join("disc-remuxer-configure-args");
    let wanted = args.join("\n");
    let configured = build_dir.join("ffbuild/config.mak").exists()
        && fs::read_to_string(&stamp).is_ok_and(|s| s == wanted);
    if !configured {
        run(
            Command::new(src.join("configure"))
                .args(&args)
                .current_dir(&build_dir)
                .env("PKG_CONFIG_PATH", &pc_dir)
                .env("PKG_CONFIG_LIBDIR", &pc_dir),
            "FFmpeg configure",
        );
        fs::write(&stamp, &wanted).unwrap();
    }

    let jobs = env::var("NUM_JOBS").unwrap_or_else(|_| "1".into());
    run(
        Command::new("make")
            .arg(format!("-j{jobs}"))
            .current_dir(&build_dir),
        "FFmpeg make",
    );
    run(
        Command::new("make").arg("install").current_dir(&build_dir),
        "FFmpeg make install",
    );

    copy_programs(&prefix.join("bin"), &out);

    // Our C glue (glue/glue.c) comes first in the link line: it calls into
    // libavformat / libavutil.
    let include = prefix.join("include");
    cc::Build::new()
        .file(manifest.join("glue/glue.c"))
        .include(&include)
        .warnings(true)
        .compile("disc_remuxer_ffmpeg_glue");

    println!("cargo:rustc-link-search=native={}", prefix.join("lib").display());
    for lib in LIBS {
        println!("cargo:rustc-link-lib=static={lib}");
    }
    for lib in system_libs(&prefix.join("lib/pkgconfig"), &pc_dir) {
        println!("cargo:rustc-link-lib={lib}");
    }

    println!("cargo:include={}", include.display());
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed={}", manifest.join("glue").display());
    println!("cargo:rerun-if-changed=build.rs");
}

/// `dvdread.pc` / `dvdnav.pc` for FFmpeg's configure, pointing at the
/// libraries and headers of our `libdvd*-sys` builds.
fn write_dvd_pc_files(pc_dir: &Path) {
    let dep = |name: &str| env::var(name).unwrap_or_else(|_| panic!("{name} not set"));
    let includes = |name: &str| {
        env::split_paths(&dep(name))
            .map(|p| format!("-I{}", p.display()))
            .collect::<Vec<_>>()
            .join(" ")
    };
    let css_libs = format!("-L{} -ldvdcss", dep("DEP_DVDCSS_ROOT"));
    let read_libs = format!("-L{} -ldvdread {css_libs}", dep("DEP_DVDREAD_ROOT"));
    let nav_libs = format!("-L{} -ldvdnav {read_libs} -lpthread", dep("DEP_DVDNAV_ROOT"));
    let read_cflags = format!("{} {}", includes("DEP_DVDREAD_INCLUDE"), includes("DEP_DVDCSS_INCLUDE"));
    let nav_cflags = format!("{} {read_cflags}", includes("DEP_DVDNAV_INCLUDE"));
    let pc = |name: &str, version: String, cflags: &str, libs: &str| {
        fs::write(
            pc_dir.join(format!("{name}.pc")),
            format!(
                "Name: {name}\nDescription: {name} built by disc-remuxer\n\
                 Version: {version}\nCflags: {cflags}\nLibs: {libs}\n"
            ),
        )
        .unwrap();
    };
    pc("dvdread", dep("DEP_DVDREAD_VERSION"), &read_cflags, &read_libs);
    pc("dvdnav", dep("DEP_DVDNAV_VERSION"), &nav_cflags, &nav_libs);
}

/// `expat.pc` for FFmpeg's configure, pointing at our `libexpat-sys` build.
fn write_expat_pc_file(pc_dir: &Path) {
    let dep = |name: &str| env::var(name).unwrap_or_else(|_| panic!("{name} not set"));
    fs::write(
        pc_dir.join("expat.pc"),
        format!(
            "Name: expat\nDescription: expat built by disc-remuxer\n\
             Version: {}\nCflags: -I{}\nLibs: -L{} -lexpat\n",
            dep("DEP_EXPAT_VERSION"),
            dep("DEP_EXPAT_INCLUDE"),
            dep("DEP_EXPAT_ROOT")
        ),
    )
    .unwrap();
}

/// System libraries FFmpeg's static libraries need (from the installed
/// `.pc` files), minus the FFmpeg, DVD and expat libraries linked separately.
fn system_libs(ffmpeg_pc: &Path, dvd_pc: &Path) -> Vec<String> {
    let path = env::join_paths([ffmpeg_pc, dvd_pc]).unwrap();
    let names: Vec<String> = LIBS.iter().map(|l| format!("lib{l}")).collect();
    let output = Command::new("pkg-config")
        .args(["--static", "--libs-only-l"])
        .args(&names)
        .env("PKG_CONFIG_PATH", &path)
        .env("PKG_CONFIG_LIBDIR", &path)
        .output()
        .expect("cannot run pkg-config");
    assert!(
        output.status.success(),
        "pkg-config failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let ours: Vec<&str> = LIBS
        .iter()
        .copied()
        .chain(["dvdnav", "dvdread", "dvdcss", "expat"])
        .collect();
    let mut libs = Vec::new();
    for flag in String::from_utf8(output.stdout).unwrap().split_whitespace() {
        if let Some(lib) = flag.strip_prefix("-l") {
            if !ours.contains(&lib) && !libs.iter().any(|l| l == lib) {
                libs.push(lib.to_string());
            }
        }
    }
    libs
}

/// Copies `ffmpeg` and `ffprobe` to `target/<profile>/`
/// (`OUT_DIR` = `target/<profile>/build/ffmpeg-sys-<hash>/out`).
fn copy_programs(bin: &Path, out: &Path) {
    let profile_dir = out.ancestors().nth(3).unwrap();
    for program in ["ffmpeg", "ffprobe"] {
        let from = bin.join(program);
        let to = profile_dir.join(program);
        fs::copy(&from, &to).unwrap_or_else(|e| {
            panic!("cannot copy {} to {}: {e}", from.display(), to.display())
        });
    }
}

fn run(command: &mut Command, what: &str) {
    let status = command
        .status()
        .unwrap_or_else(|e| panic!("cannot start {what}: {e}"));
    assert!(status.success(), "{what} failed ({status})");
}
