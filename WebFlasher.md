# Web flasher — plan

A browser page that identifies a connected board and writes the right firmware to
it, hosted on Cloudflare Pages. Its job is **first install and recovery**. Routine
updates already work over the air from the firmware update page, so this is for
boards that are new, bricked, or on a version too old to update itself.

## The constraint that shapes everything

Flashing from a browser means the **Web Serial API**, which exists only in
Chrome, Edge and Opera on desktop. Not Firefox, not Safari, nothing on iOS or
Android. That is not a gap that closes soon — Mozilla and Apple have both
declined to implement it.

So the page must say so plainly, up front, and keep the existing zip-and-script
route as the answer for everyone else. It supplements
`scripts/Upgrade-DiyBatteryBMS.ps1`, it does not replace it.

Web Serial also requires a secure context. Cloudflare Pages serves HTTPS by
default, so that costs nothing.

## What the browser can work out, and what it cannot

`esptool-js` speaks the same ROM protocol as `esptool`, so over Web Serial the
page can read:

| Readable | Example from the two boards on the bench |
| --- | --- |
| Chip family and revision | `ESP32-D0WDQ6-V3 (revision v3.1)`, `ESP32-S3 (QFN56) (revision v0.2)` |
| Flash size | 4MB, 16MB |
| PSRAM presence and size | `Embedded PSRAM 8MB (AP_3v3)`, or absent |
| MAC address | identity, and a way to remember a board between visits |
| Partition table at `0x8000` | whether this is already a DiyBatteryBMS board, and which layout |

What it **cannot** see is which board it is. `esp32dev`, `esp32plus` and
`esp32-ESPCAN` are all "an ESP32" to the chip; the difference is an MCP2515 on
SPI versus the built-in TWAI controller, and which pins are wired to what. None
of that is electrically detectable.

So: **detection narrows the list, a dropdown picks the rest.** Detection is still
worth having, because it can rule out the choices that would brick the board —
offering a 16MB-partition build to a 4MB chip, or a PSRAM build to a module
without any.

## Flow

1. **Connect.** User clicks, picks the port in the browser's dialog.
2. **Identify.** Chip family, revision, flash size, PSRAM, MAC.
3. **Read the partition table** at `0x8000`. If it parses and carries our layout,
   this is an existing install — remember where `nvs` is.
4. **Choose a board** from a dropdown, filtered to builds compatible with what
   was detected. Incompatible ones are shown greyed with the reason, not hidden;
   "why isn't my board listed" is worse than "your board has 4MB, this build
   needs 16MB".
5. **Decide install or upgrade** — see below. The page states which it is doing
   and why, before it does it.
6. **Back up NVS** to a file the user downloads, always, before writing anything.
7. **Write**, with a progress bar.
8. **Verify** — re-read the partition table, and offer to reconnect at 115200 to
   watch the boot log, which now prints the reset reason, PSRAM and heap.

## Install versus upgrade

This is where a flasher earns its keep or destroys somebody's configuration, and
`Upgrade-DiyBatteryBMS.ps1` has already worked out the rules. The web version
must follow them rather than invent a second set:

- **Never erase the whole chip on an upgrade.** Settings live in `nvs` at
  `0x9000` and survive a re-flash only because every partition table in this repo
  keeps `nvs` and `otadata` at byte-identical offsets.
- **Refuse when `nvs` has moved or resized** between the device's current table
  and the target build's. That is the one case where a flash that looks entirely
  normal silently wipes every setting. The script has a `-Force` for people who
  understand it; the page should have the same, worded as a warning rather than
  a checkbox.
- **Back up NVS first, byte-level, every time.** Cheap, and the only thing that
  turns a mistake into an inconvenience.
- **Full erase is for first install only** — a board with no recognisable
  partition table, or one deliberately being reset.

The partition change made for the S3 PSRAM env is a live example: a board coming
from `partitions_8mb.csv` to `partitions_16mb.csv` has `app1` and `coredump` at
different offsets, and `nvs` is the only region that lines up. That upgrade is
safe; one that moved `nvs` would not be.

## Artifacts and manifests

`scripts/package_release.py` already produces `firmware.factory.bin`, a merged
image, alongside the separate parts. Two things to get right:

- **The merged image is always written at `0x0`, on every chip.** `esptool`'s
  `merge_bin` pads from address 0 up to wherever that chip's bootloader
  actually lives (`0x1000` on ESP32, `0x0` on S3/C3) and bakes that padding
  into the file — that's the whole point of a factory image, one offset for
  every chip. A manifest that "corrects" the offset per chip instead
  double-shifts it, so the write completes and the board still doesn't boot.
  This bricked an `esp32dev` board before it was caught (see git history).
- **One manifest per board, not per chip family.** ESP Web Tools selects builds by
  `chipFamily`, which cannot distinguish `esp32dev` from `esp32plus`. Letting the
  dropdown choose the manifest URL keeps the component's polished install dialog
  *and* keeps its chip-family check as a backstop against an S3 build reaching an
  ESP32.

## Hosting

- **Cloudflare Pages** for the page. Static, HTTPS, free.
- **Binaries**: roughly 1.7MB per env and eight envs, so ~14MB per release. Pages
  will hold that, but every retained release multiplies it. **R2** is the better
  home once more than a couple of versions are kept, and it avoids the CORS
  question that comes with pulling assets from GitHub Releases.
- **CI**: a workflow that runs `pio run` (which builds every env in one pass —
  see the All Envs task), then publishes the factory binaries and generated
  manifests. The manifest generation belongs next to `package_release.py`, which
  already knows each env's partition table and version.

## Crash dump reader

Every partition table in this repo reserves a `coredump` partition — 64KB at the
top of flash — and nothing has ever read it. The IDF panic handler writes a full
register set and stack there when a board dies, and it survives the reboot.

A page that pulls that off a board and hands back a decoded backtrace is worth
more than the flashing feature. It is the difference between "it keeps
restarting" and "it aborted in `AsyncWebSocket::_queueMessage` with 900 bytes of
heap left", which is the question this firmware has spent real effort answering
by other means.

The mechanics are within reach: read the partition table at `0x8000` to find the
coredump partition, `read_flash` those 64KB over Web Serial, and check the header
— an empty partition is all `0xFF`, so "no crash recorded" is easy to report.

The honest part is decoding. Turning addresses into function names and line
numbers needs the `.elf` for the exact firmware that crashed, which means the
build that produced each release has to be kept and matched by version. Without
it the page can still show the reset reason, the faulting PC and the raw stack,
which is enough to file a useful bug report. `espcoredump.py` does the full job
offline and ships with the IDF, so a first version could simply hand the user a
`.bin` and the command to run.

Worth doing after the flasher works, and worth designing the release pipeline
around now — keeping each build's `.elf` costs nothing today and cannot be
recovered later.

## Worth considering later

- **Improv Serial** would let the page hand the board its WiFi credentials
  straight after flashing. ESP Web Tools supports it, but the firmware would need
  to speak the protocol — it currently has its own serial setup in
  `SerialSetup.h`, so this is real work, not a checkbox.

## Open questions

- Keep every released version selectable, or only the latest plus one previous?
- Should the page offer the plain `.bin` for the firmware's own OTA page, for
  users on browsers without Web Serial?
- Does the dropdown list board names (`LilyGo T-CAN485`) or env names
  (`esp32-ESPCAN`)? Board names are kinder, but the README and release zips are
  organised by env.
