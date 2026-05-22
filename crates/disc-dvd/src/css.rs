//! CSS-protection probe.
//!
//! libdvdread does not expose CSS state through its public API — it just
//! uses libdvdcss internally to decrypt sectors transparently. To answer
//! "is this disc scrambled?" we have to probe directly.
//!
//! Our approach dispatches by path type:
//!
//! * **Block device** (`/dev/sr0`, `/dev/dvd`, …): trust
//!   `dvdcss_is_scrambled()`. libdvdcss issues DVD ioctls — the
//!   standard SCSI MMC authentication probe. The disc returns sense
//!   codes such as `READ OF SCRAMBLED SECTOR WITHOUT AUTHENTICATION`
//!   or the `COPY PROTECTION KEY EXCHANGE FAILURE` family if it's
//!   CSS-protected and we haven't authenticated yet.
//!
//! * **ISO file** (`*.iso`, `*.img`): libdvdcss can't probe (no ioctls
//!   on regular files; its `b_scrambled` stays at the "assume the
//!   worst" default), so we use libdvdread's `UDFFindFile` to locate
//!   `/VIDEO_TS/VTS_01_1.VOB`, then read four raw bytes from the file
//!   at that block offset (bypassing libdvdread's decryption layer).
//!   Cleartext VOB sectors start with the MPEG-PS pack-start code
//!   `00 00 01 BA`; anything else means CSS is in play.
//!
//! * **Directory rip** (`VIDEO_TS/`): read `VTS_NN_1.VOB` directly and
//!   apply the same MPEG-PS magic check.
//!
//! * **Anything else**: report `Inconclusive`.
//!
//! Note that libdvdread does NOT decrypt UDFFindFile's *return value*
//! — that's just a file-location lookup. Our raw `read_at` of the
//! resulting block is what gets ciphertext or plaintext, depending on
//! whether the ISO was dumped with CSS keys applied or not.

use std::ffi::{CStr, CString};
use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use disc_core::DiscError;
use libdvdcss_sys as css_sys;
use libdvdread_sys as read_sys;

const DVD_BLOCK_SIZE: u64 = 2048;
const MPEG_PS_PACK_START: [u8; 4] = [0x00, 0x00, 0x01, 0xBA];

/// Which mechanism produced the scramble verdict.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProbeMethod {
    /// Block device path. libdvdcss `dvdcss_test()` ran DVD ioctls and
    /// gave a definitive answer (effectively a SCSI sense probe).
    LibdvdcssIoctl,
    /// ISO image. We located `/VIDEO_TS/VTS_01_1.VOB` via libdvdread's
    /// UDF reader, then read the first 4 bytes from that block offset
    /// in the file directly (no libdvdread decryption layer).
    IsoUdfSector,
    /// Directory rip. We read the first 4 bytes of `VTS_NN_1.VOB`
    /// directly via `std::fs`.
    VobFile,
    /// Couldn't determine via any path.
    Inconclusive,
}

#[derive(Debug, Clone)]
pub struct CssProbe {
    /// `true` = scrambled, `false` = plaintext. Meaningless when
    /// `method == Inconclusive`.
    pub is_scrambled: bool,
    /// Which mechanism produced `is_scrambled`.
    pub method: ProbeMethod,
    /// libdvdcss's last error string after the open. Often `"no error"`.
    /// We surface this for transparency but it's only authoritative for
    /// the `LibdvdcssIoctl` path.
    pub last_error: Option<String>,
    /// Where we sampled bytes from (file path or `{iso}@sector=N`).
    pub probed_location: Option<String>,
    /// The first 4 bytes of the sampled location. Zeroed if we didn't
    /// read one.
    pub first_bytes: [u8; 4],
    /// The path we probed.
    pub path: PathBuf,
}

