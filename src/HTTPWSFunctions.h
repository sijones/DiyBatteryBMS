#pragma once

#include <ArduinoJson.h>
#include <WiFi.h>
#include <Update.h>
#include <esp_ota_ops.h>   // running partition size, for the legacy-layout warning
#include <esp_heap_caps.h> // largest free block, checked before the big broadcast
#include <memory>
#include <vector>
#include "VictronBLE.h"
#include "MqttShunt.h"

// Defined in main.cpp. Cached at boot so the status JSON does not read NVS on
// every broadcast - see the note where the BLE block is built.
extern uint8_t shuntSource;
// Which source takes over while the primary is stale, or SHUNT_FALLBACK_NONE
extern uint8_t fallbackSource;
// Which link last actually applied a reading, as opposed to which is configured
extern uint8_t activeShuntLink;
#include "config.h"
#include "embedded_html.h"
#include "Syslog.h"
#include "Schedule.h"
#include "RemoteOverride.h"
#include "GPIOForbidden.h"

// Every GPIO role the web UI lets the user pin-assign. Kept in one place so a
// newly assigned pin can be checked against every other role, not just its own.
struct PinRole { const char* field; const char* prefKey; };
static const PinRole PIN_ROLES[] = {
  {"fanpin",       ccFanPin},
  {"onewirepin",   ccOneWirePin},
  {"canbuscspin",  ccCanCSPin},
  {"victronrxpin", ccVictronRX},
  {"victrontxpin", ccVictronTX},
  {"can_rx_pin",   ccCAN_RX_PIN},
  {"can_tx_pin",   ccCAN_TX_PIN},
  {"can_en_pin",   ccCAN_EN_PIN},
};

// Field name of another configured role already assigned to `pin`, or nullptr
// if `pin` is free. `excludeField` is the role being written, so a pin is not
// reported as conflicting with its own previously stored value.
static inline const char* FindPinConflict(uint8_t pin, const char* excludeField) {
  if (pin == 0) return nullptr;
  for (const PinRole &role : PIN_ROLES) {
    if (strcmp(role.field, excludeField) == 0) continue;
    if (pref.getUInt8(role.prefKey, 0) == pin) return role.field;
  }
  return nullptr;
}

volatile bool otaInProgress = false;

// Broadcasts skipped for want of a big enough contiguous block. Non-zero means
// the board is running too close to the edge - see notifyWSClients().
static uint32_t wsSkippedLowHeap = 0;

/* Two settings the full status payload reports, held in RAM rather than read
   back out of NVS every time it is built.

   Each getString() is a flash read that allocates a String, and both were being
   done on every full broadcast - visible on the serial line as a pair of

     nvs_get_str len fail: TimeZone NOT_FOUND
     nvs_get_str len fail: SyslogSrv NOT_FOUND

   every fifteen seconds, for settings that change perhaps twice in the life of
   an install. Same reasoning as the VBLEMac lookup further down: it is not the
   size of any one allocation that hurts, it is repeating it forever on a board
   where the largest free block is the thing in short supply.

   Refreshed by applyTimeZone() and applySyslogConfig(), which are already the
   single funnel for "this setting changed" and are both called at boot. */
static String g_tzCached;
static String g_syslogServerCached;

// Forward declarations for MQTT temperature subscriptions (defined in mqttFunctions.h)
extern String sMqttBattTopic;
extern String sMqttInvTopic;
// The four shunt-source topics, same RAM-copy arrangement as the two above
extern String sMqttShuntSOC;
extern String sMqttShuntVolt;
extern String sMqttShuntCurr;
extern String sMqttShuntTemp;
void mqttResubscribeTemp();
void mqttRunCopyTest();   // diagnostic, see mqttFunctions.h
/* Re-publish HA discovery (defined in mqttFunctions.h) so number-control limits
   track config changes. force = true says the configs are genuinely stale and
   must go out now; an MQTT reconnect passes false and is answered with nothing,
   because the configs are retained and the broker still has them. */
void publishHADiscovery(bool force = false);

/* Log buffer for web UI.

   This is the largest single object in the firmware's static RAM, so its size
   is a direct tax on the heap everything else runs from. At 100 entries of
   201+10+4 bytes it was 21,600 bytes, and a second 50-entry copy for replaying
   to a browser added 10,800 more - together a third of all static RAM, and more
   than the entire cost of running Bluetooth. On a 4MB ESP32 with BLE enabled
   that was the difference between ~11KB free heap (which aborts inside
   AsyncTCP the moment an allocation fails) and a working device.

   144 characters holds every WS_LOG_* line this firmware actually produces -
   the longest are the CC-CV and schedule lines around 120 - and the level is
   one of four values, so it is a byte rather than a ten-character string. */
#define LOG_BUFFER_SIZE 60
#define LOG_SEND_MAX 50      // how many of those a GetLogs() replay hands over
#define LOG_MSG_MAX 143      // + null terminator

enum LogLevel : uint8_t { LVL_DEBUG = 0, LVL_INFO, LVL_WARNING, LVL_ERROR };

static const char* const LOG_LEVEL_NAMES[] = { "debug", "info", "warning", "error" };

static inline uint8_t logLevelCode(const char* level) {
  if (!level) return LVL_INFO;
  switch (level[0]) {          // first letter is unique across the four
    case 'd': return LVL_DEBUG;
    case 'w': return LVL_WARNING;
    case 'e': return LVL_ERROR;
    default:  return LVL_INFO;
  }
}
static inline const char* logLevelName(uint8_t code) {
  return LOG_LEVEL_NAMES[code < 4 ? code : LVL_INFO];
}

struct LogEntry {
  char message[LOG_MSG_MAX + 1];
  uint8_t level;              // LogLevel
  unsigned long timestamp;
};
LogEntry logBuffer[LOG_BUFFER_SIZE];
int logBufferIndex = 0;
portMUX_TYPE logMutex = portMUX_INITIALIZER_UNLOCKED;

// WiFi scan state tracking
int lastWifiScanCount = -2;  // -2 = no scan in progress, -1 = scan in progress, >=0 = completed with count
portMUX_TYPE wifiScanMutex = portMUX_INITIALIZER_UNLOCKED;
bool wifiScanRequested = false;  // Track if scan was requested to trigger background scan

/* ---------- SSID transport ----------

   802.11 says an SSID is up to 32 octets with no encoding attached, and routers
   in Japan, China, Korea and parts of Europe still broadcast names in Shift_JIS,
   GBK, EUC-KR or Latin-1. Those bytes cannot travel as JSON text: the browser
   decodes our WebSocket frames as UTF-8, so it would hand back a name full of
   U+FFFD and WiFi.begin() would look for a network nobody is advertising.

   So every SSID crosses the wire twice - "ssid" is a sanitised copy for the
   dropdown to show, "ssidhex" is the exact bytes. The browser picks a network by
   its hex, and that is what comes back to be stored, byte for byte. */
inline String bytesToHex(const String& s) {
  static const char* digits = "0123456789abcdef";
  const uint8_t* p = (const uint8_t*)s.c_str();
  String out;
  out.reserve(s.length() * 2);
  for (size_t i = 0; i < s.length(); i++) {
    out += digits[p[i] >> 4];
    out += digits[p[i] & 0x0F];
  }
  return out;
}

// Empty on anything that is not clean, even-length hex - the caller then falls
// back to the plain text field rather than storing half a name.
inline String hexToBytes(const String& hex) {
  const size_t len = hex.length();
  if (len == 0 || (len & 1)) return String();
  String out;
  out.reserve(len / 2);
  for (size_t i = 0; i < len; i += 2) {
    uint8_t b = 0;
    for (size_t j = 0; j < 2; j++) {
      const char c = hex[i + j];
      b <<= 4;
      if      (c >= '0' && c <= '9') b |= (c - '0');
      else if (c >= 'a' && c <= 'f') b |= (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') b |= (c - 'A' + 10);
      else return String();
      }
    if (b == 0) return String();   // a NUL would truncate the String it lands in
    out += (char)b;
  }
  return out;
}

/* Broadcast to every client that can take it, skipping only the ones that
   cannot - never withholding from everybody because of one.

   ws.textAll() was guarded by ws.availableForWriteAll(), which is
     none_of(clients, queueIsFull)
   so a SINGLE client with a full queue silenced the broadcast to all of them. A
   backgrounded tab, a laptop that went to sleep, a phone that walked out of
   range, or a script that died without closing its socket was enough: every
   browser then sat there with frozen values until that client was reaped, and
   reloading the page only helped because it dropped the reader's own stale
   connection. Sending per client keeps one bad reader from starving the rest.

   textAll() was always the right call - it builds ONE shared buffer and hands
   the same one to every client, and AsyncWebSocketClient::_queueMessage already
   skips a client that is not connected and discards (or closes) per client when
   that client's own queue is full. Only the gate in front of it was wrong. Do
   not "improve" this into a per-client loop calling c.text(payload): each of
   those calls copies the whole payload into its own buffer, so a 3KB status
   broadcast to four browsers allocates 12KB instead of 3KB, several times a
   second, on a board that aborts when an allocation fails. */
inline void wsBroadcast(const String& payload) {
  if (payload.length() == 0) return;
  ws.textAll(payload);
}

/* For the scan lists, which are built by string concatenation rather than
   ArduinoJson. An SSID is free to contain a quote or a backslash, and one of
   those used to be enough to make the whole response unparseable. */
inline String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c < 0x20) { char u[7]; snprintf(u, sizeof(u), "\\u%04x", c); out += u; }
    else out += c;
  }
  return out;
}

SyslogSender Syslog;
RemoteOverrideClass RemoteOverride;
#ifndef DISABLE_SCHEDULER
ChargeSchedule Schedule;

// Load the UI (repeating) schedule from NVS. MQTT windows are deliberately never
// persisted - see the note in Schedule.h.
void loadUiSchedule() {
  String js = pref.getString(ccSchedule, "");
  if (js.length() == 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, js)) { log_e("Stored schedule is not valid JSON"); return; }
  int n = Schedule.setUiFromJson(doc.as<JsonArrayConst>());
  log_d("Loaded %d scheduled window(s) from NVS", n);
}

void saveUiSchedule() {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  Schedule.uiToJson(a);
  String out;
  serializeJson(doc, out);
  pref.putString(ccSchedule, out);
}
#endif  // DISABLE_SCHEDULER

// Re-read syslog settings from preferences and push them into the sender.
// Called on boot and whenever any syslog field changes.
void applySyslogConfig() {
  g_syslogServerCached = pref.getString(ccSyslogServer, "");
  Syslog.configure(g_syslogServerCached.c_str(),
                   pref.getUInt16(ccSyslogPort, 514),
                   pref.getBool(ccSyslogEnabled, false),
                   wifiManager.GetWifiHostName().c_str());
}

// Make a log message safe to sit inside a JSON string. Quotes become apostrophes
// rather than \" - lossy, but it keeps the line short and readable, and it is what
// the UI has always displayed. Shared by the live and replayed paths so the two
// cannot drift: an unescaped replay emitted broken JSON, and the client parses
// every frame without a net.
String jsonEscapeLog(const char* message) {
  String msg = String(message).substring(0, LOG_MSG_MAX);  // Limit message length
  msg.replace("\\", "\\\\");
  msg.replace("\"", "'");
  msg.replace("\n", "\\n");
  msg.replace("\r", "\\r");
  msg.replace("\t", "\\t");
  return msg;
}

// Function to send log to WebSocket clients
void sendLogToWS(const char* message, const char* level) {
  // Every WS_LOG_* macro funnels through here, so syslog picks up all of them
  // without needing a hook at each of the ~96 call sites.
  Syslog.log(message, level);

  // Only send if WebSocket is initialized and has connected clients
  if(ws.count() > 0) {
    String json = "{\"log\":\"";
    json += jsonEscapeLog(message);
    json += "\",\"level\":\"";
    json += level;
    json += "\"}";
    wsBroadcast(json);
  }
  
  // Also store in circular buffer (strncpy is safe inside critical section - no heap alloc).
  // The level is mapped to a code out here; the critical section stays a copy.
  const uint8_t lvl = logLevelCode(level);
  taskENTER_CRITICAL(&logMutex);
  strncpy(logBuffer[logBufferIndex].message, message, LOG_MSG_MAX);
  logBuffer[logBufferIndex].message[LOG_MSG_MAX] = '\0';
  logBuffer[logBufferIndex].level = lvl;
  logBuffer[logBufferIndex].timestamp = millis();
  logBufferIndex = (logBufferIndex + 1) % LOG_BUFFER_SIZE;
  taskEXIT_CRITICAL(&logMutex);
}

// WS_LOG macros - shared with other translation units via WebLog.h
#include "WebLog.h"


#include <esp_sntp.h>

/*
   Clock sync tracking.

   The lwIP SNTP client polls on its own timer - CONFIG_LWIP_SNTP_UPDATE_DELAY,
   3 hours in this build - so nothing here drives the sync. What we want is to
   know when one actually lands, rather than printing the time on a timer of our
   own and calling it a sync (which is what the hourly log line used to do, two
   times out of three inaccurately).

   The callback runs in the SNTP task context, so it does the least possible
   work: record millis() and let TaskSetClock do the logging. Recording millis()
   rather than the wall clock also means "time since last sync" survives the
   clock stepping, which it does on every sync in SNTP_SYNC_MODE_IMMED.
*/
volatile uint32_t g_ntpLastSyncMs = 0;   // 0 = never synced since boot

void ntpSyncCallback(struct timeval *tv) {
  (void)tv;
  uint32_t ms = millis();
  g_ntpLastSyncMs = ms ? ms : 1;         // never leave it at the "never" sentinel
}

