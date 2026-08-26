/* Talking to the firmware's own serial console.
 *
 * SerialSetup.h already accepts exactly what is needed - scan, ssid, ssidhex,
 * pass, host, connect - so putting a board on WiFi needs no new firmware
 * protocol, just a page that types the same lines a person would.
 *
 * Holds the port open across several commands, unlike watch.js which opens,
 * listens once and closes. Credentials are typed, answered and then the board
 * restarts, which is three round trips rather than one.
 */

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class Console {
  constructor(port) {
    this.port = port;
    this.reader = null;
    this.writer = null;
    this.buffer = "";
    this.onText = null;
  }

  async open(baudRate = 115_200) {
    if (!this.port.readable) {
      try {
        await this.port.close();
      } catch {
        /* was not open */
      }
      await this.port.open({ baudRate });
    }
    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
    this._pump();
  }

  /** Read continuously into a buffer so commands can await patterns in it. */
  async _pump() {
    const decoder = new TextDecoder();
    try {
      while (true) {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (!value?.length) continue;
        const chunk = decoder.decode(value, { stream: true });
        this.buffer += chunk;
        this.onText?.(chunk);
      }
    } catch {
      /* closed underneath us, which is how close() stops this loop */
    }
  }

  async send(line) {
    // The firmware reads one command per line and takes the rest of the line
    // as the value, so a passphrase with spaces in it arrives intact.
    await this.writer.write(new TextEncoder().encode(line + "\r\n"));
  }

  /** Wait for a pattern in everything received so far. Resolves null on timeout. */
  async expect(regex, ms) {
    const deadline = Date.now() + ms;
    while (Date.now() < deadline) {
      const m = regex.exec(this.buffer);
      if (m) return m;
      await sleep(60);
    }
    return null;
  }

  clear() {
    this.buffer = "";
  }

  async close() {
    try {
      await this.reader?.cancel();
    } catch {
      /* already gone */
    }
    try {
      this.reader?.releaseLock();
    } catch {
      /* ditto */
    }
    try {
      this.writer?.releaseLock();
    } catch {
      /* ditto */
    }
    try {
      await this.port.close();
    } catch {
      /* ditto */
    }
  }
}

/**
 * Wait for the console to prove it is actually listening, not just open.
 *
 * Opening the port resets this board - confirmed directly: a fresh open logs
 * `rst:0x15 (USB_UART_CHIP_RESET)` before anything else - and the reboot that
 * follows takes several seconds of setup() running (every stored NVS key
 * gets read, the CAN pin check runs, the access point comes up) before
 * SerialSetup's serialSetupLoop() ever runs once. Serial.begin() itself only
 * happens partway through that, so anything sent before this resolves is not
 * delayed, it is gone - there is nothing running yet to have buffered it.
 * serialBanner() prints the one line that only appears once the console loop
 * is actually reading input, so waiting for it is what turns "send a command
 * right after open()" from a race into something safe to rely on.
 */
export async function waitReady(con, ms = 15_000) {
  return con.expect(/Type 'help' for WiFi setup/, ms);
}

/**
 * Ask the board what networks it can see.
 *
 * The scan is synchronous in the firmware and takes a couple of seconds, so
 * the wait is real rather than defensive.
 */
export async function scanNetworks(con, ms = 15_000) {
  con.clear();
  await con.send("scan");
  // Either a count line followed by rows, or an outright failure
  const done = await con.expect(/\[scan\]\s+(\d+\s+network|no networks found|scan failed)/, ms);
  if (!done) return { text: con.buffer, timedOut: true };
  // Rows follow the count; give them a moment to arrive
  await sleep(400);
  return { text: con.buffer, timedOut: false };
}

/**
 * Send credentials and watch the board rejoin.
 *
 * `connect` makes the firmware save and restart, so the answer arrives after a
 * reboot rather than immediately.
 */
export async function sendCredentials(con, lines, ms = 30_000) {
  con.clear();
  for (const line of lines) {
    await con.send(line);
    // Each command answers on the next line; a short gap keeps the exchange
    // readable in the log and avoids racing the firmware's line parser.
    await sleep(150);
  }

  /* Not 0.0.0.0: the firmware announces as soon as WiFi says connected, which
     can be before DHCP has answered, so the first address printed is often a
     placeholder and the real one follows. Matching it would report success
     with an address nobody can open. */
  const ip = await con.expect(/^\[net\]\s+IP\s+(?!0\.0\.0\.0)(\d+\.\d+\.\d+\.\d+)/m, ms);
  if (ip) return { ok: true, ip: ip[1], text: con.buffer };

  const ap = /\[net\]\s+no WiFi configured/.test(con.buffer);
  return { ok: false, ip: null, apMode: ap, text: con.buffer };
}
