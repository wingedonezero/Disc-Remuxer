//! The settings file: created with every setting, kept in step with the
//! registry without changing the user's values, refused when a value is wrong.

use std::fs;
use std::path::PathBuf;

use disc_core::settings::{self, Settings, SettingsError, Source, SETTINGS};

/// A fresh folder for one test under the system temp folder.
fn folder(test: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("disc-core-settings-{test}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn missing_file_is_created_with_every_setting_at_its_default() {
    let path = folder("create").join("sub").join("settings.toml");
    let (s, report) = settings::load(&path).unwrap();
    assert!(report.created);
    let text = fs::read_to_string(&path).unwrap();
    for setting in SETTINGS {
        assert!(text.contains(&format!("\n{} = ", setting.key)), "{} missing", setting.name());
        assert!(text.contains(&format!("[{}]", setting.group)));
    }
    assert!(s.iter().all(|(_, _, src)| src == Source::Default));

    // Loading again changes nothing.
    let (_, again) = settings::load(&path).unwrap();
    assert!(!again.created && again.added.is_empty() && again.removed.is_empty() && again.backup.is_none());
    assert_eq!(fs::read_to_string(&path).unwrap(), text);
}

#[test]
fn file_is_brought_in_step_and_user_values_are_kept() {
    let dir = folder("upgrade");
    let path = dir.join("settings.toml");
    let old = "[output]\nscan_depth = 9\nroot = \"/media/rips \\\"x\\\"\"\nold_option = 3\n\n[gone]\nthing = true\n";
    fs::write(&path, old).unwrap();

    let (s, report) = settings::load(&path).unwrap();
    assert_eq!(s.number("output.scan_depth"), 9);
    assert_eq!(s.text("output.root"), Some("/media/rips \"x\""));
    let added: Vec<_> = report.added.iter().map(|a| a.0.as_str()).collect();
    assert!(added.contains(&"output.keep_structure") && added.contains(&"log.console"));
    let removed: Vec<_> = report.removed.iter().map(|r| r.0.as_str()).collect();
    assert_eq!(removed, ["gone.thing", "output.old_option"]);

    // The previous file is kept as a backup, unchanged.
    let backup = report.backup.expect("backup written");
    assert_eq!(fs::read_to_string(&backup).unwrap(), old);

    // The rewritten file reads back to the same values and is complete.
    let (again, report2) = settings::load(&path).unwrap();
    assert!(report2.added.is_empty() && report2.removed.is_empty() && report2.backup.is_none());
    assert_eq!(again.number("output.scan_depth"), 9);
    assert_eq!(again.text("output.root"), Some("/media/rips \"x\""));
    assert!(again.switch("output.keep_structure"));
}

#[test]
fn wrong_values_stop_and_leave_the_file_untouched() {
    let dir = folder("invalid");
    let path = dir.join("settings.toml");
    for bad in [
        "[output]\nscan_depth = \"five\"\n",
        "[output]\nscan_depth = 99\n",
        "[log]\nconsole = \"loud\"\n",
        "[output]\nkeep_structure = 1\n",
    ] {
        fs::write(&path, bad).unwrap();
        let err = settings::load(&path).unwrap_err();
        assert!(matches!(err, SettingsError::Invalid { .. }), "{bad}: {err}");
        assert_eq!(fs::read_to_string(&path).unwrap(), bad);
    }
    fs::write(&path, "this is = = not toml").unwrap();
    assert!(matches!(settings::load(&path).unwrap_err(), SettingsError::Syntax { .. }));
    assert_eq!(fs::read_dir(&dir).unwrap().count(), 1, "no backup or temp file left behind");
}

#[test]
fn command_line_overrides_the_file() {
    let mut s = Settings::defaults();
    s.set_from_command_line("output.scan_depth=2").unwrap();
    s.set_from_command_line("log.debug_file = on").unwrap();
    assert_eq!(s.number("output.scan_depth"), 2);
    assert!(s.switch("log.debug_file"));
    let src: Vec<_> = s.iter().filter(|(_, _, src)| *src == Source::CommandLine).map(|(st, _, _)| st.name()).collect();
    assert_eq!(src, ["output.scan_depth", "log.debug_file"]);

    assert!(matches!(s.set_from_command_line("output.nope=1"), Err(SettingsError::Unknown { .. })));
    assert!(matches!(s.set_from_command_line("output.scan_depth=21"), Err(SettingsError::Invalid { .. })));
    assert!(matches!(s.set_from_command_line("no-equals"), Err(SettingsError::BadOverride(_))));
}
