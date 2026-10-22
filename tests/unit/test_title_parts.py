"""Stream files go into a folder per title (src/disc-remuxer/engine.c,
writer.c), also when one title is ripped; a title written as parts (--split,
setting output.split_joined_titles, the GUI's row menu) is a title per part:
a folder each (<title>_p1, <title>_p2, ...). A built Blu-ray (tests/synth/bluray.py) whose title
plays two whole stream files back to back (two parts), each with MPEG-2
video and AC-3."""

import subprocess

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
S, E = 27000000, 27000000 + 90000 * 4
STREAMS = ((0x1011, 0x02, b"\x31"), (0x1100, 0x81, b"\x31eng"))


def clip_data():
    packets = []
    for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
        packets += bd.ts_packets(0x1011, bd.pes(0xe0, c, pts=900 + 3003 * i))
    for i, c in enumerate(AC3[k:k + 768] for k in range(0, len(AC3), 768)):
        packets += bd.ts_packets(0x1100, bd.pes(0xfd, c, ext=0x71, pts=900 + 2880 * i))
    return bd.source_units(packets)


def disc(tmp_path):
    data = clip_data()
    clips = {n: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=STREAMS) for n in (1, 2)}
    items = [Item(f"0000{n}", S, E, videos=[Stream(0x1011, 0x02, b"\x31")],
                  audio=[Stream(0x1100, 0x81, b"\x31eng")]) for n in (1, 2)]
    files = bd.bdmv(clips, {1: bd.mpls(items)})
    files["BDMV/STREAM/00001.m2ts"] = data
    files["BDMV/STREAM/00002.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


def demux(tmp_path, *opts):
    out = tmp_path / "out"
    p = subprocess.run([*PROGRAM, *opts, "demux", str(disc(tmp_path)), "0", str(out)], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    return out


def layout(out):
    """Folder -> the stream files in it (no logs)."""
    got = {}
    for f in out.rglob("*"):
        if f.is_file() and not f.name.endswith(".log"):
            got.setdefault(str(f.parent.relative_to(out)), []).append(f.name)
    return {k: sorted(v) for k, v in got.items()}


def test_each_part_gets_a_folder_of_its_own_like_a_title(tmp_path):
    got = layout(demux(tmp_path, "--split", "0"))
    assert sorted(got) == ["title_t00_p1", "title_t00_p2"], got
    for p in ("p1", "p2"):
        names = got[f"title_t00_{p}"]
        assert any(n.startswith(f"title_t00_{p}_00_") and n.endswith(".m2v") for n in names), names
        assert any(n.startswith(f"title_t00_{p}_01_[eng]_DD") for n in names), names


def test_a_title_ripped_alone_gets_its_folder_too(tmp_path):
    got = layout(demux(tmp_path))
    assert sorted(got) == ["title_t00"], got
    assert any(n.startswith("title_t00_00_") for n in got["title_t00"]), got


def test_a_clean_rip_logs_its_checks_as_detail_and_ends_with_one_line(tmp_path):
    # the standard log: passing title checks are detail lines (-v, the debug
    # log); the title's last line says the checks passed, its chapters and size
    import json
    import re

    def one_clip(path):
        # one clip whose first picture is at the playlist's start time
        packets = []
        for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
            packets += bd.ts_packets(0x1011, bd.pes(0xe0, c, pts=2 * S + 3003 * i))
        for i, c in enumerate(AC3[k:k + 768] for k in range(0, len(AC3), 768)):
            packets += bd.ts_packets(0x1100, bd.pes(0xfd, c, ext=0x71, pts=2 * S + 2880 * i))
        data = bd.source_units(packets)
        clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=STREAMS)}
        item = Item("00001", S, E, videos=[Stream(0x1011, 0x02, b"\x31")], audio=[Stream(0x1100, 0x81, b"\x31eng")])
        files = bd.bdmv(clips, {1: bd.mpls([item])})
        files["BDMV/STREAM/00001.m2ts"] = data
        return bd.write(path / "disc", files)
    out = tmp_path / "out"
    p = subprocess.run([*PROGRAM, "--json", "demux", str(one_clip(tmp_path)), "0", str(out)], capture_output=True,
                       text=True)
    msgs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith('{"type":"message"')]
    texts = [m["text"] for m in msgs]
    assert p.returncode == 0, p.stdout + p.stderr
    assert not [t for t in texts if t.startswith("Title check:")], texts
    done = [t for t in texts if t.startswith("Title #0: done")]
    # the size in bytes, KB, MB or GB (below 1 MB not "0.0 MB")
    assert done and "all checks passed (" in done[0] and "chapter" in done[0] and \
        re.search(r", [0-9.]+ (bytes?|KB|MB|GB)\)$", done[0]), done
    (tmp_path / "v").mkdir()
    v = subprocess.run([*PROGRAM, "-v", "--json", "demux", str(one_clip(tmp_path / "v")), "0", str(tmp_path / "out_v")],
                       capture_output=True, text=True)
    detail = [json.loads(l) for l in v.stdout.splitlines() if l.startswith('{"type":"message"')]
    assert any(m["text"].startswith("Title check:") and m["level"] == "detail" for m in detail)
