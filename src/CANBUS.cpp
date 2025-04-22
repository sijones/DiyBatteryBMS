#include "CANBUS.h"
#include "mEEPROM.h"

/*

0x35E len: 8 50 59 4C 4F 4E 20 20 20 (ok) (ok)
0x35C len: 2 C0 0 (ok) (ok)
0x356 len: 6 2 13 0 0 4A 1 (ok) (ok)
0x355 len: 4 E 0 64 0 (ok) (ok)
0x351 len: 8 14 2 74 E 74 E CC 1 (ok) (ok)
0x359 len: 7 0 0 0 0 A 50 4E (ok) (ok)

*/


void canSendTask(void * pointer){
  
//  portMUX_TYPE CanMutex = portMUX_INITIALIZER_UNLOCKED;

  CANBUS *Inverter = (CANBUS *) pointer;
  log_i("Starting CAN Bus send task");

  for (;;) {
//    taskENTER_CRITICAL(&CanMutex);
    if(!Inverter->SendAllUpdates())
        log_e("Failure returned from SendAllUpdates");
//    taskEXIT_CRITICAL(&CanMutex);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

bool CANBUS::Begin(uint8_t _CS_PIN) {

  if(_pref.isKey(ccCANBusEnabled))
    _canbusEnabled = _pref.getBool(ccCANBusEnabled,true);
  else
    _canbusEnabled = _pref.putBool(ccCANBusEnabled,true);

  if (!_canbusEnabled) return false;

  if (CAN != NULL)
    delete(CAN);
  
  CAN = new MCP_CAN(_CS_PIN);

  // Initialize MCP2515 running at 8MHz with a baudrate of 500kb/s and the masks and filters disabled.
  if (CAN->begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) == CAN_OK) 
  {
    // Change to normal mode to allow messages to be transmitted
    CAN->setMode(MCP_NORMAL);  
    log_i("CAN Bus initialised at 8Mhz");
    _initialised = true;
    CanBusAvailable = true;
    _failedCanSendTotal = 0;   
  }
  else if (CAN->begin(MCP_ANY, CAN_500KBPS, MCP_16MHZ) == CAN_OK) 
  {
    // Change to normal mode to allow messages to be transmitted
    CAN->setMode(MCP_NORMAL);  
    log_i("CAN Bus initialised at 16Mhz");
    _initialised = true;
    CanBusAvailable = true;
    _failedCanSendTotal = 0;   
  }
  else
  {
    log_e("CAN Bus Failed to Initialise");
    _initialised = false;
    return false;
  }
  
  _lastChangeInterval = time(nullptr);

  return true;
}

bool CANBUS::StartRunTask()
{
    if(CanBusAvailable){
    // Create task and pin to Core1
      xTaskCreatePinnedToCore(&canSendTask,"canSendTask",2048,this,6,&tHandle,1);    
      return true;
    }
    else
      return false;
}

bool CANBUS::SendAllUpdates()
{
  if (!_canbusEnabled) return false;

  if (Initialised() && Configured())
    {
    
    time_t t = time(nullptr);

    // Turn off force charge, this is defined in PylonTech Protocol
   // if (_battSOC > 96 && _forceCharge){
   //   ForceCharge(false);
   // }
   // Check Auto charge settings are setup
    if (!_enableAutoCharge){
      if(_overVoltage > 0 && _fullVoltage > 0 && _adjustStep > 0 && _minChargeCurrent > 0) _enableAutoCharge = true;
    }

    uint16_t _tempOverVoltage = (_overVoltage * 0.1);
    uint16_t _tempFullVoltage = (_fullVoltage * 0.1);
    // By default we want to be charging
    bool _tempChargeEnabled = true;
    // Track Charging / Discharging
    bool BatteryCharging = (_battCurrentmA >= 5) ? true : false;
    // New Charge Current
    uint32_t _tempChargingCurrent;

    // Calculate charging current
    if (_battCapacity > 0 && _initialBattData)
    {
      // Initial Calcuation
      if(_slowchargeSOC[1] > 0 && _battSOC >= _slowchargeSOC[1] && _slowchargeSOCdiv[1] > 0) {
        _tempChargingCurrent = (_battCapacity / _slowchargeSOCdiv[1]); 
        ChargingState = Level2;
      }
      else if(_slowchargeSOC[0] > 0 && _battSOC >= _slowchargeSOC[0] && _slowchargeSOCdiv[0] > 0) {
        _tempChargingCurrent = (_battCapacity / _slowchargeSOCdiv[0]);
        ChargingState = Level1;
      }
      else // If no limits set or not hit a limit
      {
        ChargingState = Bulk;
        _tempChargingCurrent = _maxChargeCurrentmA;
      }

      if(ChargingState != PrevChargingState){
        PrevChargingState = ChargingState;
        _chargeAdjust = 0;
      }

      if(_useAutoCharge && _enableAutoCharge) 
      {
          // Calculate if an adjust is required
          if(BatteryCharging && (_battVoltage > _tempFullVoltage) &&  (abs(t - _lastChangeInterval) >= SMARTINTERVAL) ) {
            _lastChangeInterval = t;
            //   25                   22                2               4 / 6
            if(((_tempChargingCurrent-_chargeAdjust) + _adjustStep) > (_minChargeCurrent+_adjustStep)) {
              _chargeAdjust+=_adjustStep;
              log_d("Decrease Charge Current %i and Adjustment is: %i",_tempChargingCurrent,_chargeAdjust);
            } else {
              // Stop charging if no more adjustment available
              ChargeEnable(false);
              log_d("Minimum Charge Hit, Stopping Charge by Charge Adjustment");
            }
          } 
          else
          {
            if (BatteryCharging && _battVoltage < (_tempFullVoltage-30) && _chargeAdjust > 0) {
              if(_chargeAdjust>=_adjustStep) {
                _chargeAdjust-=_adjustStep;
                if (_chargeAdjust<0) _chargeAdjust = 0;
                log_d("Increase Charge Current %i and Adjustment is: %i",_tempChargingCurrent,_chargeAdjust);
              }
            } else if (_battVoltage < (_tempFullVoltage-50) && _chargeEnabled == false && _battSOC <= 97) {
              // Start charging if battery voltage under full voltage by 500mV
              ChargeEnable(true);
              log_d("Charging Reenabled by Charge Adjustment");
            }
          }
      } // End of Auto Charge Logic

      _tempChargingCurrent=(_tempChargingCurrent-_chargeAdjust);
      // Here we check we dont go under the min charge
      if(_tempChargingCurrent < _minChargeCurrent)
        _tempChargingCurrent = _minChargeCurrent;

      // Check the charge current has changed and if so set it.
      if (_tempChargingCurrent != GetChargeCurrent())
        SetChargeCurrent(_tempChargingCurrent);
    }
    
    // If Low SOC is greater than 0 then we have a limit set
    // if the current battery soc is less or equal stop discharge
    if( _lowSOCLimit > 0) {
      if (_battSOC <= _lowSOCLimit && _dischargeEnabled == true)
        DischargeEnable(false);
      else if (_battSOC > _lowSOCLimit && _dischargeEnabled == false)
      {
        DischargeEnable(true);
      }
    }

    // If High SOC is set lower than 100 then we have a limit set
    // if the current battery soc is equal or more stop charge
    if(_highSOCLimit < 100) {
      if (_battSOC >= _highSOCLimit && _chargeEnabled == true)
      _tempChargeEnabled = false;
    }
    // If Battery Voltage is over "Battery Over Voltage" then stop charging.
    if ((_overVoltage > _dischargeVoltage && _battVoltage >= _tempOverVoltage))
      _tempChargeEnabled = false;
      
    if(_tempChargeEnabled != _chargeEnabled)
      ChargeEnable(_tempChargeEnabled);

    if (SendCANData()) 
      return true;
    else 
      return false;
  } 
  else 
  {
    log_e("CAN Bus Data not initialised or configured.");
    return false;
  }
    
}

void CANBUS::SetChargeVoltage(uint16_t Voltage){
  _initialChargeVoltage = true; 
  if(_chargeVoltage != Voltage) {
    _dataChanged = true;
    _chargeVoltage = Voltage;
    }
  }

void CANBUS::SetChargeCurrent(uint32_t CurrentmA){
  if (_chargeCurrentmA != CurrentmA && _initialDone) { 
    _dataChanged = true;
    _chargeCurrentmA = CurrentmA;
  } 

}

void CANBUS::SetDischargeVoltage(uint32_t Voltage){
  _initialDischargeVoltage = true; 
  _dischargeVoltage = Voltage;
  }

void CANBUS::SetDischargeCurrent(uint32_t CurrentmA){
  if (_dischargeCurrentmA != CurrentmA && _initialDone) {
    _dischargeCurrentmA = CurrentmA;
    _dataChanged = true;
    }
  }

void CANBUS::ForceCharge(bool State) {
  if (State != _forceCharge) _dataChanged = true;
  _forceCharge = State;
  }

void CANBUS::ChargeEnable(bool State) {
  if (State != _chargeEnabled) _dataChanged = true;
  _chargeEnabled = State;
  }

void CANBUS::DischargeEnable(bool State) {
  if (State != _dischargeEnabled) _dataChanged = true;
  _dischargeEnabled = State;
  }

bool CANBUS::DataChanged(){
  if (_dataChanged) {
    _dataChanged = false;
    return true;
  } else return false;
}

bool CANBUS::SendCANData(){

  if (!Initialised() && !Configured()) return false;

  uint16_t _tempFullVoltage = (_fullVoltage * 0.1);
  uint16_t _tempChargeVolt = (_chargeVoltage * 0.01);
  uint16_t _tempDisCharVolt = (_dischargeVoltage * 0.01);
  uint16_t _tempChargeCurr = (_chargeCurrentmA * 0.01);
  uint16_t _tempDisChargeCurr = (_dischargeCurrentmA * 0.01);
  int16_t _tempBattTemp = (_battTemp * 10);

  byte sndStat;
  _failedCanSendCount=0;

  // Send PYLON String
  //if(_enablePYLONTECH) {
  sndStat = CAN->sendMsgBuf(0x35E, 0, 8, MSG_PYLON);
  if (sndStat != CAN_OK){
    _failedCanSendCount++;
    _failedCanSendTotal++;
  }   
  delay(_canSendDelay);

  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  // 0x35C – C0 00 – Battery charge request flags - means allow charge and discharge
  CAN_MSG[0] = 0xC0;
  CAN_MSG[1] = 0x00;
  if(_enablePYLONTECH) {
    //CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagForceCharge,_forceCharge);
    CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagRequestFullCharge,_forceCharge);
    //CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagChargeEnable,(_chargeEnabled && _ManualAllowCharge) ? true : false);
    //CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagDischargeEnable,(_dischargeEnabled && _ManualAllowDischarge) ? true : false);
  } 

  //if (_forceCharge) CAN_MSG[0] |= bmsForceCharge;
  //if (_chargeEnabled && _ManualAllowCharge) CAN_MSG[0] |= bmsChargeEnable;
  //if (_dischargeEnabled && _ManualAllowDischarge) CAN_MSG[0] |= bmsDischargeEnable;
  
  sndStat = CAN->sendMsgBuf(0x35C, 2, CAN_MSG);
  if (sndStat != CAN_OK){
    _failedCanSendCount++;
    _failedCanSendTotal++;
  } 

  delay(_canSendDelay); 

    // Current measured values of the BMS battery voltage, battery current, battery temperature
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CAN_MSG[0] = lowByte(_battVoltage);
  CAN_MSG[1] = highByte(_battVoltage);
  CAN_MSG[2] = lowByte(int16_t(_battCurrentmA));
  CAN_MSG[3] = highByte(int16_t(_battCurrentmA));
  CAN_MSG[4] = lowByte(_tempBattTemp);
  CAN_MSG[5] = highByte(_tempBattTemp);

  sndStat = CAN->sendMsgBuf(0x356, 6, CAN_MSG);

  if (sndStat != CAN_OK){
    _failedCanSendCount++;
    _failedCanSendTotal++;
  } 
  delay(_canSendDelay); 

  memset(CAN_MSG,0x00,sizeof(CAN_MSG));

  //log_d("Statistics: SOC: %i, BV: %i, BC: %i, Full Voltage: %i, Discharge Voltage: %i",_battSOC,_battVoltage,_battCurrentmA,_tempFullVoltage,_dischargeVoltage);
  if(_enablePYLONTECH) {
    if(_fullVoltage > _dischargeVoltage && _battVoltage < _tempFullVoltage && _battSOC >= 99) {
      CAN_MSG[0] = lowByte(99);
      CAN_MSG[1] = highByte(99);      
    } 
    else {
      CAN_MSG[0] = lowByte(_battSOC);
      CAN_MSG[1] = highByte(_battSOC);
    }
  } 
  else if (_forceCharge) {
    CAN_MSG[0] = lowByte(u_int8_t(_battSOC * 0.1));
    CAN_MSG[1] = highByte(u_int8_t(_battSOC * 0.1));
  }
    else {
      CAN_MSG[0] = lowByte(_battSOC);
      CAN_MSG[1] = highByte(_battSOC);
    }
  

  CAN_MSG[2] = lowByte(_battSOH);
  CAN_MSG[3] = highByte(_battSOH);

  sndStat = CAN->sendMsgBuf(0x355, 4, CAN_MSG);
  if (sndStat != CAN_OK){
    _failedCanSendCount++;
    _failedCanSendTotal++;
  } 

  delay(_canSendDelay);

  memset(CAN_MSG,0x00,sizeof(CAN_MSG));

  // Battery charge and discharge parameters

  CAN_MSG[0] = lowByte(_tempChargeVolt);             // Maximum battery voltage
  CAN_MSG[1] = highByte(_tempChargeVolt);
  if((_chargeEnabled && _ManualAllowCharge)){
    CAN_MSG[2] = lowByte(_tempChargeCurr);         // Maximum charging current 
    CAN_MSG[3] = highByte(_tempChargeCurr);
  } else {
    CAN_MSG[2] = 0;                                                 // Maximum charging current 
    CAN_MSG[3] = 0;
  }
  if((_dischargeEnabled && _ManualAllowDischarge)){
    CAN_MSG[4] = lowByte(_tempDisChargeCurr);      // Maximum discharge current 
    CAN_MSG[5] = highByte(_tempDisChargeCurr);
  } else {
    CAN_MSG[4] = 0;                                       // Maximum discharge current 
    CAN_MSG[5] = 0;
  }
  CAN_MSG[6] = lowByte(_tempDisCharVolt);          // Currently not used by SOLIS
  CAN_MSG[7] = highByte(_tempDisCharVolt);         // Currently not used by SOLIS

  sndStat = CAN->sendMsgBuf(0x351, 8, CAN_MSG);

  if (sndStat != CAN_OK){
    _failedCanSendCount++;
    _failedCanSendTotal++;
  } 
  
  delay(_canSendDelay); 

  memset(CAN_MSG,0x00,sizeof(CAN_MSG));

  CAN_MSG[0] = 0x00;
  CAN_MSG[1] = 0x00;
  CAN_MSG[2] = 0x00;
  CAN_MSG[3] = 0x00;
  CAN_MSG[4] = 0x0A;
  CAN_MSG[5] = 0x50;
  CAN_MSG[6] = 0x4E;
  CAN_MSG[7] = 0x00;

  sndStat = CAN->sendMsgBuf(0x359, 8, CAN_MSG);
  if (sndStat != CAN_OK){
    _failedCanSendCount++;
    _failedCanSendTotal++;
  } 
  //delay(_canSendDelay); 

if(_failedCanSendCount > 0)
{
  log_e("Failed to Send CAN Packets: %i",_failedCanSendCount);
}
if (CanBusFailed()) 
    CanBusDataOK = false;
  else
    CanBusDataOK = true;
  
  if (_failedCanSendCount == 0)
    return true;
  else
    return false;

}

