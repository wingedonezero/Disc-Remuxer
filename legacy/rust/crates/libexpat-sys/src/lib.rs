//! expat (XML parser), compiled from `libs/expat` by `build.rs` and linked
//! statically.
//!
//! Declarations are added here as the workspace starts to call the library
//! directly.

use std::os::raw::{c_char, c_int, c_ulong, c_void};

/// Version of the compiled expat copy.
pub const VERSION: &str = "2.9.0";

/// `struct XML_ParserStruct` (only handled through pointers).
#[repr(C)]
pub struct XmlParserStruct {
    _private: [u8; 0],
}

/// `XML_Parser`.
pub type XmlParser = *mut XmlParserStruct;

/// `enum XML_Status`.
pub const STATUS_ERROR: c_int = 0;
pub const STATUS_OK: c_int = 1;

/// `XML_StartElementHandler`.
pub type StartElementHandler =
    unsafe extern "C" fn(user_data: *mut c_void, name: *const c_char, atts: *mut *const c_char);
/// `XML_EndElementHandler`.
pub type EndElementHandler = unsafe extern "C" fn(user_data: *mut c_void, name: *const c_char);
/// `XML_CharacterDataHandler`.
pub type CharacterDataHandler = unsafe extern "C" fn(user_data: *mut c_void, s: *const c_char, len: c_int);

extern "C" {
    pub fn XML_ParserCreate(encoding: *const c_char) -> XmlParser;
    pub fn XML_ParserFree(parser: XmlParser);
    pub fn XML_SetUserData(parser: XmlParser, user_data: *mut c_void);
    pub fn XML_SetElementHandler(
        parser: XmlParser,
        start: Option<StartElementHandler>,
        end: Option<EndElementHandler>,
    );
    pub fn XML_SetCharacterDataHandler(parser: XmlParser, handler: Option<CharacterDataHandler>);
    pub fn XML_Parse(parser: XmlParser, s: *const c_char, len: c_int, is_final: c_int) -> c_int;
    /// `enum XML_Error`.
    pub fn XML_GetErrorCode(parser: XmlParser) -> c_int;
    pub fn XML_ErrorString(code: c_int) -> *const c_char;
    pub fn XML_GetCurrentLineNumber(parser: XmlParser) -> c_ulong;
    pub fn XML_ExpatVersion() -> *const c_char;
}
