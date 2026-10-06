//! Finding discs under a path and choosing each disc's job folder.
//!
//! A disc is one of: a folder holding `VIDEO_TS` (DVD) or `BDMV` (Blu-ray); a
//! folder holding the DVD files directly (`VIDEO_TS.IFO`) or the Blu-ray files
//! directly (`index.bdmv`); a disc image file (`.iso`, `.img`). A path to a
//! `VIDEO_TS.IFO` file stands for the folder holding it. A folder that is
//! a disc is not searched further. Other folders are searched in name order, up
//! to a depth limit; a folder may hold disc images and disc folders side by side
//! (e.g. extras as an image next to the volumes as folders).

use std::fs;
use std::path::{Path, PathBuf};

/// Disc image file extensions (lower case).
pub const IMAGE_EXTENSIONS: &[&str] = &["iso", "img"];

/// What a found disc looks like on the file system.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Layout {
    /// A folder holding `VIDEO_TS`.
    DvdFolder,
    /// A folder holding the DVD files (`VIDEO_TS.IFO`) directly.
    DvdFilesFolder,
    /// A folder holding `BDMV`.
    BlurayFolder,
    /// A folder holding the Blu-ray files (`index.bdmv`) directly.
    BlurayFilesFolder,
    /// A disc image file.
    Image,
}

impl Layout {
    /// For messages.
    #[must_use]
    pub fn describe(self) -> &'static str {
        match self {
            Layout::DvdFolder => "DVD folder",
            Layout::DvdFilesFolder => "DVD files folder",
            Layout::BlurayFolder => "Blu-ray folder",
            Layout::BlurayFilesFolder => "Blu-ray files folder",
            Layout::Image => "disc image",
        }
    }
}

/// One disc found by [`find`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Disc {
    /// What is opened: the disc folder (the folder holding `VIDEO_TS` / `BDMV`
    /// or the disc files) or the image file.
    pub path: PathBuf,
    pub layout: Layout,
    /// The disc's name for its job folder: the folder's name (for a folder
    /// named `VIDEO_TS` / `BDMV` itself, its parent's), or the image file's
    /// name without its extension.
    pub name: String,
    /// Where the disc sits below the searched path (`.` = the path itself).
    pub relative: PathBuf,
}

/// A folder that could not be searched.
#[derive(Debug, Clone)]
pub struct Skipped {
    pub path: PathBuf,
    pub reason: String,
}

/// Searches `path` for discs, at most `depth` folder levels below it.
#[must_use]
pub fn find(path: &Path, depth: u32) -> (Vec<Disc>, Vec<Skipped>) {
    let mut discs = Vec::new();
    let mut skipped = Vec::new();
    if is_image(path) {
        discs.push(Disc {
            path: path.to_path_buf(),
            layout: Layout::Image,
            name: stem(path),
            relative: PathBuf::from("."),
        });
    } else if let Some(dir) = vmg_ifo_folder(path) {
        discs.push(Disc {
            path: dir.to_path_buf(),
            layout: Layout::DvdFilesFolder,
            name: folder_name(dir),
            relative: PathBuf::from("."),
        });
    } else if path.is_dir() {
        search(path, path, 0, depth, &mut discs, &mut skipped);
    }
    (discs, skipped)
}

fn search(root: &Path, dir: &Path, level: u32, depth: u32, discs: &mut Vec<Disc>, skipped: &mut Vec<Skipped>) {
    let relative = dir.strip_prefix(root).map_or_else(|_| PathBuf::from("."), |r| {
        if r.as_os_str().is_empty() { PathBuf::from(".") } else { r.to_path_buf() }
    });
    if let Some(layout) = disc_layout(dir) {
        discs.push(Disc { path: dir.to_path_buf(), layout, name: folder_name(dir), relative });
        return;
    }
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(e) => {
            skipped.push(Skipped { path: dir.to_path_buf(), reason: e.to_string() });
            return;
        }
    };
    let mut paths: Vec<PathBuf> = entries.filter_map(|e| e.ok().map(|e| e.path())).collect();
    paths.sort();
    for p in paths {
        if is_image(&p) {
            let relative = p.strip_prefix(root).map_or_else(|_| PathBuf::from("."), Path::to_path_buf);
            discs.push(Disc { name: stem(&p), path: p, layout: Layout::Image, relative });
        } else if p.is_dir() && level < depth {
            search(root, &p, level + 1, depth, discs, skipped);
        }
    }
}

