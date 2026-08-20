#pragma once

/* Serial console, for the one job that has to happen before the network does.

   A freshly flashed board has no WiFi credentials, so the only way in was to
   leave your own network, join the board's access point, configure it at
   192.168.4.1, and rejoin. That is a lot of ceremony while a USB cable is
   already plugged in and sitting idle.

   This deliberately handles WiFi and nothing else. Everything else is already
   settable over the WebSocket, and a second settings surface would be a second
   thing to keep in step with the first - so once the board is on the network,
   this gets out of the way and the existing JSON path does the rest.

   The other half is telling you the address. The board logs "WiFi connected,
   IP: ..." through WS_LOG_I, which at CORE_DEBUG_LEVEL=1 reaches the web log
   and syslog but never the serial line - so the device knew its own address and
   would not say it on the one channel you were already attached to. Finding a
   freshly configured board meant sweeping the subnet. The announcements below
   go out with Serial.print directly, so they do not disappear at low debug
   levels the way a log macro would. */

#include <Arduino.h>
#include <WiFi.h>

#define SERIAL_CMD_MAX 160      // SSID 32 + passphrase 63 + a keyword and spaces

static char     _serCmd[SERIAL_CMD_MAX + 1];
static uint16_t _serLen = 0;
static bool     _serOverflow = false;
static bool     _serLastConnected = false;
static bool     _serAnnounced = false;

static void serialBanner() {
  Serial.println();
  Serial.printf("DIY Battery BMS %s\r\n", FW_VERSION);
  Serial.println("Type 'help' for WiFi setup over this cable.");
}

// What the board is on the network, printed the moment it becomes true.
static void serialAnnounceNetwork() {
  if (WiFi.getMode() == WIFI_MODE_AP) {
    Serial.printf("[net] no WiFi configured - access point '%s' at %s\r\n",
                  wifiManager.GetWifiHostName().c_str(),
                  WiFi.softAPIP().toString().c_str());
    Serial.println("[net] set credentials here with: ssid <name>  pass <secret>  connect");
    return;
  }
  Serial.printf("[net] connected to '%s'\r\n", wifiManager.GetWifiSSID().c_str());
  Serial.printf("[net] IP %s   http://%s/\r\n",
                WiFi.localIP().toString().c_str(),
                WiFi.localIP().toString().c_str());
  Serial.printf("[net] hostname '%s'   RSSI %d dBm\r\n",
                wifiManager.GetWifiHostName().c_str(), (int)WiFi.RSSI());
}

static void serialHelp() {
  Serial.println();
  Serial.println("WiFi setup:");
  Serial.println("  scan              list the networks in range");
  Serial.println("  ssid <name>       network name, rest of the line, spaces kept");
  Serial.println("  ssidhex <hex>     network name as exact bytes, for a name that");
  Serial.println("                    is not UTF-8 (some routers in JP/CN/KR/EU)");
  Serial.println("  pass <secret>     passphrase, rest of the line");
  Serial.println("  host <name>       hostname, also the access point name");
  Serial.println("  save              write what has been entered to flash");
  Serial.println("  connect           save, then restart and join the network");
  Serial.println("  status            what is stored and where the board is");
  Serial.println("  ip                the address, once connected");
  Serial.println("  reboot            restart");
  Serial.println();
  Serial.println("Everything else is set in the web UI once the board is on the");
  Serial.println("network - this console is only here to get it there.");
}

static void serialStatus() {
  Serial.println();
  Serial.printf("  firmware   %s\r\n", FW_VERSION);
  Serial.printf("  ssid       '%s'\r\n", wifiManager.GetWifiSSID().c_str());
  // Never the passphrase itself: this is a console, and it gets logged, pasted
  // into issues and photographed. Whether one is stored is all anyone needs.
  Serial.printf("  passphrase %s\r\n",
                wifiManager.GetWifiPass().length() ? "set" : "(not set)");
  Serial.printf("  hostname   '%s'\r\n", wifiManager.GetWifiHostName().c_str());
  if (WiFi.getMode() == WIFI_MODE_STA) {
    Serial.printf("  wifi       %s\r\n", WiFi.isConnected() ? "connected" : "joining");
    if (WiFi.isConnected()) Serial.printf("  ip         %s\r\n", WiFi.localIP().toString().c_str());
  }
  else {
    Serial.printf("  wifi       access point at %s\r\n", WiFi.softAPIP().toString().c_str());
  }
  Serial.println();
}

/* One command per line. The value is the rest of the line rather than a token,
   so an SSID or passphrase containing spaces arrives intact - and they do. */
