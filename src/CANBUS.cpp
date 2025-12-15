#include "CANBUS.h"
#include "mEEPROM.h"

#ifdef ESPCAN
  #ifdef ESPCAN_S3
    // ESP32-S3 TWAI specific variables
    const int rx_queue_size = 10;
  #else
    // Original ESP32 CAN config
    CAN_device_t CAN_cfg;             // CAN Config
    const int interval = 1000;        // interval at which send CAN Messages (milliseconds)
    const int rx_queue_size = 10;     // Receive Queue size
  #endif
#else

#endif

bool CANBUS::SendToDriver(uint32_t CMD,uint8_t Length,uint8_t *Data) {
  
#ifdef ESPCAN
  #ifdef ESPCAN_S3
    // ESP32-S3 TWAI implementation
    twai_message_t tx_msg;
    tx_msg.identifier = CMD;
    tx_msg.data_length_code = (Length > 8) ? 8 : Length;
    tx_msg.flags = TWAI_MSG_FLAG_NONE;  // Standard frame, no RTR
    memcpy(tx_msg.data, Data, tx_msg.data_length_code);
    
    if (twai_transmit(&tx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
      return true;
    } else {
      return false;
    }
  #else
    // Original ESP32 implementation
    CAN_frame_t tx_frame;
    tx_frame.FIR.B.FF = CAN_frame_std;
    tx_frame.MsgID = CMD;
    if (Length > 8) Length = 8; // Make sure a maximum of 8 bytes can be copied.
    tx_frame.FIR.B.DLC = Length;
    memcpy(tx_frame.data.u8,Data,Length);
    tx_frame.FIR.B.RTR = CAN_no_RTR;
    ESP32Can.CANWriteFrame(&tx_frame);
    return true;
  #endif
#else
  byte sndStat;
  //sndStat = CAN->sendMsgBuf(0x35E, 0, 8, MSG_PYLON);
  sndStat = CAN->sendMsgBuf(CMD, Length, Data);
  if (sndStat == CAN_OK)
    return true;
  else
    return false;
#endif
}

void canSendTask(void * pointer){
  
//  portMUX_TYPE CanMutex = portMUX_INITIALIZER_UNLOCKED;

  CANBUS *Inverter = (CANBUS *) pointer;
  log_i("Starting CAN Bus send task");

  while (true)
  {
  //    taskENTER_CRITICAL(&CanMutex);

#ifdef ESPCAN
  #ifdef ESPCAN_S3
      // ESP32-S3 TWAI receive implementation
      twai_message_t rx_msg;
      
      // Receive next CAN frame (non-blocking)
      if (twai_receive(&rx_msg, 0) == ESP_OK)
      {
        if (rx_msg.flags & TWAI_MSG_FLAG_EXTD)
        {
          log_i("New extended frame");
        }
        else
        {
          log_i("New standard frame");
        }

        if (rx_msg.flags & TWAI_MSG_FLAG_RTR)
        {
          log_i(" RTR from 0x%08X, DLC %d", rx_msg.identifier, rx_msg.data_length_code);
        }
        else
        {
          log_i(" from 0x%08X, DLC %d, Data ", rx_msg.identifier, rx_msg.data_length_code);
          for (int i = 0; i < rx_msg.data_length_code; i++)
          {
            log_i("0x%02X ", rx_msg.data[i]);
          }
        }
      }
  #else
      // Original ESP32 implementation
      CAN_frame_t rx_frame;

      // Receive next CAN frame from queue
      if (xQueueReceive(CAN_cfg.rx_queue, &rx_frame, 0) == pdTRUE)
      {
        if (rx_frame.FIR.B.FF == CAN_frame_std)
        {
          log_i("New standard frame");
        }
        else
        {
          log_i("New extended frame");
        }

        if (rx_frame.FIR.B.RTR == CAN_RTR)
        {
          log_i(" RTR from 0x%08X, DLC %d", rx_frame.MsgID, rx_frame.FIR.B.DLC);
        }
        else
        {
          log_i(" from 0x%08X, DLC %d, Data ", rx_frame.MsgID, rx_frame.FIR.B.DLC);
          for (int i = 0; i < rx_frame.FIR.B.DLC; i++)
          {
            log_i("0x%02X ", rx_frame.data.u8[i]);
          }
        }
      }
  #endif
#endif
        if(!Inverter->SendAllUpdates())
          log_e("Failure returned from SendAllUpdates");
    //    taskEXIT_CRITICAL(&CanMutex);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
  }

  vTaskDelete(nullptr);

}

// End of canSendTask

#ifdef ESPCAN
bool CANBUS::Begin(uint8_t ESPCAN_TX_PIN, uint8_t ESPCAN_RX_PIN, uint8_t ESPCAN_EN_PIN) {
#else
bool CANBUS::Begin(uint8_t _CS_PIN, bool _CAN16Mhz) {
#endif

  #ifdef ESPCAN
  #ifdef ESPCAN_S3
    // ESP32-S3 TWAI initialization
    if (ESPCAN_EN_PIN > 0 && ESPCAN_EN_PIN < 49)
    {
      pinMode(ESPCAN_EN_PIN, OUTPUT);
      digitalWrite(ESPCAN_EN_PIN, LOW);
    }

    // Configure TWAI general configuration
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)ESPCAN_TX_PIN, (gpio_num_t)ESPCAN_RX_PIN, TWAI_MODE_NORMAL);
    
    // Configure TWAI timing configuration for 500kbps
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    
    // Configure TWAI filter configuration (accept all)
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // Install TWAI driver
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
      log_i("TWAI driver installed");
    } else {
      log_e("Failed to install TWAI driver");
      _initialised = false;
      return false;
    }

    // Start TWAI driver
    if (twai_start() == ESP_OK) {
      log_i("TWAI driver started");
    } else {
      log_e("Failed to start TWAI driver");
      _initialised = false;
      return false;
    }

    log_i("ESP32-S3 CAN Bus (TWAI) initialised");
    _initialised = true;
    CanBusAvailable = true;
    _failedCanSendTotal = 0;   
    _lastChangeInterval = time(nullptr);
    return true;

  #else
    // Original ESP32 CAN initialization
    if (ESPCAN_EN_PIN > 0 && ESPCAN_EN_PIN < 35)
    {
      pinMode(ESPCAN_EN_PIN, OUTPUT);
      digitalWrite(ESPCAN_EN_PIN, 0);
    }

    CAN_cfg.speed = CAN_SPEED_500KBPS;
    CAN_cfg.tx_pin_id = (gpio_num_t) ESPCAN_TX_PIN;
    CAN_cfg.rx_pin_id = (gpio_num_t) ESPCAN_RX_PIN;
    CAN_cfg.rx_queue = xQueueCreate(rx_queue_size, sizeof(CAN_frame_t));
    // Init CAN Module
    ESP32Can.CANInit();
    log_i("CAN Bus initialised");

    _initialised = true;
    CanBusAvailable = true;
    _failedCanSendTotal = 0;   
    _lastChangeInterval = time(nullptr);
    return true;
  #endif

  #else
  if(_pref.isKey(ccCANBusEnabled))
    _canbusEnabled = _pref.getBool(ccCANBusEnabled,true);
  else
    _canbusEnabled = _pref.putBool(ccCANBusEnabled,true);

  if (!_canbusEnabled) return false;

  if (CAN != NULL)
    delete(CAN);
  
  CAN = new MCP_CAN(_CS_PIN);
  
  // Initialize MCP2515 running at 8MHz with a baudrate of 500kb/s and the masks and filters disabled.
  if ((!_CAN16Mhz) && (CAN->begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) == CAN_OK)) 
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
    delay(10);
    // Change to normal mode to allow messages to be transmitted
    CAN->setMode(MCP_NORMAL);  
    if(_CAN16Mhz)
      log_i("CAN Bus initialised at 16Mhz using Forced method");
    else
      log_i("CAN Bus initialised at 16Mhz using Auto");
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

  #endif
}
bool CANBUS::StartRunTask(bool Run)
{
 
  if (tHandle != nullptr && !Run) {
    log_i("Stopping CAN Bus Task");
    vTaskDelete(tHandle);
    tHandle = nullptr;
    return true;
  }

  if (tHandle != nullptr && Run) {
    eTaskState state = eTaskGetState(tHandle);
    switch (state) {
    case eRunning:
        log_i("Task is running.");
        break;
    case eReady:
        log_i("Task is ready to run.");
        StartRunTask();
        break;
    case eBlocked:
        log_i("Task is blocked (e.g. waiting on a delay or semaphore).");
        break;
    case eSuspended:
        Serial.println("Task is suspended.");
        break;
    case eDeleted:
        log_i("Task has been deleted.");
        StartRunTask();
        break;
    default:
        log_i("Unknown task state.");
        StartRunTask();
        break;
    }
  } else
  {
    log_i("Starting CAN Bus Task");
    StartRunTask();
  }
  return true;
}

bool CANBUS::StartRunTask()
{
  if(CanBusAvailable && _canbusEnabled){
  // Create task and pin to Core
    xTaskCreatePinnedToCore(&canSendTask,"canSendTask",2048,this,6,&tHandle,0);    
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
  sndStat = SendToDriver(0x35E, 8, MSG_PYLON);
  if (sndStat != false){
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
  
  sndStat = SendToDriver(0x35C, 2, CAN_MSG);
  if (sndStat != false){
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

  sndStat = SendToDriver(0x356, 6, CAN_MSG);

  if (sndStat != false){
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

  sndStat = SendToDriver(0x355, 4, CAN_MSG);
  if (sndStat != false){
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

  sndStat = SendToDriver(0x351, 8, CAN_MSG);

  if (sndStat != false){
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

  sndStat = SendToDriver(0x359, 8, CAN_MSG);
  if (sndStat != false){
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