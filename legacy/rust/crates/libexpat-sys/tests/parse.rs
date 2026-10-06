//! The compiled expat: its version, and a parse with element and character
//! data callbacks (the calls the HD DVD playlist reader makes).

use libexpat_sys as x;
use std::ffi::{CStr, CString};
use std::fmt::Write as _;
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;

#[derive(Default)]
struct Seen {
    events: Vec<String>,
}

unsafe extern "C" fn start(ud: *mut c_void, name: *const c_char, atts: *mut *const c_char) {
    let seen = unsafe { &mut *ud.cast::<Seen>() };
    let mut ev = format!("<{}", unsafe { CStr::from_ptr(name) }.to_str().unwrap());
    let mut i = 0;
    loop {
        let k = unsafe { *atts.add(i) };
        if k.is_null() {
            break;
        }
        let v = unsafe { *atts.add(i + 1) };
        let (k, v) = unsafe { (CStr::from_ptr(k), CStr::from_ptr(v)) };
        write!(ev, " {}={}", k.to_str().unwrap(), v.to_str().unwrap()).unwrap();
        i += 2;
    }
    seen.events.push(ev);
}

unsafe extern "C" fn end(ud: *mut c_void, name: *const c_char) {
    let seen = unsafe { &mut *ud.cast::<Seen>() };
    seen.events.push(format!("</{}", unsafe { CStr::from_ptr(name) }.to_str().unwrap()));
}

unsafe extern "C" fn text(ud: *mut c_void, s: *const c_char, len: c_int) {
    let seen = unsafe { &mut *ud.cast::<Seen>() };
    let b = unsafe { std::slice::from_raw_parts(s.cast::<u8>(), usize::try_from(len).unwrap()) };
    seen.events.push(format!("text {:?}", String::from_utf8_lossy(b)));
}

/// Parses `doc` in one call; returns the events and the error code.
fn parse(doc: &[u8]) -> (Vec<String>, c_int) {
    let mut seen = Seen::default();
    unsafe {
        let p = x::XML_ParserCreate(ptr::null());
        assert!(!p.is_null());
        x::XML_SetUserData(p, ptr::from_mut(&mut seen).cast());
        x::XML_SetElementHandler(p, Some(start), Some(end));
        x::XML_SetCharacterDataHandler(p, Some(text));
        let st = x::XML_Parse(p, doc.as_ptr().cast(), c_int::try_from(doc.len()).unwrap(), 1);
        let code = if st == x::STATUS_OK { 0 } else { x::XML_GetErrorCode(p) };
        x::XML_ParserFree(p);
        (seen.events, code)
    }
}

#[test]
fn the_compiled_version_is_ours() {
    let v = unsafe { CStr::from_ptr(x::XML_ExpatVersion()) };
    assert_eq!(v.to_str().unwrap(), format!("expat_{}", x::VERSION));
}

#[test]
fn elements_attributes_and_text_reach_the_callbacks() {
    let doc = br#"<?xml version="1.0" encoding="UTF-8"?>
<Playlist majorVersion="1"><Title id="t1"><Chapter titleTimeBegin="00:00:00:00">A &amp; B</Chapter></Title></Playlist>"#;
    // Character data arrives in pieces (here split at the entity): a reader
    // must join them.
    let (ev, code) = parse(doc);
    assert_eq!(code, 0);
    assert_eq!(
        ev,
        [
            "<Playlist majorVersion=1",
            "<Title id=t1",
            "<Chapter titleTimeBegin=00:00:00:00",
            r#"text "A ""#,
            r#"text "&""#,
            r#"text " B""#,
            "</Chapter",
            "</Title",
            "</Playlist",
        ]
    );
}

#[test]
fn a_malformed_document_is_an_error() {
    let (_, code) = parse(b"<Playlist><Title></Playlist>");
    assert_ne!(code, 0);
    let msg = unsafe { CStr::from_ptr(x::XML_ErrorString(code)) };
    assert_eq!(msg.to_str().unwrap(), "mismatched tag");
    // A NUL inside the text is not XML either.
    let doc = CString::new("<a>").unwrap();
    let mut bytes = doc.into_bytes();
    bytes.extend_from_slice(b"\0</a>");
    assert_ne!(parse(&bytes).1, 0);
}
