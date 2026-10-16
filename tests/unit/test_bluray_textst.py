"""Blu-ray text subtitles (HDMV TextST) from a clip of their own
(src/disc/title_reader.c's side clips, bluray_part.c side_offset,
src/mux/ts_disc.c dialog_pids, src/parse/pgs_rule.c, disctl_textst.c,
disctl_title.c, src/disc-remuxer/writer.c): no real disc of the corpus has
them, so a disc is built (tests/synth/bluray.py): an MPEG-2 video clip and a
text subtitle clip of PES packets without a PTS (private_stream_2), named by
a SubPath of type 4. The .textst file is MKVToolNix's layout: "TextST", the
dialog style segment, the number of dialogs, each dialog with its start /
end PTS set to its time on the title timeline."""

import json
import shutil
import struct
import subprocess

import pytest

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream, SubPath

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
S, E = 27000000, 27000000 + 45000 * 4
V0 = 2 * S                                  # the video's first PTS (90 kHz): the STC sequence's start
TEXT_PID = 0x1800

# a dialog style segment: an arbitrary body (its contents are not read);
# on the disc the number of dialogs follows it
STYLE_BODY = bytes(range(40))


def style(count):
    return bytes([0x81]) + struct.pack(">H", len(STYLE_BODY)) + STYLE_BODY + struct.pack(">H", count)


def pts5(v):
    return bytes([0xfe | (v >> 32) & 1]) + struct.pack(">I", v & 0xffffffff)


def dialog(start, end, regions=((0, b"Hello"),), palette=b""):
    """A dialog presentation segment: regions (flags, text) as inline text."""
    body = pts5(start) + pts5(end)
    body += bytes([0x80]) + struct.pack(">H", len(palette)) + palette if palette else bytes([0])
    body += bytes([len(regions)])
    for flags, text in regions:
        data = bytes([0x1b, 0x01, len(text)]) + text
        body += bytes([flags, 0]) + struct.pack(">H", len(data)) + data
    return bytes([0x82]) + struct.pack(">H", len(body)) + body


MP2 = (DATA / "sine_11520.mp2").read_bytes()       # 576-byte frames of 24 ms


def video_clip(v0=V0, audio=False):
    """The video (and with audio its MPEG audio on PID 0x1100: a clip that is
    no filler)."""
    packets = []
    for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
        packets += bd.ts_packets(0x1011, bd.pes(0xe0, c, pts=v0 + 3003 * i))
    if audio:
        for i, c in enumerate(MP2[k:k + 576] for k in range(0, len(MP2), 576)):
            packets += bd.ts_packets(0x1100, bd.pes(0xc0, c, pts=v0 + 2160 * i))
    return bd.source_units(packets)