impl CssProbe {
    /// Dispatch by path type and run the appropriate probe.
    pub fn open(path: &Path) -> Result<Self, DiscError> {
        let path_kind = classify_path(path);
        log::debug!("css probe: path={} kind={path_kind:?}", path.display());

        // We always run the libdvdcss open for the `last_error` string —
        // it's useful diagnostic info even when we don't trust the verdict.
        let (libdvdcss_scrambled, libdvdcss_error) = run_libdvdcss_probe(path)?;

        match path_kind {
            PathKind::BlockDevice => {
                log::info!(
                    "css probe: block device → trusting libdvdcss-ioctl, is_scrambled={libdvdcss_scrambled}"
                );
                Ok(Self {
                    is_scrambled: libdvdcss_scrambled,
                    method: ProbeMethod::LibdvdcssIoctl,
                    last_error: libdvdcss_error,
                    probed_location: None,
                    first_bytes: [0; 4],
                    path: path.to_path_buf(),
                })
            }
            PathKind::IsoFile => match probe_iso_via_udf(path) {
                Some((sector, first_bytes)) => {
                    let scrambled = first_bytes != MPEG_PS_PACK_START;
                    log::info!(
                        "css probe: ISO/UDF verdict, sector={sector} first_bytes={first_bytes:02x?} is_scrambled={scrambled}"
                    );
                    Ok(Self {
                        is_scrambled: scrambled,
                        method: ProbeMethod::IsoUdfSector,
                        last_error: libdvdcss_error,
                        probed_location: Some(format!(
                            "{}@sector={sector}",
                            path.display()
                        )),
                        first_bytes,
                        path: path.to_path_buf(),
                    })
                }
                None => Ok(inconclusive(path, libdvdcss_error)),
            },
            PathKind::Directory => match find_and_read_vob_header(path) {
                Some((vob_path, first_bytes)) => {
                    let scrambled = first_bytes != MPEG_PS_PACK_START;
                    log::info!(
                        "css probe: VOB-file verdict, vob={} first_bytes={first_bytes:02x?} is_scrambled={scrambled}",
                        vob_path.display(),
                    );
                    Ok(Self {
                        is_scrambled: scrambled,
                        method: ProbeMethod::VobFile,
                        last_error: libdvdcss_error,
                        probed_location: Some(vob_path.display().to_string()),
                        first_bytes,
                        path: path.to_path_buf(),
                    })
                }
                None => Ok(inconclusive(path, libdvdcss_error)),
            },
            PathKind::Other => Ok(inconclusive(path, libdvdcss_error)),
        }
    }
}

