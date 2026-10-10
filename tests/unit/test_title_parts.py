"""A title written as parts (--split, setting output.split_joined_titles,
the GUI's row menu; src/disc-remuxer/engine.c, writer.c): each part is an
output of its own, like a title: as soon as a run writes more than one
output, each gets a folder of its own name (<title>_p1, <title>_p2, ...),
also when the split title is the only one selected; one output stays in the
output folder itself. A built Blu-ray (tests/synth/bluray.py) whose title
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


def test_a_title_written_whole_alone_stays_in_the_output_folder(tmp_path):
    got = layout(demux(tmp_path))
    assert sorted(got) == ["."], got
    assert any(n.startswith("title_t00_00_") for n in got["."]), got
