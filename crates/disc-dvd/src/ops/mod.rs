//! DVD operations — the orchestration layer.
//!
//! Each operation composes the low-level primitives (`DvdFile`,
//! `IfoHandle`, `DvdNav`, `Demuxer`, the `video_es` / `vobsub` writers,
//! …) into one complete task and returns a structured report. Callers —
//! the CLI today, the facade / PyQt bindings later — only map inputs and
//! present the report; no DVD orchestration lives in the frontend.
//!
//! Convention: one module per operation, each exposing `Params`, `Report`,
//! and `run(reader, params) -> anyhow::Result<Report>`. Disc-opening ops
//! take an already-open [`crate::DvdReader`] (the path is available via
//! `reader.path()` for the ones that also drive libdvdnav); file-level
//! diagnostics take a path directly.

pub mod demux_title;
pub mod demux_title_nav;
pub mod demux_vob;
pub mod dump_sectors;
pub mod dump_title;
pub mod dump_title_nav;
pub mod info;
pub mod rip_title;
pub mod scan_streams;
