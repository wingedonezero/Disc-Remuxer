//! Disc-type detection by filesystem layout.
//!
//! Mirrors the role of MakeMKV's disc-type detector (anchored at FUN_006c5ce0
//! in the atlas) — looks at which marker files / directories are present in
//! the given path and dispatches to the right backend. We use the public
//! file-system layout defined by the DVD-Video / BDMV specs, not MakeMKV's
//! internal heuristics.

use std::path::Path;

use crate::DiscError;

/// What kind of disc a path refers to.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum DiscType {
    /// DVD-Video. `VIDEO_TS/VIDEO_TS.IFO` present (or the path is an ISO image
    /// that libdvdread is expected to auto-detect).
    Dvd,
    /// Blu-ray. `BDMV/` directory present.
    Bluray,
    /// UHD Blu-ray. `BDMV/AACS/` present with UHD-specific MKB version.
    /// Refined when we add BD support.
    UltraHd,
}

impl DiscType {
    /// Short human-readable name, e.g. `"DVD"`, `"Blu-ray"`, `"UHD Blu-ray"`.
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Dvd => "DVD",
            Self::Bluray => "Blu-ray",
            Self::UltraHd => "UHD Blu-ray",
        }
    }
}

/// Inspect the given path and decide which backend should handle it.
///
/// * A directory containing `VIDEO_TS/VIDEO_TS.IFO` → [`DiscType::Dvd`]
/// * A directory containing `BDMV/` → [`DiscType::Bluray`] (UHD detection
///   is refined when the BD backend lands)
/// * A regular file ending in `.iso` → assumed DVD (libdvdread auto-detects;
///   if it's actually a BD image we'll surface that as an `OpenFailed`
///   when the backend tries to read it)
/// * A device node (e.g. `/dev/sr0`, `/dev/dvd`) is also assumed DVD for
///   now; BD device probing comes later.
///
/// Returns [`DiscError::PathNotFound`] if the path doesn't exist or
/// [`DiscError::UnknownDiscType`] if nothing matches.
pub fn detect_disc_type(path: &Path) -> Result<DiscType, DiscError> {
    if !path.exists() {
        return Err(DiscError::PathNotFound(path.to_path_buf()));
    }

    if path.is_dir() {
        // DVD: standard layout has VIDEO_TS/ subdirectory containing VIDEO_TS.IFO.
        // Some older rips have VIDEO_TS.IFO at the root — accept that too.
        let vmg_in_subdir = path.join("VIDEO_TS").join("VIDEO_TS.IFO");
        let vmg_at_root = path.join("VIDEO_TS.IFO");
        if vmg_in_subdir.is_file() || vmg_at_root.is_file() {
            log::debug!(
                "detect_disc_type: VIDEO_TS layout found at {}",
                path.display()
            );
            return Ok(DiscType::Dvd);
        }

        // Blu-ray / UHD: BDMV/ directory at the root.
        let bdmv = path.join("BDMV");
        if bdmv.is_dir() {
            log::debug!(
                "detect_disc_type: BDMV directory found at {}",
                path.display()
            );
            // TODO: distinguish UHD by checking BDMV/AACS/MKB version when
            // the BD backend lands. For now everything BDMV is reported
            // as plain Blu-ray.
            return Ok(DiscType::Bluray);
        }
    }

    if path.is_file() {
        let ext = path.extension().and_then(|s| s.to_str()).map(str::to_ascii_lowercase);
        if matches!(ext.as_deref(), Some("iso") | Some("img")) {
            log::debug!(
                "detect_disc_type: image file extension `{:?}` — defaulting to DVD",
                ext
            );
            return Ok(DiscType::Dvd);
        }
    }

    // Block devices (e.g. /dev/sr0) — assume DVD for now. libdvdread/libbluray
    // will both probe the actual disc on open. We refine when BD support lands.
    #[cfg(unix)]
    {
        use std::os::unix::fs::FileTypeExt;
        if let Ok(meta) = std::fs::metadata(path) {
            let ft = meta.file_type();
            if ft.is_block_device() || ft.is_char_device() {
                log::debug!(
                    "detect_disc_type: device node {} — defaulting to DVD",
                    path.display()
                );
                return Ok(DiscType::Dvd);
            }
        }
    }

    Err(DiscError::UnknownDiscType(path.to_path_buf()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn detects_video_ts_subdir() {
        let tmp = tempdir();
        fs::create_dir(tmp.path().join("VIDEO_TS")).unwrap();
        fs::write(tmp.path().join("VIDEO_TS/VIDEO_TS.IFO"), b"DVDVIDEO-VMG").unwrap();
        assert_eq!(detect_disc_type(tmp.path()).unwrap(), DiscType::Dvd);
    }

    #[test]
    fn detects_bdmv_dir() {
        let tmp = tempdir();
        fs::create_dir(tmp.path().join("BDMV")).unwrap();
        assert_eq!(detect_disc_type(tmp.path()).unwrap(), DiscType::Bluray);
    }

    #[test]
    fn rejects_unknown_dir() {
        let tmp = tempdir();
        let err = detect_disc_type(tmp.path()).unwrap_err();
        assert!(matches!(err, DiscError::UnknownDiscType(_)));
    }

    #[test]
    fn rejects_missing_path() {
        let err = detect_disc_type(Path::new("/does/not/exist/abc123")).unwrap_err();
        assert!(matches!(err, DiscError::PathNotFound(_)));
    }

    // Tiny tempdir helper to avoid a `tempfile` dep just for tests.
    fn tempdir() -> TmpDir {
        let base = std::env::temp_dir().join(format!(
            "disc-core-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&base).unwrap();
        TmpDir(base)
    }

    struct TmpDir(std::path::PathBuf);
    impl TmpDir {
        fn path(&self) -> &Path {
            &self.0
        }
    }
    impl Drop for TmpDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
}
