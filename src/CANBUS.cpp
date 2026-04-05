#include "CANBUS.h"
#include "mEEPROM.h"
#include "WebLog.h"

// Implements Pylontech CAN BUS communication
// using either MCP2515 or ESP32 TWAI peripheral
// Sijones 2025

#ifdef ESPCAN
const int rx_queue_size = 10; // Receive Queue size (common for ESP32 and ESP32-S3)
#else

#endif

bool CANBUS::SendToDriver(uint32_t CMD,uint8_t Length,uint8_t *Data) {
  
#ifdef ESPCAN
  // TWAI implementation (unified for ESP32 and ESP32-S3)
  twai_message_t tx_msg;
  tx_msg.identifier = CMD;
  tx_msg.data_length_code = (Length > 8) ? 8 : Length;
  tx_msg.flags = TWAI_MSG_FLAG_NONE; // Standard frame, no RTR
  memcpy(tx_msg.data, Data, tx_msg.data_length_code);

  esp_err_t result = twai_transmit(&tx_msg, pdMS_TO_TICKS(50));
  if (result == ESP_OK) {
    return true;
  } else {
    if (result == ESP_ERR_TIMEOUT) {
      // Increase inter-frame delay to prevent future timeouts
      if (_canSendDelay < 50) {
        _canSendDelay += 5;
        WS_LOG_W("CAN TX timeout 0x%03X, inter-frame delay increased to %dms", CMD, _canSendDelay);
      }
    } else if (result == ESP_ERR_INVALID_STATE) {
      log_w("TWAI TX invalid state (bus-off?) for ID 0x%03X", CMD);
    }
    return false;
  }
#else
  byte sndStat = CAN->sendMsgBuf(CMD, 0, Length, Data);
  if (sndStat == CAN_OK)
    return true;
  // On TX buffer timeout, increase inter-frame delay to prevent future failures
  if ((sndStat == CAN_GETTXBFTIMEOUT || sndStat == CAN_SENDMSGTIMEOUT) && _canSendDelay < 50) {
    _canSendDelay += 5;
    WS_LOG_W("CAN TX timeout 0x%03X, inter-frame delay increased to %dms", CMD, _canSendDelay);
  }
  return false;
#endif
}

#ifndef ESPCAN
bool CANBUS::ReadMCP(unsigned long &id, uint8_t &len, uint8_t *buf) {
  if (CAN == NULL || !_initialised) return false;
  if (CAN->checkReceive() == CAN_MSGAVAIL) {
    CAN->readMsgBuf(&id, &len, buf);
    return true;
  }
  return false;
}
#endif

