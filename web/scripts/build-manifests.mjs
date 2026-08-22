/* Turn PlatformIO build output into something the flasher can serve.
 *
 * Copies the binaries for whatever has been built into
 * ../site/firmware/<version>/<env>/, writes a manifest beside each, then
 * rebuilds the index by SCANNING what is on disk - so previously published
 * versions survive. Rolling back is the point: the day a new release misbehaves
 * is the day someone needs the one before it, and a flasher that only offers
 * the newest build is no help at all then.
 *
 * Two things here are easy to get wrong and impossible to notice afterwards:
 *
 *   - The merged image starts at a DIFFERENT offset per chip: 0x1000 on the
 *     original ESP32, 0x0 on the S3 and C3, because that is where each chip's
 *     bootloader lives. A manifest with the wrong offset flashes cleanly and
 *     produces a board that never boots.
 *
 *   - nvs is read out of the env's own partition CSV rather than assumed. It is
 *     what the flasher compares against the board's live table to decide whether
 *     an upgrade keeps the user's settings, so a guess here would defeat the
 *     whole check.
 */

import { readFileSync, writeFileSync, mkdirSync, copyFileSync, existsSync, readdirSync, statSync, rmSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const BUILD = join(ROOT, ".pio", "build");
const OUT = join(ROOT, "site", "firmware");

/** Bootloader offset, which is where a merged factory image must be written. */
const MERGED_OFFSET = { esp32: 0x1000, esp32s3: 0x0, esp32c3: 0x0, esp32s2: 0x0 };

/** What esptool-js reports as chip.CHIP_NAME, per board family. */
const CHIP_FAMILY = { esp32: "ESP32", esp32s3: "ESP32-S3", esp32c3: "ESP32-C3", esp32s2: "ESP32-S2" };

/* Which physical WIRING each env is for, in words a person recognises. The env
   name alone ("esp32-ESPCAN") tells a user nothing about whether it is theirs.
 *
 * Keyed by the family, not the env: an env name is a family plus a flash size
 * plus whether the module has PSRAM, and those last two are read off the build
 * rather than written down twice. Thirty envs sharing seven wirings would
 * otherwise be thirty lines here, drifting apart one careless edit at a time. */
const BOARDS = {
  "esp32dev": { label: "ESP32 with MCP2515", detail: "CAN over SPI · CS 2, INT 22" },
  "esp32plus": { label: "ESP32 'plus' board", detail: "CAN over SPI · CS 5, INT 13" },
  "esp32-ESPCAN": { label: "LilyGo T-CAN485", detail: "Built-in CAN · TX 27, RX 26, EN 23" },
  "esp32s3-ESPCAN": { label: "ESP32-S3 DevKit, built-in CAN", detail: "TWAI · TX 27, RX 26, EN 23" },
  "esp32s3-MCP": { label: "ESP32-S3 with MCP2515", detail: "CAN over SPI" },
  "xiao-esp32s3": { label: "XIAO ESP32-S3 + CAN expansion", detail: "MCP2515 over SPI · CS 44" },
  "esp32c3-ESPCAN": { label: "ESP32-C3 with built-in CAN", detail: "TWAI · TX 6, RX 7" },
};

/* Read platformio.ini the way PlatformIO reads it, which is more than reading
   the lines under [env:name].
 *
 * Three things have to be handled or an env comes back missing the very fields
 * this script exists to publish:
 *
 *   - `extends` pulls options in from other sections, and an env may extend
 *     several. Nearly every env in this project now gets its board and its
 *     partition table that way and defines neither itself.
 *   - a value continues onto following indented lines, which is how build_flags
 *     and lib_deps are written.
 *   - ${section.option} interpolates, which is how a family adds a flag to its
 *     base without repeating it.
 *
 * A parser that skipped any of those would not fail - it would quietly report
 * `undefined` for the partition table, and the manifest would carry a wrong
 * minimum flash size or no nvs offset. That is the failure this file's header
 * warns about: flashes cleanly, produces a board that never boots. */
function parseIni(text) {
  const sections = {};
  let current = null;
  let key = null;

  for (const raw of text.split(/\r?\n/)) {
    if (!raw.trim() || /^\s*[;#]/.test(raw)) continue;

    const sec = /^\[(.+)\]\s*$/.exec(raw.trim());
    if (sec) {
      current = sec[1];
      sections[current] = sections[current] ?? {};
      key = null;
      continue;
    }
    if (!current) continue;

    const kv = /^([\w.]+)\s*=\s*(.*)$/.exec(raw.trim());
    if (kv && !/^\s/.test(raw)) {
      key = kv[1];
      sections[current][key] = kv[2].trim();
      continue;
    }
    // An indented line with no key of its own continues the previous value.
    if (key) sections[current][key] += " " + raw.trim();
  }

  /* Resolve extends depth-first. Later sections in the list win, and the env's
     own options win over all of them - the same order PlatformIO applies. */
  const resolved = {};
  const resolve = (name, seen = new Set()) => {
    if (resolved[name]) return resolved[name];
    if (seen.has(name)) throw new Error(`platformio.ini: extends loops through [${name}]`);
    seen.add(name);

    const own = sections[name] ?? {};
    let out = {};
    for (const parent of (own.extends ?? "").split(",").map((s) => s.trim()).filter(Boolean)) {
      out = { ...out, ...resolve(parent, seen) };
    }
    out = { ...out, ...own };
    delete out.extends;
    resolved[name] = out;
    return out;
  };

  const interpolate = (value, depth = 0) =>
    depth > 8
      ? value
      : value.replace(/\$\{([^.}]+)\.([^}]+)\}/g, (whole, sec, opt) => {
          const from = sections[sec] ? resolve(sec) : null;
          return from?.[opt] !== undefined ? interpolate(from[opt], depth + 1) : whole;
        });

  const envs = {};
  for (const name of Object.keys(sections)) {
    if (!name.startsWith("env:")) continue;
    const opts = resolve(name);
    envs[name.slice(4)] = Object.fromEntries(
      Object.entries(opts).map(([k, v]) => [k, interpolate(v)]),
    );
  }
  return envs;
}

/** nvs offset and size, straight out of the env's partition CSV. */
function readNvs(csvName) {
  const path = join(ROOT, csvName);
  if (!existsSync(path)) return null;
  for (const raw of readFileSync(path, "utf8").split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith("#")) continue;
    const [name, type, subtype, offset, size] = line.split(",").map((s) => s.trim());
    if (name === "nvs" && type === "data" && subtype === "nvs") {
      return { offset: Number(offset), size: Number(size) };
    }
  }
  return null;
}

