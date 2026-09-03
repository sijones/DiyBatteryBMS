/* The web flasher.
 *
 * Built in increments, in dependency order: connect and identify first, because
 * everything else rests on esptool-js actually talking to a board; then the
 * build chooser and the safety check that decides whether an upgrade keeps the
 * user's settings. Writing firmware comes next and is not wired yet - the
 * Confirm step says so plainly rather than offering a button that lies.
 */

import {
  connectAndIdentify,
  release,
  detachTransport,
  webSerialSupported,
  browserName,
  isMobile,
  isPortBusy,
  reconnectAndReadNvs,
} from "./board.js";
import { fmtHex, fmtSize, nvsVerdict } from "./partitions.js";
import {
  loadReleases,
  loadManifest,
  compatibility,
  rank,
  collapseToWirings,
  defaultSelection,
  versionsIn,
  buildsFor,
} from "./builds.js";
import { backupNvs, flashBuild, restoreNvs, restart } from "./flash.js";
import { watchBoot } from "./watch.js";
import { Console, awaitScanResult, sendCredentials, sendScan, waitReady } from "./console.js";
import { parseScan, dedupe, signalBars, credentialLines } from "./wifi.js";

const $ = (id) => document.getElementById(id);
const esc = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

let session = null;
let choices = [];
let chosen = null;
let verdict = null;
let backedUp = false;
let releaseIndex = null;
/* nvs read right before an upgrade that was told it would keep its settings,
 * held in memory only - never written to disk, since nvs can hold WiFi and
 * MQTT passwords. Compared against a post-flash read once the board is back,
 * so "safe" is something checked rather than just claimed. null whenever
 * there is nothing to compare - a fresh install, an erase, or a build that
 * did not declare itself safe in the first place. */
let preFlashNvs = null;
let selected = { channel: "release", version: null };

function setStage(name) {
  for (const el of document.querySelectorAll("[data-stage]")) el.hidden = el.dataset.stage !== name;
}

function log(line) {
  const el = $("log");
  if (!el) return;
  el.textContent += (el.textContent ? "\n" : "") + line;
  el.scrollTop = el.scrollHeight;
}

const row = (label, value, tone) =>
  `<div class="kv"><div class="k">${esc(label)}</div><div class="v${tone ? " v-" + tone : ""}">${esc(value)}</div></div>`;

/* ---------------------------------------------------------------- identify */

function renderIdentity(s) {
  // Reset from any previous connection - the DOM persists across sessions
  // even though a fresh session object does not.
  $("restore-status").hidden = true;
  $("restore-nvs").disabled = false;
  $("to-choose").disabled = false;

  const i = s.info;
  $("identity").innerHTML = [
    row("Family", i.chipName),
    row("Description", i.description),
    row("Flash", fmtSize(i.flashSizeBytes)),
    row("PSRAM", i.psram.present ? `${i.psram.megabytes} MB` : "none found", i.psram.present ? "ok" : "dim"),
    row("MAC", i.mac, "dim"),
  ].join("");

  if (!s.table.valid) {
    $("layout").innerHTML = `<p class="note">No partition table at ${fmtHex(0x8000)} — this board is blank, or carries something we do not recognise.</p>`;
    return;
  }

  const rows = s.table.partitions
    .map(
      (p) =>
        `<tr><td class="mono">${esc(p.label)}</td><td class="mono dim">${p.typeName}/${p.subtypeName}</td>` +
        `<td class="mono">${fmtHex(p.offset)}</td><td class="mono dim">${fmtSize(p.size)}</td></tr>`,
    )
    .join("");

  $("layout").innerHTML =
    `<table><thead><tr><th>Label</th><th>Type</th><th>Offset</th><th>Size</th></tr></thead><tbody>${rows}</tbody></table>` +
    (s.nvs
      ? `<p class="note ok">Settings live at ${fmtHex(s.nvs.offset)}, ${fmtSize(s.nvs.size)}.</p>`
      : `<p class="note warn">No nvs partition — there are no settings here to preserve.</p>`);

  // Only offered where there is somewhere to put it back. Size is checked for
  // real at write time, against this exact partition - this is just whether
  // asking makes sense at all.
  $("restore-nvs").hidden = !s.nvs;
}

/* What is published, shown before a board is connected.
 *
 * Fetched on load rather than when the chooser opens, because the question it
 * answers - "is there anything newer than what I am running?" - is one people
 * arrive with, and making them plug a board in to find out is a poor trade for
 * one small request. It also warms the index the chooser needs later.
 *
 * Failure is silent and leaves the row hidden. Nothing here is needed to flash
 * a board: the chooser fetches the index again and reports its own errors in
 * the place where they would actually stop someone. A red banner on the landing
 * page for a fact nobody asked for yet would be noise. */
async function showLatest() {
  try {
    releaseIndex = await loadReleases();
  } catch {
    return;
  }

  const set = (id, version) => {
    const el = $(id);
    el.textContent = version ?? "none published yet";
    el.classList.toggle("none", !version);
  };
  set("latest-release", releaseIndex.latest?.release);
  set("latest-beta", releaseIndex.latest?.beta);
  $("latest").hidden = false;
}

/* How much the thing is actually used, shown beside what is published.
 *
 * Three aggregate numbers from /api/stats: firmware downloads, which is
 * GitHub's own count on the release assets, and installs and upgrades, which
 * are the counters reportFlash() below adds to. Nothing here identifies
 * anybody - there is no per-visitor state to show, because none is kept.
 *
 * Same shape and the same silence as showLatest(): if the endpoint is missing,
 * unbound or slow, the row stays hidden and the page carries on. Nobody came
 * here to read a counter. */
