
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
#include "WifiMQTTManager.h"
#include <ArduinoJson.h> // Include ArduinoJson Library
#include <AsyncElegantOTA.h>

WiFiClient _wifiClient;

#include "mEEPROM.h"
mEEPROM pref;

#include "VEDirectFrameHandler.h"
#include "TimeLib.h"
#include "CANBUS.h"

uint32_t SendCanBusMQTTUpdates;
CANBUS Inverter;

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

void setup()
{
  pref.begin();
  Serial.begin(115200);

  if (!pref.isKey("EEPROMSetup"))
  {
    log_d("EEPROM not setup, writing inital values.");
    // pref.putBool("WifiEnabled", true);
    pref.putBool(ccCANBusEnabled, true);
    pref.putBool(ccLcdEnabled, false);
    // pref.putBool("MQTTEnabled", false);
    pref.putUInt(ccCanCSPin, CAN_BUS_CS_PIN);
    pref.putUInt(ccVictronRX, VEDIRECT_RX);
    pref.putUInt(ccVictronTX, VEDIRECT_TX);
    pref.putUInt(ccChargeVolt, initBattChargeVoltage);
    pref.putUInt(ccChargeCurrent, initBattChargeCurrent);
    pref.putUInt(ccDischargeVolt, initBattDischargeVoltage);
    pref.putUInt(ccDischargeCurrent, initBattDischargeCurrent);
    pref.putUInt(ccLowSOCLimit, initLowSOCLimit);
    pref.putUInt(ccHighSOCLimit, initHighSOCLimit);
    pref.putUInt(ccBattCapacity, initBattCapacity);
    pref.putBool(ccPylonTech, false);
    pref.putUInt("VE_WAIT_TIME", VE_WAIT_TIME);
    pref.putUInt("VE_STARTUP_TIME", VE_STARTUP_TIME);
    pref.putUInt("VE_LCD_REFRESH", VE_LCD_REFRESH);
    pref.putUInt("VE_MQTT_REC", VE_MQTT_RECONNECT);
    pref.putUInt(ccVELOOPTIME, VE_LOOP_TIME);
    pref.putBool("EEPROMSetup", true);
  }
  else
    log_d("EEPROM Store opened, initial key found.");

  VE_WAIT_TIME = pref.getUInt("VE_WAIT_TIME", VE_WAIT_TIME);
  VE_STARTUP_TIME = pref.getUInt("VE_STARTUP_TIME", VE_STARTUP_TIME);
  VE_LCD_REFRESH = pref.getUInt("VE_LCD_REFRESH", VE_LCD_REFRESH);
  VE_MQTT_RECONNECT = pref.getUInt("VE_MQTT_REC", VE_MQTT_RECONNECT);
  VE_LOOP_TIME = pref.getUInt(ccVELOOPTIME, VE_LOOP_TIME);

  if(pref.getBool(ccLcdEnabled,false)) {
    Lcd.Begin(Lcd.LCD2004);
    Lcd.SetScreen(Lcd.StartUp);
  }

  // #ifdef USE_OTA
  // OTA_WAIT_TIME = pref.getInt("OTA_WAIT_TIME", OTA_WAIT_TIME);
  // #endif
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

  if (Inverter.Begin(pref.getUInt(ccCanCSPin, (uint32_t)CAN_BUS_CS_PIN)))
  {
    Lcd.Data.CANInit.setValue(true);
    Inverter.SetChargeVoltage(pref.getUInt(ccChargeVolt, initBattChargeVoltage));
    Inverter.SetMaxChargeCurrent(pref.getUInt(ccChargeCurrent, initBattChargeCurrent));
    Inverter.SetDischargeVoltage(pref.getUInt(ccDischargeVolt, initBattDischargeVoltage));
    Inverter.SetMaxDischargeCurrent(pref.getUInt(ccDischargeCurrent, initBattDischargeCurrent));
    Inverter.SetLowSOCLimit((uint8_t) pref.getUInt(ccLowSOCLimit, initLowSOCLimit));
    Inverter.SetHighSOCLimit((uint8_t) pref.getUInt(ccHighSOCLimit, initHighSOCLimit));
    Inverter.SetBattCapacity(pref.getUInt(ccBattCapacity, initBattCapacity));
    Inverter.EnablePylonTech(pref.getBool(ccPylonTech, false));
    Inverter.SetSlowChargeDivider(1,pref.getUInt(ccSlowSOCDivider1,initSlowSOCDivider1));
    Inverter.SetSlowChargeDivider(2,pref.getUInt(ccSlowSOCDivider2,initSlowSOCDivider2));
    Inverter.SetSlowChargeSOCLimit(1, pref.getUInt(ccSlowSOCCharge1, initSlowSOCCharge1));
    Inverter.SetSlowChargeSOCLimit(2, pref.getUInt(ccSlowSOCCharge2, initSlowSOCCharge2));
    Inverter.StartRunTask();
  }
  else
  {
    Lcd.Data.CANInit.setValue(false);
    Lcd.Data.CANBusData.setValue(false);
  }

  SendCanBusMQTTUpdates = millis();
  //ve.begin();
  Lcd.UpdateScreenValues();

  xTaskCreate(&taskStartWebServices,"taskStartWebServices",4096, NULL, 6, NULL);

  if(veHandle.OpenSerial((uint8_t) pref.getUInt(ccVictronRX,VEDIRECT_RX), (uint8_t) pref.getUInt(ccVictronTX,VEDIRECT_TX)))
      veHandle.startReadTask();

  xTaskCreate(&TaskSetClock,"taskSetClock", 2048, NULL, 5, NULL); 
  // Set the lcd timer
  time_t t = time(nullptr);
  last_lcd_refresh = t;

  return;
}

void loop()
{

#ifdef M5STACK
  M5.update();
#endif

  time_t t = time(nullptr);
 
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
  }

}
