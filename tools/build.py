"""Link the DA14585 peripheral image against SDK 6.0.22.1401."""

import hashlib
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "src" / "app"
OEPL = ROOT / "src" / "oepl"


SDK_MARKER = Path("sdk/platform/system_library/output/IAR/da14585_586.a")


def is_sdk(path):
    return path.is_dir() and (path / SDK_MARKER).is_file()


def load_sdk():
    """The SDK in sdk/ unless sdk_root.txt names another one.

    The tree ships with the SDK, so a fresh clone builds with nothing else
    fetched. sdk_root.txt is the override, for anyone who keeps their own copy
    somewhere else; it is gitignored and wins when it is there.
    """
    pointer = ROOT / "sdk_root.txt"
    raw = ""
    if pointer.is_file():
        for line in pointer.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                raw = line
                break
    if raw:
        path = Path(raw)
        if not path.is_absolute():
            path = (ROOT / path).resolve()
        if not path.is_dir():
            raise SystemExit("sdk_root.txt points at %s, which is not there" % path)
        if not is_sdk(path):
            raise SystemExit("%s has no %s" % (path, SDK_MARKER.as_posix()))
        return path
    bundled = ROOT / "sdk"
    for candidate in [bundled] + sorted(p for p in bundled.glob("*") if p.is_dir()):
        if is_sdk(candidate):
            return candidate
    raise SystemExit(
        "no SDK found. It should be in %s; if yours lives elsewhere, put the "
        "path in sdk_root.txt (see sdk_root.txt.example)." % bundled)


def arm_bin(tool):
    """Local toolchain, then PATH, then the usual Windows install."""
    names = [tool]
    if tool.lower().endswith(".exe"):
        names.append(tool[:-4])
    else:
        names.append(tool + ".exe")
    local = ROOT / "tools" / "gcc-arm-none-eabi" / "bin"
    for name in names:
        path = local / name
        if path.is_file():
            return path
    for name in names:
        which = shutil.which(name)
        if which:
            return Path(which)
        path = Path(r"C:\gcc-arm-none-eabi\bin") / name
        if path.is_file():
            return path
    raise SystemExit("missing compiler tool %s" % tool)


SDK = load_sdk()
UVPROJX = (
    SDK
    / "projects"
    / "target_apps"
    / "template"
    / "empty_peripheral_template"
    / "Keil_5"
    / "empty_peripheral_template.uvprojx"
)
TEMPLATE_CFG = (
    SDK / "projects" / "target_apps" / "template" / "empty_peripheral_template" / "src" / "config"
)
GCC = arm_bin("arm-none-eabi-gcc")
OBJCOPY = arm_bin("arm-none-eabi-objcopy")
OBJDIR = ROOT / "build" / "obj"
OUTDIR = ROOT / "build"


def sdk_path(base, raw):
    """Resolve a path written the Keil way: backslashes, and whatever case the
    project file felt like. Windows does not care about either; a Linux build
    runner cares about both, so each component is matched case-insensitively
    when an exact match is not there.
    """
    parts = [p for p in raw.strip().replace("\\", "/").split("/") if p and p != "."]
    path = base
    for part in parts:
        if part == "..":
            path = path.parent
            continue
        step = path / part
        if not step.exists() and path.is_dir():
            low = part.lower()
            for entry in path.iterdir():
                if entry.name.lower() == low:
                    step = entry
                    break
        path = step
    return path


def include_paths():
    tree = ET.parse(str(UVPROJX))
    target = tree.getroot().find("Targets/Target")
    texts = []
    for node in target.findall(".//IncludePath"):
        if node.text:
            texts.append(node.text)
    text = max(texts, key=len)
    keil = UVPROJX.parent
    paths = []
    for raw in text.split(";"):
        raw = raw.strip()
        if not raw:
            continue
        path = sdk_path(keil, raw).resolve()
        if path.is_dir():
            paths.append(path)
    return paths


def sdk_sources():
    tree = ET.parse(str(UVPROJX))
    target = tree.getroot().find("Targets/Target")
    keil = UVPROJX.parent
    found = []
    for node in target.findall(".//File"):
        flag = node.find(".//IncludeInBuild")
        if flag is not None and (flag.text or "").strip() == "0":
            continue
        path_node = node.find("FilePath")
        if path_node is None or not path_node.text:
            continue
        path = sdk_path(keil, path_node.text).resolve()
        if path.suffix.lower() != ".c":
            continue
        if "empty_peripheral_template" in str(path):
            continue
        found.append(path)
    return found


def cflags(includes):
    flags = [
        "-mcpu=cortex-m0",
        "-mthumb",
        "-mfloat-abi=soft",
        "-Os",
        "-ffunction-sections",
        "-fdata-sections",
        "-fno-strict-aliasing",
        "-fshort-enums",
        "-funsigned-char",
        "-std=gnu99",
        "-Wall",
        "-Wno-unused-function",
        "-Wno-unused-variable",
        "-Wno-unused-parameter",
        "-Wno-address-of-packed-member",
        "-Wno-maybe-uninitialized",
        "-D__DA14585__",
        "-include",
        "da1458x_config_basic.h",
        "-include",
        "da1458x_config_advanced.h",
        "-include",
        "user_config.h",
    ]
    flags.extend(["-I", str(APP)])
    flags.extend(["-I", str(OEPL)])
    flags.extend(["-I", str(TEMPLATE_CFG)])
    for path in includes:
        flags.extend(["-I", str(path)])
    return flags


