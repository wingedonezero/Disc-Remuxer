//! The message catalog is well formed and docs/SETTINGS.md matches the registry.

use std::collections::HashSet;

use disc_core::msg::{self, ALL};

#[test]
fn message_numbers_are_unique_and_in_their_area() {
    let mut seen = HashSet::new();
    for m in ALL {
        assert!(seen.insert(m.id), "message {} twice", m.id);
        assert!((1000..7000).contains(&m.id), "message {} outside the areas", m.id);
        assert!(!m.text.is_empty() && !m.text.ends_with('.'), "message {} text", m.id);
        for name in msg::fields(m) {
            assert!(!name.is_empty() && name.chars().all(|c| c.is_ascii_lowercase() || c == '_'), "message {} field {name:?}", m.id);
        }
    }
    let ids: Vec<_> = ALL.iter().map(|m| m.id).collect();
    let mut sorted = ids.clone();
    sorted.sort_unstable();
    assert_eq!(ids, sorted, "catalog not in number order");
}

#[test]
fn fields_are_filled_in() {
    let text = msg::format(&msg::JOB_START, &[("index", &2), ("count", &5), ("source", &"/discs/A")]);
    assert_eq!(text, "Job 2 of 5: /discs/A");
}

#[test]
fn settings_doc_matches_the_registry() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../docs/SETTINGS.md");
    let want = disc_core::settings::markdown();
    if std::env::var_os("UPDATE_DOCS").is_some() {
        std::fs::write(path, &want).unwrap();
    }
    let have = std::fs::read_to_string(path).unwrap_or_default();
    assert!(have == want, "docs/SETTINGS.md is out of date: run UPDATE_DOCS=1 cargo test -p disc-core");
}
