
/*

   VE.Direct to CAN BUS & MQTT Gateway using a ESP32 Board
   Copyright (c) 2022-2026 Nexion Software Solutions Ltd - https://nexion.uk

   Free to use in personal projects and modify for your own use, no permission for
   selling or commercialising this code in this project.

   The copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   See the LICENSE file at the root of this repository for the full terms.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

#include <Arduino.h>
#include <nvs_flash.h>
#include "config.h"
#include "FS.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include "VEDisplay.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h> // Include WebServer Library for ESP32
#include <ESPAsyncHTTPUpdateServer.h>

#include "WifiMQTTManager.h"
#include <ArduinoJson.h> // Include ArduinoJson Library
// #include <AsyncElegantOTA.h>
#include <Wire.h>

#include "GPIOForbidden.h"

WiFiClient _wifiClient;

#include "mEEPROM.h"
mEEPROM pref;

#include "VeDirectFrameHandler.h"
#include "TimeLib.h"
#include "CANBUS.h"
#include "FAN.h"
#include "VictronBLE.h"

uint32_t SendCanBusMQTTUpdates;
CANBUS Inverter;

// Shunt source, read once at boot - see the selection logic in loop()
uint8_t  shuntSource = SHUNT_SRC_VEDIRECT;
bool     bleFallback = false;
uint32_t lastBleApplied = 0;
bool     lastBleFresh = false;

//create an object from the UpdateServer
ESPAsyncHTTPUpdateServer updateServer;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

WifiMQTTManagerClass wifiManager;
Display Lcd;

static portMUX_TYPE MainMutex = portMUX_INITIALIZER_UNLOCKED;
VeDirectFrameHandler veHandle;

#include "HTTPWSFunctions.h"
#include "mqttFunctions.h"

#ifdef USE_ONEWIRE
#include "ONEWIRE.h"
#endif


// Functions for handling VE Data
#include "DataProcessing.h"

time_t last_boot;
time_t last_vedirect;
time_t last_lcd_refresh;
time_t last_mqtt_reconnect;
time_t last_loop;
TaskHandle_t tHandleWeb = NULL;
bool FirstRun = true;

void setup()
{
  pref.begin();
  Serial.begin(115200);
#if defined(BMS_S3) || defined(BMS_C3)
  // ESP32-S3 and ESP32-C3 USB-CDC needs time to initialize
  delay(2000);
#else
  delay(100);
#endif

  WS_LOG_I("=== DIY Battery BMS Starting ===");

  if (!pref.isKey("EEPROMSetup"))
  {
    if(pref.isKey(ccWifiSSID) && pref.isKey(ccWifiPass))
      log_d("Wifi Details found, but NVS not been setup, saving initial values.");
    else
      log_d("NVS not setup, writing inital values.");
    
    // pref.putBool("WifiEnabled", true);
    pref.putBool(ccCANBusEnabled, true);
    pref.putBool(ccLcdEnabled, false);
#ifndef ESPCAN
    pref.putBool(ccCAN16Mhz, initCAN16Mhz);
    // pref.putBool("MQTTEnabled", false);
    pref.putUInt8(ccCanCSPin, 0); // Must be set via web interface
#endif
    pref.putUInt8(ccVictronRX, 0); // Must be set via web interface
    pref.putUInt8(ccVictronTX, 0); // Must be set via web interface
#ifdef ESPCAN
    pref.putUInt8(ccCAN_EN_PIN, 0); // Must be set via web interface
    pref.putUInt8(ccCAN_RX_PIN, 0); // Must be set via web interface
    pref.putUInt8(ccCAN_TX_PIN, 0); // Must be set via web interface
#endif
    pref.putUInt16(ccChargeVolt, initBattChargeVoltage);
    pref.putUInt16(ccOverVoltage, initBattOverVoltage);
    pref.getUInt16(ccAdjustStep,initAdjustStep);
    pref.getUInt32(ccMinCharge,initMinChargeCurrent);
    pref.putUInt32(ccChargeCurrent, initBattChargeCurrent);
    pref.putUInt32(ccDischargeVolt, initBattDischargeVoltage);
    pref.putUInt32(ccDischargeCurrent, initBattDischargeCurrent);
    pref.putUInt8(ccLowSOCLimit, initLowSOCLimit);
    pref.putUInt8(ccHighSOCLimit, initHighSOCLimit);
    pref.putUInt8(ccSlowSOCCharge1,0);
    pref.putUInt8(ccSlowSOCCharge2,0);
    pref.putUInt8(ccSlowSOCDivider1,0);
    pref.putUInt8(ccSlowSOCDivider2,0);
    pref.putUInt32(ccBattCapacity, initBattCapacity);
    pref.putBool(ccPylonTech, false);
    pref.putBool(ccSOCTrick, false);
    pref.putBool(ccRequestFlags, false);
    pref.putBool(ccAutoAdjustCharge, true);
    pref.putUInt8(ccSmartInterval,initSmartInterval);
    pref.putUInt8("VE_WAIT_TIME", VE_WAIT_TIME);
    pref.putUInt8("VE_STARTUP_TIME", VE_STARTUP_TIME);
    pref.putUInt8("VE_LCD_REFRESH", VE_LCD_REFRESH);
    pref.putUInt8("VE_MQTT_REC", VE_MQTT_RECONNECT);
    pref.putUInt8(ccVELOOPTIME, VE_LOOP_TIME);
    pref.putString(ccNTPServer,"");
    pref.putString(ccTimeZone, initTimeZone);
    pref.putUInt16(ccOverrideTime, initOverrideTimeout);
    pref.putString(ccSyslogServer,"");
    pref.putUInt16(ccSyslogPort, 514);
    pref.putBool(ccSyslogEnabled, false);
    pref.putBool(ccNever100SOC, false);
    pref.putUInt8(ccPylonVersion, 1);
    pref.putUInt8(ccBattTempSrc, 0);
    pref.putUInt8(ccFanTempSrc, 0);
    pref.putString(ccMQTTBattTopic, "");
    pref.putString(ccMQTTInvTopic, "");
    pref.putInt16(ccFanOffTemp, 30);
    pref.putInt16(ccFanFullTemp, 50);
    pref.putBool("EEPROMSetup", true);
  }
  else {
      log_d("NVS Store opened, initial key found.");
      log_d("NVS has free entries of: %i",pref.freeentries());
  }


  VE_WAIT_TIME = pref.getUInt8("VE_WAIT_TIME", VE_WAIT_TIME);
  VE_STARTUP_TIME = pref.getUInt8("VE_STARTUP_TIME", VE_STARTUP_TIME);
  VE_LCD_REFRESH = pref.getUInt8("VE_LCD_REFRESH", VE_LCD_REFRESH);
  VE_MQTT_RECONNECT = pref.getUInt8("VE_MQTT_REC", VE_MQTT_RECONNECT);
  VE_LOOP_TIME = pref.getUInt8(ccVELOOPTIME, VE_LOOP_TIME);
  
  // Setup FAN, will only complete if a valid, non-forbidden PIN number is assigned.
  {
    uint8_t fanpin = pref.getUInt8(ccFanPin,0);
    if (fanpin > 0 && !IsForbiddenPin(fanpin)) {
      FanInit(fanpin);
    } else if (fanpin > 0) {
      log_w("FAN pin %u is forbidden; skipping FanInit", fanpin);
    }
  }

  // Setup LCD Screen if Enabled
  if(pref.getBool(ccLcdEnabled,false)) {
    Wire.begin();
    Lcd.Begin(Lcd.LCD2004);
    Lcd.SetScreen(Lcd.StartUp);
  }

  //if(pref.getBool())

  // #ifdef USE_ONEWIRE
  // OW_WAIT_TIME = pref.getInt("OW_WAIT_TIME", OW_WAIT_TIME);
  // #endif
    
  // LittleFS removed - HTML now embedded in firmware
  log_d("Using embedded HTML (no filesystem).");
  
  if (!wifiManager.begin())
  {
    // Failed to configure, start the basics to enable web configuration
    // on an Access Point
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    StartWebServices();
    server.begin();
  }

  // Load MQTT temp source settings before mqttsetup so onMqttConnect can subscribe
  Inverter.BattTempSource(pref.getUInt8(ccBattTempSrc, 0));
  Inverter.FanTempSource(pref.getUInt8(ccFanTempSrc, 0));
  mqttsetup();
#ifdef ESPCAN
  {
    uint8_t tx = pref.getUInt8(ccCAN_TX_PIN, 0);
    uint8_t rx = pref.getUInt8(ccCAN_RX_PIN, 0);
    uint8_t en = pref.getUInt8(ccCAN_EN_PIN, 0);
    if (tx && rx && en && !IsForbiddenPin(tx) && !IsForbiddenPin(rx) && !IsForbiddenPin(en)) {
      WS_LOG_I("Initializing CAN Bus (TWAI) on TX:%d RX:%d EN:%d", tx, rx, en);
      if(Inverter.Begin(tx, rx, en))
      {
        Lcd.Data.CANInit.setValue(true);
        WS_LOG_I("CAN Bus (TWAI) initialized on TX:%d RX:%d EN:%d", tx, rx, en);
      }
      else {
        Lcd.Data.CANInit.setValue(false);
        WS_LOG_E("CAN Bus (TWAI) failed to initialize");
      }
    } else {
      log_e("Forbidden or zero GPIO for CAN pins: TX=%u RX=%u EN=%u", tx, rx, en);
      Lcd.Data.CANInit.setValue(false);
    }
  }
#else
  {
    uint8_t cs = pref.getUInt8(ccCanCSPin, 0);
    bool mhz16 = pref.getBool(ccCAN16Mhz, initCAN16Mhz);
    if (cs && !IsForbiddenPin(cs)) {
      if (Inverter.Begin(cs, mhz16))
      {
        Lcd.Data.CANInit.setValue(true);
        WS_LOG_I("CAN Bus (MCP2515) initialized on CS:%d", cs);
      }
      else {
        Lcd.Data.CANInit.setValue(false);
        WS_LOG_E("CAN Bus (MCP2515) failed to initialize");
      }
    } else {
      log_e("Forbidden or zero GPIO for CAN CS pin: CS=%u", cs);
      Lcd.Data.CANInit.setValue(false);
    }
  }
#endif
  // Continue setup based on CAN init status
  bool canInitOK = Lcd.Data.CANInit.getValue();
  
  Inverter.SetChargeVoltage((u_int16_t) pref.getUInt32(ccChargeVolt, initBattChargeVoltage));
  Inverter.SetOverVoltage((u_int16_t) pref.getUInt32(ccOverVoltage, initBattOverVoltage));
  Inverter.SetChargeStepAdjust(pref.getUInt16(ccAdjustStep,initAdjustStep));
  Inverter.MinChargeCurrent(pref.getUInt32(ccMinCharge,initMinChargeCurrent));
  Inverter.SetMaxChargeCurrent(pref.getUInt32(ccChargeCurrent, initBattChargeCurrent));
  Inverter.SetDischargeVoltage(pref.getUInt32(ccDischargeVolt, initBattDischargeVoltage));
  Inverter.SetMaxDischargeCurrent(pref.getUInt32(ccDischargeCurrent, initBattDischargeCurrent));
  Inverter.SetLowSOCLimit((uint8_t) pref.getUInt8(ccLowSOCLimit, initLowSOCLimit));
  Inverter.SetHighSOCLimit((uint8_t) pref.getUInt8(ccHighSOCLimit, initHighSOCLimit));
  Inverter.SetBattCapacity(pref.getUInt32(ccBattCapacity, initBattCapacity));
  Inverter.EnableSOCTrick(pref.getBool(ccSOCTrick, false));
  Inverter.EnableRequestFlags(pref.getBool(ccRequestFlags, false));
  Inverter.Never100SOC(pref.getBool(ccNever100SOC, false));
  // Apply the timezone before anything timestamps anything - syslog and the
  // clock display both use localtime_r().
  applyTimeZone();
  applySyslogConfig();
#ifndef DISABLE_SCHEDULER
  loadUiSchedule();
  // Nothing is latched at boot, so control starts with the schedule. Deliberate:
  // a reboot is exactly when you want the locally saved plan back, not whatever
  // a controller had commanded before the power went.
  RemoteOverride.SetTimeout(pref.getUInt16(ccOverrideTime, initOverrideTimeout));
#endif
  Inverter.SetCANProtocol((CANProtocol)pref.getUInt8(ccCANProtocol, PROTO_PYLONTECH_13));
  Inverter.SetSlowChargeDivider(1,pref.getUInt8(ccSlowSOCDivider1,initSlowSOCDivider1));
  Inverter.SetSlowChargeDivider(2,pref.getUInt8(ccSlowSOCDivider2,initSlowSOCDivider2));
  Inverter.SetSlowChargeSOCLimit(1, pref.getUInt8(ccSlowSOCCharge1, initSlowSOCCharge1));
  Inverter.SetSlowChargeSOCLimit(2, pref.getUInt8(ccSlowSOCCharge2, initSlowSOCCharge2));
  Inverter.AutoCharge(pref.getBool(ccAutoAdjustCharge, true));
  Inverter.SmartInterval(pref.getUInt8(ccSmartInterval,initSmartInterval));

  // CC-CV Charging Parameters
  Inverter.SetTailCurrentmA(pref.getUInt32(ccTailCurrent, initTailCurrentmA));
  Inverter.SetTailCurrentDuration(pref.getUInt16(ccTailDuration, initTailCurrentDuration));
  Inverter.SetMaxAbsorptionTime(pref.getUInt16(ccMaxAbsTime, initMaxAbsorptionTime));
  Inverter.SetRechargeSOC(pref.getUInt8(ccRechargeSOC, initRechargeSOC));
  Inverter.SetRechargeVoltageOffset(pref.getUInt16(ccRechargeVOff, initRechargeVoltageOffset));
  Inverter.SetFloatVoltage((uint16_t) pref.getUInt32(ccFloatVoltage, initFloatVoltage));
  Inverter.SetFloatCurrent(pref.getUInt32(ccFloatCurrent, initFloatCurrentmA));
  // Requests themselves are never restored - nothing is in force at boot, so the
  // configured ceilings apply until a controller asks for something.
  Inverter.SetRequestTimeout(pref.getUInt16(ccReqTimeout, initRequestTimeout));

  Inverter.TempProtectionEnabled(pref.getBool(ccTempProtect, false));
  Inverter.SetChargeHighTemp(pref.getInt16(ccChgHighTemp, 45));
  Inverter.SetChargeLowTemp(pref.getInt16(ccChgLowTemp, 0));
  Inverter.SetDischargeHighTemp(pref.getInt16(ccDisHighTemp, 50));
  Inverter.SetDischargeLowTemp(pref.getInt16(ccDisLowTemp, -20));
  Inverter.ShowTempOnDashboard(pref.getBool(ccShowTemp, false));

  // MQTT temperature source & fan control settings (must be loaded before MQTT connects)
  Inverter.BattTempSource(pref.getUInt8(ccBattTempSrc, 0));
  Inverter.FanTempSource(pref.getUInt8(ccFanTempSrc, 0));
  Inverter.SetFanOffTemp(pref.getInt16(ccFanOffTemp, 30));
  Inverter.SetFanFullTemp(pref.getInt16(ccFanFullTemp, 50));
  log_i("Temp sources: batt=%u fan=%u, FanOff=%d FanFull=%d",
    Inverter.BattTempSource(), Inverter.FanTempSource(),
    Inverter.GetFanOffTemp(), Inverter.GetFanFullTemp());

  if(pref.getBool(ccCANBusEnabled,true) && canInitOK) {
    Inverter.StartRunTask();
    WS_LOG_I("CAN Bus task started");
  }

  SendCanBusMQTTUpdates = millis();
  Lcd.UpdateScreenValues();
  xTaskCreate(&taskStartWebServices,"taskStartWebServices",4096, NULL, 6, NULL);

  // Start VE.Direct Serial Reading if Enabled
  uint8_t vrx = (uint8_t) pref.getUInt8(ccVictronRX, 0);
  uint8_t vtx = (uint8_t) pref.getUInt8(ccVictronTX, 0); // 0 = not wired, TX is optional
  if (vrx && !IsForbiddenPin(vrx) && (vtx == 0 || !IsForbiddenPin(vtx))) {
    if(veHandle.OpenSerial(vrx, vtx))
      veHandle.startReadTask();
  } else if (vrx || vtx) {
    log_e("Forbidden or missing GPIO for VE.Direct pins: RX=%u TX=%u", vrx, vtx);
  }

  /* Victron BLE. Only brought up when it is the selected source - the radio and
     its stack cost RAM that an install reading the shunt over the wire has no
     reason to spend. The serial reader above always starts regardless, because
     it is the fallback path and costs a task either way. */
  shuntSource = pref.getUInt8(ccShuntSource, SHUNT_SRC_VEDIRECT);
  bleFallback = pref.getBool(ccBLEFallback, false);
  if (shuntSource == SHUNT_SRC_BLE) {
    VictronBle.SetMac(pref.getString(ccVBLEMac, ""));
    VictronBle.SetKeyHex(pref.getString(ccVBLEKey, ""));
    VictronBle.Begin(true);
    log_i("Shunt source: Victron BLE%s", bleFallback ? " (falls back to serial)" : "");
  }
  // Start NTP Clock Set Task
