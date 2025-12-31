
/*
PYLON Protocol, messages sent every 1 second.

0x35E – 50 59 4C 4F 4E 20 20 20 – Manufacturer name (“PYLON   “) (8 bytes)
0x351 – 14 02 74 0E 74 0E CC 01 – Battery voltage + current limits (8 bytes)
0x355 – 1A 00 64 00 – State of Health (SOH) / State of Charge (SOC) (4 bytes)
0x356 – 4e 13 02 03 04 05 – Voltage / Current / Temp (6 bytes)
0x359 – 00 00 00 00 0A 50 4E – Protection & Alarm flags (7 bytes)
0x35C – C0 00 – Battery charge request flags (2 bytes)

*/

#include <Arduino.h>
#include <SPI.h>

#ifdef ESPCAN
// Both ESP32 and ESP32-S3 use TWAI driver
#include <driver/twai.h>
#else
#include <mcp_can.h>              // Library for CAN Interface      https://github.com/coryjfowler/MCP_CAN_lib
#endif

#include <mEEPROM.h>

#define flagChargeEnable 7      // Bit 7
#define flagDischargeEnable 6   // Bit 6
#define flagForceCharge 4       // Bit 4
#define flagRequestFullCharge 3 // Bit 3

class CANBUS {
  private:
//#pragma once

#ifndef ESPCAN
MCP_CAN *CAN;
#endif

uint8_t CAN_MSG[8];
uint8_t MSG_PYLON[8] = {0x50,0x59,0x4C,0x4F,0x4E,0x20,0x20,0x20};

bool _canbusEnabled = true;
bool _initialised = false;
bool _enablePYLONTECH = false;
bool _forceCharge = false;
bool _chargeEnabled = true;
bool _dischargeEnabled = true;
bool _dataChanged = false;
bool _useAutoCharge = true;
bool _enableAutoCharge = false;

uint8_t _canSendDelay = 10;

int ChargingState = Bulk;
int PrevChargingState = Bulk;

enum Charging {
  bmsForceCharge = 8,
  bmsDischargeEnable = 64,
  bmsChargeEnable = 128
};
// Flags set to check all data has come before starting CANBUS sending
bool _initialBattSOC = false;
bool _initialBattVoltage = false;
bool _initialBattCurrent = false;
bool _initialChargeVoltage = false;
bool _initialChargeCurrent = false;
bool _initialDischargeVoltage = false;
bool _initialDischargeCurrent = false;
bool _initialDone = false;
bool _initialConfig = false;
bool _initialBattData = false;

    // Used to tell the inverter battery data
volatile uint8_t _battSOC = 0;
volatile uint8_t _battSOH = 100; // State of health, not useful so defaulted to 100% 
volatile uint16_t _battVoltage = 0;
volatile int32_t _battCurrentmA = 0;
volatile int16_t _battTemp = 10;
volatile uint32_t _battCapacity = 0; // Only used for limiting current at high SOC.

    // Used to tell the inverter battery limits
volatile uint16_t _chargeVoltage = 0;
volatile uint32_t _dischargeVoltage = 0; 
volatile uint16_t _fullVoltage = 0;
volatile uint16_t _overVoltage = 0;
  // Dynamically set limits for charge/discharge
uint16_t _adjustStep = 2000;
volatile uint32_t _chargeCurrentmA = 0;
volatile uint32_t _dischargeCurrentmA = 0;
volatile uint32_t _chargeAdjust = 0;
uint32_t _dischargeAdjust = 0;
uint32_t _minChargeCurrent = 4000;
uint32_t _minDischargeCurrent = 20000;

  // Max Current Limits for Inverter.
uint32_t _maxChargeCurrentmA = 0;
uint32_t _maxDischargeCurrentmA = 0;

// Override charging, used by MQTT/Web.
bool _ManualAllowCharge = true;
bool _ManualAllowDischarge = true;

