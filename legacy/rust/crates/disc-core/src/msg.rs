//! The message catalog: every message the program itself writes to its logs.
//!
//! A message has a number, a level and a text with named fields (`{path}`).
//! Numbers are grouped by area:
//!
//! | Range | Area |
//! |---|---|
//! | 1000-1999 | program, settings, jobs |
//! | 2000-2999 | sources, discs, file systems |
//! | 3000-3999 | titles |
//! | 4000-4999 | reading |
//! | 5000-5999 | streams, timeline |
//! | 6000-6999 | output |
//!
//! Lines from FFmpeg and the libraries under it are not catalog messages;
//! they are logged as they come, marked with where they came from.

use std::fmt::Write as _;

/// One catalog entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Msg {
    pub id: u16,
    pub level: log::Level,
    pub text: &'static str,
}

/// Log target of catalog messages (FFmpeg's lines use `ffmpeg`).
pub const TARGET: &str = "disc_remuxer";

macro_rules! catalog {
    ($($name:ident = $id:literal, $level:ident, $text:literal;)*) => {
        $(pub const $name: Msg = Msg { id: $id, level: log::Level::$level, text: $text };)*
        /// Every message, in number order.
        pub const ALL: &[Msg] = &[$($name),*];
    };
}

catalog! {
    // 1000-1999 program, settings, jobs
    PROGRAM_START = 1001, Info, "{program} {version} started: {command}";
    SETTINGS_FILE = 1010, Debug, "Settings file: {path}";
    SETTINGS_CREATED = 1011, Info, "Settings file created with every setting at its default: {path}";
    SETTING_ADDED = 1012, Info, "Setting added to the settings file: {name} = {value}";
    SETTING_REMOVED = 1013, Warn, "Setting removed from the settings file (this program has no such setting): {name} = {value}";
    SETTINGS_BACKUP = 1014, Info, "Previous settings file saved as {path}";
    SETTING_IN_EFFECT = 1015, Info, "Setting {name} = {value} ({source})";
    JOB_START = 1100, Info, "Job {index} of {count}: {source}";
    JOB_FOLDER = 1101, Info, "Job folder: {path}";
    JOB_LOG = 1102, Info, "Job log: {path}";
    JOB_DEBUG_LOG = 1103, Info, "Debug log: {path}";
    JOB_DONE = 1110, Info, "Job {index} of {count} finished: {source}";
    JOB_FAILED = 1111, Error, "Job {index} of {count} failed: {source}: {reason}";
    JOBS_SUMMARY = 1120, Info, "{done} of {count} job(s) finished, {failed} failed";
    NO_OUTPUT_FOLDER = 1130, Error, "No output folder: set output.root in the settings file or give --out";

    // 2000-2999 sources, discs, file systems
    SCAN_START = 2001, Info, "Searching {path} for discs (up to {depth} folder level(s) deep)";
    SCAN_FOUND = 2002, Info, "Found {kind}: {path}";
    SCAN_NONE = 2003, Error, "No disc found in {path} (searched {depth} folder level(s) deep)";
    SCAN_UNREADABLE = 2004, Warn, "Folder skipped, cannot be read: {path}: {reason}";
    PROBE_TITLE = 2100, Info, "Probing title {title} of {source} with the DVD-Video demuxer";
    PROBE_FAILED = 2101, Error, "Title {title} of {source} could not be opened: {reason}";

    // 6000-6999 output
    DEMUX_TITLE = 6001, Info, "Writing the streams of title {title} of {source} to {folder}";
    DEMUX_STREAM = 6002, Info, "Stream {index} ({kind}, {codec}, language {lang}): {packets} packets, {bytes} bytes, {start} to {end}: {file}";
    DEMUX_EMPTY = 6005, Info, "Stream {index} ({kind}, {codec}, language {lang}) has no packets: no file";
    DEMUX_NAME = 6006, Info, "Title {title}: files named {prefix}_*";
    DEMUX_FOLDER_FAILED = 6007, Error, "The folder {folder} could not be created: {reason}";
    DEMUX_UNTESTED = 6008, Error, "Title {title}: {count} feature(s) met that no real disc has tested: its files are written, but check the log before using them";
    DEMUX_FAILED = 6010, Error, "Title {title} of {source} could not be demuxed: {reason}";
    DEMUX_TITLES = 6011, Info, "{source}: {count} title(s)";
}

/// The text of `msg` with its fields filled in from `fields` (`name`, value).
/// A field the text names but `fields` lacks stays as `{name}` (a programming
/// error, caught in debug builds).
#[must_use]
pub fn format(msg: &Msg, fields: &[(&str, &dyn std::fmt::Display)]) -> String {
    let mut out = String::with_capacity(msg.text.len() + 32);
    let mut rest = msg.text;
    while let Some(open) = rest.find('{') {
        out.push_str(&rest[..open]);
        let after = &rest[open + 1..];
        let Some(close) = after.find('}') else {
            out.push_str(&rest[open..]);
            return out;
        };
        let name = &after[..close];
        if let Some((_, v)) = fields.iter().find(|(n, _)| *n == name) {
            let _ = write!(out, "{v}");
        } else {
            debug_assert!(false, "message {} has no value for field {name}", msg.id);
            let _ = write!(out, "{{{name}}}");
        }
        rest = &after[close + 1..];
    }
    out.push_str(rest);
    out
}

/// Writes `msg` to the log as `[id] text`.
pub fn emit(msg: &Msg, fields: &[(&str, &dyn std::fmt::Display)]) {
    log::log!(target: TARGET, msg.level, "[{}] {}", msg.id, format(msg, fields));
}

/// `emit!(msg::JOB_FOLDER, path = folder.display())`
#[macro_export]
macro_rules! emit {
    ($msg:expr $(, $name:ident = $value:expr)* $(,)?) => {
        $crate::msg::emit(&$msg, &[$((stringify!($name), &$value as &dyn ::std::fmt::Display)),*])
    };
}

/// The field names a message text uses, in order.
#[must_use]
pub fn fields(msg: &Msg) -> Vec<&'static str> {
    let mut names = Vec::new();
    let mut rest = msg.text;
    while let Some(open) = rest.find('{') {
        let after = &rest[open + 1..];
        let Some(close) = after.find('}') else { break };
        names.push(&after[..close]);
        rest = &after[close + 1..];
    }
    names
}