fn inconclusive(path: &Path, last_error: Option<String>) -> CssProbe {
    log::warn!("css probe: inconclusive for {}", path.display());
    CssProbe {
        is_scrambled: true, // "assume the worst" — meaningless when method = Inconclusive
        method: ProbeMethod::Inconclusive,
        last_error,
        probed_location: None,
        first_bytes: [0; 4],
        path: path.to_path_buf(),
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PathKind {
    BlockDevice,
    IsoFile,
    Directory,
    Other,
}

fn classify_path(path: &Path) -> PathKind {
    let Ok(meta) = fs::metadata(path) else {
        return PathKind::Other;
    };

    #[cfg(unix)]
    {
        use std::os::unix::fs::FileTypeExt;
        let ft = meta.file_type();
        if ft.is_block_device() || ft.is_char_device() {
            return PathKind::BlockDevice;
        }
    }

    if meta.is_dir() {
        return PathKind::Directory;
    }
    if meta.is_file() {
        return PathKind::IsoFile;
    }
    PathKind::Other
}

// -- libdvdcss probe (only authoritative on block devices) ------------------

fn run_libdvdcss_probe(path: &Path) -> Result<(bool, Option<String>), DiscError> {
    let c_path = cstring_from_path(path).map_err(|()| DiscError::InvalidPath)?;

    log::debug!("dvdcss_open path={}", path.display());
    // SAFETY: `c_path` lives for the call; libdvdcss copies what it needs.
    let handle = unsafe { css_sys::dvdcss_open(c_path.as_ptr()) };
    if handle.is_null() {
        return Err(DiscError::OpenFailed {
            path: path.to_path_buf(),
            reason: "libdvdcss dvdcss_open() returned NULL".into(),
        });
    }

    // SAFETY: handle is non-null and valid.
    let scrambled = unsafe { css_sys::dvdcss_is_scrambled(handle) } == 1;

    // SAFETY: dvdcss_error returns a pointer into the handle's owned buffer,
    // valid until close. The initial value is `"no error"` per libdvdcss
    // source — we treat that as "no error condition".
    let err_ptr = unsafe { css_sys::dvdcss_error(handle) };
    let last_error = if err_ptr.is_null() {
        None
    } else {
        let s = unsafe { CStr::from_ptr(err_ptr) }
            .to_string_lossy()
            .into_owned();
        match s.as_str() {
            "" | "no error" => None,
            _ => Some(s),
        }
    };

    // SAFETY: closing a valid handle.
    unsafe { css_sys::dvdcss_close(handle) };

    Ok((scrambled, last_error))
}

// -- Directory path: read VTS_NN_1.VOB directly via std::fs -----------------

fn find_and_read_vob_header(path: &Path) -> Option<(PathBuf, [u8; 4])> {
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
    let Some(stem) = name.strip_prefix("VTS_").and_then(|s| s.strip_suffix(".VOB")) else {
        return false;
    };
    let Some((nn, m)) = stem.split_once('_') else {
        return false;
    };
    if nn.is_empty() || !nn.chars().all(|c| c.is_ascii_digit()) {
        return false;
    }
    m.parse::<u8>().is_ok_and(|m| m >= 1)
}

// -- ISO file: use libdvdread to locate VTS_01_1.VOB, then read raw bytes ---

/// For an ISO image, open it with libdvdread, ask UDF where
/// `/VIDEO_TS/VTS_01_1.VOB` lives, close libdvdread, then read four raw
/// bytes from that block offset directly. Returns `(sector, bytes)`.
fn probe_iso_via_udf(path: &Path) -> Option<(u32, [u8; 4])> {
    let sector = lookup_vob_start_sector(path)?;
    if sector == 0 {
        return None;
    }

    let mut f = fs::File::open(path).ok()?;
    let byte_offset = u64::from(sector) * DVD_BLOCK_SIZE;
    if f.seek(SeekFrom::Start(byte_offset)).is_err() {
        return None;
    }
    let mut buf = [0u8; 4];
    if f.read_exact(&mut buf).is_err() {
        return None;
    }
    Some((sector, buf))
}

/// Try a small set of VOB paths via libdvdread's `UDFFindFile`, returning
/// the first one that exists. Returns the starting LBA (block number) on
/// the disc.
fn lookup_vob_start_sector(path: &Path) -> Option<u32> {
    let c_path = cstring_from_path(path).ok()?;

    log::debug!("DVDOpen path={} (UDF lookup)", path.display());
    // SAFETY: `c_path` lives for the call.
    let reader = unsafe { read_sys::DVDOpen(c_path.as_ptr()) };
    if reader.is_null() {
        return None;
    }

    // Per libdvdread convention the title VOBs live at
    // `/VIDEO_TS/VTS_NN_M.VOB` with `M >= 1`. Try VTS 1 / VOB 1 first,
    // fall back through a few others in case the disc starts numbering
    // somewhere else (rare but seen).
    let candidates = [
        "/VIDEO_TS/VTS_01_1.VOB",
        "/VIDEO_TS/VTS_02_1.VOB",
        "/VIDEO_TS/VTS_03_1.VOB",
        "/VIDEO_TS/VTS_04_1.VOB",
    ];

    let mut found_sector: u32 = 0;
    for c in candidates {
        let Ok(filename) = CString::new(c) else {
            continue;
        };
        let mut size_out: u32 = 0;
        // SAFETY: `reader` is non-null, `filename` lives for the call,
        // `&mut size_out` is a valid u32 destination.
        let lba = unsafe {
            read_sys::UDFFindFile(reader, filename.as_ptr(), &mut size_out as *mut u32)
        };
        if lba != 0 && size_out >= DVD_BLOCK_SIZE as u32 {
            log::debug!("UDFFindFile hit: {c} -> lba={lba} size={size_out}");
            found_sector = lba;
            break;
        }
    }

    // SAFETY: closing a valid handle we opened above.
    unsafe { read_sys::DVDClose(reader) };

    if found_sector == 0 {
        None
    } else {
        Some(found_sector)
    }
}

// -- Path → CString helper --------------------------------------------------

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
