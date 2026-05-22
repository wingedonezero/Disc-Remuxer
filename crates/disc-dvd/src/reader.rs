//! RAII wrapper for libdvdread's `dvd_reader_t`.

use std::ffi::CString;
use std::path::{Path, PathBuf};

use disc_core::DiscError;
use libdvdread_sys as sys;

/// An open DVD handle. Holds the libdvdread reader pointer; closes it on
/// drop. Safe to pass `&DvdReader` across threads (the underlying libdvdread
/// reader is documented as thread-safe for read-only operations).
pub struct DvdReader {
    handle: *mut sys::dvd_reader_t,
    path: PathBuf,
}

// libdvdread's `dvd_reader_t` is internally locked for read-only access.
// Wrapping it in `Send + Sync` is conservative but correct for our use.
unsafe impl Send for DvdReader {}
unsafe impl Sync for DvdReader {}

impl DvdReader {
    /// Open a DVD at the given path. The path may be:
    ///
    /// * a directory containing `VIDEO_TS/`,
    /// * an `.iso` image,
    /// * a block device (e.g. `/dev/sr0`).
    ///
    /// libdvdread auto-detects which case applies.
    pub fn open(path: &Path) -> Result<Self, DiscError> {
        let c_path =
            cstring_from_path(path).map_err(|()| DiscError::InvalidPath)?;

        log::info!("DVDOpen path={}", path.display());
        // SAFETY: `c_path` lives for the duration of this call; `DVDOpen`
        // copies any data it needs out of the C string.
        let handle = unsafe { sys::DVDOpen(c_path.as_ptr()) };

        if handle.is_null() {
            return Err(DiscError::OpenFailed {
                path: path.to_path_buf(),
                reason: "libdvdread DVDOpen() returned NULL".into(),
            });
        }

        log::debug!("DVDOpen ok handle={handle:p}");
        Ok(Self {
            handle,
            path: path.to_path_buf(),
        })
    }

    /// The path the reader was opened from.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The raw libdvdread handle. Crate-internal: used by `IfoHandle::open`
    /// to call `ifoOpen(reader, ifo_nr)`.
    pub(crate) fn raw(&self) -> *mut sys::dvd_reader_t {
        self.handle
    }
}

impl Drop for DvdReader {
    fn drop(&mut self) {
        log::debug!("DVDClose handle={:p}", self.handle);
        // SAFETY: `handle` is non-null (checked in `open`) and points to a
        // valid `dvd_reader_t` for the lifetime of `self`.
        unsafe { sys::DVDClose(self.handle) };
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
