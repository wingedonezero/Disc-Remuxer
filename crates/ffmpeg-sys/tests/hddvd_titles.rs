//! `libavformat/hddvd_titles.c` and `disclang.c`: the HD DVD title plan.
//! With `HDDVD_CORPUS` (a folder of HD DVD images) and
//! `HDDVD_TITLES_REFERENCE` (one `<image name>.titles.txt` per image, every
//! character but A-Z a-z 0-9 _ of the name replaced by _; one line per title:
//! index|name|language|H:MM:SS|bytes|first EVO file|EVOBs|EVOB names|chapters)
//! the plan of every image must give the reference's lines. Titles the
//! reference program drops after its title plan (when it builds their
//! streams) are listed, by first EVO file, in `<image name>.titles.dropped.txt`
//! (lines starting with # are comments): they are taken out of our list and
//! the rest renumbered before the comparison.

mod common;

use std::ffi::{CStr, CString};
use std::fmt::Write as _;
use std::os::raw::c_int;
use std::ptr;

use ffmpeg_sys::discio;
use common::hddvd::{clip, evob, image, plan, playlist, tmap, vti, Evob, File};
use ffmpeg_sys::hddvd;

const INVALIDDATA: c_int = -0x4144_4E49; // AVERROR_INVALIDDATA

fn lang(code: &str) -> Option<String> {
    let c = CString::new(code).unwrap();
    let r = unsafe { hddvd::ff_disc_lang_code(c.as_ptr()) };
    (!r.is_null()).then(|| unsafe { CStr::from_ptr(r) }.to_str().unwrap().to_owned())
}

#[test]
fn language_codes_follow_iso_639_with_three_extras() {
    for (code, want) in [
        ("en", Some("eng")), ("eng", Some("eng")), ("EN", Some("eng")), ("Fre", Some("fre")),
        ("fra", Some("fre")), ("fr", Some("fre")), ("zho", Some("chi")), ("jp", Some("jpn")),
        ("iw", Some("heb")), ("ptb", Some("ptb")), ("PTB", Some("ptb")),
        ("in", None), ("ji", None), ("jw", None), ("mo", None), ("sh", None),
        ("", None), ("e", None), ("engl", None), ("xx", None), ("en\u{e9}", None),
    ] {
        assert_eq!(lang(code).as_deref(), want, "{code}");
    }
}

/// Our plan of an image as reference lines.
fn plan_lines(img: &std::path::Path) -> Vec<String> {
    let path = CString::new(img.to_str().unwrap()).unwrap();
    let (mut src, mut fs, mut vti, mut xs, mut nb, mut plan) =
        (ptr::null_mut(), ptr::null_mut(), ptr::null_mut(), ptr::null_mut(), 0 as c_int, ptr::null_mut());
    let dump = unsafe {
        assert_eq!(discio::ff_discio_source_open_file(ptr::null_mut(), path.as_ptr(), &raw mut src), 0);
        assert_eq!(discio::ff_discio_mount_image(src, ptr::null(), &raw mut fs), 0);
        assert_eq!(hddvd::ff_hddvd_vti_open(ptr::null_mut(), fs, &raw mut vti), 0);
        assert_eq!(hddvd::ff_hddvd_xpl_load(ptr::null_mut(), fs, &raw mut xs, &raw mut nb), 0);
        assert_eq!(hddvd::ff_hddvd_titles_plan(ptr::null_mut(), fs, vti, xs, nb, 0, &raw mut plan), 0);
        let d = hddvd::ff_hddvd_titles_dump(plan);
        let s = CStr::from_ptr(d).to_str().unwrap().to_owned();
        ffmpeg_sys::av_free(d.cast());
        hddvd::ff_hddvd_titles_free(&raw mut plan);
        hddvd::ff_hddvd_xpl_free_all(&raw mut xs, nb);
        hddvd::ff_hddvd_vti_free(&raw mut vti);
        discio::ff_discio_fs_close(&raw mut fs);
        discio::ff_discio_source_free(&raw mut src);
        s
    };
    dump.lines()
        .filter(|l| l.starts_with("title "))
        .map(|l| {
            // title i|name|lang|kind|selected|duration|secs|size|file|clips|map|chapters
            let f: Vec<&str> = l.split('|').collect();
            let secs: u64 = f[6].parse().unwrap();
            let lang = if f[2] == "(null)" { "" } else { f[2] };
            format!(
                "{}|{}|{}|{}:{:02}:{:02}|{}|{}|{}|{}|{}",
                &f[0][6..], f[1], lang, secs / 3600, secs / 60 % 60, secs % 60, f[7], f[8], f[9], f[10], f[11]
            )
        })
        .collect()
}