async function showStats() {
  let stats;
  try {
    const res = await fetch("/api/stats");
    if (!res.ok) return;
    stats = await res.json();
  } catch {
    return;
  }

  const num = (n) => (typeof n === "number" && Number.isFinite(n) ? n : null);
  const values = [num(stats?.downloads), num(stats?.installs), num(stats?.upgrades)];

  /* All three missing means the endpoint answered but has nothing behind it -
     no KV binding yet, or a preview deployment. Stay hidden rather than show a
     row of "not counted yet" under a note promising anonymous counts; a
     half-built feature announcing itself is worse than one that waits. */
  if (values.every((v) => v === null)) return;

  const set = (id, n) => {
    const el = $(id);
    el.textContent = n === null ? "not counted yet" : n.toLocaleString();
    el.classList.toggle("none", n === null);
  };
  set("stat-downloads", values[0]);
  set("stat-installs", values[1]);
  set("stat-upgrades", values[2]);
  $("stats").hidden = false;
  $("stats-note").hidden = false;
}

/* Add one to the install or upgrade count, and do not wait for it.
 *
 * Sent once, at the single point a flash is known to have finished. No
 * identifier and no payload beyond which of the two it was; the endpoint keeps
 * two integers and nothing else.
 *
 * Never awaited and never surfaced. The firmware is already on the board by the
 * time this runs, so a failed count is not a thing the person in front of the
 * page can act on or would want to be told about. */
function reportFlash(kind) {
  try {
    fetch("/api/stats", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ kind }),
      keepalive: true,
    }).catch(() => {});
  } catch {
    // No fetch, no network, no counter. Immaterial to the job just completed.
  }
}

/* ------------------------------------------------------------------ choose */

async function loadChoices() {
  const list = $("builds");
  list.innerHTML = `<p class="note">Loading builds…</p>`;

  try {
    releaseIndex = await loadReleases();
  } catch (e) {
    list.innerHTML = `<p class="note warn">${esc(e.message)}</p>`;
    return;
  }

  const pick = defaultSelection(releaseIndex);
  selected = pick;
  renderChannelPicker();
  await renderBuildsFor(pick.channel, pick.version);
}

/** Channel tabs and the version dropdown beside them. */
function renderChannelPicker() {
  const counts = {
    release: versionsIn(releaseIndex, "release").length,
    beta: versionsIn(releaseIndex, "beta").length,
  };

  for (const ch of ["release", "beta"]) {
    const tab = $(`tab-${ch}`);
    tab.disabled = counts[ch] === 0;
    tab.classList.toggle("active", selected.channel === ch);
    tab.title = counts[ch] === 0 ? `No ${ch} builds published` : "";
  }

  const sel = $("version");
  const list = versionsIn(releaseIndex, selected.channel);
  sel.innerHTML = list
    .map(
      (v, i) =>
        `<option value="${esc(v.version)}"${v.version === selected.version ? " selected" : ""}>` +
        `${esc(v.version)}${i === 0 ? " — newest" : ""}</option>`,
    )
    .join("");
  sel.disabled = list.length <= 1;

  // Older-than-newest is the rollback case; say so rather than leaving the
  // dropdown to imply it.
  const isOlder = list.length > 1 && selected.version !== list[0]?.version;
  $("older-note").hidden = !isOlder;
  $("beta-note").hidden = selected.channel !== "beta";
}

async function renderBuildsFor(channel, version) {
  const list = $("builds");
  const entries = buildsFor(releaseIndex, channel, version);

  if (!entries.length) {
    list.innerHTML = `<p class="note warn">No builds published for ${esc(version ?? "this channel")}.</p>`;
    $("to-confirm").disabled = true;
    return;
  }

  const manifests = await Promise.all(
    entries.map(async (b) => {
      try {
        return await loadManifest("./firmware", b.path);
      } catch {
        return null;
      }
    }),
  );

  choices = rank(
    manifests.filter(Boolean).map((manifest) => ({ manifest, compat: compatibility(manifest, session.info) })),
  );

  renderChoiceList();
}

/* Collapsed by default, showing one build per wiring.
 *
 * There are thirty builds now, and on a 16MB S3 with PSRAM nine of them will
 * run. Listing all nine does not give a person more control, it gives them a
 * choice they have no way to make: eight of those nine differ only in things
 * the flasher has already read off the chip. What is left after collapsing is
 * the single question the silicon cannot answer - how CAN is wired - with the
 * best build for this board already picked behind each answer.
 *
 * The full list is one click away and never disappears. Someone who wants the
 * 8MB image on a 16MB board, or an older partition layout, has a real reason
 * for it and should not be argued with. */
let showAllBuilds = false;

