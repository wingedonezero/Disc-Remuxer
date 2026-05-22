//! RAII wrapper around libdvdread's `dvd_file_t` for raw sector reading.
//!
//! A `dvd_file_t` represents one of the addressable streams on a DVD:
//!
//! * `INFO_FILE`        — `VIDEO_TS.IFO` or `VTS_NN_0.IFO`
//! * `INFO_BACKUP_FILE` — `VIDEO_TS.BUP` or `VTS_NN_0.BUP`
//! * `MENU_VOBS`        — `VIDEO_TS.VOB` or `VTS_NN_0.VOB`
//! * `TITLE_VOBS`       — `VTS_NN_1.VOB` … `VTS_NN_9.VOB` concatenated
//!                        into a single logical 2048-byte-block stream
//!
//! Reads via `DVDReadBlocks` automatically decrypt CSS-protected blocks
//! using libdvdcss when needed (no caller action). Per the libdvdread
//! API contract, `DVDReadBlocks` is only valid for VOB domains
//! (`MENU_VOBS` / `TITLE_VOBS`); IFO/BUP files should be read through
//! `ifoOpen` / the IFO API instead.
//!
//! All operations log at appropriate levels and verify their invariants
//! via `disc_core::check`. The intent is that someone reading the log
//! can reconstruct exactly which sectors were touched and whether each
//! read returned the expected count.

use std::marker::PhantomData;

use disc_core::{check_eq, check_in_range, DiscError};
use libdvdread_sys as sys;

use crate::reader::DvdReader;

/// DVD sector size (a fixed 2048 bytes per the DVD-Video spec).
pub const BLOCK_SIZE: u32 = 2048;

/// libdvdread's read domains, in safe-Rust dress.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReadDomain {
    /// `VIDEO_TS.IFO` (when `vts_nr == 0`) or `VTS_NN_0.IFO`.
    InfoFile,
    /// `VIDEO_TS.BUP` (when `vts_nr == 0`) or `VTS_NN_0.BUP`.
    InfoBackup,
    /// `VIDEO_TS.VOB` (when `vts_nr == 0`) or `VTS_NN_0.VOB` — menu video.
    MenuVobs,
    /// `VTS_NN_1.VOB` … `VTS_NN_9.VOB` concatenated as one logical
    /// stream of 2048-byte blocks. This is what we'll read during a
    /// rip — the title's actual A/V content.
    TitleVobs,
}

impl ReadDomain {
    fn as_sys(self) -> sys::dvd_read_domain_t {
        match self {
            Self::InfoFile => sys::dvd_read_domain_t_DVD_READ_INFO_FILE,
            Self::InfoBackup => sys::dvd_read_domain_t_DVD_READ_INFO_BACKUP_FILE,
            Self::MenuVobs => sys::dvd_read_domain_t_DVD_READ_MENU_VOBS,
            Self::TitleVobs => sys::dvd_read_domain_t_DVD_READ_TITLE_VOBS,
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::InfoFile => "INFO_FILE",
            Self::InfoBackup => "INFO_BACKUP",
            Self::MenuVobs => "MENU_VOBS",
            Self::TitleVobs => "TITLE_VOBS",
        }
    }
}

/// RAII handle for an opened `dvd_file_t`. Closes the file on drop.
/// Borrowing tied to the owning `DvdReader` because the file references
/// the reader's UDF / I/O state.
pub struct DvdFile<'r> {
    handle: *mut sys::dvd_file_t,
    vts_nr: u32,
    domain: ReadDomain,
    block_count: u32,
    _parent: PhantomData<&'r DvdReader>,
}

impl<'r> DvdFile<'r> {
    /// Open a file on the DVD given the title set number and domain.
    ///
    /// `vts_nr == 0` means the disc-wide files (VIDEO_TS.{IFO,VOB,BUP});
    /// `1..=99` selects `VTS_NN_*` per the DVD-Video spec.
    pub fn open(
        reader: &'r DvdReader,
        vts_nr: u32,
        domain: ReadDomain,
    ) -> Result<Self, DiscError> {
        log::info!("DVDOpenFile vts={vts_nr} domain={}", domain.as_str());
        // SAFETY: `reader.raw()` is valid for the lifetime `'r`. libdvdread
        // returns NULL on failure (handled immediately below).
        #[allow(clippy::cast_possible_wrap)]
        let handle = unsafe {
            sys::DVDOpenFile(reader.raw(), vts_nr as i32, domain.as_sys())
        };
        if handle.is_null() {
            return Err(DiscError::FileOpenFailed {
                vts_nr,
                domain: domain.as_str(),
                reason: "libdvdread DVDOpenFile() returned NULL".into(),
            });
        }

        // SAFETY: handle is non-null and valid.
        let raw_size = unsafe { sys::DVDFileSize(handle) };
        let block_count = if raw_size < 0 {
            log::error!(
                "DVDFileSize returned {raw_size} for vts={vts_nr} domain={} — closing",
                domain.as_str(),
            );
            // SAFETY: closing a valid handle.
            unsafe { sys::DVDCloseFile(handle) };
            return Err(DiscError::FileOpenFailed {
                vts_nr,
                domain: domain.as_str(),
                reason: format!("DVDFileSize returned {raw_size}"),
            });
        } else {
            u32::try_from(raw_size).unwrap_or(u32::MAX)
        };

        let byte_size = u64::from(block_count) * u64::from(BLOCK_SIZE);
        log::info!(
            "DVDOpenFile ok handle={handle:p} vts={vts_nr} domain={} block_count={block_count} byte_size={byte_size}",
            domain.as_str(),
        );

        // Sanity check: refuse implausibly large file sizes. A DVD-9 holds
        // ~4.3 million blocks (8.5 GiB). Anything over ~10M blocks is
        // either a libdvdread bug or a corrupt IFO.
        check_in_range("DvdFile::open block_count plausible", u64::from(block_count), 10_000_000);

        Ok(Self {
            handle,
            vts_nr,
            domain,
            block_count,
            _parent: PhantomData,
        })
    }

