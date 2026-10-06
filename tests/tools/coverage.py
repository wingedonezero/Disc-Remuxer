#!/usr/bin/env python3
"""Which lines of our disc code a test run reaches (gcov, build/coverage).

    make coverage
    tests/tools/coverage.py run <out.json> [pytest arguments]
    tests/tools/coverage.py only <a.json> <b.json>

run: empties the counters, runs pytest on the coverage build (DR_MODE=coverage)
with the arguments given, and writes {source file: [lines reached]} for our
files (libavformat discio_* / discrip_* / dvdvideo_* / hddvd_* / disclang,
libdvdread, libdvdnav, libdvdcss). only: the lines b reaches and a does not,
by file and function (e.g. a = the built-data tests, b = the real-disc tests).
"""

import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "coverage"
OURS = re.compile(r"libs/(ffmpeg/libavformat/(discio|discrip|dvdvideo|hddvd|disclang)[^/]*\.c"
                  r"|libdvdread/src/.*\.c|libdvdnav/src/.*\.c|libdvdcss/src/.*\.c)$")


def run(out, args):
    for f in BUILD.rglob("*.gcda"):
        f.unlink()
    env = {"DR_MODE": "coverage"}
    import os
    env = {**os.environ, **env}
    r = subprocess.run([str(ROOT / "build" / "venv" / "bin" / "python"), "-m", "pytest", "tests", *args],
                       cwd=ROOT, env=env)
    reached = {}
    gcdas = [str(f) for f in BUILD.rglob("*.gcda")]
    for i in range(0, len(gcdas), 200):
        p = subprocess.run(["gcov", "--json-format", "--stdout", *gcdas[i:i + 200]], cwd=BUILD,
                           capture_output=True, text=True, check=True)
        for line in p.stdout.splitlines():
            if not line.strip():
                continue
            doc = json.loads(line)
            cwd = pathlib.Path(doc.get("current_working_directory", BUILD))
            for f in doc["files"]:
                name = str((cwd / f["file"]).resolve())
                rel = name[len(str(ROOT)) + 1:] if name.startswith(str(ROOT)) else name
                if not OURS.search(rel):
                    continue
                hit = reached.setdefault(rel, {})
                for ln in f["lines"]:
                    if ln["count"] > 0:
                        hit[ln["line_number"]] = ln.get("function_name", "")
    pathlib.Path(out).write_text(json.dumps({k: sorted(v.items()) for k, v in sorted(reached.items())}))
    print(f"{sum(len(v) for v in reached.values())} lines reached in {len(reached)} files; pytest exit {r.returncode}")
    return r.returncode


def only(a, b):
    a = {k: {ln for ln, _ in v} for k, v in json.loads(pathlib.Path(a).read_text()).items()}
    b = json.loads(pathlib.Path(b).read_text())
    total = 0
    for f, lines in b.items():
        extra = [(ln, fn) for ln, fn in lines if ln not in a.get(f, set())]
        if not extra:
            continue
        total += len(extra)
        by_fn = {}
        for ln, fn in extra:
            by_fn.setdefault(fn, []).append(ln)
        print(f"{f}: {len(extra)} lines")
        for fn, lns in by_fn.items():
            print(f"    {fn or '?'}: {compress(lns)}")
    print(f"{total} lines reached only by the second run")


def compress(lns):
    out, start, prev = [], None, None
    for n in sorted(lns):
        if start is None:
            start = prev = n
        elif n == prev + 1:
            prev = n
        else:
            out.append(f"{start}-{prev}" if prev != start else str(start))
            start = prev = n
    if start is not None:
        out.append(f"{start}-{prev}" if prev != start else str(start))
    return ",".join(out)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "run":
        sys.exit(run(sys.argv[2], sys.argv[3:]))
    if len(sys.argv) == 4 and sys.argv[1] == "only":
        only(sys.argv[2], sys.argv[3])
        sys.exit(0)
    sys.exit(__doc__)