function renderChoiceList() {
  const list = $("builds");
  const wirings = collapseToWirings(choices);
  const rows = showAllBuilds ? choices.map((entry, index) => ({ entry, index, hidden: 0 })) : wirings;

  /* A ruled-out wiring shows its name and why, and nothing else.
   *
   * Which of its builds happens to represent it is an accident of sorting, and
   * naming one would suggest the others might work - they cannot: a wiring is
   * ruled out here on chip family, which every build of it shares. Same for the
   * count. What the row is for is answering "why isn't my board in this list",
   * and the reason alone answers it. */
  const buildRow = ({ entry: c, index, hidden }) => `
      <label class="choice${c.compat.ok ? "" : " disabled"}">
        <input type="radio" name="build" value="${index}"${c.compat.ok ? "" : " disabled"}${
          chosen && choices[index] === chosen ? " checked" : ""
        }>
        <div class="choice-body">
          <div class="choice-head">
            <span class="choice-name">${esc(c.manifest.board)}</span>
            ${c.compat.ok || !hidden ? `<span class="mono dim choice-env">${esc(c.manifest.env)}</span>` : ""}
          </div>
          <div class="choice-detail${c.compat.ok ? "" : " bad"}">${esc(c.compat.ok ? c.manifest.detail : c.compat.reason)}</div>
          ${c.compat.ok && hidden ? `<div class="choice-note">Best match of ${hidden + 1} builds for this board</div>` : ""}
          ${c.compat.ok && c.manifest.hint ? `<div class="choice-note">${esc(c.manifest.hint)}</div>` : ""}
        </div>
      </label>`;

  list.innerHTML =
    rows.map(buildRow).join("") +
    (choices.length > wirings.length
      ? `<button type="button" class="link-more" id="toggle-all">${
          showAllBuilds ? "Show best match per board" : `Show all ${choices.length} builds`
        }</button>`
      : "");

  $("toggle-all")?.addEventListener("click", () => {
    showAllBuilds = !showAllBuilds;
    renderChoiceList();
  });

  for (const input of list.querySelectorAll("input[name=build]")) {
    input.addEventListener("change", () => {
      chosen = choices[Number(input.value)];
      $("to-confirm").disabled = false;
    });
  }

  // Expanding or collapsing must not leave Continue enabled for a build that is
  // no longer on screen to be checked.
  if (chosen && !list.querySelector("input[name=build]:checked")) {
    chosen = null;
    $("to-confirm").disabled = true;
  }
}

/* ----------------------------------------------------------------- confirm */

function renderConfirm() {
  const m = chosen.manifest;
  verdict = nvsVerdict(session.nvs, m.nvs, m.gap);
  backedUp = false;

  $("verdict").className = `banner ${verdict.safe ? "ok" : verdict.kind === "fresh" ? "" : "err"}`;
  $("verdict").innerHTML =
    `<div><div class="verdict-title">${
      verdict.safe ? "Settings will survive" : verdict.kind === "fresh" ? "Nothing to preserve" : "Settings would be lost"
    }</div><div class="verdict-detail">${esc(verdict.detail)}</div></div>`;

  $("plan").innerHTML = m.parts
    .map(
      (p) =>
        `<div class="kv"><div class="k mono">${fmtHex(p.offset)}</div>` +
        `<div class="v dim">${esc(p.path)} · ${fmtSize(p.size)}</div></div>`,
    )
    .join("") +
    (session.nvs && verdict.safe
      ? `<div class="kv"><div class="k mono">${fmtHex(session.nvs.offset)}</div><div class="v v-ok">nvs · not touched</div></div>`
      : "");

  /* The env, not just the board, because several builds now share a wiring and
     differ only in flash size and PSRAM. This is the screen after which the
     write happens, so it names exactly what is about to be written rather than
     the family it belongs to. */
  $("chosen-name").textContent = `${m.board} · ${m.env} · ${m.version}`;

  /* Two separate choices, both the user's:
       - keep the settings, or erase them
       - save a copy first, or don't
     They were previously conflated and decided automatically. Keeping is the
     default when it is possible, because it is what almost everyone wants from
     an upgrade - but it is a default, not a decision made on their behalf. */
  const canKeep = verdict.safe;
  const nothingToKeep = !session.nvs;

  $("settings-choice").hidden = nothingToKeep;
  $("keep-settings").disabled = !canKeep;
  $("keep-label").classList.toggle("disabled", !canKeep);
  $("keep-why").textContent = canKeep ? verdict.detail : verdict.detail;
  $("keep-why").className = canKeep ? "choice-detail" : "choice-detail bad";

  // When keeping is impossible, erasing is the only way through - so select it
  // rather than leaving a dead screen with everything disabled.
  if (canKeep) $("keep-settings").checked = true;
  else $("erase-settings").checked = true;

  // Backup is offered whenever there is something to read, and remembers
  // whether this person wants it - reflashing a board twenty times in an
  // afternoon should not mean twenty downloads.
  $("backup-choice").hidden = nothingToKeep;
  $("want-backup").checked = localStorage.getItem("backup") !== "off";

  updateFlashButton();
}

function eraseChosen() {
  return !session.nvs || $("erase-settings").checked;
}

function updateFlashButton() {
  const erase = eraseChosen();
  $("do-flash").textContent = erase ? "Erase and write firmware" : "Write firmware";
  $("do-flash").className = erase ? "danger" : "primary";
  $("erase-warning").hidden = !erase || !session.nvs;

  // Re-render the plan: whether nvs survives is the difference that matters
  const m = chosen.manifest;
  $("plan").innerHTML =
    m.parts
      .map(
        (p) =>
          `<div class="kv"><div class="k mono">${fmtHex(p.offset)}</div>` +
          `<div class="v dim">${esc(p.path)} · ${fmtSize(p.size)}</div></div>`,
      )
      .join("") +
    (session.nvs
      ? `<div class="kv"><div class="k mono">${fmtHex(session.nvs.offset)}</div>` +
        (erase
          ? `<div class="v" style="color:var(--err)">nvs · erased</div></div>`
          : `<div class="v v-ok">nvs · not touched</div></div>`)
      : "");
}

