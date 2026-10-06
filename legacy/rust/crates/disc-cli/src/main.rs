//! `disc-remuxer`: the command-line front end.
//!
//! Commands so far: `version`, `settings`, `scan` (which discs are found under
//! a path and where their job folders go) and `probe` (FFmpeg's DVD-Video
//! demuxer, unchanged, on one title of each disc found; with an output folder
//! every disc becomes a job with its own job folder and job log); `debug`
//! runs single steps of the DVD processing.

mod ffmpeg;
mod jobs;
mod logger;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use disc_core::emit;
use disc_core::msg;
use disc_core::settings::{self, Settings};
use std::path::{Path, PathBuf};

#[derive(Parser)]
#[command(name = "disc-remuxer", version, about = "Rips DVD, Blu-ray and HD DVD sources")]
struct Cli {
    /// More log detail on the console: -v debug, -vv trace (overrides log.console).
    #[arg(short, long, global = true, action = clap::ArgAction::Count)]
    verbose: u8,
    /// Errors only on the console (overrides log.console).
    #[arg(short, long, global = true, conflicts_with = "verbose")]
    quiet: bool,
    /// Settings file to use instead of the default one.
    #[arg(long, global = true, value_name = "FILE")]
    settings: Option<PathBuf>,
    /// Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`.
    #[arg(long = "set", global = true, value_name = "GROUP.KEY=VALUE")]
    set: Vec<String>,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Program version and the versions of the bundled libraries.
    Version,
    /// The settings in effect, or where the settings file is.
    Settings {
        #[command(subcommand)]
        what: Option<SettingsCommand>,
    },
    /// List the discs found under a path and the job folder each would get.
    /// Creates nothing.
    Scan {
        /// A disc (folder or image) or a folder holding discs.
        source: PathBuf,
        /// Output folder (overrides output.root).
        #[arg(long, value_name = "DIR")]
        out: Option<PathBuf>,
    },
    /// Open one title of every disc found with FFmpeg's DVD-Video demuxer as
    /// it is and log the streams it finds. With an output folder, every disc
    /// is a job with its own job folder and job log.
    Probe {
        /// A disc (folder or image) or a folder holding discs.
        source: PathBuf,
        /// Title number (1-based; 0 = the demuxer's own choice).
        #[arg(long, default_value_t = 1)]
        title: i32,
        /// Output folder for the job folders (overrides output.root).
        #[arg(long, value_name = "DIR")]
        out: Option<PathBuf>,
    },
    /// Write the streams of HD DVD titles as elementary-stream files, one file
    /// per track, and the chapters as Matroska XML, into each disc's job
    /// folder. A title's name comes from `output.file_name_template` (default
    /// <title name>_t<NN>); its files are <name>_<track>_<language>_<codec>
    /// [_<channels>][ DELAY <ms>ms].<ext> and <name>_chapters.xml (LPCM as
    /// .wav). With every title, each title's files go in a folder of its name.
    Demux {
        /// A disc image or a folder holding disc images.
        source: PathBuf,
        /// Title number (0-based, as listed); every title when not given.
        #[arg(long)]
        title: Option<i32>,
        /// Output folder for the job folders (overrides output.root).
        #[arg(long, value_name = "DIR")]
        out: Option<PathBuf>,
    },
    /// Developer tools: the steps of the DVD processing one at a time, with
    /// their raw results on standard output.
    Debug {
        #[command(subcommand)]
        what: DebugCommand,
    },
}

#[derive(Subcommand)]
enum DebugCommand {
    /// Scan the navigation of one DVD (folder or image) and print the cell
    /// sequence found for each title and program chain: `result <start>
    /// title <n> pgc <n> cells <list>`, then `entered <title set>` for every
    /// title set the navigation played, or `scan failed: <reason>`.
    DvdScan {
        /// The disc: a DVD folder or image.
        source: PathBuf,
        /// Print every navigator call, event and sector read before the results.
        #[arg(long)]
        trace: bool,
    },
    /// Build the title plan of one DVD (folder or image) with the dvd.*
    /// settings and print the findings made (`event <name>` and its
    /// arguments, tab-separated) and every title: `title <name> vts <n> pgc
    /// <n> angle <a>/<n> cells <n> chapters <n> length <h:mm:ss> measured <s>
    /// size <bytes> segments <n> map <cells> scan <start> selected <yes|short|fake>`.
    DvdTitles {
        /// The disc: a DVD folder or image.
        source: PathBuf,
    },
}

