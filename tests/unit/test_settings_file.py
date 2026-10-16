"""The settings file (src/base/settings.c), as the program uses it: made
with every setting at its default when missing; brought in step with the
program (settings added at their default, unknown ones removed, the user's
values kept, the old file kept as .bak); a wrong value replaced by its
default and said; a file that is no TOML refused; --set changes a setting
for one run only and refuses unknown settings, bad values and bad forms."""

import re
import subprocess

from helpers import DIST

PROG = str(DIST / "disc-remuxer")


def run(cfg, *args):
    p = subprocess.run([PROG, "--config", str(cfg), *args, "settings"], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def shown(out, key):
    """The value `settings` lists for key, with where it came from."""
    m = re.search(rf"^  {re.escape(key)} +(.*)$", out, re.M)
    return m.group(1).strip() if m else None


def test_a_missing_file_is_made_with_every_setting_at_its_default(tmp_path):
    cfg = tmp_path / "s.toml"
    code, out = run(cfg)
    assert code == 0 and "Settings file created with every setting at its default" in out, out
    text = cfg.read_text()
    for key in re.findall(r"^  ([a-z_]+\.[a-z_0-9]+) ", out, re.M):
        group, name = key.split(".")
        assert f"[{group}]" in text and f"\n{name} = " in text, key
    assert "(settings file)" not in out and "(command line)" not in out     # every value its default
    code, out = run(cfg)                                                    # loading again changes nothing
    assert code == 0 and "created" not in out and "added" not in out and "removed" not in out, out
    assert cfg.read_text() == text and not (tmp_path / "s.toml.bak").exists()


def test_the_file_is_brought_in_step_and_the_user_s_values_kept(tmp_path):
    cfg = tmp_path / "s.toml"
    run(cfg)
    old = cfg.read_text().replace("attempts = 5", "attempts = 9").replace('\nudf_reader = "netbsd"\n', "\n")
    old = old.replace("[aacs]", "old_option = true\n\n[aacs]") + "\n[gone]\nthing = 1\n"
    cfg.write_text(old)
    code, out = run(cfg)
    assert code == 0, out
    assert "read.udf_reader added (default netbsd)" in out, out
    assert "read.old_option is no setting of this program: removed" in out, out
    assert "gone.thing is no setting of this program: removed" in out, out
    assert shown(out, "read.attempts") == "9   (settings file)", out
    assert (tmp_path / "s.toml.bak").read_text() == old
    new = cfg.read_text()
    assert "attempts = 9" in new and 'udf_reader = "netbsd"' in new and "old_option" not in new and "gone" not in new
    code, out = run(cfg)                                                    # in step now
    assert "added" not in out and "removed" not in out and cfg.read_text() == new, out


def test_a_wrong_value_is_said_and_its_default_used(tmp_path):
    cfg = tmp_path / "s.toml"
    run(cfg)
    cfg.write_text(cfg.read_text().replace("attempts = 5", "attempts = 100"))
    code, out = run(cfg)
    assert code == 0 and "read.attempts has a value of the wrong kind (line 10)" in out, out
    assert shown(out, "read.attempts") == "5", out
    assert "attempts = 5" in cfg.read_text()


def test_a_file_that_is_no_toml_stops_the_program_and_is_left_alone(tmp_path):
    cfg = tmp_path / "s.toml"
    cfg.write_text("[read\nx")
    code, out = run(cfg)
    assert code == 1 and "is not valid TOML" in out, out
    assert cfg.read_text() == "[read\nx" and sorted(p.name for p in tmp_path.iterdir()) == ["s.toml"]


def test_set_changes_a_setting_for_one_run_only(tmp_path):
    cfg = tmp_path / "s.toml"
    run(cfg)
    text = cfg.read_text()
    code, out = run(cfg, "--set", "read.attempts=2", "--set", "log.debug_file=true")
    assert code == 0 and shown(out, "read.attempts") == "2   (command line)", out
    assert shown(out, "log.debug_file") == "true   (command line)", out
    assert cfg.read_text() == text
    for bad, said in (("output.nope=1", "there is no setting output.nope"),
                      ("read.attempts=100", "the value must be a number from 1 to 99"),
                      ("read.attempts=0", "the value must be a number from 1 to 99"),
                      ("noequals", "--set wants group.key=value, not 'noequals'")):
        code, out = run(cfg, "--set", bad)
        assert code == 2 and said in out, (bad, out)
    assert cfg.read_text() == text