/* ------------------------------------------------------------------- done */

/**
 * What to say once it is back.
 *
 * The WiFi step is offered only when the board actually needs it. An upgrade
 * that preserved nvs kept its credentials too, so it rejoins on its own and
 * asking again would be asking for something it already has. But "settings
 * preserved" does not by itself mean "has credentials" - a board that was
 * never configured has an nvs full of defaults - so this branches on what the
 * board reported, not on what we did to it.
 */
/* ------------------------------------------------------------------- wifi */

let con = null;
let networks = [];
let pickedNetwork = null;

/** Append a line to the WiFi step's console panel and keep it scrolled to the end. */
function logConsole(text) {
  $("wifi-log").textContent += text;
  $("wifi-log").scrollTop = $("wifi-log").scrollHeight;
}

async function openWifiStep() {
  setStage("wifi");
  $("wifi-status").textContent = "Opening the console…";
  $("wifi-list").innerHTML = "";
  pickedNetwork = null;
  $("wifi-connect").disabled = true;

  con = new Console(session.port);
  con.onText = (chunk) => logConsole(chunk);
  // Prefixed and on its own line so a command is visually distinct from the
  // board's own output - onSend already redacted the passphrase by the time
  // this sees it.
  con.onSend = (line) => logConsole(`\n> ${line}\n`);

  try {
    await con.open();
    // Opening the port resets this board - confirmed on the bench, not
    // theoretical - so the console is not actually listening yet just
    // because open() resolved. See waitReady() for why.
    $("wifi-status").textContent = "Waiting for the board to finish restarting…";
    const ready = await waitReady(con);
    if (!ready) {
      $("wifi-status").textContent = "The board did not answer after restarting. Try again, or type the name in.";
      return;
    }
    await doScan();
  } catch (e) {
    $("wifi-status").textContent = `Could not open the console: ${e.message}`;
  }
}

async function doScan() {
  $("wifi-status").textContent = "Sending the scan command…";
  $("wifi-rescan").disabled = true;

  // Wait for the board's own "[scan] scanning..." line rather than assuming
  // it the moment the command is sent - send() only proves bytes left the
  // USB lead, not that anything on the other end was listening for them.
  const acked = await sendScan(con);
  if (!acked) {
    $("wifi-status").textContent = "The board did not acknowledge the scan command. Try again, or type the name in.";
    $("wifi-rescan").disabled = false;
    return;
  }

  $("wifi-status").textContent = "Scanning… (a few seconds)";
  const { text, timedOut } = await awaitScanResult(con);
  $("wifi-rescan").disabled = false;

  if (timedOut) {
    $("wifi-status").textContent = "The board did not answer the scan. Try again, or type the name in.";
    return;
  }

  const parsed = parseScan(text);
  networks = dedupe(parsed.networks);

  const hiddenCount = parsed.networks.length - networks.length;
  $("wifi-status").textContent =
    `${networks.length} network${networks.length === 1 ? "" : "s"}` +
    (hiddenCount > 0 ? ` (${hiddenCount} duplicate radio${hiddenCount === 1 ? "" : "s"} merged)` : "");

  $("wifi-list").innerHTML = networks
    .map(
      (n, i) => `
      <label class="choice">
        <input type="radio" name="ssid" value="${i}">
        <div class="choice-body">
          <div class="choice-head">
            <span class="choice-name mono">${esc(n.name)}</span>
            ${n.radios > 1 ? `<span class="choice-env">${n.radios} radios</span>` : ""}
            ${n.open ? `<span class="choice-env">open</span>` : ""}
            ${!n.plainText ? `<span class="choice-env" style="color:var(--warn)">sent as bytes</span>` : ""}
          </div>
          <div class="choice-detail">${"▮".repeat(signalBars(n.rssi))}${"▯".repeat(4 - signalBars(n.rssi))} &nbsp;${n.rssi} dBm</div>
        </div>
      </label>`,
    )
    .join("");

  sizeWifiList();

  for (const input of $("wifi-list").querySelectorAll("input[name=ssid]")) {
    input.addEventListener("change", () => {
      pickedNetwork = networks[Number(input.value)];
      $("wifi-pass").disabled = pickedNetwork.open;
      $("wifi-connect").disabled = false;
    });
  }
}

/** How many networks the list shows before it starts scrolling. */
const WIFI_ROWS_SHOWN = 5;

/**
 * Cap the scan list at five rows and let the rest scroll.
 *
 * Measured off the rendered rows rather than written as a max-height in the
 * stylesheet, because rows are not all the same height - a long SSID with a
 * couple of badges wraps its head line and takes an extra line. Cutting at a
 * guessed pixel figure would show four and a half rows for one scan and five
 * and a bit for the next.
 *
 * The cut lands in the gap BELOW the fifth row (its bottom edge plus the
 * margin that separates it from the sixth), so five rows are whole and the
 * sixth is not sliced through its name.
 *
 * Nothing is capped when everything already fits: a list of three should not
 * carry a scrollbar gutter for scrolling it will never do.
 */
