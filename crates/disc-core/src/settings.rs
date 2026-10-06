//! Program settings: the registry of every setting, the settings file and the
//! value in effect.
//!
//! Each setting has one value from, in order (a later source wins): its
//! built-in default, the settings file, the command line (`--set group.key=value`
//! or a dedicated flag).
//!
//! The settings file (TOML) always lists every setting of the program, grouped,
//! each with a short explanation, so it doubles as the reference of what can be
//! set. It is kept in step with the program on every start: settings the file
//! lacks are added with their default, entries the program no longer knows are
//! removed, the values that stay are never changed, and the previous file is
//! saved as a backup before it is rewritten. A value of the wrong type or out of
//! range stops the program and leaves the file untouched.

use std::collections::BTreeMap;
use std::fmt::{self, Write as _};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

/// What a setting holds and which values are allowed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// On / off.
    Switch,
    /// A whole number within `min..=max`.
    Number { min: i64, max: i64 },
    /// Free text; empty = not set.
    Text,
    /// A folder path; empty = not set.
    Folder,
    /// One of the listed words.
    Choice(&'static [&'static str]),
}

/// A setting's value.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Value {
    Switch(bool),
    Number(i64),
    /// Text, folder and choice settings.
    Text(String),
}

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Switch(b) => write!(f, "{b}"),
            Value::Number(n) => write!(f, "{n}"),
            Value::Text(t) => write!(f, "{}", toml::Value::String(t.clone())),
        }
    }
}

/// A setting's built-in default.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DefaultValue {
    Switch(bool),
    Number(i64),
    Text(&'static str),
}

impl DefaultValue {
    fn value(self) -> Value {
        match self {
            DefaultValue::Switch(b) => Value::Switch(b),
            DefaultValue::Number(n) => Value::Number(n),
            DefaultValue::Text(t) => Value::Text(t.to_string()),
        }
    }
}

/// One entry of the registry.
#[derive(Debug, Clone, Copy)]
pub struct Setting {
    /// Section in the settings file.
    pub group: &'static str,
    /// Name within the group.
    pub key: &'static str,
    pub kind: Kind,
    pub default: DefaultValue,
    /// One or two sentences: what it does, what the values mean.
    pub help: &'static str,
}

impl Setting {
    /// `group.key`, the name used on the command line and in messages.
    #[must_use]
    pub fn name(&self) -> String {
        format!("{}.{}", self.group, self.key)
    }

    /// The allowed values, in words (for the file and the docs).
    #[must_use]
    pub fn allowed(&self) -> String {
        match self.kind {
            Kind::Switch => "true or false".into(),
            Kind::Number { min, max } => format!("{min} to {max}"),
            Kind::Text => "text; empty = not set".into(),
            Kind::Folder => "a folder; empty = not set".into(),
            Kind::Choice(words) => words.join(", "),
        }
    }
}

