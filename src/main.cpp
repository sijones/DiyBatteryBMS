
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
// Before HTTPWSFunctions.h: the WebSocket payload reports the figures it keeps
#include "Diagnostics.h"

WiFiClient _wifiClient;

#include "mEEPROM.h"
mEEPROM pref;

#include "VeDirectFrameHandler.h"
#include "TimeLib.h"
#include "CANBUS.h"
#include "FAN.h"
#include "VictronBLE.h"
#include "MqttShunt.h"
// Optional features register themselves - nothing to include or call per
// feature, see Features.h. Their own headers are pulled in by their .cpp files.
#include "Features.h"

uint32_t SendCanBusMQTTUpdates;
CANBUS Inverter;

// Shunt source and its fallback, read once at boot - see the selection logic in
// loop(). fallbackSource is SHUNT_FALLBACK_NONE or any source but the primary.
uint8_t  shuntSource = SHUNT_SRC_VEDIRECT;
uint8_t  fallbackSource = SHUNT_FALLBACK_NONE;
uint32_t lastBleApplied = 0;
bool     lastBleFresh = false;
/* How long a VE.Direct frame counts for. Serial had no freshness of its own
   while it was only ever the thing being fallen back TO - a frame either
   arrived on this pass of loop() or it did not. Now that it can be the source
   something else takes over from, "is the cable delivering" has to be a state
   rather than an instant, and this is how long since the last completed frame
   still counts as yes. Frames come about once a second, so five seconds is
   several missed ones - late enough not to twitch, early enough that an
   unplugged cable is noticed while it still matters.

   Deliberately not the two seconds the VEData timeout below uses: that one
   asks whether ANY source has produced a reading lately, and is what puts "no
   data" on the display. This one is about one source only. */
#define SHUNT_SERIAL_STALE_MS 5000
uint32_t lastSerialFrameMs = 0;
/* Starts true so the first frame after boot is not announced as data
   "returning" from an outage that never happened. A cable that is not there at
   all stays silent for a different reason: lastSerialFrameMs is still zero, so
   the state never changes and there is nothing to report. */
bool     lastSerialFresh = true;
/* MQTT source. The instance lives here rather than in a .cpp of its own -
   MqttShunt.h is scalars only, there is no radio or discovery behind it. */
MqttShuntSource MqttShunt;
uint32_t lastMqttApplied = 0;
uint32_t lastMqttHeartbeat = 0;
bool     lastMqttFresh = false;
/* Which link last actually put a reading into Inverter, as opposed to which one
   is configured. 255 = nothing has yet. Drives the shuntlink status field, which
   tells the UI which identity fields it can expect to be empty. */
uint8_t  activeShuntLink = 255;

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
// After HTTPWSFunctions.h: uses hexToBytes() and the wifiManager instance
#include "SerialSetup.h"

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
  /* setup() and loop() both run inside the Arduino core's own loopTask, which
     the framework creates at priority 1 (cores/esp32/main.cpp) - the lowest
     priority anything in this project runs at, on whichever core
     CONFIG_ARDUINO_RUNNING_CORE names (1, here). CANBUS's send task shares
     that core at priority 6, so loop() - which is what drains VE.Direct's
     Serial1 - was the lowest-priority task on its own core, able to be
     preempted for as long as CANBUS had work. Priority 7 clears that while
     staying under AsyncTCP's task (priority 10, unpinned - may or may not
     land on this core), which still needs to win when it actually has
     network data waiting. NULL means "the calling task", which is this
     one. */
  vTaskPrioritySet(NULL, 7);

  pref.begin();
  Serial.begin(115200);
#if defined(BMS_S3)
  // ESP32-S3 USB-CDC needs time to initialize
  delay(2000);
#else
  delay(100);
