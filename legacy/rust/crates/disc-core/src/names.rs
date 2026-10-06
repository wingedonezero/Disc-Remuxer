//! Output names of a title: a template (setting `output.file_name_template`)
//! filled from the title's name, comment, date and number, the way
//! MKV-producing rippers name their files (`<name>_t00`).
//!
//! Template fields: `{VAR}`, `{prefix:VAR}`, `{prefix:VAR:default}`. The
//! prefix is written only when the variable has a value and something has
//! been written before it (or the value is `DFLT`). Variables: `NAME`,
//! `NAME0`..`NAME2` and `CMNT`, `CMNT0`..`CMNT2` (the title's name / comment,
//! cleaned for file names with rule 0, 1 or 2), `DY DM DD TH TM TS DT` (its
//! date `YYYY-MM-DD hh:mm:ss`; `DT` = `YYYYMMDDhhmmss`), `N` / `AN` (the title
//! number), `M` / `AM` (the number + 1), `T` / `AT` (the number, not for
//! title 0), `DFLT` (a value only where nothing was written yet). `+VAR` /
//! `-VAR` test whether a variable has a value. A number variable takes a
//! width: `N2` = two digits. Without a date, `N`, `M` and `T` stand for the
//! number; with one only the `A` forms do. An empty result is `title`.

use std::collections::BTreeMap;

/// The default template.
pub const DEFAULT_TEMPLATE: &str = "{NAME1}{-:CMNT1}{-:DT}{title:+DFLT}{_t:N2}";

/// Most characters a name may have (before its extension).
const MAX: usize = 378;
/// Where an invalid template's result is cut before its error marker.
const MAX_ERR: usize = 358;

/// What a title's name is made from.
#[derive(Debug, Default, Clone)]
pub struct TitleNaming<'a> {
    pub name: Option<&'a str>,
    pub comment: Option<&'a str>,
    /// `YYYY-MM-DD hh:mm:ss`
    pub date: Option<&'a str>,
    /// The title's number in the disc's title list (from 0).
    pub index: u32,
}

/// A name or comment cleaned for file names. Rule 0: characters no file
/// system takes (`/ \ * ; ?` and control characters) become `_`, `|` `I`,
/// `:` `-`, `"` `'`; rule 1 also: `#` `$` -> `_`, middle dot -> `-`, typographic
/// quotes and primes -> `'`; rule 2 also: `.` and space -> `_`. Repeated `_`
/// become one, trailing `_` are removed.
#[must_use]
pub fn clean(s: &str, rule: u8) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        let mut u = match c {
            '/' | '\\' | '*' | ';' | '?' => '_',
            c if (c as u32) < 0x20 => '_',
            '|' => 'I',
            ':' => '-',
            '"' => '\'',
            c => c,
        };
        if rule >= 1 && (u == '#' || u == '$') {
            u = '_';
        }
        if rule >= 2 && (u == '.' || u == ' ') {
            u = '_';
        }
        if rule >= 1 {
            if u == '\u{b7}' {
                u = '-';
            }
            if matches!(u, '\u{2018}' | '\u{2019}' | '\u{201b}' | '\u{2032}' | '\u{2033}') {
                u = '\'';
            }
        }
        if u == '_' && out.ends_with('_') {
            continue;
        }
        out.push(u);
    }
    while out.ends_with('_') {
        out.pop();
    }
    out
}

/// A variable's value: text, a number (width forms allowed), the DFLT
/// marker, or none.
#[derive(Debug, Clone, PartialEq)]
enum Val {
    Text(String),
    Num(String),
    Dflt,
    None,
}

