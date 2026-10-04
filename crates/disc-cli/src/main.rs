//! `disc-remuxer`: the command-line front end.
//!
//! Skeleton stage: `version`, and `probe`, which runs FFmpeg's DVD-Video
//! demuxer unchanged on one title so its behaviour can be checked on real
//! discs before anything is changed.

mod ffmpeg;
mod logger;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
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
    /// Open one DVD title with FFmpeg's DVD-Video demuxer as it is and print
    /// the streams it finds.
    Probe {
        /// DVD folder (holding `VIDEO_TS`) or image file.
        source: PathBuf,
        /// Title number (1-based; 0 = the demuxer's own choice).
        #[arg(long, default_value_t = 1)]
        title: i32,
    },
}

#[derive(Subcommand)]
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

    let settings = match load_settings(&cli) {
        Ok(s) => s,
        Err(e) => {
            log::error!("{e:#}");
            std::process::exit(1);
        }
    };
    let level = console_level(&cli, settings.text("log.console"));
    log::set_max_level(level);
    ffmpeg::route_log(level);

    if let Err(e) = run(cli.command, &settings, cli.settings.as_deref()) {
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

/// Reads the settings file (bringing it in step with this program) and applies
/// the command-line overrides.
fn load_settings(cli: &Cli) -> Result<Settings> {
    let path = match &cli.settings {
        Some(p) => p.clone(),
        None => settings::default_path()?,
    };
    let (mut s, report) = settings::load(&path)?;
    if report.created {
        log::info!("settings file created with every setting at its default: {}", path.display());
    }
    for (name, value) in &report.added {
        log::info!("setting added to the settings file: {name} = {value}");
    }
    for (name, value) in &report.removed {
        log::warn!("setting removed from the settings file (this program has no such setting): {name} = {value}");
    }
    if let Some(backup) = &report.backup {
        log::info!("previous settings file saved as {}", backup.display());
    }
    log::debug!("settings file: {}", path.display());
    for assignment in &cli.set {
        s.set_from_command_line(assignment)?;
    }
    Ok(s)
}

fn run(command: Command, settings: &Settings, settings_file: Option<&Path>) -> Result<()> {
    match command {
        Command::Version => {
            print_versions();
            Ok(())
        }
        Command::Settings { what } => match what.unwrap_or(SettingsCommand::Show) {
            SettingsCommand::Show => {
                show_settings(settings);
                Ok(())
            }
            SettingsCommand::Path => {
                let path = match settings_file {
                    Some(p) => p.to_path_buf(),
                    None => settings::default_path().context("no default settings file")?,
                };
                println!("{}", path.display());
                Ok(())
            }
        },
        Command::Probe { source, title } => ffmpeg::probe_dvdvideo(&source, title),
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
}
