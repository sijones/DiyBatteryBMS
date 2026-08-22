/* GET/POST /api/stats - the three numbers on the landing page.
 *
 * A Cloudflare Pages Function, not a Worker: it is bundled and uploaded by the
 * `wrangler pages deploy ../site` call the site already makes, so this needs no
 * new deploy step, no new CI wiring and no CORS - it answers on the site's own
 * origin, and the path here is the URL: functions/api/stats.js is /api/stats.
 *
 * WHY THIS LIVES IN web/ AND NOT site/. It looks like it belongs beside the
 * thing it is served with, and it does not. Wrangler resolves the functions
 * directory as `<cwd>/functions` - not `<assets dir>/functions` - so what makes
 * this file deploy at all is that `npm run pages` runs from web/, this being
 * the directory npm puts a script in. Under site/ it is silently ignored:
 * wrangler prints "No Functions", deploys the assets, and the endpoint 404s
 * with nothing anywhere saying why. It also happens to match how the rest of
 * the repo is split - site/ is a deploy directory, web/ is what builds it.
 *
 *   downloads  how many times firmware has been downloaded, taken from
 *              GitHub's own download_count on the release assets. Nothing is
 *              recorded here to produce it; it is a number GitHub already keeps.
 *   installs   flashes onto a board with no existing settings partition.
 *   upgrades   flashes onto a board that already had one.
 *
 * The last two are two integers in KV, incremented by a fire-and-forget POST
 * from the flasher at the moment a flash is known to have finished. No
 * identifier, no address, no timestamp, nothing per-user: the entire stored
 * state is "installs: 41". That is the whole reason this is a counter and not
 * an analytics product.
 *
 * MISSING BINDING DEGRADES, IT DOES NOT ERROR. If STATS_KV is not bound - a
 * preview deployment, a local `wrangler pages dev` without --kv, the window
 * between this shipping and the binding being added in the dashboard - GET
 * answers with nulls and POST answers 204 having done nothing. These are
 * vanity figures at the bottom of a page whose actual job is writing firmware
 * over a serial port; a 500 here would make a working deploy look broken, and
 * would do it in the browser console of every visitor.
 *
 * KV is eventually consistent and read-modify-write increments can lose a
 * concurrent write. Accepted: at this traffic a lost count is invisible, and a
 * Durable Object to protect a number nobody audits is not a trade worth making.
 */

const REPO = "sijones/DiyBatteryBMS";

/* Only the images. publish-release.mjs uploads a manifest.json beside each
   binary, and a manifest fetch is the flasher reading metadata, not somebody
   taking a copy of the firmware - counting it would roughly double the figure
   and mean something different from what the page says. Same suffixes
   restore-published.mjs matches on, minus that manifest. */
const FIRMWARE_SUFFIXES = ["-firmware.factory.bin", "-firmware.bin"];

/* Long enough that the GitHub API is asked at most four times an hour, short
   enough that the number is never meaningfully stale. */
const DOWNLOADS_TTL_MS = 15 * 60 * 1000;

const PER_PAGE = 100;
const MAX_PAGES = 10;

const json = (body, status = 200, headers = {}) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", ...headers },
  });

async function readCount(kv, key) {
  const raw = await kv.get(key);
  const n = Number.parseInt(raw ?? "0", 10);
  return Number.isFinite(n) ? n : 0;
}

/* Sum download_count across every firmware asset of every release.
 *
 * Paginated because the archive already runs to several releases of twenty-one
 * envs at two binaries each, which is comfortably past one page. The page cap
 * is a stop rather than a limit: it is far above what this repo will ever have,
 * and exists so a malformed response cannot spin here. */
async function fetchDownloads(env) {
  const headers = {
    accept: "application/vnd.github+json",
    "user-agent": "diy-power-pilot-stats",
    ...(env.GITHUB_TOKEN ? { authorization: `Bearer ${env.GITHUB_TOKEN}` } : {}),
  };

  let total = 0;
  for (let page = 1; page <= MAX_PAGES; page++) {
    const res = await fetch(
      `https://api.github.com/repos/${REPO}/releases?per_page=${PER_PAGE}&page=${page}`,
      { headers },
    );
    if (!res.ok) throw new Error(`GitHub returned ${res.status}`);
    const releases = await res.json();
    if (!Array.isArray(releases) || releases.length === 0) break;

    for (const rel of releases) {
      for (const asset of rel.assets ?? []) {
        if (!FIRMWARE_SUFFIXES.some((s) => asset.name?.endsWith(s))) continue;
        total += asset.download_count ?? 0;
      }
    }

    if (releases.length < PER_PAGE) break;
  }
  return total;
}

/* The cached total, refreshed when it ages out.
 *
 * A failed refresh returns the stale figure rather than propagating: GitHub
 * being briefly unavailable, or the shared edge address hitting the
 * unauthenticated rate limit, should cost the number its freshness and nothing
 * else. Only a failure with nothing cached at all gives up and returns null. */
async function cachedDownloads(kv, env) {
  let cached = null;
  try {
    cached = JSON.parse((await kv.get("gh_downloads_cache")) ?? "null");
  } catch {
    cached = null;
  }

  const fresh = cached && Date.now() - (cached.fetchedAt ?? 0) < DOWNLOADS_TTL_MS;
  if (fresh && typeof cached.total === "number") return cached.total;

  try {
    const total = await fetchDownloads(env);
    await kv.put("gh_downloads_cache", JSON.stringify({ total, fetchedAt: Date.now() }));
    return total;
  } catch {
    return typeof cached?.total === "number" ? cached.total : null;
  }
}

export async function onRequestGet({ env }) {
  const kv = env.STATS_KV;
  if (!kv) return json({ downloads: null, installs: null, upgrades: null });

  try {
    const [downloads, installs, upgrades] = await Promise.all([
      cachedDownloads(kv, env),
      readCount(kv, "installs"),
      readCount(kv, "upgrades"),
    ]);
    return json(
      { downloads, installs, upgrades },
      200,
      { "cache-control": "public, max-age=60" },
    );
  } catch {
    return json({ downloads: null, installs: null, upgrades: null });
  }
}

export async function onRequestPost({ request, env }) {
  let kind = null;
  try {
    kind = (await request.json())?.kind;
  } catch {
    kind = null;
  }

  /* The one thing worth being strict about. Anything else and a typo in the
     client would quietly write a third key nobody reads, and the flasher would
     look like it was counting when it was not. */
  if (kind !== "install" && kind !== "upgrade") {
    return json({ error: 'kind must be "install" or "upgrade"' }, 400);
  }

  const kv = env.STATS_KV;
  if (!kv) return new Response(null, { status: 204 });

  const key = kind === "install" ? "installs" : "upgrades";
  try {
    await kv.put(key, String((await readCount(kv, key)) + 1));
  } catch {
    // The flash succeeded; the count is the only thing that missed. Nothing
    // useful can be done about it here and the caller is not listening.
  }
  return new Response(null, { status: 204 });
}
