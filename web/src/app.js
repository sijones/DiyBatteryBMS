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
} from "./board.js";
import { fmtHex, fmtSize, nvsVerdict } from "./partitions.js";
import {
  loadReleases,
  loadManifest,
  compatibility,
  rank,
  defaultSelection,
  versionsIn,
  buildsFor,
} from "./builds.js";
import { backupNvs, flashBuild, restart } from "./flash.js";
import { watchBoot } from "./watch.js";
import { Console, scanNetworks, sendCredentials } from "./console.js";
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

  list.innerHTML = choices
    .map(
      (c, idx) => `
      <label class="choice${c.compat.ok ? "" : " disabled"}">
        <input type="radio" name="build" value="${idx}"${c.compat.ok ? "" : " disabled"}>
        <div class="choice-body">
          <div class="choice-head">
            <span class="choice-name">${esc(c.manifest.board)}</span>
            <span class="mono dim choice-env">${esc(c.manifest.env)}</span>
          </div>
          <div class="choice-detail${c.compat.ok ? "" : " bad"}">${esc(c.compat.ok ? c.manifest.detail : c.compat.reason)}</div>
        </div>
      </label>`,
    )
    .join("");

  for (const input of list.querySelectorAll("input[name=build]")) {
    input.addEventListener("change", () => {
      chosen = choices[Number(input.value)];
      $("to-confirm").disabled = false;
    });
  }
}

/* ----------------------------------------------------------------- confirm */

function renderConfirm() {
  const m = chosen.manifest;
  verdict = nvsVerdict(session.nvs, m.nvs);
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

  $("chosen-name").textContent = `${m.board} · ${m.version}`;

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

async function openWifiStep() {
  setStage("wifi");
  $("wifi-status").textContent = "Opening the console…";
  $("wifi-list").innerHTML = "";
  pickedNetwork = null;
  $("wifi-connect").disabled = true;

  con = new Console(session.port);
  con.onText = (chunk) => {
    $("wifi-log").textContent += chunk;
    $("wifi-log").scrollTop = $("wifi-log").scrollHeight;
  };

  try {
    await con.open();
    await doScan();
  } catch (e) {
    $("wifi-status").textContent = `Could not open the console: ${e.message}`;
  }
}

async function doScan() {
  $("wifi-status").textContent = "Scanning… (a few seconds)";
  $("wifi-rescan").disabled = true;

  const { text, timedOut } = await scanNetworks(con);
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

  $("wifi-status").textContent = res.apMode
    ? "The board did not join — it came back up on its own access point. Check the passphrase."
    : "No address came back within 30 seconds. It may still be trying; check the log.";
  $("wifi-connect").disabled = false;
  // Reopen so another attempt can be made without starting over
  con = new Console(session.port);
  con.onText = (chunk) => { $("wifi-log").textContent += chunk; };
  try { await con.open(); } catch { /* the user can go back and retry */ }
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

async function onFlash() {
  const m = chosen.manifest;
  const eraseAll = eraseChosen();
  const wantBackup = session.nvs && $("want-backup").checked;

  setStage("flashing");
  $("flash-what").textContent = `${m.board} · ${m.version}`;
  progress(0, 1);

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
  } catch (err) {
    showFailure(err);
  }
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

  if (!webSerialSupported()) {
    explainNoSerial();
    watchForLateSerial();
  }
}

document.addEventListener("DOMContentLoaded", boot);
