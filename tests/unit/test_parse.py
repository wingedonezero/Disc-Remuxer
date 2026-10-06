"""The compiled expat: its version, and a parse with element and character
data callbacks (the calls the HD DVD playlist reader makes)."""

from helpers import ffi, lib


def parse(doc):
    """Parses doc in one call: (the events, the error code)."""
    seen = []
    h = ffi.new_handle(seen)
    p = lib.XML_ParserCreate(ffi.NULL)
    assert p != ffi.NULL
    lib.XML_SetUserData(p, h)
    lib.XML_SetElementHandler(p, lib.tb_py_xml_start, lib.tb_py_xml_end)
    lib.XML_SetCharacterDataHandler(p, lib.tb_py_xml_text)
    st = lib.XML_Parse(p, doc, len(doc), 1)
    code = 0 if st == lib.XML_STATUS_OK else lib.XML_GetErrorCode(p)
    lib.XML_ParserFree(p)
    return seen, code


def test_the_compiled_version_is_ours():
    v = ffi.string(lib.XML_ExpatVersion()).decode()
    assert v == f"expat_{lib.XML_MAJOR_VERSION}.{lib.XML_MINOR_VERSION}.{lib.XML_MICRO_VERSION}"
    assert v == "expat_2.9.0"


def test_elements_attributes_and_text_reach_the_callbacks():
    doc = (b'<?xml version="1.0" encoding="UTF-8"?>\n<Playlist majorVersion="1"><Title id="t1"><Chapter '
           b'titleTimeBegin="00:00:00:00">A &amp; B</Chapter></Title></Playlist>')
    # Character data arrives in pieces (here split at the entity): a reader
    # must join them.
    ev, code = parse(doc)
    assert code == 0
    assert ev == [
        "<Playlist majorVersion=1",
        "<Title id=t1",
        "<Chapter titleTimeBegin=00:00:00:00",
        "text 'A '",
        "text '&'",
        "text ' B'",
        "</Chapter",
        "</Title",
        "</Playlist",
    ]


def test_a_malformed_document_is_an_error():
    _, code = parse(b"<Playlist><Title></Playlist>")
    assert code != 0
    assert ffi.string(lib.XML_ErrorString(code)).decode() == "mismatched tag"
    # A NUL inside the text is not XML either.
    assert parse(b"<a>\0</a>")[1] != 0