  // SOC Limits for allowing charge and discharge
  // and changing charge rate
uint8_t _lowSOCLimit = 10;
uint8_t _highSOCLimit = 100;

// Slow Charge SOC Limits
uint8_t _slowchargeSOC[2];
// Divider, calculation is battery capacity divide by below
// based on SOC being above the limit above
uint8_t _slowchargeSOCdiv[2];

// Track how many failed CAN BUS sends and reboot ESP if more than limit
uint8_t _maxFailedCanSendCount = 20;
uint16_t _failedCanSendCount = 0;
uint32_t _failedCanSendTotal = -1;

/* CAN Data Values, calculated every cycle 
   these Varibles are used to feed the 
   json sent to the webpage for debugging */

uint16_t _tempChargeVolt;
uint16_t _tempDisCharVolt; 
uint16_t _tempChargeCurr;  
uint16_t _tempDisChargeCurr;

uint32_t LoopTimer; // store current time
// The interval for sending the inverter updated information
// Normally around 5 seconds is ok, but for Pylontech protocol it's around every second.
uint16_t _CanBusSendInterval = 1000; 

// How long between change of charge current before making another change in seconds.
time_t _lastChangeInterval;
uint8_t SMARTINTERVAL = 2;

// Task Handle
TaskHandle_t tHandle = NULL;

mEEPROM _pref;

// Number is the value to alter, n is the bit to change, 
// x to set to 1 or 0
inline uint8_t bit_set_to(uint8_t number, uint8_t n, bool x) {
    return (number & ~((uint8_t)1 << n)) | ((uint8_t)x << n);
}

public:

  portMUX_TYPE CANMutex = portMUX_INITIALIZER_UNLOCKED;

  bool CanBusAvailable = false;
  bool CanBusDataOK = false;

  enum ChargingStates {
    Bulk,
    Level1,
    Level2
  };

  enum Command
  {
    ChargeDischargeLimits = 0x351,
    BattVoltCurrent = 0x356,
    StateOfCharge = 0x355
  };

  //void CANBUSBMS();
  
  #ifdef ESPCAN
  bool Begin(uint8_t ESPCAN_TX_PIN, uint8_t ESPCAN_RX_PIN, uint8_t ESPCAN_EN_PIN);
  #else
  bool Begin(uint8_t _CS_PIN, bool _CAN16Mhz);
  #endif

  bool StartRunTask();
  bool StartRunTask(bool Run);
  bool SendBattUpdate(uint8_t SOC, uint16_t Voltage, int32_t CurrentmA, int16_t BattTemp, uint8_t SOH);
  bool SendAllUpdates();
  bool SendBattUpdate();
  bool SendCANData();
  bool SendToDriver(u_int32_t CMD,uint8_t Bytes, uint8_t * Data);
  bool DataChanged();
  void SetChargeVoltage(uint16_t Voltage);
  uint16_t GetChargeVoltage() {return _chargeVoltage; }
  void SetChargeCurrent(uint32_t CurrentmA);
  void SetDischargeCurrent(uint32_t CurrentmA);
  void SetMaxChargeCurrent(uint32_t CurrentmA) {_initialChargeCurrent = true; _maxChargeCurrentmA = CurrentmA;}
  void SetMaxDischargeCurrent(uint32_t CurrentmA) {_initialDischargeCurrent = true; _maxDischargeCurrentmA = CurrentmA;}
  void SetFullVoltage(uint16_t Voltage) { _fullVoltage = Voltage; }
  void SetOverVoltage(uint16_t Voltage) { _overVoltage = Voltage; }
  uint32_t GetChargeCurrent() {return _chargeCurrentmA; }
  void SetDischargeVoltage(uint32_t Voltage);
  void SetBattCapacity(uint32_t BattCapacity){_battCapacity = BattCapacity;} 
  uint32_t GetBatteryCapacity() { return _battCapacity; }
  uint32_t GetDischargeVoltage() { return _dischargeVoltage; }
  uint16_t GetFullVoltage() { return _fullVoltage; }
  uint16_t GetOverVoltage() { return _overVoltage; }