fn vars(t: &TitleNaming<'_>) -> BTreeMap<&'static str, Val> {
    let mut d = BTreeMap::new();
    d.insert("DFLT", Val::Dflt);
    let text = |s: Option<String>| s.map_or(Val::None, Val::Text);
    let name = t.name;
    let comment = t.comment;
    for (k, r) in [("NAME", 0), ("NAME0", 0), ("NAME1", 1), ("NAME2", 2)] {
        d.insert(k, text(name.map(|n| clean(n, r))));
    }
    for (k, r) in [("CMNT", 0), ("CMNT0", 0), ("CMNT1", 1), ("CMNT2", 2)] {
        d.insert(k, text(comment.map(|c| clean(c, r))));
    }
    let date = t.date.filter(|s| s.len() >= 19 && s.is_ascii());
    if let Some(s) = date {
        let parts = [&s[0..4], &s[5..7], &s[8..10], &s[11..13], &s[14..16], &s[17..19]];
        for (k, p) in ["DY", "DM", "DD", "TH", "TM", "TS"].iter().zip(parts) {
            d.insert(k, Val::Text(p.to_string()));
        }
        d.insert("DT", Val::Text(parts.concat()));
    } else {
        for k in ["DY", "DM", "DD", "TH", "TM", "TS", "DT"] {
            d.insert(k, Val::None);
        }
    }
    let n = Val::Num(format!("{:02}", t.index));
    let m = Val::Num(format!("{:02}", t.index + 1));
    d.insert("AN", n.clone());
    d.insert("AM", m.clone());
    // with a date, N / M / T stand for nothing and their width forms too
    if date.is_some() {
        for k in ["N", "N:", "M", "M:", "T", "T:"] {
            d.insert(k, Val::None);
        }
    } else {
        d.insert("N", n.clone());
        d.insert("M", m);
        d.insert("T", if t.index == 0 { Val::None } else { n.clone() });
        if t.index == 0 {
            d.insert("T:", Val::None);
        }
    }
    d.insert("AT", if t.index == 0 { Val::None } else { n });
    if t.index == 0 {
        d.insert("AT:", Val::None);
    }
    d
}

/// What a field yields: text, or the DFLT marker (no text; its prefix is
/// written even at the start).
#[derive(Debug, Clone, PartialEq)]
enum Out {
    Text(String),
    Marker,
}

/// A variable's value for the template (None: no value), `Err` for an
/// unknown variable.
fn lookup(vars: &BTreeMap<&'static str, Val>, name: &str, at_start: bool) -> Result<Option<Out>, ()> {
    if let Some(rest) = name.strip_prefix(['+', '-']) {
        if rest.is_empty() || rest.starts_with(['+', '-']) {
            return Err(());
        }
        let value = lookup(vars, rest, at_start)?;
        let yields = if value == Some(Out::Marker) { Out::Marker } else { Out::Text(String::new()) };
        return Ok((name.starts_with('+') == value.is_some()).then_some(yields));
    }
    if let Some(v) = vars.get(name) {
        return Ok(match v {
            Val::Text(s) | Val::Num(s) => Some(Out::Text(s.clone())),
            Val::Dflt => at_start.then_some(Out::Marker),
            Val::None => None,
        });
    }
    // a width form: VAR<digit>
    let bytes = name.as_bytes();
    let last = *bytes.last().unwrap_or(&0);
    if bytes.len() < 2 || !(b'1'..=b'9').contains(&last) || bytes[bytes.len() - 2].is_ascii_digit() {
        return Err(());
    }
    let base = &name[..name.len() - 1];
    if vars.contains_key(format!("{base}:").as_str()) {
        return Ok(None);
    }
    match vars.get(base) {
        Some(Val::Num(s)) => {
            let width = usize::from(last - b'0');
            let digits = s.trim_start_matches('0');
            let digits = if digits.is_empty() { &s[s.len() - 1..] } else { digits };
            Ok(Some(Out::Text(if digits.len() < width { format!("{digits:0>width$}") } else { digits.to_string() })))
        }
        _ => Err(()),
    }
}

fn push(out: &mut String, s: &str) {
    for c in s.chars() {
        if out.chars().count() >= MAX {
            break;
        }
        out.push(c);
    }
}

/// The template filled in; `Err` with what was written before the field
/// that makes it invalid.
fn expand(d: &BTreeMap<&'static str, Val>, tmpl: &str) -> Result<String, String> {
    let mut out = String::new();
    let mut rest = tmpl;
    while let Some(c) = rest.chars().next() {
        if out.chars().count() >= MAX {
            break;
        }
        if c != '{' {
            out.push(c);
            rest = &rest[c.len_utf8()..];
            continue;
        }
        let Some(body_end) = rest.find('}') else { return Err(out) };
        let body = &rest[1..body_end];
        if body.contains('{') {
            return Err(out);
        }
        let (prefix, name, dflt) = match body.split_once(':') {
            None => (None, body, None),
            Some((p, r)) => match r.split_once(':') {
                None => (Some(p), r, None),
                Some((n, df)) => (Some(p), n, Some(df)),
            },
        };
        let at_start = out.is_empty();
        let Ok(mut v) = lookup(d, name, at_start) else { return Err(out) };
        if v.is_none() {
            v = dflt.map(|s| Out::Text(s.to_string()));
        }
        if let Some(v) = v {
            if let Some(p) = prefix {
                if !at_start || v == Out::Marker {
                    push(&mut out, p);
                }
            }
            if let Out::Text(t) = v {
                push(&mut out, &t);
            }
        }
        rest = &rest[body_end + 1..];
    }
    if out.is_empty() {
        out.push_str("title");
    }
    Ok(out)
}

/// A title's output name (without extension). A blank template is the
/// default one; an invalid one gives what it produced before the error
/// (at most 358 characters) with an error marker and the title number.
#[must_use]
pub fn title_name(template: &str, t: &TitleNaming<'_>) -> String {
    let tmpl = if template.trim().is_empty() { DEFAULT_TEMPLATE } else { template };
    match expand(&vars(t), tmpl) {
        Ok(s) => s,
        Err(s) => {
            let s: String = s.chars().take(MAX_ERR).collect();
            format!("{s}!ERRtemplate_t{:02}", t.index)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn name(n: Option<&str>, index: u32) -> String {
        title_name("", &TitleNaming { name: n, index, ..Default::default() })
    }

    #[test]
    fn default_template_is_the_name_and_the_title_number() {
        assert_eq!(name(Some("HD_Feature"), 2), "HD_Feature_t02");
        assert_eq!(name(Some("Blade Runner Branching - Director's Cut"), 0), "Blade Runner Branching - Director's Cut_t00");
        assert_eq!(name(Some("feature"), 0), "feature_t00");
        assert_eq!(name(Some("x"), 123), "x_t123");
    }

    #[test]
    fn without_a_name_the_title_word_stands_in() {
        assert_eq!(name(None, 4), "title_t04");
        assert_eq!(name(Some(""), 0), "title_t00");
    }

    #[test]
    fn cleaning_rules() {
        assert_eq!(clean("A: B|C \"D\" e/f*g;h?i", 0), "A- BIC 'D' e_f_g_h_i");
        assert_eq!(clean("#1 $2 a\u{b7}b \u{2019}c\u{2019}", 1), "_1 _2 a-b 'c'");
        assert_eq!(clean("a.b c", 2), "a_b_c");
        assert_eq!(clean("a//b??", 0), "a_b");
        assert_eq!(clean("_a_", 0), "_a");
        assert_eq!(clean("THE LORD OF THE RINGS: THE FELLOWSHIP OF THE RING (EXT.) PT. 1", 1),
                   "THE LORD OF THE RINGS- THE FELLOWSHIP OF THE RING (EXT.) PT. 1");
    }

    #[test]
    fn comment_date_and_number_forms() {
        let t = TitleNaming { name: Some("Disc"), comment: Some("Extra"), date: Some("2026-10-06 12:34:56"), index: 3 };
        assert_eq!(title_name("", &t), "Disc-Extra-20261006123456");
        assert_eq!(title_name("{NAME}{_t:AN2}{_m:AM3}", &t), "Disc_t03_m004");
        assert_eq!(title_name("{NAME}{_t:N2}", &t), "Disc", "with a date N stands for nothing");
        let t0 = TitleNaming { name: Some("Disc"), index: 0, ..Default::default() };
        assert_eq!(title_name("{NAME}{_:T2}", &t0), "Disc", "T: nothing for title 0");
        assert_eq!(title_name("{x:+NAME}{-NAME}{y:-CMNT}", &t0), "title", "no prefix at the start");
        assert_eq!(title_name("{NAME}{x:+NAME}{-NAME}{y:-CMNT}", &t0), "Discxy");
    }

    #[test]
    fn invalid_templates() {
        let t = TitleNaming { name: Some("Disc"), index: 7, ..Default::default() };
        assert_eq!(title_name("{NAME", &t), "!ERRtemplate_t07");
        assert_eq!(title_name("{NAME}-{NOPE}", &t), "Disc-!ERRtemplate_t07");
        assert_eq!(title_name("x{a{NAME}}", &t), "x!ERRtemplate_t07");
    }
}
