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
 * Slice one file's bytes around the nvs/otadata range described by `gap`, so
 * writing it can never touch that range. See readGap() in
 * web/scripts/build-manifests.mjs for exactly what gap covers and why this
 * exists at all - short version: esptool erases and reprograms the FULL span
 * of any file it writes as a normal part of writing it, "eraseAll" or not, so
 * a merged image that runs from the bootloader through the app has always
 * taken nvs with it on every "upgrade" that told the user otherwise. This is
 * the fix: never send bytes for the range nvs lives in, so there is nothing
 * for esptool to erase there in the first place.
 *
 * Returns one or two {bytes, address} chunks. No gap, or a part that does not
 * overlap it (an OTA-only app image, say), comes back unchanged as one chunk.
 */
function splitAroundGap(bytes, address, gap) {
  if (!gap) return [{ bytes, address }];

  const fileStart = address;
  const fileEnd = address + bytes.length;
  const gapStart = gap.offset;
  const gapEnd = gap.offset + gap.size;
  if (gapEnd <= fileStart || gapStart >= fileEnd) return [{ bytes, address }];

  const chunks = [];
  if (gapStart > fileStart) chunks.push({ bytes: bytes.subarray(0, gapStart - fileStart), address: fileStart });
  if (gapEnd < fileEnd) chunks.push({ bytes: bytes.subarray(gapEnd - fileStart), address: gapEnd });
  return chunks;
}

/**
 * Write a build to the board.
 *
 * flashSize/flashMode/flashFreq are all "keep": a merged factory image already
 * carries the right values in its bootloader header, and overriding them with
 * anything else is how a board ends up flashed correctly and unable to boot.
 *
 * Every part is sliced around manifest.gap regardless of eraseAll. On an
 * upgrade that is what keeps nvs untouched - eraseAll was never what did
 * that, whatever the option's name suggests, since esptool erases the span of
 * whatever it is told to write either way. On a full erase the chip is
 * already blank under that range, so skipping it changes nothing except not
 * transferring bytes nobody needed sent.
 *
 * @param {(written:number, total:number)=>void} onProgress
 */
export async function flashBuild(session, manifest, { eraseAll = false, onProgress } = {}) {
  const chunks = [];

  for (const part of manifest.parts) {
    const res = await fetch(`${manifest._dir}/${part.path}`, { cache: "no-cache" });
    if (!res.ok) throw new Error(`Could not download ${part.path} (HTTP ${res.status})`);
    const bytes = new Uint8Array(await res.arrayBuffer());

    // The manifest records the size the build produced; a mismatch means the
    // file on the server is not the file the manifest describes.
    if (part.size && bytes.length !== part.size) {
      throw new Error(`${part.path} is ${bytes.length} bytes, manifest says ${part.size} - refusing to write it`);
    }

    chunks.push(...splitAroundGap(bytes, part.offset, manifest.gap));
  }

  const totalBytes = chunks.reduce((sum, c) => sum + c.bytes.length, 0);

  /* esptool-js reports progress per FILE in fileArray - written/total both
     reset at each file boundary, and both are in COMPRESSED bytes (compress:
     true below), not the uncompressed lengths totalBytes is counted in. That
     makes written/total a valid 0..1 completion FRACTION for the current
     file regardless of compression, but not a byte count safe to add
     directly to a total measured in uncompressed bytes. Scale the fraction by
     the current chunk's real (uncompressed) length instead of adding
     written itself, and only fold a chunk's full length into completedBytes
     once its file index has actually moved on - that keeps the bar exact on
     the single-chunk case (identical to the old behaviour) and monotonic
     once the gap split makes it two. */
  let completedBytes = 0;
  let lastFileIndex = -1;

  await session.loader.writeFlash({
    fileArray: chunks.map((c) => ({ data: toBinaryString(c.bytes), address: c.address })),
    flashSize: "keep",
    flashMode: "keep",
    flashFreq: "keep",
    eraseAll,
    compress: true,
    reportProgress: (fileIndex, written, total) => {
      if (fileIndex !== lastFileIndex) {
        if (lastFileIndex >= 0) completedBytes += chunks[lastFileIndex].bytes.length;
        lastFileIndex = fileIndex;
      }
      const fraction = total ? written / total : 0;
      onProgress?.(completedBytes + fraction * chunks[fileIndex].bytes.length, totalBytes);
    },
  });
}

/**
 * Write a previously-downloaded nvs backup straight back onto the board.
 *
 * Unlike flashBuild this writes exactly the nvs partition and nothing either
 * side of it - there is no merged image involved, so no gap to slice around.
 * The length must match the board's live nvs partition exactly: a backup
 * taken from a different flash size, or from before a partition table change,
 * could be the wrong length, and writing a mismatched length would spill into
 * whatever sits on either side of nvs rather than stopping short of it.
 *
 * @param {(written:number, total:number)=>void} onProgress
 */
export async function restoreNvs(session, bytes, { onProgress } = {}) {
  if (!session.nvs) throw new Error("This board has no nvs partition to restore into.");
  if (bytes.length !== session.nvs.size) {
    throw new Error(
      `This backup is ${bytes.length} bytes, but this board's nvs partition is ${session.nvs.size} bytes - refusing to write it.`,
    );
  }

  await session.loader.writeFlash({
    fileArray: [{ data: toBinaryString(bytes), address: session.nvs.offset }],
    flashSize: "keep",
    flashMode: "keep",
    flashFreq: "keep",
    eraseAll: false,
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