#[derive(Subcommand, Clone, Copy)]
enum SettingsCommand {
    /// Every setting with its value and where the value came from (default).
    Show,
    /// The path of the settings file.
    Path,
}

fn main() {
    let cli = Cli::parse();
    // Until the settings are read, the console shows what -q / -v ask for.
    logger::init(console_level(&cli, None));
    ffmpeg::route_log(logger::max_level());

    let settings = match load_settings(&cli) {
        Ok(s) => s,
        Err(e) => {
            log::error!("{e:#}");
            std::process::exit(1);
        }
    };
    logger::set_console(console_level(&cli, settings.text("log.console")));
    ffmpeg::route_log(logger::max_level());

    let command_line = std::env::args().collect::<Vec<_>>().join(" ");
    if let Err(e) = run(&cli, &settings, &command_line) {
        log::error!("{e:#}");
        std::process::exit(1);
    }
}

/// -q / -v win over the log.console setting.
fn console_level(cli: &Cli, setting: Option<&str>) -> log::LevelFilter {
    if cli.quiet {
        return log::LevelFilter::Error;
    }
    match cli.verbose {
        0 => match setting {
            Some("error") => log::LevelFilter::Error,
            Some("warning") => log::LevelFilter::Warn,
            Some("debug") => log::LevelFilter::Debug,
            Some("trace") => log::LevelFilter::Trace,
            _ => log::LevelFilter::Info,
        },
        1 => log::LevelFilter::Debug,
        _ => log::LevelFilter::Trace,
    }
}

fn settings_path(cli: &Cli) -> Result<PathBuf> {
    match &cli.settings {
        Some(p) => Ok(p.clone()),
        None => settings::default_path().context("no default settings file"),
    }
}

/// Reads the settings file (bringing it in step with this program) and applies
/// the command-line overrides.
fn load_settings(cli: &Cli) -> Result<Settings> {
    let path = settings_path(cli)?;
    let (mut s, report) = settings::load(&path)?;
    emit!(msg::SETTINGS_FILE, path = path.display());
    if report.created {
        emit!(msg::SETTINGS_CREATED, path = path.display());
    }
    for (name, value) in &report.added {
        emit!(msg::SETTING_ADDED, name = name, value = value);
    }
    for (name, value) in &report.removed {
        emit!(msg::SETTING_REMOVED, name = name, value = value);
    }
    if let Some(backup) = &report.backup {
        emit!(msg::SETTINGS_BACKUP, path = backup.display());
    }
    for assignment in &cli.set {
        s.set_from_command_line(assignment)?;
    }
    Ok(s)
}

/// The output folder: `--out`, else `output.root`, else none.
fn output_root<'a>(out: Option<&'a Path>, settings: &'a Settings) -> Option<&'a Path> {
    out.or_else(|| settings.text("output.root").map(Path::new))
}

