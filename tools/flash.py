"""Program the application bank at 0x1F000.

The firmware file is the secondary-bootloader image: signature 70 51, valid
AA, image id 00 so it is preferred over an image id of FF. CRC is zlib over
the bytes that land at 0x07FC0000. An access point sends this same file.
The raw linker output stays in atc_oepl.payload.

Pass --serial with the J-Link serial number. --image-only rewrites
build/atc_oepl.bin from build/atc_oepl.payload and does not open a probe.
"""

import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build"
PAYLOAD = OUT / "atc_oepl.payload"
BIN = OUT / "atc_oepl.bin"
FLASHER = OUT / "bank.bin"
BANK = 0x0001F000
BANK_END = 0x00038000
MAIL = 0x07FD5000
ENTRY = 0x07FD6000
BUF = 0x07FC0000
MAGIC = 0xA7C0115E
ST_OK = 0xA7C00002
CMD_JEDEC = 1
CMD_ERASE = 2
CMD_PROGRAM = 3
CMD_READ = 4


def arm_bin(tool):
    import shutil

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


def probe_serial():
    if "--serial" not in sys.argv:
        raise SystemExit("pass --serial <j-link serial number>")
    index = sys.argv.index("--serial")
    if index + 1 >= len(sys.argv):
        raise SystemExit("pass --serial <j-link serial number>")
    try:
        return int(sys.argv[index + 1], 0)
    except ValueError:
        raise SystemExit("serial is not a number: %s" % sys.argv[index + 1])


def build_flasher():
    gcc = arm_bin("arm-none-eabi-gcc")
    objcopy = arm_bin("arm-none-eabi-objcopy")
    src = ROOT / "src" / "flash" / "bank.c"
    ld = ROOT / "src" / "flash" / "bank.ld"
    elf = OUT / "bank.elf"
    elf.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(gcc),
        "-mcpu=cortex-m0",
        "-mthumb",
        "-Os",
        "-ffreestanding",
        "-fno-builtin",
        "-ffunction-sections",
        "-nostdlib",
        "-Wl,--gc-sections",
        "-T",
        str(ld),
        "-o",
        str(elf),
        str(src),
    ]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        sys.stderr.write(done.stderr)
        raise SystemExit("flasher compile failed")
    done = subprocess.run(
        [str(objcopy), "-O", "binary", str(elf), str(FLASHER)],
        capture_output=True,
        text=True,
    )
    if done.returncode != 0:
        sys.stderr.write(done.stderr)
        raise SystemExit("flasher objcopy failed")
    symbols = subprocess.run(
        [str(arm_bin("arm-none-eabi-nm")), str(elf)],
        capture_output=True,
        text=True,
    ).stdout
    for line in symbols.splitlines():
        if line.endswith(" flash_entry"):
            addr = int(line.split()[0], 16)
            if addr != ENTRY:
                raise SystemExit("flash_entry at %08X" % addr)
            return
    raise SystemExit("flash_entry missing")


def build_image():
    OUT.mkdir(parents=True, exist_ok=True)
    payload = PAYLOAD.read_bytes()
    if len(payload) < 8 or len(payload) + 64 > (BANK_END - BANK):
        raise SystemExit("payload does not fit the application bank")
    sp, reset = struct.unpack_from("<II", payload)
    if not (0x07FC0000 <= sp < 0x07FD8000) or (reset & 1) == 0:
        raise SystemExit("vector table is not a DA14585 RAM image")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack(
        "<4sII16sI",
        bytes([0x70, 0x51, 0xAA, 0x00]),
        len(payload),
        crc,
        b"ATC_OEPL".ljust(16, b"\x00"),
        0,
    )
    header += bytes(32)
    if len(header) != 64:
        raise SystemExit("header is %u bytes" % len(header))
    image = header + payload
    BIN.write_bytes(image)
    print("firmware %u bytes crc %08X id 00 bank %05X %s" % (len(image), crc, BANK, BIN.name))
    return image


def probe_speed():
    """SWD clock in kHz. 4 MHz on a short lead; drop it if a flash fails."""
    if "--speed" in sys.argv:
        index = sys.argv.index("--speed")
        if index + 1 < len(sys.argv):
            return int(sys.argv[index + 1], 0)
    return 4000


def write_bytes(jlink, addr, data):
    """Word writes, not byte writes.

    pylink's memory_write8 puts one byte per SWD access, and a 256-byte page
    that way took well over a second: programming a 61 KiB image ran for six
    minutes. The same bytes as 32-bit words are four times fewer accesses and
    the probe pipelines them, which takes the same image under half a minute.
    Trailing bytes are padded with 0xFF - the mailbox is told the real length,
    so the padding is never programmed.
    """
    data = bytes(data)
    pad = (-len(data)) % 4
    if pad:
        data += bytes([0xFF]) * pad
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    jlink.memory_write32(addr, words)


