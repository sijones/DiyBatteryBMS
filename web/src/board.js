/* Talking to a board over Web Serial.
 *
 * Everything that touches esptool-js lives here, so the UI never has to know
 * about transports, stubs or reset strategies.
 */

import { ESPLoader, Transport } from "esptool-js";
import {
  PARTITION_TABLE_OFFSET,
  PARTITION_TABLE_SIZE,
  parsePartitionTable,
  findNvs,
  findCoredump,
  looksLikeOurLayout,
} from "./partitions.js";

export function webSerialSupported() {
  return typeof navigator !== "undefined" && "serial" in navigator;
}

/**
 * Which browser this is, for telling someone their own browser will not do
 * rather than reciting a list and leaving them to work out which line is theirs.
 *
 * User-agent sniffing, with all the usual caveats - it is a claim the browser
 * makes about itself, not a fact - but it is only used to word a message. What
 * the page actually DOES is decided by webSerialSupported(), which asks whether
 * the API is there. A browser lying about its name gets a slightly odd sentence;
 * it never gets a broken flasher.
 *
 * Order matters and is the opposite of obvious: Chrome, Edge and Opera all
 * carry "Safari" in their UA string, Edge and Opera both carry "Chrome", so the
 * specific tokens have to be checked before the general ones.
 *
 * @returns {string|null} a name to put in a sentence, or null if unrecognised
 */
