//! Safe Rust wrapper over `libdvdnav` — DVD-Video navigation VM driver.
//!
//! libdvdnav sits on top of libdvdread and executes the DVD-Video
//! navigation virtual machine: it walks the PGC commands the disc
//! author specified, follows JumpPGCN / LinkPGCN / CallSS / etc., picks
//! the correct cell at each step, and hands the caller back one
//! 2048-byte sector at a time (or a navigation event).
//!
//! Why we want it for ripping:
//!
//! 1. **Authored playback path.** A simple `cell_playback[0..]` walk in
//!    IFO order works for plain titles but skips logic the disc author
//!    encoded. libdvdnav follows the actual playback chain.
//!
//! 2. **Structural-protection bypass.** RipGuard, ARccOS, and Sony
//!    fake-sector schemes work by adding decoy cells / trap titles
//!    that aren't in the authored playback path. A naive ripper walks
//!    everything and stumbles; libdvdnav follows the author's intent
//!    and the protection is invisible.
//!
//! 3. **Multi-angle and branching titles.** libdvdnav handles
//!    interleaved-VOB-unit (ILVU) blocks correctly, picking the
//!    currently-selected angle's bytes.
//!
//! ## API shape
//!
//! [`DvdNav::open`] wraps `dvdnav_open` (RAII). Configuration setters
//! mirror libdvdnav's: [`DvdNav::set_readahead`] disables libdvdnav's
//! internal read-ahead (we want exact sector boundaries),
//! [`DvdNav::set_pgc_positioning`] expresses position queries in PGC
//! time rather than VTS time.
//!
//! [`DvdNav::title_play`] starts playback at the given title number
//! (1-based, matching `dvdnav_get_number_of_titles`).
//!
//! [`DvdNav::next_block`] is the inner-loop call — fills a shared
//! 2048-byte buffer (owned by `DvdNav`) and returns a [`NavEvent`]
//! describing what kind of block it is. The caller dispatches on the
//! variant and consumes the buffer for [`NavEvent::Block`] only.
//!
//! Still-frame and wait events have explicit "skip" methods
//! ([`DvdNav::still_skip`], [`DvdNav::wait_skip`]) — callers ripping
//! data want to advance immediately past these.
//!
//! Note on libdvdnav title numbering: it's the **playable-title count**
//! (excludes the first-play PGC and menus). For the test discs we've
//! used so far this lines up with libdvdread's `tt_srpt` numbering,
//! but in general the two can differ for discs that use first-play /
//! VMG-menu PGCs as separate "titles." The caller should map between
//! schemes themselves if needed.

use std::ffi::CString;
use std::path::Path;

use disc_core::DiscError;
use libdvdnav_sys as sys;

