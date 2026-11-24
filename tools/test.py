#!/usr/bin/env python3
"""Build and run the host tests.

Three modules in this firmware are written so they also build for the host:
the protocol, the settings record and the panel catalog. None of them includes
an SDK header, so they can be compiled by whatever C compiler is on PATH and
exercised in a second. test/test_unified.c is what exercises them.

This needs a host compiler (gcc, clang or cc), not the bundled ARM one. On
Windows, MinGW or the compiler that ships with Git for Windows both work.

    python tools/test.py
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [
    ROOT / "src" / "oepl" / "oepl_session.c",
    ROOT / "src" / "oepl" / "oepl_inflate.c",
    ROOT / "src" / "app" / "settings.c",
    ROOT / "src" / "app" / "panel.c",
    ROOT / "test" / "test_unified.c",
]
INCLUDES = [ROOT / "src" / "oepl", ROOT / "src" / "app"]


def find_compiler():
    """A host compiler, not the ARM one in tools/."""
    override = os.environ.get("CC")
    if override:
        found = shutil.which(override)
        if found:
            return found
        sys.exit(f"CC is set to {override!r} but that is not on PATH")
    for name in ("gcc", "clang", "cc"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit(
        "No host C compiler found. Install one, or set CC.\n"
        "This does not use tools/gcc-arm-none-eabi: these tests run on this\n"
        "machine, not on the tag."
    )


def main():
    compiler = find_compiler()
    print("compiler", compiler)

    missing = [p for p in SOURCES if not p.is_file()]
    if missing:
        sys.exit("missing sources:\n  " + "\n  ".join(str(p) for p in missing))

    with tempfile.TemporaryDirectory(prefix="atc-oepl-test-") as work:
        binary = Path(work) / ("tests.exe" if os.name == "nt" else "tests")
        cmd = [
            compiler,
            "-std=c99",
            "-O1",
            "-Wall",
            "-Wextra",
            # The tests are the one place a mistake in the PIN or the stored
            # address is cheap to find, so warnings are errors here.
            "-Werror",
            "-o",
            str(binary),
        ]
        for inc in INCLUDES:
            cmd += ["-I", str(inc)]
        cmd += [str(p) for p in SOURCES]

        print("building")
        build = subprocess.run(cmd, cwd=ROOT)
        if build.returncode != 0:
            sys.exit("build failed")

        print("running")
        run = subprocess.run([str(binary)], cwd=ROOT)
        if run.returncode != 0:
            sys.exit(f"tests failed (exit {run.returncode})")


if __name__ == "__main__":
    main()
