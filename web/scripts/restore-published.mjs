/* Bring back what is already published, before republishing.
 *
 * site/firmware is a deploy directory, not a source one: it is gitignored, and
 * a Pages deployment is a COMPLETE SNAPSHOT of it. Those two facts combine into
 * a trap. On a machine that has been building for months the directory holds
 * every recent version and everything works; from a fresh clone - which is what
 * CI always is - it starts empty, build-manifests.mjs scans an empty disk,
 * writes an index with one version in it, and wrangler uploads a site where
 * every earlier build has ceased to exist.
 *
 * Nothing fails. The release succeeds, the flasher works, and the only thing
 * missing is the older build - which nobody looks for until the new one
 * misbehaves, which is the one moment it is needed and the one moment it is
 * gone. KEEP_PER_CHANNEL exists precisely to hold that build; this is what
 * makes it possible to keep.
 *
 * The live site is the source of truth here rather than the GitHub release
 * archive, because the live site is exactly the thing being replaced: whatever
 * it is serving now is what must survive the deployment.
 *
 *   node scripts/restore-published.mjs [--all] [--dry-run] [--source=site|releases]
 *
 * --all keeps the newest version too, for a deploy that is not rebuilding
 * anything. --source picks where to read it back from - the live site, or the
 * GitHub release archive, which is what CI must use. SITE_BASE overrides the
 * site address, SITE_DIR where to write to.
 */