/// Every setting of the program. Add new settings here; the settings file and
/// `docs/SETTINGS.md` follow from this list.
pub const SETTINGS: &[Setting] = &[
    Setting {
        group: "output",
        key: "root",
        kind: Kind::Folder,
        default: DefaultValue::Text(""),
        help: "Folder the job folders are created in. Not set: the output folder must be given on \
               the command line (--out).",
    },
    Setting {
        group: "output",
        key: "keep_structure",
        kind: Kind::Switch,
        default: DefaultValue::Switch(true),
        help: "When a folder holding several discs is given, recreate its sub-folders under the \
               output root, so each disc's job folder sits where the disc sat.",
    },
    Setting {
        group: "output",
        key: "scan_depth",
        kind: Kind::Number { min: 0, max: 20 },
        default: DefaultValue::Number(5),
        help: "How many folder levels below a given folder are searched for discs (VIDEO_TS \
               folders, BDMV folders, disc images).",
    },
    Setting {
        group: "read",
        key: "attempts",
        kind: Kind::Number { min: 1, max: 99 },
        default: DefaultValue::Number(5),
        help: "How many times a read from a disc is tried before it counts as failed; after the \
               first retry the rest of the read goes one sector at a time.",
    },
    Setting {
        group: "read",
        key: "udf_reader",
        kind: Kind::Choice(&["netbsd", "linux"]),
        default: DefaultValue::Text("netbsd"),
        help: "Which UDF reader opens disc images: netbsd (based on NetBSD's UDF code) or linux \
               (based on Linux's).",
    },
    Setting {
        group: "read",
        key: "prefer_iso_for_old_udf102",
        kind: Kind::Switch,
        default: DefaultValue::Switch(true),
        help: "Read a DVD image whose UDF 1.02 file system was recorded before 2006 through its \
               ISO 9660 file system, when that holds a valid DVD-Video structure.",
    },
    Setting {
        group: "dvd",
        key: "cell_mode",
        kind: Kind::Choice(&["auto", "walk", "trim", "walk_trim"]),
        default: DefaultValue::Text("auto"),
        help: "How a DVD title's cells are chosen: walk = the cells the disc's navigation plays \
               (found by playing the navigation through), trim = cells that do not look like content \
               are trimmed off the program chain's ends (the navigation is not played), walk_trim = \
               walk, trim where walk finds nothing, auto = walk when the navigation found titles, \
               else trim.",
    },
    Setting {
        group: "dvd",
        key: "title_order",
        kind: Kind::Choice(&["auto", "scan_first", "table"]),
        default: DefaultValue::Text("auto"),
        help: "Order of a DVD's titles: scan_first = the titles the disc's navigation leads to \
               first, then the others; table = the disc's title table order; auto = scan_first with \
               the cell modes auto and walk, table with trim and walk_trim.",
    },
    Setting {
        group: "dvd",
        key: "min_title_length",
        kind: Kind::Number { min: 0, max: 86_400 },
        default: DefaultValue::Number(120),
        help: "Titles shorter than this many seconds are listed but not selected.",
    },
    Setting {
        group: "aacs",
        key: "key_files",
        kind: Kind::Text,
        default: DefaultValue::Text(""),
        help: "AACS key files (KEYDB.cfg format), separated by commas, read in this order; the first \
               one holding a key for the disc is used. Needed for encrypted HD DVD images.",
    },
    Setting {
        group: "log",
        key: "console",
        kind: Kind::Choice(&["error", "warning", "info", "debug", "trace"]),
        default: DefaultValue::Text("info"),
        help: "Most detailed level shown on the console; -q and -v override it for one run.",
    },
    Setting {
        group: "log",
        key: "debug_file",
        kind: Kind::Switch,
        default: DefaultValue::Switch(false),
        help: "Also write the detailed debug log (every step, library messages included) into \
               the job folder, next to the job log.",
    },
];

/// Where a value in effect came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    Default,
    File,
    CommandLine,
}

impl fmt::Display for Source {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Source::Default => "default",
            Source::File => "settings file",
            Source::CommandLine => "command line",
        })
    }
}

/// What reading the settings file did to it.
#[derive(Debug, Clone, Default)]
pub struct FileReport {
    pub path: PathBuf,
    /// The file did not exist and was written with every default.
    pub created: bool,
    /// Settings the file lacked, added with their default (`group.key`, value).
    pub added: Vec<(String, String)>,
    /// Entries the program does not know, removed (`group.key`, value as written).
    pub removed: Vec<(String, String)>,
    /// Copy of the previous file, written before it was rewritten.
    pub backup: Option<PathBuf>,
}

/// Why settings could not be used.
#[derive(Debug, thiserror::Error)]
pub enum SettingsError {
    #[error("no settings folder: neither XDG_CONFIG_HOME nor HOME is set")]
    NoConfigHome,
    #[error("cannot read the settings file {path}: {source}")]
    Read { path: PathBuf, source: std::io::Error },
    #[error("cannot write the settings file {path}: {source}")]
    Write { path: PathBuf, source: std::io::Error },
    #[error("the settings file {path} is not valid TOML: {message}")]
    Syntax { path: PathBuf, message: String },
    #[error("{origin}: setting {name} = {found} is not allowed (allowed: {allowed}); nothing was changed")]
    Invalid { origin: String, name: String, found: String, allowed: String },
    #[error("unknown setting {name} (see `disc-remuxer settings show`)")]
    Unknown { name: String },
    #[error("--set needs group.key=value, got {0:?}")]
    BadOverride(String),
}

/// The value of every setting in effect, with its source.
#[derive(Debug, Clone)]
pub struct Settings {
    values: BTreeMap<String, (Value, Source)>,
}

impl Settings {
    /// Every setting at its built-in default.
    #[must_use]
    pub fn defaults() -> Self {
        Settings {
            values: SETTINGS.iter().map(|s| (s.name(), (s.default.value(), Source::Default))).collect(),
        }
    }

    fn get(&self, name: &str) -> &Value {
        &self
            .values
            .get(name)
            .unwrap_or_else(|| panic!("setting {name} is not in the registry"))
            .0
    }

    /// A switch setting. Panics when `name` is not a switch in the registry.
    #[must_use]
    pub fn switch(&self, name: &str) -> bool {
        match self.get(name) {
            Value::Switch(b) => *b,
            v => panic!("setting {name} is not a switch ({v:?})"),
        }
    }