#if defined(BMS_S3) || defined(BMS_C3)
  // ESP32-S3 and ESP32-C3 require more stack space for String operations and NTP
  xTaskCreate(&TaskSetClock,"taskSetClock", 8192, NULL, 5, NULL);
#else
  xTaskCreate(&TaskSetClock,"taskSetClock", 4096, NULL, 5, NULL);
#endif 
  // Set the lcd timer
  time_t t = time(nullptr);
  last_lcd_refresh = t;
  log_d("Setup complete, starting loop.");
  WS_LOG_I("System initialization complete, entering main loop");
  return;
}

// Monitor WiFi scan completion and send results via WebSocket
void UpdateWifiScanResults() {
  int n = WiFi.scanComplete();
  
  // Check if scan completed (n >= 0) and results changed or newly requested
  taskENTER_CRITICAL(&wifiScanMutex);
  bool shouldSend = (n >= 0 && (lastWifiScanCount != n || wifiScanRequested));
  if(shouldSend) {
    lastWifiScanCount = n;
    wifiScanRequested = false;
  }
  taskEXIT_CRITICAL(&wifiScanMutex);
  
  // Build and send JSON outside critical section
  if(shouldSend) {
    String json = "{\"wifinetworks\":[";
    
    if(n > 0) {
      for(int i = 0; i < n; i++) {
        if(i > 0) json += ",";
        // Get network info - store locally to minimize WiFi API calls
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        uint8_t channel = WiFi.channel(i);
        uint8_t secure = WiFi.encryptionType(i);
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(rssi) + ",\"channel\":" + String(channel) + ",\"secure\":" + String(secure) + "}";
      }
      log_d("WiFi scan completed: %d networks found", n);
      WS_LOG_I("WiFi scan completed: %d networks found", n);
    } else {
      log_d("WiFi scan completed: no networks found");
      WS_LOG_I("WiFi scan completed: no networks found");
    }
    
    json += "]}";
    
    // Send to all WebSocket clients
    if(ws.count() > 0 && ws.availableForWriteAll()) {
      ws.textAll(json);
    }
  }
}