def run_mail(jlink, cmd, addr, length):
    words = [MAGIC, cmd, addr, length, BUF, 0, 0]
    jlink.halt()
    jlink.memory_write32(MAIL, words)
    jlink.register_write("R13 (SP)", 0x07FD7F00)
    jlink.register_write("XPSR", 0x01000000)
    jlink.register_write("R15 (PC)", ENTRY)
    jlink._dll.JLINKARM_Go()
    deadline = time.time() + 8.0
    while time.time() < deadline:
        if jlink.halted():
            break
        time.sleep(0.01)
    else:
        jlink.halt()
        raise SystemExit("flasher did not halt cmd %u" % cmd)
    status, result = jlink.memory_read32(MAIL + 20, 2)
    if status != ST_OK:
        raise SystemExit(
            "flasher status %08X result %08X cmd %u addr %05X" % (status, result, cmd, addr)
        )
    return result


# Freezing the watchdog while the RAM flasher runs. See open_probe().
SET_FREEZE_REG = 0x50003300
FRZ_WDOG = 0x0008
# SET_FREEZE_REG lives in the peripheral power domain, which a tag halted out
# of extended sleep has switched off - the freeze write then lands nowhere and
# the watchdog resets the chip in the middle of programming. Raise the domain
# first and wait for it. PERIPH_SLEEP is bit 1, not bit 0.
PMU_CTRL_REG = 0x50000010
PERIPH_SLEEP = 0x0002
SYS_STAT_REG = 0x50000014
PER_IS_UP = 0x0008
WATCHDOG_REG = 0x50003100
# Silencing the BLE core before the flasher is uploaded.
#
# Halting the core does not stop the BLE timer. A tag that is advertising has
# a wake event pending in hardware, and when it comes due the wake sequence
# runs and resets the CPU - with the RAM flasher half way through a bank.
# That is what "flasher status 00000000 cmd 3" at 11.5 KiB was: deterministic
# on the first attempt after a power cycle, and gone on the second, because by
# then the firmware was no longer scheduling sleeps. Holding the BLE timer in
# reset and taking its clock away is what the SDK itself does before deep
# sleep (arch_sleep.c), and the chip reset at the end of programming undoes it.
CLK_RADIO_REG = 0x50000008
BLE_LP_RESET = 0x0040
BLE_ENABLE = 0x0080


def quiet_ble(jlink):
    radio = jlink.memory_read16(CLK_RADIO_REG, 1)[0]
    radio = (radio & ~BLE_ENABLE) | BLE_LP_RESET
    jlink.memory_write16(CLK_RADIO_REG, [radio & 0xFFFF])


def wake_peripherals(jlink):
    pmu = jlink.memory_read16(PMU_CTRL_REG, 1)[0]
    jlink.memory_write16(PMU_CTRL_REG, [pmu & ~PERIPH_SLEEP])
    for _ in range(200):
        if jlink.memory_read16(SYS_STAT_REG, 1)[0] & PER_IS_UP:
            return True
        time.sleep(0.005)
    return False


def open_probe(serial):
    import pylink

    build_flasher()
    jlink = pylink.JLink()
    jlink.open(serial_no=serial)
    try:
        opened = int(jlink.serial_number)
    except Exception:
        opened = serial
    if opened != serial:
        jlink.close()
        raise SystemExit("opened probe %s, wanted %s" % (opened, serial))
    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    # Probe power is opt-in, because it is wrong whenever something else
    # supplies the board. Two supplies on one rail fight each other: the rail
    # is no longer what either of them says, and current flows back into the
    # probe, which makes any measurement on it worthless. Pass --probe-power
    # only when the probe really is the only supply.
    if "--probe-power" in sys.argv:
        jlink.power_on(default=True)
        time.sleep(0.3)
    jlink.connect("DA14585", speed=probe_speed(), verbose=True)
    # Reset and halt before anything else. Halting a running tag leaves
    # whatever it was doing half done, and a core that has hard-faulted into
    # lockup will not execute the RAM flasher at all - that shows up as
    # "flasher did not halt cmd 1" and is otherwise baffling. Coming out of
    # reset halted gives the same clean start every time.
    try:
        jlink.reset(halt=True)
    except Exception:
        jlink.halt()
    time.sleep(0.05)
    jlink.halt()
    if not wake_peripherals(jlink):
        jlink.close()
        raise SystemExit("peripheral power domain stayed down; cannot freeze the watchdog")
    # Freeze the watchdog before anything is uploaded.
    #
    # Halting the core does not stop the watchdog, and a target that is
    # resetting on its own - a firmware stuck in a reboot loop is exactly when
    # you most want to reflash it - will reset in the middle of programming.
    # The RAM flasher is wiped with it and the bank is left half written,
    # which turns a recoverable tag into one that does not boot. Seen on this
    # bench: "flasher status 00000000 cmd 3 addr 21000" after 8 KiB.
    #
    # SET_FREEZE_REG is in the always-on domain, so this holds for as long as
    # the debugger is attached, and the chip reset at the end clears it.
    jlink.memory_write16(SET_FREEZE_REG, [FRZ_WDOG])
    quiet_ble(jlink)
    print("watchdog frozen, BLE timer held in reset")
    write_bytes(jlink, ENTRY, FLASHER.read_bytes())
    return jlink