/// The layout when `dir` is itself a disc.
fn disc_layout(dir: &Path) -> Option<Layout> {
    if dir.join("VIDEO_TS").is_dir() || dir.join("video_ts").is_dir() {
        Some(Layout::DvdFolder)
    } else if dir.join("BDMV").is_dir() {
        Some(Layout::BlurayFolder)
    } else if dir.join("VIDEO_TS.IFO").is_file() || dir.join("video_ts.ifo").is_file() {
        Some(Layout::DvdFilesFolder)
    } else if dir.join("index.bdmv").is_file() || dir.join("INDEX.BDMV").is_file() {
        Some(Layout::BlurayFilesFolder)
    } else {
        None
    }
}

/// The folder of `p` when `p` is a `VIDEO_TS.IFO` file.
fn vmg_ifo_folder(p: &Path) -> Option<&Path> {
    let named = p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.eq_ignore_ascii_case("VIDEO_TS.IFO"));
    if named && p.is_file() { p.parent() } else { None }
}

fn is_image(p: &Path) -> bool {
    p.is_file()
        && p.extension()
            .and_then(|e| e.to_str())
            .is_some_and(|e| IMAGE_EXTENSIONS.contains(&e.to_ascii_lowercase().as_str()))
}

fn stem(p: &Path) -> String {
    p.file_stem().map_or_else(String::new, |s| s.to_string_lossy().into_owned())
}

/// A folder named `VIDEO_TS` / `BDMV` says nothing about the disc: use its
/// parent's name.
fn folder_name(dir: &Path) -> String {
    let name = dir.file_name().map_or_else(String::new, |n| n.to_string_lossy().into_owned());
    match (name.to_ascii_uppercase().as_str(), dir.parent().and_then(Path::file_name)) {
        ("VIDEO_TS" | "BDMV", Some(parent)) => parent.to_string_lossy().into_owned(),
        _ => name,
    }
}

/// A name usable as a folder name: characters that are not allowed in file
/// names on common file systems become spaces, runs of white space one space;
/// empty becomes `Unnamed`.
#[must_use]
pub fn safe_name(name: &str) -> String {
    let replaced: String = name
        .chars()
        .map(|c| if matches!(c, '\\' | '/' | ':' | '*' | '?' | '"' | '<' | '>' | '|') || c.is_control() { ' ' } else { c })
        .collect();
    let joined = replaced.split_whitespace().collect::<Vec<_>>().join(" ");
    if joined.is_empty() { "Unnamed".into() } else { joined }
}

/// The job folder for `disc` found under `searched`, below `root`. With
/// `keep_structure`, the disc's place below the searched folder is recreated
/// (starting with the searched folder's own name); otherwise every job folder
/// sits directly in `root`. When the folder exists already, `_001`, `_002`, ...
/// is added. Nothing is created.
#[must_use]
pub fn job_folder(disc: &Disc, searched: &Path, root: &Path, keep_structure: bool) -> PathBuf {
    let base = if !keep_structure || disc.relative == Path::new(".") {
        root.join(safe_name(&disc.name))
    } else {
        let mut p = root.join(safe_name(&searched.file_name().map_or_else(String::new, |n| n.to_string_lossy().into_owned())));
        let parts: Vec<_> = disc.relative.components().collect();
        // The disc's own entry gets the disc's name (an image's extension dropped).
        for (i, c) in parts.iter().enumerate() {
            let part = c.as_os_str().to_string_lossy();
            p.push(safe_name(if i + 1 == parts.len() { &disc.name } else { &part }));
        }
        p
    };
    unique(base)
}

fn unique(base: PathBuf) -> PathBuf {
    if !base.exists() {
        return base;
    }
    let name = base.file_name().map_or_else(String::new, |n| n.to_string_lossy().into_owned());
    let mut n = 1u32;
    loop {
        let candidate = base.with_file_name(format!("{name}_{n:03}"));
        if !candidate.exists() {
            return candidate;
        }
        n += 1;
    }
}
