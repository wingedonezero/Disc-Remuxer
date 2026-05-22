//! Raw FFI bindings to `libdvdcss`. Vendored at `vendor/libdvdcss/` and built
//! at compile time by `build.rs` via meson + ninja.
//!
//! Bindings are regenerated from `<dvdcss/dvdcss.h>` on every build. All
//! symbols matching `dvdcss_*` / `DVDCSS_*` are exposed.
//!
//! This crate is `unsafe` to use directly; safe wrappers belong in higher
//! layers (see `disc-dvd`).

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals, dead_code)]
#![allow(clippy::missing_safety_doc, clippy::redundant_static_lifetimes)]
#![allow(clippy::pedantic)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

#[cfg(test)]
mod tests {
    use super::*;

    /// The test binary linking successfully implies libdvdcss.so was found
    /// at link time and rustc accepted the bindgen output. Touch one symbol
    /// from each major API surface to defeat dead-code-elimination.
    #[test]
    fn link_smoke_test() {
        let _ = dvdcss_open as *const ();
        let _ = dvdcss_close as *const ();
        let _ = dvdcss_read as *const ();
        let _ = dvdcss_seek as *const ();
        assert_eq!(DVDCSS_BLOCK_SIZE, 2048);
    }
}