function sizeWifiList() {
  const list = $("wifi-list");
  const rows = list.children;

  // Cleared first so a rescan that finds fewer networks is measured against an
  // uncapped list rather than against the height the last scan left behind.
  list.style.maxHeight = "";
  list.classList.remove("scrolls");
  if (rows.length <= WIFI_ROWS_SHOWN) return;

  const last = rows[WIFI_ROWS_SHOWN - 1];
  const gap = parseFloat(getComputedStyle(last).marginBottom) || 0;
  const height = last.getBoundingClientRect().bottom - list.getBoundingClientRect().top + gap;

  // A hidden list measures as zero, and a 0px cap would hide the networks
  // altogether. Leave it uncapped rather than collapse it.
  if (height <= 0) return;

  list.style.maxHeight = `${Math.round(height)}px`;
  list.classList.add("scrolls");

  // Say so in words. The scrollbar is thin by design and easy to miss, and
  // someone hunting for a network that is three rows below the fold will
  // otherwise decide the board cannot see it and start retyping the name.
  $("wifi-status").textContent += ` · scroll for the other ${rows.length - WIFI_ROWS_SHOWN}`;
}

async function onWifiConnect() {
  const name = $("wifi-manual").value.trim();
  const network = name ? null : pickedNetwork;
  if (!network && !name) return;

  $("wifi-connect").disabled = true;
  $("wifi-status").textContent = "Sending credentials and restarting…";

  const lines = credentialLines({
    network,
    name,
    passphrase: $("wifi-pass").value,
    hostname: $("wifi-host").value.trim() || null,
  });

  const res = await sendCredentials(con, lines);
  await con.close();
  con = null;

  if (res.ok) {
    $("done-address").hidden = false;
    $("done-ip").textContent = `http://${res.ip}/`;
    $("done-ip").href = `http://${res.ip}/`;
    $("done-settings").textContent = `Joined ${network ? network.name : name}.`;
    $("done-next").textContent =
      "Everything else — CAN protocol, charge limits, MQTT, the schedule — is set from the board's own pages.";
    $("wifi-cta").hidden = true;
    setStage("done");
    return;
  }

  const failMessage = res.apMode
    ? "The board did not join — it came back up on its own access point. Check the passphrase."
    : "No address came back within 30 seconds. It may still be trying; check the log.";
  $("wifi-status").textContent = failMessage;
  $("wifi-connect").disabled = false;
  // Reopen so another attempt can be made without starting over. The failed
  // "connect" already restarted the board once, so this reopen races the
  // same reset-on-open behaviour openWifiStep() waits out - wait here too
  // rather than leaving the next click to find out the hard way.
  con = new Console(session.port);
  con.onText = (chunk) => logConsole(chunk);
  con.onSend = (line) => logConsole(`\n> ${line}\n`);
  try {
    await con.open();
    if (!(await waitReady(con))) {
      $("wifi-status").textContent = `${failMessage} The console has not answered since restarting either - reconnecting may help.`;
    }
  } catch {
    /* the user can go back and retry */
  }
}

/* ------------------------------------------------------------------- done */

function renderDone(m, erased, boot) {
  $("done-what").textContent = `${m.board} · ${m.version}`;

  if (boot.onNetwork) {
    const where = boot.ssid ? `on <span class="mono" style="color:var(--text)">${esc(boot.ssid)}</span>` : "on your network";
    $("done-settings").innerHTML = `Back up and ${where}` + (erased ? "." : " with your settings intact.");
    $("done-address").hidden = false;
    $("done-ip").textContent = `http://${boot.ip}/`;
    $("done-ip").href = `http://${boot.ip}/`;
    $("done-next").textContent =
      "Everything else — CAN protocol, charge limits, MQTT, the schedule — is set from the board's own pages.";
    // Already on a network: offering to set WiFi up would be asking for
    // something it plainly has.
    $("wifi-cta").hidden = true;
    return;
  }

  $("done-address").hidden = true;
  // It needs a network, or we could not tell - either way the offer is useful,
  // and the note says which case it is.
  $("wifi-cta").hidden = false;
  $("wifi-cta-note").textContent = boot.joinedWithoutAddress
    ? "It joined but had no address yet when we stopped listening. It may be fine — check the board, or set the network up again here."
    : boot.needsWifi
      ? "It came up on its own access point, so it still needs putting on your network."
      : "If it is not on your network, you can set that up over this cable now.";

  if (!boot.heardFrom) {
    // Silence is not the same as unconfigured, and "we could not listen" is not
    // the same as "it said nothing" - conflating those sent us hunting for a
    // hardware fault that did not exist.
    $("done-settings").textContent = erased
      ? "The chip was erased, so it starts with no settings."
      : "Your settings were left where they were.";
    $("done-next").textContent = boot.problem
      ? `The firmware was written, but this page could not listen to the board afterwards (${boot.problem}). Open a serial monitor on the same port to see how it came up.`
      : "It did not print anything after restarting, which is normal for boards whose console is on a different USB port. Check there to see how it came up.";
    return;
  }

  $("done-settings").textContent = erased
    ? "The chip was erased, so it starts with no settings."
    : "Your settings were kept, but it has no WiFi credentials stored.";
  $("done-next").textContent =
    "It came up on its own access point, so it still needs to be put on your network.";
}

/* ------------------------------------------------------------------ flash */

function progress(written, total) {
  const pct = total ? Math.round((written / total) * 100) : 0;
  $("bar").style.width = `${pct}%`;
  $("pct").textContent = `${pct}%`;
  $("bytes").textContent = `${(written / 1024 / 1024).toFixed(2)} / ${(total / 1024 / 1024).toFixed(2)} MB`;
}

