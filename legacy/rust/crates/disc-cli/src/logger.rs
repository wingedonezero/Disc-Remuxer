//! The program's logger.
//!
//! - Console (standard error): up to the console level (`log.console`, `-q`,
//!   `-v`).
//! - While a job runs: the job log in the job folder gets every line from info
//!   up, the debug log (when `log.debug_file` is on) every line; both with
//!   local timestamps.
//!
//! Catalog messages (target `disc_remuxer`) are written as they are
//! (`[id] text`); lines from FFmpeg and the libraries under it get an
//! `ffmpeg: ` prefix, anything else its target.

use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

use log::{Level, LevelFilter, Log, Metadata, Record};

struct Logger;

#[derive(Default)]
struct JobFiles {
    job: Option<BufWriter<File>>,
    debug: Option<BufWriter<File>>,
}

static LOGGER: Logger = Logger;
static CONSOLE: AtomicUsize = AtomicUsize::new(LevelFilter::Info as usize);
static FILES: Mutex<JobFiles> = Mutex::new(JobFiles { job: None, debug: None });

fn level_filter(n: usize) -> LevelFilter {
    [LevelFilter::Off, LevelFilter::Error, LevelFilter::Warn, LevelFilter::Info, LevelFilter::Debug, LevelFilter::Trace]
        [n.min(5)]
}

impl Log for Logger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        metadata.level() <= log::max_level()
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }
        let text = match record.target() {
            disc_core::msg::TARGET => format!("{}", record.args()),
            "ffmpeg" => format!("ffmpeg: {}", record.args()),
            other => format!("{other}: {}", record.args()),
        };
        let level = record.level();
        if level <= level_filter(CONSOLE.load(Ordering::Relaxed)) {
            eprintln!("{level:<5} {text}");
        }
        let mut files = FILES.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        if files.job.is_none() && files.debug.is_none() {
            return;
        }
        let stamp = chrono::Local::now().format("%Y-%m-%d %H:%M:%S%.3f");
        if level <= Level::Info {
            if let Some(f) = files.job.as_mut() {
                let _ = writeln!(f, "{stamp} {level:<5} {text}");
            }
        }
        if let Some(f) = files.debug.as_mut() {
            let _ = writeln!(f, "{stamp} {level:<5} {text}");
        }
    }

    fn flush(&self) {
        let mut files = FILES.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        if let Some(f) = files.job.as_mut() {
            let _ = f.flush();
        }
        if let Some(f) = files.debug.as_mut() {
            let _ = f.flush();
        }
    }
}

/// Installs the logger with the console showing up to `console`.
pub fn init(console: LevelFilter) {
    log::set_logger(&LOGGER).expect("logger installed twice");
    set_console(console);
}

/// Sets the console level; the overall level follows (see [`max_level`]).
pub fn set_console(console: LevelFilter) {
    CONSOLE.store(console as usize, Ordering::Relaxed);
    log::set_max_level(max_level());
}

/// The most detailed level any destination wants right now.
pub fn max_level() -> LevelFilter {
    let console = level_filter(CONSOLE.load(Ordering::Relaxed));
    let files = FILES.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
    let file = if files.debug.is_some() {
        LevelFilter::Trace
    } else if files.job.is_some() {
        LevelFilter::Info
    } else {
        LevelFilter::Off
    };
    console.max(file)
}

/// Starts writing the job log (and the debug log when `debug` is given).
pub fn start_job(job: &Path, debug: Option<&Path>) -> std::io::Result<()> {
    let job_file = BufWriter::new(File::create(job)?);
    let debug_file = debug.map(File::create).transpose()?.map(BufWriter::new);
    {
        let mut files = FILES.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        files.job = Some(job_file);
        files.debug = debug_file;
    }
    log::set_max_level(max_level());
    Ok(())
}

/// Finishes and closes the job's log files.
pub fn end_job() {
    LOGGER.flush();
    {
        let mut files = FILES.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        *files = JobFiles::default();
    }
    log::set_max_level(max_level());
}
