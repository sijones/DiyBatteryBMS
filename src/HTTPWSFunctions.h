#pragma once

#include <ArduinoJson.h>
#include <WiFi.h>
#include <Update.h>
#include "config.h"
#include "embedded_html.h"
#include "Syslog.h"
#include "Schedule.h"
#include "GPIOForbidden.h"

volatile bool otaInProgress = false;

// Forward declarations for MQTT temperature subscriptions (defined in mqttFunctions.h)
extern String sMqttBattTopic;
extern String sMqttInvTopic;
void mqttResubscribeTemp();
// Re-publish HA discovery (defined in mqttFunctions.h) so number-control limits track config changes
void publishHADiscovery();

// Log buffer for web UI
#define LOG_BUFFER_SIZE 100
struct LogEntry {
  char message[201];  // max 200 chars + null terminator
  char level[10];
  unsigned long timestamp;
};
LogEntry logBuffer[LOG_BUFFER_SIZE];
int logBufferIndex = 0;
portMUX_TYPE logMutex = portMUX_INITIALIZER_UNLOCKED;

// WiFi scan state tracking
int lastWifiScanCount = -2;  // -2 = no scan in progress, -1 = scan in progress, >=0 = completed with count
portMUX_TYPE wifiScanMutex = portMUX_INITIALIZER_UNLOCKED;
bool wifiScanRequested = false;  // Track if scan was requested to trigger background scan

SyslogSender Syslog;
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
  Syslog.configure(pref.getString(ccSyslogServer, "").c_str(),
                   pref.getUInt16(ccSyslogPort, 514),
                   pref.getBool(ccSyslogEnabled, false),
                   wifiManager.GetWifiHostName().c_str());
}

