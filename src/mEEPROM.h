#ifndef VEDIRECTEEPROM_H
#define VEDIRECTEEPROM_H

// Constants
// no key larger than 15 chars

#include <Arduino.h>
#include <Preferences.h>
#include <nvs_flash.h>
#define RW_MODE true
#define RO_MODE false

/* Longest string NVS can store, terminator included (nvs.h, nvs_set_str). An
   entry claiming more than this cannot have been written by us. */
#define NVS_STRING_MAX 4000

/* NVS hands back whatever bytes are in flash. A half-written entry, a value
   left by an older build, or a corrupted page all read back as a String that
   looks fine to us but is not valid UTF-8 - and MQTT 3.1.1 (1.5.3) requires
   every string in a packet to be well-formed UTF-8, so a broker answers a bad
   client ID, user, password or topic by dropping the connection with nothing
   useful in the log.

   This is Table 3-7 of the Unicode standard rather than a "top bit set" test:
   overlong encodings, UTF-16 surrogates (U+D800-U+DFFF) and anything past
   U+10FFFF are all rejected, because a broker's own validator rejects them. */
// Length of the well-formed sequence starting at p, or 0 if it is not one.
inline int utf8SeqLen(const uint8_t* p, size_t avail) {
  if (avail == 0) return 0;
  const uint8_t c = p[0];
  size_t extra;
  uint8_t lo = 0x80, hi = 0xBF;    // allowed range of the FIRST continuation byte
  if (c < 0x80) return 1;
  else if (c >= 0xC2 && c <= 0xDF) extra = 1;
  else if (c == 0xE0)            { extra = 2; lo = 0xA0; }  // no overlong 3-byte
  else if (c >= 0xE1 && c <= 0xEC) extra = 2;
  else if (c == 0xED)            { extra = 2; hi = 0x9F; }  // no surrogates
  else if (c >= 0xEE && c <= 0xEF) extra = 2;
  else if (c == 0xF0)            { extra = 3; lo = 0x90; }  // no overlong 4-byte
  else if (c >= 0xF1 && c <= 0xF3) extra = 3;
  else if (c == 0xF4)            { extra = 3; hi = 0x8F; }  // stop at U+10FFFF
  else return 0;                   // 0x80-0xC1 and 0xF5-0xFF are never lead bytes
  if (extra >= avail) return 0;                             // truncated sequence
  if (p[1] < lo || p[1] > hi) return 0;
  for (size_t j = 2; j <= extra; j++)
    if (p[j] < 0x80 || p[j] > 0xBF) return 0;
  return (int)(extra + 1);
}

inline bool isValidUTF8(const char* s, size_t len) {
  if (!s) return false;
  const uint8_t* p = (const uint8_t*)s;
  size_t i = 0;
  while (i < len) {
    const int n = utf8SeqLen(p + i, len - i);
    if (n == 0) return false;
    i += (size_t)n;
  }
  return true;
}

inline bool isValidUTF8(const String& s) { return isValidUTF8(s.c_str(), s.length()); }

/* Make a string safe to put in JSON bound for the browser, keeping every
   character that is real text and replacing each byte that is not.

   Needed because a WiFi SSID is allowed to be raw bytes in any encoding (see
   getStringRaw), while a WebSocket text frame is not: RFC 6455 requires the
   browser to FAIL THE CONNECTION on invalid UTF-8, so one Shift_JIS network
   name in the settings would take the whole web UI down rather than just look
   wrong. Only the copy sent for display is changed; what WiFi.begin() gets is
   always the untouched bytes. */
inline String toDisplayUTF8(const String& s) {
  const uint8_t* p = (const uint8_t*)s.c_str();
  const size_t len = s.length();
  String out;
  out.reserve(len);
  size_t i = 0;
  while (i < len) {
    const int n = utf8SeqLen(p + i, len - i);
    if (n == 0) { out += '?'; i++; continue; }
    for (int j = 0; j < n; j++) out += (char)p[i + j];
    i += (size_t)n;
  }
  return out;
}

const char* const ccChargeVolt = "ChargeVolt";
const char* const ccDischargeVolt = "DischargeVolt";
const char* const ccChargeCurrent = "ChargeCurr";
const char* const ccDischargeCurrent = "DischargeCurr";
const char* const ccOverVoltage = "overvoltage";

const char* const ccLowSOCLimit = "LowSOCLimit";
const char* const ccHighSOCLimit = "HighSOCLimit";

const char* const ccSlowSOCCharge1 = "SlowSOCC1";
const char* const ccSlowSOCCharge2 = "SlowSOCC2";
const char* const ccSlowSOCDivider1 = "SlowSOCD1";
const char* const ccSlowSOCDivider2 = "SlowSOCD2";

const char* const ccBattCapacity = "BattCapacity";
const char* const ccPylonTech = "PylonTech";
const char* const ccSOCTrick = "SOCTrick";
const char* const ccRequestFlags = "ReqFlags";

const char* const ccWifiSSID = "WifiSSID";
const char* const ccWifiPass = "WifiPass";
const char* const ccWifiHostName = "WifiHostName";

const char* const ccMQTTServerIP = "MQTTServerIP";
const char* const ccMQTTClientID = "MQTTClientID";
const char* const ccMQTTUser = "MQTTUser";
const char* const ccMQTTPass = "MQTTPass";
const char* const ccMQTTPort = "MQTTPort";
const char* const ccMQTTTopic = "MQTTTopic";
const char* const ccMQTTParam = "MQTTParam";

const char* const ccVictronRX = "VictronRX";
const char* const ccVictronTX = "VictronTX";

