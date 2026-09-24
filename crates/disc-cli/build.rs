//! Bake absolute-path rpaths into the produced `disc-remuxer` binary.
//!
//! The three vendored libraries (libdvdread / libdvdcss / libdvdnav) are
//! built by their `-sys` crates into `target/<profile>/build/.../out/install/lib/`.
//! Without this build script, the produced binary's RUNPATH is just
//! `$ORIGIN` (from `.cargo/config.toml`), and since the `.so` files do NOT
//! live in `target/<profile>/`, the dynamic linker falls back to
//! `/etc/ld.so.cache` — picking up whatever older versions the host system
//! has installed in `/usr/lib`. That's a silent ABI hazard (our headers
//! came from libdvdread 7.0.1; system libdvdread might be 6.x).
//!
//! The fix is to add each vendored install-lib dir to the binary's RUNPATH
//! via a `-Wl,-rpath,<abs>` link arg. The dynamic linker tries each rpath
//! entry in order; with our vendored absolute paths listed alongside
//! `$ORIGIN`, dev builds load our libs, and distribution builds (where the
//! .so files are copied next to the binary) still work via `$ORIGIN`.

use std::env;

fn main() {
    // These env vars are set by Cargo because libdv{cd,read,nav}-sys all
    // emit `cargo:lib_dir=…` from their own build scripts, and they each
    // carry a `links = "…"` key in their Cargo.toml. The convention is
    // documented at <https://doc.rust-lang.org/cargo/reference/build-script-examples.html#using-another-sys-crate>.
    for var in &[
        "DEP_DVDREAD_LIB_DIR",
        "DEP_DVDCSS_LIB_DIR",
        "DEP_DVDNAV_LIB_DIR",
    ] {
        let dir = env::var(var)
            .unwrap_or_else(|_| panic!("expected env var `{var}` from the corresponding -sys crate's build.rs"));
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        println!("cargo:rerun-if-env-changed={var}");
    }

    // Use DT_RPATH (older, deprecated tag) instead of DT_RUNPATH so the
    // baked rpaths get searched for *transitive* shared-lib dependencies,
    // not just the binary's direct DT_NEEDEDs. Without this, libdvdread.so
    // would pull libdvdcss.so from /usr/lib (host system) instead of from
    // our libdvdcss-sys vendored install dir, because the binary's RUNPATH
    // is only consulted for its own direct deps.
    //
    // ld(1):
    //   --enable-new-dtags / --disable-new-dtags
    //     Sets DT_RUNPATH (new) or DT_RPATH (old). DT_RPATH is searched for
    //     transitive lookups; DT_RUNPATH only for direct ones.
    println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");

    println!("cargo:rerun-if-changed=build.rs");
}