/// One of libdvdnav's `DVDNAV_*` events, with the payload decoded.
///
/// [`NavEvent::Block`] is the only variant that carries the
/// sector-sized buffer borrowed from the parent [`DvdNav`]; the
/// lifetime ensures we can't keep the buffer past the next
/// [`DvdNav::next_block`] call (which would overwrite it).
#[derive(Debug)]
pub enum NavEvent<'b> {
    /// A 2048-byte sector of MPEG-PS data. This is what the demuxer
    /// consumes.
    Block { sector: &'b [u8] },
    /// `DVDNAV_NOP` — no-op padding event. Skip.
    Nop,
    /// `DVDNAV_STILL_FRAME` — the disc author asked for a still pause.
    /// `length` is seconds; `0xFF` means infinite. For ripping, the
    /// caller should immediately call [`DvdNav::still_skip`] to advance.
    StillFrame { length: u8 },
    /// `DVDNAV_SPU_STREAM_CHANGE` — the disc author switched the
    /// active subpicture stream(s). We don't act on this during
    /// ripping (we just keep emitting every stream's bytes), but we
    /// surface the event so the caller can log it.
    SpuStreamChange,
    /// `DVDNAV_AUDIO_STREAM_CHANGE` — the disc author switched the
    /// active audio stream. Same treatment as `SpuStreamChange`.
    AudioStreamChange,
    /// `DVDNAV_VTS_CHANGE` — playback crossed into a different VTS.
    /// Relevant because cell-info lookups (for stc_discontinuity) need
    /// to consult the new VTS's IFO.
    VtsChange,
    /// `DVDNAV_CELL_CHANGE` — playback moved to a new cell. Payload
    /// from [`sys::dvdnav_cell_change_event_t`].
    CellChange {
        cell_nr: i32,
        program_nr: i32,
        cell_length_pts: i64,
        program_length_pts: i64,
        pgc_length_pts: i64,
        cell_start_pts: i64,
        program_start_pts: i64,
    },
    /// `DVDNAV_NAV_PACKET` — a navigation pack (PCI + DSI) has just
    /// been read. The previous `Block` event carried the NV_PCK
    /// sector; this is the post-pack notification.
    NavPacket,
    /// `DVDNAV_STOP` — end of the disc / end of the requested title.
    /// The caller should break out of the loop.
    Stop,
    /// `DVDNAV_HIGHLIGHT` — menu button highlight changed. Irrelevant
    /// for ripping.
    Highlight,
    /// `DVDNAV_SPU_CLUT_CHANGE` — subpicture palette change. The 16
    /// palette colors live in `buf[..64]` (4 bytes each, YCrCb).
    SpuClutChange,
    /// `DVDNAV_HOP_CHANNEL` — the VM jumped (e.g. due to a menu
    /// selection). Marks a discontinuity in playback state.
    HopChannel,
    /// `DVDNAV_WAIT` — libdvdnav wants the caller to acknowledge by
    /// calling [`DvdNav::wait_skip`] before continuing.
    Wait,
    /// Any event code we don't have an explicit variant for. Carries
    /// the raw `DVDNAV_*` value for diagnostics.
    Other(i32),
}

/// RAII wrapper for `dvdnav_t`.
///
/// Owns:
///
/// * The libdvdnav handle (closed on drop).
/// * A 2048-byte sector buffer used by every [`Self::next_block`] call.
///   Sharing the buffer across calls means we don't allocate per
///   sector.
pub struct DvdNav {
    handle: *mut sys::dvdnav_t,
    buf: Box<[u8; 2048]>,
}

// SAFETY: `dvdnav_t` is per-instance; libdvdnav documents the handle
// as not thread-safe but safe to move across threads. We mark `Send`
// to allow moving but not `Sync` (concurrent access would race).
unsafe impl Send for DvdNav {}

impl DvdNav {
    /// Open a DVD via libdvdnav. `path` may be a directory containing
    /// `VIDEO_TS/`, an ISO image, or a block device — same as
    /// [`crate::DvdReader::open`].
    pub fn open(path: &Path) -> Result<Self, DiscError> {
        let c_path = cstring_from_path(path).map_err(|()| DiscError::InvalidPath)?;
        let mut handle: *mut sys::dvdnav_t = std::ptr::null_mut();
        log::info!("dvdnav_open path={}", path.display());
        // SAFETY: `c_path` is valid for the duration of this call;
        // libdvdnav returns DVDNAV_STATUS_OK on success and writes the
        // handle pointer through `&mut handle`.
        let status = unsafe { sys::dvdnav_open(&mut handle, c_path.as_ptr()) };
        if status != sys::DVDNAV_STATUS_OK as i32 || handle.is_null() {
            return Err(DiscError::OpenFailed {
                path: path.to_path_buf(),
                reason: format!("libdvdnav dvdnav_open() returned status={status}"),
            });
        }
        log::debug!("dvdnav_open ok handle={handle:p}");
        Ok(Self {
            handle,
            buf: Box::new([0u8; 2048]),
        })
    }

