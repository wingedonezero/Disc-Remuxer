//! `libavformat/hddvd_xpl.c`: the HD DVD playlists. The parsing rules on
//! built XML; the loader (file numbering, AACS header, files left out) on
//! built ISO 9660 images; with `HDDVD_CORPUS` (a folder of HD DVD images)
//! and `HDDVD_XPL_REFERENCE` (dumps of their VPLST000.XPL made by an
//! independent reader, one `<image name>.VPLST000.txt` each, every character
//! but A-Z a-z 0-9 _ of the name replaced by _) the dumps must be equal.

mod common;

use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;

use common::{iso, Img, D, S};
use ffmpeg_sys::discio::{self, Fs, Source, SourceOps};
use ffmpeg_sys::hddvd::{self, Xpl};

const INVALIDDATA: c_int = -0x4144_4E49; // AVERROR_INVALIDDATA

// ---- parsing from memory ----

struct Mem {
    data: Vec<u8>,
    reads: RefCell<Vec<(i64, c_int)>>,
}

unsafe extern "C" fn mem_read(opaque: *mut c_void, pos: i64, buf: *mut u8, len: c_int) -> c_int {
    // SAFETY: opaque is the Mem the caller keeps alive; buf has len bytes.
    let m = unsafe { &*opaque.cast::<Mem>() };
    m.reads.borrow_mut().push((pos, len));
    let (p, n) = (usize::try_from(pos).unwrap(), usize::try_from(len).unwrap());
    if p + n > m.data.len() {
        return -22;
    }
    unsafe { ptr::copy_nonoverlapping(m.data[p..].as_ptr(), buf, n) };
    0
}

fn dump(x: *const Xpl) -> String {
    unsafe {
        let d = hddvd::ff_hddvd_xpl_dump(x);
        assert!(!d.is_null());
        let s = CStr::from_ptr(d).to_str().unwrap().to_owned();
        ffmpeg_sys::av_free(d.cast());
        s
    }
}

/// Parse `xml` whole; Ok(dump) or Err(code).
fn parse(xml: &str) -> Result<String, c_int> {
    parse_at(xml.as_bytes().to_vec(), 0, i64::try_from(xml.len()).unwrap()).0
}

fn parse_at(data: Vec<u8>, offset: i64, length: i64) -> (Result<String, c_int>, Vec<(i64, c_int)>) {
    install_log();
    let m = Mem { data, reads: RefCell::new(Vec::new()) };
    let mut x = ptr::null_mut();
    let ret = unsafe {
        hddvd::ff_hddvd_xpl_parse(ptr::null_mut(), mem_read, ptr::from_ref(&m).cast_mut().cast(), offset, length, &raw mut x)
    };
    let out = if ret < 0 {
        assert!(x.is_null());
        Err(ret)
    } else {
        let d = dump(x);
        unsafe { hddvd::ff_hddvd_xpl_free(&raw mut x) };
        Ok(d)
    };
    (out, m.reads.into_inner())
}

static LOG: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());

unsafe extern "C" fn log_sink(_level: c_int, line: *const c_char) {
    // SAFETY: the glue passes a NUL-terminated line.
    let text = unsafe { CStr::from_ptr(line) }.to_string_lossy().into_owned();
    LOG.lock().unwrap().push(text);
}

fn install_log() {
    // SAFETY: log_sink is a valid callback for the whole test run.
    unsafe { ffmpeg_sys::dr_log_install(log_sink, ffmpeg_sys::log_level::DEBUG) };
}

fn logged(part: &str) -> bool {
    LOG.lock().unwrap().iter().any(|l| l.contains(part))
}

const PLAYLIST: &str = r#"Playlist description="" displayName="" majorVersion=0 minorVersion=0 type="Advanced""#;
const TITLESET: &str = r#"TitleSet defaultLanguage="" tickBase="" timeBase="""#;

// ---- rules ----

#[test]
fn every_attribute_of_a_type_starts_at_its_default() {
    let d = parse(r"<Playlist><TitleSet><Title><PrimaryAudioVideoClip/><ChapterList><Chapter/></ChapterList></Title></TitleSet></Playlist>").unwrap();
    let want = format!(
        "{PLAYLIST}\n  {TITLESET}\n    Title alternativeSDDisplayMode=\"panscanOrLetterbox\" base=\"\" description=\"\" \
         displayName=\"\" id=\"\" onEnd=\"\" parentalLevel=\"*:1\" selectable=1 tickBaseDivisor=\"1\" titleDuration=\"\" \
         titleNumber=\"\" type=\"Advanced\"\n      PrimaryAudioVideoClip clipTimeBegin=\"00:00:00:00\" dataSource=\"Disc\" \
         description=\"\" id=\"\" seamless=0 src=\"\" titleTimeBegin=\"\" titleTimeEnd=\"\"\n      ChapterList\n        \
         Chapter description=\"\" displayName=\"\" id=\"\" titleTimeBegin=\"\"\n"
    );
    assert_eq!(d, want);
}

