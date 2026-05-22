//! CSS-protection probe.
//!
//! libdvdread uses libdvdcss internally to transparently decrypt scrambled
//! VOBs but doesn't expose whether the disc *is* scrambled. libdvdcss does,
//! via `dvdcss_is_scrambled()` — but that's only reliable on a real DVD
//! block device, because libdvdcss's probe issues DVD ioctls (or reads the
//! disc lead-in). For directory rips and ISO images, libdvdcss falls back
//! to its "assume the worst" default of `b_scrambled = 1` regardless of
//! actual content. We've verified this against ANGEL_S1D1 (cleartext VOBs
//! that libdvdcss falsely reports as scrambled).
//!
//! Strategy:
//!
//! 1. Run the libdvdcss probe. If it's clearly authoritative (no
//!    `last_error`), trust it.
//! 2. Otherwise — directory paths, ISO images, anywhere libdvdcss couldn't
//!    issue real ioctls — fall back to inspecting one of the title VOB
//!    files. CSS encryption is sector-level on title VOBs; if the first
//!    four bytes of a `VTS_NN_1.VOB` are the MPEG-PS pack-start code
//!    `00 00 01 BA` then the sector is plaintext and the disc is not
//!    scrambled. Anything else (high-entropy bytes) means CSS is in play
//!    for that VOB.
//! 3. If neither path can give a confident answer (no readable VOBs,
//!    file open failures), report `Inconclusive`.

use std::ffi::{CStr, CString};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

use disc_core::DiscError;
use libdvdcss_sys as sys;

/// Which mechanism produced the scramble verdict.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProbeMethod {
    /// libdvdcss `dvdcss_test()` succeeded — DVD ioctls were available
    /// (block device path) and gave a definitive answer.
    LibdvdcssIoctl,
    /// libdvdcss could not probe (`last_error` set on the handle).
    /// We read the first 4 bytes of a title VOB and checked for the
    /// MPEG-PS pack-start code `00 00 01 BA`.
    VobMagic,
    /// Neither libdvdcss nor the VOB-magic heuristic could conclude.
    Inconclusive,
}

#[derive(Debug, Clone)]
pub struct CssProbe {
    /// `true` = scrambled, `false` = plaintext. Meaningless when
    /// `method == Inconclusive`.
    pub is_scrambled: bool,
    /// Which mechanism produced `is_scrambled`.
    pub method: ProbeMethod,
    /// libdvdcss's last_error string after the open (often `"read error"`
    /// on directory paths — the signal that ioctls failed).
    pub last_error: Option<String>,
    /// VOB we used for the magic-byte check, if any.
    pub probed_vob: Option<PathBuf>,
    /// The first 4 bytes of `probed_vob`. Zeroed if we didn't read one.
    pub first_bytes: [u8; 4],
    /// The path we probed.
    pub path: PathBuf,
}

const MPEG_PS_PACK_START: [u8; 4] = [0x00, 0x00, 0x01, 0xBA];

impl CssProbe {
    /// Open libdvdcss against `path`, ask for its verdict, and fall back
    /// to inspecting a VOB's first bytes if the libdvdcss probe wasn't
    /// authoritative.
    pub fn open(path: &Path) -> Result<Self, DiscError> {
        let (dvdcss_is_scrambled, last_error) = run_libdvdcss_probe(path)?;

        // If libdvdcss probed cleanly (no last_error) the answer is
        // authoritative — it ran the DVD ioctl test and got a real reply.
        if last_error.is_none() {
            log::info!(
                "css probe: libdvdcss authoritative, is_scrambled={dvdcss_is_scrambled}"
            );
            return Ok(Self {
                is_scrambled: dvdcss_is_scrambled,
                method: ProbeMethod::LibdvdcssIoctl,
                last_error: None,
                probed_vob: None,
                first_bytes: [0; 4],
                path: path.to_path_buf(),
            });
        }

        // libdvdcss failed its probe (typically because `path` is a
        // directory or regular file — no DVD ioctls available). Fall back
        // to reading the first sector header of a VOB.
        log::debug!(
            "css probe: libdvdcss probe inconclusive (last_error={last_error:?}), \
             falling back to VOB-magic heuristic"
        );

        if let Some((vob_path, first_bytes)) = find_and_read_vob_header(path) {
            let scrambled = first_bytes != MPEG_PS_PACK_START;
            log::info!(
                "css probe: VOB-magic verdict, vob={} first_bytes={:02x?} is_scrambled={}",
                vob_path.display(),
                first_bytes,
                scrambled,
            );
            return Ok(Self {
                is_scrambled: scrambled,
                method: ProbeMethod::VobMagic,
                last_error,
                probed_vob: Some(vob_path),
                first_bytes,
                path: path.to_path_buf(),
            });
        }

        log::warn!("css probe: no readable VOBs found at {}", path.display());
        Ok(Self {
            // `is_scrambled = true` here is meaningless — `method` is
            // Inconclusive. We pick `true` only because that's the more
            // conservative assumption: if we can't tell, assume yes.
            is_scrambled: true,
            method: ProbeMethod::Inconclusive,
            last_error,
            probed_vob: None,
            first_bytes: [0; 4],
            path: path.to_path_buf(),
        })
    }
}