def compile_one(src, flags):
    # Two SDK sources share a stem, so the object name carries a digest of the
    # full path. hashlib, not hash(): Python salts the built-in one per run,
    # which would leave a fresh set of stale objects behind on every build.
    token = hashlib.md5(str(src).encode("utf-8")).hexdigest()[:8]
    obj = OBJDIR / (src.stem + "_" + token + ".o")
    cmd = [str(GCC), *flags, "-c", str(src), "-o", str(obj)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    return src, done.returncode, done.stdout, done.stderr, obj


def preprocess_link(includes):
    lds_in = SDK / "sdk" / "common_project_files" / "ldscripts" / "ldscript_DA14585_586.lds.S"
    lds_out = OUTDIR / "ldscript.ld"
    cmd = [
        str(GCC),
        "-E",
        "-P",
        "-x",
        "c",
        "-D__DA14585__",
        "-I",
        str(APP),
        "-I",
        str(TEMPLATE_CFG),
        "-I",
        str(SDK / "sdk" / "common_project_files"),
        "-I",
        str(SDK / "sdk" / "common_project_files" / "ldscripts"),
    ]
    for path in includes:
        cmd.extend(["-I", str(path)])
    cmd.extend([str(lds_in), "-o", str(lds_out)])
    done = subprocess.run(cmd, capture_output=True, text=True)
    return done, lds_out


def main():
    OBJDIR.mkdir(parents=True, exist_ok=True)
    includes = include_paths()
    sources = sdk_sources()
    sources.extend(
        [
            APP / "user_atc.c",
            APP / "user_custs_config.c",
            APP / "user_periph_setup.c",
            APP / "rtt.c",
            APP / "settings.c",
            APP / "epd.c",
            APP / "panel.c",
            APP / "gui.c",
            OEPL / "oepl_session.c",
            OEPL / "oepl_inflate.c",
        ]
    )
    asm = [
        SDK / "sdk" / "platform" / "arch" / "boot" / "GCC" / "startup_DA14585_586.S",
        SDK / "sdk" / "platform" / "arch" / "boot" / "GCC" / "ivtable_DA14585_586.S",
    ]
    print("SDK", SDK)
    print("compiler", GCC)
    print("C sources", len(sources))
    flags = cflags(includes)
    objects = []
    failed = False
    print("compiling C files")
    with ThreadPoolExecutor(max_workers=8) as pool:
        jobs = [pool.submit(compile_one, src, flags) for src in sources]
        for job in as_completed(jobs):
            src, code, out, err, obj = job.result()
            if code != 0:
                failed = True
                sys.stderr.write("FAIL %s\n%s%s\n" % (src, out, err))
            else:
                objects.append(obj)
                if err.strip():
                    sys.stderr.write("WARN %s\n%s\n" % (src.name, err))
    if failed:
        print("compile failed")
        return 1
    # The compile pool finishes in whatever order it likes; the linker lays the
    # image out in the order it is handed. Sort, so two builds of the same tree
    # produce the same image.
    objects.sort()
    for src in asm:
        obj = OBJDIR / (src.stem + ".o")
        cmd = [str(GCC), "-mcpu=cortex-m0", "-mthumb", "-D__DA14585__",
               # 0xC00, not 0x1000. Measured on the tag with the stack painted over SWD
               # and read back: 1120 of 4096 bytes at the deepest, across a full
               # 116160-byte picture upload and the refresh that follows. 360 bytes
               # across boot and a status-screen refresh alone. 3072 keeps better
               # than 1.7x over the measured peak and hands 1024 bytes back to the
               # gap between the heap and the stack, which this image has been
               # running short of. The zlib picture path is not in that measurement,
               # which is why this is not cut further.
               "-D__STACK_SIZE=0xC00", "-D__HEAP_SIZE=0x100", "-c", str(src), "-o", str(obj)]
        done = subprocess.run(cmd, capture_output=True, text=True)
        if done.returncode != 0:
            sys.stderr.write(done.stderr)
            print("asm failed", src)
            return 1
        objects.append(obj)
    pre, lds = preprocess_link(includes)
    if pre.returncode != 0:
        sys.stderr.write(pre.stderr)
        print("linker script preprocess failed")
        return 1
    elf = OUTDIR / "atc_oepl.elf"
    link = [
        str(GCC),
        "-mcpu=cortex-m0",
        "-mthumb",
        "-nostartfiles",
        "-specs=nano.specs",
        "-specs=nosys.specs",
        "-Wl,--gc-sections",
        "-Wl,-Map,%s" % (OUTDIR / "atc_oepl.map"),
        "-T",
        str(lds),
        "-L",
        str(SDK / "sdk" / "common_project_files" / "misc"),
        "-o",
        str(elf),
        *[str(obj) for obj in objects],
        str(SDK / "sdk" / "platform" / "system_library" / "output" / "IAR" / "da14585_586.a"),
        "-lc",
        "-lgcc",
        "-lnosys",
    ]
    print("Cortex-M0 link")
    done = subprocess.run(link, capture_output=True, text=True)
    sys.stdout.write(done.stdout)
    sys.stderr.write(done.stderr)
    if done.returncode != 0:
        print("link failed")
        return 1
    binary = OUTDIR / "atc_oepl.payload"
    copy = subprocess.run(
        [str(OBJCOPY), "-O", "binary", str(elf), str(binary)],
        capture_output=True,
        text=True,
    )
    if copy.returncode != 0:
        sys.stderr.write(copy.stderr)
        return 1
    size = binary.stat().st_size
    print("Cortex-M0 link succeeded")
    print("payload %u bytes %s" % (size, binary))
    sys.path.insert(0, str(ROOT / "tools"))
    import flash
    flash.build_image()
    return 0


if __name__ == "__main__":
    sys.exit(main())