#[test]
fn corpus_every_title_list_equals_the_reference() {
    let (Ok(dir), Ok(refdir)) = (std::env::var("HDDVD_CORPUS"), std::env::var("HDDVD_TITLES_REFERENCE")) else {
        eprintln!("HDDVD_CORPUS / HDDVD_TITLES_REFERENCE not set: corpus check skipped");
        return;
    };
    let mut images: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|x| x.eq_ignore_ascii_case("iso")))
        .collect();
    images.sort();
    let mut bad = 0;
    for img in images {
        let stem: String = img.file_stem().unwrap().to_str().unwrap().chars()
            .map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' })
            .collect();
        let want: Vec<String> = std::fs::read_to_string(format!("{refdir}/{stem}.titles.txt"))
            .unwrap()
            .lines()
            .map(str::to_owned)
            .collect();
        let dropped: Vec<String> = std::fs::read_to_string(format!("{refdir}/{stem}.titles.dropped.txt"))
            .unwrap_or_default()
            .lines()
            .filter(|l| !l.is_empty() && !l.starts_with('#'))
            .map(str::to_owned)
            .collect();
        let got: Vec<String> = plan_lines(&img)
            .into_iter()
            .filter(|l| !dropped.iter().any(|d| l.split('|').nth(5) == Some(d.as_str())))
            .enumerate()
            .map(|(i, l)| format!("{i}|{}", l.split_once('|').unwrap().1))
            .collect();
        if got == want {
            eprintln!("{stem}: {} titles equal ({} dropped later by the reference)", got.len(), dropped.len());
            continue;
        }
        bad += 1;
        eprintln!("{stem}: DIFFERS ({} titles, reference {})", got.len(), want.len());
        for i in 0..got.len().max(want.len()) {
            let (g, w) = (got.get(i).map_or("-", String::as_str), want.get(i).map_or("-", String::as_str));
            if g != w {
                eprintln!("  ours {g}\n  ref  {w}");
            }
        }
    }
    assert_eq!(bad, 0, "title lists differ");
}

// ---- rules on built images ----

fn disc(evobs: &[Evob], xpl: Vec<u8>, mut extra: Vec<File>) -> common::Img {
    let mut files: Vec<File> = vec![("ADV_OBJ", "VPLST000.XPL", xpl), ("HVDVD_TS", "HVA00001.VTI", vti(evobs))];
    files.append(&mut extra);
    image(&files)
}

