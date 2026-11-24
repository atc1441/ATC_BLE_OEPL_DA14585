#!/usr/bin/env python3
"""Fetch the ARM toolchain this firmware is built with.

Run this once after cloning, then build:

    python tools/setup.py
    python tools/build.py

The toolchain is the xPack build of GCC 10.3.1 for arm-none-eabi, downloaded
from its GitHub release and checked against a pinned SHA-256. It lands in
tools/gcc-arm-none-eabi/, which is where tools/build.py looks first. Nothing
is installed system-wide and nothing is written outside this repository.

The DA14585 SDK is already in sdk/, so nothing else is needed. To build
against a different copy of it, pass --sdk with a directory or an archive and
this writes sdk_root.txt, the override the build reads before the bundled one.
"""

import argparse
import hashlib
import platform
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = ROOT / "tools" / "gcc-arm-none-eabi"
POINTER = ROOT / "sdk_root.txt"

BASE = ("https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack"
        "/releases/download/v10.3.1-2.3")
VERSION = "10.3.1-2.3"

# sha256 as published next to each asset, as <asset>.sha
RELEASES = {
    ("Windows", "AMD64"): (
        "win32-x64.zip",
        "169744f784fb04ae10c60bc6a2cd69cff93cff0bf5657e9333776036f347f9c4"),
    ("Linux", "x86_64"): (
        "linux-x64.tar.gz",
        "559dcf1c2dfddac513110fe23da0ef254032c09967eaa901f075515d51818719"),
    ("Linux", "aarch64"): (
        "linux-arm64.tar.gz",
        "6e3ee605079ba084836cd057022e16c28605f4068042cd14397976b836ea1a21"),
    ("Darwin", "x86_64"): (
        "darwin-x64.tar.gz",
        "eb9218eaafb74a05fab9cbbeb49fd4fe7b85c6ba411aefc66f681a5a862e8d8e"),
    ("Darwin", "arm64"): (
        "darwin-arm64.tar.gz",
        "a207301fb6c136f661a09024eb49424adf74cbc952a4cff150dfc7927206ab1a"),
}

SDK_MARKERS = [
    Path("projects/target_apps/template/empty_peripheral_template"
         "/Keil_5/empty_peripheral_template.uvprojx"),
    Path("sdk/platform/system_library/output/IAR/da14585_586.a"),
    Path("sdk/common_project_files/ldscripts/ldscript_DA14585_586.lds.S"),
]


def host_key():
    system = platform.system()
    machine = platform.machine()
    if system == "Darwin" and machine == "x86_64" and "ARM64" in platform.version():
        machine = "arm64"          # Rosetta reports the emulated machine
    if system == "Windows" and machine in ("x86_64", "AMD64"):
        machine = "AMD64"
    if system == "Linux" and machine in ("arm64",):
        machine = "aarch64"
    return system, machine


def compiler_present():
    for name in ("arm-none-eabi-gcc", "arm-none-eabi-gcc.exe"):
        if (TOOLCHAIN / "bin" / name).is_file():
            return True
    return False