void canSendTask(void * pointer){
  
  CANBUS *Inverter = (CANBUS *) pointer;
  log_i("Starting CAN Bus send task");

  while (true)
  {
    // Check TWAI alerts and receive any incoming frames

#ifdef ESPCAN
  // Check for TWAI alerts
  uint32_t alerts_triggered;
  if (twai_read_alerts(&alerts_triggered, 0) == ESP_OK) {
    twai_status_info_t status_info;
    twai_get_status_info(&status_info);
    
    if (alerts_triggered & TWAI_ALERT_BUS_OFF) {
      log_e("Alert: Bus-Off state. CAN bus may be disconnected.");
      log_e("TX error counter: %lu, RX error counter: %lu", status_info.tx_error_counter, status_info.rx_error_counter);
    }
    if (alerts_triggered & TWAI_ALERT_BUS_ERROR) {
      log_w("Alert: Bus error detected. Bus error count: %lu", status_info.bus_error_count);
    }
    if (alerts_triggered & TWAI_ALERT_TX_FAILED) {
      log_w("Alert: TX failed. TX buffered: %lu, TX failed: %lu", status_info.msgs_to_tx, status_info.tx_failed_count);
    }
    if (alerts_triggered & TWAI_ALERT_ERR_PASS) {
      log_w("Alert: Error passive state. TX errors: %lu, RX errors: %lu", status_info.tx_error_counter, status_info.rx_error_counter);
    }
  }

  twai_message_t rx_msg;

  // Receive CAN frames (non-blocking), check for inverter 0x305 keepalive
  while (twai_receive(&rx_msg, 0) == ESP_OK)
  {
    if (rx_msg.identifier == 0x305) {
      Inverter->InverterSeen();
    } else {
      log_d("CAN RX: 0x%03X DLC=%d", rx_msg.identifier, rx_msg.data_length_code);
    }
  }
#else
  // MCP2515: check for incoming frames (non-blocking)
  {
    unsigned long rxId;
    uint8_t rxLen;
    uint8_t rxBuf[8];
    while (Inverter->ReadMCP(rxId, rxLen, rxBuf)) {
      if (rxId == 0x305) {
        Inverter->InverterSeen();
      }
    }
  }
#endif
        if(!Inverter->SendAllUpdates())
          log_e("Failure returned from SendAllUpdates");
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
  // TWAI initialization (unified for ESP32 and ESP32-S3)
  // Check if pins are configured via web interface
  if (ESPCAN_TX_PIN == 0 || ESPCAN_RX_PIN == 0) {
    log_e("CAN TX/RX pins not configured. Please configure via web interface.");
    _initialised = false;
    return false;
  }

#ifdef ESPCAN_S3
  // Validate GPIO pins (ESP32-S3 supports GPIO 0-48)
  // Avoid GPIO 0 for TX/RX as it's used for boot mode selection
  if (ESPCAN_TX_PIN < 1 || ESPCAN_TX_PIN > 48 || ESPCAN_RX_PIN < 1 || ESPCAN_RX_PIN > 48) {
    log_e("Invalid CAN TX/RX pins. ESP32-S3 TX/RX should use GPIO 1-48 (avoid GPIO 0).");
    _initialised = false;
    return false;
  }

  // ESP32-S3 supports GPIO 1-48 for enable pin (GPIO 0 reserved for boot mode)
  if (ESPCAN_EN_PIN > 0 && ESPCAN_EN_PIN <= 48)
  {
    pinMode(ESPCAN_EN_PIN, OUTPUT);
    digitalWrite(ESPCAN_EN_PIN, 0);
  }
#else
  // Validate GPIO pins for ESP32 (supports GPIO 0-39)
  if (ESPCAN_TX_PIN > 39 || ESPCAN_RX_PIN > 39) {
    log_e("Invalid CAN TX/RX pins. ESP32 should use GPIO 0-39.");
    _initialised = false;
    return false;
  }

  if (ESPCAN_EN_PIN > 0 && ESPCAN_EN_PIN < 35)
  {
    pinMode(ESPCAN_EN_PIN, OUTPUT);
    digitalWrite(ESPCAN_EN_PIN, 0);
  }
#endif

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

  // Configure alerts to monitor TX status and bus errors
  uint32_t alerts_to_enable = TWAI_ALERT_TX_IDLE | TWAI_ALERT_TX_SUCCESS | 
                               TWAI_ALERT_TX_FAILED | TWAI_ALERT_ERR_PASS | 
                               TWAI_ALERT_BUS_ERROR | TWAI_ALERT_BUS_OFF;
  if (twai_reconfigure_alerts(alerts_to_enable, NULL) == ESP_OK) {
    log_i("TWAI alerts configured");
  } else {
    log_w("Failed to configure TWAI alerts");
  }

#ifdef ESPCAN_S3
  log_i("ESP32-S3 CAN Bus (TWAI) initialised");
#else
  log_i("ESP32 CAN Bus (TWAI) initialised");
#endif
  _initialised = true;
  CanBusAvailable = true;
  _failedCanSendTotal = 0;
  return true;

  #else
  // MCP2515 initialization
  // Check if CS pin is configured via web interface
  if (_CS_PIN == 0) {
    log_e("CAN CS pin not configured. Please configure via web interface.");
    _initialised = false;
    return false;
  }

  // Initialize preferences for NVS access
  _pref.begin(PREF_NAME);

  if(_pref.isKey(ccCANBusEnabled))
    _canbusEnabled = _pref.getBool(ccCANBusEnabled,true);
  else
    _canbusEnabled = _pref.putBool(ccCANBusEnabled,true);
  
  // Close preferences after reading/writing
  _pref.end();

  if (!_canbusEnabled) {
    log_i("CAN Bus disabled in settings");
    return false;
  }

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
    xTaskCreatePinnedToCore(&canSendTask,"canSendTask",4096,this,6,&tHandle,1);
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

    // Unit conversions: mV -> centivolts, mA -> deciamps
    uint16_t chargeVCentiV = (uint16_t)(_chargeVoltage * 0.1);
    uint16_t overVCentiV = (uint16_t)(_overVoltage * 0.1);
    uint16_t dischargeVCentiV = (uint16_t)(_dischargeVoltage * 0.1);
    int32_t tailDA = (int32_t)(_tailCurrentmA / 100);
    if (tailDA < 1) tailDA = 1;
    uint16_t rechargeOffCentiV = _rechargeVoltageOffset / 10;

    // If SOC=100 but voltage hasn't reached target, use 99 to keep charging
    uint8_t workingSOC = _battSOC;
    if (workingSOC >= 100 && _battVoltage < chargeVCentiV) {
      workingSOC = 99;
      if (!_socOverrideLogged) {
        _socOverrideLogged = true;
        WS_LOG_D("SOC at 100%% but voltage (%d) < charge voltage (%d), using SOC 99%%", _battVoltage, chargeVCentiV);
      }
    } else {
      _socOverrideLogged = false;
    }

    bool isCharging = (_battCurrentmA >= 1);
    bool _tempChargeEnabled = true;

    uint32_t baseChargeCurrent = _maxChargeCurrentmA;
    if (_battCapacity > 0 && _initialBattData) {
      if (_slowchargeSOC[1] > 0 && workingSOC >= _slowchargeSOC[1] && _slowchargeSOCdiv[1] > 0) {
        baseChargeCurrent = (_battCapacity / _slowchargeSOCdiv[1]);
      }
      else if (_slowchargeSOC[0] > 0 && workingSOC >= _slowchargeSOC[0] && _slowchargeSOCdiv[0] > 0) {
        baseChargeCurrent = (_battCapacity / _slowchargeSOCdiv[0]);
      }
    }

    ChargePhase prevPhase = _chargePhase;

    switch (_chargePhase) {

    case PHASE_BULK:
      _chargeAdjust = 0;
      if (baseChargeCurrent != GetChargeCurrent())
        SetChargeCurrent(baseChargeCurrent);

      if (isCharging && chargeVCentiV > 5 && _battVoltage >= (chargeVCentiV - 5)) {
        _chargePhase = PHASE_ABSORPTION;
        _absorptionStartTime = t;
        _tailCurrentSustained = false;
        _tailCurrentStartTime = 0;
        _chargeAdjust = 0;
        _lastAdjustTime = t;
        WS_LOG_I("CC-CV: BULK -> ABSORPTION (V=%d, target=%d)", _battVoltage, chargeVCentiV);
      }
      break;

    case PHASE_ABSORPTION:
    {
      if (_useAutoCharge && _adjustStep > 0
          && (t - _lastAdjustTime) >= SMARTINTERVAL) {
        _lastAdjustTime = t;
        if (_battVoltage >= chargeVCentiV) {
          if (_chargeAdjust + _adjustStep < baseChargeCurrent &&
              baseChargeCurrent - (_chargeAdjust + _adjustStep) >= _minChargeCurrent) {
            _chargeAdjust += _adjustStep;
            WS_LOG_D("Absorption: decrease current, adjust=%d", _chargeAdjust);
          }
        } else if (_battVoltage < (chargeVCentiV - 3) && _chargeAdjust > 0) {
          _chargeAdjust = (_chargeAdjust >= _adjustStep) ? (_chargeAdjust - _adjustStep) : 0;
          WS_LOG_D("Absorption: increase current, adjust=%d", _chargeAdjust);
        }
      }

      if (_chargeAdjust > baseChargeCurrent)
        _chargeAdjust = baseChargeCurrent;

      uint32_t absorptionCurrent = baseChargeCurrent - _chargeAdjust;
      if (absorptionCurrent < _minChargeCurrent)
        absorptionCurrent = _minChargeCurrent;

      if (absorptionCurrent != GetChargeCurrent())
        SetChargeCurrent(absorptionCurrent);

      if (rechargeOffCentiV > 0 && chargeVCentiV > rechargeOffCentiV
          && _battVoltage < (chargeVCentiV - rechargeOffCentiV)) {
        _chargePhase = PHASE_BULK;
        _tailCurrentSustained = false;
        _chargeAdjust = 0;
        WS_LOG_I("CC-CV: ABSORPTION -> BULK (voltage dropped: V=%d)", _battVoltage);
        break;
      }

      if (_battCurrentmA < tailDA
          && _battVoltage >= (chargeVCentiV - 10)) {
        if (!_tailCurrentSustained) {
          _tailCurrentSustained = true;
          _tailCurrentStartTime = t;
          WS_LOG_D("Tail current detected: I=%d DA (threshold=%d DA), timer started", _battCurrentmA, tailDA);
        } else if ((t - _tailCurrentStartTime) >= _tailCurrentDuration) {
          _chargePhase = PHASE_COMPLETE;
          WS_LOG_I("CC-CV: ABSORPTION -> COMPLETE (tail current sustained %ds)", _tailCurrentDuration);
          break;
        }
      } else {
        if (_tailCurrentSustained) {
          WS_LOG_D("Tail current lost: I=%d DA, timer reset", _battCurrentmA);
        }
        _tailCurrentSustained = false;
        _tailCurrentStartTime = 0;
      }

      if (_maxAbsorptionTime > 0 && _absorptionStartTime > 0
          && (t - _absorptionStartTime) >= ((time_t)_maxAbsorptionTime * 60)) {
        _chargePhase = PHASE_COMPLETE;
        WS_LOG_I("CC-CV: ABSORPTION -> COMPLETE (max absorption time %d min)", _maxAbsorptionTime);
      }
      break;
    }

    case PHASE_COMPLETE:
      _tempChargeEnabled = false;
      SetChargeCurrent(0);

      if (_rechargeSOC > 0 && workingSOC < _rechargeSOC) {
        _chargePhase = PHASE_BULK;
        WS_LOG_I("CC-CV: COMPLETE -> BULK (SOC %d < recharge %d)", workingSOC, _rechargeSOC);
      }
      else if (rechargeOffCentiV > 0 && chargeVCentiV > rechargeOffCentiV
               && _battVoltage < (chargeVCentiV - rechargeOffCentiV)) {
        _chargePhase = PHASE_BULK;
        WS_LOG_I("CC-CV: COMPLETE -> BULK (voltage %d < %d)", _battVoltage, chargeVCentiV - rechargeOffCentiV);
      }
      break;
    }

    if (_chargePhase != prevPhase) {
      _dataChanged = true;
    }

    bool shouldStopDischarge = false;
    if (_lowSOCLimit > 0 && _battSOC <= _lowSOCLimit)
      shouldStopDischarge = true;
    if (_dischargeVoltage > 0 && _battVoltage < dischargeVCentiV)
      shouldStopDischarge = true;
    if (_tempProtectionEnabled) {
      if (_battTemp >= _dischargeHighTemp || _battTemp <= _dischargeLowTemp)
        shouldStopDischarge = true;
    }

    if (shouldStopDischarge && _dischargeEnabled) {
      DischargeEnable(false);
      WS_LOG_W("Discharge disabled (SOC=%d, V=%d, T=%d, limit=%d, Vlimit=%d)",
               _battSOC, _battVoltage, _battTemp, _lowSOCLimit, dischargeVCentiV);
    }
    else if (!shouldStopDischarge && !_dischargeEnabled) {
      DischargeEnable(true);
      WS_LOG_I("Discharge re-enabled (SOC=%d, V=%d, T=%d)", _battSOC, _battVoltage, _battTemp);
    }

    if (_highSOCLimit < 100 && workingSOC >= _highSOCLimit)
      _tempChargeEnabled = false;
    if (_overVoltage > _dischargeVoltage && _battVoltage >= overVCentiV)
      _tempChargeEnabled = false;
    if (_tempProtectionEnabled) {
      if (_battTemp >= _chargeHighTemp || _battTemp <= _chargeLowTemp)
        _tempChargeEnabled = false;
    }

    if (_tempChargeEnabled != _chargeEnabled)
      ChargeEnable(_tempChargeEnabled);

    return SendCANData();
  } 
  else 
  {
    WS_LOG_E("CAN Bus Data not initialised or configured.");
    return false;
  }
    
}