async function onBackup() {
  const btn = $("do-backup");
  btn.disabled = true;
  btn.textContent = "Reading…";
  try {
    const saved = await backupNvs(session);
    backedUp = true;
    btn.textContent = `Saved ${saved.filename}`;
  } catch (e) {
    btn.disabled = false;
    btn.textContent = "Download backup";
    alert(`Could not read the settings: ${e.message}`);
  }
}

/**
 * Write a previously-downloaded nvs-*.bin backup back onto the connected
 * board, from the Identified screen - no build needs choosing, since this
 * touches nothing but the settings partition already on the board.
 *
 * Confirmed with a native confirm() rather than the styled dialog the rest of
 * this page uses elsewhere, because there isn't one here yet and this is a
 * single yes/no gate on a destructive action - not worth building a modal for.
 */
async function onRestoreNvsFile(input) {
  const file = input.files[0];
  input.value = ""; // so picking the same file again still fires a change event
  if (!file) return;

  if (
    !confirm(
      `Write ${file.name} (${file.size} bytes) to this board's settings partition at ` +
        `${fmtHex(session.nvs.offset)}? This overwrites every setting currently on the board and ` +
        `reboots it. This cannot be undone.`,
    )
  ) {
    return;
  }

  const status = $("restore-status");
  const text = $("restore-status-text");
  status.hidden = false;
  $("restore-nvs").disabled = true;
  $("to-choose").disabled = true;
  text.textContent = "Reading the backup file…";

  try {
    const bytes = new Uint8Array(await file.arrayBuffer());

    text.textContent = "Writing settings…";
    await restoreNvs(session, bytes, {
      onProgress: (written, total) => {
        const pct = total ? Math.round((written / total) * 100) : 0;
        text.textContent = `Writing settings… ${pct}%`;
      },
    });

    // Same reboot sequence onFlash() uses and for the same reason: esptool's
    // own reset only releases RTS, relying on state that does not survive
    // closing the port, so detach first and let the watcher drive the reset.
    text.textContent = "Restarting the board…";
    await detachTransport(session);
    session.finished = true;

    const boot = await watchBoot(session.port, () => {});
    text.textContent = boot.problem
      ? `Settings restored, but the board did not come back cleanly: ${boot.problem}. Disconnect and reconnect to check on it.`
      : "Settings restored — the board has restarted with them. Disconnect and reconnect if you want to do anything else.";
    // The port closed under watchBoot and session.finished is now set - there
    // is nothing left here for either button to act on until reconnecting.
  } catch (err) {
    text.textContent = `Could not restore: ${err.message}`;
    $("restore-nvs").disabled = false;
    $("to-choose").disabled = false;
  }
}

async function onFlash() {
  const m = chosen.manifest;
  const eraseAll = eraseChosen();
  const wantBackup = session.nvs && $("want-backup").checked;

  setStage("flashing");
  $("flash-what").textContent = `${m.board} · ${m.version}`;
  progress(0, 1);

  /* Independent of the backup checkbox above, which only controls whether a
     FILE gets offered - this is an in-memory safety net for the one case that
     matters: an upgrade the Confirm screen just told the user would keep its
     settings. Nothing to verify on a fresh install (nothing there yet) or a
     chosen erase (the point was to wipe it). */
  preFlashNvs = null;
  const wantVerify = !eraseAll && verdict?.safe && session.nvs;

  try {
    /* Only if asked. It used to run regardless, which is a download nobody
       requested every time a board is reflashed - and on an erase it is the
       one time the copy genuinely matters, so the choice is worth respecting
       in both directions rather than being clever about it. */
    if (wantBackup && !backedUp) {
      $("flash-step").textContent = "Saving your settings…";
      await backupNvs(session);
      backedUp = true;
    }

    if (wantVerify) {
      $("flash-step").textContent = "Reading settings to verify afterward…";
      try {
        const snap = await backupNvs(session, { download: false });
        preFlashNvs = snap?.bytes ?? null;
      } catch {
        // The flash itself does not depend on this - just nothing to verify.
        preFlashNvs = null;
      }
    }

    $("flash-step").textContent = eraseAll ? "Erasing and writing…" : "Writing firmware…";
    await flashBuild(session, m, { eraseAll, onProgress: progress });

    /* Release the port, then let the watcher reset the board itself.
     *
     * Not esptool's restart(): its hard reset only releases RTS, relying on
     * state from the connect sequence that does not survive closing the port -
     * and reopening can leave the board held in reset or in the bootloader.
     * Resetting once we already hold the reader means the boot is captured
     * from its first byte. */
    await detachTransport(session);
    session.finished = true;

    $("flash-step").textContent = "Restarting and listening… (up to 20s)";
    $("boot-log").hidden = false;
    $("boot-log").textContent = "";

    const boot = await watchBoot(session.port, (chunk) => {
      $("boot-log").textContent += chunk;
      $("boot-log").scrollTop = $("boot-log").scrollHeight;
    });


    renderDone(m, eraseAll, boot);
    setStage("done");

    /* Counted here and nowhere else: this is the one line that only runs when
       a flash has actually completed. verdict is what the Confirm step already
       worked out about the board's settings partition, so "fresh" means there
       was nothing on it to keep - a first install. Deliberately not the
       eraseAll flag, which is also true when someone chooses to wipe a board
       that has been running for a year. */
    reportFlash(verdict?.kind === "fresh" ? "install" : "upgrade");

    // After renderDone(), not before: the done screen should appear straight
    // away rather than wait on a second reconnect, and this only updates one
    // card within it once it resolves.
    await verifySettingsSurvived();
  } catch (err) {
    showFailure(err);
  }
}

