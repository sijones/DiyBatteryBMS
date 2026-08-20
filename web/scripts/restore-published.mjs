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
 *   node scripts/restore-published.mjs [--dry-run]
 *
 * SITE_BASE overrides where to read from, SITE_DIR where to write to.
 */

import { readFileSync, existsSync, mkdirSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { get as httpGet } from "node:http";
import { get as httpsGet } from "node:https";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");

const BASE = (process.env.SITE_BASE || "https://diy.power-pilot.uk").replace(/\/+$/, "");
const OUT = process.env.SITE_DIR || join(ROOT, "site", "firmware");
const DRY = process.argv.includes("--dry-run");

/* The version being built is deliberately NOT restored. It is about to be
   written from .pio/build, and pulling the published copy first would drag back
   any env that has since been removed - the nine classic-ESP32 PSRAM builds,
   say - as though they were still current. Only older versions are wanted. */
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
function get(url, redirects = 0) {
  return new Promise((resolve, reject) => {
    if (redirects > 5) return reject(new Error(`${url} -> too many redirects`));
    const fetcher = url.startsWith("https:") ? httpsGet : httpGet;
    const req = fetcher(url, { agent: false, headers: { "cache-control": "no-cache" } }, (res) => {
      const { statusCode, headers } = res;
      if (statusCode >= 300 && statusCode < 400 && headers.location) {
        res.resume();
        return resolve(get(new URL(headers.location, url).toString(), redirects + 1));
      }
      const chunks = [];
      res.on("data", (c) => chunks.push(c));
      res.on("end", () => resolve({ status: statusCode, body: Buffer.concat(chunks) }));
    });
    req.on("error", reject);
    req.setTimeout(60_000, () => req.destroy(new Error(`${url} -> timed out`)));
  });
}

async function getJson(url) {
  const res = await get(url);
  if (res.status === 404) return null;
  if (res.status !== 200) throw new Error(`${url} -> HTTP ${res.status}`);
  return JSON.parse(res.body.toString("utf8"));
}

async function getBytes(url) {
  const res = await get(url);
  if (res.status !== 200) throw new Error(`${url} -> HTTP ${res.status}`);
  return res.body;
}

/* Everything below runs inside main() and returns rather than calling
   process.exit(). Ending a script with process.exit() while fetch still holds
   sockets trips a libuv assertion on Windows - the work is done, the output is
   correct, and the process dies with code 127 anyway, which in CI is a failed
   release for no reason. Let the event loop drain instead. */
async function main() {
  const building = fwVersion();
  console.log(`Restoring published firmware from ${BASE} (skipping ${building}, which is being rebuilt)`);

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
      if (v.version === building) continue;
      for (const b of v.builds) wanted.push({ channel, version: v.version, build: b });
    }
  }

  if (!wanted.length) {
    console.log("  nothing published other than the version being built");
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
