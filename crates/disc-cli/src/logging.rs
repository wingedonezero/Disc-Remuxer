//! Logging setup.
//!
//! Logs go to stderr by default. When the user is running a command
//! that produces an output file or directory, we can additionally
//! duplicate the log stream into a `disc-remuxer.log` next to that
//! output. That gives the user a self-documenting record of the
//! operation that can be archived with the rip and shared back if
//! something doesn't look right.
//!
//! Log level is taken from `RUST_LOG` (env_logger-style syntax), and
//! defaults to `info` when unset. Set `RUST_LOG=debug` for IFO and
//! sector-read lifecycle traces, `=trace` for byte-level activity,
//! `RUST_LOG=disc_check=info` to see just the verification PASS/FAIL
//! lines.

use std::path::Path;

use anyhow::{Context, Result};
use flexi_logger::{
    DeferredNow, Duplicate, FileSpec, LogSpecification, Logger, LoggerHandle, Record,
    WriteMode,
};

/// Initialize the global logger.
///
/// `job_log_file`, when `Some`, names a file path (absolute or relative)
/// that we'll additionally write to — alongside the normal stderr
/// stream. The file is overwritten on each run; we don't append (a job
/// log is per-invocation by intent).
///
/// Returns a `LoggerHandle` that the caller must keep alive for the
/// duration of the program (it owns background flush state). Dropping
/// it cleans up cleanly.
pub fn init(job_log_file: Option<&Path>) -> Result<LoggerHandle> {
    let spec = LogSpecification::env_or_parse("info")
        .context("parsing RUST_LOG")?;

    let mut logger = Logger::with(spec)
        .write_mode(WriteMode::BufferAndFlush)
        .format_for_stderr(format_line)
        .format_for_files(format_line_with_timestamp);

    if let Some(path) = job_log_file {
        let parent = path
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .map_or_else(|| std::path::PathBuf::from("."), Path::to_path_buf);
        let basename = path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("disc-remuxer");
        let suffix = path
            .extension()
            .and_then(|s| s.to_str())
            .unwrap_or("log");

        let file_spec = FileSpec::default()
            .directory(parent)
            .basename(basename)
            .suffix(suffix)
            .suppress_timestamp();

        logger = logger
            .log_to_file(file_spec)
            .duplicate_to_stderr(Duplicate::All);
    }

    let handle = logger.start().context("starting flexi_logger")?;

    if let Some(path) = job_log_file {
        log::info!("disc-remuxer job log: {}", path.display());
    }
    Ok(handle)
}

/// `[LEVEL  module]  message` — no timestamp on stderr (developers
/// running CLI commands don't need it; they have their shell prompt).
fn format_line(
    w: &mut dyn std::io::Write,
    _now: &mut DeferredNow,
    record: &Record,
) -> std::io::Result<()> {
    write!(
        w,
        "[{:5} {}] {}",
        record.level(),
        record.module_path().unwrap_or("<?>"),
        record.args(),
    )
}

/// `YYYY-MM-DD HH:MM:SS.SSS [LEVEL  module]  message` — timestamp on
/// disk so anyone reading the job log later has wall-clock context.
fn format_line_with_timestamp(
    w: &mut dyn std::io::Write,
    now: &mut DeferredNow,
    record: &Record,
) -> std::io::Result<()> {
    write!(
        w,
        "{} [{:5} {}] {}",
        now.format("%Y-%m-%d %H:%M:%S%.3f"),
        record.level(),
        record.module_path().unwrap_or("<?>"),
        record.args(),
    )
}