def download(url, dest):
    print("downloading %s" % url)
    digest = hashlib.sha256()
    seen = 0
    shown = -1
    with urllib.request.urlopen(url) as src, open(dest, "wb") as out:
        total = int(src.headers.get("Content-Length") or 0)
        while True:
            chunk = src.read(1 << 16)
            if not chunk:
                break
            out.write(chunk)
            digest.update(chunk)
            seen += len(chunk)
            if total:
                step = seen * 20 // total
                if step != shown:
                    shown = step
                    print("\r  %3d%%  %.1f of %.1f MiB"
                          % (seen * 100 // total, seen / 1048576.0, total / 1048576.0),
                          end="", flush=True)
    print()
    return digest.hexdigest()


def unpack(archive, into):
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(into)
    else:
        with tarfile.open(archive) as tf:
            try:
                tf.extractall(into, filter="data")
            except TypeError:       # filter= arrived in Python 3.12
                tf.extractall(into)


def get_toolchain():
    if compiler_present():
        print("toolchain already in %s" % TOOLCHAIN)
        return
    key = host_key()
    if key not in RELEASES:
        raise SystemExit(
            "no pinned toolchain for %s/%s.\n"
            "Install arm-none-eabi-gcc 10.x yourself and put it on PATH; "
            "tools/build.py will find it there." % key)
    asset, want = RELEASES[key]
    name = "xpack-arm-none-eabi-gcc-%s-%s" % (VERSION, asset)
    with tempfile.TemporaryDirectory(prefix="atc-toolchain-") as work:
        work = Path(work)
        archive = work / name
        got = download("%s/%s" % (BASE, name), archive)
        if got != want:
            raise SystemExit("checksum mismatch\n  want %s\n  got  %s" % (want, got))
        print("sha256 ok, unpacking")
        unpack(archive, work)
        roots = [p for p in work.iterdir() if p.is_dir() and p.name.startswith("xpack-")]
        if len(roots) != 1:
            raise SystemExit("unexpected archive layout: %s" % [p.name for p in roots])
        TOOLCHAIN.parent.mkdir(parents=True, exist_ok=True)
        if TOOLCHAIN.exists():
            shutil.rmtree(TOOLCHAIN)
        shutil.move(str(roots[0]), str(TOOLCHAIN))
    if not compiler_present():
        raise SystemExit("unpacked, but no arm-none-eabi-gcc in %s/bin" % TOOLCHAIN)
    print("toolchain ready in %s" % TOOLCHAIN)


def sdk_complete(root):
    return [m for m in SDK_MARKERS if not (root / m).is_file()]


def find_sdk_root(tree):
    """The SDK root is the directory the markers sit under, zip or not."""
    if not sdk_complete(tree):
        return tree
    marker = SDK_MARKERS[0]
    for found in tree.rglob(marker.name):
        candidate = found
        for _ in marker.parts:
            candidate = candidate.parent
        if not sdk_complete(candidate):
            return candidate
    return None


def record_sdk(given):
    src = Path(given).expanduser()
    if not src.exists():
        raise SystemExit("no such SDK path: %s" % src)
    if src.is_file():
        low = src.name.lower()
        if not (low.endswith(".zip") or low.endswith(".tar.gz") or low.endswith(".tgz")):
            raise SystemExit("--sdk takes a directory, a .zip or a .tar.gz, got %s" % src.name)
        into = ROOT / "sdk"
        into.mkdir(exist_ok=True)
        print("unpacking %s into %s" % (src.name, into))
        unpack(src, into)
        tree = into
    else:
        tree = src.resolve()
    root = find_sdk_root(tree)
    if root is None:
        missing = sdk_complete(tree)
        raise SystemExit(
            "that does not look like a DA145xx SDK 6.0.22.1401 tree.\n"
            "Missing, relative to %s:\n  %s"
            % (tree, "\n  ".join(str(m) for m in missing)))
    try:
        text = root.relative_to(ROOT).as_posix()
    except ValueError:
        text = root.as_posix()
    POINTER.write_text(
        "# Written by tools/setup.py. The SDK itself is not part of this repo.\n"
        "%s\n" % text, encoding="utf-8")
    print("sdk_root.txt -> %s" % text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sdk", metavar="PATH",
                    help="build against this SDK instead of the one in sdk/")
    ap.add_argument("--toolchain-only", action="store_true")
    ap.add_argument("--sdk-only", action="store_true")
    args = ap.parse_args()

    if not args.sdk_only:
        get_toolchain()
    if args.sdk and not args.toolchain_only:
        record_sdk(args.sdk)
    elif not args.sdk and not args.toolchain_only:
        if POINTER.is_file():
            print("sdk_root.txt already there, leaving it alone")
        else:
            print("using the SDK in sdk/")
    print("done. next: python tools/build.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
