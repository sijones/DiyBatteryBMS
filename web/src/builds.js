/* Which builds can go on the board that is plugged in.
 *
 * Detection narrows the list; the person picks from what is left. The chip
 * cannot tell us which board it is soldered to - an MCP2515 on SPI and a
 * built-in CAN controller are indistinguishable from the ROM - so the last step
 * is always a human choice. What detection CAN do is rule out the choices that
 * would brick the board, which is most of the value: a 16MB partition table on
 * a 4MB chip, or a PSRAM build on a module without any.
 *
 * Incompatible builds are listed WITH THE REASON rather than hidden. "Why isn't
 * my board there" is a worse failure than a greyed-out row that explains itself.
 */

export async function loadReleases(base = "./firmware") {
  const res = await fetch(`${base}/releases.json`, { cache: "no-cache" });
  if (!res.ok) throw new Error(`No firmware index (HTTP ${res.status})`);
  return res.json();
}

/**
 * Which channel and version to land on.
 *
 * Release wins when there is one, because someone arriving without an opinion
 * should get the firmware that is meant to work, not the newest thing that
 * exists. Beta is only the default when there is no release to offer at all.
 */
export function defaultSelection(index) {
  if (index.latest?.release) return { channel: "release", version: index.latest.release };
  if (index.latest?.beta) return { channel: "beta", version: index.latest.beta };
  return { channel: "release", version: null };
}

export function versionsIn(index, channel) {
  return index.channels?.[channel] ?? [];
}

export function buildsFor(index, channel, version) {
  return versionsIn(index, channel).find((v) => v.version === version)?.builds ?? [];
}

export async function loadManifest(base, path) {
  const res = await fetch(`${base}/${path}`, { cache: "no-cache" });
  if (!res.ok) throw new Error(`Cannot read ${path} (HTTP ${res.status})`);
  const m = await res.json();
  // Remember where the manifest lives so its parts resolve alongside it
  m._dir = `${base}/${path}`.replace(/\/[^/]+$/, "");
  return m;
}

const MB = (n) => `${Math.round(n / 1024 / 1024)} MB`;

/**
 * Judge one build against a connected board.
 * @returns {{ok: boolean, reason: string|null}}
 */
export function compatibility(manifest, info) {
  if (manifest.chipFamily !== info.chipName) {
    return { ok: false, reason: `Built for ${manifest.chipFamily}, this board is ${info.chipName}` };
  }
  if (manifest.minFlashBytes > info.flashSizeBytes) {
    return {
      ok: false,
      reason: `Needs ${MB(manifest.minFlashBytes)} of flash, this board has ${MB(info.flashSizeBytes)}`,
    };
  }
  if (manifest.psramRequired && !info.psram.present) {
    return { ok: false, reason: "Needs PSRAM, none was found on this module" };
  }
  return { ok: true, reason: null };
}

/**
 * Sort so the usable options come first, then by name.
 *
 * Deliberately not hiding the rest, and deliberately not auto-selecting the
 * single compatible one either: picking the wrong board is the mistake this
 * step exists to prevent, and an auto-selection invites clicking past it.
 */
export function rank(entries) {
  return [...entries].sort((a, b) => {
    if (a.compat.ok !== b.compat.ok) return a.compat.ok ? -1 : 1;

    const byBoard = a.manifest.board.localeCompare(b.manifest.board);
    if (byBoard) return byBoard;

    /* Same wiring, different module. Several builds fit a 16MB board with
       PSRAM - the 8MB one runs perfectly well on it - so the tie is broken
       towards the build that uses what the board actually has. Anything that
       does NOT fit has already sorted below on compat.ok, so a PSRAM build
       still in the running here is one this board can run.
       Order still decides nothing on its own: nothing is auto-selected, and
       every row carries its env name and its detail line. This only means the
       best fit is the first thing read, not the first thing found. */
    if (a.manifest.psramRequired !== b.manifest.psramRequired) return a.manifest.psramRequired ? -1 : 1;
    return (b.manifest.minFlashBytes ?? 0) - (a.manifest.minFlashBytes ?? 0);
  });
}

/**
 * One entry per wiring, which is the only thing left for a person to decide.
 *
 * Detection answers chip, flash size and PSRAM off the ROM. It cannot answer
 * how CAN is wired, because an MCP2515 on SPI and a built-in controller are
 * indistinguishable from down there - so that is the question the list should
 * ask, and it is the only one. Everything else that separates two builds of the
 * same wiring has a right answer the flasher already knows: on a 16MB module
 * with PSRAM, the 16MB PSRAM build is simply better than the 8MB one, and
 * asking a person to choose between them is asking them to guess at something
 * that was already worked out.
 *
 * Takes a ranked list, so the first entry for each wiring is the best fit for
 * this board - and, when a wiring has nothing compatible at all, the first
 * entry is the one whose reason best explains why. Those stay in the list
 * rather than vanishing: "why isn't my board here" is a worse failure than a
 * greyed-out row that says what is wrong.
 *
 * @returns {Array<{entry: object, index: number, hidden: number}>} `index` is
 *   the position in the ranked list it came from, so selection still refers to
 *   one build; `hidden` is how many others share that wiring.
 */
export function collapseToWirings(ranked) {
  const seen = new Map();
  ranked.forEach((entry, index) => {
    const key = entry.manifest.board;
    if (seen.has(key)) seen.get(key).hidden++;
    else seen.set(key, { entry, index, hidden: 0 });
  });
  return [...seen.values()];
}
