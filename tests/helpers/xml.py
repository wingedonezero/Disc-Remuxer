"""expat's callbacks, recording what a parse delivers."""

from helpers import extern, ffi


@extern
def tb_py_xml_start(ud, name, atts):
    ev = "<" + ffi.string(name).decode()
    i = 0
    while atts[i] != ffi.NULL:
        ev += f" {ffi.string(atts[i]).decode()}={ffi.string(atts[i + 1]).decode()}"
        i += 2
    ffi.from_handle(ud).append(ev)


@extern
def tb_py_xml_end(ud, name):
    ffi.from_handle(ud).append("</" + ffi.string(name).decode())


@extern
def tb_py_xml_text(ud, s, length):
    ffi.from_handle(ud).append("text " + repr(bytes(ffi.buffer(s, length)).decode("utf-8", "replace")))