/// Call `dvdcss_open` / `dvdcss_is_scrambled` / `dvdcss_error` / close.
fn run_libdvdcss_probe(path: &Path) -> Result<(bool, Option<String>), DiscError> {
    let c_path = cstring_from_path(path).map_err(|()| DiscError::InvalidPath)?;

    log::debug!("dvdcss_open path={}", path.display());
    // SAFETY: `c_path` lives for the call; libdvdcss copies what it needs.
    let handle = unsafe { sys::dvdcss_open(c_path.as_ptr()) };
    if handle.is_null() {
        return Err(DiscError::OpenFailed {
            path: path.to_path_buf(),
            reason: "libdvdcss dvdcss_open() returned NULL".into(),
        });
    }

    // SAFETY: handle is non-null and valid.
    let scrambled = unsafe { sys::dvdcss_is_scrambled(handle) } == 1;

    // SAFETY: dvdcss_error returns a pointer into the handle's owned
    // buffer, valid until close.
    let err_ptr = unsafe { sys::dvdcss_error(handle) };
    let last_error = if err_ptr.is_null() {
        None
    } else {
        let s = unsafe { CStr::from_ptr(err_ptr) }.to_string_lossy().into_owned();
        if s.is_empty() {
            None
        } else {
            Some(s)
        }
    };

    // SAFETY: closing a valid handle.
    unsafe { sys::dvdcss_close(handle) };

    Ok((scrambled, last_error))
}

/// Find the first `VTS_NN_M.VOB` (with `M >= 1`) under `path` and read its
/// first 4 bytes. Returns `None` if there's no such file or it can't be
/// read.
fn find_and_read_vob_header(path: &Path) -> Option<(PathBuf, [u8; 4])> {
    // Only meaningful for directory-style paths. For ISO/block-device
    // paths the libdvdcss path normally gives a real answer; this
    // heuristic doesn't apply.
    if !path.is_dir() {
        return None;
    }

    let candidates = [path.join("VIDEO_TS"), path.to_path_buf()];

    for dir in &candidates {
        let Ok(entries) = fs::read_dir(dir) else {
            continue;
        };
        let mut vob_files: Vec<PathBuf> = entries
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| {
                p.file_name()
                    .and_then(|n| n.to_str())
                    .is_some_and(is_title_vob_filename)
            })
            .collect();
        vob_files.sort();
        for vob in vob_files {
            let Ok(metadata) = fs::metadata(&vob) else {
                continue;
            };
            if metadata.len() < 4 {
                continue;
            }
            let Ok(mut f) = fs::File::open(&vob) else {
                continue;
            };
            let mut buf = [0u8; 4];
            if f.read_exact(&mut buf).is_ok() {
                return Some((vob, buf));
            }
        }
    }

    None
}

/// Match filenames of the form `VTS_<NN>_<M>.VOB` with `M >= 1`. We skip
/// `VTS_NN_0.VOB` because that's the menu VOB — title VOBs are sequence 1+.
fn is_title_vob_filename(name: &str) -> bool {
    // Strip the `VTS_` prefix and `.VOB` suffix, then expect "<NN>_<M>"
    // — exactly one internal underscore separating two digit runs.
    let Some(stem) = name.strip_prefix("VTS_").and_then(|s| s.strip_suffix(".VOB")) else {
        return false;
    };
    let Some((nn, m)) = stem.split_once('_') else {
        return false;
    };
    // Both halves must be non-empty digit strings; the M half must
    // additionally parse to a number >= 1.
    if nn.is_empty() || !nn.chars().all(|c| c.is_ascii_digit()) {
        return false;
    }
    m.parse::<u8>().is_ok_and(|m| m >= 1)
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn title_vob_filename_matcher() {
        assert!(is_title_vob_filename("VTS_01_1.VOB"));
        assert!(is_title_vob_filename("VTS_99_9.VOB"));
        assert!(!is_title_vob_filename("VTS_01_0.VOB")); // menu VOB
        assert!(!is_title_vob_filename("VIDEO_TS.VOB"));
        assert!(!is_title_vob_filename("VTS_01_1.IFO"));
        assert!(!is_title_vob_filename("VTS_01.VOB"));
        assert!(!is_title_vob_filename("not_a_vob.txt"));
    }
}