    /// Title set number this file belongs to (1..=99 for VTS files, 0
    /// for the disc-wide VIDEO_TS files).
    #[must_use]
    pub fn vts_nr(&self) -> u32 {
        self.vts_nr
    }

    /// Which file-domain this represents.
    #[must_use]
    pub fn domain(&self) -> ReadDomain {
        self.domain
    }

    /// Size of the file in 2048-byte DVD blocks. For `TITLE_VOBS` this
    /// is the sum across all `VTS_NN_[1-9].VOB` parts.
    #[must_use]
    pub fn block_count(&self) -> u32 {
        self.block_count
    }

    /// Size of the file in bytes (`block_count * 2048`).
    #[must_use]
    pub fn byte_size(&self) -> u64 {
        u64::from(self.block_count) * u64::from(BLOCK_SIZE)
    }

    /// Read `count` 2048-byte blocks starting at block `offset` within
    /// this file. Decryption happens transparently via libdvdcss if the
    /// underlying disc is CSS-protected.
    ///
    /// Returns the bytes actually read (always a multiple of 2048).
    /// Short reads are logged as warnings; a libdvdread return of -1
    /// surfaces as `DiscError::ReadFailed`.
    pub fn read_blocks(&self, offset: u32, count: u32) -> Result<Vec<u8>, DiscError> {
        // Range check up-front.
        let end = offset.saturating_add(count);
        if end > self.block_count {
            log::error!(
                "DVDReadBlocks range error: offset={offset} count={count} (end={end}) exceeds file block_count={}",
                self.block_count,
            );
            return Err(DiscError::ReadOutOfRange {
                offset,
                count,
                total: self.block_count,
            });
        }

        let byte_count = (count as usize) * (BLOCK_SIZE as usize);
        let mut buf = vec![0u8; byte_count];

        log::trace!(
            "DVDReadBlocks vts={} domain={} offset={offset} count={count}",
            self.vts_nr,
            self.domain.as_str(),
        );

        // SAFETY: `handle` is non-null and valid for the lifetime of `self`.
        // `buf` has enough room for `count * 2048` bytes. libdvdread returns
        // the number of blocks actually read, or -1 on error.
        #[allow(clippy::cast_possible_wrap, clippy::cast_possible_truncation)]
        let ret_isize: isize = unsafe {
            sys::DVDReadBlocks(
                self.handle,
                offset as i32,
                count as usize,
                buf.as_mut_ptr(),
            )
        };

        if ret_isize < 0 {
            log::error!(
                "DVDReadBlocks failed: vts={} domain={} offset={offset} count={count} ret={ret_isize}",
                self.vts_nr,
                self.domain.as_str(),
            );
            return Err(DiscError::ReadFailed {
                offset,
                count,
                ret: i32::try_from(ret_isize).unwrap_or(i32::MIN),
            });
        }

        // ret_isize is non-negative here, fits in u32 (it's <= count).
        #[allow(clippy::cast_sign_loss, clippy::cast_possible_truncation)]
        let got = ret_isize as u32;
        check_eq("DVDReadBlocks returned requested count", got, count);

        if got < count {
            log::warn!(
                "DVDReadBlocks short read: requested={count} got={got} — truncating buffer"
            );
            buf.truncate((got as usize) * (BLOCK_SIZE as usize));
        }

        log::debug!(
            "DVDReadBlocks ok vts={} domain={} offset={offset} count={got} bytes={}",
            self.vts_nr,
            self.domain.as_str(),
            buf.len(),
        );

        Ok(buf)
    }
}

impl Drop for DvdFile<'_> {
    fn drop(&mut self) {
        log::debug!(
            "DVDCloseFile handle={:p} vts={} domain={}",
            self.handle,
            self.vts_nr,
            self.domain.as_str(),
        );
        // SAFETY: handle is non-null (verified in `open`) and we hold
        // unique ownership.
        unsafe { sys::DVDCloseFile(self.handle) };
    }
}
