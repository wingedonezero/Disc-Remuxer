"""A title's output name (src/disc-remuxer/names.c): the file-name template
(setting output.file_name_template) with the title's name, comment, date and
number, and the cleaning rules that make a name a file name."""

from helpers import ffi, lib

DEFAULT = "{NAME1}{-:CMNT1}{-:DT}{title:+DFLT}{_t:N2}"


def name(tmpl, title=None, index=0, comment=None, date=None):
    out = ffi.new("char[512]")
    enc = lambda s: s.encode() if s is not None else ffi.NULL
    lib.title_name(tmpl.encode(), enc(title), enc(comment), enc(date), index, out, 512)
    return ffi.string(out).decode()


def clean(s, rule):
    out = ffi.new("char[512]")
    lib.clean_name(s.encode(), rule, out, 512)
    return ffi.string(out).decode()


def test_default_template_is_the_name_and_the_title_number():
    assert name("", "HD_Feature", 2) == "HD_Feature_t02"
    assert name(DEFAULT, "HD_Feature", 2) == "HD_Feature_t02"
    assert name("", "Blade Runner Branching - Director's Cut", 0) == "Blade Runner Branching - Director's Cut_t00"
    assert name("", "feature", 0) == "feature_t00"
    assert name("", "x", 123) == "x_t123"


def test_without_a_name_the_title_word_stands_in():
    assert name("", None, 4) == "title_t04"
    assert name("", "", 0) == "title_t00"


def test_cleaning_rules():
    assert clean('A: B|C "D" e/f*g;h?i', 0) == "A- BIC 'D' e_f_g_h_i"
    assert clean("#1 $2 a·b ’c’", 1) == "_1 _2 a-b 'c'"
    assert clean("a.b c", 2) == "a_b_c"
    assert clean("a//b??", 0) == "a_b"
    assert clean("_a_", 0) == "_a"
    assert clean("THE LORD OF THE RINGS: THE FELLOWSHIP OF THE RING (EXT.) PT. 1", 1) == \
        "THE LORD OF THE RINGS- THE FELLOWSHIP OF THE RING (EXT.) PT. 1"


def test_comment_date_and_number_forms():
    t = dict(title="Disc", comment="Extra", date="2026-10-06 12:34:56", index=3)
    assert name("", **t) == "Disc-Extra-20261006123456"
    assert name("{NAME}{_t:AN2}{_m:AM3}", **t) == "Disc_t03_m004"
    assert name("{NAME}{_t:N2}", **t) == "Disc"                         # with a date N stands for nothing
    assert name("{NAME}{_:T2}", "Disc", 0) == "Disc"                    # T: nothing for title 0
    assert name("{x:+NAME}{-NAME}{y:-CMNT}", "Disc", 0) == "title"      # no prefix at the start
    assert name("{NAME}{x:+NAME}{-NAME}{y:-CMNT}", "Disc", 0) == "Discxy"


def test_invalid_templates():
    assert name("{NAME", "Disc", 7) == "!ERRtemplate_t07"
    assert name("{NAME}-{NOPE}", "Disc", 7) == "Disc-!ERRtemplate_t07"
    assert name("x{a{NAME}}", "Disc", 7) == "x!ERRtemplate_t07"
