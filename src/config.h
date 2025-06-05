#pragma once
#include <Arduino.h>

#ifndef CONFIG_H
#define CONFIG_H

/*
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
#define SYSLOG 1

#define initBattChargeVoltage 55600       // Battery Charge Voltage sent to inverter
#define initBattFullVoltage 0             // Battery Full Voltage - keep SOC from 100% until this value.
#define initBattOverVoltage 0             // Battery Over Voltage - stop charging if Batt Volts hits this.
#define initBattDischargeVoltage 45000    // Battery discharge voltage, not currently used
#define initBattChargeCurrent 0           // in mA, 
#define initBattDischargeCurrent 0        // in mA 
#define initBattCapacity 0                // used for charge limits when batteries becoming full.
#define initLowSOCLimit 0                 // Stop discharge limit (defauly only)
#define initHighSOCLimit 100              // Stop Charge above this limit (default only)
#define initSlowSOCCharge1 100
#define initSlowSOCCharge2 100
#define initSlowSOCDivider1 0
#define initSlowSOCDivider2 0
#define initAdjustStep 0                  // If smart charging this is the amount the current is changed by in each step
#define initSmartInterval 0               // Default Interval for Smart Adjust
#define initMinChargeCurrent 0            // Minimum supported charging current, Inverter dependant.
#define initMinDischargeCurrent 0         // Minimum supported discharging current
#define initCAN16Mhz false                // This is the default MCP2515 8mhz Crystal speed, use 16mhz if true
// To use strict PYLONTECH Protocol enable below
//#define USE_PYLONTECH

//
// Use OneWire temperature sensors 
//
//#define USE_ONEWIRE

#ifdef USE_ONEWIRE
#define ONEWIRE_PIN 22
/*
   define the wait time between 2 attempts to send one wire data
   300000 = every 5 minutes
*/
int OW_WAIT_TIME = 10; // in s
time_t last_ow;
#endif

/*
   WiFi parameters
*/
#define WIFIMANAGER

#ifdef USE_ONEWIRE
String MQTT_ONEWIRE = "/Temp/OneWire";
#endif

/*
   Wait time in Loop
   this determines how many frames are send to MQTT
   if wait time is e.g. 10 minutes, we will send only every 10 minutes to MQTT
   Note: only the last incoming block will be send; all previous blocks will be discarded
   Wait time is in seconds
   Waittime of 1 or 0 means every received packet will be transmitted to MQTT
   Packets during OTA or OneWire will be discarded
*/
uint8_t VE_WAIT_TIME = 1; // in s
// Change from start up screen to normal after
uint8_t VE_STARTUP_TIME = 30;
// Update LCD Screen
uint8_t VE_LCD_REFRESH = 1;

uint8_t VE_MQTT_RECONNECT = 5;

uint8_t VE_LOOP_TIME = 5;

uint8_t ONEWIRE_PIN = 0;



#define AppCore 1
#define SysCore 0

#endif