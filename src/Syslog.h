#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

/*
   Copyright (c) 2022-2026 Nexion Software Solutions Ltd - https://nexion.uk

   Minimal RFC 3164 syslog over UDP.

   Hooked into sendLogToWS(), which every WS_LOG_* macro already funnels through,
   so enabling this forwards the same lines the web Logs tab shows - no extra call
   sites needed. Sending is fire-and-forget: UDP with no retry, so a missing or
   unreachable collector can never block or slow the control loops.
*/
class SyslogSender {
public:
  // server may be blank or an unresolved name; only a literal IP is accepted so
  // that logging never triggers a blocking DNS lookup from a real-time path.
  // Applies immediately - every field takes effect on the next log line, with no
  // reboot. Destination IP and port are supplied per-packet, so only the local
  // socket needs managing here.
  void configure(const char* server, uint16_t port, bool enabled,
                 const char* hostname) {
    _port = port ? port : 514;
    // In practice the caller passes the device's WiFi hostname, so this
    // fallback only applies before one is set. Same name as everywhere else, so
    // a line in a collector's log is attributable to the same device a browser
    // and a broker would name.
    _hostname = (hostname && *hostname) ? hostname : "diy-battery-bms";
    _hostname.replace(' ', '-');            // RFC 3164 HOSTNAME cannot contain spaces
    _valid = (server && *server) ? _ip.fromString(server) : false;
    _enabled = enabled && _valid;

    // Release the socket and its 1460-byte tx buffer while disabled, rather than
    // holding them for a feature that is switched off.
    if (!_enabled && _started) {
      _udp.stop();
      _started = false;
    }
  }

  bool enabled() const { return _enabled; }
  bool configured() const { return _valid; }

  void log(const char* msg, const char* level) {
    if (!_enabled || !msg) return;
    if (WiFi.status() != WL_CONNECTED) return;

    if (!_started) {
      if (!_udp.begin(0)) return;           // ephemeral local port
      _started = true;
    }

    // PRI = facility(1, user) * 8 + severity
    uint8_t sev = 6;                        // info
    switch (level[0]) {
      case 'e': sev = 3; break;             // error
      case 'w': sev = 4; break;             // warning
      case 'd': sev = 7; break;             // debug
      default:  sev = 6; break;
    }

    // Timestamp. Before NTP sync time() is meaningless, so fall back to a fixed
    // stamp rather than emitting a wildly wrong date - collectors handle this by
    // substituting their own receive time.
    char stamp[17] = "Jan  1 00:00:00";
    time_t now = time(nullptr);
    if (now > 1600000000) {                 // sane epoch => clock has been set
      struct tm t;
      localtime_r(&now, &t);
      strftime(stamp, sizeof(stamp), "%b %e %H:%M:%S", &t);
    }

    char packet[288];
    int n = snprintf(packet, sizeof(packet), "<%u>%s %s bms: %s",
                     (unsigned)(8 + sev), stamp, _hostname.c_str(), msg);
    if (n <= 0) return;
    if (n >= (int)sizeof(packet)) n = sizeof(packet) - 1;

    if (_udp.beginPacket(_ip, _port)) {
      _udp.write((const uint8_t*)packet, (size_t)n);
      _udp.endPacket();
    }
  }

private:
  WiFiUDP   _udp;
  IPAddress _ip;
  String    _hostname = "diy-battery-bms";
  uint16_t  _port = 514;
  bool      _enabled = false;
  bool      _valid = false;
  bool      _started = false;
};

extern SyslogSender Syslog;