/*
   Apply the active schedule window.

   Precedence: safety > outside override > schedule > default (charge on,
   discharge on, force off).

   Safety is not checked here - it cannot be bypassed from this layer. The CAN
   code emits (_chargeEnabled && _ManualAllowCharge), and _chargeEnabled belongs
   to the protection logic, so writing ManualAllow* can only ever withhold, never
   grant. Force charge is gated inside evaluate() on the same flag, and again
   below for a force asserted from outside the schedule.

   Levers taken by the web UI, MQTT or a WebSocket supervisor are left alone
   here until their latch times out - see RemoteOverride.h.

   Runs once a second; the setters are no-ops when nothing changed.
*/
#ifndef DISABLE_SCHEDULER
uint32_t lastScheduleEval = 0;
bool lastSchedActive = false;

void ScheduleApply(time_t now) {
  if ((uint32_t)(millis() - lastScheduleEval) < 1000) return;
  lastScheduleEval = millis();

  /* Retire timed-out overrides first, and say so once rather than on every
     pass. Deliberately ahead of the clock check below: a latch has to be able
     to expire even while the clock is unset, or a controller that vanished
     before NTP synced would keep its levers indefinitely. */
  uint8_t expired = RemoteOverride.Expired();
  if (expired) {
    for (uint8_t i = 0; i < OV_COUNT; i++)
      if (expired & (1 << i))
        WS_LOG_I("Remote override on %s timed out, schedule back in control",
                 RemoteOverrideClass::Name((RemoteLever)i));
    publishScheduleStatus();
    notifyWSClients();
  }

  /* Safety is not something a latch can hold off. evaluate() already refuses to
     assert force charge when protection has disabled charging; apply the same
     rule to a force asserted from outside, so a controller cannot leave the
     inverter being told to charge at maximum while the pack is too cold or too
     full. Ahead of the clock check, since it depends on neither the clock nor
     the schedule. Logged on the transition only - the controller will keep
     re-asserting, and one line a second helps nobody. */
  static bool forceCutBySafety = false;
  if (RemoteOverride.Active(OV_FORCE) && !Inverter.ChargeEnable() && Inverter.ForceCharge()) {
    Inverter.ForceCharge(false);
    if (!forceCutBySafety) {
      forceCutBySafety = true;
      WS_LOG_W("Force charge from an outside override dropped: protection has charging disabled");
    }
  } else if (Inverter.ChargeEnable()) forceCutBySafety = false;

  // Scheduling is meaningless without a real clock, and actively unsafe: the
  // windows would be evaluated against a 1970 date and could assert force charge
  // at an arbitrary time. Hold off entirely until NTP has synced, leaving the
  // defaults in place, and say so once rather than every second.
  static bool warnedNoClock = false;
  if (!clockIsValid()) {
    if (!warnedNoClock && Schedule.uiCount() + Schedule.mqttCount() > 0) {
      warnedNoClock = true;
      WS_LOG_W("Schedule held off: clock not set. Configure an NTP server under Settings.");
    }
    return;
  }
  warnedNoClock = false;

  // Absolute windows expire on their own; drop them once past so the list does
  // not grow stale and the fallback to the UI schedule happens by itself.
  Schedule.pruneExpired(now);

  SchedDecision d = Schedule.evaluate(now, Inverter.BattSOC(), Inverter.ChargeEnable());

  /* Leave latched levers alone. Anything not currently held is re-asserted as
     before, so the schedule takes a lever straight back the moment its latch
     expires or is cleared - there is no state to restore. */
  if (!RemoteOverride.Active(OV_CHARGE))    Inverter.ManualAllowCharge(d.charge);
  if (!RemoteOverride.Active(OV_DISCHARGE)) Inverter.ManualAllowDischarge(d.discharge);
  if (!RemoteOverride.Active(OV_FORCE)) {
    if (d.force != Inverter.ForceCharge()) Inverter.ForceCharge(d.force);
  }
  if (d.active != lastSchedActive) {
    lastSchedActive = d.active;
    if (d.active)
      WS_LOG_I("Schedule window started (%s): charge %s, discharge %s, force %s%s",
               d.fromMqtt ? "MQTT" : "web UI",
               d.charge ? "on" : "off", d.discharge ? "on" : "off",
               d.force ? "on" : "off",
               d.targetSOC ? (" to " + String(d.targetSOC) + "%").c_str() : "");
    else
      WS_LOG_I("Schedule window ended, back to defaults");
    publishScheduleStatus();
  }
}
#else
static inline void ScheduleApply(time_t) {}
#endif