const char* CANBUS::GetChargePhaseName() {
  switch (_chargePhase) {
    case PHASE_BULK:       return "Bulk";
    case PHASE_ABSORPTION: return "Absorption";
    case PHASE_COMPLETE:   return "Complete";
    default:               return "Unknown";
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
  switch (_canProtocol) {
    case PROTO_SMA:          return SendCANData_SMA();
    case PROTO_VICTRON:      return SendCANData_Victron();
    case PROTO_GROWATT:      // Growatt uses Pylontech 1.3 protocol
    case PROTO_PYLONTECH_12:
    case PROTO_PYLONTECH_13:
    default:                 return SendCANData_Pylontech();
  }
}

// Helper lambda-like pattern used by all protocol methods
#define CAN_SEND_BEGIN() \
  if (!Initialised() || !Configured()) return false; \
  _failedCanSendCount = 0;

#define CAN_SEND_MSG(id, len, data) do { \
  if (!SendToDriver(id, len, data)) { \
    _failedCanSendCount++; \
    _failedCanSendTotal++; \
    WS_LOG_W("CAN TX fail: 0x%03X", id); \
  } \
  vTaskDelay(_canSendDelay / portTICK_PERIOD_MS); \
} while(0)

#define CAN_SEND_END() \
  if (_failedCanSendCount > 0) \
    WS_LOG_E("Failed to Send CAN Packets: %i", _failedCanSendCount); \
  CanBusDataOK = !CanBusFailed(); \
  return (_failedCanSendCount == 0);