    /// Disable / enable libdvdnav's read-ahead cache. We want it OFF
    /// for ripping — read-ahead can re-order or skip blocks in ways
    /// that aren't byte-exact.
    pub fn set_readahead(&mut self, on: bool) -> Result<(), DiscError> {
        let flag = i32::from(on);
        // SAFETY: handle is valid for the lifetime of `self`.
        let status = unsafe { sys::dvdnav_set_readahead_flag(self.handle, flag) };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::OpenFailed {
                path: std::path::PathBuf::new(),
                reason: format!("dvdnav_set_readahead_flag returned {status}"),
            });
        }
        Ok(())
    }

    /// Configure libdvdnav to express position queries in PGC time
    /// (when `on == true`) rather than VTS-relative time. We enable
    /// this because our demuxer logs use PGC-relative offsets.
    pub fn set_pgc_positioning(&mut self, on: bool) -> Result<(), DiscError> {
        let flag = i32::from(on);
        // SAFETY: handle is valid for the lifetime of `self`.
        let status = unsafe { sys::dvdnav_set_PGC_positioning_flag(self.handle, flag) };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::OpenFailed {
                path: std::path::PathBuf::new(),
                reason: format!(
                    "dvdnav_set_PGC_positioning_flag returned {status}"
                ),
            });
        }
        Ok(())
    }

    /// Number of playable titles as libdvdnav sees them. Equivalent to
    /// `dvdnav_get_number_of_titles`.
    pub fn num_titles(&self) -> Result<i32, DiscError> {
        let mut n: i32 = 0;
        // SAFETY: handle is valid for the lifetime of `self`.
        let status = unsafe { sys::dvdnav_get_number_of_titles(self.handle, &mut n) };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::OpenFailed {
                path: std::path::PathBuf::new(),
                reason: format!("dvdnav_get_number_of_titles returned {status}"),
            });
        }
        Ok(n)
    }

    /// Number of chapters/parts in `title` (1-based per libdvdnav).
    pub fn num_parts(&self, title: i32) -> Result<i32, DiscError> {
        let mut n: i32 = 0;
        // SAFETY: handle is valid; `title` is bounds-checked by libdvdnav.
        let status =
            unsafe { sys::dvdnav_get_number_of_parts(self.handle, title, &mut n) };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::OpenFailed {
                path: std::path::PathBuf::new(),
                reason: format!(
                    "dvdnav_get_number_of_parts({title}) returned {status}"
                ),
            });
        }
        Ok(n)
    }

    /// Start playback at title `title` (1-based per libdvdnav's
    /// numbering, NOT necessarily the same as libdvdread's `tt_srpt`
    /// index).
    pub fn title_play(&mut self, title: i32) -> Result<(), DiscError> {
        log::info!("dvdnav_title_play({title})");
        // SAFETY: handle is valid; libdvdnav bounds-checks `title`.
        let status = unsafe { sys::dvdnav_title_play(self.handle, title) };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::OpenFailed {
                path: std::path::PathBuf::new(),
                reason: format!("dvdnav_title_play({title}) returned {status}"),
            });
        }
        Ok(())
    }

    /// Query the current `(title, part)` libdvdnav is playing back.
    /// Returns `(title, part)`, both 1-based. Title `0` means the
    /// first-play PGC (intro / menus); the caller should treat that
    /// as "not in a content title yet."
    pub fn current_title_part(&self) -> Result<(i32, i32), DiscError> {
        let mut title: i32 = 0;
        let mut part: i32 = 0;
        // SAFETY: handle is valid; both out-pointers are non-null.
        let status = unsafe {
            sys::dvdnav_current_title_info(self.handle, &mut title, &mut part)
        };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::OpenFailed {
                path: std::path::PathBuf::new(),
                reason: format!("dvdnav_current_title_info returned {status}"),
            });
        }
        Ok((title, part))
    }

    /// Query the current `(title, pgcn, pgn)` libdvdnav is playing
    /// back — all 1-based. The PGC number is what we need to look up
    /// cell metadata via [`crate::CellLookup`].
    pub fn current_title_program(&self) -> Result<(i32, i32, i32), DiscError> {
        let mut title: i32 = 0;
        let mut pgcn: i32 = 0;
        let mut pgn: i32 = 0;
        // SAFETY: handle is valid; all three out-pointers are non-null.
        let status = unsafe {
            sys::dvdnav_current_title_program(
                self.handle, &mut title, &mut pgcn, &mut pgn,
            )
        };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::OpenFailed {
                path: std::path::PathBuf::new(),
                reason: format!(
                    "dvdnav_current_title_program returned {status}"
                ),
            });
        }
        Ok((title, pgcn, pgn))
    }

    /// Drive one step of the navigation VM.
    ///
    /// Fills the shared 2048-byte buffer with either sector data (for
    /// `BLOCK_OK`) or the event payload struct (for other event
    /// codes), then returns a [`NavEvent`] describing what was emitted.
    pub fn next_block(&mut self) -> Result<NavEvent<'_>, DiscError> {
        let mut event: i32 = 0;
        let mut len: i32 = 0;
        // SAFETY: `self.buf` is 2048 bytes; libdvdnav writes up to
        // `len` bytes (`len <= 2048` for all event types).
        let status = unsafe {
            sys::dvdnav_get_next_block(
                self.handle,
                self.buf.as_mut_ptr(),
                &mut event,
                &mut len,
            )
        };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            // Fetch libdvdnav's error string for diagnostics.
            let err_str = self.last_error();
            return Err(DiscError::ReadFailed {
                offset: 0,
                count: 1,
                ret: status,
            }
            .also_context(format!("dvdnav_get_next_block: {err_str}")));
        }

        Ok(match event as u32 {
            sys::DVDNAV_BLOCK_OK => NavEvent::Block {
                sector: &self.buf[..len as usize],
            },
            sys::DVDNAV_NOP => NavEvent::Nop,
            sys::DVDNAV_STILL_FRAME => {
                // payload = dvdnav_still_event_t { length: c_int }
                // SAFETY: buffer is sized large enough; struct alignment matches.
                let evt = unsafe {
                    &*(self.buf.as_ptr().cast::<sys::dvdnav_still_event_t>())
                };
                let raw_len = { evt.length };
                let length = u8::try_from(raw_len.clamp(0, 255)).unwrap_or(0);
                NavEvent::StillFrame { length }
            }
            sys::DVDNAV_SPU_STREAM_CHANGE => NavEvent::SpuStreamChange,
            sys::DVDNAV_AUDIO_STREAM_CHANGE => NavEvent::AudioStreamChange,
            sys::DVDNAV_VTS_CHANGE => NavEvent::VtsChange,
            sys::DVDNAV_CELL_CHANGE => {
                // SAFETY: buffer is sized large enough; struct alignment matches.
                let evt = unsafe {
                    &*(self.buf.as_ptr().cast::<sys::dvdnav_cell_change_event_t>())
                };
                NavEvent::CellChange {
                    cell_nr: { evt.cellN },
                    program_nr: { evt.pgN },
                    cell_length_pts: { evt.cell_length },
                    program_length_pts: { evt.pg_length },
                    pgc_length_pts: { evt.pgc_length },
                    cell_start_pts: { evt.cell_start },
                    program_start_pts: { evt.pg_start },
                }
            }
            sys::DVDNAV_NAV_PACKET => NavEvent::NavPacket,
            sys::DVDNAV_STOP => NavEvent::Stop,
            sys::DVDNAV_HIGHLIGHT => NavEvent::Highlight,
            sys::DVDNAV_SPU_CLUT_CHANGE => NavEvent::SpuClutChange,
            sys::DVDNAV_HOP_CHANNEL => NavEvent::HopChannel,
            sys::DVDNAV_WAIT => NavEvent::Wait,
            other => NavEvent::Other(other as i32),
        })
    }

    /// Read the current VOBU's start/end presentation times from the
    /// NAV-pack PCI (`pci_gi.vobu_s_ptm` / `vobu_e_ptm`, 90 kHz ticks).
    /// Valid immediately after a [`NavEvent::NavPacket`]; returns `None`
    /// when no PCI is currently available. The DVD clock resets per cell
    /// (DVD-Video Part 3, NV_PCK), so these times drive the title-relative
    /// timeline reconstruction across `stc_discontinuity`.
    #[must_use]
    pub fn current_vobu_ptm(&self) -> Option<(u32, u32)> {
        // SAFETY: handle is valid for `self`'s lifetime; the returned
        // pci_t is owned by libdvdnav and valid until the next
        // `next_block`. We only read scalar fields out of it here.
        let pci = unsafe { sys::dvdnav_get_current_nav_pci(self.handle) };
        if pci.is_null() {
            return None;
        }
        let gi = unsafe { (*pci).pci_gi };
        Some((gi.vobu_s_ptm, gi.vobu_e_ptm))
    }

    /// Acknowledge a `StillFrame` event and resume — without this
    /// libdvdnav stalls indefinitely on still cells.
    pub fn still_skip(&mut self) -> Result<(), DiscError> {
        // SAFETY: handle is valid.
        let status = unsafe { sys::dvdnav_still_skip(self.handle) };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::ReadFailed {
                offset: 0,
                count: 0,
                ret: status,
            });
        }
        Ok(())
    }

    /// Acknowledge a `Wait` event and resume.
    pub fn wait_skip(&mut self) -> Result<(), DiscError> {
        // SAFETY: handle is valid.
        let status = unsafe { sys::dvdnav_wait_skip(self.handle) };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::ReadFailed {
                offset: 0,
                count: 0,
                ret: status,
            });
        }
        Ok(())
    }

    /// Stop navigation. Optional — `Drop` will close the handle either
    /// way — but calling this explicitly lets libdvdnav clean up
    /// internal state before close.
    pub fn stop(&mut self) -> Result<(), DiscError> {
        // SAFETY: handle is valid.
        let status = unsafe { sys::dvdnav_stop(self.handle) };
        if status != sys::DVDNAV_STATUS_OK as i32 {
            return Err(DiscError::ReadFailed {
                offset: 0,
                count: 0,
                ret: status,
            });
        }
        Ok(())
    }

    /// libdvdnav's most-recent error string (UTF-8 lossy).
    pub fn last_error(&self) -> String {
        // SAFETY: handle is valid; the returned pointer is owned by
        // libdvdnav and remains valid until the next dvdnav call.
        let p = unsafe { sys::dvdnav_err_to_string(self.handle) };
        if p.is_null() {
            return "(no error)".into();
        }
        // SAFETY: pointer is non-null and points to a NUL-terminated
        // C string.
        let cstr = unsafe { std::ffi::CStr::from_ptr(p) };
        cstr.to_string_lossy().into_owned()
    }
}

