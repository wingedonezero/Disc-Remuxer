//! Running work on every disc found under a path, one job per disc.
//!
//! With an output folder, each job gets its job folder (see
//! `disc_core::discs::job_folder`) holding the job log
//! `<job folder name>_disc-remuxer.log` and, with `log.debug_file`, the debug
//! log `<job folder name>_disc-remuxer_debug.log`. Each job log starts with the
//! program, the command and every setting in effect, so it explains the job on
//! its own.

use std::path::{Path, PathBuf};

use anyhow::Result;
use disc_core::discs::{self, Disc};
use disc_core::emit;
use disc_core::msg;
use disc_core::settings::Settings;

use crate::logger;

/// One job's place: the disc and, with an output folder, its job folder.
pub struct Job<'a> {
    pub disc: &'a Disc,
    pub folder: Option<PathBuf>,
}

/// The discs under `source`, each with its job folder when `root` is given.
/// Logs what the search found or skipped. Nothing is created.
pub fn plan<'a>(source: &Path, discs: &'a [Disc], root: Option<&Path>, settings: &Settings) -> Vec<Job<'a>> {
    let keep = settings.switch("output.keep_structure");
    let mut planned: Vec<PathBuf> = Vec::new();
    discs
        .iter()
        .map(|disc| {
            let folder = root.map(|r| {
                // Two discs may want the same folder within one run; the
                // second gets the next free number.
                let mut f = discs::job_folder(disc, source, r, keep);
                let mut n = 1;
                let base = f.clone();
                while planned.contains(&f) {
                    let name = base.file_name().map_or_else(String::new, |s| s.to_string_lossy().into_owned());
                    f = base.with_file_name(format!("{name}_{n:03}"));
                    n += 1;
                }
                planned.push(f.clone());
                f
            });
            Job { disc, folder }
        })
        .collect()
}

/// Finds the discs under `source` (logging the search).
pub fn find(source: &Path, settings: &Settings) -> Result<Vec<Disc>> {
    let depth = u32::try_from(settings.number("output.scan_depth")).unwrap_or(0);
    emit!(msg::SCAN_START, path = source.display(), depth = depth);
    let (found, skipped) = discs::find(source, depth);
    for s in &skipped {
        emit!(msg::SCAN_UNREADABLE, path = s.path.display(), reason = s.reason);
    }
    for d in &found {
        emit!(msg::SCAN_FOUND, kind = d.layout.describe(), path = d.path.display());
    }
    if found.is_empty() {
        emit!(msg::SCAN_NONE, path = source.display(), depth = depth);
        anyhow::bail!("nothing to do");
    }
    Ok(found)
}

/// Runs `work` for every job; each job's log files are open while it runs.
/// Returns an error when any job failed (after running them all).
pub fn run(jobs: &[Job], settings: &Settings, command: &str, mut work: impl FnMut(&Disc) -> Result<()>) -> Result<()> {
    let count = jobs.len();
    let mut failed = 0;
    for (i, job) in jobs.iter().enumerate() {
        let index = i + 1;
        let mut logs = None;
        if let Some(folder) = &job.folder {
            match open_job_logs(folder, settings) {
                Ok(l) => logs = Some(l),
                Err(e) => {
                    emit!(msg::JOB_FAILED, index = index, count = count, source = job.disc.path.display(), reason = format!("{e:#}"));
                    failed += 1;
                    continue;
                }
            }
            emit!(msg::PROGRAM_START, program = "disc-remuxer", version = env!("CARGO_PKG_VERSION"), command = command);
            for (s, value, source) in settings.iter() {
                emit!(msg::SETTING_IN_EFFECT, name = s.name(), value = value, source = source);
            }
        }
        emit!(msg::JOB_START, index = index, count = count, source = job.disc.path.display());
        if let (Some(folder), Some((job_log, debug_log))) = (&job.folder, &logs) {
            emit!(msg::JOB_FOLDER, path = folder.display());
            emit!(msg::JOB_LOG, path = job_log.display());
            if let Some(d) = debug_log {
                emit!(msg::JOB_DEBUG_LOG, path = d.display());
            }
        }
        match work(job.disc) {
            Ok(()) => emit!(msg::JOB_DONE, index = index, count = count, source = job.disc.path.display()),
            Err(e) => {
                failed += 1;
                emit!(msg::JOB_FAILED, index = index, count = count, source = job.disc.path.display(), reason = format!("{e:#}"));
            }
        }
        logger::end_job();
        crate::ffmpeg::route_log(logger::max_level());
    }
    emit!(msg::JOBS_SUMMARY, done = count - failed, count = count, failed = failed);
    if failed > 0 {
        anyhow::bail!("{failed} of {count} job(s) failed");
    }
    Ok(())
}

/// Creates the job folder and opens its log files; returns their paths.
fn open_job_logs(folder: &Path, settings: &Settings) -> Result<(PathBuf, Option<PathBuf>)> {
    std::fs::create_dir_all(folder)?;
    let name = folder.file_name().map_or_else(String::new, |n| n.to_string_lossy().into_owned());
    let job_log = folder.join(format!("{name}_disc-remuxer.log"));
    let debug_log = settings.switch("log.debug_file").then(|| folder.join(format!("{name}_disc-remuxer_debug.log")));
    logger::start_job(&job_log, debug_log.as_deref())?;
    crate::ffmpeg::route_log(logger::max_level());
    Ok((job_log, debug_log))
}
