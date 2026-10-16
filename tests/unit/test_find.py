"""Finding discs and where their output goes (src/disc-remuxer/discs.c): the
one rule the GUI's queue and the command line share — find <folder> lists
the discs as the GUI's queue takes them, demux of a folder of discs rips
each into its own folder, and a setting that moved keeps its value."""

import subprocess

from helpers import PROGRAM
from test_stream_names import disc


def run(*args, settings=None):
    cmd = [*PROGRAM]
    if settings:
        cmd[cmd.index("--config") + 1] = str(settings)
    p = subprocess.run([*cmd, *args], capture_output=True, text=True)
    return p.returncode, p.stdout


def shelf(tmp_path):
    """A folder of discs: a Blu-ray folder two folders down, an image (its
    name: without the extension), a disc's BDMV folder itself, a hidden
    folder (left out) and a file that is no disc."""
    root = tmp_path / "Shelf"
    disc(root / "Films" / "Alpha")                      # Shelf/Films/Alpha/disc/BDMV
    (root / "Beta.img").write_bytes(b"\0" * 2048)
    disc(root / "Gamma")                                # Shelf/Gamma/disc/BDMV
    disc(root / ".hidden")
    (root / "notes.txt").write_text("not a disc")
    return root


def test_find_lists_the_discs_in_name_order_with_their_output_folders(tmp_path):
    root = shelf(tmp_path)
    code, out = run("--set", f"output.folder={tmp_path / 'out'}", "find", str(root))
    assert code == 0
    lines = out.splitlines()
    discs = [l for l in lines if l.startswith("Disc ")]
    assert discs == ["Disc 1: Beta", "Disc 2: disc", "Disc 3: disc"]
    assert f"  output: {tmp_path / 'out'}/Shelf/Beta" in lines
    assert f"  output: {tmp_path / 'out'}/Shelf/Films/Alpha/disc" in lines
    assert f"  output: {tmp_path / 'out'}/Shelf/Gamma/disc" in lines
    assert "  below:  Films/Alpha/disc" in lines
    assert any(l.endswith("3 discs found in " + str(root)) for l in lines)


def test_without_the_folder_structure_each_disc_goes_straight_under_the_main_folder(tmp_path):
    root = shelf(tmp_path)
    code, out = run("--set", f"output.folder={tmp_path / 'out'}", "--set", "output.keep_folder_structure=false",
                    "find", str(root))
    assert code == 0
    assert f"  output: {tmp_path / 'out'}/Beta" in out.splitlines()


def test_a_folder_without_discs_says_so(tmp_path):
    (tmp_path / "empty").mkdir()
    code, out = run("find", str(tmp_path / "empty"))
    assert code == 1 and "No disc found in" in out


def test_demux_of_a_folder_of_discs_rips_each_into_its_own_folder(tmp_path):
    root = tmp_path / "Shelf"
    disc(root / "Films" / "Alpha")
    disc(root / "Gamma")
    out = tmp_path / "out"
    code, text = run("demux", str(root), "0", str(out))
    assert code == 0, text
    assert "=== Disc 1/2: disc ===" in text and "=== Disc 2/2: disc ===" in text
    assert "2 discs of 2 ripped without failures" in text
    for d in (out / "Shelf" / "Films" / "Alpha" / "disc", out / "Shelf" / "Gamma" / "disc"):
        assert (d / "disc.log").is_file(), sorted(out.rglob("*"))
        assert list(d.glob("*/*.m2v")), sorted(d.rglob("*"))         # each title in a folder of its own


def test_a_setting_that_moved_keeps_its_value(tmp_path):
    settings = tmp_path / "s.toml"
    settings.write_text("[gui]\nkeep_folder_structure = false\n")
    root = shelf(tmp_path)
    code, out = run("--set", f"output.folder={tmp_path / 'out'}", "find", str(root), settings=settings)
    assert code == 0
    assert "Settings file: gui.keep_folder_structure is now output.keep_folder_structure (its value kept)" in out
    assert f"  output: {tmp_path / 'out'}/Beta" in out.splitlines()
    assert "keep_folder_structure = false" in settings.read_text().split("[output]")[1].split("[")[0]