/**
 * The safety net preFlashNvs exists for: reconnect once the board is back up
 * and check nvs actually matches what was read right before writing, rather
 * than trusting the Confirm screen's own promise unverified. A silent no-op
 * whenever there was nothing to compare - see preFlashNvs's own comment for
 * the cases that skips.
 *
 * Informational only - there used to be a one-click "Restore now" here, and
 * it was wrong to offer it. A byte-for-byte partition compare is not proof
 * of data loss: ESP32's NVS is a page-based log, and writing or updating
 * ANY key can relocate OTHER, unrelated entries to different physical
 * offsets during its own housekeeping, changing the raw bytes with no
 * logical value ever changing. A board's first real boot makes this worse -
 * main.cpp's own first-run block writes roughly forty default keys the
 * moment "EEPROMSetup" is not already set, which alone is enough to fail a
 * raw compare against a snapshot taken one boot earlier. Restoring on a
 * false positive would write that PRE-defaults snapshot back over a board
 * that had just correctly initialised itself - actively worse than doing
 * nothing. A mismatch here is a prompt to go and check the Settings page,
 * not evidence to act on unverified.
 */
async function verifySettingsSurvived() {
  if (!preFlashNvs) return;
  const before = preFlashNvs;
  const hadBackup = backedUp; // capture before anything below can change it
  preFlashNvs = null; // one check per flash, whatever the outcome

  const card = $("verify-status");
  const text = $("verify-text");
  card.hidden = false;
  text.textContent = "Checking the settings partition…";

  const backupNote = hadBackup
    ? " Your settings were also saved to a file before flashing, if you need it."
    : " No settings file was saved before flashing, so there is nothing to fall back on beyond this.";

  let after;
  try {
    after = await reconnectAndReadNvs(session.port, session.nvs, () => {});
  } catch (e) {
    text.textContent = `Could not reconnect to check: ${e.message}.${backupNote}`;
    return;
  }

  const same = before.length === after.length && before.every((b, i) => b === after[i]);
  if (same) {
    text.textContent = "The settings partition is unchanged from right before flashing.";
    return;
  }

  text.textContent =
    "The settings partition's raw bytes changed from right before flashing. This is not necessarily a " +
    "problem — ESP32's NVS storage can relocate unrelated entries internally, and a board's first real " +
    "boot writes its own defaults the same way — so check the device's own Settings page before assuming " +
    `anything was lost.${backupNote}`;
}

/* ------------------------------------------------------------------- flow */

/**
 * The failure screen, with advice that matches the failure.
 *
 * "Failed to open serial port" and "the board did not answer" are different
 * problems with no overlap in what fixes them, and one paragraph covering both
 * meant everyone read half of it and acted on the wrong half. On Windows in
 * particular a port is claimed by exactly one program at a time, so an open
 * failure is nearly always another program - most often the serial monitor in
 * the IDE the board was just built from.
 */
function showFailure(err) {
  $("error").textContent = err.message ?? String(err);

  $("fail-advice").innerHTML = isPortBusy(err)
    ? "Something else has the port open — Windows hands it to one program at a time. Close any serial " +
      "monitor (PlatformIO, the Arduino IDE, PuTTY, a VS Code terminal) <b>and</b> any other tab of this " +
      "page, then try again. If nothing obvious is holding it, unplug the board and plug it back in: a " +
      "monitor closed while the port was busy can leave it claimed until the board re-enumerates."
    : "A board already open in a serial monitor cannot be opened here as well — close it and try again. " +
      "Some boards also need the boot button held while connecting.";

  setStage("failed");
}

async function onConnect() {
  const btn = $("connect");
  btn.disabled = true;
  $("log").textContent = "";
  setStage("working");

  try {
    session = await connectAndIdentify(log);
    renderIdentity(session);
    setStage("identified");
  } catch (err) {
    if (/No port selected|cancell?ed/i.test(err.message ?? "")) setStage("idle");
    else showFailure(err);
  } finally {
    btn.disabled = false;
  }
}

async function onDisconnect() {
  if (session) await release(session);
  session = null;
  chosen = null;
  setStage("idle");
}

/**
 * Say why this browser cannot flash, on the welcome screen rather than instead
 * of it.
 *
 * The previous version replaced the whole page with a dead end, so someone
 * arriving in Firefox from a forum link never found out what the thing they had
 * been sent to actually was. They still cannot flash from here - the Connect
 * button goes dead, because a button that opens a picker that cannot work is
 * worse than one that is plainly unavailable - but they can read what it does
 * and where to go to do it.
 */