fn run(cli: &Cli, settings: &Settings, command_line: &str) -> Result<()> {
    match &cli.command {
        Command::Version => {
            print_versions();
            Ok(())
        }
        Command::Settings { what } => {
            match what.unwrap_or(SettingsCommand::Show) {
                SettingsCommand::Show => show_settings(settings),
                SettingsCommand::Path => println!("{}", settings_path(cli)?.display()),
            }
            Ok(())
        }
        Command::Scan { source, out } => {
            let discs = jobs::find(source, settings)?;
            let root = output_root(out.as_deref(), settings);
            for job in jobs::plan(source, &discs, root, settings) {
                match &job.folder {
                    Some(f) => println!("{}\t{}\t-> {}", job.disc.layout.describe(), job.disc.path.display(), f.display()),
                    None => println!("{}\t{}", job.disc.layout.describe(), job.disc.path.display()),
                }
            }
            Ok(())
        }
        Command::Probe { source, title, out } => {
            let discs = jobs::find(source, settings)?;
            let root = output_root(out.as_deref(), settings);
            let planned = jobs::plan(source, &discs, root, settings);
            jobs::run(&planned, settings, command_line, |job| {
                let disc = job.disc;
                emit!(msg::PROBE_TITLE, title = title, source = disc.path.display());
                ffmpeg::probe_dvdvideo(&disc.path, *title, settings).map_err(|reason| {
                    emit!(msg::PROBE_FAILED, title = title, source = disc.path.display(), reason = reason);
                    anyhow::anyhow!("title {title} could not be opened")
                })
            })
        }
        Command::Demux { source, title, out } => {
            let discs = jobs::find(source, settings)?;
            let root = output_root(out.as_deref(), settings);
            if root.is_none() {
                emit!(msg::NO_OUTPUT_FOLDER);
                anyhow::bail!("no output folder");
            }
            let planned = jobs::plan(source, &discs, root, settings);
            jobs::run(&planned, settings, command_line, |job| {
                let folder = job.folder.as_deref().expect("demux jobs have a job folder");
                ffmpeg::demux_hddvd(&job.disc.path, *title, folder, settings)
            })
        }
        Command::Debug { what: DebugCommand::DvdScan { source, trace } } => ffmpeg::debug_dvd_scan(source, *trace, settings),
        Command::Debug { what: DebugCommand::DvdTitles { source } } => ffmpeg::debug_dvd_titles(source, settings),
    }
}

fn show_settings(settings: &Settings) {
    for (s, value, source) in settings.iter() {
        println!("{:<22} = {:<10} ({source})", s.name(), value.to_string());
    }
}

fn print_versions() {
    println!("disc-remuxer {}", env!("CARGO_PKG_VERSION"));
    println!("  FFmpeg        {}", ffmpeg::version());
    for (name, v) in ffmpeg::library_versions() {
        println!("    {name:<12}{}.{}.{}", v.0, v.1, v.2);
    }
    println!("  libdvdread    {}", libdvdread_sys::VERSION);
    println!("  libdvdnav     {}", libdvdnav_sys::VERSION);
    println!("  libdvdcss     {}", libdvdcss_sys::VERSION);
    let helper = ccextractor_sys::program().map_or_else(|| "missing".to_owned(), |p| p.display().to_string());
    println!("  CCExtractor   {} (helper: {helper})", ccextractor_sys::VERSION);
}

/// `docs/CLI.md`: every command with its full help, generated from the
/// command-line definitions.
#[cfg(test)]
mod cli_doc {
    use clap::CommandFactory;
    use std::fmt::Write as _;

    fn page() -> String {
        let mut out = String::from(
            "# Commands and options\n\nGenerated from the command-line definitions \
             (`crates/disc-cli/src/main.rs`); a test keeps this page in step \
             (`UPDATE_DOCS=1 cargo test -p disc-cli` rewrites it). Settings: `docs/SETTINGS.md`.\n",
        );
        let mut root = super::Cli::command();
        root.build();
        add(&mut out, &mut root, "disc-remuxer");
        out
    }

    fn add(out: &mut String, cmd: &mut clap::Command, path: &str) {
        let help = cmd.render_long_help().to_string();
        let _ = write!(out, "\n## `{path}`\n\n```text\n{}\n```\n", help.trim_end());
        let names: Vec<String> =
            cmd.get_subcommands().filter(|s| s.get_name() != "help").map(|s| s.get_name().to_string()).collect();
        for name in names {
            let sub = cmd.find_subcommand_mut(&name).expect("listed above");
            add(out, sub, &format!("{path} {name}"));
        }
    }

    #[test]
    fn cli_doc_matches_the_definitions() {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../docs/CLI.md");
        let want = page();
        if std::env::var_os("UPDATE_DOCS").is_some() {
            std::fs::write(path, &want).unwrap();
        }
        let have = std::fs::read_to_string(path).unwrap_or_default();
        assert!(have == want, "docs/CLI.md is out of date: run UPDATE_DOCS=1 cargo test -p disc-cli");
    }
}