function fwVersion() {
  const cfg = readFileSync(join(ROOT, "src", "config.h"), "utf8");
  const m = /#define\s+FW_VERSION\s+"([^"]+)"/.exec(cfg);
  return m ? m[1] : "0.0.0";
}

function chipOf(env, ini) {
  const board = ini[env]?.board ?? "";
  if (board.includes("s3")) return "esp32s3";
  if (board.includes("c3")) return "esp32c3";
  if (board.includes("s2")) return "esp32s2";
  return "esp32";
}

/**
 * How much flash the build is laid out for, in bytes.
 *
 * board_upload.flash_size is the authority because it is what the image is
 * actually built for. The partition table name is the fallback for an env that
 * has not been given one, and it is only a naming convention - which is exactly
 * why it is second and not first.
 */
function flashBytesOf(opts) {
  const declared = /^(\d+)MB$/i.exec(opts["board_upload.flash_size"] ?? "");
  if (declared) return Number(declared[1]) * 1024 * 1024;
  const csv = opts["board_build.partitions"] ?? "";
  if (csv.includes("16mb")) return 16 * 1024 * 1024;
  if (csv.includes("8mb")) return 8 * 1024 * 1024;
  return 4 * 1024 * 1024;
}

/**
 * What the board is, for someone reading a list of builds.
 *
 * The wiring comes from BOARDS, keyed by the env name with its -8mb / -16mb /
 * -psram suffixes taken off. Everything else is read off the resolved build:
 * PSRAM from the flag that actually enables it rather than from the name, so an
 * env that says psram and does not set the flag is described as what it builds,
 * not as what it is called; and octal vs quad from memory_type, because the two
 * are different parts and the wrong one silently yields no PSRAM at all.
 */