// Shared helper: build SOC bytes into CAN_MSG[0-1]
// Handles SOC trick, 100% override, and never100SOC logic
void inline CANBUS_BuildSOC(uint8_t* msg, uint8_t battSOC, bool enableSOCTrick, bool forceCharge,
                            CANBUS::ChargePhase chargePhase, bool never100SOC) {
  if (enableSOCTrick && forceCharge) {
    msg[0] = lowByte(uint8_t(battSOC * 0.1));
    msg[1] = highByte(uint8_t(battSOC * 0.1));
  }
  else if (battSOC >= 100 && (chargePhase != CANBUS::PHASE_COMPLETE || never100SOC)) {
    msg[0] = lowByte(99);
    msg[1] = highByte(99);
  }
  else {
    msg[0] = lowByte(battSOC);
    msg[1] = highByte(battSOC);
  }
}

bool CANBUS::SendCANData_Pylontech(){
  CAN_SEND_BEGIN();

  // Pylontech units: voltage in 0.01V (centivolts), current in 0.1A (deciamps)
  uint16_t _tempChargeVolt = (_chargeVoltage * 0.01);
  uint16_t _tempDisCharVolt = (_dischargeVoltage * 0.01);
  uint16_t _tempChargeCurr = (_chargeCurrentmA * 0.01);
  uint16_t _tempDisChargeCurr = (_dischargeCurrentmA * 0.01);
  u_int16_t _tempMaxDisChargeCurr = (_maxDischargeCurrentmA * 0.01);
  int16_t _tempBattTemp = (_battTemp * 10);

  // 0x351 - Battery charge and discharge parameters
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CAN_MSG[0] = lowByte(_tempChargeVolt);
  CAN_MSG[1] = highByte(_tempChargeVolt);
  if((_chargeEnabled && _ManualAllowCharge)){
    CAN_MSG[2] = lowByte(_tempChargeCurr);
    CAN_MSG[3] = highByte(_tempChargeCurr);
  } else {
    CAN_MSG[2] = 0;
    CAN_MSG[3] = 0;
  }
  if((_dischargeEnabled && _ManualAllowDischarge)){
    if(_dischargeCurrentmA > _maxDischargeCurrentmA)
      _tempDisChargeCurr = _tempMaxDisChargeCurr;
    CAN_MSG[4] = lowByte(_tempDisChargeCurr);
    CAN_MSG[5] = highByte(_tempDisChargeCurr);
  } else {
    CAN_MSG[4] = 0;
    CAN_MSG[5] = 0;
  }
  CAN_MSG[6] = lowByte(_tempDisCharVolt);
  CAN_MSG[7] = highByte(_tempDisCharVolt);
  CAN_SEND_MSG(0x351, 8, CAN_MSG);

  // 0x355 - SOC / SOH (4 bytes v1.2, 6 bytes v1.3)
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CANBUS_BuildSOC(CAN_MSG, _battSOC, _enableSOCTrick, _forceCharge, _chargePhase, _never100SOC);
  CAN_MSG[2] = lowByte(_battSOH);
  CAN_MSG[3] = highByte(_battSOH);
  CAN_SEND_MSG(0x355, (_canProtocol == PROTO_PYLONTECH_13) ? 6 : 4, CAN_MSG);

  // 0x356 - Battery voltage, current, temperature
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CAN_MSG[0] = lowByte(_battVoltage);
  CAN_MSG[1] = highByte(_battVoltage);
  CAN_MSG[2] = lowByte(int16_t(_battCurrentmA));
  CAN_MSG[3] = highByte(int16_t(_battCurrentmA));
  CAN_MSG[4] = lowByte(_tempBattTemp);
  CAN_MSG[5] = highByte(_tempBattTemp);
  CAN_SEND_MSG(0x356, 6, CAN_MSG);

  if (_canProtocol == PROTO_PYLONTECH_12 || _canProtocol == PROTO_GROWATT) {
    // v1.2: 0x359 - Protection & alarm flags
    memset(CAN_MSG,0x00,sizeof(CAN_MSG));
    CAN_MSG[4] = 0x0A;
    CAN_MSG[5] = 0x50;
    CAN_MSG[6] = 0x4E;
    CAN_SEND_MSG(0x359, 8, CAN_MSG);

    // v1.2: 0x35C - Battery charge request flags
    memset(CAN_MSG,0x00,sizeof(CAN_MSG));
    CAN_MSG[0] = 0xC0;
    CAN_MSG[1] = 0x00;
    if(_enableRequestFlags) {
      CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagRequestFullCharge,_forceCharge);
      CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagChargeEnable,(_chargeEnabled && _ManualAllowCharge) ? true : false);
      CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagDischargeEnable,(_dischargeEnabled && _ManualAllowDischarge) ? true : false);
    }
    CAN_SEND_MSG(0x35C, 2, CAN_MSG);
  }
  else if (_canProtocol == PROTO_PYLONTECH_13) {
    // v1.3: 0x35A - Alarms & Warnings (bit-pair alarm/clear format)
    memset(CAN_MSG,0x00,sizeof(CAN_MSG));
    // Byte 0: voltage alarms - set clear bits (odd bits) when no alarm
    // Bit 1: general alarm clears, Bit 3: high voltage clears,
    // Bit 5: low voltage clears, Bit 7: high temp clears
    CAN_MSG[0] = 0xAA;  // All clear bits set (no alarms)
    // Byte 1: temperature & current alarms
    // Bit 1: low temp clears, Bit 3: high temp charge clears,
    // Bit 5: low temp charge clears, Bit 7: high current clears
    CAN_MSG[1] = 0xAA;  // All clear bits set
    // Byte 2: charge current & fault alarms
    // Bit 1: high charge current clears, Bit 3: contactor fault clears,
    // Bit 5: short circuit clears, Bit 7: BMS internal fault clears
    CAN_MSG[2] = 0xAA;  // All clear bits set
    // Byte 3: reserved
    CAN_MSG[3] = 0x00;

    // Set alarm bits based on current state
    if (_alarmActive) {
      CAN_MSG[0] = (CAN_MSG[0] & ~0x02) | 0x01;  // General alarm set, clear bit removed
    }
    if (_tempProtectionEnabled) {
      if (_battTemp >= _chargeHighTemp || _battTemp >= _dischargeHighTemp)
        CAN_MSG[0] = (CAN_MSG[0] & ~0x80) | 0x40;  // High temp alarm
      if (_battTemp <= _chargeLowTemp || _battTemp <= _dischargeLowTemp)
        CAN_MSG[1] = (CAN_MSG[1] & ~0x02) | 0x01;  // Low temp alarm
    }
    if (_battVoltage > 0 && _overVoltage > 0 && _battVoltage >= (_overVoltage * 0.1))
      CAN_MSG[0] = (CAN_MSG[0] & ~0x08) | 0x04;  // High voltage alarm
    if (_battVoltage > 0 && _dischargeVoltage > 0 && _battVoltage <= (_dischargeVoltage * 0.1))
      CAN_MSG[0] = (CAN_MSG[0] & ~0x20) | 0x10;  // Low voltage alarm

    CAN_SEND_MSG(0x35A, 8, CAN_MSG);

    // v1.3: 0x35F - Battery type & BMS info
    memset(CAN_MSG,0x00,sizeof(CAN_MSG));
    CAN_MSG[0] = 0x4C;  // 'L'
    CAN_MSG[1] = 0x69;  // 'i' (Lithium)
    CAN_MSG[2] = 0x01;  // BMS version major
    CAN_MSG[3] = 0x03;  // BMS version minor (1.3)
    // Bytes 4-5: Battery capacity in Ah * 10
    uint16_t capAh10 = (_battCapacity > 0) ? (uint16_t)((_battCapacity / 1000) * 10) : 0;
    CAN_MSG[4] = lowByte(capAh10);
    CAN_MSG[5] = highByte(capAh10);
    CAN_MSG[6] = 0x00;  // Manufacturer ID
    CAN_MSG[7] = 0x00;
    CAN_SEND_MSG(0x35F, 8, CAN_MSG);
  }

  // 0x35E - Manufacturer name
  CAN_SEND_MSG(0x35E, 8, MSG_PYLON);

  CAN_SEND_END();
}

