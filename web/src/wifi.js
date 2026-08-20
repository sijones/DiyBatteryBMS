/* Parsing what the board's `scan` command prints.
 *
 * The firmware writes one line per network:
 *
 *   [scan]  -41  lock  Jameater5G
 *   [scan]  -94  open  EE WiFi
 *   [scan]  -71  lock  (hidden)
 *   [scan]             hex e38393e38383e38388   <- use 'ssidhex' for this one
 *
 * Signal and lock come first because the NAME IS LAST ON PURPOSE: it is the
 * only field that can contain spaces, and one turned up on the first real scan
 * ("DIRECT-2C-HP ENVY 5540 series"). So the parser splits two fields off the
 * front and takes the whole rest of the line - the same rule the firmware's own
 * `ssid` command uses for its argument.
 */

const ROW = /^\[scan\]\s+(-?\d+)\s+(lock|open)\s+(.*)$/;
const HEX = /^\[scan\]\s+hex\s+([0-9a-fA-F]+)/;
const COUNT = /^\[scan\]\s+(\d+)\s+network/;

/**
 * @param {string} text  everything the board printed after `scan`
 * @returns {{networks: Array, count: number|null}}
 */
export function parseScan(text) {
  const networks = [];
  let count = null;

  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trimEnd();

    const c = COUNT.exec(line);
    if (c) {
      count = Number(c[1]);
      continue;
    }

    // A hex line belongs to the network printed immediately above it
    const h = HEX.exec(line);
    if (h && networks.length) {
      networks[networks.length - 1].ssidHex = h[1].toLowerCase();
      networks[networks.length - 1].plainText = false;
      continue;
    }

    const m = ROW.exec(line);
    if (!m) continue;

    const name = m[3];
    networks.push({
      rssi: Number(m[1]),
      open: m[2] === "open",
      name: name === "(hidden)" ? "" : name,
      hidden: name === "(hidden)",
      ssidHex: null,
      plainText: true,
    });
  }

  return { networks, count };
}

/**
 * Collapse the duplicates a real scan is full of.
 *
 * The first scan on real hardware returned fifteen rows for nine networks:
 * jameater, Jameater5G, J-iot and Mancity each appeared twice, because a mesh
 * or a dual-band router broadcasts the same name from more than one radio. A
 * picker that listed them raw would ask someone to choose between two
 * identical-looking rows, where the choice means nothing - the board joins by
 * name and picks a radio itself.
 *
 * So: one row per name, strongest signal kept, and a count of how many radios
 * carried it. Deduping belongs here rather than in the firmware, which should
 * report what the radio actually saw.
 *
 * Hidden networks are dropped - they have no name to choose, and the UI offers
 * typing one in instead. Names are compared exactly: "jameater" and "Jameater5G"
 * are different networks, and case matters to an SSID even when it looks like a
 * typo.
 */
export function dedupe(networks) {
  const byName = new Map();

  for (const n of networks) {
    if (n.hidden) continue;

    const existing = byName.get(n.name);
    if (!existing) {
      byName.set(n.name, { ...n, radios: 1 });
      continue;
    }

    existing.radios += 1;
    if (n.rssi > existing.rssi) {
      // Keep the strongest, but carry the running count across
      const radios = existing.radios;
      byName.set(n.name, { ...n, radios });
    }
    // An open row and a locked row under one name should read as locked:
    // the stricter answer is the safe one to show.
    if (!n.open) byName.get(n.name).open = false;
  }

  return [...byName.values()].sort((a, b) => b.rssi - a.rssi);
}

/** Four bars, mapped from dBm the way every WiFi indicator does. */
export function signalBars(rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  return 1;
}

/**
 * The lines to send for a given choice.
 *
 * A network whose name is not valid UTF-8 goes as `ssidhex`, because that is
 * the only way to reproduce the exact bytes - and those are precisely the names
 * nobody can retype.
 */
export function credentialLines({ network, name, passphrase, hostname }) {
  const lines = [];
  if (network && !network.plainText && network.ssidHex) lines.push(`ssidhex ${network.ssidHex}`);
  else lines.push(`ssid ${network ? network.name : name}`);

  if (passphrase) lines.push(`pass ${passphrase}`);
  if (hostname) lines.push(`host ${hostname}`);
  lines.push("connect");
  return lines;
}
