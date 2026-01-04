
/*

   VE.Direct to CAN BUS & MQTT Gateway using a ESP32 Board
   Copyright (c) 2025 Simon Jones

   Free to use in personal projects and modify for your own use, no permission for 
   selling or commericalising this code in this project.
   
   The copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

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
#include "LittleFS.h"
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

uint32_t SendCanBusMQTTUpdates;
CANBUS Inverter;

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
  delay(100);
  
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
    pref.putUInt16(ccFullVoltage,initBattFullVoltage);
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
    pref.putUInt8(ccBattCapacity, initBattCapacity);
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
    
  if (LittleFS.begin(false))
  {
    Lcd.Data.LittleFSMounted.setValue(true);
    log_d("LittleFS storage mounted successfully.");
    LittleFS.exists("/index.htm") ? log_d("index.htm file found.") : log_d("index.htm file NOT found.");
    log_d("Testing for index-ap.htm file...");
    LittleFS.exists("/index-ap.htm") ? log_d("index-ap.htm file found.") : log_d("index-ap.htm file NOT found.");
  }
  else
  {
    Lcd.Data.LittleFSMounted.setValue(false);
    log_d("Failed to mount storage (LittleFS).");
  }
  
  if (!wifiManager.begin())
  {
    // Failed to configure, start the basics to enable web configuration
    // on an Access Point
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    StartWebServices();
    server.begin();
  }

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
  Inverter.SetFullVoltage((u_int16_t) pref.getUInt32(ccFullVoltage, initBattFullVoltage));
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
  Inverter.SetSlowChargeDivider(1,pref.getUInt8(ccSlowSOCDivider1,initSlowSOCDivider1));
  Inverter.SetSlowChargeDivider(2,pref.getUInt8(ccSlowSOCDivider2,initSlowSOCDivider2));
  Inverter.SetSlowChargeSOCLimit(1, pref.getUInt8(ccSlowSOCCharge1, initSlowSOCCharge1));
  Inverter.SetSlowChargeSOCLimit(2, pref.getUInt8(ccSlowSOCCharge2, initSlowSOCCharge2));
  Inverter.AutoCharge(pref.getBool(ccAutoAdjustCharge, true));
  Inverter.SmartInterval(pref.getUInt8(ccSmartInterval,initSmartInterval));
  
  if(pref.getBool(ccCANBusEnabled,true) && canInitOK) {
    Inverter.StartRunTask();
    WS_LOG_I("CAN Bus task started");
  }

  SendCanBusMQTTUpdates = millis();
  Lcd.UpdateScreenValues();
  xTaskCreate(&taskStartWebServices,"taskStartWebServices",4096, NULL, 6, NULL);

  // Start VE.Direct Serial Reading if Enabled
  uint8_t vrx = (uint8_t) pref.getUInt8(ccVictronRX, 0);
  uint8_t vtx = (uint8_t) pref.getUInt8(ccVictronTX, 0);
  if (vrx && vtx && !IsForbiddenPin(vrx) && !IsForbiddenPin(vtx)) {
    if(veHandle.OpenSerial(vrx, vtx))
      veHandle.startReadTask();
  } else if (vrx || vtx) {
    log_e("Forbidden or zero GPIO for VE.Direct pins: RX=%u TX=%u", vrx, vtx);
  }
  // Start NTP Clock Set Task
  xTaskCreate(&TaskSetClock,"taskSetClock", 2048, NULL, 5, NULL); 
  // Set the lcd timer
  time_t t = time(nullptr);
  last_lcd_refresh = t;
  log_d("Setup complete, starting loop.");
  WS_LOG_I("System initialization complete, entering main loop");
  return;
}

void loop()
{

  time_t t = time(nullptr);
 
  if(WiFi.isConnected() && FirstRun && mqttEnabled) {
    connectToMqtt();
    WS_LOG_I("WiFi connected, IP: %s", WiFi.localIP().toString().c_str());
    FirstRun = false; }
  
  wifiManager.loop(); 

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

  if (veHandle.dataavailable())
  {
    last_vedirect = t;
    if(!Lcd.Data.VEData._currentValue)
      Lcd.Data.VEData.setValue(true);
    log_d("Data Available to Process");
    VEDataProcess();
    if (wifiManager.isWiFiConnected())
    {
      sendVE2MQTT();
      ws.cleanupClients();
      notifyWSClients(false);
    }
  }

// Time out for data arrival
  if ((abs(t - last_vedirect) > 2) && Lcd.Data.VEData._currentValue)
      Lcd.Data.VEData.setValue(false);

  // Send MQTT Data every 15 seconds or if the inverter data has changed
  if ((((millis() - SendCanBusMQTTUpdates) > 15000) || Inverter.DataChanged()) && Lcd.Data.VEData.getValue() == true)
  {
    log_d("Send MQTT Param Update");
    SendCanBusMQTTUpdates = millis();
    sendUpdateMQTTData();
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
    if(FAN_INIT){
      FanUpdate((b * 0.1));
    }
  }

  // Yield to watchdog and other tasks
  yield();
  delay(1);

}
