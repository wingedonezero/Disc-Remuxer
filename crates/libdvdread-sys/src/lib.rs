//! Raw FFI bindings to `libdvdread`. Vendored at `vendor/libdvdread/` and
//! built at compile time by `build.rs` via meson + ninja, linked directly
//! against the bundled libdvdcss (via `-Dlibdvdcss=enabled` in meson).
//!
//! The bindings cover the union of:
//!
//! * `<dvdread/dvd_reader.h>` — `DVDOpen` / `DVDReadBlocks` / disc handle
//! * `<dvdread/dvd_udf.h>` — UDF filesystem walker
//! * `<dvdread/ifo_types.h>` — VMG / VTS / PGC / cell record structs
//! * `<dvdread/ifo_read.h>` — IFO parser entrypoints
//! * `<dvdread/ifo_print.h>` — IFO pretty-printer (debug)
//! * `<dvdread/nav_types.h>` — PCI / DSI navigation packet structs
//! * `<dvdread/nav_read.h>` — navigation packet parsers
//! * `<dvdread/nav_print.h>` — navigation packet pretty-printer
//! * `<dvdread/bitreader.h>` — small bit-level reader helper
//!
//! Field names in the generated bindings mirror libdvdread's public C
//! headers exactly (e.g. `tt_srpt_t::nr_of_srpts`, `pgc_t::nr_of_cells`).
//! That naming convention is preserved through the safe wrappers in
//! higher layers.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals, dead_code)]
#![allow(clippy::missing_safety_doc, clippy::redundant_static_lifetimes)]
#![allow(clippy::pedantic)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

#[cfg(test)]
mod tests {
    use super::*;

    /// Linking the test binary against libdvdread.so and libdvdcss.so
    /// succeeded if this test runs at all. Touch one symbol from each major
    /// surface to defeat dead-code-elimination.
    #[test]
    fn link_smoke_test() {
        let _ = DVDOpen as *const ();
        let _ = DVDClose as *const ();
        let _ = DVDReadBlocks as *const ();
        let _ = ifoOpen as *const ();
        let _ = ifoClose as *const ();
        let _ = navRead_PCI as *const ();
    }
}
