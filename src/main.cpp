
/*

   VE.Direct to CAN BUS & MQTT Gateway using a ESP32 Board
   Copyright (c) 2022 Simon Jones

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
    pref.putUInt8(ccCanCSPin, CAN_BUS_CS_PIN);
#endif
    pref.putUInt8(ccVictronRX, VEDIRECT_RX);
    pref.putUInt8(ccVictronTX, VEDIRECT_TX);
#ifdef ESPCAN
    pref.putUInt8(ccCAN_EN_PIN,CAN_EN_PIN);
    pref.putUInt8(ccCAN_RX_PIN,CAN_RX_PIN);
    pref.putUInt8(ccCAN_TX_PIN,CAN_TX_PIN);
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
  
  // Setup FAN, will only complete if a PIN number is assigned.
  FanInit(pref.getUInt8(ccFanPin,0));

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
  if(Inverter.Begin(pref.getUInt8(ccCAN_TX_PIN,CAN_TX_PIN),pref.getUInt8(ccCAN_RX_PIN,CAN_RX_PIN),pref.getUInt8(ccCAN_EN_PIN,CAN_EN_PIN)))
#else
  if (Inverter.Begin(pref.getUInt8(ccCanCSPin, (uint32_t)CAN_BUS_CS_PIN), pref.getBool(ccCAN16Mhz,initCAN16Mhz)))
#endif
  {
    Lcd.Data.CANInit.setValue(true);
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
    Inverter.EnablePylonTech(pref.getBool(ccPylonTech, false));
    Inverter.SetSlowChargeDivider(1,pref.getUInt8(ccSlowSOCDivider1,initSlowSOCDivider1));
    Inverter.SetSlowChargeDivider(2,pref.getUInt8(ccSlowSOCDivider2,initSlowSOCDivider2));
    Inverter.SetSlowChargeSOCLimit(1, pref.getUInt8(ccSlowSOCCharge1, initSlowSOCCharge1));
    Inverter.SetSlowChargeSOCLimit(2, pref.getUInt8(ccSlowSOCCharge2, initSlowSOCCharge2));
    Inverter.AutoCharge(pref.getBool(ccAutoAdjustCharge, true));
    Inverter.SmartInterval(pref.getUInt8(ccSmartInterval,initSmartInterval));
    if(pref.getBool(ccCANBusEnabled,true))
      Inverter.StartRunTask();
  }
  else
  {
    Lcd.Data.CANInit.setValue(false);
    Lcd.Data.CANBusData.setValue(false);
  }

  SendCanBusMQTTUpdates = millis();
  
  Lcd.UpdateScreenValues();

  xTaskCreate(&taskStartWebServices,"taskStartWebServices",4096, NULL, 6, NULL);

  if(veHandle.OpenSerial((uint8_t) pref.getUInt8(ccVictronRX,VEDIRECT_RX), (uint8_t) pref.getUInt8(ccVictronTX,VEDIRECT_TX)))
      veHandle.startReadTask();

  xTaskCreate(&TaskSetClock,"taskSetClock", 2048, NULL, 5, NULL); 
  // Set the lcd timer
  time_t t = time(nullptr);
  last_lcd_refresh = t;
  log_d("Setup complete, starting loop.");
  return;
}

void loop()
{

  time_t t = time(nullptr);
 
  if(WiFi.isConnected() && FirstRun && mqttEnabled) {
    connectToMqtt();
    FirstRun = false; }
  
  wifiManager.loop(); 

  while(Serial1.available() > 0)
      veHandle.rxData(Serial1.read());

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
    // Update LCD Screen Values
    Lcd.Data.ChargeVolts.setValue(Inverter.GetChargeVoltage());
    Lcd.Data.ChargeAmps.setValue(Inverter.GetChargeCurrent());
    Lcd.Data.DischargeVolts.setValue(Inverter.GetDischargeVoltage());
    Lcd.Data.DischargeAmps.setValue(Inverter.GetDischargeCurrent());
    Lcd.Data.ChargeEnable.setValue((Inverter.ChargeEnable() && Inverter.ManualAllowCharge()) ? true : false);
    Lcd.Data.DischargeEnable.setValue((Inverter.DischargeEnable() && Inverter.ManualAllowDischarge()) ? true : false);
    Lcd.Data.WifiConnected.setValue(WiFi.isConnected());
    Lcd.Data.MQTTConnected.setValue(mqttClient.connected());
    Lcd.Data.IPAddr.setValue(wifiManager.GetIPAddr());
    Lcd.Data.CANBusData.setValue(!Inverter.CanBusFailed());
    Lcd.Data.ForceCharging.setValue(Inverter.ForceCharge());
    CheckAndChangeLCD();
    Lcd.UpdateScreenValues();
    if(FAN_INIT){
      int32_t b = Inverter.BattCurrentmA();
      if (b < 0)
        b = -b;
      FanUpdate((b * 0.1));
    }
  }

}