  uint8_t SmartInterval() { return SMARTINTERVAL; }
  void SmartInterval(uint8_t Value) { SMARTINTERVAL = Value; }

  bool ManualAllowCharge(){return _ManualAllowCharge;}
  bool ManualAllowDischarge(){return _ManualAllowDischarge;}
  void ManualAllowCharge(bool Value){if(Value != _ManualAllowCharge) {_ManualAllowCharge = Value; _dataChanged = true;} }
  void ManualAllowDischarge(bool Value){if(Value != _ManualAllowDischarge) {_ManualAllowDischarge = Value; _dataChanged = true;} }
  bool AutoCharge(){return _useAutoCharge;}
  void AutoCharge(bool Value) {_useAutoCharge = Value;}

  uint32_t GetChargeAdjust() { return _chargeAdjust;}
  void     SetChargeStepAdjust(uint16_t Value) {_adjustStep = Value;}
  uint32_t MinChargeCurrent() { return _minChargeCurrent;}
  void     MinChargeCurrent(uint32_t Value) { _minChargeCurrent = Value;}

  uint32_t GetDischargeCurrent() { return _dischargeCurrentmA; }
  uint32_t GetFailedTotalCount() { return _failedCanSendTotal; }
  uint32_t GetMaxChargeCurrent() { return _maxChargeCurrentmA; }
  uint32_t GetMaxDischargeCurrent() { return _maxDischargeCurrentmA; }
  uint8_t GetLowSOCLimit() { return _lowSOCLimit; }
  void SetLowSOCLimit(uint8_t Limit) { _lowSOCLimit = Limit; }
  uint8_t GetHighSOCLimit() { return _highSOCLimit; }
  void SetHighSOCLimit(uint8_t Limit) { _highSOCLimit = Limit; }

  void SetSlowChargeSOCLimit(uint8_t SelectLimit, uint8_t SOC);
  void SetSlowChargeDivider(uint8_t SelectLimit, uint8_t Divider);

  uint8_t GetSlowChargeSOCLimit(uint8_t SelectLimit);
  uint8_t GetSlowChargeDivider(uint8_t SelectLimit);

  void CANBusEnabled(bool State) { _canbusEnabled = State;}
  bool CANBusEnabled() { return _canbusEnabled; }
  void ChargeEnable(bool);
  void DischargeEnable(bool);
  bool AllReady();
  void ForceCharge(bool);
  bool Initialised(){return _initialised;}
  bool Configured();

  void BattSOC(uint8_t soc){_initialBattSOC = true; _battSOC = soc;}
  void BattVoltage(uint16_t voltage){_initialBattVoltage = true; _battVoltage = voltage;}
  void BattSOH(uint8_t soh){_battSOH = soh;}
  void BattCurrentmA(int32_t currentmA){_initialBattCurrent = true; _battCurrentmA = currentmA;}
  void BattTemp(int16_t batttemp){_battTemp = batttemp;}
  void EnablePylonTech(bool State) {_enablePYLONTECH = State;} 

  uint8_t BattSOC(){return _battSOC;}
  uint16_t BattVoltage(){return _battVoltage;}
  uint8_t BattSOH(){return _battSOH;}
  int32_t BattCurrentmA(){return _battCurrentmA;}
  int16_t BattTemp(){return _battTemp;}
  bool ForceCharge(){return _forceCharge;}
  bool ChargeEnable(){return _chargeEnabled;}
  bool DischargeEnable(){return _dischargeEnabled;}
  bool EnablePylonTech(){return _enablePYLONTECH;}
  bool CanBusFailed(){return (_failedCanSendCount > _maxFailedCanSendCount || !_initialised); }

}; // End of Class