static void serialHandle(char* line) {
  while (*line == ' ') line++;
  if (*line == '\0') return;

  char* arg = strchr(line, ' ');
  if (arg) { *arg = '\0'; arg++; while (*arg == ' ') arg++; }

  if      (!strcasecmp(line, "help") || !strcmp(line, "?")) serialHelp();
  else if (!strcasecmp(line, "status")) serialStatus();
  else if (!strcasecmp(line, "ip")) {
    if (WiFi.getMode() == WIFI_MODE_STA && WiFi.isConnected())
      Serial.printf("%s\r\n", WiFi.localIP().toString().c_str());
    else
      Serial.println("not connected");
  }
  else if (!strcasecmp(line, "scan")) {
    /* Synchronous on purpose. Everything else on this console answers on the
       line after the command, and a scan that returned later would arrive in
       the middle of whatever was typed next. It blocks the loop for a couple of
       seconds; nothing here is time-critical while the board has no network.

       Printed widest-useful-first: signal, whether it needs a passphrase, then
       the name last because it is the only field that can contain spaces. A
       reader - human or otherwise - can split the first two off the front and
       take the rest of the line as the name, the same rule 'ssid' itself uses. */
    Serial.println("[scan] scanning...");
    Serial.flush();
    const int n = WiFi.scanNetworks();
    if (n <= 0) {
      Serial.println(n == 0 ? "[scan] no networks found" : "[scan] scan failed");
      WiFi.scanDelete();
      return;
    }
    Serial.printf("[scan] %d network(s)\r\n", n);
    for (int i = 0; i < n; i++) {
      const String nm = WiFi.SSID(i);
      /* A name that is not valid UTF-8 cannot be typed back in, so give the
         hex form alongside it - that is exactly what 'ssidhex' takes. Hidden
         networks scan with an empty name and have to be typed from memory. */
      if (nm.length() == 0) {
        Serial.printf("[scan] %4d  %-4s  (hidden)\r\n",
                      WiFi.RSSI(i),
                      WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "lock");
        continue;
      }
      Serial.printf("[scan] %4d  %-4s  %s\r\n",
                    WiFi.RSSI(i),
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "lock",
                    nm.c_str());
      if (!isValidUTF8(nm)) {
        Serial.print("[scan]             hex ");
        for (unsigned k = 0; k < nm.length(); k++) Serial.printf("%02x", (uint8_t)nm[k]);
        Serial.println("   <- use 'ssidhex' for this one");
      }
    }
    // The results hold heap until dropped, and this board has little to spare
    WiFi.scanDelete();
  }
  else if (!strcasecmp(line, "ssid")) {
    if (!arg) { Serial.println("usage: ssid <name>"); return; }
    wifiManager.SetWifiSSID(String(arg));
    Serial.printf("[ok] ssid '%s'\r\n", arg);
  }
  else if (!strcasecmp(line, "ssidhex")) {
    if (!arg) { Serial.println("usage: ssidhex <hex>"); return; }
    String bytes = hexToBytes(String(arg));
    if (bytes.length() == 0) { Serial.println("[err] not valid hex"); return; }
    wifiManager.SetWifiSSID(bytes);
    Serial.printf("[ok] ssid set from %u hex byte(s)\r\n", (unsigned)bytes.length());
  }
  else if (!strcasecmp(line, "pass")) {
    if (!arg) { Serial.println("usage: pass <secret>"); return; }
    wifiManager.SetWifiPass(String(arg));
    Serial.printf("[ok] passphrase set (%u characters)\r\n", (unsigned)strlen(arg));
  }
  else if (!strcasecmp(line, "host")) {
    if (!arg) { Serial.println("usage: host <name>"); return; }
    wifiManager.SetWifiHostName(String(arg));
    Serial.printf("[ok] hostname '%s'\r\n", arg);
  }
  else if (!strcasecmp(line, "save")) {
    // The setters above already write NVS, so this only confirms
    Serial.println("[ok] stored - 'connect' to restart and join");
  }
  else if (!strcasecmp(line, "connect") || !strcasecmp(line, "reboot")) {
    if (!strcasecmp(line, "connect") && wifiManager.GetWifiSSID().length() < 2) {
      Serial.println("[err] no ssid set");
      return;
    }
    /* Restarting rather than switching mode in place. Bringing the station up
       while the access point, its DNS responder and the web server are already
       running has too many half-states to be worth it for a one-off, and the
       boot path already does exactly the right thing with stored credentials.
       The address is printed on the way back up. */
    Serial.println("[ok] restarting - the IP will be printed when it joins");
    Serial.flush();
    delay(150);
    ESP.restart();
  }
  else {
    Serial.printf("[err] unknown command '%s' - try 'help'\r\n", line);
  }
}

static void serialSetupBegin() {
  serialBanner();
}

// Called every pass of the main loop. Reads at most a line's worth per pass.
static void serialSetupLoop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      _serCmd[_serLen] = '\0';
      if (_serOverflow) {
        Serial.println("[err] line too long, ignored");
        _serOverflow = false;
      }
      else if (_serLen > 0) {
        serialHandle(_serCmd);
      }
      _serLen = 0;
      continue;
    }
    if (_serLen < SERIAL_CMD_MAX) _serCmd[_serLen++] = c;
    else _serOverflow = true;      // keep draining, report once at end of line
  }

  /* Announce the address as soon as there is one, and again if it comes back.
     "As soon as there is one" has to include actually having one: isConnected()
     goes true at association, which is before DHCP has answered, so announcing
     on that alone printed

       [net] IP 0.0.0.0

     - true, useless to a reader, and taken at face value by anything parsing
     this output. Waiting for a real address costs a second and makes the line
     mean what it says. A static configuration has its address immediately and
     is unaffected. */
  const bool sta = (WiFi.getMode() == WIFI_MODE_STA);
  const bool up = sta && WiFi.isConnected() && WiFi.localIP() != IPAddress(0, 0, 0, 0);
  if (up != _serLastConnected) {
    _serLastConnected = up;
    if (up) serialAnnounceNetwork();
    else    Serial.println("[net] WiFi lost, reconnecting");
  }
  // AP mode never "connects", so say where it is once, after it has settled
  if (!sta && !_serAnnounced && millis() > 4000) {
    _serAnnounced = true;
    serialAnnounceNetwork();
  }
}