/// MAP + EVO files for EVOBs named `names` (each map: one entry of 1 block).
fn files(names: &[&'static str]) -> Vec<File> {
    let mut f = Vec::new();
    for n in names {
        let base = n.strip_suffix(".EVO").unwrap();
        f.push(("HVDVD_TS", &*Box::leak(format!("{base}.MAP").into_boxed_str()), tmap(&[1], 0, 0, false)));
        f.push(("HVDVD_TS", *n, vec![0u8; 16]));
    }
    f
}

fn titles(dump: &str) -> Vec<&str> {
    dump.lines().filter(|l| !l.starts_with("clip ")).collect()
}

const T: &str = "00:00:00:00";

#[test]
fn titles_come_from_seamless_runs_in_reverse_candidate_order() {
    let body = format!(
        r#"<FirstPlayTitle>{d}</FirstPlayTitle><Title displayName="Main">{a}{b}{c}</Title><Title id="extra">{d}</Title>"#,
        d = clip("D.MAP", T, T, false),
        a = clip("A.MAP", T, T, false),
        b = clip("B.MAP", T, T, true),
        c = clip("C.MAP", T, T, false),
    );
    let im = disc(
        &[evob("A.EVO", 1, 10), evob("B.EVO", 2, 20), evob("C.EVO", 3, 30), evob("D.EVO", 4, 40)],
        playlist(r#"defaultLanguage="en""#, &body),
        files(&["A.EVO", "B.EVO", "C.EVO", "D.EVO"]),
    );
    let d = plan(&im, 0).unwrap();
    // candidates sorted: [C], [D] (the FirstPlayTitle's; the Title's is the same), [A,B]
    assert_eq!(
        titles(&d),
        [
            "title 0|Main|eng|playlist|1|2700000|30|4096|A.EVO|2|A,B|0",
            "title 1|extra|eng|playlist|1|3600000|40|2048|D.EVO|1|D|0",
            "title 2|Main|eng|playlist|1|2700000|30|2048|C.EVO|1|C|0",
        ]
    );
}

#[test]
fn an_evob_no_title_plays_becomes_a_title_only_when_there_is_a_title() {
    let im = disc(
        &[evob("A.EVO", 1, 10), evob("E.EVO", 5, 5)],
        playlist("", &format!("<Title>{}</Title>", clip("A.MAP", T, T, false))),
        files(&["A.EVO", "E.EVO"]),
    );
    assert_eq!(
        titles(&plan(&im, 0).unwrap()),
        ["title 0|A||playlist|1|900000|10|2048|A.EVO|1|A|0", "title 1|E|(null)|evob|1|450000|5|2048|E.EVO|1|E|0"]
    );
    // the only playlist title names an EVOB that does not exist: no title at all
    let im = disc(
        &[evob("E.EVO", 5, 5)],
        playlist("", &format!("<Title>{}</Title>", clip("Z.MAP", T, T, false))),
        files(&["E.EVO"]),
    );
    assert_eq!(plan(&im, 0).unwrap(), "");
}

#[test]
fn clip_names_match_the_first_evob_up_to_the_first_dot_without_case() {
    let body = format!(
        r#"<Title displayName="Feature"><PrimaryAudioVideoClip src="file:///dvddisc/hvdvd_ts/feature_1.map"/></Title><Title>{}</Title><Title><PrimaryAudioVideoClip src="file:///other/B.MAP"/></Title>"#,
        clip("B.EVO", T, T, false)
    );
    let mut f = files(&["FEATURE_1.EVO", "B.EVO"]);
    f.push(("HVDVD_TS", "FEATURE_1.ALT", vec![0u8; 16]));
    let im = disc(&[evob("FEATURE_1.ALT", 1, 10), evob("FEATURE_1.EVO", 2, 20), evob("B.EVO", 3, 1)], playlist("", &body), f);
    // feature_1.map -> slot 1 FEATURE_1.ALT (the first match); its name is not "*.evo", so
    // no playlist Title names it: its base name. FEATURE_1.EVO (same time map) is played
    // by no title: a title of its own, named by the Title whose clip names it. The clips
    // "B.EVO" (no .map) and file:///other/ (other place) give no name: those titles are
    // left out; B.EVO becomes a title of its own.
    assert_eq!(
        titles(&plan(&im, 0).unwrap()),
        [
            "title 0|FEATURE_1||playlist|1|900000|10|2048|FEATURE_1.ALT|1|FEATURE_1|0",
            "title 1|Feature|(null)|evob|1|1800000|20|2048|FEATURE_1.EVO|1|FEATURE_1|0",
            "title 2|B|(null)|evob|1|90000|1|2048|B.EVO|1|B|0",
        ]
    );
}

#[test]
fn an_evob_whose_time_map_cannot_be_used_loses_its_titles() {
    let mut bad_magic = tmap(&[1], 0, 0, false);
    bad_magic[..12].copy_from_slice(b"HDDVD_TMAP01");
    let mut no_table = tmap(&[1], 0, 0, false);
    no_table[0x37..0x39].fill(0);
    let names = ["G1", "G2", "G3", "G4", "G5", "H"];
    let body = names.iter().fold(String::new(), |mut b, n| {
        let _ = write!(b, "<Title>{}</Title>", clip(&format!("{n}.MAP"), T, T, false));
        b
    });
    let evobs: Vec<Evob> = names
        .iter()
        .enumerate()
        .map(|(i, n)| evob(Box::leak(format!("{n}.EVO").into_boxed_str()), u16::try_from(i + 1).unwrap(), 1))
        .collect();
    let mut f: Vec<File> = vec![
        ("HVDVD_TS", "G1.MAP", bad_magic),
        ("HVDVD_TS", "G2.MAP", tmap(&[1], 0, 0x02, false)),
        ("HVDVD_TS", "G3.MAP", no_table),
        ("HVDVD_TS", "G5.MAP", tmap(&[1], 0, 0, false)),
    ];
    for n in ["G1.EVO", "G2.EVO", "G3.EVO", "G4.EVO"] {
        f.push(("HVDVD_TS", n, vec![0u8; 16]));
    }
    f.extend(files(&["H.EVO"]));
    let d = plan(&disc(&evobs, playlist("", &body), f), 0).unwrap();
    assert_eq!(titles(&d), ["title 0|H||playlist|1|90000|1|2048|H.EVO|1|H|0"]);
    let clips: Vec<&str> = d.lines().filter(|l| l.starts_with("clip ")).collect();
    assert_eq!(
        clips,
        [
            "clip 1|G1.EVO|not usable|0|",
            "clip 2|G2.EVO|not usable|0|",
            "clip 3|G3.EVO|not usable|0|",
            "clip 4|G4.EVO|not usable|0|",
            "clip 5|G5.EVO|not usable|0|",
            "clip 6|H.EVO|usable|2048|0:0:1",
        ]
    );
}

#[test]
fn the_size_counts_every_entry_and_an_entry_at_a_taken_block_is_not_mapped() {
    // entries of 5, 0 and 3 blocks: the 3-block entry starts at block 5, taken by the
    // 0-block entry; the first table's value 7 is reported, a second table is read, the
    // top 3 bits of an entry are not part of the count
    let im = disc(
        &[evob("A.EVO", 1, 10)],
        playlist("", &format!("<Title>{}</Title>", clip("A.MAP", T, T, false))),
        vec![("HVDVD_TS", "A.MAP", tmap(&[5, 0, 3], 7, 0x20, true)), ("HVDVD_TS", "A.EVO", vec![0u8; 16])],
    );
    let d = plan(&im, 0).unwrap();
    assert!(d.contains("title 0|A||playlist|1|900000|10|16384|A.EVO|1|A|0\n"), "{d}");
    assert!(d.contains("clip 1|A.EVO|usable|16384|0:0:5,5:5:0\n"), "{d}");
}

fn chapter_disc(time_base: &str) -> common::Img {
    let chapters = [("00:00:00:00", "One"), ("00:00:30:30", "Two"), ("00:01:00:00", "Three"), ("00:01:30:00", "Four"), ("00:02:59:59", "Five")]
        .iter()
        .fold(String::new(), |mut c, (t, n)| {
            let _ = write!(c, r#"<Chapter titleTimeBegin="{t}" displayName="{n}"/>"#);
            c
        });
    let body = format!(
        "<Title>{}{}<ChapterList>{chapters}</ChapterList><ChapterList><Chapter/></ChapterList></Title>",
        clip("C1.MAP", "00:00:00:00", "00:01:00:00", false),
        clip("C2.MAP", "00:01:00:00", "00:03:00:00", false)
    );
    disc(
        &[evob("C1.EVO", 1, 60), evob("C2.EVO", 2, 120)],
        playlist(&format!(r#"timeBase="{time_base}""#), &body),
        files(&["C1.EVO", "C2.EVO"]),
    )
}

#[test]
fn chapters_are_those_of_the_first_chapter_list_in_the_runs_window() {
    // C1 (run 0): window 0..60000 ms. C2 (run 1): the window starts at 1 x the duration
    // of C2 itself (120000, not C1's 60000) and lasts 120000: only "Five" (179983) is in it
    let d = plan(&chapter_disc("60fps"), 0).unwrap();
    assert_eq!(
        titles(&d),
        [
            "title 0|C2||playlist|1|10800000|120|2048|C2.EVO|1|C2|1",
            "  chapter 1|59983|Five",
            "title 1|C1||playlist|1|5400000|60|2048|C1.EVO|1|C1|3",
            "  chapter 1|0|One",
            "  chapter 2|30500|Two",
            "  chapter 3|60000|Three",
        ]
    );
    // 50 frames per second: frame 30 is 600 ms
    assert!(plan(&chapter_disc("50fps"), 0).unwrap().contains("  chapter 2|30600|Two\n"));
    // a timeBase that is not 5 characters long: every time is 0, every chapter kept at 0
    let d = plan(&chapter_disc("60"), 0).unwrap();
    assert_eq!(d.matches("|0|").count(), 10, "{d}");
}

#[test]
fn the_language_is_the_title_sets_default_as_an_iso_639_2_code_when_known() {
    for (attr, want) in [("fr", "fre"), ("FRE", "fre"), ("xx", "xx"), ("", "")] {
        let im = disc(
            &[evob("A.EVO", 1, 10)],
            playlist(&format!(r#"defaultLanguage="{attr}""#), &format!("<Title>{}</Title>", clip("A.MAP", T, T, false))),
            files(&["A.EVO"]),
        );
        let d = plan(&im, 0).unwrap();
        assert!(d.starts_with(&format!("title 0|A|{want}|")), "{attr}: {d}");
    }
}

#[test]
fn titles_shorter_than_the_minimum_are_not_selected_and_do_not_count_as_playing() {
    // the only title is short: not selected, and no EVOB gets a title of its own
    let im = disc(
        &[evob("A.EVO", 1, 5), evob("E.EVO", 2, 50)],
        playlist("", &format!("<Title>{}</Title>", clip("A.MAP", T, T, false))),
        files(&["A.EVO", "E.EVO"]),
    );
    assert_eq!(titles(&plan(&im, 10).unwrap()), ["title 0|A||playlist|0|450000|5|2048|A.EVO|1|A|0"]);
    // with a selected title, the short title's EVOB is played by no selected title
    let body = format!("<Title>{}</Title><Title>{}</Title>", clip("A.MAP", T, T, false), clip("B.MAP", T, T, false));
    let im = disc(&[evob("A.EVO", 1, 5), evob("B.EVO", 2, 50)], playlist("", &body), files(&["A.EVO", "B.EVO"]));
    assert_eq!(
        titles(&plan(&im, 10).unwrap()),
        [
            "title 0|B||playlist|1|4500000|50|2048|B.EVO|1|B|0",
            "title 1|A||playlist|0|450000|5|2048|A.EVO|1|A|0",
            "title 2|A|(null)|evob|0|450000|5|2048|A.EVO|1|A|0",
        ]
    );
}

#[test]
fn every_playlist_needs_exactly_one_title_set() {
    let xpl = b"<Playlist><TitleSet/><TitleSet/></Playlist>".to_vec();
    assert_eq!(plan(&disc(&[evob("A.EVO", 1, 1)], xpl, files(&["A.EVO"])), 0), Err(INVALIDDATA));
    let xpl = b"<Playlist/>".to_vec();
    assert_eq!(plan(&disc(&[evob("A.EVO", 1, 1)], xpl, files(&["A.EVO"])), 0), Err(INVALIDDATA));
}