// Function to send log to WebSocket clients
void sendLogToWS(const char* message, const char* level) {
  // Every WS_LOG_* macro funnels through here, so syslog picks up all of them
  // without needing a hook at each of the ~96 call sites.
  Syslog.log(message, level);

  // Only send if WebSocket is initialized and has connected clients
  if(ws.count() > 0 && ws.availableForWriteAll()) {
    String json = "{\"log\":\"";
    String msg = String(message).substring(0, 200);  // Limit message length
    // Escape characters that break JSON strings
    msg.replace("\\", "\\\\");
    msg.replace("\"", "'");
    msg.replace("\n", "\\n");
    msg.replace("\r", "\\r");
    msg.replace("\t", "\\t");
    json += msg;
    json += "\",\"level\":\"";
    json += level;
    json += "\"}";
    ws.textAll(json);
  }
  
  // Also store in circular buffer (strncpy is safe inside critical section - no heap alloc)
  taskENTER_CRITICAL(&logMutex);
  strncpy(logBuffer[logBufferIndex].message, message, 200);
  logBuffer[logBufferIndex].message[200] = '\0';
  strncpy(logBuffer[logBufferIndex].level, level, 9);
  logBuffer[logBufferIndex].level[9] = '\0';
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
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

void TaskSetClock(void * pointer) {
  
  log_d("Entering TaskSetClock");
  applyTimeZone();
  String Servers = pref.getString(ccNTPServer,"");
    // Return if no servers set.
  if (Servers.length()==0)
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

  if (ServerArray[0].length() == 0) {
    WS_LOG_W("NTP server setting is not a usable address, clock will not be set");
    vTaskDelete(NULL);
    return;
  }

  String tz = pref.getString(ccTimeZone, initTimeZone);
  if (tz.length() == 0) tz = initTimeZone;

  // Register before starting SNTP so the very first sync is not missed
  sntp_set_time_sync_notification_cb(ntpSyncCallback);

  if (secondserver) {
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
      WS_LOG_E("No NTP response from %s after %us - check the address is reachable and UDP/123 is open",
               ServerArray[0].c_str(), (unsigned)(NTP_SYNC_TIMEOUT_MS / 1000));
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

String generateDatatoJSON(bool All)
{
  JsonDocument doc;

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
    doc["wifissid"] = wifiManager.GetWifiSSID();
    doc["wifipass"] = wifiManager.GetWifiPass();
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
    doc["timezone"] = pref.getString(ccTimeZone, initTimeZone);
#ifndef DISABLE_SCHEDULER
    Schedule.uiToJson(doc["schedui"].to<JsonArray>());
    Schedule.mqttToJson(doc["schedmqtt"].to<JsonArray>());
#endif
    doc["syslogserver"] = pref.getString(ccSyslogServer,"");
    doc["syslogport"] = pref.getUInt16(ccSyslogPort, 514);
    doc["syslogenabled"] = pref.getBool(ccSyslogEnabled, false);
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
    doc["mqttbatttopic"] = pref.getString(ccMQTTBattTopic, "");
    doc["mqttinvtopic"] = pref.getString(ccMQTTInvTopic, "");
    doc["fanofftemp"] = Inverter.GetFanOffTemp();
    doc["fanfulltemp"] = Inverter.GetFanFullTemp();
    doc["fwversion_bms"] = FW_VERSION;
    doc["fwbuild"] = FW_BUILD;
    #ifdef ESPCAN
    doc["can_tx_pin"] = pref.getUInt8(ccCAN_TX_PIN, 0);
    doc["can_rx_pin"] = pref.getUInt8(ccCAN_RX_PIN, 0);
    doc["can_en_pin"] = pref.getUInt8(ccCAN_EN_PIN, 0);
    #endif
  }

  doc["RealTime"] = true;
  taskENTER_CRITICAL(&(Inverter.CANMutex));
  doc["battsoc"] = Inverter.BattSOC();
  doc["battvoltage"] = Inverter.BattVoltage();
  doc["battcurrent"] = Inverter.BattCurrentmA();
  doc["battpower"] = Inverter.BattPower();
  doc["batttemp"] = Inverter.BattTemp();
  doc["timetogo"] = Inverter.TimeToGo();
  doc["alarmactive"] = Inverter.AlarmActive();
  doc["alarmreason"] = Inverter.AlarmReason();
  doc["pidstring"] = Inverter.PIDString();
  doc["fwversion"] = Inverter.FWVersion();
  doc["serialnumber"] = Inverter.SerialNumber();
  doc["modelstring"] = Inverter.ModelString();
  doc["chargevoltage"] = Inverter.GetChargeVoltage();
  taskEXIT_CRITICAL(&(Inverter.CANMutex));
  
  doc["chargeadjust"] = Inverter.GetChargeAdjust();
  doc["chargeenabled"] = (Inverter.ChargeEnable() && Inverter.ManualAllowCharge()) ? true : false;
  doc["dischargeenabled"] = (Inverter.DischargeEnable() && Inverter.ManualAllowDischarge()) ? true : false;
  doc["forcecharge"] = Inverter.ForceCharge();
  doc["autocharge"] = Inverter.AutoCharge();  // smart-charge state for HA "Smart Charge Status" binary sensor
  doc["chargecurrent"] = Inverter.GetChargeCurrent();
  doc["dischargecurrent"] = Inverter.GetDischargeCurrent();
  doc["maxdischargecurrent"] = Inverter.GetMaxDischargeCurrent();
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
  }
#endif
  // -1 = never synced. -2 = no NTP server configured, so nothing will sync.
  doc["ntpsyncago"] = (pref.getString(ccNTPServer,"").length() == 0)
                        ? -2 : ntpSecondsSinceSync();
  doc["inverterpresent"] = Inverter.InverterPresent();
  doc["victrondata"] = Lcd.Data.VEData.getValue();
  doc["mqttconnected"] = Lcd.Data.MQTTConnected.getValue();
  doc["mqttinvertertemp"] = Inverter.MqttInverterTemp();
  doc["mqttbatttemp"] = Inverter.MqttBattTemp();
  doc["fanpwm"] = FAN_PWM;
  doc["totalheap"] = ESP.getHeapSize();
  doc["freeheap"] = ESP.getFreeHeap();
  
  String outputJson;
  int b = serializeJson(doc, outputJson);
  outputJson.trim();  // Remove trailing newline and whitespace from serializeJson
  return outputJson;
}

void notifyWSClients(bool sendalldata = true) {
  if(otaInProgress) return;
  ws.cleanupClients();
  if(ws.count()>0 && ws.availableForWriteAll())
    ws.textAll(generateDatatoJSON(sendalldata));
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
      // Send buffered logs to requesting client (last 50 entries)
      // Copy logs out of critical section first to avoid blocking
      // static to avoid ~10KB on stack; struct copy is safe (no heap alloc with char arrays)
      static LogEntry tempLogs[50];
      int count = 0;

      taskENTER_CRITICAL(&logMutex);
      for(int i = 0; i < LOG_BUFFER_SIZE && count < 50; i++) {
        int idx = (logBufferIndex + i) % LOG_BUFFER_SIZE;
        if(logBuffer[idx].message[0] != '\0') {
          tempLogs[count] = logBuffer[idx];
          count++;
        }
      }
      taskEXIT_CRITICAL(&logMutex);

      // Send logs outside critical section
      for(int i = 0; i < count; i++) {
        if(wsclient->status() == WS_CONNECTED) {
          char json[256];
          snprintf(json, sizeof(json), "{\"log\":\"%s\",\"level\":\"%s\"}", tempLogs[i].message, tempLogs[i].level);
          wsclient->text(json);

          // Yield every 10 messages to prevent WDT
          if(i % 10 == 0) {
            yield();
          }
        } else {
          break;  // Client disconnected, stop sending
        }
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
      int32_t current = Inverter.BattCurrentmA();
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
        pref.putUInt32(ccChargeVolt,(uint32_t) doc["chargevoltage"]);
        Inverter.SetChargeVoltage((uint32_t) doc["chargevoltage"]); 
        WS_LOG_I("Set Charge Voltage to %u", (uint32_t) doc["chargevoltage"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["overvoltage"].isNull()) {
        pref.putUInt32(ccOverVoltage,(uint32_t) doc["overvoltage"]);
        Inverter.SetOverVoltage((uint32_t) doc["overvoltage"]); 
        WS_LOG_I("Set Over Voltage to %u", (uint32_t) doc["overvoltage"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["dischargevoltage"].isNull()) {
        pref.putUInt32(ccDischargeVolt,(uint32_t) doc["dischargevoltage"]);
        Inverter.SetDischargeVoltage((uint32_t) doc["dischargevoltage"]); 
        WS_LOG_I("Set Discharge Voltage to %u", (uint32_t) doc["dischargevoltage"]);
        handled = true;
        notifyWSClients(); }
      if (!doc["maxchargecurrent"].isNull()) {
        pref.putUInt32(ccChargeCurrent,(uint32_t) doc["maxchargecurrent"]);
        Inverter.SetMaxChargeCurrent((uint32_t) doc["maxchargecurrent"]);
        WS_LOG_I("Set Max Charge Current to %u", (uint32_t) doc["maxchargecurrent"]);
        // Re-publish HA discovery so the Charge Current slider max tracks the new limit
        publishHADiscovery();
        handled = true;
        notifyWSClients(); }
      if (!doc["maxdischargecurrent"].isNull()) {
        pref.putUInt32(ccDischargeCurrent,(uint32_t) doc["maxdischargecurrent"]);
        Inverter.SetMaxDischargeCurrent((uint32_t) doc["maxdischargecurrent"]); 
        WS_LOG_I("Set Max Discharge Current to %u", (uint32_t) doc["maxdischargecurrent"]);
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
      if (!doc["manualallowcharge"].isNull()) {
        Inverter.ManualAllowCharge((bool) doc["manualallowcharge"]);
        WS_LOG_I("Manual Allow Charge set to %s", (bool) doc["manualallowcharge"] ? "true" : "false");
        handled = true;
        notifyWSClients(); }
      if (!doc["manualallowdischarge"].isNull()) {
        Inverter.ManualAllowDischarge((bool) doc["manualallowdischarge"]);
        WS_LOG_I("Manual Allow Discharge set to %s", (bool) doc["manualallowdischarge"] ? "true" : "false");
        handled = true;
        notifyWSClients(); }
      // Low SOC OFF
      if (!doc["lowsoclimit"].isNull()) {
        pref.putUInt8(ccLowSOCLimit,(uint8_t) doc["lowsoclimit"]);
        Inverter.SetLowSOCLimit((uint8_t) doc["lowsoclimit"]);
        WS_LOG_I("Set Low SOC Limit to %u", (uint8_t) doc["lowsoclimit"]);
        handled = true;
        notifyWSClients(); }
      // High SOC Limit
      if (!doc["highsoclimit"].isNull()) {
        pref.putUInt8(ccHighSOCLimit,(uint8_t) doc["highsoclimit"]);
        Inverter.SetHighSOCLimit((uint8_t) doc["highsoclimit"]);
        WS_LOG_I("Set High SOC Limit to %u", (uint8_t) doc["highsoclimit"]);
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
      
      #define HANDLE_PIN(field, prefKey) \
        if (!doc[field].isNull()) { \
          uint8_t _pv = (uint8_t) doc[field]; \
          handled = true; \
          if (_pv == 0) { log_w("Ignoring %s value 0", field); } \
          else if (IsForbiddenPin(_pv)) { wsclient->printf("{\"ERROR\" : \"%s %u is forbidden\"}", field, _pv); } \
          else { pref.putUInt8(prefKey, _pv); notifyWSClients(); } \
        }
      HANDLE_PIN("canbuscspin", ccCanCSPin)
      HANDLE_PIN("victronrxpin", ccVictronRX)
      HANDLE_PIN("victrontxpin", ccVictronTX)
      HANDLE_PIN("can_rx_pin", ccCAN_RX_PIN)
      HANDLE_PIN("can_tx_pin", ccCAN_TX_PIN)
      HANDLE_PIN("can_en_pin", ccCAN_EN_PIN)
      #undef HANDLE_PIN

      if (!doc["wifissid"].isNull()) {
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

      if (!doc["fanpin"].isNull()) {
        uint8_t value = doc["fanpin"];
        if (value == 0) {
          log_w("Ignoring fanpin value 0");
          handled = true;
        } else if (IsForbiddenPin(value)) {
          wsclient->printf("{\"ERROR\" : \"fanpin %u is forbidden\"}", value);
          handled = true;
        } else {
          handled = true;
          pref.putUInt8(ccFanPin,value);
          if (!FAN_INIT)
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
  AsyncWebServerResponse *response =
      request->beginResponse(200, "text/html; charset=utf-8", EMBEDDED_HTML, EMBEDDED_HTML_LEN);
  response->addHeader("Content-Encoding", "gzip");
  // The page changes with every firmware build and carries no ETag, so without
  // this a browser can serve a cached copy after an update - including the
  // location.reload() the OTA flow performs, which would look like a failed
  // update. It is 18KB over LAN, so there is nothing worth caching.
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  request->send(response);
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
        json += ",\"ssid\":\""+WiFi.SSID(i)+"\"";
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
  
