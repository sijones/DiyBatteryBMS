/* Delete old Cloudflare Pages deployments so the project does not accumulate
 * one forever.
 *
 * Every `npm run deploy` / `npm run pages` creates a new Pages deployment, and
 * Cloudflare never deletes an old one on its own. Two workflows call that on
 * every push that touches site/ or web/ (site.yml, any branch) and on every
 * release tag (release.yml) - which is a lot of deployments a week, and none
 * of them are the firmware rollback mechanism (that is site/firmware plus the
 * GitHub release archive, see build-manifests.mjs and publish-release.mjs).
 * A Pages deployment past the newest handful is just history nobody reads,
 * and left alone it heads for whatever limit Cloudflare enforces on a project.
 *
 *   node scripts/prune-deployments.mjs [--dry-run] [--keep=15]
 *
 * Wants CLOUDFLARE_API_TOKEN and CLOUDFLARE_ACCOUNT_ID (the same two
 * release.yml already carries as secrets).
 *
 * Never deletes the deployment currently aliased to the production domain -
 * that is what a viewer actually gets - nor anything inside the newest `keep`
 * regardless of alias, so a rollback in the Cloudflare dashboard still has
 * something recent to roll back to.
 */

const TOKEN = process.env.CLOUDFLARE_API_TOKEN;
const ACCOUNT = process.env.CLOUDFLARE_ACCOUNT_ID;
const PROJECT = process.env.CLOUDFLARE_PAGES_PROJECT || "diy-power-pilot-uk";
const DRY = process.argv.includes("--dry-run");
const KEEP = Number((process.argv.find((a) => a.startsWith("--keep=")) || "").split("=")[1] || 15);

if (!TOKEN || !ACCOUNT) {
  console.error("Need CLOUDFLARE_API_TOKEN and CLOUDFLARE_ACCOUNT_ID in the environment.");
  process.exit(1);
}

const API = `/client/v4/accounts/${ACCOUNT}/pages/projects/${PROJECT}/deployments`;

/* Plain https rather than fetch, and exactly one reason: this script has to
   END. Node's fetch keeps its connection pool open after the last response,
   so a run over dozens of deployments either hangs until something times out
   or needs a process.exit() bolted on - which on Windows trips a libuv
   assertion and kills the process before the summary line prints, the same
   failure mode restore-published.mjs's comment documents for the same
   reason. `agent: false` closes the socket per request instead, so the event
   loop drains and the process exits on its own everywhere this runs: a
   contributor's machine as much as the ubuntu-latest CI runner. */
import { request as httpsRequest } from "node:https";

function cf(method, path = "", body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const req = httpsRequest(
      {
        hostname: "api.cloudflare.com",
        path: `${API}${path}`,
        method,
        agent: false,
        headers: {
          authorization: `Bearer ${TOKEN}`,
          "content-type": "application/json",
          ...(data ? { "content-length": Buffer.byteLength(data) } : {}),
        },
      },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          let json = null;
          try { json = JSON.parse(Buffer.concat(chunks).toString("utf8")); } catch {}
          if (!res.statusCode || res.statusCode >= 300 || !json?.success) {
            reject(new Error(`${method} ${path} -> ${res.statusCode} ${JSON.stringify(json?.errors ?? json)}`));
          } else {
            resolve(json);
          }
        });
      },
    );
    req.on("error", reject);
    req.setTimeout(30_000, () => req.destroy(new Error(`${method} ${path} -> timed out`)));
    if (data) req.write(data);
    req.end();
  });
}

/* Every page, not just the first - result_info.total_pages is the authority,
   and this project already has enough deployments to span several pages.
   25 is the API's own maximum page size for this endpoint; asking for more
   is refused outright rather than clamped. */
async function allDeployments() {
  const all = [];
  for (let page = 1; ; page++) {
    const res = await cf("GET", `?page=${page}&per_page=25`);
    all.push(...res.result);
    if (page >= (res.result_info?.total_pages ?? 1)) return all;
  }
}

const deployments = await allDeployments();
deployments.sort((a, b) => new Date(b.created_on) - new Date(a.created_on));

const toDelete = deployments
  .slice(KEEP)
  .filter((d) => !d.aliases || d.aliases.length === 0);

const skippedAliased = deployments.slice(KEEP).length - toDelete.length;

console.log(
  `${deployments.length} deployment(s) total, keeping the newest ${KEEP}` +
    (skippedAliased ? ` (+${skippedAliased} older but still aliased, left alone)` : "") +
    `.`,
);

if (!toDelete.length) {
  console.log("Nothing to prune.");
  process.exit(0);
}

for (const d of toDelete) {
  const label = `${d.id}  ${d.created_on}  ${d.deployment_trigger?.metadata?.commit_hash?.slice(0, 8) ?? "?"}`;
  if (DRY) {
    console.log(`  would delete  ${label}`);
    continue;
  }
  try {
    await cf("DELETE", `/${d.id}`);
    console.log(`  deleted       ${label}`);
  } catch (err) {
    // One deployment Cloudflare refuses (a stale alias it has not let go of
    // yet, a race with another deploy) should not abort the rest of the prune.
    console.warn(`  ! could not delete ${d.id}: ${err.message}`);
  }
}

console.log(`${DRY ? "Would delete" : "Deleted"} ${toDelete.length} deployment(s).`);