#endif

  WS_LOG_I("=== DIY Battery BMS Starting ===");
  // Straight to the serial line, not through a log macro - see SerialSetup.h
  serialSetupBegin();
  /* Straight after the banner, and before anything that could itself fail: the
     first question about any restart is whether it was one we asked for, and
     the answer sits unread in the RTC registers until this runs. */
  Diag.Begin();

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
    // 0 on a non-Waveshare build means "must be set via web interface"; see
    // config.h for why the Waveshare build defaults RX instead.
    pref.putUInt8(ccVictronRX, initVictronRX);
    pref.putUInt8(ccVictronTX, 0); // VE.Direct is listen-only, TX need not be wired
#ifdef ESPCAN
    // 0 on a non-Waveshare build means "must be set via web interface"; see
    // config.h for why the Waveshare build defaults these instead.
    pref.putUInt8(ccCAN_EN_PIN, initCAN_EN_PIN);
    pref.putUInt8(ccCAN_RX_PIN, initCAN_RX_PIN);
    pref.putUInt8(ccCAN_TX_PIN, initCAN_TX_PIN);
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

  /* Has to be set BEFORE the DHCP lease is taken, which happens a second or two
     into wifiManager.begin() below: lwIP hands the offered NTP servers to
     dhcp_set_ntp_servers() as the lease is processed, and that drops them
     unless this flag is already on. Enabling it later, when TaskSetClock
     configures SNTP, is far too late and the clock silently never syncs.

     esp_netif_init() first, and not optional. esp_sntp_servermode_dhcp() reaches
     lwIP through tcpip_callback(), so calling it before the TCP/IP thread exists
     is not a no-op or an error return - it is
       assert failed: tcpip_callback ... (Invalid mbox)
     and a boot loop that only a serial flash gets you out of. esp_netif_init()
     is idempotent and WiFi will call it again itself. */
  if (pref.getBool(ccNTPFromDHCP, true)) {
    esp_netif_init();
    esp_sntp_servermode_dhcp(true);
    log_d("NTP: accepting a server from the DHCP lease");
  }

  /* Milestones through the rest of setup, because "setup complete" on its own
     said 95KB had gone without saying to whom. The suspicion is the WiFi
     driver's buffers, which is worth confirming rather than assuming - it is
     the difference between tuning WiFi and looking somewhere else entirely. */
  Diag.Milestone("settings loaded");

  if (!wifiManager.begin())
  {
    // Failed to configure, start the basics to enable web configuration
    // on an Access Point
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    StartWebServices();
    server.begin();
  }

  // WiFi.mode()/WiFi.begin() bring the driver up in here, buffers and all
  Diag.Milestone("WiFi stack up");

  // Load MQTT temp source settings before mqttsetup so onMqttConnect can subscribe
  Inverter.BattTempSource(pref.getUInt8(ccBattTempSrc, 0));
  Inverter.FanTempSource(pref.getUInt8(ccFanTempSrc, 0));
  /* Same reason, one line further out: when the shunt source is MQTT its four
     topics are subscribed from onMqttConnect too, and that can fire before
     setup() gets as far as the BLE block below. Only these two reads move -
     the radio is still brought up down there, where it belongs. */
  shuntSource = pref.getUInt8(ccShuntSource, SHUNT_SRC_VEDIRECT);
  /* Migrate the old blunt "fall back to serial" checkbox. An install that had
     it ticked meant something specific - wireless primary, serial behind it -
     and reading the new key with a plain "none" default would quietly take
     that away on the first boot after an update, on the one setting whose
     whole job is to cover a source going quiet. So the old boolean supplies
     the default only until a fallback has been chosen under the new key. */
  const uint8_t migratedFallback = pref.getBool(ccBLEFallback, false)
                                     ? SHUNT_SRC_VEDIRECT : SHUNT_FALLBACK_NONE;
  fallbackSource = pref.getUInt8(ccFallbackSrc, migratedFallback);
  // A fallback equal to the primary is not a fallback; refuse it rather than
  // carry a contradiction through the resolution in loop().
  if (fallbackSource == shuntSource) fallbackSource = SHUNT_FALLBACK_NONE;
  log_i("Shunt source: %s, fallback %s", ShuntSrcName(shuntSource),
        ShuntSrcName(fallbackSource));
  mqttsetup();
  Diag.Milestone("MQTT client made");
