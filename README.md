# ATC_BLE_OEPL_DA14585

Custom firmware for Renesas **DA14585** electronic shelf labels.

The tag is a BLE peripheral with one service and one characteristic. Over it a host
sends pictures to the e-paper, updates the firmware, drives the RGB LED, stores NDEF
bytes, locks the tag with a PIN, changes the panel type or the BLE address, and reads
the tag's own log live. Everything needed to build it is in this repository,
including the SDK.

| | |
| --- | --- |
| Target | Renesas DA14585, image runs from SysRAM, loaded from SPI flash by the ROM bootloader |
| Advertised name | `ATC_` + the last three address octets, e.g. `ATC_279DFC`, in the scan response |
| Service / characteristic | `0x1337` (both), on the Bluetooth base UUID |
| Firmware version | `0x0036` (54) |
| Boards | One config per PCB: which glass, and whether the LED and NFC are fitted. Config `1` is the 7.5″ 880 × 528 black/white/red |
| Firmware file | `firmware/ATC_OEPL_DA14585.bin`, 64 080 bytes |
| License | **GPL-3.0-or-later**; the bundled SDK keeps its own terms |

---

## Contents

- [Flash a tag](#flash-a-tag)
- [What the tag does](#what-the-tag-does)
- [Build it yourself](#build-it-yourself)
- [Releases](#releases)
- [Configure the board](#configure-the-board)
- [Commands](#commands)
- [Flash layout](#flash-layout)
- [Pins](#pins)
- [License](#license)

---

## Flash a tag

You need a **SEGGER J-Link** on the tag's SWD pads, Python 3, and `pip install -r
requirements.txt` for `pylink-square`.

```sh
python tools/flash.py --serial <j-link serial number>
```

It writes `firmware/ATC_OEPL_DA14585.bin` after `python tools/build.py` has made
it, or pass `--image-only` to rebuild the file from an existing payload without
opening a probe. There is no default probe; the serial number is the one J-Link
Commander prints.

**Power-cycle the tag first.** SWD is on `P1_4` (SWCLK) and `P1_5` (SWDIO), and the
firmware hands those two pads back to the GPIO block **30 seconds after boot**, which
is most of why an idle tag is quiet. A probe can only attach inside that window, so
the sequence is: remove power, reapply it, flash. `ATC_SWD_GRACE_SECONDS` in
`src/app/user_atc.c` sets the window.

**Do it while the glass is idle.** The tool halts the core and loads a small flasher
into RAM. A refresh, a picture erase or a firmware copy that is still running will be
cut off and the glass can be left half drawn.

It touches **only** the application bank, `0x0001F000`…`0x00038000`. The bootloader,
the stock image, the pin record, the waveform, the NDEF sector, the settings record
and the picture window are all left alone.

What it does, in order:

1. Builds `src/flash/bank.c` and checks that `flash_entry` landed at `0x07FD6000`.
2. Opens the J-Link in SWD mode. Probe target power is opt-in with `--probe-power`;
   it is wrong whenever something else supplies the board. Connects as `DA14585`.
3. Halts, raises the peripheral power domain, freezes the watchdog and holds the BLE
   timer in reset — halting the core does not stop that timer, and a pending
   advertising wake will reset the CPU with the flasher half way through a bank.
4. Reads the JEDEC id; `0x000000` and `0xFFFFFF` are rejected.
5. Erases 4 KiB sectors across the bank, programs 256-byte pages, reads them back.
6. Restarts the chip the DA1458x way: clear `REMAP_ADR0` in `SYS_CTRL_REG` so the ROM
   is mapped at address 0, then set `SW_RESET`. A plain `SYSRESETREQ` would come back
   up inside the RAM flasher and the tag would never advertise again.

The ROM bootloader then copies the image from the bank into SysRAM and runs it.

**A freshly flashed tag is unconfigured** and leaves the glass alone until it is told
which board it is — one command, see [Configure the board](#configure-the-board).

### Over the air, without a probe

Once a tag runs this firmware it can be updated over BLE: send the same `.bin` as an
`AvailDataInfo` transfer of type `0x03` (see [Commands](#commands)). The tag stages
the file in its picture window, checks the boot header *before* erasing anything,
copies it into the bank and reboots. A full update takes about 45 s. Because the file
is staged in the picture window, a firmware update replaces the stored picture.

---

## What the tag does

| | |
| --- | --- |
| **Pictures** | Two 1-bit planes, black and red, raw (`0x20` / `0x21`) or zlib-compressed (`0x30`). 880 × 528 is 116 160 bytes and takes about 70 s over BLE. The picture is kept in flash and redrawn after a power cycle |
| **Board config** | Which glass is fitted, and whether the PCB has the RGB LED and an NFC chip. Set once with `0x0004`, kept in flash |
| **Status screen** | Drawn on every boot when no picture is stored: address, firmware version, panel, battery, temperature |
| **RGB LED** | Steady colours and blink patterns, `0x0001` and `0x0015`, on the boards that have one fitted |
| **NFC** | The PCB's NFC tag chip sits on the MCU's I2C bus, so a record sent to the tag is written into the chip as well and a phone held to the label reads it. Send one as a normal transfer of type `0xA0` (a type 2 TLV) or `0xA1` (a bare URL), or with `0x0018`…`0x001D`; the tag keeps its own copy in flash and `0x0019` returns it |
| **PIN lock** | A RAM-only PIN that refuses every command but two while the tag is locked |
| **Log stream** | `0x0022` pushes the tag's own debug output to the host as notifications |
| **Sleep** | Extended sleep between advertisements, on by default. The advertising interval is 2 s and `0x0008` changes it |

---

## Build it yourself

Needed: **Python 3**, and a host C compiler (`gcc`, `clang` or `cc`) only if you want
to run the tests. Everything else is fetched or already here.

```sh
python tools/setup.py     # the compiler, once per clone
python tools/build.py
```

`tools/setup.py` downloads the xPack build of **GCC 10.3.1 for arm-none-eabi**,
checks it against a pinned SHA-256 and unpacks it into `tools/gcc-arm-none-eabi/`,
which the build uses ahead of anything on `PATH`. Nothing is installed system-wide.
The **DA145xx SDK 6.0.22.1401** is already in `sdk/6.0.22.1401/`, so there is nothing
to register for and nothing else to download. To build against a different copy of
the SDK, `python tools/setup.py --sdk <directory or archive>` writes `sdk_root.txt`,
which the build reads ahead of the bundled tree.

| Output | What it is |
| --- | --- |
| `build/atc_oepl.bin` | **The firmware file.** 64-byte boot header plus the RAM payload. J-Link writes this, and a host sends this same file |
| `build/atc_oepl.payload` | The bare payload, an intermediate. **Not** a firmware file — no header, and the bootloader refuses it |
| `build/atc_oepl.elf` | The linked image, with `build/atc_oepl.map` |

This tree links a 61 576-byte payload, so a 61 640-byte firmware file. Payload plus
header has to fit the 102 400-byte application bank.

```sh
python tools/test.py      # 184 checks: protocol, settings record, panel catalog
```

Three modules build for the host as well as for the DA14585 — the protocol, the
settings record and the panel catalog. The PIN, the lock gate and the stored address
are where a mistake is expensive and hard to see on a tag, so they are tested where a
test takes a second. Warnings are errors there.

---

## Releases

`.github/workflows/build.yml` runs on every push: the host tests, then the firmware.
A push to the default branch refreshes a rolling **`latest`** release; a `v*` tag
makes a release of its own. Both carry the same three files:

| Asset | |
| --- | --- |
| `ATC_OEPL_DA14585.bin` | The firmware. Short name, no version in it: an access point lists the files it can send by name and its name field is short, so the version lives in the release title, the notes and the image's own header |
| `ATC_OEPL_DA14585.elf`, `.map` | For chasing a symbol or a size |

`firmware/ATC_OEPL_DA14585.bin` in the tree is the same file, ready to flash without
building anything.

Nothing has to be configured: the SDK is in the tree and the only thing the runner
fetches is the compiler, pinned and cached, so a build takes about a minute. A fork
builds and releases the same way.

---

## Configure the board

The same firmware runs on boards that are not the same: a different glass behind the
same connector, the RGB LED fitted or not, an NFC chip or not. One row in
`src/app/panel.c` is one board, and a tag is told which row it is.

| Config | Glass | Pixels | Colours | LED | NFC | |
| --- | --- | --- | --- | --- | --- | --- |
| `0` | — | — | — | — | — | **unconfigured**, where a freshly flashed tag starts. Advertises hardware type `0xFA`, which an access point reads as config mode |
| `1` | ZKC75S, 7.5″ | 880 × 528 | black / white / red | yes | yes | hardware type `0x73`. **Verified on hardware** |
| `2` | ZKC133V-N, 13.3″ | 960 × 680 | black / white / red | no | yes | hardware type `0x74`, `tagtypes/74.json`. **Verified on hardware** |

The id is big-endian in command `0x0004`:

```
00 04 00 01   -> 0x0063, and the tag reboots into config 1
00 04 00 00   -> back to unconfigured
00 04 00 63   -> 0xFFFF, not a config this build carries
```

An access point does the same thing its own way, and this firmware answers
that too: an `AvailDataInfo` of type **`0xAF`** carries a command rather than
data, and an argument of **`0xA0` + config id** selects a board — `0xA2` for
the 13.3″ glass. Argument `0` reboots. That is the pairing that makes a tag
usable from an access point without a word of configuration: an unconfigured
tag advertises `0xFA`, the access point recognises config mode and offers the
list, and the choice comes back as `0xAF` with `0xA0` + id.

The geometry is read once at startup, so the tag stores the choice and restarts into
it. Setting the config it already has is acknowledged **without** a reboot, so a host
that configures on every connect does not keep restarting the tag. Changing it drops
the marker that says a picture is stored, because that picture is the wrong shape for
the new glass.

**Unconfigured means the tag leaves the glass alone.** It advertises and answers
commands, reports config `0` and a 0 × 0 panel in `0x0005` and `0x0021`, and refuses
a picture: there is no geometry to draw into, and writing a guessed size into an
unknown panel is worse than writing nothing. Firmware updates and NDEF transfers are
still accepted, so a tag can always be recovered over the air.

**The picture window is worked out at boot**, not compiled in. The firmware reads the
capacity out of the flash chip's JEDEC id and puts the window at the top of it, as
many whole 64 KiB blocks as two planes and the marker sector need, never less than
128 KiB and never reaching below `0x60000`. A 512 KiB board with the 7.5″ glass gets
`0x60000`…`0x80000`, which is where that window has always been; a 2 MiB board with
the 13.3″ glass gets `0x1D0000`…`0x200000`. A config whose picture will not fit the
flash that is fitted is refused rather than stored.

A board whose row does not carry the LED leaves those pads alone; `0x0001` and
`0x0015` still acknowledge, so a host does not have to know which board it is talking
to — there is simply nothing to light.

### Adding a board

A glass on the same controller is one more row:

```c
/* src/app/panel.c */
static const atc_panel catalog[] = {
    /* id   hw       w     h    col driver          caps                       boost  name */
    {   0u, 0x00FAu,   0u,   0u, 0u, ATC_DRV_NONE,   0u,                        0x00u, "unconfigured"       },
    {   1u, 0x0073u, 880u, 528u, 2u, ATC_DRV_ZKC75S, ATC_CAP_LED | ATC_CAP_NFC, 0x40u, "7.5 BWR ZKC75S"     },
    {   2u, 0x0074u, 960u, 680u, 2u, ATC_DRV_ZKC75S, ATC_CAP_NFC,               0x80u, "13.3 BWR ZKC133V-N" },
};
```

`boost` is the last byte of the controller's `0x0C` booster soft start, the one
number the two panels disagree on. An access point also needs a tag type file for a
new glass: `tagtypes/74.json` here is the one for the 13.3″ board, and it belongs in
the access point's `/tagtypes/` as `<hardware type>.json`.

`caps` is what the PCB carries, not what the firmware can do: `ATC_CAP_LED`,
`ATC_CAP_NFC`, or `0u` for a board with neither. `hw_type` is the hardware type that
goes into the advertisement.

`python tools/test.py` then checks the row: unique ids, width a multiple of 8 and at
most 1024 px, and two 1-bit planes fitting the 126 976-byte picture window — so
width × height ≤ 507 904 pixels. A different controller needs its own source file, a
driver id in `panel.h`, and `glass_ok()` in `src/app/epd.c` taught to accept it.

---

## What the tag advertises

Every advertisement carries one manufacturer record, company id `0x1337`, so an
access point learns the state of a tag without connecting to it. 16 bytes, after
the length and type bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | company id, `0x1337` |
| 2 | 1 | layout version, `2` |
| 3 | 2 | hardware type, or `0x00FA` while unconfigured |
| 5 | 2 | firmware version |
| 7 | 2 | capabilities |
| 9 | 2 | battery, mV |
| 11 | 1 | temperature, °C, signed |
| 12 | 1 | a counter, one up per advertisement |
| 13 | 2 | **CRC of the picture on the glass** |
| 15 | 1 | **wake byte** |

The last three bytes sit behind the version-2 layout rather than behind a version
of their own: an access point dispatches on the version byte and refuses what it
does not know, so a tag announcing version 3 would vanish from an older one
instead of merely losing a field. Appended, an old access point copies its 13
bytes and never looks further, and a newer one finds them because the record is
longer and because capability bit `0x0200` says they are there.

**The picture CRC** is CRC-16/CCITT-FALSE over the eight bytes of the `dataVer`
that is on the glass, little end first. A tag showing nothing transferred reports
`0x313E`, which is not a reserved value but simply the CRC of eight zero bytes;
the access point arrives at the same number by running the same bytes through the
same function.

After a restart a configured tag reports `0x313E` again, and that is the truth
rather than forgetfulness: it draws its status screen on every boot, so the
transferred picture is no longer what is on the glass. The picture is still in
flash and the next `0x0064` redraws it, which is exactly what an access point
does when it sees this. The version is kept in the marker sector beside the
picture, for the case where the tag comes up and does not redraw - an
unconfigured one, which leaves the glass alone.

**The wake byte** is an event counter in the top nibble and a reason in the
bottom one: `0` nothing to report, `1` boot, `2` GPIO, `3` NFC, `4`–`6` buttons,
`7` failed firmware, `8` first boot, `9` network scan, `10` watchdog. This tag
reports `1` after every start and `0` once an access point has talked to it.
A DA14585 has no reset-cause register — `0x50000304` is a DA14531 register and
reads zero here — so a power-on, a commanded reboot and a watchdog reset are not
told apart. The counter is what makes the byte usable from a passive scan: an
advertisement cannot be acknowledged, so the reason stays standing until the next
exchange clears it, and a receiver keeps the last byte it saw per tag and acts
when it changes.

Capabilities follow the board config: `0x0100` BLE and `0x0002` compression
always, `0x0200` for the fields above, plus `0x0001` when the PCB has the LED and
`0x0040` when it has the NFC chip.

**The name is in the scan response**, not the advertisement: 16 bytes of
manufacturer data and a ten character name are more than the 31 an advertisement
holds. The scan response carries the service UUID `0x1337` and the name, which is
where the reference firmware keeps it too — an active scan sees it, a passive one
does not need it.

## Commands

A command is a **big-endian 16-bit opcode** followed by its payload, written to
characteristic `0x1337`. Notifications have the same shape. **Anything not
implemented answers `0xFFFF`**, which is a normal answer and not a fault.

| Opcode | Name | Answer | Notes |
| --- | --- | --- | --- |
| `0x0001` | LED | `0x0063` | One colour byte, 0…7. Cancels a running blink |
| `0x0002` | Fast link | `0x0063` | Asks once for a 7.5–15 ms connection interval |
| `0x0003` | Slow link | `0x0063` | Asks once for 30–50 ms |
| `0x0004` | Set config | `0x0063` | Config id, big-endian; `0` is unconfigured. Stores and reboots |
| `0x0005` | Panel info | `0x0005` + 176 B | Geometry; a host sizes its canvas from this. Zeros while unconfigured |
| `0x0008` | Advertising period | `0x0063` | 0.625 ms slots, 100…10000, else 3200 |
| `0x0009` | Set address | `0x0063` | Eight bytes. Stores and reboots |
| `0x0011` | Dynamic read | `0x00CD` + 93 B | Geometry only |
| `0x0014` | Deep sleep | `0x0063` | Accepted and ignored; the tag does not sleep |
| `0x0015` | Blink | `0x0063` | Twelve bytes: colour, on-time, counts |
| `0x0016` | Status | `0x00D1` + 6 B | Battery, raw ADC, temperature |
| `0x0017` | Reboot | `0x0063` | Refused while busy |
| `0x0018` | Set NDEF | `0x0063` | Stores bytes |
| `0x0019` | Get NDEF | `0x00D2` | Optional little-endian offset |
| `0x001A` | Clear NDEF | `0x0063` | |
| `0x001C` / `0x001D` | NDEF commands off / on | `0x0063` | Flag only |
| `0x001E` | Set / remove PIN | `0x0063` | Payload sets, bare removes |
| `0x001F` | Unlock / lock now | `0x0063` | Payload unlocks, bare locks |
| `0x0020` | Protection | `0x00D4` + 6 B | PIN state and both timeouts |
| `0x0021` | Sysinfo | `0x00D5` + 116 B | The whole readout |
| `0x0022` | Log stream | `0x0063` | `01` starts, `00` stops; text arrives as `0x00D6` |
| `0x0023` | Sleep on / off | `0x0063` | Runtime switch, from RAM; a power cycle undoes it |
| `0x0064` | AvailDataInfo | `0x00C6` | Starts a picture, firmware or NDEF transfer |
| `0x0065` | Block part | `0x00C5` | One part of the current block |

**The tag never goes into deep sleep.** `0x0014` is acknowledged so that an access
point stops asking, and then ignored, because nothing on this board can wake the
chip out of a deep sleep: the reset pin cannot restart a DA14585, so a tag that
took the command came back only by losing power - which on a sealed label means
opening it. `0x0017` reboots, and `0x0023` switches the sleep mode the tag uses
while it runs.

### Transfers

`0x0064` announces a version, a size and a type — `0x20`/`0x21` raw picture, `0x30`
zlib picture, `0x03` firmware, NDEF. The tag answers `0x00C6` with the block it wants
and a 6-byte part mask, one bit per part, LSB first. Each `0x0065` carries one 230-byte part
and is acknowledged with `0x00C5`; a bad checksum comes back as `0x00C4`. When the
last block is in, the tag answers `0x00C7` and starts the refresh, or `0x00C8` if it
already holds that version. A firmware transfer ends in `0x00C9`.

Blocks are 4096 bytes, so 18 parts, and a 116 160-byte picture is 29 blocks. One part
in flight is the reliable mode: about 1.6 KiB/s, 70 s for a full picture. The tag
holds the connection while it works, and it stops advertising while a host is
connected.

### LED

`0x0001` takes one colour byte: `0` off, `1` red, `2` green, `3` blue, `4` yellow,
`5` magenta, `6` cyan, `7` white. A missing byte means 7 and anything above 7 is
clamped. The LEDs are common anode — a pad is driven **low** to light one — which is
why the LED is on from reset until the firmware takes over. On a board whose config
has no LED, both commands acknowledge and do nothing.

`0x0015` blinks: colours are RGB332, the on-time is the high nibble of byte 0 (`0` is
500 µs, `1`…`14` are milliseconds, `15` stays on until the next pulse). The pulse is
a busy-wait, so a long on-time blocks the radio for that long.

### PIN

A PIN is at most 16 bytes and is **held in RAM only** — these tags have no button and
no console, so a stored PIN nobody remembers would be a brick. A power cycle is the
escape hatch.

An unlock expires after **60 s** of quiet and at **600 s** whatever the traffic, and
the link dropping locks the tag at once. While it is locked exactly two commands get
through: `0x0020`, so a host can find out why it is being refused, and `0x001F`, so it
can do something about it. A wrong PIN is compared over its whole stored length, so
the answer carries nothing about how much of it was right.

---

## Flash layout

A 4 Mbit SPI NOR, JEDEC `0x00A14013`, 524 288 bytes, clocked at 2 MHz on the ROM boot
pins.

| Start | End | Contents |
| --- | --- | --- |
| `0x00000000` | `0x00004000` | Bootloader. Not programmed |
| `0x00004000` | `0x00014000` | The image in the first boot slot. Readable; erase and program refuse it |
| `0x0001F000` | `0x00038000` | **Application bank, 102 400 bytes. This firmware.** Image id `0x00` is preferred over `0xFF` |
| `0x00039000` | `0x0003A000` | Pin record. Read-only |
| `0x0003B000` | `0x00040000` | Waveform. Read-only |
| `0x00050000` | `0x00051000` | NDEF sector, at most 512 message bytes |
| `0x00051000` | `0x00052000` | Settings record — config id and address, 32 bytes |
| `0x00060000` | `0x00080000` | Picture window, 131 072 bytes, one slot |
| `0x0007F000` | | `ATCIMG` marker, written after a successful refresh |

### Boot header

64 bytes, then the RAM payload:

| Offset | Value |
| --- | --- |
| 0 | `0x70`, `0x51` — signature |
| 2 | `0xAA` — valid |
| 3 | `0x00` — image id |
| 4 | Payload length, little-endian |
| 8 | zlib CRC-32 of the payload |
| 12 | `ATC_OEPL`, 16 bytes, zero-padded |
| 36 | `0x00` — not encrypted |

A file is checked before anything is erased: the version string, the code size
against the bank, the file length against the header, and the first two words of the
payload against a DA14585 vector table — stack pointer inside
`0x07FC0000`…`0x07FD8000`, reset vector Thumb and in the same range. An 8-byte prefix
`00 00 00 00 93 27 00 00` in front of the header is accepted and stripped.

---

## Pins

| Function | Pin | | Function | Pin |
| --- | --- | --- | --- | --- |
| Flash CLK | `P0_0` | | Panel MOSI | `P2_0` |
| Flash CS | `P0_3` | | Panel CS | `P2_1` |
| Flash MISO | `P0_5` | | Panel BS | `P2_2` |
| Flash MOSI | `P0_6` | | Panel power | `P2_3` |
| Panel SCLK | `P0_1` | | LED red | `P0_2` |
| Panel DC | `P0_7` | | LED green | `P2_5` |
| Panel RST | `P1_0` | | LED blue | `P2_9` |
| Panel BUSY | `P1_1` | | SWD | `P1_4` clock, `P1_5` data |
| NFC SDA | `P2_7` | | NFC CS | `P2_6`, active low |
| NFC SCL | `P2_8` | | NFC IRQ | `P1_2` |

The NFC chip's VCC is permanently on, so CS decides who has it: high, and the chip
belongs to its radio side; low, and it answers the MCU while a reader gets no
further than anticollision. So CS idles high and SDA and SCL idle as inputs on
their pull-ups - driving them is what keeps the chip from ever answering.

`P1_3`, `P0_4`, `P2_4` and `P3_0` are held high; LED colour 7 drives them too. None
of the LED pads is touched on a board whose config does not carry `ATC_CAP_LED`,
and the NFC bus is only used on a board whose config carries `ATC_CAP_NFC`.

**Do not halt the core during a refresh.** The refresh clocks each plane through the
GPIO registers: one step every 10 ms, up to 16 rows a step, the first step 100 ms
after the picture is accepted, and the wait on BUSY bounded at 40 s. Sending a new
picture while one is still drawing stops the old refresh, so wait for BUSY to fall.

---

## License

The code in this repository — everything under `src/`, `test/` and `tools/` — is
**GPL-3.0-or-later**; `LICENSE` is the full text.

`sdk/6.0.22.1401/` is the Dialog / Renesas DA145xx SDK, included so that a clone
builds on its own. It is **not** covered by the GPL: it keeps its own terms, which
are in `sdk/6.0.22.1401/doc/licensing.txt` and `.../doc/renesas_license.txt`, and
that covers the prebuilt stack library
`sdk/6.0.22.1401/sdk/platform/system_library/output/IAR/da14585_586.a` as well. A
firmware image built from this tree links the two together, so read those terms
before redistributing binaries.

The GNU Arm Embedded toolchain that `tools/setup.py` downloads is not part of this
repository and keeps the license files shipped with it.