bool CANBUS::SendCANData_SMA(){
  CAN_SEND_BEGIN();

  // SMA units: voltage in 0.1V (decivolts), current in 0.1A (deciamps)
  uint16_t _tempChargeVolt = (_chargeVoltage * 0.1);
  uint16_t _tempDisCharVolt = (_dischargeVoltage * 0.1);
  uint16_t _tempChargeCurr = (_chargeCurrentmA * 0.01);
  uint16_t _tempDisChargeCurr = (_dischargeCurrentmA * 0.01);
  u_int16_t _tempMaxDisChargeCurr = (_maxDischargeCurrentmA * 0.01);
  int16_t _tempBattTemp = (_battTemp * 10);

  // 0x351 - Battery charge and discharge parameters
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CAN_MSG[0] = lowByte(_tempChargeVolt);
  CAN_MSG[1] = highByte(_tempChargeVolt);
  if((_chargeEnabled && _ManualAllowCharge)){
    CAN_MSG[2] = lowByte(_tempChargeCurr);
    CAN_MSG[3] = highByte(_tempChargeCurr);
  } else {
    CAN_MSG[2] = 0;
    CAN_MSG[3] = 0;
  }
  if((_dischargeEnabled && _ManualAllowDischarge)){
    if(_dischargeCurrentmA > _maxDischargeCurrentmA)
      _tempDisChargeCurr = _tempMaxDisChargeCurr;
    CAN_MSG[4] = lowByte(_tempDisChargeCurr);
    CAN_MSG[5] = highByte(_tempDisChargeCurr);
  } else {
    CAN_MSG[4] = 0;
    CAN_MSG[5] = 0;
  }
  CAN_MSG[6] = lowByte(_tempDisCharVolt);
  CAN_MSG[7] = highByte(_tempDisCharVolt);
  CAN_SEND_MSG(0x351, 8, CAN_MSG);

  // 0x355 - SOC / SOH
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CANBUS_BuildSOC(CAN_MSG, _battSOC, _enableSOCTrick, _forceCharge, _chargePhase, _never100SOC);
  CAN_MSG[2] = lowByte(_battSOH);
  CAN_MSG[3] = highByte(_battSOH);
  CAN_SEND_MSG(0x355, 4, CAN_MSG);

  // 0x356 - Battery voltage, current, temperature
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CAN_MSG[0] = lowByte(_battVoltage);
  CAN_MSG[1] = highByte(_battVoltage);
  CAN_MSG[2] = lowByte(int16_t(_battCurrentmA));
  CAN_MSG[3] = highByte(int16_t(_battCurrentmA));
  CAN_MSG[4] = lowByte(_tempBattTemp);
  CAN_MSG[5] = highByte(_tempBattTemp);
  CAN_SEND_MSG(0x356, 6, CAN_MSG);

  // SMA: no 0x359, 0x35C, or 0x35E messages

  CAN_SEND_END();
}