impl Drop for DvdNav {
    fn drop(&mut self) {
        log::debug!("dvdnav_close handle={:p}", self.handle);
        // SAFETY: handle is valid; `dvdnav_close` cleans up. We
        // intentionally ignore its return value — there's nothing
        // useful to do on close failure from a destructor.
        let _ = unsafe { sys::dvdnav_close(self.handle) };
    }
}

#[cfg(unix)]
fn cstring_from_path(path: &Path) -> Result<CString, ()> {
    use std::os::unix::ffi::OsStrExt;
    CString::new(path.as_os_str().as_bytes()).map_err(|_| ())
}

#[cfg(not(unix))]
fn cstring_from_path(path: &Path) -> Result<CString, ()> {
    let s = path.to_str().ok_or(())?;
    CString::new(s).map_err(|_| ())
}

/// Small ergonomic extension to attach an additional context string to
/// an existing `DiscError`. Kept private to this module.
trait ErrorContextExt {
    fn also_context(self, ctx: String) -> Self;
}
impl ErrorContextExt for DiscError {
    fn also_context(self, _ctx: String) -> Self {
        // DiscError variants already carry their own structured fields;
        // we don't have a generic context attachment point. Return self
        // as-is. Future work could add a Wrapped { inner, ctx } variant.
        self
    }
}