def disc(tmp_path, segments, in_time=S, sync_pts=S, name="disc", sync_item=0, char_code=1):
    main = video_clip(audio=bool(sync_item))
    text = []
    for seg in segments:
        text += bd.ts_packets(TEXT_PID, bd.pes_private2(seg), cc=len(text))
    side = bd.source_units(text)
    clips = {1: bd.clpi(packets=len(main) // 192, stcs=((0, 0x1001, S, E),), streams=((0x1011, 0x02, b"\x31"),)),
             2: bd.clpi(packets=len(side) // 192, stcs=((0, TEXT_PID, S, E),),
                        streams=((TEXT_PID, 0x92, bytes([char_code]) + b"eng"),))}
    pg = Stream(TEXT_PID, 0x92, b"\x01eng", entry_type=2, entry=bytes([0, 0]) + bd.be16(TEXT_PID) + bytes(4))
    items = [Item("00001", S, E, videos=[Stream(0x1011, 0x02, b"\x31")], pg=[pg])]
    if sync_item:           # a second item (clip 00003, a segment of its own) the subtitles are synchronised with
        audio = Stream(0x1100, 0x03, b"\x31eng")
        clips[1] = bd.clpi(packets=len(main) // 192, stcs=((0, 0x1001, S, E),),
                           streams=((0x1011, 0x02, b"\x31"), (0x1100, 0x03, b"\x31eng")))
        clips[3] = clips[1]
        items[0].audio = [audio]
        items.append(Item("00003", S, E, connection=5, videos=[Stream(0x1011, 0x02, b"\x31")], audio=[audio],
                          pg=[pg]))
    sp = SubPath(type=4, clips=["00002"], in_time=in_time, out_time=E, sync_item=sync_item, sync_pts=sync_pts)
    # two chapters: the playlist is a title of its own
    files = bd.bdmv(clips, {1: bd.mpls(items, subpaths=[sp], marks=((1, 0, S), (1, 0, S + 90000)))})
    files["BDMV/STREAM/00001.m2ts"] = main
    files["BDMV/STREAM/00002.m2ts"] = side
    if sync_item:
        files["BDMV/STREAM/00003.m2ts"] = main
    return bd.write(tmp_path / name, files)


def rip(tmp_path, disc_dir, out="out"):
    o = tmp_path / out
    p = subprocess.run([*PROGRAM, "--json", "demux", str(disc_dir), "0", str(o)], capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    texts = [r.get("text", "") for r in recs if r.get("type") == "message"]
    result = [r for r in recs if r.get("type") == "title_result"]
    files = {f.name: f for f in o.rglob("*") if f.is_file()}
    return p, texts, result[0] if result else None, files


def textst_files(files, forced=None):
    out = [f for n, f in files.items() if n.endswith(".textst")]
    if forced is not None:
        out = [f for f in out if ("(forced)" in f.name) == forced]
    return out


def textst(style_body, dialogs):
    """The expected .textst file: dialogs as (segment, start, end) in 90 kHz."""
    out = b"TextST" + bytes([0x81]) + struct.pack(">H", len(style_body)) + style_body + struct.pack(">H", len(dialogs))
    for seg, start, end in dialogs:
        seg = bytearray(seg)
        seg[3:8] = bytes([(seg[3] & 0xfe) | (start >> 32) & 1]) + struct.pack(">I", start & 0xffffffff)
        seg[8:13] = bytes([(seg[8] & 0xfe) | (end >> 32) & 1]) + struct.pack(">I", end & 0xffffffff)
        out += bytes(seg)
    return out


def test_text_subtitles_are_ripped_on_the_title_timeline_and_said_untested(tmp_path):
    d1 = dialog(V0 + 45000, V0 + 90000)                 # 0.5 s .. 1.0 s on the title
    d2 = dialog(V0 + 135000, V0 + 162000, ((0, b"Second"), (0x40, b"forced")))
    p, texts, result, files = rip(tmp_path, disc(tmp_path, [style(2), d1, d2]))
    assert result and result["status"] != "failed", (result, p.stderr[-3000:])
    assert any("text subtitles (from a clip of their own) on a Blu-ray [untested on real discs]" in t
               for t in texts), texts
    full = textst_files(files, forced=False)
    assert len(full) == 1, sorted(files)
    assert full[0].read_bytes() == textst(STYLE_BODY, [(d1, 45000, 90000), (d2, 135000, 162000)])
    # the forced-only copy: the second dialog with its forced region alone
    forced = textst_files(files, forced=True)
    assert len(forced) == 1, sorted(files)
    d2f = dialog(V0 + 135000, V0 + 162000, ((0x40, b"forced"),))
    assert forced[0].read_bytes() == textst(STYLE_BODY, [(d2f, 135000, 162000)])


@pytest.mark.skipif(not shutil.which("mkvmerge") or not shutil.which("mkvextract"),
                    reason="MKVToolNix is not installed")
def test_the_textst_file_goes_through_mkvmerge_and_back_unchanged(tmp_path):
    d1 = dialog(V0 + 45000, V0 + 90000, palette=bytes(5))
    p, texts, result, files = rip(tmp_path, disc(tmp_path, [style(1), d1]))
    full = textst_files(files, forced=False)[0]
    mkv, back = tmp_path / "t.mkv", tmp_path / "back.textst"
    subprocess.run(["mkvmerge", "-q", "-o", str(mkv), str(full)], check=True)
    subprocess.run(["mkvextract", "-q", str(mkv), "tracks", f"0:{back}"], check=True)
    assert back.read_bytes() == full.read_bytes()


def test_a_clip_whose_in_time_lies_ahead_of_its_sync_start_is_moved_back(tmp_path):
    # IN time 1 s (45 kHz) after the sync start: the clip's clock is 1 s ahead
    d1 = dialog(V0 + 90000 + 45000, V0 + 90000 + 90000)
    p, texts, result, files = rip(tmp_path, disc(tmp_path, [style(1), d1], in_time=S + 45000))
    assert textst_files(files, forced=False)[0].read_bytes() == textst(STYLE_BODY, [(d1, 45000, 90000)])


def test_dialogs_that_cannot_be_used_are_left_out_and_said(tmp_path):
    early = dialog(V0 - 45000, V0 - 9000)              # before the title's start
    bad = bytearray(dialog(V0 + 45000, V0 + 90000))
    bad[14] = 3                                         # number_of_regions 3
    backwards = dialog(V0 + 90000, V0 + 45000)          # ends before it starts
    good = dialog(V0 + 135000, V0 + 162000)
    p, texts, result, files = rip(tmp_path, disc(tmp_path, [style(4), early, bytes(bad), backwards, good]))
    assert result and result["status"] != "failed", result
    assert result["warnings"] > 0
    assert textst_files(files, forced=False)[0].read_bytes() == textst(STYLE_BODY, [(good, 135000, 162000)])
    log = "\n".join(texts)
    assert "before the title's start: left out" in log, log
    assert "ends (PTS" in log and "before it starts" in log, log
    assert "refused" in log or "does not parse" in log or "left out" in log


def test_a_clip_without_a_style_segment_gives_no_subtitles(tmp_path):
    d1 = dialog(V0 + 45000, V0 + 90000)
    p, texts, result, files = rip(tmp_path, disc(tmp_path, [d1]))
    assert result and result["status"] != "failed", result
    assert not textst_files(files), sorted(files)
    assert any("does not start with a dialog style segment" in t for t in texts), texts


def test_subtitles_synchronised_with_a_later_segment_keep_the_first_segments_clock(tmp_path):
    # the reference program's way: its clock is taken as the first segment's (offset 0), said
    d1 = dialog(V0 + 45000, V0 + 90000)
    p, texts, result, files = rip(tmp_path, disc(tmp_path, [style(1), d1], sync_item=1))
    assert result and result["status"] != "failed", result
    assert any("on another segment than the first one read: its times are taken as the first segment's" in t and
               "[untested on real discs]" in t for t in texts), texts
    assert textst_files(files, forced=False)[0].read_bytes() == textst(STYLE_BODY, [(d1, 45000, 90000)])


def test_text_in_another_encoding_than_utf8_is_kept_and_said(tmp_path):
    d1 = dialog(V0 + 45000, V0 + 90000)
    p, texts, result, files = rip(tmp_path, disc(tmp_path, [style(1), d1], char_code=3))
    assert any("text subtitles whose text is in Shift-JIS (character code 3): kept as on the disc" in t
               for t in texts), texts
    assert textst_files(files, forced=False)[0].read_bytes() == textst(STYLE_BODY, [(d1, 45000, 90000)])