export function browserName() {
  const ua = typeof navigator !== "undefined" ? navigator.userAgent || "" : "";
  if (/Firefox\/|FxiOS\//.test(ua)) return "Firefox";
  if (/OPR\/|OPiOS\/|Opera/.test(ua)) return "Opera";
  if (/Edg(A|iOS|)\//.test(ua)) return "Edge";
  if (/Chrome\/|CriOS\//.test(ua)) return "Chrome";
  if (/Safari\//.test(ua)) return "Safari";
  return null;
}

/**
 * True on a phone or a tablet, where the answer is not "use another browser".
 *
 * No browser on iOS or Android implements Web Serial, and on iOS none can:
 * every browser there is Safari's engine wearing a different name, so
 * "install Chrome" is advice that cannot work. iPadOS reports itself as a Mac,
 * hence the touch-points check - a real Mac reports 0.
 */
export function isMobile() {
  if (typeof navigator === "undefined") return false;
  const ua = navigator.userAgent || "";
  if (/Android|iPhone|iPad|iPod/i.test(ua)) return true;
  return /Macintosh/.test(ua) && (navigator.maxTouchPoints ?? 0) > 1;
}

/**
 * esptool-js reports chip features as a list of strings - the same ones
 * esptool's "Features:" line carries. PSRAM shows up there and nowhere else
 * over the wire, so this is how the page knows an S3 has 8MB of it.
 *
 * Two forms seen in the wild:
 *   "Embedded PSRAM 8MB (AP_3v3)"   in-package, S3R8 and friends
 *   "Embedded PSRAM 2MB (AP_3v3)"   the R2 parts
 * External PSRAM on a WROVER is NOT reported - it cannot be detected without
 * initialising it - so absence here means "none found", not "none fitted".
 */
function psramFromFeatures(features) {
  for (const f of features) {
    const m = /Embedded PSRAM\s+(\d+)\s*MB/i.exec(f);
    if (m) return { present: true, megabytes: Number(m[1]), detail: f };
  }
  return { present: false, megabytes: 0, detail: null };
}

/**
 * Connect, identify, and read the partition table.
 *
 * The whole identify step runs on one connection: opening the port, syncing
 * with the ROM and uploading the stub is by far the slowest part, so doing it
 * once and reading everything is much faster than a call per fact.
 *
 * @param {(line: string) => void} log
 */
/* Flashing speed.
 *
 * The ROM bootloader always answers at 115200; esptool-js syncs there and then
 * raises the rate itself, so `baudrate` is the speed the image actually
 * transfers at. Leaving it at 115200 - which is what this did at first - means
 * roughly three minutes to write 1.65MB, and there is no reason to pay that.
 *
 * A ladder rather than one value, because the fastest rate is not universally
 * safe: it depends on the bridge chip, the cable and sometimes the USB port.
 * A CH343 or CP2102 manages 921600 comfortably; a long lead or a tired clone
 * adapter may not, and the failure is a sync error rather than corruption.
 * Native-USB boards ignore the number entirely - it is a USB endpoint, not a
 * UART - so they lose nothing by trying the top of the ladder first.
 */
const BAUD_LADDER = [921_600, 460_800, 115_200];

/**
 * Did this fail because the PORT would not open, rather than because the board
 * did not answer?
 *
 * Worth telling apart, because the two have nothing in common. A port that will
 * not open says nothing about the board on the end of it - something else is
 * holding it - so trying a slower baud rate cannot help, and advice about
 * holding the boot button is a false lead that sends people hunting for a
 * hardware fault they do not have.
 *
 * Chromium words it two ways, and the wording is not stable enough to match on
 * alone, so the DOMException name is checked first:
 *
 *   NetworkError       "Failed to execute 'open' on 'SerialPort': Failed to open serial port."
 *   InvalidStateError  "The port is already open."
 */
export function isPortBusy(e) {
  if (e?.name === "NetworkError" || e?.name === "InvalidStateError") return true;
  return /failed to open serial port|port is already open/i.test(e?.message ?? "");
}

/**
 * Sync with the ROM bootloader on an already-open port and hand back a
 * working ESPLoader, walking BAUD_LADDER down from the fastest rate.
 *
 * Split out of connectAndIdentify() so reconnectAndReadNvs()'s post-flash
 * re-sync does not have to repeat this, and - the part that matters - does
 * not call navigator.serial.requestPort() again. That call is what pops the
 * native port picker; the port has already been granted once for this
 * session; asking a second time mid-flow would stop an automatic
 * verification step on a dialog nobody was told to expect.
 */
async function syncLoader(port, log) {
  const terminal = {
    clean() {},
    writeLine(data) { log(String(data)); },
    write(data) { log(String(data)); },
  };

  let transport = null;
  let loader = null;
  let lastError = null;

  for (const baudrate of BAUD_LADDER) {
    transport = new Transport(port, true);
    loader = new ESPLoader({ transport, baudrate, romBaudrate: 115_200, terminal, enableTracing: false });

    try {
      // main() syncs, identifies the chip, uploads the stub and raises the
      // rate, and returns a human-readable chip description ("ESP32-S3
      // (QFN56) (revision v0.2)") that chip.CHIP_NAME alone does not carry.
      const description = await loader.main();
      if (baudrate !== BAUD_LADDER[0]) log(`Settled at ${baudrate} baud.`);
      return { transport, loader, description };
    } catch (e) {
      lastError = e;

      /* A port that will not open will not open any slower. Stop here rather
         than working down the ladder printing the same failure three times a
         quarter-second apart - that buries the one line that matters and makes
         a busy port look like a flaky board. */
      if (isPortBusy(e)) {
        try {
          await transport.disconnect();
        } catch {
          /* it never opened; there is nothing to hand back */
        }
        throw e;
      }

      log(`${baudrate} baud did not work (${e.message}) — trying slower.`);
      // Let go of the port before the next attempt, or reopening it fails
      try {
        await transport.disconnect();
      } catch {
        /* nothing to do but try the next rate */
      }
      await new Promise((r) => setTimeout(r, 250));
      loader = null;
    }
  }

  throw lastError ?? new Error("Could not talk to the board at any speed.");
}

export async function connectAndIdentify(log) {
  if (!webSerialSupported()) throw new Error("This browser has no Web Serial support.");

  // Throws on user cancel, which the caller treats as "nothing happened"
  const port = await navigator.serial.requestPort();

  const { transport, loader, description } = await syncLoader(port, log);

  const chip = loader.chip;
  const features = await chip.getChipFeatures(loader);
  const mac = await chip.readMac(loader);
  const flashSizeBytes = await loader.getFlashSize();

  const info = {
    chipName: chip.CHIP_NAME,
    description,
    features,
    psram: psramFromFeatures(features),
    mac,
    flashSizeBytes: flashSizeBytes * 1024, // getFlashSize() answers in KB
  };

  // The table is 3KB at a fixed offset; reading it costs a moment and decides
  // everything about whether an install can keep the user's settings.
  let table = { partitions: [], valid: false };
  try {
    const bytes = await loader.readFlash(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE);
    table = parsePartitionTable(bytes);
  } catch (e) {
    // A blank board has nothing there, which is a legitimate answer rather
    // than a failure - report it as "no table" and let the caller decide.
    log(`Could not read the partition table: ${e.message}`);
  }

  return {
    loader,
    transport,
    port,
    info,
    table,
    nvs: findNvs(table.partitions),
    coredump: findCoredump(table.partitions),
    recognised: looksLikeOurLayout(table.partitions),
  };
}

/**
 * Reconnect after a flash to read nvs back and check it survived.
 *
 * Reuses the already-granted port rather than requesting a new one - see
 * syncLoader()'s comment - so this can run right after watchBoot() with no
 * extra dialog. Self-contained: syncs, reads, and releases the transport
 * again before returning, since a verify-only read has no reason to hold the
 * port open afterward the way a flash in progress does.
 *
 * @param {SerialPort} port
 * @param {{offset:number, size:number}} nvs
 * @param {(line: string) => void} log
 * @returns {Promise<Uint8Array>}
 */
export async function reconnectAndReadNvs(port, nvs, log) {
  const { transport, loader } = await syncLoader(port, log);
  try {
    return await loader.readFlash(nvs.offset, nvs.size);
  } finally {
    try {
      await bounded(transport.disconnect(), 3000);
    } catch {
      /* best effort - the caller only needed the bytes */
    }
  }
}

/**
 * Hand the port back.
 *
 * Every step is time-boxed: a serial operation on a board that has just
 * rebooted, or been unplugged, can stall indefinitely, and a teardown that
 * hangs leaves the page stuck exactly where a failed read would.
 *
 * `finished` means the watcher already took the port and closed it, so there
 * is nothing here to disconnect.
 */
const bounded = (p, ms) => Promise.race([p, new Promise((r) => setTimeout(r, ms))]);

/**
 * Let go of the port so something else can open it.
 *
 * esptool-js holds a reader AND a writer on the port's streams. While those
 * locks are held, port.close() rejects and the port stays open - so anything
 * that then tries to open it fails with "already open" and, if that error is
 * swallowed, looks exactly like a board that said nothing. Only
 * transport.disconnect() releases the locks properly.
 */
export async function detachTransport(session) {
  if (!session) return;
  try {
    await bounded(session.transport.disconnect(), 3000);
  } catch {
    /* nothing further to try; the watcher reports what it finds */
  }
  // A moment for the OS to actually free the handle - reopening the instant a
  // close returns fails on Windows often enough to be worth the wait.
  await new Promise((r) => setTimeout(r, 250));
}

export async function release(session) {
  if (!session || session.finished) return;
  try {
    await bounded(session.transport.disconnect(), 3000);
  } catch {
    /* already gone */
  }
  try {
    await bounded(session.port.close(), 2000);
  } catch {
    /* ditto */
  }
}
