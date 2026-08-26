/* Reading the partition table off a live board.
 *
 * This is what turns "flash it and hope" into a decision. The table says
 * whether there is already a DiyBatteryBMS install here, how the flash is laid
 * out, and - the part that matters - exactly where nvs sits. Settings survive a
 * re-flash only because every partition table in this project keeps nvs at the
 * same offset and size; a build that moves it wipes everything the user
 * configured, and the flash looks completely normal while doing it.
 *
 * Upgrade-DiyBatteryBMS.ps1 already refuses that case. This is the same check,
 * in the browser.
 */

// Where the table lives, and how much of it the ROM reserves.
export const PARTITION_TABLE_OFFSET = 0x8000;
export const PARTITION_TABLE_SIZE = 0xc00; // 3KB, 96 entries of 32 bytes

const ENTRY_MAGIC = 0x50aa; // little-endian 0xAA 0x50
const ENTRY_SIZE = 32;

const TYPE_APP = 0x00;
const TYPE_DATA = 0x01;

// Only the subtypes this project actually uses are named. Anything else is
// reported by number rather than guessed at.
const APP_SUBTYPES = { 0x00: "factory", 0x10: "ota_0", 0x11: "ota_1" };
const DATA_SUBTYPES = { 0x00: "otadata", 0x01: "phy", 0x02: "nvs", 0x03: "coredump", 0x04: "nvs_keys" };

/**
 * Parse the bytes read from PARTITION_TABLE_OFFSET.
 *
 * Stops at the first entry without the magic, which is how the ROM itself
 * decides the table has ended - a table is not required to fill its 3KB, and
 * every table in this project uses five of the ninety-six slots.
 *
 * @param {Uint8Array} bytes
 * @returns {{partitions: Array, valid: boolean}}
 */
export function parsePartitionTable(bytes) {
  const partitions = [];
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  for (let off = 0; off + ENTRY_SIZE <= bytes.length; off += ENTRY_SIZE) {
    if (view.getUint16(off, true) !== ENTRY_MAGIC) break;

    const type = view.getUint8(off + 2);
    const subtype = view.getUint8(off + 3);
    const offset = view.getUint32(off + 4, true);
    const size = view.getUint32(off + 8, true);

    // Label is a fixed 16 bytes, NUL-padded rather than NUL-terminated
    let label = "";
    for (let i = 0; i < 16; i++) {
      const c = view.getUint8(off + 12 + i);
      if (c === 0) break;
      label += String.fromCharCode(c);
    }

    partitions.push({
      label,
      type,
      subtype,
      offset,
      size,
      typeName: type === TYPE_APP ? "app" : type === TYPE_DATA ? "data" : `0x${type.toString(16)}`,
      subtypeName:
        type === TYPE_APP
          ? APP_SUBTYPES[subtype] ?? `0x${subtype.toString(16)}`
          : type === TYPE_DATA
            ? DATA_SUBTYPES[subtype] ?? `0x${subtype.toString(16)}`
            : `0x${subtype.toString(16)}`,
    });
  }

  return { partitions, valid: partitions.length > 0 };
}

/** The nvs partition, or null. This is the one that decides whether settings live. */
export function findNvs(partitions) {
  return partitions.find((p) => p.type === TYPE_DATA && p.subtype === 0x02) ?? null;
}

/** The coredump partition, or null - where a crash is recorded. */
export function findCoredump(partitions) {
  return partitions.find((p) => p.type === TYPE_DATA && p.subtype === 0x03) ?? null;
}

/**
 * Does this look like one of ours?
 *
 * Deliberately loose. The point is not to prove authorship - it is to know
 * whether the layout is one whose nvs we can preserve, so a table with the
 * app slots and an nvs where we expect it qualifies regardless of who wrote it.
 */
export function looksLikeOurLayout(partitions) {
  const nvs = findNvs(partitions);
  const apps = partitions.filter((p) => p.type === TYPE_APP);
  return Boolean(nvs) && apps.length > 0;
}

/**
 * Compare the board's nvs against the one a build expects.
 *
 * Returns why an upgrade is or is not safe. `move` is the case that silently
 * destroys a configuration: the flash succeeds, the board boots, and every
 * setting is gone because the new table looks for nvs somewhere else.
 *
 * Matching offsets are necessary but not sufficient. The flasher writes one
 * contiguous merged image, and esptool erases and reprograms the full span of
 * any file it writes as a normal part of writing it, regardless of eraseAll -
 * so unless flash.js is told to slice nvs back out of that span before
 * writing, an "upgrade" that only checked offsets would still silently wipe
 * every setting while telling the user otherwise. targetGap is what proves a
 * safe write is actually going to happen, not just that it theoretically
 * could: see readGap() in build-manifests.mjs for what it covers and why.
 *
 * @param {object|null} boardNvs    from the board's own table
 * @param {object|null} targetNvs   from the build being installed
 * @param {object|null} targetGap   the range flash.js will skip, from the build's manifest
 */
export function nvsVerdict(boardNvs, targetNvs, targetGap) {
  if (!boardNvs) {
    return { safe: false, kind: "fresh", detail: "No existing settings partition - this is a first install." };
  }
  if (!targetNvs) {
    return { safe: false, kind: "unknown", detail: "The chosen build does not declare where nvs lives." };
  }
  if (boardNvs.offset !== targetNvs.offset) {
    return {
      safe: false,
      kind: "moved",
      detail:
        `Settings are at 0x${boardNvs.offset.toString(16)} on this board but the build puts them ` +
        `at 0x${targetNvs.offset.toString(16)}. Flashing would leave every setting unreachable.`,
    };
  }
  if (boardNvs.size !== targetNvs.size) {
    return {
      safe: false,
      kind: "resized",
      detail:
        `Settings occupy ${boardNvs.size} bytes on this board and ${targetNvs.size} in the build. ` +
        `Resizing nvs discards its contents.`,
    };
  }
  const nvsStart = targetNvs.offset;
  const nvsEnd = targetNvs.offset + targetNvs.size;
  const gapCoversNvs = targetGap && targetGap.offset <= nvsStart && targetGap.offset + targetGap.size >= nvsEnd;
  if (!gapCoversNvs) {
    return {
      safe: false,
      kind: "unprotected",
      detail:
        "This build does not declare a safe way to write around nvs, so a write cannot be guaranteed to " +
        "leave it alone. Back up your settings and choose to erase.",
    };
  }
  return { safe: true, kind: "match", detail: `Settings stay at 0x${boardNvs.offset.toString(16)}, untouched.` };
}

export const fmtHex = (n) => "0x" + n.toString(16).toUpperCase().padStart(6, "0");

export function fmtSize(bytes) {
  if (bytes >= 1024 * 1024) {
    const mb = bytes / (1024 * 1024);
    return `${Number.isInteger(mb) ? mb : mb.toFixed(2)} MB`;
  }
  return `${Math.round(bytes / 1024)} KB`;
}
