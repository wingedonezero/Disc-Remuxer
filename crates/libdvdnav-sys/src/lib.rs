//! Raw FFI bindings to `libdvdnav`. Vendored at `vendor/libdvdnav/` and built
//! at compile time by `build.rs` via meson + ninja against the bundled
//! libdvdread.
//!
//! The bindings cover:
//!
//! * `<dvdnav/dvd_types.h>` — shared DVD type aliases
//! * `<dvdnav/dvdnav_events.h>` — VM event enum + payload structs
//! * `<dvdnav/dvdnav.h>` — `dvdnav_open` / `dvdnav_get_next_block` /
//!   navigation control surface
//!
//! Field names mirror libdvdnav's public C headers verbatim.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals, dead_code)]
#![allow(clippy::missing_safety_doc, clippy::redundant_static_lifetimes)]
#![allow(clippy::pedantic)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

#[cfg(test)]
mod tests {
    use super::*;

    /// Link-check: the test binary linking against libdvdnav.so / libdvdread.so
    /// / libdvdcss.so succeeded if this runs at all.
    #[test]
    fn link_smoke_test() {
        let _ = dvdnav_open as *const ();
        let _ = dvdnav_close as *const ();
        let _ = dvdnav_get_next_block as *const ();
        let _ = dvdnav_err_to_string as *const ();
    }
}