def program(image, serial):
    jlink = open_probe(serial)
    try:
        jedec = run_mail(jlink, CMD_JEDEC, BANK, 1)
        print("jedec %06X" % jedec)
        if jedec == 0 or jedec == 0xFFFFFF:
            raise SystemExit("flash id rejected")

        sector = BANK
        end = BANK + len(image)
        while sector < end:
            run_mail(jlink, CMD_ERASE, sector, 0x1000)
            print("erased %05X" % sector)
            sector += 0x1000

        offset = 0
        while offset < len(image):
            page_off = (BANK + offset) & 0xFF
            room = 256 - page_off
            chunk = image[offset:offset + room]
            write_bytes(jlink, BUF, chunk)
            run_mail(jlink, CMD_PROGRAM, BANK + offset, len(chunk))
            offset += len(chunk)
            if offset % 4096 == 0 or offset == len(image):
                # Re-assert the freeze and reload the counter as we go.
                #
                # One write at the start was not enough: this failed twice at
                # the same point, about 8 KiB in, with the flasher no longer
                # answering at all - which is what a reset mid-program looks
                # like. SET_FREEZE_REG and WATCHDOG_REG both live in the
                # peripheral domain, so anything that drops and re-raises that
                # domain loses the freeze silently. Rewriting it every 4 KiB
                # costs one SWD write per chunk and removes the question.
                jlink.memory_write16(WATCHDOG_REG, [0x00FF])
                jlink.memory_write16(SET_FREEZE_REG, [FRZ_WDOG])
                print("programmed %u/%u (wdog %d)"
                      % (offset, len(image), jlink.memory_read16(WATCHDOG_REG, 1)[0]))

        offset = 0
        while offset < len(image):
            chunk = min(1024, len(image) - offset)
            run_mail(jlink, CMD_READ, BANK + offset, chunk)
            got = bytes(jlink.memory_read8(BUF, chunk))
            if got != image[offset:offset + chunk]:
                raise SystemExit("verify mismatch at %05X" % (BANK + offset))
            offset += chunk
        print("verify ok %u bytes" % len(image))

        # Restart into the new image.
        #
        # This has to be the DA1458x's own reset and not the Cortex-M0's.
        # SYSRESETREQ (AIRCR = 0x05FA0004) resets the core but leaves
        # SYS_CTRL_REG alone, and REMAP_ADR0 in that register is still
        # pointing address 0 at SysRAM - which at this moment holds the RAM
        # flasher this script just uploaded, not a vector table. The core
        # came back up inside the flasher, wedged on a bogus PC, and the tag
        # never advertised again. From the outside that is indistinguishable
        # from a bad flash, and it is why a freshly programmed tag looked
        # dead until it was power-cycled.
        #
        # The right sequence is the one the SDK uses in platform_reset_func():
        # clear REMAP_ADR0 so the ROM is mapped at 0, and set SW_RESET. The
        # ROM bootloader then runs, reads the bank over SPI and copies the
        # image into SysRAM, which is the whole point of resetting here.
        SYS_CTRL_REG = 0x50000012
        REMAP_ADR0 = 0x0003
        SW_RESET = 0x8000

        jlink.memory_write32(0xE000EDFC, [0])  # clear vector catch
        current = jlink.memory_read16(SYS_CTRL_REG, 1)[0]
        boot_rom = ((current & ~REMAP_ADR0) | SW_RESET) & 0xFFFF
        print("reset SYS_CTRL_REG %04X -> %04X" % (current, boot_rom))
        jlink.memory_write16(SYS_CTRL_REG, [boot_rom])
        time.sleep(0.05)
        try:
            jlink._dll.JLINKARM_Go()
        except Exception as exc:  # the core is already running after SW_RESET
            print("go: %s" % exc)
        time.sleep(1.0)
    finally:
        jlink.close()


def main():
    if not PAYLOAD.is_file():
        raise SystemExit("missing %s - run python tools/build.py first" % PAYLOAD)
    image = build_image()
    if "--image-only" in sys.argv:
        print("header %s" % BIN.read_bytes()[:8].hex())
        return
    program(image, probe_serial())
    print("bank programmed, target released to ROM boot")


if __name__ == "__main__":
    main()