void loop()
{

  time_t t = time(nullptr);
 
  if(WiFi.isConnected() && FirstRun && mqttEnabled) {
    connectToMqtt();
    WS_LOG_I("WiFi connected, IP: %s", WiFi.localIP().toString().c_str());
    FirstRun = false; }
  
  wifiManager.loop();
  ScheduleApply(t);

  // Monitor WiFi scan completion and send results via WebSocket
  UpdateWifiScanResults();

  // Read serial data with timeout protection to prevent watchdog issues
  uint32_t serialStartTime = millis();
  while(Serial1.available() > 0)
  {
      veHandle.rxData(Serial1.read());
      // Yield after processing multiple bytes or if taking too long (>50ms)
      if (millis() - serialStartTime > 50) {
        yield();
        serialStartTime = millis();
      }
  }

  /* Which shunt source feeds the charge logic.

     dataavailable() is called unconditionally because it clears the frame flag:
     skipping it while BLE is in charge would leave serial frames marked pending
     forever and the flag would be stale the moment fallback did kick in.

     Fallback only applies when BLE is the configured source. It is opt-in
     because a source changing itself mid-charge is hard to diagnose after the
     fact, so the switch is logged both ways. */
  // Push the device list to the browser the moment a scan window closes
  if (VictronBle.DiscoveryTick())
    notifyWSClients();

  const bool bleFresh  = (shuntSource == SHUNT_SRC_BLE) && VictronBle.DataFresh();
  const bool haveFrame = veHandle.dataavailable();
  const bool useSerial = (shuntSource == SHUNT_SRC_VEDIRECT) ||
                         (shuntSource == SHUNT_SRC_BLE && bleFallback && !bleFresh);

  if (shuntSource == SHUNT_SRC_BLE && bleFallback && bleFresh != lastBleFresh) {
    lastBleFresh = bleFresh;
    if (bleFresh) WS_LOG_I("Shunt source: BLE data returned, back on BLE");
    else          WS_LOG_W("Shunt source: BLE data stale, falling back to VE.Direct serial");
  }

  if (bleFresh && VictronBle.LastUpdateMs != lastBleApplied)
  {
    lastBleApplied = VictronBle.LastUpdateMs;
    last_vedirect = t;
    if(!Lcd.Data.VEData._currentValue)
      Lcd.Data.VEData.setValue(true);
    BLEDataProcess();
  }
  else if (haveFrame && useSerial)
  {
    last_vedirect = t;
    if(!Lcd.Data.VEData._currentValue)
      Lcd.Data.VEData.setValue(true);
    log_d("Data Available to Process");
    VEDataProcess();
  }

// Time out for data arrival
  if ((abs(t - last_vedirect) > 2) && Lcd.Data.VEData._currentValue)
      Lcd.Data.VEData.setValue(false);

  if (((millis() - SendCanBusMQTTUpdates) > ((uint32_t)VE_LOOP_TIME * 1000) || Inverter.DataChanged())
      && Lcd.Data.VEData.getValue() == true)
  {
    SendCanBusMQTTUpdates = millis();
    if (wifiManager.isWiFiConnected())
    {
      sendVE2MQTT();
      sendUpdateMQTTData();
      ws.cleanupClients();
      notifyWSClients(false);
    }
  }

  if (abs(t - last_lcd_refresh) >= VE_LCD_REFRESH)
  {

    last_lcd_refresh = t;
    // Update LCD Screen Values - CRITICAL: Protect battery state reads with mutex
    taskENTER_CRITICAL(&(Inverter.CANMutex));
    
    Lcd.Data.ChargeVolts.setValue(Inverter.GetChargeVoltage());
    Lcd.Data.ChargeAmps.setValue(Inverter.GetChargeCurrent());
    Lcd.Data.DischargeVolts.setValue(Inverter.GetDischargeVoltage());
    Lcd.Data.DischargeAmps.setValue(Inverter.GetDischargeCurrent());
    Lcd.Data.ChargeEnable.setValue((Inverter.ChargeEnable() && Inverter.ManualAllowCharge()) ? true : false);
    Lcd.Data.DischargeEnable.setValue((Inverter.DischargeEnable() && Inverter.ManualAllowDischarge()) ? true : false);
    Lcd.Data.CANBusData.setValue(!Inverter.CanBusFailed());
    Lcd.Data.ForceCharging.setValue(Inverter.ForceCharge());
    
    int32_t b = Inverter.BattCurrentmA();
    if (b < 0)
      b = -b;
    
    taskEXIT_CRITICAL(&(Inverter.CANMutex));
    
    Lcd.Data.WifiConnected.setValue(WiFi.isConnected());
    Lcd.Data.MQTTConnected.setValue(mqttClient.connected());
    Lcd.Data.IPAddr.setValue(wifiManager.GetIPAddr());
    CheckAndChangeLCD();
    Lcd.UpdateScreenValues();
    if(FAN_INIT) {
      if (Inverter.FanTempSource() == 1 && Inverter.HasMqttInverterTemp()) {
        // Temperature-based linear fan control from MQTT inverter temp
        int16_t temp = Inverter.MqttInverterTemp();
        int16_t offT = Inverter.GetFanOffTemp();
        int16_t fullT = Inverter.GetFanFullTemp();
        float duty = 0.0f;
        if (temp >= fullT) duty = 100.0f;
        else if (temp > offT) duty = ((float)(temp - offT) / (float)(fullT - offT)) * 100.0f;
        FanUpdateTemp(duty);
      } else {
        // Fallback: current-based fan control
        FanUpdate((b * 0.1));
      }
    }
  }

  // Yield to watchdog and other tasks
  yield();
  delay(1);

}
