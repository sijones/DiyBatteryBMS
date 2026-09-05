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
 *   node scripts/publish-release.mjs [--dry-run] [--force]
 *
 * Wants GITHUB_TOKEN (or GH_TOKEN) with `contents: write` on this repository.
 *
 * A bare version is only published from a commit that is on the default branch,
 * and only from a clean tree. A suffixed one goes out from anywhere, which is
 * the whole point of a beta. See the guards below for why those are the two
 * rules and not more.
 */

import { readFileSync, existsSync, readdirSync, statSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const OUT = join(ROOT, "site", "firmware");

const DRY = process.argv.includes("--dry-run");
const FORCE = process.argv.includes("--force");
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
         name. Nineteen files called firmware.factory.bin would be nineteen
         attempts to write one name: the API refuses a duplicate outright with
         422 already_exists, part-way through, leaving a release holding one
         board's binary and an error for the rest. */
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

/* ---- what is allowed to be published, and from where -------------------- */

/* The remote's own idea of its default branch, rather than a hard-coded "main",
   so renaming it does not silently turn the rule below off. */
function defaultBranch() {
  try {
    return git("symbolic-ref", "refs/remotes/origin/HEAD").replace(/^refs\/remotes\/origin\//, "");
  }
  catch {
    return "main";
  }
}

/* Anything git would not recognise in a fresh clone of HEAD: modified tracked
   files, staged changes, and untracked files too. Untracked counts because
   PlatformIO compiles src/ off the disk, not out of git - a src/foo.cpp that
   was never added is in the binary and in no commit. Ignored files are already
   excluded by --porcelain, which is why site/firmware does not show up here. */
function dirtyPaths() {
  return git("status", "--porcelain")
    .split("\n")
    .map((s) => s.trim())
    .filter(Boolean);
}

/* HEAD is contained in the remote default branch. Deliberately a containment
   test rather than "which branch am I standing on": what is being published is
   a commit, and a commit merged to main is on main whether or not the checkout
   says so. It also proves the commit was pushed, which release creation needs
   anyway. */
function mergedToDefault(branch) {
  try {
    git("merge-base", "--is-ancestor", "HEAD", `refs/remotes/origin/${branch}`);
    return true;
  }
  catch {
    return false;
  }
}

const version = fwVersion();
const slug = repoSlug();
const builds = collect(version);
const prerelease = isPrerelease(version);

console.log(`Release ${version} -> ${slug}${prerelease ? "  (pre-release)" : ""}`);
for (const b of builds) {
  console.log(`  ${b.manifest.board.padEnd(38)} ${b.files.map((f) => f.name).join(", ")}`);
}

/* Two rules, and the difference between them is the difference between the
   channels the flasher offers.
 *
 * A pre-release is a build handed to someone who volunteered to test it, cut
 * mid-cycle from whatever branch the work is on. Both of those are fine and the
 * rules stay out of the way.
 *
 * A bare version is the build the flasher offers by default to a person who
 * arrived with no opinion, and it gets one guarantee: the source is on main and
 * the source is what was built. A dirty tree breaks the second - the binaries
 * came from files no commit contains, and the tag points at source that never
 * produced them - and nothing about the result ever shows it, because a release
 * built from uncommitted work looks exactly like one that was not. */
const problems = [];
const warnings = [];
const dirty = dirtyPaths();
const main = defaultBranch();

if (dirty.length) {
  const shown = dirty.slice(0, 8).join("\n    ");
  const more = dirty.length > 8 ? `\n    ... and ${dirty.length - 8} more` : "";
  const what = `the working tree has ${dirty.length} uncommitted change${dirty.length === 1 ? "" : "s"}:\n    ${shown}${more}`;
  if (prerelease) warnings.push(`${what}\n  A pre-release may not match its tag. Publishing anyway.`);
  else problems.push(`${what}\n  Commit or stash, rebuild, and publish again.`);
}

if (!prerelease) {
  try {
    git("fetch", "origin", main, "--quiet");
  }
  catch {
    // Offline, or the remote is unreachable. The check below still runs against
    // whatever origin/main was last known to be; it can only be too strict.
    warnings.push(`could not reach origin, so "is it on ${main}" is being judged on a stale ref`);
  }
  if (!mergedToDefault(main)) {
    problems.push(
      `${version} is a release, and HEAD is not on ${main}.\n` +
        `  Releases are what the flasher hands to someone with no opinion, so they come\n` +
        `  off the branch everybody has. Merge to ${main}, push, and publish from there -\n` +
        `  or give this version a suffix and it goes out as a beta from here.`,
    );
  }
}

for (const w of warnings) console.warn(`  ! ${w}`);

/* --dry-run reports the verdict rather than acting on it. It sends nothing
   either way, and "what would stop this" is most of what anyone runs it to
   find out. */
if (DRY) {
  if (problems.length) {
    console.log(`\n--dry-run: this would ${FORCE ? "be refused without --force" : "be refused"}:\n`);
    for (const p of problems) console.log(`  - ${p}\n`);
  }
  console.log("--dry-run: nothing sent. Notes would read:\n");
  console.log(releaseNotes(version, builds));
  process.exit(0);
}

if (problems.length && !FORCE) {
  console.error("\nNot publishing:\n");
  for (const p of problems) console.error(`  - ${p}\n`);
  console.error("Nothing has been sent. --force overrides this, and means what it says.");
  process.exit(1);
}
if (problems.length) console.warn("  ! --force: publishing over the checks above");

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

/* Every asset on the release, not the first page of them.
 *
 * This endpoint pages at 30 by default, and a release here is one manifest and
 * two binaries per env - 57 assets across nineteen envs, and it grows every
 * time a board is added. Unpaginated, the replace below deleted the first
 * thirty and left the rest, so the thirty-first upload landed on a name that
 * still existed and the whole release died on
 *
 *   POST .../assets?name=esp32s3-MCP-16mb-firmware.factory.bin -> 422
 *   {"code":"already_exists","field":"name"}
 *
 * after uploading half a release. 100 is the maximum page size the API allows,
 * so the loop is what makes this correct rather than merely enough for now. */
async function allAssets(releaseId) {
  const all = [];
  for (let page = 1; ; page++) {
    const batch = (await gh("GET", `${API}/repos/${slug}/releases/${releaseId}/assets?per_page=100&page=${page}`)) ?? [];
    all.push(...batch);
    if (batch.length < 100) return all;
  }
}

/* Replace rather than add. Re-running a deploy for the same version is normal -
   a build gets fixed and goes out again - and an upload onto an existing name
   is refused outright, so the old asset has to go first. A release left
   half-replaced by the failure above heals on the next run for the same reason:
   what is deleted is every name this build is about to write, however it got
   there. */
const existing = await allAssets(release.id);
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