function describe(env, opts) {
  const family = /^(.*?)(?:-\d+mb)?(?:-psram)?$/i.exec(env)[1];
  const base = BOARDS[family] ?? BOARDS[env] ?? { label: env, detail: "" };

  const psram = /-DBOARD_HAS_PSRAM\b/.test(opts.build_flags ?? "");
  const octal = (opts["board_build.arduino.memory_type"] ?? "").includes("opi");
  const mb = Math.round(flashBytesOf(opts) / 1024 / 1024);

  const detail = [base.detail, `${mb}MB flash`, psram ? `${octal ? "octal" : "quad"} PSRAM` : null]
    .filter(Boolean)
    .join(" · ");

  return { label: base.label, detail, psram };
}

/**
 * Newest first. Numeric parts compare numerically so 3.0.10 beats 3.0.9, and a
 * plain release beats its own pre-release: 3.0.0 is newer than 3.0.0-DEV, the
 * way semver orders them.
 */
function compareVersions(a, b) {
  const split = (v) => {
    const [core, pre] = v.split("-", 2);
    return { nums: core.split(".").map((n) => parseInt(n, 10) || 0), pre: pre ?? null };
  };
  const A = split(a), B = split(b);
  for (let i = 0; i < Math.max(A.nums.length, B.nums.length); i++) {
    const d = (B.nums[i] ?? 0) - (A.nums[i] ?? 0);
    if (d) return d;
  }
  if (A.pre === B.pre) return 0;
  if (!A.pre) return -1; // released sorts above its pre-release
  if (!B.pre) return 1;
  /* Descending, so BETA2 sorts ahead of BETA1.
   *
   * This read `A.pre < B.pre` and therefore called the OLDEST pre-release the
   * newest. Published 3.0.0-BETA2 alongside BETA1 and the site announced BETA1
   * as the latest beta - but the label was the mild half. This same comparator
   * orders the list that KEEP_PER_CHANNEL slices, so a third beta would have
   * pruned the newest build off the site and kept the two oldest.
   *
   * A plain release still beats its own pre-release, which is the case above. */
  return A.pre > B.pre ? -1 : 1;
}

const ini = parseIni(readFileSync(join(ROOT, "platformio.ini"), "utf8"));
const version = fwVersion();
let written = 0;

/* ---- 1. publish whatever has just been built ---------------------------- */

for (const env of Object.keys(ini)) {
  const factory = join(BUILD, env, "firmware.factory.bin");
  if (!existsSync(factory)) continue; // not built - skip rather than fail

  const chip = chipOf(env, ini);
  const csv = ini[env]["board_build.partitions"];
  const nvs = csv ? readNvs(csv) : null;

  if (!nvs) {
    console.warn(`! ${env}: no nvs found in ${csv ?? "(no partition table set)"} - upgrades cannot be checked`);
  }

  const destDir = join(OUT, version, env);
  mkdirSync(destDir, { recursive: true });
  copyFileSync(factory, join(destDir, "firmware.factory.bin"));

  const size = readFileSync(factory).length;
  const board = describe(env, ini[env]);

  /* The app image as well as the merged one. They are for different jobs and
     are NOT interchangeable:
       - firmware.factory.bin is bootloader + partition table + app, written
         over serial from offset 0x1000 (ESP32) or 0x0 (S3/C3). That is what
         this flasher writes.
       - firmware.bin is the app alone, which is what an over-the-air update
         writes into the spare OTA slot. Handing OTA the merged image would
         write a bootloader into an app partition.
     Published now so a future device-pull OTA has something to fetch; nothing
     in the firmware uses it yet. */
  const app = join(BUILD, env, "firmware.bin");
  let appPart = null;
  if (existsSync(app)) {
    copyFileSync(app, join(destDir, "firmware.bin"));
    appPart = { path: "firmware.bin", size: readFileSync(app).length };
  }

  writeFileSync(
    join(destDir, "manifest.json"),
    JSON.stringify(
      {
        name: "DIY Battery BMS",
        version,
        env,
        board: board.label,
        detail: board.detail,
        chipFamily: CHIP_FAMILY[chip],
        // Minimum flash the partition table needs. A 16MB table on a 4MB chip
        // is the fastest way to brick a board, so the flasher greys those out.
        minFlashBytes: flashBytesOf(ini[env]),
        // Read from the flag that turns PSRAM on, not from the env name. The
        // name is a label someone types; the flag is what the image was built
        // with, and it is the flag the board has to live up to.
        psramRequired: board.psram,
        nvs,
        published: new Date().toISOString(),
        // What the serial flasher writes
        parts: [{ path: "firmware.factory.bin", offset: MERGED_OFFSET[chip], size }],
        // What an over-the-air update would fetch instead. No offset: OTA picks
        // the inactive slot itself, and hard-coding one would write over the
        // running firmware.
        ota: appPart,
      },
      null,
      2,
    ) + "\n",
  );

  console.log(`  + ${version}  ${env.padEnd(24)} ${CHIP_FAMILY[chip].padEnd(9)} ${(size / 1024 / 1024).toFixed(2)} MB`);
  written++;
}

