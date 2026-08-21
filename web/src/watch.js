/* Watching the board come back up after a flash.
 *
 * This exists so the page never has to GUESS what state the board is in. An
 * upgrade that preserved nvs kept the WiFi credentials with it, so asking for
 * them again would be wrong - but "preserved nvs" is not the same as "has
 * credentials": a board that was never configured has an nvs full of defaults.
 *
 * Rather than infer, read what the firmware says about itself:
 *
 *   [net] connected to 'J-iot'
 *   [net] IP 10.10.11.25   http://10.10.11.25/
 *   [net] no WiFi configured - access point 'diy-battery-bms' at 192.168.4.1
 *
 * It reads the port DIRECTLY rather than through esptool-js's transport. Two
 * reasons, both learned the hard way:
 *
 *   - After a flash the transport's reader is in a state this code does not
 *     own, and a generator wrapped around it can stall with no way to unstick.
 *   - Racing a read against a timer does not cancel the read. The pending call
 *     keeps the lock, so the cleanup that follows blocks too and the page hangs
 *     exactly where it did before. reader.cancel() is the primitive that
 *     actually unblocks a pending read - a timer alone never will.
 */

const CONNECTED = /^\[net\]\s+connected to '(.*)'\s*$/gm;
const IP = /^\[net\]\s+IP\s+(\d+\.\d+\.\d+\.\d+)/gm;
const AP = /^\[net\]\s+no WiFi configured/m;
const BANNER = /DIY Battery BMS\s+(\S+)/;

/**
 * An address of 0.0.0.0 is the board saying "associated, no lease yet".
 *
 * The firmware announces as soon as WiFi reports connected, which can be
 * before DHCP has answered - so the first IP line is often 0.0.0.0 and a real
 * address follows a second later. Taking the first one and stopping there
 * reported success with an address nobody can open.
 */
const isRealIp = (ip) => Boolean(ip) && ip !== "0.0.0.0";

/** Last capture of a global regex, or null. Later lines supersede earlier ones. */
function lastMatch(regex, text) {
  regex.lastIndex = 0;
  let found = null;
  for (const m of text.matchAll(regex)) found = m[1];
  return found;
}

/** Never await a serial operation without a bound - any of them can stall. */
function withTimeout(promise, ms) {
  return Promise.race([promise, new Promise((resolve) => setTimeout(resolve, ms))]);
}

async function closeQuietly(port) {
  try {
    await withTimeout(port.close(), 3000);
  } catch {
    /* already closed, or closing on a port that has gone */
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * Reset the board into its application, on a port we own.
 *
 * esptool-js's own HardReset only RELEASES RTS - it assumes the connect
 * sequence left it asserted. That assumption does not survive closing and
 * reopening the port, and worse, the browser's own DTR/RTS defaults on open
 * can leave the board held in reset or sitting in the ROM bootloader. Either
 * way it prints nothing, which is indistinguishable from a board that is fine
 * but quiet.
 *
 * So drive the lines explicitly, after we are already listening, and catch the
 * boot from its first byte:
 *
 *   RTS asserted  -> EN low, chip held in reset
 *   RTS released  -> EN high, chip boots
 *
 * DTR stays LOW throughout: on the standard auto-reset circuit DTR pulls GPIO0
 * down, and a board that boots with GPIO0 low lands in the bootloader instead
 * of running the firmware we just wrote.
 */
async function resetIntoApp(port) {
  try {
    await port.setSignals({ dataTerminalReady: false, requestToSend: true });
    await sleep(120);
    await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    return true;
  } catch {
    // Some ports do not implement setSignals; the board may already be running
    return false;
  }
}

/**
 * @param {SerialPort} port    the raw Web Serial port, already released by esptool-js
 * @param {(text: string) => void} onText  streamed output, for the log view
 * @param {number} ms          how long to listen at most
 */
export async function watchBoot(port, onText, ms = 20_000) {
  let text = "";

  /* The caller must have released the transport first. If it has not, the port
     is still open with its streams locked and open() below throws - which used
     to be swallowed and reported as "the board said nothing", sending everyone
     hunting for a hardware fault that was not there. Say what actually
     happened instead. */
  if (!port.readable) {
    await closeQuietly(port);
    try {
      await withTimeout(port.open({ baudRate: 115200 }), 5000);
    } catch (e) {
      onText?.(`\n[flasher] could not reopen the port to listen: ${e.message}\n`);
      return summarise("", false, e.message);
    }
  }

  if (!port.readable) {
    onText?.("\n[flasher] the port opened but has no readable stream\n");
    return summarise("", false, "port not readable");
  }

  const reader = port.readable.getReader();
  const decoder = new TextDecoder();

  // Reset AFTER taking the reader, so nothing is missed between the two.
  if (!(await resetIntoApp(port))) {
    onText?.("\n[flasher] could not drive the reset lines — waiting for output anyway\n");
  }

  // The deadline cancels the READER, which is what makes a pending read return.
  const deadline = setTimeout(() => {
    reader.cancel().catch(() => {});
  }, ms);

  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break; // cancelled, or the device went away
      if (!value?.length) continue;

      const chunk = decoder.decode(value, { stream: true });
      text += chunk;
      onText?.(chunk);

      /* Stop as soon as the board has said something conclusive - but 0.0.0.0
         is not conclusive, it is the board still waiting on DHCP. Keep
         listening until a real address turns up or the timeout ends it. */
      if (isRealIp(lastMatch(IP, text))) break;
      if (AP.test(text)) break;
    }
  } catch {
    /* a board that reboots mid-read throws here; whatever arrived still counts */
  } finally {
    clearTimeout(deadline);
    try {
      await withTimeout(reader.cancel(), 1500);
    } catch {
      /* nothing useful left to do */
    }
    try {
      reader.releaseLock();
    } catch {
      /* ditto */
    }
    await closeQuietly(port);
  }

  return summarise(text, true, null);
}

function summarise(text, opened, problem) {
  // Empty string, not just null: the board can print connected to '' while the
  // driver is still filling that in, and an empty name rendered as "on ."
  const ssidRaw = lastMatch(CONNECTED, text);
  const ssid = ssidRaw && ssidRaw.trim() ? ssidRaw : null;

  const ipRaw = lastMatch(IP, text);
  const ip = isRealIp(ipRaw) ? ipRaw : null;

  return {
    text,
    problem, // non-null when we never got to listen at all
    version: BANNER.exec(text)?.[1] ?? null,
    ssid,
    ip,
    /* Associated but with no lease is not "on the network" in any sense that
       helps someone trying to open its page. */
    onNetwork: Boolean(ip),
    joinedWithoutAddress: Boolean(ipRaw && !ip),
    needsWifi: AP.test(text),
    /* Distinguish "it told us it has no credentials" from "it never said
       anything". A board whose console is on a different USB endpoint than the
       one we flashed over is silent here, and that is not the same as
       unconfigured - claiming otherwise would be a guess. */
    heardFrom: opened && text.trim().length > 0,
  };
}