function explainNoSerial() {
  const who = browserName();
  const mobile = isMobile();

  $("no-serial-title").textContent = mobile
    ? "A phone or tablet cannot flash a board"
    : `${who ?? "This browser"} cannot talk to a board`;

  const zip = '<a href="https://github.com/sijones/DiyBatteryBMS">release zip</a>';

  /* Three sentences, not one with the name swapped in, because the way out
     differs by case:
       - mobile: there is no way out on the device. Every browser on iOS is
         Safari underneath, so "install Chrome" is advice that cannot work.
       - Firefox: an add-on can supply the API, and this page will use it if it
         does - the check above is for the API, never for the browser name. So
         Firefox gets told that rather than being sent away.
       - anything else: another browser, or the zip. */
  $("no-serial-detail").innerHTML = mobile
    ? "Flashing over USB needs the Web Serial API, which no browser on iOS or Android implements. " +
      `Come back on a desktop with <b>Chrome</b>, <b>Edge</b> or <b>Opera</b>, or flash from the ${zip}.`
    : who === "Firefox"
      ? "Flashing over USB needs the Web Serial API, which Firefox does not ship. An add-on that adds " +
        "it works here — this page looks for the API, not for the browser, so it will use one if it is " +
        `installed. Otherwise open this page in <b>Chrome</b>, <b>Edge</b> or <b>Opera</b>, or flash from the ${zip}.`
      : `Flashing over USB needs the Web Serial API, which ${who ?? "this browser"} does not implement. ` +
        "Open this page in <b>Chrome</b>, <b>Edge</b> or <b>Opera</b> on a desktop, or flash from the " +
        `${zip} with the upgrade script.`;

  $("no-serial").hidden = false;
  // The fine print underneath says the same thing in general terms. With the
  // banner up it is repetition, and it is the weaker of the two.
  $("serial-note").hidden = true;
  $("connect").disabled = true;
  $("connect").title = "This browser has no Web Serial support";
}

/** Undo explainNoSerial(), for when the API turns up after all. */
function clearNoSerial() {
  $("no-serial").hidden = true;
  $("serial-note").hidden = false;
  $("connect").disabled = false;
  $("connect").removeAttribute("title");
}

/**
 * Keep looking for a little while, because support can ARRIVE.
 *
 * A Firefox add-on that supplies navigator.serial does it from a content
 * script, and nothing guarantees that has run by the time this page boots. The
 * failure that would cause is a nasty one to report: the add-on is installed,
 * the port picker works, and the page is sitting there insisting Firefox cannot
 * do this - which reads as the page being broken rather than early.
 *
 * Five seconds of half-second checks, then it stops. A browser that has not
 * grown the API by then is not going to.
 */
function watchForLateSerial() {
  let tries = 0;
  const timer = setInterval(() => {
    if (webSerialSupported()) {
      clearNoSerial();
      clearInterval(timer);
    } else if (++tries >= 10) {
      clearInterval(timer);
    }
  }, 500);
}

function boot() {
  /* Everything is wired up whether or not there is Web Serial, and the Connect
     button is switched off afterwards if there is none. The alternative -
     returning early - left a page where every other control was dead too, and
     gave nothing to re-enable if support appeared a moment later. */
  $("connect").addEventListener("click", onConnect);
  $("disconnect").addEventListener("click", onDisconnect);
  $("retry").addEventListener("click", () => setStage("idle"));

  $("to-choose").addEventListener("click", async () => {
    setStage("choose");
    chosen = null;
    $("to-confirm").disabled = true;
    await loadChoices();
  });
  $("back-to-identified").addEventListener("click", () => setStage("identified"));
  $("restore-nvs").addEventListener("click", () => {
    $("restore-status").hidden = true;
    $("restore-nvs-file").click();
  });
  $("restore-nvs-file").addEventListener("change", (e) => onRestoreNvsFile(e.target));
  $("to-confirm").addEventListener("click", () => {
    renderConfirm();
    setStage("confirm");
  });
  $("back-to-choose").addEventListener("click", () => setStage("choose"));
  $("do-backup").addEventListener("click", onBackup);
  $("do-flash").addEventListener("click", onFlash);
  for (const ch of ["release", "beta"]) {
    $(`tab-${ch}`).addEventListener("click", async () => {
      const list = versionsIn(releaseIndex, ch);
      if (!list.length) return;
      selected = { channel: ch, version: list[0].version };
      chosen = null;
      $("to-confirm").disabled = true;
      renderChannelPicker();
      await renderBuildsFor(selected.channel, selected.version);
    });
  }
  $("version").addEventListener("change", async (e) => {
    selected = { ...selected, version: e.target.value };
    chosen = null;
    $("to-confirm").disabled = true;
    renderChannelPicker();
    await renderBuildsFor(selected.channel, selected.version);
  });

  $("to-wifi").addEventListener("click", openWifiStep);
  $("wifi-rescan").addEventListener("click", doScan);
  $("wifi-connect").addEventListener("click", onWifiConnect);
  $("wifi-skip").addEventListener("click", async () => {
    await con?.close();
    con = null;
    setStage("done");
  });
  // Typing a name by hand overrides the picked row, so the button has to
  // follow whichever the person actually used last.
  $("wifi-manual").addEventListener("input", (e) => {
    if (e.target.value.trim()) $("wifi-connect").disabled = false;
    else $("wifi-connect").disabled = !pickedNetwork;
  });

  $("keep-settings").addEventListener("change", updateFlashButton);
  $("erase-settings").addEventListener("change", updateFlashButton);
  $("want-backup").addEventListener("change", (e) =>
    localStorage.setItem("backup", e.target.checked ? "on" : "off"),
  );
  $("flash-another").addEventListener("click", onDisconnect);

  setStage("idle");

  // Not awaited: the page is usable while it is in flight, and a slow or
  // unreachable index must not hold up the Connect button.
  showLatest();
  showStats();

  if (!webSerialSupported()) {
    explainNoSerial();
    watchForLateSerial();
  }
}

document.addEventListener("DOMContentLoaded", boot);