/* Shunt data source. 0 = VE.Direct serial (the default, so nothing changes for
   an existing install), 1 = Victron BLE. The fallback only applies when BLE is
   the source: it hands back to serial while BLE data is stale, for a shunt that
   is at the edge of range rather than absent. */
const char* const ccShuntSource = "ShuntSrc";
const char* const ccBLEFallback = "BLEFallback";
const char* const ccVBLEMac = "VBLEMac";
const char* const ccVBLEKey = "VBLEKey";
#define SHUNT_SRC_VEDIRECT 0
#define SHUNT_SRC_BLE      1

const char* const ccCAN_TX_PIN = "CAN_TX_PIN";
const char* const ccCAN_RX_PIN = "CAN_RX_PIN";
const char* const ccCAN_EN_PIN = "CAN_EN_PIN";
const char* const ccCanCSPin = "CAN_CS_PIN";
const char* const ccOneWirePin = "onewirepin";
const char* const ccFanPin = "fanpin";
const char* const ccAutoAdjustCharge = "AutoAdjust";
const char* const ccSmartInterval = "SmartInterval";
const char* const ccAdjustStep = "AdjustStep";
const char* const ccMinCharge = "MinCharge";
const char* const ccMinDischarge = "MinDischarge";
const char* const ccCAN16Mhz = "CAN16Mhz";

// CC-CV Charging Parameters
const char* const ccTailCurrent = "TailCurr";
const char* const ccTailDuration = "TailDuration";
const char* const ccMaxAbsTime = "MaxAbsTime";
const char* const ccRechargeSOC = "RechargeSOC";
const char* const ccRechargeVOff = "RechargeVOff";
const char* const ccFloatVoltage = "FloatVolt";
const char* const ccFloatCurrent = "FloatCurr";

const char* const ccTempProtect = "TempProtect";
const char* const ccChgHighTemp = "ChgHighTemp";
const char* const ccChgLowTemp = "ChgLowTemp";
const char* const ccDisHighTemp = "DisHighTemp";
const char* const ccDisLowTemp = "DisLowTemp";
const char* const ccShowTemp = "ShowTemp";

// MQTT Temperature Subscription & Fan Control
const char* const ccBattTempSrc = "BattTempSrc";
const char* const ccFanTempSrc = "FanTempSrc";
const char* const ccMQTTBattTopic = "MQBattTopic";
const char* const ccMQTTInvTopic = "MQInvTopic";
const char* const ccNever100SOC = "Never100SOC";
const char* const ccPylonVersion = "PylonVer";  // Also used as ccCANProtocol (backward compat)
const char* const ccCANProtocol = "PylonVer";   // Same NVS key: 0-1=Pylontech, 2=SMA, 3=Victron, 4=Growatt
const char* const ccFanOffTemp = "FanOffTemp";
const char* const ccFanFullTemp = "FanFullTemp";

const char* const ccVELOOPTIME = "VE_LOOP_TIME";
const char* const ccCANBusEnabled  = "CANEnabled";
const char* const ccLcdEnabled = "lcdenabled";
const char* const ccNTPServer = "NTPServer";
/* Take the NTP server from DHCP option 42 as well as (or instead of) the one
   typed in. Defaults on: a device with nothing configured currently never sets
   its clock at all, and almost every router hands out an NTP server. */
const char* const ccNTPFromDHCP = "NTPDHCP";
const char* const ccTimeZone = "TimeZone";
const char* const ccSchedule = "Schedule";
const char* const ccOverrideTime = "OvrTimeout";   // remote override latch, seconds. 0 = off
const char* const ccReqTimeout = "ReqTimeout";     // live current request staleness, seconds
const char* const ccSyslogServer = "SyslogSrv";
const char* const ccSyslogPort = "SyslogPort";
const char* const ccSyslogEnabled = "SyslogEn";
const char* const PREF_NAME = "smartbms";


class mEEPROM {
  public:
    mEEPROM();
    void begin();
    void begin(const char * nvsspace);
    void end();

    Preferences _preferences;
    bool isKey(String key);
    bool clear(bool all);

    String getString(String key, String default_value);
    String getString(const char* key, String default_value);
    /* Same length and corruption checks as getString, but the bytes are handed
       back whatever they are. 802.11 puts no encoding on an SSID - it is 32
       octets of anything - and routers in Japan, China, Korea and parts of
       Europe still broadcast them in Shift_JIS, GBK, EUC-KR or Latin-1. Those
       are not valid UTF-8, and this product ships worldwide, so rejecting them
       would strand a working device in AP mode over a name it never had to
       understand. MQTT is the opposite case: the protocol requires UTF-8, so
       those keys stay on getString. */
    String getStringRaw(const char* key, String default_value);
    bool putString(String key, String value);
    bool putString(const char* key, String value);

    int32_t getInt32(const char* key, int32_t default_value);
    bool putInt32(const char* key, int32_t value);
    uint32_t getUInt32(const char* key, uint32_t default_value);
    bool putUInt32(const char* key, uint32_t value);

    int16_t getInt16(const char* key, int16_t default_value);
    bool putInt16(const char* key, int16_t value);
    uint16_t getUInt16(const char* key, uint16_t default_value);
    bool putUInt16(const char* key, uint16_t value);

    int8_t getInt8(const char* key, int8_t default_value);
    bool putInt8(const char* key, int8_t value);
    uint8_t getUInt8(const char* key, uint8_t default_value);
    bool putUInt8(const char* key, uint8_t value);

    bool getBool(String key, boolean default_value);
    bool putBool(String key, boolean value);
    bool getBool(const char* key, boolean default_value);
    bool putBool(const char* key, boolean value);
    uint16_t freeentries();
};


#endif