#[test]
fn unknown_elements_are_skipped_with_their_content_and_unknown_attributes_ignored() {
    let d = parse(
        r#"<Playlist><Unknown><TitleSet/></Unknown><TitleSet timeBase="60fps" other="x"><Title><Video/></Title></TitleSet></Playlist>"#,
    )
    .unwrap();
    // the TitleSet inside <Unknown> is not read, <Video> is no child of <Title>
    assert_eq!(d, format!("{PLAYLIST}\n  TitleSet defaultLanguage=\"\" tickBase=\"\" timeBase=\"60fps\"\n    Title alternativeSDDisplayMode=\"panscanOrLetterbox\" base=\"\" description=\"\" displayName=\"\" id=\"\" onEnd=\"\" parentalLevel=\"*:1\" selectable=1 tickBaseDivisor=\"1\" titleDuration=\"\" titleNumber=\"\" type=\"Advanced\"\n"));
    assert!(logged("XPL: <Unknown> in <Playlist> is not read"));
}

#[test]
fn names_are_matched_exactly_after_a_namespace_prefix() {
    let d = parse(r#"<x:Playlist xmlns:x="urn:x"><x:TitleSet x:timeBase="50fps" TimeBase="no"/><titleset/></x:Playlist>"#).unwrap();
    assert_eq!(d, format!("{PLAYLIST}\n  TitleSet defaultLanguage=\"\" tickBase=\"\" timeBase=\"50fps\"\n"));
}

#[test]
fn numbers_are_read_as_strtoul_and_booleans_are_true_or_yes() {
    let d = parse(
        r#"<Playlist majorVersion=" 12abc" minorVersion="x"><TitleSet><Title selectable="1"/><Title selectable="YES"/><Title selectable="True"/><Title selectable="false"/></TitleSet></Playlist>"#,
    )
    .unwrap();
    assert!(d.starts_with(r#"Playlist description="" displayName="" majorVersion=12 minorVersion=0"#), "{d}");
    let sel: Vec<&str> = d.lines().filter(|l| l.contains("Title ")).map(|l| if l.contains("selectable=1") { "1" } else { "0" }).collect();
    assert_eq!(sel, ["0", "1", "1", "0"]);
    // a number beyond 32 bits is cut to its low 32 bits, a repeated attribute is XML's error
    let d = parse(r#"<Playlist majorVersion="4294967297"/>"#).unwrap();
    assert!(d.contains("majorVersion=1 "), "{d}");
}

#[test]
fn an_element_without_text_gets_its_last_childs_text_and_what_follows_it() {
    let d = parse("<Playlist><TitleSet>\n<Title>abc</Title>\n</TitleSet></Playlist>").unwrap();
    // Title: "abc"; TitleSet and Playlist: "abc" and the newline after </Title>
    // (the buffer is emptied only when an element starts)
    let title = "Title alternativeSDDisplayMode=\"panscanOrLetterbox\" base=\"\" description=\"\" displayName=\"\" \
                 id=\"\" onEnd=\"\" parentalLevel=\"*:1\" selectable=1 tickBaseDivisor=\"1\" titleDuration=\"\" \
                 titleNumber=\"\" type=\"Advanced\"";
    assert_eq!(d, format!("{PLAYLIST} text=\"abc\n\"\n  {TITLESET} text=\"abc\n\"\n    {title} text=\"abc\"\n"));
    // the start of an element empties the buffer
    let d = parse("<Playlist>lost<TitleSet/></Playlist>").unwrap();
    assert!(!d.contains("lost"), "{d}");
}

#[test]
fn text_inside_a_skipped_element_is_collected_and_text_stops_at_4096_bytes() {
    let d = parse("<Playlist><TitleSet><Skipped>in skipped</Skipped></TitleSet></Playlist>").unwrap();
    assert!(d.contains("TitleSet defaultLanguage=\"\" tickBase=\"\" timeBase=\"\" text=\"in skipped\""), "{d}");
    let long = "y".repeat(5000);
    let d = parse(&format!("<Playlist><TitleSet>{long}</TitleSet></Playlist>")).unwrap();
    let t = d.lines().nth(1).unwrap();
    assert_eq!(t.matches('y').count(), 4096, "{t}");
}

#[test]
fn a_root_other_than_playlist_leaves_an_empty_playlist() {
    let d = parse(r"<Other><TitleSet/></Other>").unwrap();
    assert_eq!(d, "Playlist description=(null) displayName=(null) majorVersion=0 minorVersion=0 type=(null)\n");
}

#[test]
fn xml_that_is_not_well_formed_is_an_error() {
    assert_eq!(parse("<Playlist><TitleSet></Playlist>").err(), Some(INVALIDDATA));
    assert!(logged("XPL: not well-formed XML: expat error 7 (mismatched tag)"));
    assert_eq!(parse("").err(), Some(INVALIDDATA));
    assert!(logged("XPL: not well-formed XML: expat error 3 (no element found)"));
}

#[test]
fn reads_are_at_most_16_kib_and_stay_inside_2048_byte_blocks_until_aligned() {
    let mut xml = format!("<Playlist>{}</Playlist>", " ".repeat(40000)).into_bytes();
    let len = i64::try_from(xml.len()).unwrap();
    let mut data = vec![0u8; 0x11b];
    data.append(&mut xml);
    let (r, reads) = parse_at(data, 0x11b, len);
    assert!(r.is_ok());
    let mut want = vec![(0x11b, 0x800 - 0x11b)];
    let mut pos = 0x800;
    while pos < 0x11b + len {
        let n = (0x11b + len - pos).min(0x4000);
        want.push((pos, c_int::try_from(n).unwrap()));
        pos += n;
    }
    assert_eq!(reads, want);
}

// ---- the loader on built images ----

struct ImgMem(Vec<u8>);

unsafe extern "C" fn img_read(opaque: *mut c_void, pos: i64, buf: *mut u8, len: c_int) -> c_int {
    // SAFETY: opaque is the boxed ImgMem of `load`.
    let m = unsafe { &*opaque.cast::<ImgMem>() };
    let (p, n) = (usize::try_from(pos).unwrap(), usize::try_from(len).unwrap());
    if p >= m.0.len() {
        return 0;
    }
    let k = n.min(m.0.len() - p);
    unsafe { ptr::copy_nonoverlapping(m.0.as_ptr().add(p), buf, k) };
    c_int::try_from(k).unwrap()
}

unsafe extern "C" fn img_close(opaque: *mut c_void) {
    // SAFETY: opaque came from Box::into_raw in `load`.
    drop(unsafe { Box::from_raw(opaque.cast::<ImgMem>()) });
}

static IMG_OPS: SourceOps = SourceOps { read_at: img_read, close: Some(img_close) };

/// Mounts `im` and loads its playlists: Ok(file numbers) or Err(code).
fn load(im: &Img) -> Result<Vec<c_int>, c_int> {
    install_log();
    let opaque = Box::into_raw(Box::new(ImgMem(im.d.clone()))).cast::<c_void>();
    let size = i64::try_from(im.d.len()).unwrap();
    let mut src: *mut Source = unsafe { discio::ff_discio_source_new(ptr::null_mut(), c"memory".as_ptr(), size, &raw const IMG_OPS, opaque) };
    let mut fs: *mut Fs = ptr::null_mut();
    assert_eq!(unsafe { discio::ff_discio_mount_image(src, ptr::null(), &raw mut fs) }, 0);
    let (mut xs, mut nb) = (ptr::null_mut(), 0);
    let ret = unsafe { hddvd::ff_hddvd_xpl_load(ptr::null_mut(), fs, &raw mut xs, &raw mut nb) };
    let out = if ret < 0 {
        Err(ret)
    } else {
        let v = (0..usize::try_from(nb).unwrap()).map(|i| unsafe { (**xs.add(i)).file }).collect();
        unsafe { hddvd::ff_hddvd_xpl_free_all(&raw mut xs, nb) };
        Ok(v)
    };
    unsafe {
        discio::ff_discio_fs_close(&raw mut fs);
        discio::ff_discio_source_free(&raw mut src);
    }
    out
}

/// An ISO 9660 image with `files` in `ADV_OBJ` (name, content), from sector 100.
fn image(files: &[(&'static str, Vec<u8>)]) -> Img {
    let mut im = Img::new();
    let mut entries = Vec::new();
    for (k, (name, data)) in files.iter().enumerate() {
        let sector = 100 + 4 * u32::try_from(k).unwrap();
        assert!(data.len() <= 4 * S);
        im.put(sector, data);
        entries.push((*name, sector, u32::try_from(data.len()).unwrap()));
    }
    let dirs: Vec<D> = vec![("ADV_OBJ", entries)];
    iso(&mut im, "HDDVD", &dirs, None);
    im
}

fn aacs(kind: u8, xml: &str) -> Vec<u8> {
    let mut d = vec![0u8; 0x11b];
    d[..4].copy_from_slice(b"AACS");
    d[4] = kind;
    d[7..11].copy_from_slice(&u32::try_from(xml.len()).unwrap().to_be_bytes());
    d.extend_from_slice(xml.as_bytes());
    d.extend_from_slice(b"trailing bytes after the XML");
    d
}

const XML: &str = "<Playlist><TitleSet/></Playlist>";

#[test]
fn playlists_are_read_up_to_the_first_missing_file_and_bad_ones_left_out() {
    let im = image(&[
        ("VPLST000.XPL", XML.as_bytes().to_vec()),
        ("VPLST001.XPL", aacs(0x12, XML)),
        ("VPLST002.XPL", b"<Playlist>".to_vec()),
        ("VPLST003.XPL", aacs(0x05, XML)),
        ("VPLST004.XPL", aacs(0x21, XML)),
        ("VPLST005.XPL", aacs(0x02, XML)),
        ("VPLST006.XPL", b"AAC".to_vec()),
        // VPLST007 missing: VPLST008 is not read
        ("VPLST008.XPL", XML.as_bytes().to_vec()),
    ]);
    assert_eq!(load(&im), Ok(vec![0, 1, 4, 5]));
    assert!(logged("/ADV_OBJ/VPLST001.XPL: AACS header type 0x12, 32 bytes of XML at 283"));
    assert!(logged("/ADV_OBJ/VPLST002.XPL: playlist left out"));
    assert!(logged("/ADV_OBJ/VPLST003.XPL: AACS header type 0x05 (0x02, 0x12, 0x21); playlist left out"));
    assert!(logged("/ADV_OBJ/VPLST006.XPL: cannot read its first bytes; playlist left out"));
}

#[test]
fn without_a_readable_playlist_the_disc_has_none() {
    assert_eq!(load(&image(&[("VPLST000.XPL", b"<x>".to_vec())])), Err(INVALIDDATA));
    assert_eq!(load(&image(&[("VPLST001.XPL", XML.as_bytes().to_vec())])), Err(INVALIDDATA));
    assert!(logged("No playlist (/ADV_OBJ/VPLST000.XPL ...) could be read"));
}

// ---- corpus ----

#[test]
fn corpus_every_playlist_equals_the_independent_reading() {
    let (Ok(dir), Ok(refdir)) = (std::env::var("HDDVD_CORPUS"), std::env::var("HDDVD_XPL_REFERENCE")) else {
        eprintln!("HDDVD_CORPUS / HDDVD_XPL_REFERENCE not set: corpus check skipped");
        return;
    };
    install_log();
    let mut images: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|x| x.eq_ignore_ascii_case("iso")))
        .collect();
    images.sort();
    assert!(!images.is_empty());
    for img in images {
        let stem: String = img
            .file_stem()
            .unwrap()
            .to_str()
            .unwrap()
            .chars()
            .map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' })
            .collect();
        let want = std::fs::read_to_string(format!("{refdir}/{stem}.VPLST000.txt")).unwrap();
        let path = CString::new(img.to_str().unwrap()).unwrap();
        let (mut src, mut fs, mut xs, mut nb) = (ptr::null_mut(), ptr::null_mut(), ptr::null_mut(), 0);
        unsafe {
            assert_eq!(discio::ff_discio_source_open_file(ptr::null_mut(), path.as_ptr(), &raw mut src), 0);
            assert_eq!(discio::ff_discio_mount_image(src, ptr::null(), &raw mut fs), 0);
            assert_eq!(hddvd::ff_hddvd_xpl_load(ptr::null_mut(), fs, &raw mut xs, &raw mut nb), 0);
        }
        assert_eq!(nb, 1, "{}", img.display());
        let got = dump(unsafe { *xs });
        if got != want {
            let first = got.lines().zip(want.lines()).position(|(a, b)| a != b);
            panic!("{}: differs at line {first:?}", img.display());
        }
        eprintln!("{}: VPLST000.XPL equal ({} lines)", img.display(), got.lines().count());
        unsafe {
            hddvd::ff_hddvd_xpl_free_all(&raw mut xs, nb);
            discio::ff_discio_fs_close(&raw mut fs);
            discio::ff_discio_source_free(&raw mut src);
        }
    }
}