    /// A number setting. Panics when `name` is not a number in the registry.
    #[must_use]
    pub fn number(&self, name: &str) -> i64 {
        match self.get(name) {
            Value::Number(n) => *n,
            v => panic!("setting {name} is not a number ({v:?})"),
        }
    }

    /// A text, folder or choice setting; `None` when it is not set (empty).
    #[must_use]
    pub fn text(&self, name: &str) -> Option<&str> {
        match self.get(name) {
            Value::Text(t) if t.is_empty() => None,
            Value::Text(t) => Some(t),
            v => panic!("setting {name} is not text ({v:?})"),
        }
    }

    /// Every setting in registry order with its value and source.
    pub fn iter(&self) -> impl Iterator<Item = (&'static Setting, &Value, Source)> + '_ {
        SETTINGS.iter().map(|s| {
            let (v, src) = &self.values[&s.name()];
            (s, v, *src)
        })
    }

    /// Applies one command-line override `group.key=value`.
    pub fn set_from_command_line(&mut self, assignment: &str) -> Result<(), SettingsError> {
        let (name, text) = assignment
            .split_once('=')
            .ok_or_else(|| SettingsError::BadOverride(assignment.to_string()))?;
        let name = name.trim();
        let setting = find(name).ok_or_else(|| SettingsError::Unknown { name: name.to_string() })?;
        let value = parse_text(setting, text.trim(), "command line")?;
        self.values.insert(setting.name(), (value, Source::CommandLine));
        Ok(())
    }
}

/// The registry entry named `group.key`.
#[must_use]
pub fn find(name: &str) -> Option<&'static Setting> {
    SETTINGS.iter().find(|s| s.name() == name)
}

/// The settings file used when none is given:
/// `$XDG_CONFIG_HOME/disc-remuxer/settings.toml`, else `~/.config/disc-remuxer/settings.toml`.
pub fn default_path() -> Result<PathBuf, SettingsError> {
    let base = match std::env::var_os("XDG_CONFIG_HOME").filter(|v| !v.is_empty()) {
        Some(dir) => PathBuf::from(dir),
        None => PathBuf::from(std::env::var_os("HOME").ok_or(SettingsError::NoConfigHome)?).join(".config"),
    };
    Ok(base.join("disc-remuxer").join("settings.toml"))
}

/// Reads the settings file at `path` (writing it first when it does not exist)
/// and brings it in step with the registry. Returns the settings it holds and
/// what was done to the file.
pub fn load(path: &Path) -> Result<(Settings, FileReport), SettingsError> {
    let mut report = FileReport { path: path.to_path_buf(), ..FileReport::default() };
    let mut settings = Settings::defaults();

    if !path.exists() {
        write_file(path, &settings)?;
        report.created = true;
        return Ok((settings, report));
    }

    let text = fs::read_to_string(path).map_err(|source| SettingsError::Read { path: path.to_path_buf(), source })?;
    let table: toml::Table = text
        .parse()
        .map_err(|e: toml::de::Error| SettingsError::Syntax { path: path.to_path_buf(), message: e.message().to_string() })?;

    let origin = path.display().to_string();
    for s in SETTINGS {
        match table.get(s.group).and_then(|g| g.as_table()).and_then(|g| g.get(s.key)) {
            Some(v) => {
                let value = parse_toml(s, v, &origin)?;
                settings.values.insert(s.name(), (value, Source::File));
            }
            None => report.added.push((s.name(), s.default.value().to_string())),
        }
    }
    for (group, entry) in &table {
        match entry.as_table() {
            Some(keys) => {
                for (key, v) in keys {
                    let name = format!("{group}.{key}");
                    if find(&name).is_none() {
                        report.removed.push((name, v.to_string()));
                    }
                }
            }
            None => report.removed.push((group.clone(), entry.to_string())),
        }
    }

    if !report.added.is_empty() || !report.removed.is_empty() {
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs());
        let backup = path.with_file_name(format!(
            "{}.{stamp}.bak",
            path.file_name().map_or_else(|| "settings.toml".into(), |n| n.to_string_lossy())
        ));
        fs::copy(path, &backup).map_err(|source| SettingsError::Write { path: backup.clone(), source })?;
        report.backup = Some(backup);
        write_file(path, &settings)?;
    }
    Ok((settings, report))
}

fn parse_toml(s: &Setting, v: &toml::Value, origin: &str) -> Result<Value, SettingsError> {
    let invalid = || SettingsError::Invalid {
        origin: origin.to_string(),
        name: s.name(),
        found: v.to_string(),
        allowed: s.allowed(),
    };
    let value = match (s.kind, v) {
        (Kind::Switch, toml::Value::Boolean(b)) => Value::Switch(*b),
        (Kind::Number { .. }, toml::Value::Integer(n)) => Value::Number(*n),
        (Kind::Text | Kind::Folder | Kind::Choice(_), toml::Value::String(t)) => Value::Text(t.clone()),
        _ => return Err(invalid()),
    };
    check(s, value).map_err(|()| invalid())
}