bool CANBUS::AllReady()
{
  if (_initialDone) 
    return true;
  else if (_initialBattSOC && _initialBattVoltage && _initialBattCurrent &&
          _initialChargeVoltage && _initialChargeCurrent && _initialDischargeVoltage && _initialDischargeCurrent)
    {
      _dischargeCurrentmA = _maxDischargeCurrentmA;
      _chargeCurrentmA = _maxChargeCurrentmA;
      _initialDone = true;
      _initialConfig = true;
      _initialBattData = true;
      log_d("All initial and inverter data set, going to running mode.");
      return true;
    } 
  else if (_initialChargeVoltage && _initialChargeCurrent 
            && _initialDischargeVoltage && _initialDischargeCurrent &&(!_initialConfig))
    { 
      _initialConfig = true;
      return false;
    }
  else 
    return false;
}

bool CANBUS::Configured()
{
  AllReady(); // Check if we need to set the flags

  if (_initialConfig) return true;
  else if (_initialChargeVoltage && _initialChargeCurrent 
      && _initialDischargeVoltage && _initialDischargeCurrent)
      {
        _initialConfig = true;
        return true;
      }
  else return false;
}

void CANBUS::SetSlowChargeSOCLimit(uint8_t SelectLimit, uint8_t SOC)
{
  _slowchargeSOC[SelectLimit-1] = SOC;
}

void CANBUS::SetSlowChargeDivider(uint8_t Selectlimit, uint8_t Divider)
{
  _slowchargeSOCdiv[Selectlimit-1] = Divider;
}

uint8_t CANBUS::GetSlowChargeSOCLimit(uint8_t SelectLimit)
{
  return _slowchargeSOC[SelectLimit-1];
}

uint8_t CANBUS::GetSlowChargeDivider(uint8_t Selectlimit)
{
  return _slowchargeSOCdiv[Selectlimit-1];
}