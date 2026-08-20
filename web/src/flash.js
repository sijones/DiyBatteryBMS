/* Writing to the board, and saving what is already on it first.
 *
 * The order here is the whole point: back up nvs, then write. Never the other
 * way round, and never a full erase on an upgrade. Upgrade-DiyBatteryBMS.ps1
 * arrived at those rules the hard way and this follows them.
 */

/* The reset modes are a TypeScript string-union type, not a runtime enum -
   importing `After` as a value fails to bundle. The strings below are the
   values esploader.after() switches on. */

/**
 * esptool-js wants image data as a BINARY STRING - one character per byte -
 * not a Uint8Array. Handing it typed-array data produces a flash that reports
 * success and writes rubbish, which is the worst kind of failure available
 * here, so the conversion is explicit and in one place.
 *
 * Chunked because String.fromCharCode.apply blows the argument limit somewhere
 * around a hundred thousand bytes, and these images are over a million.
 */
function toBinaryString(bytes) {
  let out = "";
  const CHUNK = 0x8000;
  for (let i = 0; i < bytes.length; i += CHUNK) {
    out += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
  }
  return out;
}

/** Hand the browser a file to save. */
function offerDownload(bytes, filename) {
  const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  // Revoke on the next tick; revoking immediately can cancel the save
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
}

/**
 * Read the settings partition off the board and save it.
 *
 * Cheap - twenty kilobytes - and it is the difference between a mistake being
 * an inconvenience and being someone's whole configuration. Always offered,
 * never skipped silently.
 */
export async function backupNvs(session, { download = true } = {}) {
  if (!session.nvs) return null;
  const bytes = await session.loader.readFlash(session.nvs.offset, session.nvs.size);
  const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-");
  const filename = `nvs-${session.info.mac.replace(/:/g, "")}-${stamp}.bin`;
  if (download) offerDownload(bytes, filename);
  return { bytes, filename };
}

/**
 * Write a build to the board.
 *
 * flashSize/flashMode/flashFreq are all "keep": a merged factory image already
 * carries the right values in its bootloader header, and overriding them with
 * anything else is how a board ends up flashed correctly and unable to boot.
 *
 * eraseAll stays FALSE for an upgrade. Erasing takes nvs with it, and the
 * caller has already established - via nvsVerdict - whether the settings can
 * survive. A first install passes true deliberately.
 *
 * @param {(written:number, total:number)=>void} onProgress
 */
export async function flashBuild(session, manifest, { eraseAll = false, onProgress } = {}) {
  const parts = [];

  for (const part of manifest.parts) {
    const res = await fetch(`${manifest._dir}/${part.path}`, { cache: "no-cache" });
    if (!res.ok) throw new Error(`Could not download ${part.path} (HTTP ${res.status})`);
    const bytes = new Uint8Array(await res.arrayBuffer());

    // The manifest records the size the build produced; a mismatch means the
    // file on the server is not the file the manifest describes.
    if (part.size && bytes.length !== part.size) {
      throw new Error(`${part.path} is ${bytes.length} bytes, manifest says ${part.size} - refusing to write it`);
    }

    parts.push({ data: toBinaryString(bytes), address: part.offset });
  }

  await session.loader.writeFlash({
    fileArray: parts,
    flashSize: "keep",
    flashMode: "keep",
    flashFreq: "keep",
    eraseAll,
    compress: true,
    reportProgress: (_fileIndex, written, total) => onProgress?.(written, total),
  });
}

/**
 * Is this port the chip's own USB, or a USB-to-serial bridge?
 *
 * It decides which reset actually works, and it CANNOT be inferred from the
 * chip family - that was a bug: an ESP32-S3 on a CH343 bridge got the USB-JTAG
 * reset sequence, which does nothing on a bridge, so the board never restarted
 * and sat in the flasher stub printing nothing.
 *
 * Espressif's native USB is vendor 0x303A. Everything else here is a bridge:
 * CH340/CH343 (0x1A86), CP210x (0x10C4), FTDI (0x0403).
 */
function isNativeUsb(port) {
  try {
    return port.getInfo?.().usbVendorId === 0x303a;
  } catch {
    return false;
  }
}

/** Restart the board so it comes up on what was just written. */
export async function restart(session) {
  await session.loader.after("hard_reset", isNativeUsb(session.port));
}
