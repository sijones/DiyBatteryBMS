
/*
PYLON Protocol, messages sent every 1 second.

Common to v1.2 and v1.3:
0x351 – Battery voltage + current limits (8 bytes)
0x355 – State of Charge (SOC) / State of Health (SOH) (4 bytes v1.2, 6 bytes v1.3)
0x356 – Voltage / Current / Temp (6 bytes)
0x35E – Manufacturer name “PYLON   “ (8 bytes)

Pylontech v1.2 only:
0x359 – Protection & Alarm flags (8 bytes)
0x35C – Battery charge request flags (2 bytes)

Pylontech v1.3 only (replaces 0x359/0x35C):
0x35A – Alarms & Warnings (8 bytes, bit-pair alarm/clear format)
0x35F – Battery type & BMS info (8 bytes)

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

enum CANProtocol : uint8_t {
  PROTO_PYLONTECH_12 = 0,  // Pylontech v1.2
  PROTO_PYLONTECH_13 = 1,  // Pylontech v1.3
  PROTO_SMA          = 2,  // SMA Sunny Island
  PROTO_VICTRON      = 3,  // Victron CAN-BMS
  PROTO_GROWATT      = 4   // Growatt / SolArk
};

class CANBUS {
  private:
//#pragma once

#ifndef ESPCAN
MCP_CAN *CAN;
#endif

uint8_t CAN_MSG[8];
uint8_t MSG_PYLON[8] = {0x50,0x59,0x4C,0x4F,0x4E,0x20,0x20,0x20};   // "PYLON   "
uint8_t MSG_VICTRON[8] = {0x44,0x49,0x59,0x42,0x4D,0x53,0x20,0x20}; // "DIYBMS  "

bool _canbusEnabled = true;
bool _initialised = false;
bool _enableSOCTrick = false;
bool _enableRequestFlags = false;
bool _forceCharge = false;
bool _chargeEnabled = true;
bool _dischargeEnabled = true;
bool _dataChanged = false;
bool _useAutoCharge = true;

uint8_t _canSendDelay = 10;

// CC-CV Charge Phase State Machine (enum is public for protocol helpers)
public:
enum ChargePhase { PHASE_BULK, PHASE_ABSORPTION, PHASE_COMPLETE };
private:
ChargePhase _chargePhase = PHASE_BULK;
time_t _absorptionStartTime = 0;
time_t _tailCurrentStartTime = 0;
bool _tailCurrentSustained = false;
uint32_t _chargeAdjust = 0;
time_t _lastAdjustTime = 0;
bool _socOverrideLogged = false;

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
volatile uint8_t _battSOH = 100;
volatile uint16_t _battVoltage = 0;
volatile int32_t _battCurrentmA = 0;
volatile int16_t _battTemp = 10;
volatile uint32_t _battCapacity = 0;

volatile int32_t _battPower = 0;
volatile int32_t _timeToGo = -1;
volatile bool _alarmActive = false;
String _alarmReason = "";
String _pidString = "";
String _fwVersion = "";
String _serialNumber = "";
String _modelString = "";

    // Used to tell the inverter battery limits
volatile uint16_t _chargeVoltage = 0;
volatile uint32_t _dischargeVoltage = 0; 
volatile uint16_t _overVoltage = 0;
  // Dynamically set limits for charge/discharge
uint16_t _adjustStep = 2000;
volatile uint32_t _chargeCurrentmA = 0;
volatile uint32_t _dischargeCurrentmA = 0;
uint32_t _minChargeCurrent = 4000;

// CC-CV Charging Parameters
uint32_t _tailCurrentmA = 500;
uint16_t _tailCurrentDuration = 60;
uint16_t _maxAbsorptionTime = 120;
uint8_t  _rechargeSOC = 90;
uint16_t _rechargeVoltageOffset = 200;
uint32_t _minDischargeCurrent = 20000;

// Temperature protection
int16_t _chargeHighTemp = 45;
int16_t _chargeLowTemp = 0;
int16_t _dischargeHighTemp = 50;
int16_t _dischargeLowTemp = -20;
bool _tempProtectionEnabled = false;
bool _showTempOnDashboard = false;

bool _never100SOC = false;       // Never send 100% SOC over CAN
CANProtocol _canProtocol = PROTO_PYLONTECH_13;  // Selected inverter protocol

bool SendCANData_Pylontech();
bool SendCANData_SMA();
bool SendCANData_Victron();

// Temperature source selection
uint8_t _battTempSource = 0;      // 0=VE.Direct, 1=MQTT
uint8_t _fanTempSource = 0;       // 0=Disabled (current-based), 1=MQTT Inverter

// MQTT-received temperatures
volatile int16_t _mqttBattTemp = -127;      // -127 = no data yet
volatile int16_t _mqttInverterTemp = -127;

// Fan temperature control
int16_t _fanOffTemp = 30;    // Fan off below this (C)
int16_t _fanFullTemp = 50;   // Fan 100% at this (C)

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

// Inverter presence detection via 0x305 keepalive
volatile unsigned long _lastInverterSeen = 0;  // millis() of last 0x305 received
static const unsigned long INVERTER_TIMEOUT_MS = 5000;  // 5 seconds without 0x305 = offline

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

  // CC-CV Phase accessors
  ChargePhase GetChargePhase() { return _chargePhase; }
  const char* GetChargePhaseName();

  // CC-CV Parameter accessors
  uint32_t GetTailCurrentmA() { return _tailCurrentmA; }
  void SetTailCurrentmA(uint32_t value) { _tailCurrentmA = value; }
  uint16_t GetTailCurrentDuration() { return _tailCurrentDuration; }
  void SetTailCurrentDuration(uint16_t value) { _tailCurrentDuration = value; }
  uint16_t GetMaxAbsorptionTime() { return _maxAbsorptionTime; }
  void SetMaxAbsorptionTime(uint16_t value) { _maxAbsorptionTime = value; }
  uint8_t GetRechargeSOC() { return _rechargeSOC; }
  void SetRechargeSOC(uint8_t value) { _rechargeSOC = value; }
  uint16_t GetRechargeVoltageOffset() { return _rechargeVoltageOffset; }
  void SetRechargeVoltageOffset(uint16_t value) { _rechargeVoltageOffset = value; }

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
  #ifndef ESPCAN
  bool ReadMCP(unsigned long &id, uint8_t &len, uint8_t *buf);
  #endif
  bool DataChanged();
  void SetChargeVoltage(uint16_t Voltage);
  uint16_t GetChargeVoltage() {return _chargeVoltage; }
  void SetChargeCurrent(uint32_t CurrentmA);
  void SetDischargeCurrent(uint32_t CurrentmA);
  void SetMaxChargeCurrent(uint32_t CurrentmA) {_initialChargeCurrent = true; _maxChargeCurrentmA = CurrentmA;}
  void SetMaxDischargeCurrent(uint32_t CurrentmA) {_initialDischargeCurrent = true; _maxDischargeCurrentmA = CurrentmA;}
  void SetOverVoltage(uint16_t Voltage) { _overVoltage = Voltage; }
  uint32_t GetChargeCurrent() {return _chargeCurrentmA; }
  void SetDischargeVoltage(uint32_t Voltage);
  void SetBattCapacity(uint32_t BattCapacity){_battCapacity = BattCapacity;} 
  uint32_t GetBatteryCapacity() { return _battCapacity; }
  uint32_t GetDischargeVoltage() { return _dischargeVoltage; }
  uint16_t GetOverVoltage() { return _overVoltage; }

  uint8_t SmartInterval() { return SMARTINTERVAL; }
  void SmartInterval(uint8_t Value) { SMARTINTERVAL = Value; }

  bool ManualAllowCharge(){return _ManualAllowCharge;}
  bool ManualAllowDischarge(){return _ManualAllowDischarge;}
  void ManualAllowCharge(bool Value){if(Value != _ManualAllowCharge) {_ManualAllowCharge = Value; _dataChanged = true;} }
  void ManualAllowDischarge(bool Value){if(Value != _ManualAllowDischarge) {_ManualAllowDischarge = Value; _dataChanged = true;} }
  bool AutoCharge(){return _useAutoCharge;}
  void AutoCharge(bool Value) {_useAutoCharge = Value;}

  uint32_t GetChargeAdjust() { return _chargeAdjust; }
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
  void EnableSOCTrick(bool State) {_enableSOCTrick = State;} 
  void EnableRequestFlags(bool State) {_enableRequestFlags = State;}

  uint8_t BattSOC(){return _battSOC;}
  uint16_t BattVoltage(){return _battVoltage;}
  uint8_t BattSOH(){return _battSOH;}
  int32_t BattCurrentmA(){return _battCurrentmA;}
  int16_t BattTemp(){return _battTemp;}
  bool ForceCharge(){return _forceCharge;}
  bool ChargeEnable(){return _chargeEnabled;}
  bool DischargeEnable(){return _dischargeEnabled;}
  bool EnableSOCTrick(){return _enableSOCTrick;}
  bool EnableRequestFlags(){return _enableRequestFlags;}
  bool CanBusFailed(){return (_failedCanSendCount > _maxFailedCanSendCount || !_initialised); }

  // Additional VE.Direct parameter accessors
  int32_t BattPower(){return _battPower;}
  void BattPower(int32_t power){_battPower = power;}
  int32_t TimeToGo(){return _timeToGo;}
  void TimeToGo(int32_t ttg){_timeToGo = ttg;}
  bool AlarmActive(){return _alarmActive;}
  void AlarmActive(bool state){_alarmActive = state;}
  String AlarmReason(){return _alarmReason;}
  void AlarmReason(String reason){_alarmReason = reason;}
  String PIDString(){return _pidString;}
  void PIDString(String pid){_pidString = pid;}
  String FWVersion(){return _fwVersion;}
  void FWVersion(String fw){_fwVersion = fw;}
  String SerialNumber(){return _serialNumber;}
  void SerialNumber(String sn){_serialNumber = sn;}
  String ModelString(){return _modelString;}
  void ModelString(String model){_modelString = model;}

  // Temperature protection accessors
  int16_t GetChargeHighTemp() { return _chargeHighTemp; }
  void SetChargeHighTemp(int16_t v) { _chargeHighTemp = v; }
  int16_t GetChargeLowTemp() { return _chargeLowTemp; }
  void SetChargeLowTemp(int16_t v) { _chargeLowTemp = v; }
  int16_t GetDischargeHighTemp() { return _dischargeHighTemp; }
  void SetDischargeHighTemp(int16_t v) { _dischargeHighTemp = v; }
  int16_t GetDischargeLowTemp() { return _dischargeLowTemp; }
  void SetDischargeLowTemp(int16_t v) { _dischargeLowTemp = v; }
  bool TempProtectionEnabled() { return _tempProtectionEnabled; }
  void TempProtectionEnabled(bool v) { _tempProtectionEnabled = v; }
  bool ShowTempOnDashboard() { return _showTempOnDashboard; }
  void ShowTempOnDashboard(bool v) { _showTempOnDashboard = v; }

  bool Never100SOC() { return _never100SOC; }
  void Never100SOC(bool v) { _never100SOC = v; }
  CANProtocol GetCANProtocol() { return _canProtocol; }
  void SetCANProtocol(CANProtocol p) { _canProtocol = p; }
  bool InverterPresent() { return _lastInverterSeen > 0 && (millis() - _lastInverterSeen) < INVERTER_TIMEOUT_MS; }
  void InverterSeen() { _lastInverterSeen = millis(); }

  // Temperature source selectors
  uint8_t BattTempSource() { return _battTempSource; }
  void BattTempSource(uint8_t v) { _battTempSource = v; }
  uint8_t FanTempSource() { return _fanTempSource; }
  void FanTempSource(uint8_t v) { _fanTempSource = v; }

  // MQTT temperature accessors
  int16_t MqttBattTemp() { return _mqttBattTemp; }
  void MqttBattTemp(int16_t v) { _mqttBattTemp = v; }
  int16_t MqttInverterTemp() { return _mqttInverterTemp; }
  void MqttInverterTemp(int16_t v) { _mqttInverterTemp = v; }
  bool HasMqttInverterTemp() { return _mqttInverterTemp != -127; }

  // Fan temperature control accessors
  int16_t GetFanOffTemp() { return _fanOffTemp; }
  void SetFanOffTemp(int16_t v) { _fanOffTemp = v; }
  int16_t GetFanFullTemp() { return _fanFullTemp; }
  void SetFanFullTemp(int16_t v) { _fanFullTemp = v; }

}; // End of Class
