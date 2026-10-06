//! `CCExtractor`, compiled from `libs/ccextractor` by `build.rs` (C only) into
//! the `ccextractor` program, which is copied next to ours. It runs as a
//! helper process: closed-caption data in, SRT text out.

use std::path::PathBuf;

/// Version of the compiled `CCExtractor` copy.
pub const VERSION: &str = "0.94";

/// File name of the helper program.
pub const PROGRAM: &str = "ccextractor";

/// The helper program next to the running executable (where the build puts
/// it), or None when it is not there.
#[must_use]
pub fn program() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let mut dir = exe.parent()?.to_path_buf();
    // test executables live in target/<profile>/deps
    if dir.ends_with("deps") {
        dir.pop();
    }
    let p = dir.join(PROGRAM);
    p.is_file().then_some(p)
}