/*
   Has the wall clock ever actually been set?

   Without NTP, time() returns seconds since boot, which is a small but non-zero
   number - so a naive "is it > 0" test passes and the scheduler would happily
   evaluate windows against a 1970 date. Repeating windows would match at the
   wrong local time and absolute windows would either never fire or expire
   instantly. Since the scheduler can assert force charge, that is worth being
   strict about: only a real NTP sync counts.
*/
bool clockIsValid() { return g_ntpLastSyncMs != 0; }

// Seconds since the last successful sync, or -1 if the clock has never been set.
int32_t ntpSecondsSinceSync() {
  uint32_t last = g_ntpLastSyncMs;
  if (last == 0) return -1;
  return (int32_t)((uint32_t)(millis() - last) / 1000);
}

// Apply the configured POSIX TZ string to the C library. Called at boot and
// whenever the setting changes, so localtime_r() - used by the clock display and
// by syslog timestamps - reports local time including any DST shift.
void applyTimeZone() {
  String tz = pref.getString(ccTimeZone, initTimeZone);
  if (tz.length() == 0) tz = initTimeZone;
  g_tzCached = tz;
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

void TaskSetClock(void * pointer) {

  log_d("Entering TaskSetClock");
  applyTimeZone();
  String Servers = pref.getString(ccNTPServer,"");
  const bool dhcpNtp = pref.getBool(ccNTPFromDHCP, true);
    // Nothing to go on: no server typed in and not willing to take one from DHCP
  if (Servers.length()==0 && !dhcpNtp)
  {
    log_d("No NTP Server Set.");
    vTaskDelete(NULL);
    return;
  }

  while (!WiFi.isConnected())
    vTaskDelay(1000 / portTICK_PERIOD_MS);
     
  bool secondserver = false;
  String ServerArray[2];

  // A single server has no comma to split on. The old code only populated
  // ServerArray[0] inside the "comma found" branch, so one server meant
  // configTime() was handed an empty string and the clock never synced -
  // while the log below still printed the configured address, which made it
  // look like it was working.
  int CommaLoc = Servers.indexOf(',');
  if (CommaLoc == -1) {
    ServerArray[0] = Servers;
    ServerArray[0].trim();
  } else {
    ServerArray[0] = Servers.substring(0, CommaLoc);
    ServerArray[0].trim();
    ServerArray[1] = Servers.substring(CommaLoc + 1);
    ServerArray[1].trim();
    secondserver = (ServerArray[1].length() > 0);
  }

  if (ServerArray[0].length() == 0 && !dhcpNtp) {
    WS_LOG_W("NTP server setting is not a usable address, clock will not be set");
    vTaskDelete(NULL);
    return;
  }

  String tz = pref.getString(ccTimeZone, initTimeZone);
  if (tz.length() == 0) tz = initTimeZone;

  // Register before starting SNTP so the very first sync is not missed
  sntp_set_time_sync_notification_cb(ntpSyncCallback);

  /* Slot 0 is reserved for whatever DHCP offered, because that is the only slot
     it can use - CONFIG_LWIP_DHCP_MAX_NTP_SERVERS is 1 - and configTzTime()
     writes slots 0,1,2 unconditionally, so a manually configured server in slot
     0 would quietly overwrite the DHCP one. With DHCP enabled the typed-in
     servers move down to slots 1 and 2 and act as the fallback: SNTP works
     through the slots in order, so the router is tried first and the manual
     entries answer if it offered nothing. */
  if (dhcpNtp) {
    /* Already switched on in setup(), before the lease - see the note there.
       Repeated here only because it is cheap and makes this block correct on
       its own if the clock is ever reconfigured at runtime. */
    esp_sntp_servermode_dhcp(true);

    // Say whether the router actually offered one, so "no NTP response" can be
    // told apart from "your router does not send DHCP option 42"
    const ip_addr_t* dhcpSrv = esp_sntp_getserver(0);
    if (dhcpSrv && !ip_addr_isany(dhcpSrv))
      WS_LOG_I("DHCP offered NTP server %s", ipaddr_ntoa(dhcpSrv));
    else
      WS_LOG_W("DHCP did not offer an NTP server (option 42)%s",
               ServerArray[0].length() ? " - using the configured server instead"
                                       : " - set one manually or the clock will not sync");

    if (ServerArray[0].length()) {
      WS_LOG_I("Setting NTP clock from DHCP, falling back to %s%s%s (TZ %s)",
               ServerArray[0].c_str(),
               secondserver ? " and " : "",
               secondserver ? ServerArray[1].c_str() : "",
               tz.c_str());
      configTzTime(tz.c_str(), NULL, ServerArray[0].c_str(),
                   secondserver ? ServerArray[1].c_str() : NULL);
    } else {
      WS_LOG_I("Setting NTP clock from DHCP (TZ %s)", tz.c_str());
      configTzTime(tz.c_str(), NULL, NULL, NULL);
    }
  }
  else if (secondserver) {
    WS_LOG_I("Setting NTP clock from %s and %s (TZ %s)",
             ServerArray[0].c_str(), ServerArray[1].c_str(), tz.c_str());
    configTzTime(tz.c_str(), ServerArray[0].c_str(), ServerArray[1].c_str(), NULL);
  } else {
    WS_LOG_I("Setting NTP clock from %s (TZ %s)", ServerArray[0].c_str(), tz.c_str());
    configTzTime(tz.c_str(), ServerArray[0].c_str(), NULL, NULL);
  }

  // Wait for the first sync, but give up eventuallyrather than spinning forever
  // in silence the way this used to when the server was unreachable.
  const uint32_t NTP_SYNC_TIMEOUT_MS = 120000;
  uint32_t waitStart = millis();
  time_t now = time(nullptr);
  while (g_ntpLastSyncMs == 0) {
    if ((uint32_t)(millis() - waitStart) > NTP_SYNC_TIMEOUT_MS) {
      // Name the source that was actually tried, so "no response from " is not
      // followed by an empty string on a DHCP-only setup
      const char* srv = esp_sntp_getservername(0);
      WS_LOG_E("No NTP response from %s after %us - check the address is reachable and UDP/123 is open",
               ServerArray[0].length() ? ServerArray[0].c_str()
                                       : (srv ? srv : "the DHCP-provided server"),
               (unsigned)(NTP_SYNC_TIMEOUT_MS / 1000));
      vTaskDelete(NULL);
      return;
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
    now = time(nullptr);
  }

  struct tm timeinfo;
  char stamp[48];
  uint32_t lastLogged = 0;

  // One line per real sync, instead of one per hour regardless. The 5s poll is
  // just picking up the flag the callback set; it does not drive anything.
  while (true)
  {
    uint32_t sync = g_ntpLastSyncMs;
    if (sync != lastLogged) {
      lastLogged = sync;
      time(&now);
      localtime_r(&now, &timeinfo);
      // strftime, not asctime: asctime appends a trailing newline, which
      // sendLogToWS escapes and turns into a mangled log line. %Z gives GMT/BST.
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
      const char* srv = esp_sntp_getservername(0);
      WS_LOG_I("Clock synced from %s: %s", srv ? srv : "NTP", stamp);
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }

}

/* Fills doc with the current state.

   Split out of generateDatatoJSON() so the WebSocket path can serialise
   straight into the buffer it is about to send, rather than building a String
   for the library to copy. See notifyWSClients(). */
static void buildDataDoc(JsonDocument& doc, bool All)
{
  // If ALL is true generate a json with all data
  if (All){
    doc["BMS"] = All;
    doc["dischargevoltage"] = Inverter.GetDischargeVoltage();
    doc["maxchargecurrent"] = Inverter.GetMaxChargeCurrent();
    doc["maxdischargecurrent"] = Inverter.GetMaxDischargeCurrent();
    doc["minchargecur"] = Inverter.MinChargeCurrent();
    doc["adjuststep"] = pref.getUInt16(ccAdjustStep,initAdjustStep);
    doc["minchargecurr"] = pref.getUInt32(ccMinCharge,initMinChargeCurrent);
    doc["lowsoclimit"] = Inverter.GetLowSOCLimit();
    doc["highsoclimit"] = Inverter.GetHighSOCLimit();
    doc["batterycapacity"] = Inverter.GetBatteryCapacity();
    doc["victrondata"] = Lcd.Data.VEData.getValue();
    doc["canbusinterfaceup"] = Inverter.CanBusAvailable;
    doc["canbusdata"] = Inverter.CanBusDataOK;
    doc["mqttconnected"] = Lcd.Data.MQTTConnected.getValue();
    doc["mqttclientid"] = wifiManager.GetMQTTClientID();
    doc["mqttserverip"] = wifiManager.GetMQTTServerIP();
    doc["mqttport"] = wifiManager.GetMQTTPort();
    doc["victronrxpin"] = pref.getUInt8(ccVictronRX, 0);
    doc["victrontxpin"] = pref.getUInt8(ccVictronTX, 0);
    doc["canbusenabled"] = Inverter.CANBusEnabled();
    #ifndef ESPCAN
    doc["canbuscspin"] = pref.getUInt8(ccCanCSPin, 0);
    doc["can16mhz"] = pref.getBool(ccCAN16Mhz,initCAN16Mhz);
    #endif
    doc["pylontechenabled"] = Inverter.EnableRequestFlags(); // Legacy compatibility
    doc["soctrickenabled"] = Inverter.EnableSOCTrick();
    doc["requestflagsenabled"] = Inverter.EnableRequestFlags();
    doc["never100soc"] = Inverter.Never100SOC();
    doc["canprotocol"] = (uint8_t)Inverter.GetCANProtocol();
    doc["pylonversion"] = (uint8_t)Inverter.GetCANProtocol(); // backward compat for cached pages
    // Display copy plus exact bytes - see the SSID transport note above. The
    // sanitised one must never be what gets saved back.
    doc["wifissid"] = toDisplayUTF8(wifiManager.GetWifiSSID());
    doc["wifissidhex"] = bytesToHex(wifiManager.GetWifiSSID());
    doc["wifipass"] = toDisplayUTF8(wifiManager.GetWifiPass());
    doc["wifihostname"] = wifiManager.GetWifiHostName();
    doc["mqttuser"] = wifiManager.GetMQTTUser();
    doc["mqttpass"] = wifiManager.GetMQTTPass();
    doc["mqttclientid"] = wifiManager.GetMQTTClientID();
    doc["mqttport"] = wifiManager.GetMQTTPort();
    doc["mqtttopic"] = wifiManager.GetMQTTTopic();
    doc["mqttserverip"] = wifiManager.GetMQTTServerIP();
    doc["velooptime"] = VE_LOOP_TIME;
    doc["slowchargesoc1"] = Inverter.GetSlowChargeSOCLimit(1);
    doc["slowchargesoc2"] = Inverter.GetSlowChargeSOCLimit(2);
    doc["slowchargesoc1div"] = Inverter.GetSlowChargeDivider(1);
    doc["slowchargesoc2div"] = Inverter.GetSlowChargeDivider(2);
    doc["lcdenabled"] = pref.getBool(ccLcdEnabled,false);
    doc["ntpserver"] = pref.getString(ccNTPServer,"");
    doc["ntpfromdhcp"] = pref.getBool(ccNTPFromDHCP, true);
    doc["timezone"] = g_tzCached;
#ifndef DISABLE_SCHEDULER
    Schedule.uiToJson(doc["schedui"].to<JsonArray>());
    Schedule.mqttToJson(doc["schedmqtt"].to<JsonArray>());
    doc["overridetimeout"] = RemoteOverride.GetTimeout();
#endif
    doc["requesttimeout"] = Inverter.GetRequestTimeout();
    doc["syslogserver"] = g_syslogServerCached;
    doc["syslogport"] = pref.getUInt16(ccSyslogPort, 514);
    doc["syslogenabled"] = pref.getBool(ccSyslogEnabled, false);
    doc["cansniffer"] = Inverter.CANSniffer();   // runtime only, never persisted
    doc["blesniffer"] = VictronBle.Sniffer();    // ditto
    doc["shuntsource"] = shuntSource;   // cached at boot, not re-read per call
    /* The fallback is a source now, not a switch - same three values as
       shuntsource plus SHUNT_FALLBACK_NONE (255) for none at all. The old
       blefallback boolean is gone from the payload rather than kept alongside
       it: there is no honest boolean to send for "MQTT primary, BLE fallback",
       and a field that answered such a setup with "false" would be read as the
       fallback being off. */
    doc["fallbacksource"] = fallbackSource;

    /* The BLE block is built only when it can matter. This function runs on
       every notifyWSClients(), many times a second, and each field costs an
       allocation - the earlier version also read the MAC out of NVS every time,
       which showed up in the log as VBLEMac being looked up dozens of times a
       second. Together with the device list that was enough allocation churn to
       exhaust the heap on a 4MB ESP32: AsyncTCP's tcp_poll eventually could not
       allocate, and with exceptions disabled a failed operator new is not a
       thrown bad_alloc but a call to abort(). The board died with the web
       server unable to accept connections, then panicked.

       An install reading the shunt over the wire now pays nothing for BLE
       being compiled in. */
    /* "Configured" counts as relevant, not just "in use". Someone who has
       scanned, picked a shunt and entered its key has not switched the source
       over yet - and if we send nothing back, the address box and key state sit
       empty and the page looks like it threw the settings away. They are in NVS;
       the UI just never heard about them. An install that has never touched BLE
       still has no MAC and no key, so it pays nothing, which is what the note
       above is really protecting. */
    /* Hardware-gated ahead of everything else: a board with no PSRAM can never
       run the radio (see VictronBLE::HardwareSupported), so none of this is
       relevant to it even if a MAC/key were saved under older firmware - and
       the UI for a board that has no PSRAM omits the BLE fields it would
       otherwise update, so sending them here would find nothing to write to. */
    const bool bleHwSupported = VictronBle.HardwareSupported();
    const bool bleConfigured = bleHwSupported &&
                               (VictronBle.HaveKey() || VictronBle.GetMac().length() > 0);
    const bool bleRelevant = bleHwSupported && (
                             (shuntSource == SHUNT_SRC_BLE) ||
                             (fallbackSource == SHUNT_SRC_BLE) || bleConfigured ||
                             VictronBle.Sniffer() || VictronBle.Scanning() ||
                             VictronBle.FoundCount() > 0);
    doc["blerelevant"] = bleRelevant;
    if (bleRelevant) {
      doc["blemac"] = VictronBle.GetMac();   // held in RAM, no NVS read here
      /* The key is never sent back. It is the only credential protecting the
         shunt's broadcasts, and there is nothing to gain from echoing it into
         every browser on the network - the UI only needs to know whether one is
         stored so it can say so. */
      doc["blekeyset"] = VictronBle.HaveKey();
      doc["blescanning"] = VictronBle.Scanning();
      doc["bleadverts"] = VictronBle.AdvertsSeen;
      doc["blefailures"] = VictronBle.DecryptFailures;
      doc["blefresh"] = VictronBle.DataFresh();
      doc["blesoc"] = VictronBle.SOCPermille / 10.0;
      doc["blelastseen"] = VictronBle.LastUpdateMs ? (int32_t)((millis() - VictronBle.LastUpdateMs) / 1000) : -1;

      // Only meaningful for the few seconds after a scan, so it is not built
      // into every broadcast for the rest of the device's uptime.
      if (VictronBle.FoundCount() > 0) {
        JsonArray found = doc["blefound"].to<JsonArray>();
        for (uint8_t i = 0; i < VictronBle.FoundCount(); i++) {
          const VictronBLEFound* f = VictronBle.Found(i);
          if (!f) continue;
          JsonObject o = found.add<JsonObject>();
          o["mac"] = f->mac;
          o["name"] = f->name;
          o["rssi"] = f->rssi;
          o["shunt"] = (f->recordType == VICTRON_REC_BATTERY_MON);
          /* What each device IS, which is what someone with several of them
             needs to tell them apart. The name would be the obvious answer and
             usually is not there: Victron sends it in a scan response, which
             this passive listener never asks for, so the list reads "(no
             name)". The product id is in the advert itself and is already
             being read, so a SmartShunt says so, and an id with no entry in
             the table shows its hex rather than a guess. */
          char pid[8];
          snprintf(pid, sizeof(pid), "0x%04X", (unsigned)f->productId);
          o["productid"] = pid;
          const char* model = VictronBLE::ModelFromProductId(f->productId);
          if (model) o["model"] = model;
        }
      }
    }

    /* The MQTT shunt block, gated for the same reason the BLE one above is:
       four topic strings and a handful of counters on every broadcast is real
       allocation churn on a board where the largest free block is what runs
       out, and an install reading the shunt over the wire should pay nothing
       for this feature existing.

       "Configured" counts as relevant as well as "selected", so someone who has
       typed the topics in but not yet switched the source over still gets them
       back in the boxes instead of a page that looks like it lost them. */
    const bool mqttShuntConfigured = sMqttShuntSOC.length() > 0 || sMqttShuntVolt.length() > 0 ||
                                     sMqttShuntCurr.length() > 0 || sMqttShuntTemp.length() > 0;
    const bool mqttShuntRelevant = (shuntSource == SHUNT_SRC_MQTT) ||
                                   (fallbackSource == SHUNT_SRC_MQTT) || mqttShuntConfigured;
    doc["mqttshuntrelevant"] = mqttShuntRelevant;
    if (mqttShuntRelevant) {
      doc["mqttshuntsoc"]  = sMqttShuntSOC;
      doc["mqttshuntvolt"] = sMqttShuntVolt;
      doc["mqttshuntcurr"] = sMqttShuntCurr;
      doc["mqttshunttemp"] = sMqttShuntTemp;
      doc["mqttshuntfresh"] = MqttShunt.DataFresh();
      /* Seconds since the newest of SOC/voltage/current arrived, -1 for never.
         Same shape as blelastseen so the UI can say the same thing about it. */
      doc["mqttshuntlastseen"] = MqttShunt.LastUpdateMs
                                   ? (int32_t)((millis() - MqttShunt.LastUpdateMs) / 1000) : -1;
      /* Which fields have ever produced a value, as a bitmask: 1 = SOC,
         2 = voltage, 4 = current, 8 = temperature - the order the four settings
         are listed in. This is what tells "no topics working" from "SOC topic
         wrong", which is the difference between a five-minute fix and an
         afternoon, and the three-field freshness gate means a source with one
         topic mistyped never goes fresh and otherwise says nothing about why. */
      doc["mqttshunthave"] = (uint8_t)((MqttShunt.HaveSOC     ? 1 : 0) |
                                       (MqttShunt.HaveVoltage ? 2 : 0) |
                                       (MqttShunt.HaveCurrent ? 4 : 0) |
                                       (MqttShunt.HaveTemp    ? 8 : 0));
      doc["mqttshuntmsgs"] = MqttShunt.MessagesSeen;
    }
    doc["overvoltage"] = Inverter.GetOverVoltage();
    doc["fanpin"] = pref.getUInt8(ccFanPin,0);
    doc["onewirepin"] = pref.getUInt8(ccOneWirePin,0);
    doc["autocharge"] = Inverter.AutoCharge();
    doc["smartinterval"] = Inverter.SmartInterval();
    doc["tailcurrent"] = Inverter.GetTailCurrentmA();
    doc["tailduration"] = Inverter.GetTailCurrentDuration();
    doc["maxabsorptiontime"] = Inverter.GetMaxAbsorptionTime();
    doc["rechargesoc"] = Inverter.GetRechargeSOC();
    doc["rechargevoltageoffset"] = Inverter.GetRechargeVoltageOffset();
    doc["tempprotection"] = Inverter.TempProtectionEnabled();
    doc["chargehightemp"] = Inverter.GetChargeHighTemp();
    doc["chargelowtemp"] = Inverter.GetChargeLowTemp();
    doc["dischargehightemp"] = Inverter.GetDischargeHighTemp();
    doc["dischargelowtemp"] = Inverter.GetDischargeLowTemp();
    doc["showtempdashboard"] = Inverter.ShowTempOnDashboard();
    doc["batttempsrc"] = Inverter.BattTempSource();
    doc["fantempsrc"] = Inverter.FanTempSource();
    // The RAM copies mqttsetup() loaded at boot, kept in step by the two set
    // handlers below - no reason to go back to flash for them on every payload
    doc["mqttbatttopic"] = sMqttBattTopic;
    doc["mqttinvtopic"] = sMqttInvTopic;
    doc["fanofftemp"] = Inverter.GetFanOffTemp();
    doc["fanfulltemp"] = Inverter.GetFanFullTemp();
    doc["fwversion_bms"] = FW_VERSION;
    doc["fwbuild"] = FW_BUILD;
    /* Which of the builds this is, exactly - scripts/build_env.py puts the env
       name in at compile time. FW_BUILD above is the wiring only ("ESP32
       TWAI"), which no longer identifies a build now that flash size and PSRAM
       vary too, and those are the parts that decide which image this board can
       accept. The web UI puts it in the download link so the site can hand back
       the right file rather than a directory to guess from. */
    #ifdef PIO_ENV
    doc["pioenv"] = PIO_ENV;
    #endif
    /* 3.0 moved to app slots of 1.9375MB (4MB flash) or 3.9375MB (8MB). An OTA
       cannot rewrite the partition table, so a device updated over the air from
       2.x is running 3.x code inside 2.x's 1.25MB slot and will one day refuse
       an update for no reason it can explain. Report the running slot so the UI
       can say so while it is still only a warning. */
    const esp_partition_t* runningPart = esp_ota_get_running_partition();
    doc["apppartkb"] = runningPart ? (runningPart->size / 1024) : 0;
    doc["legacypartitions"] = runningPart && runningPart->size <= 0x140000;
    #ifdef ESPCAN
    doc["can_tx_pin"] = pref.getUInt8(ccCAN_TX_PIN, 0);
    doc["can_rx_pin"] = pref.getUInt8(ccCAN_RX_PIN, 0);
    doc["can_en_pin"] = pref.getUInt8(ccCAN_EN_PIN, 0);
    #endif
    /* Fixed for the life of this boot, so they belong here rather than in the
       payload that goes out several times a second. The previous run's figures
       are the ones worth reading after an unexplained restart: a run that ended
       with plenty of heap did not die of exhaustion, and the cause is elsewhere.
       Zero when there is nothing to report - a cold start has no history. */
    doc["resetreason"] = Diag.ResetReason();
    doc["crashed"] = Diag.Crashed();
    doc["bootcount"] = Diag.BootCount();
    doc["prevuptime"] = Diag.PrevUptimeSecs();
    doc["prevheapmin"] = Diag.PrevHeapMin();
    doc["prevheapblock"] = Diag.PrevBlockMin();
    doc["wsskipped"] = wsSkippedLowHeap;
  }

  doc["RealTime"] = true;
#if defined(BMS_S3)
  // Diag samples these every ~5s, not every broadcast - the field rides along
  // on the existing RealTime tick rather than earning its own, but the value
  // itself only actually changes on Diag's own schedule.
  doc["cpuheadroom0"] = Diag.CpuHeadroomCore0();
  doc["cpuheadroom1"] = Diag.CpuHeadroomCore1();
#endif
  taskENTER_CRITICAL(&(Inverter.CANMutex));
  /* One decimal, always, including the .0.
   *
   * Both sources measure SOC in 0.1% and it was being truncated to a byte
   * before anything saw it, so a pack sitting at 87.4% reported 87 and a
   * browser had no way to know it was not exactly 87.
   *
   * serialized() rather than a float, because ArduinoJson writes a float that
   * happens to be whole as "87" - which is the same JSON number, but a reader
   * watching the frames cannot then tell a device that reports whole percent
   * from one that measured exactly 87.0. Written out this way the field always
   * has the same shape, and it is still a JSON number rather than a string, so
   * nothing parsing it has to change. */
  doc["battsoc"] = serialized(String(Inverter.BattSOCPermille() / 10.0f, 1));
  doc["battvoltage"] = Inverter.BattVoltage();
  doc["battcurrent"] = Inverter.BattCurrentDeciA();
  doc["battpower"] = Inverter.BattPower();
  doc["batttemp"] = Inverter.BattTemp();
  doc["timetogo"] = Inverter.TimeToGo();
  doc["alarmactive"] = Inverter.AlarmActive();
  doc["alarmreason"] = Inverter.AlarmReason();
  doc["pidstring"] = Inverter.PIDString();
  doc["fwversion"] = Inverter.FWVersion();
  doc["serialnumber"] = Inverter.SerialNumber();
  doc["modelstring"] = Inverter.ModelString();

  /* Who the shunt is, from whichever link is carrying it.

     VE.Direct sends model, firmware and serial as text; BLE Instant Readout
     sends none of the three - it has a product id and whatever name was set
     in VictronConnect, and nothing else about identity. So the panel needs to
     know which link answered, or a BLE-only install reads as a broken cable:
     four empty fields and no clue why. shuntlink lets the UI say "over
     Bluetooth" and name the two fields that are simply not sent that way.

     Named shuntlink, not shuntsource: shuntsource already exists and is the
     CONFIGURED source, a uint8_t the settings handler writes back to NVS.
     Reusing the name would have put a string where the UI round-trips a
     number, which is the sort of thing that breaks a control silently. This
     one is what is actually arriving, which is a different question.

     Taken from activeShuntLink - the branch in loop() that last actually put a
     reading into Inverter - rather than inferred from what identity strings
     happen to be filled in. Inference got it wrong: a BLE install with the old
     VE.Direct cable still plugged in kept a model and serial from whenever the
     wire last spoke, so it read as "vedirect" forever. And on an MQTT source
     there is nothing at all to infer from, since no topic carries identity. */
  {
    const bool bleSeen = VictronBle.ProductId != 0;
    doc["shuntlink"] = (activeShuntLink == SHUNT_SRC_VEDIRECT) ? "vedirect"
                     : (activeShuntLink == SHUNT_SRC_BLE)      ? "ble"
                     : (activeShuntLink == SHUNT_SRC_MQTT)     ? "mqtt"
                     : "none";

    if (bleSeen) {
      char pid[8];
      snprintf(pid, sizeof(pid), "0x%04X", (unsigned)VictronBle.ProductId);
      doc["bleproductid"] = pid;
      const char* model = VictronBLE::ModelFromProductId(VictronBle.ProductId);
      if (model) doc["blemodel"] = model;
      if (VictronBle.DeviceName.length()) doc["blename"] = VictronBle.DeviceName;
      doc["blerssi"] = (int)VictronBle.Rssi;
    }
  }
  doc["chargevoltage"] = Inverter.GetChargeVoltage();
  doc["floatvoltage"] = Inverter.GetFloatVoltage();
  doc["floatcurrent"] = Inverter.GetFloatCurrent();
  // What float will actually hold at, so a configured 0 does not read as "none"
  doc["floatvoltageactive"] = Inverter.ActiveFloatVoltage();
  doc["floatvoltageauto"] = Inverter.FloatUsingAutoVoltage();
  taskEXIT_CRITICAL(&(Inverter.CANMutex));
  
  doc["chargeadjust"] = Inverter.GetChargeAdjust();
  doc["chargeenabled"] = (Inverter.ChargeEnable() && Inverter.ManualAllowCharge()) ? true : false;
  doc["dischargeenabled"] = (Inverter.DischargeEnable() && Inverter.ManualAllowDischarge()) ? true : false;
  doc["forcecharge"] = Inverter.ForceCharge();
  doc["requestfullcharge"] = Inverter.RequestFullCharge();
  /* Both of the above ride on 0x35C. False here means they are not being sent
     at all - see CANBUS::RequestFlagsActive(). */
  doc["requestflagsactive"] = Inverter.RequestFlagsActive();
  doc["soctrickactive"] = Inverter.EnableSOCTrick() && Inverter.ForceCharge();
  doc["autocharge"] = Inverter.AutoCharge();  // smart-charge state for HA "Smart Charge Status" binary sensor
  doc["chargecurrent"] = Inverter.GetChargeCurrent();
  doc["dischargecurrent"] = Inverter.GetDischargeCurrent();
  doc["maxdischargecurrent"] = Inverter.GetMaxDischargeCurrent();
  /* Configured ceiling, what a controller is asking for, and what is actually in
     force - a supervisor needs all three to tell "my request was accepted" from
     "my request was capped". -1 means no request is live, which is distinct from
     a genuine request of 0. */
  doc["maxchargecurrent"] = Inverter.GetMaxChargeCurrent();
  doc["reqchargecurrent"] = Inverter.ChargeRequestActive()
                              ? (int32_t)Inverter.GetRequestedChargeCurrent() : -1;
  doc["reqdischargecurrent"] = Inverter.DischargeRequestActive()
                              ? (int32_t)Inverter.GetRequestedDischargeCurrent() : -1;
  doc["reqchargeage"] = Inverter.ChargeRequestAge();
  doc["reqdischargeage"] = Inverter.DischargeRequestAge();
  doc["effchargecurrent"] = Inverter.EffectiveMaxChargeCurrent();
  doc["effdischargecurrent"] = Inverter.EffectiveDischargeCurrent();
  doc["chargephase"] = Inverter.GetChargePhaseName();
  // Absorption progress: the two races that can end the phase
  doc["absorbelapsed"] = Inverter.GetAbsorptionElapsed();
  doc["absorbmax"] = (uint32_t)Inverter.GetMaxAbsorptionTime() * 60;  // seconds, 0 = no limit
  doc["tailheld"] = Inverter.GetTailHeldSeconds();
  doc["tailneed"] = Inverter.GetTailCurrentDuration();
  doc["tailactive"] = Inverter.TailCurrentHeld();
  doc["tailthreshold"] = Inverter.GetTailCurrentmA();
  // Which of the two tail conditions is currently satisfied, so the dashboard
  // can name the actual blocker instead of always blaming the current.
  doc["tailvoltok"] = Inverter.TailVoltageOK();
  doc["tailvoltmin"] = Inverter.TailVoltageMinCentiV();
  // SOC as actually transmitted to the inverter. When CAN is disabled nothing is being
  // sent, so report no override rather than leaving the last value to go stale.
  if (Inverter.CANBusEnabled()) {
    doc["reportedsoc"] = Inverter.GetReportedSOC();
    doc["socoverride"] = Inverter.GetSOCOverride();
  } else {
    doc["reportedsoc"] = Inverter.BattSOC();
    doc["socoverride"] = 0;
  }
  doc["showtempdashboard"] = Inverter.ShowTempOnDashboard();
  doc["cantotalfails"] = Inverter.GetFailedTotalCount();
#ifndef DISABLE_SCHEDULER
  {
    bool clockOk = clockIsValid();
    time_t schedNow = time(nullptr);
    SchedDecision sd;
    if (clockOk) sd = Schedule.evaluate(schedNow, Inverter.BattSOC(), Inverter.ChargeEnable());
    doc["schedclock"] = clockOk;
    doc["schedactive"] = sd.active;
    doc["schedsource"] = sd.active ? (sd.fromMqtt ? "mqtt" : "ui") : "none";
    doc["schedmqttcount"] = Schedule.mqttCount();
    doc["scheduicount"] = Schedule.uiCount();
    doc["schednext"] = (uint32_t)Schedule.nextMqttStart(schedNow);
    doc["schednextin"] = clockOk ? Schedule.secondsUntilNext(schedNow) : -1;
    doc["schedendsin"] = clockOk ? Schedule.secondsUntilEnd(schedNow) : -1;
    // Which levers a controller (or the UI) currently holds off the scheduler,
    // and how long is left on the longest of them.
    doc["ovrcharge"]    = RemoteOverride.Active(OV_CHARGE);
    doc["ovrdischarge"] = RemoteOverride.Active(OV_DISCHARGE);
    doc["ovrforce"]     = RemoteOverride.Active(OV_FORCE);
    doc["ovrsecs"]      = RemoteOverride.SecondsLeft();
  }
#endif
  // -1 = never synced. -2 = no source configured at all, so nothing will sync.
  // Taking the server from DHCP counts as a source even with the field empty.
  doc["ntpsyncago"] = (pref.getString(ccNTPServer,"").length() == 0 &&
                       !pref.getBool(ccNTPFromDHCP, true))
                        ? -2 : ntpSecondsSinceSync();
  doc["inverterpresent"] = Inverter.InverterPresent();
  doc["victrondata"] = Lcd.Data.VEData.getValue();
  doc["mqttconnected"] = Lcd.Data.MQTTConnected.getValue();
  doc["mqttinvertertemp"] = Inverter.MqttInverterTemp();
  doc["mqttbatttemp"] = Inverter.MqttBattTemp();
  doc["fanpwm"] = FAN_PWM;
  doc["totalheap"] = ESP.getHeapSize();
  doc["freeheap"] = ESP.getFreeHeap();
  /* Three fields, not seven. This runs on every notifyWSClients(), many times a
     second, and each field costs an allocation - see the note above the BLE
     block. These three move; the rest of the diagnostics are fixed for the life
     of the boot and ride along in the All payload below instead.

     Free heap alone does not predict the abort: what fails is a specific
     allocation, so the largest contiguous block matters as much as the total,
     and the low-water marks matter more than either, because the moment that
     kills the board is over long before the next reading. */
  doc["uptime"] = Diag.UptimeSecs();
  doc["heapmin"] = Diag.HeapMin();
  doc["heapblock"] = Diag.BlockMin();

  /* Optional features add their own fields last, so a feature can never
     displace one of the fields above by picking the same key. No-op when none
     are compiled in - see Features.h. */
  Feature::BuildDocAll(doc, All);

}

/* The String form, for the MQTT publish of /Data - mqttPublish() wants a
   null-terminated buffer.

   reserve() first. A String grown by serializeJson() reallocates as it fills -
   64, 128, 256, ... up past 4KB - claiming a fresh contiguous block each time
   and releasing the one behind it. That leaks nothing, but run several times a
   second it is a fragmentation engine, and what kills this board is not free
   heap reaching zero: it is the largest contiguous block getting too small to
   satisfy a request while kilobytes sit free in pieces. Measured first, the
   whole payload lands in one exact-size allocation. */
String generateDatatoJSON(bool All)
{
  JsonDocument doc;
  buildDataDoc(doc, All);
  String outputJson;
  outputJson.reserve(measureJson(doc) + 1);
  serializeJson(doc, outputJson);
  outputJson.trim();  // Remove trailing newline and whitespace from serializeJson
  return outputJson;
}

/* cleanupClients() with no argument, so the library's default of 8 applies and
   nothing of ours evicts anybody.

   A tighter cap was tried here and removed. It was added on the theory that
   browser tabs were consuming the heap, which the measurements then disproved:
   a tab costs between 40 and 1,132 bytes, and the deep troughs blamed on tabs
   turned out to be the MQTT discovery outbox draining in the same window. What
   a cap would reliably do is evict the OLDEST client when a new one arrives -
   and the oldest is exactly the long-lived socket an integration holds, so
   three browser tabs would have quietly dropped it. */
void notifyWSClients(bool sendalldata = true) {
  if(otaInProgress) return;
  ws.cleanupClients();
  if(ws.count() == 0) return;

  JsonDocument doc;
  buildDataDoc(doc, sendalldata);
  const size_t n = measureJson(doc);
  if (n == 0) return;

  /* Refuse rather than abort. Exceptions are off, so a failed allocation is
     not a thrown bad_alloc but a call to abort() - and the allocation below is
     one of the largest this firmware makes, on the path that runs most often.
     Skipping an update costs nothing: the next one is along in a moment and
     carries the same state. Aborting costs the whole device. */
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < n + 1024) {
    /* Straight to serial and nothing else. Every other logging path on this
       board allocates, which is precisely what is not available right now. */
    static uint32_t lastMoan = 0;
    wsSkippedLowHeap++;
    if ((uint32_t)(millis() - lastMoan) > 5000) {
      lastMoan = millis();
      Serial.printf("[heap] skipped a %u B broadcast, largest block %u B (%u skipped)\r\n",
                    (unsigned)n, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                    (unsigned)wsSkippedLowHeap);
    }
    return;
  }

  /* Serialise into the send buffer itself. textAll(String) would allocate the
     shared vector and copy the String into it, so both were live at the peak;
     this way there is one allocation of exactly the right size and no copy.
     The buffer is shared between clients, not duplicated per client - see the
     note above wsBroadcast(). */
  auto buf = std::make_shared<std::vector<uint8_t>>(n);
  serializeJson(doc, buf->data(), n);
  ws.textAll(std::move(buf));
}

String GetWSDataJson(const String& data, const String& value)
{
  for (auto x : value)
    {
      if (!isDigit(x) && x != '.' && x != '-' )
        return "{\"" + data +"\":\"" + value + "\"}";
    }
    // if we get here all characters were digits i.e. number
  return "{\"" + data +"\":" + value + "}";
}

void handleWSRequest(AsyncWebSocketClient * wsclient,const char * data, int len){

  JsonDocument doc;
  // Check if it's a GET request
  if (strncmp(data,"Get",(int)3)==0) {
    if (strncmp(data,"GetAll()",len)==0)
      notifyWSClients();
    else if (strncmp(data,"GetLogs()",len)==0) {
      /* One entry is copied out at a time rather than the whole batch up front.
         The old version kept a static LogEntry[LOG_SEND_MAX] purely as a staging
         area - 10,800 bytes of RAM permanently reserved to serve a request that
         happens when somebody opens the Logs tab. A single entry on the stack
         does the same job, still copies out of the critical section before any
         sending, and gives that RAM back to the heap for good. */
      LogEntry entry;
      int available = 0;

      // How far back the ring actually goes, so the replay can run oldest-first
      // without holding the lock across the sends.
      taskENTER_CRITICAL(&logMutex);
      for(int i = 1; i <= LOG_BUFFER_SIZE && available < LOG_SEND_MAX; i++) {
        int idx = (logBufferIndex - i + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
        if(logBuffer[idx].message[0] == '\0') break;   // not wrapped yet, nothing older
        available++;
      }
      taskEXIT_CRITICAL(&logMutex);

      // Oldest first, so the viewer reads in chronological order.
      uint32_t nowMs = millis();
      for(int n = available; n >= 1; n--) {
        if(wsclient->status() != WS_CONNECTED) break;   // client went away

        taskENTER_CRITICAL(&logMutex);
        int idx = (logBufferIndex - n + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
        entry = logBuffer[idx];                        // plain struct copy, no alloc
        taskEXIT_CRITICAL(&logMutex);

        // Built as a String, not snprintf into a fixed buffer: message plus level
        // and age would sit close to any fixed size, and truncation would cut the
        // JSON off mid-string.
        // "age" is how long ago the line was logged. The client has no reference
        // for our millis(), and stamping replayed lines with their arrival time
        // collapsed a whole backlog onto one second of the browser's clock.
        String json = "{\"log\":\"";
        json += jsonEscapeLog(entry.message);
        json += "\",\"level\":\"";
        json += logLevelName(entry.level);
        json += "\",\"age\":";
        json += (unsigned long)(nowMs - entry.timestamp);
        json += "}";
        wsclient->text(json);

        // Yield every 10 messages to prevent WDT
        if((available - n) % 10 == 0) yield();
      }
    }
    else if (strncmp(data,"GetWifiScan()",len)==0) {
      // Request new WiFi scan in background
      log_d("WiFi scan requested via WebSocket");
      taskENTER_CRITICAL(&wifiScanMutex);
      int scanStatus = WiFi.scanComplete();
      taskEXIT_CRITICAL(&wifiScanMutex);
      
      // Start scan only if not already in progress
      if(scanStatus != -1) {
        log_d("Starting background WiFi scan");
        WS_LOG_D("Started background WiFi scan via WebSocket request");
        WiFi.scanNetworks(true);
      } else {
        log_d("WiFi scan already in progress");
      }
    }
    else if (strncmp(data,"GetChargeVoltage",len)==0)
      wsclient->printf("%s", GetWSDataJson("chargevoltage", String(Inverter.GetChargeVoltage())).c_str());
    else if (strncmp(data,"GetDischargeVoltage",len)==0)
      wsclient->printf("%s", GetWSDataJson("dischargevoltage", String(Inverter.GetDischargeVoltage())).c_str());
    else if (strncmp(data,"GetChargeCurrent",len)==0)
      wsclient->printf("%s", GetWSDataJson("chargecurrent", String(Inverter.GetChargeCurrent())).c_str());
    else if (strncmp(data,"GetDischargeCurrent",len)==0)
      wsclient->printf("%s", GetWSDataJson("dischargecurrent", String(Inverter.GetDischargeCurrent())).c_str());
    else if (strncmp(data,"GetMaxChargeCurrent",len)==0)
      wsclient->printf("%s", GetWSDataJson("maxchargecurrent", String(Inverter.GetMaxChargeCurrent())).c_str());
    else if (strncmp(data,"GetMaxDischargeCurrent",len)==0)
      wsclient->printf("%s", GetWSDataJson("maxdischargecurrent", String(Inverter.GetMaxDischargeCurrent())).c_str());
    else if (strncmp(data,"GetSOC()",len)==0) {
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      uint8_t soc = Inverter.BattSOC();
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
      wsclient->printf("%s", GetWSDataJson("battsoc", String(soc)).c_str());
    }
    else if (strncmp(data,"GetBattCurrent()",len)==0) {
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      int32_t current = Inverter.BattCurrentDeciA();
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
      wsclient->printf("%s", GetWSDataJson("battcurrent", String(current)).c_str());
    }
    else if (strncmp(data,"GetBattVoltage()",len)==0) {
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      uint16_t voltage = Inverter.BattVoltage();
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
      wsclient->printf("%s", GetWSDataJson("battvoltage", String(voltage)).c_str());
    }
    else if (strncmp(data,"GetChargeEnabled()",len)==0)
      wsclient->printf("%s", GetWSDataJson("chargeenabled", String(Inverter.ChargeEnable())).c_str());
    else if (strncmp(data,"GetDischargeEnabled()",len)==0)
      wsclient->printf("%s", GetWSDataJson("dischargeenabled", String(Inverter.DischargeEnable())).c_str());
    else if (strncmp(data,"GetCANCSPin()",len)==0)
      wsclient->printf("%s", GetWSDataJson("canbuscspin", String(pref.getUInt8(ccCanCSPin,0))).c_str());
    else if (strncmp(data,"GetVictronRXPin()",len)==0)
      wsclient->printf("%s", GetWSDataJson("victronrxpin", String(pref.getUInt8(ccVictronRX,0))).c_str());
    else if (strncmp(data,"GetVictronTXPin()",len)==0)
      wsclient->printf("%s", GetWSDataJson("victrontxpin", String(pref.getUInt8(ccVictronTX,0))).c_str());
    else if (strncmp(data,"GetPylontechEnabled()",len)==0)
      wsclient->printf("%s", GetWSDataJson("pylontechenabled", String(pref.getBool(ccRequestFlags,false))).c_str());
    else if (strncmp(data,"GetSOCTrickEnabled()",len)==0)
      wsclient->printf("%s", GetWSDataJson("soctrickenabled", String(pref.getBool(ccSOCTrick,false))).c_str());
    else if (strncmp(data,"GetRequestFlagsEnabled()",len)==0)
      wsclient->printf("%s", GetWSDataJson("requestflagsenabled", String(pref.getBool(ccRequestFlags,false))).c_str());
    else {
      WS_LOG_D("Unknown Get Request via WebSocket: %s", data);
      wsclient->printf("{\"ERROR\" : \"Unknown Get Request\"}");
    }
  }
  else {
      // Handle Set Commands
      bool handled = false;
      deserializeJson(doc,data);

      /*
         Optional "persist" flag on any set command.

         Defaults to true, so the web UI and every existing client keep saving
         to NVS exactly as before. A remote supervisor (e.g. PowerPilot) sends
         "persist":false so its setpoints apply immediately but are never
         written to flash. Two reasons that matters:

         - Wear. A controller adjusting charge current every few seconds would
           otherwise rewrite NVS thousands of times a day.
         - Fail-safe. What survives a reboot or a lost controller is whatever
           the operator saved locally, not the last value some remote system
           happened to command before it vanished.

         Only the handlers that write NVS check this; the RAM-only ones
         (manual allow charge/discharge, force charge, charge current) are
         unaffected either way.
      */
      bool persist = doc["persist"].isNull() ? true : (bool)doc["persist"];

      if (!doc["wifiscan"].isNull()) {
        // Trigger WiFi scan in background
        log_d("WiFi scan requested via JSON Set");
        taskENTER_CRITICAL(&wifiScanMutex);
        int scanStatus = WiFi.scanComplete();
        taskEXIT_CRITICAL(&wifiScanMutex);
        
        // Start scan only if not already in progress
        if(scanStatus != -1) {
          log_d("Starting background WiFi scan");
          WiFi.scanNetworks(true);
          WS_LOG_I("WiFi scan started");
        } else {
          log_d("WiFi scan already in progress");
        }
        handled = true;
      }
      
      if (!doc["chargevoltage"].isNull()) {
        if (persist) pref.putUInt32(ccChargeVolt,(uint32_t) doc["chargevoltage"]);
        Inverter.SetChargeVoltage((uint32_t) doc["chargevoltage"]);
        WS_LOG_I("Set Charge Voltage to %u%s", (uint32_t) doc["chargevoltage"], persist ? "" : " (not saved)");
        handled = true;
        notifyWSClients(); }
      if (!doc["overvoltage"].isNull()) {
        if (persist) pref.putUInt32(ccOverVoltage,(uint32_t) doc["overvoltage"]);
        Inverter.SetOverVoltage((uint32_t) doc["overvoltage"]);
        WS_LOG_I("Set Over Voltage to %u%s", (uint32_t) doc["overvoltage"], persist ? "" : " (not saved)");
        handled = true;
        notifyWSClients(); }
      if (!doc["dischargevoltage"].isNull()) {
        if (persist) pref.putUInt32(ccDischargeVolt,(uint32_t) doc["dischargevoltage"]);
        Inverter.SetDischargeVoltage((uint32_t) doc["dischargevoltage"]);
        WS_LOG_I("Set Discharge Voltage to %u%s", (uint32_t) doc["dischargevoltage"], persist ? "" : " (not saved)");
        handled = true;
        notifyWSClients(); }
      if (!doc["maxchargecurrent"].isNull()) {
        if (persist) {
          pref.putUInt32(ccChargeCurrent,(uint32_t) doc["maxchargecurrent"]);
          // Re-publish HA discovery so the Charge Current slider max tracks the
          // new limit. Only for saved changes - a supervisor retuning the limit
          // every few seconds must not spam discovery messages at the broker.
          publishHADiscovery(true);   // configs are stale, this one must go out
        }
        Inverter.SetMaxChargeCurrent((uint32_t) doc["maxchargecurrent"]);
        WS_LOG_I("Set Max Charge Current to %u%s", (uint32_t) doc["maxchargecurrent"], persist ? "" : " (not saved)");
        handled = true;
        notifyWSClients(); }
      if (!doc["maxdischargecurrent"].isNull()) {
        if (persist) {
          pref.putUInt32(ccDischargeCurrent,(uint32_t) doc["maxdischargecurrent"]);
          // Same as the charge limit above: the Discharge Current slider max is
          // built from this value, so a saved change has to be re-advertised or
          // Home Assistant keeps rejecting anything above the old ceiling.
          publishHADiscovery(true);   // configs are stale, this one must go out
        }
        Inverter.SetMaxDischargeCurrent((uint32_t) doc["maxdischargecurrent"]);
        WS_LOG_I("Set Max Discharge Current to %u%s", (uint32_t) doc["maxdischargecurrent"], persist ? "" : " (not saved)");
        handled = true;
        notifyWSClients(); }
      // Force charge - RAM only in the firmware, so nothing to persist. Exposed
      // over WebSocket as well as MQTT so a controller can assert it without
      // needing a broker. Latches the lever so the scheduler does not undo it on
      // its next pass - see RemoteOverride.h.
      if (!doc["forcecharge"].isNull()) {
        Inverter.ForceCharge((bool) doc["forcecharge"]);
        RemoteOverride.Arm(OV_FORCE);
        WS_LOG_I("Force charge set to: %s", (bool) doc["forcecharge"] ? "ON" : "OFF");
        if ((bool) doc["forcecharge"] && !Inverter.RequestFlagsActive())
          WS_LOG_W("Force charge set but 0x35C flags are not being sent - the "
                   "inverter will not see it. Enable Request Flags on a "
                   "Pylontech 1.2, Pylontech 1.3 or Growatt protocol.");
        handled = true;
        notifyWSClients(); }
      /* Request full charge - a different ask to force charge, and RAM only for
         the same reasons. It tells the inverter to run its next charge all the
         way to 100% rather than stopping at its own SOC limit, which is how you
         let a pack rebalance and reset its SOC. It does not start a charge, so
         it is not a scheduler lever and takes no override latch. */
      if (!doc["requestfullcharge"].isNull()) {
        Inverter.RequestFullCharge((bool) doc["requestfullcharge"]);
        WS_LOG_I("Request full charge set to: %s", (bool) doc["requestfullcharge"] ? "ON" : "OFF");
        handled = true;
        notifyWSClients(); }
      if (!doc["chargecurrent"].isNull()) {
        Inverter.SetChargeCurrent((uint32_t) doc["chargecurrent"]); 
        WS_LOG_I("Set Charge Current to %u", (uint32_t) doc["chargecurrent"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["dischargecurrent"].isNull()) {
        Inverter.SetDischargeCurrent((uint32_t) doc["dischargecurrent"]); 
        WS_LOG_I("Set Discharge Current to %u", (uint32_t) doc["dischargecurrent"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["batterycapacity"].isNull()) {
        pref.putUInt32(ccBattCapacity,(uint32_t) doc["batterycapacity"]);
        Inverter.SetBattCapacity((uint32_t) doc["batterycapacity"]);
        WS_LOG_I("Set Battery Capacity to %u", (uint32_t) doc["batterycapacity"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["chargeenabled"].isNull()) {
        Inverter.ChargeEnable((bool) doc["chargeenabled"]); 
        WS_LOG_I("Set Charge Enabled to %s", (bool) doc["chargeenabled"] ? "true" : "false");
        handled = true;
        notifyWSClients(); }
      if (!doc["dischargeenabled"].isNull()) {
        Inverter.DischargeEnable((bool) doc["dischargeenabled"]);
        WS_LOG_I("Set Discharge Enabled to %s", (bool) doc["dischargeenabled"] ? "true" : "false");
        handled = true;
        notifyWSClients(); }
      // Both latch, so a toggle here or from a controller survives the next
      // scheduler pass instead of snapping back within the second.
      if (!doc["manualallowcharge"].isNull()) {
        Inverter.ManualAllowCharge((bool) doc["manualallowcharge"]);
        RemoteOverride.Arm(OV_CHARGE);
        WS_LOG_I("Manual Allow Charge set to %s", (bool) doc["manualallowcharge"] ? "true" : "false");
        handled = true;
        notifyWSClients(); }
      if (!doc["manualallowdischarge"].isNull()) {
        Inverter.ManualAllowDischarge((bool) doc["manualallowdischarge"]);
        RemoteOverride.Arm(OV_DISCHARGE);
        WS_LOG_I("Manual Allow Discharge set to %s", (bool) doc["manualallowdischarge"] ? "true" : "false");
        handled = true;
        notifyWSClients(); }
#ifndef DISABLE_SCHEDULER
      // Hand control back to the schedule immediately, rather than waiting out
      // the latch. A controller shutting down cleanly should send this.
      if (!doc["clearoverride"].isNull()) {
        if ((bool) doc["clearoverride"]) {
          RemoteOverride.Clear();
          WS_LOG_I("Remote override cleared, schedule back in control");
        }
        handled = true;
        notifyWSClients(); }
      // Seconds a lever stays latched. 0 disables the latch entirely.
      if (!doc["overridetimeout"].isNull()) {
        uint16_t secs = (uint16_t) doc["overridetimeout"];
        if (secs > OVERRIDE_TIMEOUT_MAX) secs = OVERRIDE_TIMEOUT_MAX;
        if (persist) pref.putUInt16(ccOverrideTime, secs);
        RemoteOverride.SetTimeout(secs);
        WS_LOG_I("Remote override timeout set to %u s%s", secs, persist ? "" : " (not saved)");
        handled = true;
        notifyWSClients(); }
#endif
      // Low SOC OFF
      if (!doc["lowsoclimit"].isNull()) {
        if (persist) pref.putUInt8(ccLowSOCLimit,(uint8_t) doc["lowsoclimit"]);
        Inverter.SetLowSOCLimit((uint8_t) doc["lowsoclimit"]);
        WS_LOG_I("Set Low SOC Limit to %u%s", (uint8_t) doc["lowsoclimit"], persist ? "" : " (not saved)");
        handled = true;
        notifyWSClients(); }
      // High SOC Limit
      if (!doc["highsoclimit"].isNull()) {
        if (persist) pref.putUInt8(ccHighSOCLimit,(uint8_t) doc["highsoclimit"]);
        Inverter.SetHighSOCLimit((uint8_t) doc["highsoclimit"]);
        WS_LOG_I("Set High SOC Limit to %u%s", (uint8_t) doc["highsoclimit"], persist ? "" : " (not saved)");
        handled = true;
        notifyWSClients(); }
      if (!doc["canbusenabled"].isNull()) {
        bool value = bool(doc["canbusenabled"]);
        pref.putBool(ccCANBusEnabled, value);
        Inverter.CANBusEnabled(value);
        Inverter.StartRunTask(value);
        handled = true;
        notifyWSClients(); }

      if (!doc["adjuststep"].isNull()) {
        pref.putUInt16(ccAdjustStep, doc["adjuststep"]);
        Inverter.SetChargeStepAdjust(doc["adjuststep"]);
        WS_LOG_I("Set Adjust Step to %u", (uint16_t) doc["adjuststep"]);
        handled = true;
        notifyWSClients(); }

      if (!doc["minchargecurr"].isNull()) {
        pref.putUInt32(ccMinCharge, doc["minchargecurr"]);
        Inverter.MinChargeCurrent(doc["minchargecurr"]);
        WS_LOG_I("Set Min Charge Current to %u", (uint32_t) doc["minchargecurr"]);
        handled = true;
        notifyWSClients(); }

      if (!doc["lcdenabled"].isNull()) {
        pref.putBool(ccLcdEnabled, doc["lcdenabled"]);
        if(doc["lcdenabled"])
          Lcd.Enable();
          else
          Lcd.Disable();
        handled = true;
        notifyWSClients(); }
      
      // allowZero marks an optional pin, where 0 is a deliberate "not wired" rather
      // than an unset field to be ignored.
      #define HANDLE_PIN_EX(field, prefKey, allowZero) \
        if (!doc[field].isNull()) { \
          uint8_t _pv = (uint8_t) doc[field]; \
          handled = true; \
          const char* _conflict = FindPinConflict(_pv, field); \
          if (_pv == 0 && !(allowZero)) { log_w("Ignoring %s value 0", field); } \
          else if (_pv != 0 && IsForbiddenPin(_pv)) { wsclient->printf("{\"ERROR\" : \"%s %u is forbidden\"}", field, _pv); } \
          else if (_conflict) { wsclient->printf("{\"ERROR\" : \"%s %u already used by %s\"}", field, _pv, _conflict); } \
          else { pref.putUInt8(prefKey, _pv); notifyWSClients(); } \
        }
      #define HANDLE_PIN(field, prefKey) HANDLE_PIN_EX(field, prefKey, false)
      HANDLE_PIN("canbuscspin", ccCanCSPin)
      HANDLE_PIN("victronrxpin", ccVictronRX)
      HANDLE_PIN_EX("victrontxpin", ccVictronTX, true) // VE.Direct is listen-only, TX need not be wired
      HANDLE_PIN("can_rx_pin", ccCAN_RX_PIN)
      HANDLE_PIN("can_tx_pin", ccCAN_TX_PIN)
      HANDLE_PIN_EX("can_en_pin", ccCAN_EN_PIN, true) // some boards' CAN transceiver has no software enable line
      #undef HANDLE_PIN
      #undef HANDLE_PIN_EX

      /* wifissidhex wins when both are present: it is the only one that can
         carry a name the browser could not represent as text. wifissid stays
         accepted for a cached older page, an imported 2.x settings file, and
         anything scripting this WebSocket by hand. */
      if (!doc["wifissidhex"].isNull()) {
        String hex = doc["wifissidhex"];
        String value = hexToBytes(hex);
        if (value.length() == 0 && hex.length() > 0) {
          log_e("Ignoring wifissidhex '%s' - not valid hex", hex.c_str());
          WS_LOG_E("WiFi network name arrived malformed, not saved");
        } else {
          handled = true;
          wifiManager.SetWifiSSID(value);
          WS_LOG_I("WiFi SSID set to '%s' (%u bytes)",
                   toDisplayUTF8(value).c_str(), (unsigned)value.length());
          notifyWSClients();
        }
      }
      else if (!doc["wifissid"].isNull()) {
        String value = doc["wifissid"];
        handled = true;
        wifiManager.SetWifiSSID(value);
        notifyWSClients();}

      if (!doc["wifipass"].isNull()) {
        String value = doc["wifipass"];
        handled = true;
        wifiManager.SetWifiPass(value);}

      if (!doc["mqttserverip"].isNull()) {
        String value = doc["mqttserverip"];
        handled = true;
        wifiManager.SetMQTTServerIP(value);
        notifyWSClients();}

      if (!doc["mqttuser"].isNull()) {
        String value = doc["mqttuser"];
        handled = true;
        wifiManager.SetMQTTUser(value);
        notifyWSClients();}

      if (!doc["mqttpass"].isNull()) {
        String value = doc["mqttpass"];
        handled = true;
        wifiManager.SetMQTTPass(value);}

      if (!doc["mqtttopic"].isNull()) {
        String value = doc["mqtttopic"];
        handled = true;
        wifiManager.SetMQTTTopic(value);
        notifyWSClients();}

      if (!doc["mqttclientid"].isNull()) {
        String value = doc["mqttclientid"];
        handled = true;
        wifiManager.SetMQTTClientID(value);
        notifyWSClients();}

      if (!doc["mqttport"].isNull()) {
        uint16_t value = doc["mqttport"];
        handled = true;
        wifiManager.SetMQTTPort(value);
        notifyWSClients();}

      if (!doc["wifihostname"].isNull()) {
        String value = doc["wifihostname"];
        handled = true;
        wifiManager.SetWifiHostName(value);
        notifyWSClients();}

      if (!doc["slowchargesoc1"].isNull()) {
        uint8_t value = (uint8_t) doc["slowchargesoc1"];
        handled = true;
        pref.putUInt8(ccSlowSOCCharge1,value);
        Inverter.SetSlowChargeSOCLimit(1,value);
        notifyWSClients();}

      if (!doc["slowchargesoc1div"].isNull()) {
        uint8_t value = (uint8_t) doc["slowchargesoc1div"];
        handled = true;
        pref.putUInt8(ccSlowSOCDivider1,value);
        Inverter.SetSlowChargeDivider(1,value);
        notifyWSClients();}

      if (!doc["slowchargesoc2"].isNull()) {
        uint8_t value = (uint8_t) doc["slowchargesoc2"];
        handled = true;
        pref.putUInt8(ccSlowSOCCharge2,value);
        Inverter.SetSlowChargeSOCLimit(2,value);
        notifyWSClients();}

      if (!doc["slowchargesoc2div"].isNull()) {
        uint8_t value = (uint8_t) doc["slowchargesoc2div"];
        handled = true;
        pref.putUInt8(ccSlowSOCDivider2,value);
        Inverter.SetSlowChargeDivider(2,value);
        notifyWSClients();}

      if (!doc["pylontechenabled"].isNull()) {
        boolean value = doc["pylontechenabled"];
        handled = true;
        pref.putBool(ccRequestFlags, value);
        Inverter.EnableRequestFlags(value);
        notifyWSClients();}

      if (!doc["soctrickenabled"].isNull()) {
        boolean value = doc["soctrickenabled"];
        handled = true;
        pref.putBool(ccSOCTrick, value);
        Inverter.EnableSOCTrick(value);
        notifyWSClients();}

      if (!doc["requestflagsenabled"].isNull()) {
        boolean value = doc["requestflagsenabled"];
        handled = true;
        pref.putBool(ccRequestFlags, value);
        Inverter.EnableRequestFlags(value);
        notifyWSClients();}
      if (!doc["never100soc"].isNull()) {
        boolean value = doc["never100soc"];
        handled = true;
        pref.putBool(ccNever100SOC, value);
        Inverter.Never100SOC(value);
        WS_LOG_I("Never send 100%% SOC set to: %s", value ? "ON" : "OFF");
        notifyWSClients();}
      if (!doc["canprotocol"].isNull() || !doc["pylonversion"].isNull()) {
        uint8_t value = !doc["canprotocol"].isNull() ? (uint8_t)doc["canprotocol"] : (uint8_t)doc["pylonversion"];
        handled = true;
        pref.putUInt8(ccCANProtocol, value);
        Inverter.SetCANProtocol((CANProtocol)value);
        const char* protoNames[] = {"Pylontech 1.2", "Pylontech 1.3", "SMA", "Victron", "Growatt/SolArk"};
        WS_LOG_I("CAN Protocol set to: %s", (value < 5) ? protoNames[value] : "Unknown");
        notifyWSClients();}

      if (!doc["velooptime"].isNull()) {
        uint8_t value = doc["velooptime"];
        handled = true;
        VE_LOOP_TIME = value;
        pref.putUInt8(ccVELOOPTIME,VE_LOOP_TIME);
        notifyWSClients();}
      
      if (!doc["ntpserver"].isNull()) {
      String value = doc["ntpserver"];
      handled = true;
      pref.putString(ccNTPServer,value);
      notifyWSClients();}

      if (!doc["ntpfromdhcp"].isNull()) {
        bool value = doc["ntpfromdhcp"];
        handled = true;
        pref.putBool(ccNTPFromDHCP, value);
        // SNTP is configured once, in TaskSetClock, so this lands on the next boot
        WS_LOG_I("NTP from DHCP %s (reboot to apply)", value ? "enabled" : "disabled");
        notifyWSClients();
      }

#ifndef DISABLE_SCHEDULER
      if (!doc["schedule"].isNull()) {
        handled = true;
        int n = Schedule.setUiFromJson(doc["schedule"].as<JsonArrayConst>());
        saveUiSchedule();
        WS_LOG_I("Saved %d repeating schedule window(s)", n);
        notifyWSClients();
      }
#endif

      if (!doc["timezone"].isNull()) {
        String value = doc["timezone"];
        value.trim();
        handled = true;
        pref.putString(ccTimeZone, value.length() ? value : String(initTimeZone));
        applyTimeZone();          // takes effect immediately, no reboot
        time_t nowTz = time(nullptr);
        struct tm tmTz; char sTz[48];
        localtime_r(&nowTz, &tmTz);
        strftime(sTz, sizeof(sTz), "%Y-%m-%d %H:%M:%S %Z", &tmTz);
        WS_LOG_I("Timezone set, local time is now %s", sTz);
        notifyWSClients();
      }

      // Syslog settings all re-apply immediately - no reboot needed
      if (!doc["syslogserver"].isNull()) {
        String value = doc["syslogserver"];
        handled = true;
        pref.putString(ccSyslogServer, value);
        applySyslogConfig();
        WS_LOG_I("Syslog server set to %s", value.length() ? value.c_str() : "(none)");
        notifyWSClients();
      }

      if (!doc["syslogport"].isNull()) {
        uint16_t value = (uint16_t) doc["syslogport"];
        handled = true;
        pref.putUInt16(ccSyslogPort, value ? value : 514);
        applySyslogConfig();
        notifyWSClients();
      }

      if (!doc["syslogenabled"].isNull()) {
        bool value = doc["syslogenabled"];
        handled = true;
        pref.putBool(ccSyslogEnabled, value);
        applySyslogConfig();
        if (value && !Syslog.configured())
          WS_LOG_W("Syslog enabled but no valid server IP set - nothing will be sent");
        else
          WS_LOG_I("Syslog %s", value ? "enabled" : "disabled");
        notifyWSClients();
      }

      /* Deliberately not persisted and not part of settings export - turning
         this on stops the inverter being fed, so it must never survive a reboot
         or arrive with an imported config file. SetCANSniffer does the logging,
         including the warning about what has just gone quiet. */
      if (!doc["cansniffer"].isNull()) {
        bool value = doc["cansniffer"];
        handled = true;
        Inverter.SetCANSniffer(value);
        notifyWSClients();
      }

      // Also runtime only, and also not in settings export
      // Diagnostic: does the MQTT client copy our buffers? See mqttRunCopyTest()
      if (!doc["mqttcopytest"].isNull()) {
        handled = true;
        mqttRunCopyTest();
      }

      if (!doc["blesniffer"].isNull()) {
        bool value = doc["blesniffer"];
        handled = true;
        if (value && !VictronBle.EnsureRunning())
          wsclient->printf("{\"ERROR\" : \"Could not start Bluetooth\"}");
        else {
          VictronBle.SetSniffer(value);
          WS_LOG_I("Victron BLE sniffer %s", value ? "on - logging every Victron advert" : "off");
        }
        notifyWSClients();
      }

      if (!doc["blescan"].isNull()) {
        handled = true;
        /* 20 seconds, not 8.

           A shunt at the edge of range gets an advert through every twenty or
           thirty seconds, not once a second, so an eight-second window was a
           coin toss: measured on a bench shunt at -94 dBm, two scans in a row
           found nothing while the live path was receiving perfectly well. The
           result was "No Victron devices heard", which reads as "your shunt is
           not there" rather than "ask again". Waiting longer costs nothing but
           the wait, and only happens when someone presses Scan. */
        if (!VictronBle.StartDiscovery(20))
          wsclient->printf("{\"ERROR\" : \"Could not start Bluetooth\"}");
        notifyWSClients();
      }

      /* The primary source and its fallback are saved by two handlers that have
         to agree about three things, so both are written the same way: neither
         may be set to what the other already is, both consult VictronBle to
         decide whether a reboot is really needed, and both resubscribe.

         The reboot test is "BLE is wanted and its radio is not running", not
         "the setting mentions BLE". Only BLE needs a restart at all, because
         its radio and stack are brought up once at boot to keep the RAM off an
         install that does not use it - serial is always running and MQTT is
         four subscriptions, so those take effect on the next pass of loop().
         Asking VictronBle.Enabled() rather than comparing old and new values
         gets the cases where BLE was already up right: moving it from primary
         to fallback, or the other way, changes nothing about the radio and
         should not ask for a restart nothing is waiting on. */
      if (!doc["shuntsource"].isNull()) {
        uint8_t value = doc["shuntsource"];
        handled = true;
        const uint8_t was = shuntSource;
        /* Against the fallback this message is ASKING for, not the one stored -
           otherwise swapping the two in one go is rejected on the strength of a
           value the same message is about to overwrite two handlers further
           down. The fallback handler needs no equivalent: it runs after this
           one, so shuntSource is already the new primary by the time it looks. */
        const uint8_t effFallback = doc["fallbacksource"].isNull()
                                      ? fallbackSource : (uint8_t)doc["fallbacksource"];
        if (value != SHUNT_SRC_VEDIRECT && value != SHUNT_SRC_BLE && value != SHUNT_SRC_MQTT) {
          wsclient->printf("{\"ERROR\" : \"Invalid shunt source\"}");
        } else if (value == SHUNT_SRC_BLE && !VictronBle.HardwareSupported()) {
          wsclient->printf("{\"ERROR\" : \"This board has no PSRAM - Victron BLE is not available\"}");
        } else if (value == effFallback) {
          wsclient->printf("{\"ERROR\" : \"Shunt Source cannot be the same as the Fallback Source\"}");
        } else {
          pref.putUInt8(ccShuntSource, value);
          shuntSource = value;
          const bool needsReboot = (shuntSource == SHUNT_SRC_BLE ||
                                    fallbackSource == SHUNT_SRC_BLE) && !VictronBle.Enabled();
          WS_LOG_I("Shunt source set to %s%s", ShuntSrcName(value),
                   needsReboot ? " (reboot to apply)" : "");
          // The four shunt topics are only subscribed while MQTT is in play in
          // one role or the other, so a change either way has to reach the
          // broker now.
          if (was != value) mqttResubscribeTemp();
        }
        notifyWSClients();
      }

      if (!doc["fallbacksource"].isNull()) {
        uint8_t value = doc["fallbacksource"];
        handled = true;
        if (value != SHUNT_FALLBACK_NONE && value != SHUNT_SRC_VEDIRECT &&
            value != SHUNT_SRC_BLE && value != SHUNT_SRC_MQTT) {
          wsclient->printf("{\"ERROR\" : \"Invalid fallback source\"}");
        } else if (value == SHUNT_SRC_BLE && !VictronBle.HardwareSupported()) {
          wsclient->printf("{\"ERROR\" : \"This board has no PSRAM - Victron BLE is not available\"}");
        } else if (value == shuntSource) {
          wsclient->printf("{\"ERROR\" : \"Fallback source cannot be the same as the primary Shunt Source\"}");
        } else {
          pref.putUInt8(ccFallbackSrc, value);
          fallbackSource = value;   // takes effect immediately, unlike the radio
          const bool needsReboot = (shuntSource == SHUNT_SRC_BLE ||
                                    fallbackSource == SHUNT_SRC_BLE) && !VictronBle.Enabled();
          WS_LOG_I("Fallback source set to %s%s", ShuntSrcName(value),
                   needsReboot ? " (reboot to apply)" : "");
          mqttResubscribeTemp();
        }
        notifyWSClients();
      }

      if (!doc["blemac"].isNull()) {
        String value = doc["blemac"];
        handled = true;
        pref.putString(ccVBLEMac, value);
        VictronBle.SetMac(value);
        WS_LOG_I("Victron BLE device set to %s", value.c_str());
        notifyWSClients();
      }

      if (!doc["blekey"].isNull()) {
        String value = doc["blekey"];
        handled = true;
        VictronBle.SetKeyHex(value);
        if (VictronBle.HaveKey()) {
          pref.putString(ccVBLEKey, value);
          WS_LOG_I("Victron BLE encryption key stored");
        } else {
          // Not saved, so a mistyped key cannot quietly replace a working one
          wsclient->printf("{\"ERROR\" : \"Key must be 32 hex characters\"}");
        }
        notifyWSClients();
      }

      if (!doc["fanpin"].isNull()) {
        uint8_t value = doc["fanpin"];
        if (value == 0) {
          handled = true;
          pref.putUInt8(ccFanPin,value);
          FanDeinit();
          WS_LOG_I("Fan disabled (fanpin cleared)");
          notifyWSClients();
        } else if (IsForbiddenPin(value)) {
          wsclient->printf("{\"ERROR\" : \"fanpin %u is forbidden\"}", value);
          handled = true;
        } else if (const char* conflict = FindPinConflict(value, "fanpin")) {
          wsclient->printf("{\"ERROR\" : \"fanpin %u already used by %s\"}", value, conflict);
          handled = true;
        } else {
          handled = true;
          pref.putUInt8(ccFanPin,value);
          if (FAN_INIT)
            FanDeinit();
          FanInit(value);
          notifyWSClients();
        }
      }

      if (!doc["onewirepin"].isNull()) {
        uint8_t value = doc["onewirepin"];
        if (value == 0) {
          log_w("Ignoring onewirepin value 0");
          handled = true;
        } else if (IsForbiddenPin(value)) {
          wsclient->printf("{\"ERROR\" : \"onewirepin %u is forbidden\"}", value);
          handled = true;
        } else if (const char* conflict = FindPinConflict(value, "onewirepin")) {
          wsclient->printf("{\"ERROR\" : \"onewirepin %u already used by %s\"}", value, conflict);
          handled = true;
        } else {
          handled = true;
          pref.putUInt8(ccOneWirePin,value);
          notifyWSClients();
        }
      }

      if (!doc["autocharge"].isNull()) {
        bool value = doc["autocharge"];
        handled = true;
        Inverter.AutoCharge(value);
        pref.putBool(ccAutoAdjustCharge,value);  // Save to NVS when changed via Web UI
        notifyWSClients();}

      if (!doc["can16mhz"].isNull()) {
        bool value = (bool) doc["can16mhz"];
        handled = true;
        log_i("CAN Speed Change: %i", value);
        pref.putBool(ccCAN16Mhz,value);
        notifyWSClients();}

      if (!doc["smartinterval"].isNull()) {
        uint8_t value = (uint8_t) doc["smartinterval"];
        handled = true;
        pref.putUInt8(ccSmartInterval,value);
        Inverter.SmartInterval(value);
        notifyWSClients();}

      if (!doc["tailcurrent"].isNull()) {
        pref.putUInt32(ccTailCurrent,(uint32_t) doc["tailcurrent"]);
        Inverter.SetTailCurrentmA((uint32_t) doc["tailcurrent"]);
        WS_LOG_I("Set Tail Current to %u mA", (uint32_t) doc["tailcurrent"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["tailduration"].isNull()) {
        pref.putUInt16(ccTailDuration,(uint16_t) doc["tailduration"]);
        Inverter.SetTailCurrentDuration((uint16_t) doc["tailduration"]);
        WS_LOG_I("Set Tail Duration to %u s", (uint16_t) doc["tailduration"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["maxabsorptiontime"].isNull()) {
        pref.putUInt16(ccMaxAbsTime,(uint16_t) doc["maxabsorptiontime"]);
        Inverter.SetMaxAbsorptionTime((uint16_t) doc["maxabsorptiontime"]);
        WS_LOG_I("Set Max Absorption Time to %u min", (uint16_t) doc["maxabsorptiontime"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["rechargesoc"].isNull()) {
        pref.putUInt8(ccRechargeSOC,(uint8_t) doc["rechargesoc"]);
        Inverter.SetRechargeSOC((uint8_t) doc["rechargesoc"]);
        WS_LOG_I("Set Recharge SOC to %u%%", (uint8_t) doc["rechargesoc"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["rechargevoltageoffset"].isNull()) {
        pref.putUInt16(ccRechargeVOff,(uint16_t) doc["rechargevoltageoffset"]);
        Inverter.SetRechargeVoltageOffset((uint16_t) doc["rechargevoltageoffset"]);
        WS_LOG_I("Set Recharge Voltage Offset to %u mV", (uint16_t) doc["rechargevoltageoffset"]);
        handled = true;
        notifyWSClients(); }
      /* Live current setpoints from an external controller. These are commands,
         not settings: deliberately no pref.put* on any path, whatever `persist`
         says, because they arrive as often as every 30 seconds and NVS has a
         finite write budget. They are also not bound by Min Charge - 0 is a
         valid instruction meaning "do not charge now", not a broken config. */
      if (!doc["requestedchargecurrent"].isNull()) {
        uint32_t mA = (uint32_t) doc["requestedchargecurrent"];
        Inverter.RequestChargeCurrent(mA);
        WS_LOG_I("Charge current request %u mA (effective %u mA of %u configured)",
                 mA, Inverter.EffectiveMaxChargeCurrent(), Inverter.GetMaxChargeCurrent());
        // Worth saying out loud: this is the combination that makes some hybrid
        // inverters discharge the pack to shed power they cannot put anywhere.
        if (Inverter.EffectiveMaxChargeCurrent() == 0 &&
            Inverter.ChargeEnable() && Inverter.ManualAllowCharge())
          WS_LOG_W("Charge limit is 0 A but charging is still enabled - send "
                   "manualallowcharge:false as well to stop cleanly");
        handled = true;
        notifyWSClients(); }
      if (!doc["requesteddischargecurrent"].isNull()) {
        uint32_t mA = (uint32_t) doc["requesteddischargecurrent"];
        Inverter.RequestDischargeCurrent(mA);
        WS_LOG_I("Discharge current request %u mA (effective %u mA of %u configured)",
                 mA, Inverter.EffectiveDischargeCurrent(), Inverter.GetMaxDischargeCurrent());
        handled = true;
        notifyWSClients(); }
      // Hand both back to the configured ceilings now rather than waiting out the timeout
      if (!doc["clearcurrentrequests"].isNull()) {
        if (doc["clearcurrentrequests"]) {
          Inverter.ClearCurrentRequests();
          WS_LOG_I("Current requests cleared, configured ceilings back in force");
        }
        handled = true;
        notifyWSClients(); }
      if (!doc["requesttimeout"].isNull()) {
        uint16_t secs = (uint16_t) doc["requesttimeout"];
        Inverter.SetRequestTimeout(secs);
        if (persist) pref.putUInt16(ccReqTimeout, Inverter.GetRequestTimeout());
        WS_LOG_I("Current request timeout set to %u s%s", Inverter.GetRequestTimeout(),
                 persist ? "" : " (not saved)");
        handled = true;
        notifyWSClients(); }

      // 0 asks for the automatic target; at or above the charge voltage turns the
      // float stage off and the cycle ends at Complete.
      if (!doc["floatvoltage"].isNull()) {
        uint32_t mv = (uint32_t) doc["floatvoltage"];
        pref.putUInt32(ccFloatVoltage, mv);
        Inverter.SetFloatVoltage((uint16_t) mv);
        WS_LOG_I("Set Float Voltage to %u mV - holding %u mV%s", mv,
                 Inverter.ActiveFloatVoltage(),
                 !Inverter.FloatEnabled()      ? " (float stage off)"
                 : Inverter.FloatUsingAutoVoltage() ? " (automatic)" : "");
        handled = true;
        notifyWSClients(); }
      if (!doc["floatcurrent"].isNull()) {
        uint32_t ma = (uint32_t) doc["floatcurrent"];
        pref.putUInt32(ccFloatCurrent, ma);
        Inverter.SetFloatCurrent(ma);
        WS_LOG_I("Set Float Current to %u mA", ma);
        handled = true;
        notifyWSClients(); }

      if (!doc["tempprotection"].isNull()) {
        bool value = doc["tempprotection"];
        pref.putBool(ccTempProtect, value);
        Inverter.TempProtectionEnabled(value);
        handled = true;
        notifyWSClients(); }
      if (!doc["chargehightemp"].isNull()) {
        int16_t value = doc["chargehightemp"];
        pref.putInt16(ccChgHighTemp, value);
        Inverter.SetChargeHighTemp(value);
        handled = true;
        notifyWSClients(); }
      if (!doc["chargelowtemp"].isNull()) {
        int16_t value = doc["chargelowtemp"];
        pref.putInt16(ccChgLowTemp, value);
        Inverter.SetChargeLowTemp(value);
        handled = true;
        notifyWSClients(); }
      if (!doc["dischargehightemp"].isNull()) {
        int16_t value = doc["dischargehightemp"];
        pref.putInt16(ccDisHighTemp, value);
        Inverter.SetDischargeHighTemp(value);
        handled = true;
        notifyWSClients(); }
      if (!doc["dischargelowtemp"].isNull()) {
        int16_t value = doc["dischargelowtemp"];
        pref.putInt16(ccDisLowTemp, value);
        Inverter.SetDischargeLowTemp(value);
        handled = true;
        notifyWSClients(); }
      if (!doc["showtempdashboard"].isNull()) {
        bool value = doc["showtempdashboard"];
        pref.putBool(ccShowTemp, value);
        Inverter.ShowTempOnDashboard(value);
        handled = true;
        notifyWSClients(); }
      if (!doc["batttempsrc"].isNull()) {
        uint8_t value = doc["batttempsrc"];
        pref.putUInt8(ccBattTempSrc, value);
        Inverter.BattTempSource(value);
        WS_LOG_I("Battery temp source set to: %s", value == 0 ? "VE.Direct" : "MQTT");
        mqttResubscribeTemp();
        handled = true;
        notifyWSClients(); }
      if (!doc["fantempsrc"].isNull()) {
        uint8_t value = doc["fantempsrc"];
        pref.putUInt8(ccFanTempSrc, value);
        Inverter.FanTempSource(value);
        WS_LOG_I("Fan temp source set to: %s", value == 0 ? "Disabled" : "MQTT Inverter");
        mqttResubscribeTemp();
        handled = true;
        notifyWSClients(); }
      if (!doc["mqttbatttopic"].isNull()) {
        String value = doc["mqttbatttopic"].as<String>();
        pref.putString(ccMQTTBattTopic, value);
        sMqttBattTopic = value;
        WS_LOG_I("MQTT battery temp topic set to: %s", value.c_str());
        mqttResubscribeTemp();
        handled = true;
        notifyWSClients(); }
      if (!doc["mqttinvtopic"].isNull()) {
        String value = doc["mqttinvtopic"].as<String>();
        pref.putString(ccMQTTInvTopic, value);
        sMqttInvTopic = value;
        WS_LOG_I("MQTT inverter temp topic set to: %s", value.c_str());
        mqttResubscribeTemp();
        handled = true;
        notifyWSClients(); }

      /* The four shunt-source topics.

         Refused rather than saved if the topic is the device's own base topic
         or anything under it. sendVE2MQTT() publishes this board's V, I and SOC
         there, so pointing an input topic at it wires the output back into the
         input: the device would read its own last published figure, republish
         it, and hold whatever value it had when the real source went away -
         forever, and looking perfectly healthy while it did. That is worth an
         error message rather than a line in the documentation.

         Each handler also clears the matching Have flag. The stashed value and
         its timestamp belong to the old topic, and a reading latched from a
         topic that is no longer configured is not evidence about the new one -
         leaving it set would let a source read as fresh on data that can never
         be refreshed. */
      {
        const String baseTopic = wifiManager.GetMQTTTopic();
        auto shuntTopicOK = [&](const String& v) -> bool {
          if (v.length() == 0) return true;             // clearing is always allowed
          if (baseTopic.length() == 0) return true;     // nothing to collide with
          if (v == baseTopic || v.startsWith(baseTopic + "/")) {
            wsclient->printf("{\"ERROR\" : \"That is this device's own topic - it would read back its own output\"}");
            return false;
          }
          return true;
        };

        if (!doc["mqttshuntsoc"].isNull()) {
          String value = doc["mqttshuntsoc"].as<String>();
          handled = true;
          if (shuntTopicOK(value)) {
            pref.putString(ccMQShuntSOC, value);
            sMqttShuntSOC = value;
            MqttShunt.HaveSOC = false;
            WS_LOG_I("MQTT shunt SOC topic set to: %s", value.c_str());
            mqttResubscribeTemp();
          }
          notifyWSClients(); }

        if (!doc["mqttshuntvolt"].isNull()) {
          String value = doc["mqttshuntvolt"].as<String>();
          handled = true;
          if (shuntTopicOK(value)) {
            pref.putString(ccMQShuntVolt, value);
            sMqttShuntVolt = value;
            MqttShunt.HaveVoltage = false;
            WS_LOG_I("MQTT shunt voltage topic set to: %s", value.c_str());
            mqttResubscribeTemp();
          }
          notifyWSClients(); }

        if (!doc["mqttshuntcurr"].isNull()) {
          String value = doc["mqttshuntcurr"].as<String>();
          handled = true;
          if (shuntTopicOK(value)) {
            pref.putString(ccMQShuntCurr, value);
            sMqttShuntCurr = value;
            MqttShunt.HaveCurrent = false;
            WS_LOG_I("MQTT shunt current topic set to: %s", value.c_str());
            mqttResubscribeTemp();
          }
          notifyWSClients(); }

        if (!doc["mqttshunttemp"].isNull()) {
          String value = doc["mqttshunttemp"].as<String>();
          handled = true;
          if (shuntTopicOK(value)) {
            pref.putString(ccMQShuntTemp, value);
            sMqttShuntTemp = value;
            MqttShunt.HaveTemp = false;
            WS_LOG_I("MQTT shunt temperature topic set to: %s", value.c_str());
            mqttResubscribeTemp();
          }
          notifyWSClients(); }
      }
      if (!doc["fanofftemp"].isNull()) {
        int16_t value = doc["fanofftemp"];
        pref.putInt16(ccFanOffTemp, value);
        Inverter.SetFanOffTemp(value);
        WS_LOG_I("Fan off temp set to: %d C", value);
        handled = true;
        notifyWSClients(); }
      if (!doc["fanfulltemp"].isNull()) {
        int16_t value = doc["fanfulltemp"];
        pref.putInt16(ccFanFullTemp, value);
        Inverter.SetFanFullTemp(value);
        WS_LOG_I("Fan full temp set to: %d C", value);
        handled = true;
        notifyWSClients(); }

      if (!doc["reboot"].isNull()) {
        if(doc["reboot"])
        {
        //  ws.textAll("{ \"Message\" : \"Rebooting now\" }");
        //  delay(25);
          WS_LOG_I("Rebooting as requested via WebSocket");
          ws.closeAll();
          delay(25);
          handled = true;
          Lcd.ClearScreen();
          ESP.restart();
        }
        else 
        {
          handled = true;
          wsclient->printf("{ \"Message\" : \"To reboot send value true. i.e. {\"reboot\":true } \"}");
        }
      }    
      if (!doc["saveall"].isNull()){
        if(doc["saveall"]){

          pref.putUInt32(ccDischargeCurrent,Inverter.GetMaxDischargeCurrent());
          pref.putUInt32(ccBattCapacity,Inverter.GetBatteryCapacity());
          pref.putUInt8(ccLowSOCLimit,Inverter.GetLowSOCLimit());
          pref.putUInt8(ccHighSOCLimit,Inverter.GetHighSOCLimit());
          pref.putUInt8(ccSlowSOCCharge1,Inverter.GetSlowChargeSOCLimit(1));
          pref.putUInt8(ccSlowSOCCharge2,Inverter.GetSlowChargeSOCLimit(2));
          pref.putUInt8(ccSlowSOCDivider1,Inverter.GetSlowChargeDivider(1));
          pref.putUInt8(ccSlowSOCDivider2,Inverter.GetSlowChargeDivider(2));
          pref.putUInt32(ccTailCurrent,Inverter.GetTailCurrentmA());
          pref.putUInt16(ccTailDuration,Inverter.GetTailCurrentDuration());
          pref.putUInt16(ccMaxAbsTime,Inverter.GetMaxAbsorptionTime());
          pref.putUInt8(ccRechargeSOC,Inverter.GetRechargeSOC());
          pref.putUInt16(ccRechargeVOff,Inverter.GetRechargeVoltageOffset());
          pref.putUInt32(ccFloatVoltage,Inverter.GetFloatVoltage());
          pref.putUInt32(ccFloatCurrent,Inverter.GetFloatCurrent());
          pref.putUInt16(ccReqTimeout,Inverter.GetRequestTimeout());
          log_d("Save all completed.");
          handled = true;
        }
      }

      if (!doc["eraseall"].isNull()){
        if(doc["eraseall"]){
          pref.clear(true);
          handled = true;
          ESP.restart();
        }
      }
      
      if (!doc["erasekeepwifi"].isNull()){
        if(doc["erasekeepwifi"]){
          WS_LOG_I("Erasing all preferences except WiFi settings as requested via WebSocket");
          pref.clear(false);
          handled = true;
          ESP.restart();
        }
      }

      /* Optional features get the message last, after every built-in key has
         had its turn, so a feature cannot shadow a core setter. Offered the
         message even when something above already claimed it: the settings page
         batches several keys into one update, and a feature's key can ride
         along with core ones. */
      if (Feature::HandleWSAll(doc)) {
        handled = true;
        notifyWSClients();
      }

      if (!handled)
        wsclient->printf("{ \"Message\" : \"ERROR: Unknown Request\" }");
    }   

}

// Web Socket Handler
void onEvent(AsyncWebSocket * wsserver, AsyncWebSocketClient * wsclient, AwsEventType type, void * arg, uint8_t *data, size_t len){
  if(type == WS_EVT_CONNECT){
    //client connected
    log_i("ws[%s][%u] connected", wsserver->url(), wsclient->id());
    WS_LOG_I("WebSocket Client %u connected", wsclient->id());
    //wsclient->printf("Your Client %u :)", wsclient->id());
    wsclient->ping();
    // What a tab costs, which is the measurement that matters here - four of
    // them took this board from 167KB free to a 4.6KB largest block
    Diag.Milestone("WS client connected");
  } else if(type == WS_EVT_DISCONNECT){
    //client disconnected
    log_i("ws[%s][%u] disconnect: %u", wsserver->url(), wsclient->id());
  } else if(type == WS_EVT_ERROR){
    //error was received from the other end
    log_d("ws[%s][%u] error(%u): %s", wsserver->url(), wsclient->id(), *((uint16_t*)arg), (char*)data);
  } else if(type == WS_EVT_PONG){
    //pong message was received (in response to a ping request maybe)
    log_i("ws[%s][%u] pong[%u]: %s", wsserver->url(), wsclient->id(), len, (len)?(char*)data:"");
    log_i("Sending All Data to All WS Clients");
    notifyWSClients();
  } else if(type == WS_EVT_DATA){
    //data packet
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    if(info->final && info->index == 0 && info->len == len){
      //the whole message is in a single frame and we got all of it's data
      log_d("ws[%s][%u] %s-message[%llu]: ", wsserver->url(), wsclient->id(), (info->opcode == WS_TEXT)?"text":"binary", info->len);
      if(info->opcode == WS_TEXT){
        data[len] = 0;
        log_d("%s\n", (char*)data);
        handleWSRequest(wsclient, (char *)data, info->len);
      } 
    } 
  }
}

// Helper function to send embedded HTML from PROGMEM.
// The payload is gzip-compressed at build time by embed_html.py, so it must go out
// with Content-Encoding: gzip for the browser to inflate it.
void sendEmbeddedHTML(AsyncWebServerRequest *request) {
  /* Bracketed with heap readings because this is the prime suspect for the
     deepest troughs measured on this board: a ~41KB dip with a browser in use
     and no MQTT activity anywhere near it, on a device where a WebSocket client
     costs 40 bytes. The page is ~53KB gzipped and goes out of PROGMEM through
     TCP buffers, which is the only thing in the firmware big enough to explain
     it. Suspicion is not measurement, so - measure it.

     Serial only and no allocation of its own, since the point is to observe a
     moment when there may be very little left to allocate from. */
  const uint32_t freeBefore  = ESP.getFreeHeap();
  const uint32_t blockBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  AsyncWebServerResponse *response =
      request->beginResponse(200, "text/html; charset=utf-8", EMBEDDED_HTML, EMBEDDED_HTML_LEN);
  response->addHeader("Content-Encoding", "gzip");
  // The page changes with every firmware build and carries no ETag, so without
  // this a browser can serve a cached copy after an update - including the
  // location.reload() the OTA flow performs, which would look like a failed
  // update. It is 18KB over LAN, so there is nothing worth caching.
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  request->send(response);

  /* Taken straight after send(), which queues the response rather than
     completing it - so this catches what setting it up cost, not the peak
     while it drains. If the trough is really in the transfer, the low-water
     line from Diag will report deeper than this does, and the gap between the
     two is the answer. */
  const uint32_t freeAfter  = ESP.getFreeHeap();
  const uint32_t blockAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  Serial.printf("[page] %u B page queued: free %u -> %u (%+d), largest %u -> %u\r\n",
                (unsigned)EMBEDDED_HTML_LEN, (unsigned)freeBefore, (unsigned)freeAfter,
                (int)((int32_t)freeAfter - (int32_t)freeBefore),
                (unsigned)blockBefore, (unsigned)blockAfter);
}

void StartWebServices()
{
  log_d("Configuring Web Services.");

  // Helpers: serve embedded HTML
  auto sendIndex = [](AsyncWebServerRequest *request){
    log_i("Serving Embedded HTML Page");
    sendEmbeddedHTML(request);
  };
  
  server.on("/", HTTP_GET, sendIndex);
  server.on("/index.htm", HTTP_GET, sendIndex);

  // Windows NCSI: serve plain text responses to stabilize connectivity classification
  server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "Microsoft Connect Test");
  });
  
  server.on("/redirect", HTTP_GET, sendIndex);
  server.on("/generate_204", HTTP_GET, sendIndex);
  server.on("/hotspot-detect.html", HTTP_GET, sendIndex);
  server.on("/library/test/success.html", HTTP_GET, sendIndex);
  server.on("/kindle-wifi/wifistub.html", HTTP_GET, sendIndex);
  server.on("/mobile/status.php", HTTP_GET, sendIndex);
  server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "Microsoft NCSI");
  });
  
  server.on("/success.txt", HTTP_GET, sendIndex);
  server.onNotFound(sendIndex);

  //setup the updateServer with credentials
  updateServer.setup(&server);
  //hook to update events if you need to
  updateServer.onUpdateBegin = [](const UpdateType type, int &result)
  {
    // Firmware updates only - filesystem OTA disabled
    if (type == UpdateType::FILE_SYSTEM) {
      log_w("Filesystem OTA disabled - embedded HTML only");
      result = UpdateResult::UPDATE_ABORT;
    }
    //you can force abort the update like this if you need to:
    //result = UpdateResult::UPDATE_ABORT;        
    otaInProgress = true;
    ws.closeAll();
    Serial.println("Update started : " + String(type));
  };
  updateServer.onUpdateEnd = [](const UpdateType type, int &result)
  {
    otaInProgress = false;
    Serial.println("Update finished : " + String(type) + " result: " + String(result));
  };

  // Web Socket handler
  ws.onEvent(onEvent);
  server.addHandler(&ws);
  // Scan network URL call (deprecated, use WebSocket GetWifiScan() instead)
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    String json = "[";
    // WiFi API is thread-safe internally; no critical section needed
    int n = WiFi.scanComplete();

    // Return current results (if any completed scan exists)
    if(n > 0)
    {
      int i = 0;
      for (i = 0; i < n; ++i)
      {
        if(i) json += ",";
        json += "{";
        json += "\"rssi\":"+String(WiFi.RSSI(i));
        // Display copy and exact bytes, as in the WebSocket scan - see the SSID
        // transport note above.
        json += ",\"ssid\":\""+jsonEscape(toDisplayUTF8(WiFi.SSID(i)))+"\"";
        json += ",\"ssidhex\":\""+bytesToHex(WiFi.SSID(i))+"\"";
        json += ",\"bssid\":\""+WiFi.BSSIDstr(i)+"\"";
        json += ",\"channel\":"+String(WiFi.channel(i));
        json += ",\"secure\":"+String(WiFi.encryptionType(i));
        json += "}";
      }
      log_d("Network scan returning %d results",i);
    }

    // Always trigger a new background scan for next request
    if(WiFi.scanComplete() != -1)
    {
      // Only start if no scan is already in progress
      log_d("Starting new background network scan");
      WiFi.scanNetworks(true);
    }
    
    json += "]";
    request->send(200, "application/json", json);
    json = String();
  });
  // Start server
  log_i("Starting Web Server");
  server.begin();
  log_i("Completed Web Services setup.");
}


// This task is scheduled by setup function, 
// once Wifi is connected it starts Web Services
void taskStartWebServices(void * pointer)
{
  //while (!client.isWifiConnected())
  while (!WiFi.isConnected())
  {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  
  log_d("Starting Web Services");
  StartWebServices();
  Lcd.Data.WebServerState.setValue(true);
  vTaskDelete( NULL );
}
  