#ifdef ESPCAN
  {
    uint8_t tx = pref.getUInt8(ccCAN_TX_PIN, 0);
    uint8_t rx = pref.getUInt8(ccCAN_RX_PIN, 0);
    uint8_t en = pref.getUInt8(ccCAN_EN_PIN, 0);
    // en==0 means this board's transceiver has no software enable line, not
    // "not configured yet" - CANBUS::Begin() already skips driving it in that
    // case, so the startup gate here has to match rather than refuse to try.
    if (tx && rx && !IsForbiddenPin(tx) && !IsForbiddenPin(rx) && (!en || !IsForbiddenPin(en))) {
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
  Diag.Milestone("CAN driver up");
  
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
  Diag.Milestone("CAN task started");
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

  /* Optional features, after the web server and the shunt/CAN hardware so one
     may use any of them. Which are compiled in is decided by the env rather
     than by anything here, so the count is logged - an empty registry on a
     build expected to have one is otherwise invisible. */
  if (Feature::Count() > 0) {
    log_i("Starting %u optional feature(s)", Feature::Count());
    Feature::SetupAll();
  }

  /* Victron BLE. Only brought up when it is in play in one role or the other -
     the radio and its stack cost RAM that an install reading the shunt over the
     wire has no reason to spend. A fallback has to be listening before the
     primary fails, so being the fallback counts just as much as being the
     primary: bringing the radio up at the moment it was needed would mean the
     first minute of every outage had nothing behind it. The serial reader above
     always starts regardless, because it costs a task either way. shuntSource
     and fallbackSource were read earlier, before mqttsetup(). */
  /* Loaded whatever the source, so the settings page can show them and the
     status JSON never has to go back to NVS for the address. Holding them costs
     a few dozen bytes; re-reading them on every broadcast cost the heap. */
  VictronBle.SetMac(pref.getString(ccVBLEMac, ""));
  VictronBle.SetKeyHex(pref.getString(ccVBLEKey, ""));
  if (shuntSource == SHUNT_SRC_BLE || fallbackSource == SHUNT_SRC_BLE) {
    if (VictronBle.Begin(true)) {
      log_i("Victron BLE radio started (%s shunt source)",
            shuntSource == SHUNT_SRC_BLE ? "primary" : "fallback");
    }
  }
  // Start NTP Clock Set Task
#if defined(BMS_S3)
  // ESP32-S3 requires more stack space for String operations and NTP
  xTaskCreate(&TaskSetClock,"taskSetClock", 8192, NULL, 5, NULL);
#else
  xTaskCreate(&TaskSetClock,"taskSetClock", 4096, NULL, 5, NULL);
#endif 
  // Set the lcd timer
  time_t t = time(nullptr);
  last_lcd_refresh = t;
  log_d("Setup complete, starting loop.");
  WS_LOG_I("System initialization complete, entering main loop");
  Diag.Milestone("setup complete");
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
        /* ssid is the sanitised, JSON-escaped name for the dropdown to show;
           ssidhex is what the browser sends back to be saved. See the SSID
           transport note in HTTPWSFunctions.h - beacon bytes verbatim would
           make this an invalid UTF-8 text frame, and the browser is required
           to drop the WebSocket rather than render it. */
        json += "{\"ssid\":\"" + jsonEscape(toDisplayUTF8(ssid)) + "\""
                ",\"ssidhex\":\"" + bytesToHex(ssid) + "\""
                ",\"rssi\":" + String(rssi) + ",\"channel\":" + String(channel) + ",\"secure\":" + String(secure) + "}";
      }
      log_d("WiFi scan completed: %d networks found", n);
      WS_LOG_I("WiFi scan completed: %d networks found", n);
    } else {
      log_d("WiFi scan completed: no networks found");
      WS_LOG_I("WiFi scan completed: no networks found");
    }
    
    json += "]}";
    
    // Per client, so one stalled reader cannot swallow everyone's scan results
    if(ws.count() > 0) wsBroadcast(json);
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
    WS_LOG_I("WiFi connected, IP: %s", WiFi.localIP().toString().c_str());
    // Before the MQTT connect, so association is billed separately from it
    Diag.Milestone("WiFi associated");
    connectToMqtt();
    FirstRun = false; }
  
  wifiManager.loop();
  // Heap low-water marks, and the trail they leave on the way down. Ticks once
  // a second; returns immediately the rest of the time.
  Diag.Loop();
  // WiFi setup over USB, and the address announced when it joins
  serialSetupLoop();
  ScheduleApply(t);

  // Feeds out the Home Assistant discovery burst a group at a time - see the
  // note above HA_CHUNK_COUNT. No-op unless a sequence is armed.
  haDiscoveryLoop();

  // Monitor WiFi scan completion and send results via WebSocket
  UpdateWifiScanResults();

  // Optional features. No-op when none are compiled in.
  Feature::LoopAll();

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

  /* Which shunt source feeds the charge logic - VE.Direct serial, Victron BLE,
     or figures published to MQTT.

     Two settings decide it and the three sources are treated alike: shuntSource
     names the primary, fallbackSource names what takes over while the primary's
     data is stale, and either can be any of the three. So a VE.Direct cable
     backed by BLE for when it is unplugged works the same way round as BLE
     backed by the cable, which the old wireless-falls-back-to-serial switch
     could not express at all.

     A fallback is opt-in and both its directions are logged, because a source
     changing itself mid-charge is very hard to account for afterwards. With
     fallbackSource at SHUNT_FALLBACK_NONE, a stale primary means nothing is
     applied at all and the timeout below shows "no data" - the readings go
     absent rather than quietly coming from somewhere that was never asked for.

     Freshness is computed for all three whatever is selected. Both wireless
     sources answer false until something has actually arrived, so there is
     nothing to gate, and a fallback has to be able to say it is ready before
     the primary stops. dataavailable() in particular is called unconditionally
     because it clears the frame flag: skipping it while another source is in
     charge would leave frames marked pending forever and the flag would be
     stale the moment the cable was wanted. */
  // Push the device list to the browser the moment a scan window closes
  if (VictronBle.DiscoveryTick())
    notifyWSClients();

  const bool bleFresh  = VictronBle.DataFresh();
  const bool mqttFresh = MqttShunt.DataFresh();
  const bool haveFrame = veHandle.dataavailable();
  if (haveFrame) lastSerialFrameMs = millis();
  /* Serial's equivalent of the other two: a frame is an instant, this is the
     state. Zero means no frame has ever been read, which is not fresh - the
     same answer BLE and MQTT give before their first reading. */
  const bool serialFresh = lastSerialFrameMs &&
                           (millis() - lastSerialFrameMs) < SHUNT_SERIAL_STALE_MS;

  auto SourceFresh = [&](uint8_t src) -> bool {
    switch (src) {
      case SHUNT_SRC_VEDIRECT: return serialFresh;
      case SHUNT_SRC_BLE:      return bleFresh;
      case SHUNT_SRC_MQTT:     return mqttFresh;
      default:                 return false;   // SHUNT_FALLBACK_NONE, or nonsense
    }
  };

  /* The primary while it is delivering, the fallback while it is not, and 255
     for "neither has anything right now". */
  const uint8_t chosen = SourceFresh(shuntSource) ? shuntSource
                       : (fallbackSource != SHUNT_FALLBACK_NONE &&
                          SourceFresh(fallbackSource)) ? fallbackSource
                       : 255;

  /* A source going quiet or coming back is said once, at the change, and only
     about a source that is configured in one role or the other - a board with
     an old BLE key still in NVS should not narrate a link nothing is using.

     What it says depends on the role, because "stale" means different things
     either way round: a primary going quiet is a handover, a fallback going
     quiet is the safety net disappearing while nothing is wrong yet. Both are
     worth knowing and they are not the same event. */
  auto LogStale = [&](uint8_t src) {
    if (shuntSource != src)
      WS_LOG_W("Shunt source: %s data stale, no fallback available", ShuntSrcName(src));
    else if (fallbackSource == SHUNT_FALLBACK_NONE)
      WS_LOG_W("Shunt source: %s data stale, no fallback configured", ShuntSrcName(src));
    else
      WS_LOG_W("Shunt source: %s data stale, falling back to %s",
               ShuntSrcName(src), ShuntSrcName(fallbackSource));
  };

  const bool bleInPlay    = (shuntSource == SHUNT_SRC_BLE  || fallbackSource == SHUNT_SRC_BLE);
  const bool mqttInPlay   = (shuntSource == SHUNT_SRC_MQTT || fallbackSource == SHUNT_SRC_MQTT);
  const bool serialInPlay = (shuntSource == SHUNT_SRC_VEDIRECT ||
                             fallbackSource == SHUNT_SRC_VEDIRECT);

  if (bleInPlay && bleFresh != lastBleFresh) {
    lastBleFresh = bleFresh;
    if (bleFresh) WS_LOG_I("Shunt source: %s data returned", ShuntSrcName(SHUNT_SRC_BLE));
    else          LogStale(SHUNT_SRC_BLE);
  }

  if (mqttInPlay && mqttFresh != lastMqttFresh) {
    lastMqttFresh = mqttFresh;
    if (mqttFresh) WS_LOG_I("Shunt source: %s data returned", ShuntSrcName(SHUNT_SRC_MQTT));
    else           LogStale(SHUNT_SRC_MQTT);
  }

  /* Serial says the same, now that it can be the one being fallen back FROM.
     lastSerialFrameMs gates it so a board with no cable attached does not
     report a link it has never had as having just gone stale. */
  if (serialInPlay && lastSerialFrameMs && serialFresh != lastSerialFresh) {
    lastSerialFresh = serialFresh;
    if (serialFresh) WS_LOG_I("Shunt source: %s data returned", ShuntSrcName(SHUNT_SRC_VEDIRECT));
    else             LogStale(SHUNT_SRC_VEDIRECT);
  }

  if (chosen == SHUNT_SRC_BLE && VictronBle.LastUpdateMs != lastBleApplied)
  {
    lastBleApplied = VictronBle.LastUpdateMs;
    last_vedirect = t;
    if(!Lcd.Data.VEData._currentValue)
      Lcd.Data.VEData.setValue(true);
    BLEDataProcess();
    activeShuntLink = SHUNT_SRC_BLE;
  }
  /* Re-applied once a second even with nothing new, unlike BLE. A publisher on
     a 5 or 10 second timer is healthy, but the two-second timeout below would
     drop VEData between its messages and the dashboard would flap between
     "data" and "no data" on a source that is working. The setters are
     idempotent, so a re-apply just re-stamps last_vedirect. */
  else if (chosen == SHUNT_SRC_MQTT && (MqttShunt.LastUpdateMs != lastMqttApplied ||
                                        (millis() - lastMqttHeartbeat) > 1000))
  {
    lastMqttApplied = MqttShunt.LastUpdateMs;
    lastMqttHeartbeat = millis();
    last_vedirect = t;
    if(!Lcd.Data.VEData._currentValue)
      Lcd.Data.VEData.setValue(true);
    MQTTShuntDataProcess();
    activeShuntLink = SHUNT_SRC_MQTT;
  }
  // Only on the pass where a frame actually completed - serialFresh says the
  // cable is alive, haveFrame says there is something new to read out of it.
  else if (chosen == SHUNT_SRC_VEDIRECT && haveFrame)
  {
    last_vedirect = t;
    if(!Lcd.Data.VEData._currentValue)
      Lcd.Data.VEData.setValue(true);
    log_d("Data Available to Process");
    VEDataProcess();
    activeShuntLink = SHUNT_SRC_VEDIRECT;
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
