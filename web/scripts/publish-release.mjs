/* Publish what was just built to a GitHub release.
 *
 * The flasher's own copy of the binaries is deliberately small and deliberately
 * temporary: build-manifests.mjs keeps two versions per channel and DELETES the
 * rest, and site/firmware is not in git because it is seven megabytes a
 * version. Both of those are the right call for a deploy directory and both
 * mean that, on their own, a build is gone the moment two more follow it - and
 * it cannot be rebuilt once the source has moved on.
 *
 * A release fixes that without undoing either. GitHub keeps every version
 * forever, addressably, at no cost to the repository or the deploy; pruning
 * stops being a one-way door and becomes what it should have been all along,
 * which is "the flasher offers the last two, the archive has the rest".
 *
 * Run after build-manifests.mjs and BEFORE wrangler, so that a missing token or
 * an unpushed commit stops the whole deploy rather than leaving a site
 * advertising a version with no archive behind it.
 *
 *   node scripts/publish-release.mjs [--dry-run]
 *
 * Wants GITHUB_TOKEN (or GH_TOKEN) with `contents: write` on this repository.
 */

import { readFileSync, existsSync, readdirSync, statSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const OUT = join(ROOT, "site", "firmware");

const DRY = process.argv.includes("--dry-run");
const TOKEN = process.env.GITHUB_TOKEN || process.env.GH_TOKEN;

const API = "https://api.github.com";
const UPLOADS = "https://uploads.github.com";

/* The same read build-manifests.mjs does. Duplicated rather than imported
   because that file publishes on import - it is a script, not a module - and
   four lines of regex is a cheaper price than restructuring it. */
function fwVersion() {
  const cfg = readFileSync(join(ROOT, "src", "config.h"), "utf8");
  const m = /#define\s+FW_VERSION\s+"([^"]+)"/.exec(cfg);
  if (!m) throw new Error("No FW_VERSION in src/config.h - nothing to name a release after");
  return m[1];
}

function git(...args) {
  return execFileSync("git", args, { cwd: ROOT, encoding: "utf8" }).trim();
}

/** owner/repo out of the origin URL, HTTPS or SSH, with or without the .git. */
function repoSlug() {
  if (process.env.GITHUB_REPOSITORY) return process.env.GITHUB_REPOSITORY;
  const url = git("remote", "get-url", "origin");
  const m = /github\.com[/:]([^/]+\/[^/]+?)(?:\.git)?$/.exec(url);
  if (!m) throw new Error(`origin is not a GitHub remote: ${url}`);
  return m[1];
}

async function gh(method, url, { body, headers, raw } = {}) {
  const res = await fetch(url, {
    method,
    headers: {
      accept: "application/vnd.github+json",
      authorization: `Bearer ${TOKEN}`,
      "x-github-api-version": "2022-11-28",
      "user-agent": "diy-battery-bms-release",
      ...(body && !raw ? { "content-type": "application/json" } : {}),
      ...headers,
    },
    body: raw ? body : body ? JSON.stringify(body) : undefined,
  });
  // Only a GET is allowed to answer "not there" - it is how the release and its
  // assets are looked up. A 404 from an upload or a delete means the release id
  // is wrong, and swallowing that would report a successful publish that put
  // nothing anywhere.
  if (res.status === 404 && method === "GET") return null;
  if (!res.ok) {
    const text = await res.text();
    throw new Error(`${method} ${url} -> ${res.status}\n${text}`);
  }
  return res.status === 204 ? true : res.json();
}

/* Same rule as build-manifests.mjs channelOf(): any suffix at all means
   pre-release. Kept identical on purpose - a version that shows up in the
   flasher's beta channel must not be a full release on GitHub, or the two
   places disagree about what "3.1.0-testing" is. */
const isPrerelease = (v) => /-/.test(v);

const CONTENT_TYPE = {
  ".bin": "application/octet-stream",
  ".json": "application/json",
};

function collect(version) {
  const vdir = join(OUT, version);
  if (!existsSync(vdir)) {
    throw new Error(`No ${vdir} - run build-manifests.mjs first, and build something before that`);
  }

  const builds = [];
  for (const env of readdirSync(vdir)) {
    const edir = join(vdir, env);
    if (!statSync(edir).isDirectory()) continue;
    const mpath = join(edir, "manifest.json");
    if (!existsSync(mpath)) continue;

    const manifest = JSON.parse(readFileSync(mpath, "utf8"));
    const files = [];
    for (const name of ["firmware.factory.bin", "firmware.bin", "manifest.json"]) {
      const path = join(edir, name);
      /* Release assets share one flat namespace, so the env has to be in the
         name. Three files called firmware.factory.bin would not collide and
         fail - GitHub would accept them and silently rename the second and
         third, leaving a release where nobody can tell which board a binary is
         for. That is worse than a rejection. */
      if (existsSync(path)) files.push({ path, name: `${env}-${name}` });
    }
    builds.push({ env, manifest, files });
  }

  if (!builds.length) throw new Error(`No manifests under ${vdir} - nothing was built for this version`);
  builds.sort((a, b) => a.manifest.board.localeCompare(b.manifest.board));
  return builds;
}

/* What someone landing on the release page needs to know, which is not the
   commit log: which board each file is for, and which of the two binaries they
   want. Nobody can tell firmware.bin from firmware.factory.bin by looking, and
   picking wrong writes a bootloader into an app partition. */
function releaseNotes(version, builds) {
  const mb = (n) => (n / 1024 / 1024).toFixed(2) + " MB";
  const rows = builds.map((b) => {
    const m = b.manifest;
    const factory = m.parts?.[0];
    return `| ${m.board} | \`${b.env}\` | ${m.chipFamily} | ${factory ? mb(factory.size) : "-"} | \`0x${(factory?.offset ?? 0).toString(16)}\` |`;
  });

  return [
    `Firmware ${version} for the boards built at this commit.`,
    "",
    "The easiest way to install this is the browser flasher at",
    "https://diy.power-pilot.uk - it identifies the board, refuses the builds that",
    "would brick it, and keeps your settings on an upgrade.",
    "",
    "| Board | Env | Chip | Size | Flash at |",
    "|---|---|---|---|---|",
    ...rows,
    "",
    "**`<env>-firmware.factory.bin`** is bootloader, partition table and app",
    "merged, written over serial at the offset above. That is the one to flash.",
    "",
    "**`<env>-firmware.bin`** is the app on its own, for an over-the-air update",
    "into the spare OTA slot. Flashing it over serial produces a board that does",
    "not boot.",
    "",
    "`<env>-manifest.json` is what the flasher reads: chip family, minimum flash,",
    "whether PSRAM is required, and where nvs sits - the last of those being what",
    "decides whether an upgrade keeps your configuration.",
  ].join("\n");
}

const version = fwVersion();
const slug = repoSlug();
const builds = collect(version);
const prerelease = isPrerelease(version);

console.log(`Release ${version} -> ${slug}${prerelease ? "  (pre-release)" : ""}`);
for (const b of builds) {
  console.log(`  ${b.manifest.board.padEnd(38)} ${b.files.map((f) => f.name).join(", ")}`);
}

if (DRY) {
  console.log("\n--dry-run: nothing sent. Notes would read:\n");
  console.log(releaseNotes(version, builds));
  process.exit(0);
}

if (!TOKEN) {
  console.error(
    "\nNo GITHUB_TOKEN (or GH_TOKEN) in the environment.\n" +
      "Create a fine-grained token with `contents: write` on this repository, then\n" +
      "  $env:GITHUB_TOKEN = '...'   (PowerShell, this session only)\n" +
      "Nothing has been deployed - run the deploy again once it is set.",
  );
  process.exit(1);
}

/* The tag carries no `v`: every tag in this repository from 2.7.0 onwards is
   the bare version, and one odd tag among twenty-seven is worse than no
   convention at all. */
const tag = version;
const head = git("rev-parse", "HEAD");

let release = await gh("GET", `${API}/repos/${slug}/releases/tags/${encodeURIComponent(tag)}`);

if (release) {
  console.log(`  ~ release exists, updating and replacing assets`);
  release = await gh("PATCH", `${API}/repos/${slug}/releases/${release.id}`, {
    body: { name: version, body: releaseNotes(version, builds), prerelease },
  });
}
else {
  /* Tag the commit that was built, not whatever the default branch happens to
     be at. Development happens on version branches here, so letting GitHub
     default the target would tag main with a release built from somewhere
     else - and the tag is the only record of which source these binaries came
     from. Needs the commit to be pushed; the 422 below says so if it is not. */
  try {
    release = await gh("POST", `${API}/repos/${slug}/releases`, {
      body: {
        tag_name: tag,
        target_commitish: head,
        name: version,
        body: releaseNotes(version, builds),
        prerelease,
        draft: false,
      },
    });
  }
  catch (err) {
    if (/422/.test(err.message)) {
      console.error(
        `\nGitHub would not create the tag. The usual cause is that ${head.slice(0, 8)} has\n` +
          "not been pushed yet - a release can only point at a commit the remote has.\n" +
          "Push the branch and run the deploy again.\n",
      );
    }
    throw err;
  }
  console.log(`  + created ${tag} at ${head.slice(0, 8)}`);
}

/* Replace rather than add. Re-running a deploy for the same version is normal -
   a build gets fixed and goes out again - and an upload onto an existing name
   is accepted and renamed rather than refused, so the old asset has to go
   first. */
const existing = (await gh("GET", `${API}/repos/${slug}/releases/${release.id}/assets`)) ?? [];
const wanted = new Set(builds.flatMap((b) => b.files.map((f) => f.name)));
for (const asset of existing) {
  if (!wanted.has(asset.name)) continue;
  await gh("DELETE", `${API}/repos/${slug}/releases/assets/${asset.id}`);
}

for (const b of builds) {
  for (const f of b.files) {
    const data = readFileSync(f.path);
    const ext = f.name.slice(f.name.lastIndexOf("."));
    await gh("POST", `${UPLOADS}/repos/${slug}/releases/${release.id}/assets?name=${encodeURIComponent(f.name)}`, {
      raw: true,
      body: data,
      headers: { "content-type": CONTENT_TYPE[ext] ?? "application/octet-stream" },
    });
    console.log(`  ^ ${f.name.padEnd(46)} ${(data.length / 1024).toFixed(0)}K`);
  }
}

console.log(`\n${release.html_url}`);