bool CANBUS::SendCANData_Victron(){
  CAN_SEND_BEGIN();

  // Victron units: same as Pylontech (voltage 0.01V, current 0.1A)
  uint16_t _tempChargeVolt = (_chargeVoltage * 0.01);
  uint16_t _tempDisCharVolt = (_dischargeVoltage * 0.01);
  uint16_t _tempChargeCurr = (_chargeCurrentmA * 0.01);
  uint16_t _tempDisChargeCurr = (_dischargeCurrentmA * 0.01);
  u_int16_t _tempMaxDisChargeCurr = (_maxDischargeCurrentmA * 0.01);
  int16_t _tempBattTemp = (_battTemp * 10);

  // 0x351 - Battery charge and discharge parameters
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CAN_MSG[0] = lowByte(_tempChargeVolt);
  CAN_MSG[1] = highByte(_tempChargeVolt);
  if((_chargeEnabled && _ManualAllowCharge)){
    CAN_MSG[2] = lowByte(_tempChargeCurr);
    CAN_MSG[3] = highByte(_tempChargeCurr);
  } else {
    CAN_MSG[2] = 0;
    CAN_MSG[3] = 0;
  }
  if((_dischargeEnabled && _ManualAllowDischarge)){
    if(_dischargeCurrentmA > _maxDischargeCurrentmA)
      _tempDisChargeCurr = _tempMaxDisChargeCurr;
    CAN_MSG[4] = lowByte(_tempDisChargeCurr);
    CAN_MSG[5] = highByte(_tempDisChargeCurr);
  } else {
    CAN_MSG[4] = 0;
    CAN_MSG[5] = 0;
  }
  CAN_MSG[6] = lowByte(_tempDisCharVolt);
  CAN_MSG[7] = highByte(_tempDisCharVolt);
  CAN_SEND_MSG(0x351, 8, CAN_MSG);

  // 0x355 - SOC / SOH
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CANBUS_BuildSOC(CAN_MSG, _battSOC, _enableSOCTrick, _forceCharge, _chargePhase, _never100SOC);
  CAN_MSG[2] = lowByte(_battSOH);
  CAN_MSG[3] = highByte(_battSOH);
  CAN_SEND_MSG(0x355, 4, CAN_MSG);

  // 0x356 - Battery voltage, current, temperature
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  CAN_MSG[0] = lowByte(_battVoltage);
  CAN_MSG[1] = highByte(_battVoltage);
  CAN_MSG[2] = lowByte(int16_t(_battCurrentmA));
  CAN_MSG[3] = highByte(int16_t(_battCurrentmA));
  CAN_MSG[4] = lowByte(_tempBattTemp);
  CAN_MSG[5] = highByte(_tempBattTemp);
  CAN_SEND_MSG(0x356, 6, CAN_MSG);

  // 0x35A - Alarm details (Victron specific)
  memset(CAN_MSG,0x00,sizeof(CAN_MSG));
  // Byte 0: general alarm/warning flags
  // Bit mapping: 0=no alarm, 1=warning, 2=alarm
  // For now, derive from existing alarm state
  if (_alarmActive) {
    CAN_MSG[0] = 0x04;  // General alarm bit
  }
  // Bytes 1-7: reserved / additional alarm detail
  CAN_SEND_MSG(0x35A, 8, CAN_MSG);

  // 0x35E - Manufacturer name (Victron uses "DIYBMS  ")
  CAN_SEND_MSG(0x35E, 8, MSG_VICTRON);

  CAN_SEND_END();
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