fn parse_text(s: &Setting, text: &str, origin: &str) -> Result<Value, SettingsError> {
    let invalid = || SettingsError::Invalid {
        origin: origin.to_string(),
        name: s.name(),
        found: text.to_string(),
        allowed: s.allowed(),
    };
    let value = match s.kind {
        Kind::Switch => match text {
            "true" | "on" | "yes" | "1" => Value::Switch(true),
            "false" | "off" | "no" | "0" => Value::Switch(false),
            _ => return Err(invalid()),
        },
        Kind::Number { .. } => Value::Number(text.parse().map_err(|_| invalid())?),
        Kind::Text | Kind::Folder | Kind::Choice(_) => Value::Text(text.to_string()),
    };
    check(s, value).map_err(|()| invalid())
}

fn check(s: &Setting, value: Value) -> Result<Value, ()> {
    match (&s.kind, &value) {
        (Kind::Number { min, max }, Value::Number(n)) if n < min || n > max => Err(()),
        (Kind::Choice(words), Value::Text(t)) if !words.contains(&t.as_str()) => Err(()),
        _ => Ok(value),
    }
}

/// The settings file for `settings`: every setting of the registry, grouped,
/// with its explanation, allowed values and default.
#[must_use]
pub fn render(settings: &Settings) -> String {
    let mut out = String::from(
        "# disc-remuxer settings\n\
         #\n\
         # Every setting the program has is listed here; change the values as needed.\n\
         # The program keeps this file in step with itself: new settings are added with\n\
         # their default, settings it no longer has are removed, your values are kept,\n\
         # and the previous file is saved as a backup first. Command-line options\n\
         # (--set group.key=value) override this file for one run.\n",
    );
    let mut group = "";
    for (s, value, _) in settings.iter() {
        if s.group != group {
            group = s.group;
            let _ = write!(out, "\n[{group}]\n");
        }
        out.push('\n');
        for line in wrap(s.help, 76) {
            let _ = writeln!(out, "# {line}");
        }
        let _ = writeln!(out, "# Allowed: {}. Default: {}.", s.allowed(), s.default.value());
        let _ = writeln!(out, "{} = {value}", s.key);
    }
    out
}

fn write_file(path: &Path, settings: &Settings) -> Result<(), SettingsError> {
    let err = |source| SettingsError::Write { path: path.to_path_buf(), source };
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir).map_err(err)?;
    }
    // Written next to the file and renamed over it, so a failed write never
    // leaves a half-written settings file.
    let tmp = path.with_extension("toml.tmp");
    fs::write(&tmp, render(settings)).map_err(err)?;
    fs::rename(&tmp, path).map_err(err)
}

fn wrap(text: &str, width: usize) -> Vec<String> {
    let mut lines = vec![String::new()];
    for word in text.split_whitespace() {
        let line = lines.last_mut().expect("never empty");
        if !line.is_empty() && line.len() + 1 + word.len() > width {
            lines.push(word.to_string());
        } else {
            if !line.is_empty() {
                line.push(' ');
            }
            line.push_str(word);
        }
    }
    lines
}

/// `docs/SETTINGS.md`: every setting of the registry as a reference page.
#[must_use]
pub fn markdown() -> String {
    let mut out = String::from(
        "# Settings\n\n\
         Generated from the settings registry (`crates/disc-core/src/settings.rs`); a test \
         keeps this page in step (`UPDATE_DOCS=1 cargo test -p disc-core` rewrites it).\n\n\
         The settings file (`~/.config/disc-remuxer/settings.toml`, or `--settings FILE`) lists \
         every setting below with its value. It is created on the first run and kept in step \
         with the program on every start: missing settings are added with their default, \
         unknown entries removed, kept values never changed, the old file saved as a backup \
         first. A wrong value stops the program and leaves the file as it is. The value in \
         effect comes from the default, then the settings file, then the command line \
         (`--set group.key=value`).\n",
    );
    let mut group = "";
    for s in SETTINGS {
        if s.group != group {
            group = s.group;
            let _ = write!(out, "\n## [{group}]\n\n| Setting | Allowed | Default | What it does |\n|---|---|---|---|\n");
        }
        let _ = writeln!(out, "| `{}` | {} | `{}` | {} |", s.name(), s.allowed(), s.default.value(), s.help.split_whitespace().collect::<Vec<_>>().join(" "));
    }
    out
}