import { readFileSync, existsSync, mkdirSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { get as httpGet } from "node:http";
import { get as httpsGet } from "node:https";
import { execFileSync } from "node:child_process";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");

const BASE = (process.env.SITE_BASE || "https://diy.power-pilot.uk").replace(/\/+$/, "");
const OUT = process.env.SITE_DIR || join(ROOT, "site", "firmware");
const DRY = process.argv.includes("--dry-run");

/* --all restores the version being built as well.
 *
 * Which one is right depends entirely on what happens next. Before a release,
 * the current version must be skipped: it is about to be written from
 * .pio/build, and pulling the published copy first would drag back any env
 * since removed - the nine classic-ESP32 PSRAM builds, say - as though they
 * were still current.
 *
 * A site-only deploy has no .pio/build and is not rebuilding anything, so
 * skipping the current version does not leave it to be regenerated: it leaves
 * it out of the snapshot, and the deploy deletes the newest firmware from the
 * site. Editing a line of copy on the landing page would take every build with
 * it. Hence the flag, and hence it being explicit rather than inferred. */
const ALL = process.argv.includes("--all");

/* Where to read the published firmware back from.
 *
 * "site" is the live deployment, and is right on a person's machine: it is
 * literally the thing about to be replaced, so whatever it serves is what has
 * to survive.
 *
 * "releases" is the GitHub release archive, and is right in CI. A runner asking
 * Cloudflare for releases.json was answered with an HTML challenge page and a
 * 200 - datacenter address, no browser - and the release then failed on a JSON
 * syntax error. The archive holds the same three files per build, is reached
 * through api.github.com with the token the workflow already has, and is in the
 * same trust domain as the runner, so nothing sits in the way of it. */
const SOURCE =
  (process.argv.find((a) => a.startsWith("--source=")) || "").split("=")[1] ||
  process.env.RESTORE_SOURCE ||
  "site";

/* Only the newest few. The archive goes back to 2.7.0 and restoring all of it
   would download hundreds of megabytes to satisfy a rule that keeps two
   versions per channel. Older releases predate this asset layout and carry
   nothing matching, so they are skipped by the filter below in any case. */
const RESTORE_MAX_VERSIONS = 3;

function fwVersion() {
  const cfg = readFileSync(join(ROOT, "src", "config.h"), "utf8");
  const m = /#define\s+FW_VERSION\s+"([^"]+)"/.exec(cfg);
  if (!m) throw new Error("No FW_VERSION in src/config.h");
  return m[1];
}

/* Plain http/https with `agent: false` rather than fetch, for one reason: this
   script has to END.
 *
 * Node's fetch holds its connection pool open after the last response, and
 * there is no public API to shut it. The process then either hangs until
 * something times out, or gets a process.exit() bolted on - which on Windows
 * trips a libuv assertion and dies with code 127. A release that fails because
 * the download step would not put the phone down is a poor trade for a nicer
 * API. `agent: false` opens a connection per request and closes it, so the
 * event loop drains and the process exits on its own, on every platform.

   Redirects are followed because Pages answers some paths with a 30x. */
function get(url, sendHeaders = {}, redirects = 0) {
  return new Promise((resolve, reject) => {
    if (redirects > 5) return reject(new Error(`${url} -> too many redirects`));
    const fetcher = url.startsWith("https:") ? httpsGet : httpGet;
    const req = fetcher(url, { agent: false, headers: { "cache-control": "no-cache", ...sendHeaders } }, (res) => {
      const { statusCode, headers } = res;
      if (statusCode >= 300 && statusCode < 400 && headers.location) {
        res.resume();
        return resolve(get(new URL(headers.location, url).toString(), sendHeaders, redirects + 1));
      }
      const chunks = [];
      res.on("data", (c) => chunks.push(c));
      res.on("end", () => resolve({ status: statusCode, body: Buffer.concat(chunks) }));
    });
    req.on("error", reject);
    req.setTimeout(60_000, () => req.destroy(new Error(`${url} -> timed out`)));
  });
}

async function getJson(url, sendHeaders = {}) {
  const res = await get(url, sendHeaders);
  if (res.status === 404) return null;
  if (res.status !== 200) throw new Error(`${url} -> HTTP ${res.status}`);

  /* A 200 is not a promise of JSON. Cloudflare answers some requests with an
     HTML challenge page and a 200 - which is what a GitHub runner got, so
     JSON.parse was handed "<!DOCTYPE html>" and the release died on a syntax
     error that said nothing about the cause. Say what actually arrived. */
  const text = res.body.toString("utf8");
  if (text.trimStart().startsWith("<")) {
    throw new Error(
      `${url} -> served HTML, not JSON. First bytes: ` + text.trimStart().slice(0, 80).replace(/\s+/g, " "),
    );
  }
  try {
    return JSON.parse(text);
  }
  catch (err) {
    throw new Error(`${url} -> not valid JSON (${err.message})`);
  }
}

async function getBytes(url, sendHeaders = {}) {
  const res = await get(url, sendHeaders);
  if (res.status !== 200) throw new Error(`${url} -> HTTP ${res.status}`);
  return res.body;
}

/** owner/repo, the same way publish-release.mjs works it out. */
function repoSlug() {
  if (process.env.GITHUB_REPOSITORY) return process.env.GITHUB_REPOSITORY;
  const url = execFileSync("git", ["remote", "get-url", "origin"], { cwd: ROOT, encoding: "utf8" }).trim();
  const m = /github\.com[/:]([^/]+\/[^/]+?)(?:\.git)?$/.exec(url);
  if (!m) throw new Error(`origin is not a GitHub remote: ${url}`);
  return m[1];
}

/* Rebuild site/firmware from release assets.
 *
 * publish-release.mjs uploads three files per build, named <env>-<file>,
 * because a release has one flat namespace. This is that naming read backwards:
 * split the env off the front, put the file where the flasher expects it. A
 * release with no assets in this shape is from before the archive existed and
 * is skipped rather than half-restored. */
async function restoreFromReleases(building, out) {
  const slug = repoSlug();
  const token = process.env.GITHUB_TOKEN || process.env.GH_TOKEN;
  const headers = {
    accept: "application/vnd.github+json",
    "user-agent": "diy-battery-bms-restore",
    ...(token ? { authorization: `Bearer ${token}` } : {}),
  };

  const list = await getJson(`https://api.github.com/repos/${slug}/releases?per_page=20`, headers);
  if (!list || !list.length) {
    console.log("  no releases published yet - nothing to restore");
    return { files: 0, bytes: 0, builds: 0 };
  }

  const SUFFIXES = ["-firmware.factory.bin", "-firmware.bin", "-manifest.json"];
  let files = 0, bytes = 0, builds = 0, versions = 0;

  for (const rel of list) {
    if (versions >= RESTORE_MAX_VERSIONS) break;
    const tag = rel.tag_name;
    if (building && tag === building) continue;

    const byEnv = new Map();
    for (const a of rel.assets || []) {
      const suffix = SUFFIXES.find((s) => a.name.endsWith(s));
      if (!suffix) continue;
      const env = a.name.slice(0, -suffix.length);
      if (!byEnv.has(env)) byEnv.set(env, []);
      byEnv.get(env).push({ file: suffix.slice(1), url: a.browser_download_url, size: a.size });
    }
    if (!byEnv.size) continue;   // a release from before this layout
    versions++;

    for (const [env, assets] of byEnv) {
      const dir = join(out, tag, env);
      console.log(`  ${tag}/${env}: ${assets.map((a) => a.file).join(", ")}`);
      builds++;
      if (DRY) { files += assets.length; continue; }
      mkdirSync(dir, { recursive: true });
      for (const a of assets) {
        const target = join(dir, a.file);
        if (existsSync(target)) { console.log(`    ${a.file} already here`); continue; }
        const data = await getBytes(a.url);
        if (data.length !== a.size) {
          throw new Error(`${tag}/${env}/${a.file}: got ${data.length} bytes, the release says ${a.size}`);
        }
        writeFileSync(target, data);
        files++; bytes += data.length;
      }
    }
  }
  return { files, bytes, builds };
}

/* Everything below runs inside main() and returns rather than calling
   process.exit(). Ending a script with process.exit() while fetch still holds
   sockets trips a libuv assertion on Windows - the work is done, the output is
   correct, and the process dies with code 127 anyway, which in CI is a failed
   release for no reason. Let the event loop drain instead. */
async function main() {
  const building = ALL ? null : fwVersion();
  const scope = building ? ` (skipping ${building}, which is being rebuilt)` : " (everything, including the newest)";

  if (SOURCE === "releases") {
    console.log(`Restoring published firmware from the GitHub release archive${scope}`);
    const r = await restoreFromReleases(building, OUT);
    console.log(
      `${DRY ? "Would restore" : "Restored"} ${r.files} file${r.files === 1 ? "" : "s"}` +
        (r.bytes ? ` (${(r.bytes / 1024 / 1024).toFixed(1)} MB)` : "") +
        ` across ${r.builds} build${r.builds === 1 ? "" : "s"}`,
    );
    return;
  }
  if (SOURCE !== "site") throw new Error(`--source must be site or releases, not "${SOURCE}"`);

  console.log(`Restoring published firmware from ${BASE}${scope}`);

  const index = await getJson(`${BASE}/firmware/releases.json`);
  if (!index) {
    // Nothing has ever been published here. That is a legitimate first deploy,
    // not a failure - there is simply nothing to preserve.
    console.log("  no releases.json published yet - nothing to restore");
    return;
  }

  const wanted = [];
  for (const channel of ["release", "beta"]) {
    for (const v of index.channels?.[channel] ?? []) {
      if (building && v.version === building) continue;
      for (const b of v.builds) wanted.push({ channel, version: v.version, build: b });
    }
  }

  if (!wanted.length) {
    console.log(`  nothing to restore${building ? " other than the version being built" : ""}`);
    return;
  }

  let files = 0;
  let bytes = 0;

  for (const { version, build } of wanted) {
    const manifest = await getJson(`${BASE}/firmware/${build.path}`);
    if (!manifest) {
      /* The index names a build the site does not actually serve. Publishing over
         that would bake the inconsistency into the next index, so stop: an index
         promising files that 404 is worse than a failed release, because the
         flasher offers the build and only fails once someone has committed to it. */
      throw new Error(`${build.path} is in releases.json but is not on the site`);
    }

    const dir = join(OUT, version, build.env);
    const names = [
      ...(manifest.parts ?? []).map((p) => p.path),
      ...(manifest.ota ? [manifest.ota.path] : []),
    ];

    console.log(`  ${version}/${build.env}: manifest.json, ${names.join(", ")}`);
    if (DRY) {
      files += names.length + 1;
      continue;
    }

    mkdirSync(dir, { recursive: true });
    writeFileSync(join(dir, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
    files++;

    for (const name of names) {
      const target = join(dir, name);
      // Already on disk means this machine built it or restored it earlier; the
      // bytes are the same and the download is several megabytes.
      if (existsSync(target)) {
        console.log(`    ${name} already here`);
        continue;
      }
      const data = await getBytes(`${BASE}/firmware/${version}/${build.env}/${name}`);
      writeFileSync(target, data);
      files++;
      bytes += data.length;
    }
  }

  console.log(
    `${DRY ? "Would restore" : "Restored"} ${files} file${files === 1 ? "" : "s"}` +
      (bytes ? ` (${(bytes / 1024 / 1024).toFixed(1)} MB)` : "") +
      ` across ${wanted.length} build${wanted.length === 1 ? "" : "s"}`,
  );
}

await main();