/* ---- 2. rebuild the index from everything on disk ----------------------- */

mkdirSync(OUT, { recursive: true });

const versions = [];
for (const vdir of readdirSync(OUT)) {
  const vpath = join(OUT, vdir);
  if (!statSync(vpath).isDirectory()) continue;

  const builds = [];
  for (const edir of readdirSync(vpath)) {
    const mpath = join(vpath, edir, "manifest.json");
    if (!existsSync(mpath)) continue;
    try {
      const m = JSON.parse(readFileSync(mpath, "utf8"));
      builds.push({
        env: m.env,
        board: m.board,
        chipFamily: m.chipFamily,
        path: `${vdir}/${edir}/manifest.json`,
      });
    } catch {
      console.warn(`! ${vdir}/${edir}/manifest.json is not readable JSON - skipped`);
    }
  }

  if (builds.length) {
    builds.sort((a, b) => a.board.localeCompare(b.board));
    versions.push({ version: vdir, builds });
  }
}

if (!versions.length) {
  console.error("Nothing to publish: no built envs under .pio/build and nothing already in site/firmware.");
  process.exit(1);
}

/* ---- 3. split into channels and prune ----------------------------------- */

/* Anything carrying a pre-release suffix goes to beta: -DEV, -beta, -rc, -alpha
   and whatever else turns up. A bare 3.0.1 is a release.
 *
 * Deliberately a denylist of nothing rather than an allowlist of known
 * suffixes: a version tagged 3.1.0-testing must not silently be published as a
 * release because the suffix was not one this script had heard of. Erring
 * towards beta is the safe direction - the worst case is a stable build sitting
 * in the beta channel, which someone notices; the reverse ships an untested
 * build to people who asked for the one that works.
 *
 * Two channels rather than one list, because the people who want the newest
 * thing and the people who want the thing that works are not the same people,
 * and mixing them makes the second group roll back by accident. */
const channelOf = (v) => (/-/.test(v) ? "beta" : "release");

const KEEP_PER_CHANNEL = 2;

const channels = { release: [], beta: [] };
for (const v of versions) channels[channelOf(v.version)].push(v);
for (const key of Object.keys(channels)) channels[key].sort((a, b) => compareVersions(a.version, b.version));

/* Anything past the newest two in a channel is deleted from disk.
 *
 * This is not recoverable: the binaries cannot be rebuilt once the source has
 * moved on, so what goes here is gone. That is the deliberate trade for not
 * growing the deploy without limit - and two deep is enough for the case this
 * exists to serve, which is "the new one is broken, put yesterday's back". */
for (const key of Object.keys(channels)) {
  const drop = channels[key].slice(KEEP_PER_CHANNEL);
  channels[key] = channels[key].slice(0, KEEP_PER_CHANNEL);
  for (const v of drop) {
    rmSync(join(OUT, v.version), { recursive: true, force: true });
    console.log(`  - pruned ${key} ${v.version} (keeping newest ${KEEP_PER_CHANNEL})`);
  }
}

writeFileSync(
  join(OUT, "releases.json"),
  JSON.stringify(
    {
      latest: {
        release: channels.release[0]?.version ?? null,
        beta: channels.beta[0]?.version ?? null,
      },
      channels,
    },
    null,
    2,
  ) + "\n",
);

const summary = (key) =>
  channels[key].length ? channels[key].map((v) => `${v.version} (${v.builds.length})`).join(", ") : "none";

console.log(
  `\n${written} build(s) published for ${version}.\n` +
    `  release: ${summary("release")}\n` +
    `  beta:    ${summary("beta")}`,
);
