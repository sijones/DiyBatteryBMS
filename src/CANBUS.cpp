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

void CANBUS::SetCANSniffer(bool on) {
  if (_canSniffer == on) return;
  _canSniffer = on;
  _sniffCount = 0;   // a new capture starts with no history, so nothing is missed
  _dataChanged = true;
  if (on) {
    _snifferStart = millis();
    WS_LOG_W("CAN sniffer ON - listening only, NOTHING is being sent to the inverter (auto-off in %u min)",
             (uint32_t)(SNIFFER_TIMEOUT_MS / 60000UL));
  } else {
    WS_LOG_I("CAN sniffer off - sending to the inverter resumed");
  }
}

void CANBUS::SnifferCheckTimeout() {
  if (!_canSniffer) return;
  if ((uint32_t)(millis() - _snifferStart) < SNIFFER_TIMEOUT_MS) return;
  _canSniffer = false;
  _sniffCount = 0;
  _dataChanged = true;
  WS_LOG_W("CAN sniffer timed out after %u min - sending to the inverter resumed",
           (uint32_t)(SNIFFER_TIMEOUT_MS / 60000UL));
}

void CANBUS::SnifferLogFrame(uint32_t id, uint8_t len, const uint8_t* data) {
  if (len > 8) len = 8;
  uint32_t now = millis();

  int8_t slot = -1;
  for (uint8_t i = 0; i < _sniffCount; i++) {
    if (_sniffId[i] == (uint16_t) id) { slot = (int8_t) i; break; }
  }

  bool changed = true;
  if (slot >= 0) {
    changed = (_sniffLen[slot] != len) || (memcmp(_sniffData[slot], data, len) != 0);
    if (!changed && (uint32_t)(now - _sniffSeen[slot]) < SNIFF_REPEAT_MS) return;
  } else if (_sniffCount < SNIFF_SLOTS) {
    slot = (int8_t) _sniffCount++;
    _sniffId[slot] = (uint16_t) id;
  }

  if (slot >= 0) {
    _sniffLen[slot] = len;
    memcpy(_sniffData[slot], data, len);
    _sniffSeen[slot] = now;
  }

  char hex[3 * 8];   // "AA " per byte, last space overwritten by the terminator
  hex[0] = '\0';
  for (uint8_t i = 0; i < len; i++)
    snprintf(hex + (i * 3), sizeof(hex) - (i * 3), "%02X ", data[i]);
  if (len > 0) hex[(len * 3) - 1] = '\0';

  WS_LOG_I("CAN RX 0x%03X [%u] %s%s", (unsigned) id, len, hex, changed ? "" : " (still)");
}

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
    if (Inverter->CANSniffer())
      Inverter->SnifferLogFrame(rx_msg.identifier, rx_msg.data_length_code, rx_msg.data);
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
      if (Inverter->CANSniffer())
        Inverter->SnifferLogFrame(rxId, rxLen, rxBuf);
      if (rxId == 0x305) {
        Inverter->InverterSeen();
      }
    }
  }
#endif
        Inverter->SnifferCheckTimeout();
        if (Inverter->CANSniffer()) {
          /* Listen-only: our own frames share IDs with the BMS whose bus is
             being watched, so transmitting would corrupt the very capture we
             are taking. Poll far faster than the 1s send cadence too - the
             MCP2515 has only two receive buffers and would drop frames. */
          vTaskDelay(20 / portTICK_PERIOD_MS);
        } else {
          if(!Inverter->SendAllUpdates())
            log_e("Failure returned from SendAllUpdates");
          vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
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

  /* Deselect the MCP2515 before the driver object exists. Its constructor
     writes CS high and only then calls pinMode, which Arduino core 3.x rejects
     with "IO n is not set as GPIO" - the write is dropped, leaving CS undriven
     until pinMode lands a line later. Core 2.x allowed the write, so this only
     showed up on the move to 3.x. Doing it here in the right order costs two
     lines and keeps CS deselected from the start whatever the library does. */
  pinMode(_CS_PIN, OUTPUT);
  digitalWrite(_CS_PIN, HIGH);

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

/* Core 1 on the dual-core parts, keeping the CAN send task off core 0 where
   WiFi and lwIP run - see main.cpp's note on loopTask's priority, which is the
   other half of the same arrangement.

   The C3 has one core, and this is not a preference there but a crash. IDF's
   FreeRTOS asserts the core ID against the count it was built for:

     configASSERT( (xCoreID >= 0 && xCoreID < 1) || xCoreID == tskNO_AFFINITY )

   compiled into libfreertos.a with the 1 baked in (CONFIG_FREERTOS_UNICORE=y),
   and with assertions enabled at level 2, so it aborts rather than compiling
   away. Passing 1 there panicked the device the moment CAN started - which is
   why it went unseen: a C3 with its CAN pins still at 0 never reaches this,
   CanBusAvailable being false, so the board only died once someone configured
   it properly.

   tskNO_AFFINITY rather than 0: on a single core it means the same thing, and
   it says "no opinion" instead of asserting a placement this chip cannot
   express. */
#if CONFIG_FREERTOS_UNICORE
  #define CAN_SEND_TASK_CORE tskNO_AFFINITY
#else
  #define CAN_SEND_TASK_CORE 1
#endif

bool CANBUS::StartRunTask()
{
  if(CanBusAvailable && _canbusEnabled){
    xTaskCreatePinnedToCore(&canSendTask,"canSendTask",4096,this,6,&tHandle,
                            CAN_SEND_TASK_CORE);
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
    // Monotonic. Do NOT use time() here - see the timer note in CANBUS.h.
    uint32_t t = millis();

    // Unit conversions: mV -> centivolts, mA -> deciamps
    uint16_t chargeVCentiV = (uint16_t)(_chargeVoltage * 0.1);
    uint16_t floatVCentiV = (uint16_t)(ActiveFloatVoltage() * 0.1);
    uint16_t overVCentiV = (uint16_t)(_overVoltage * 0.1);
    uint16_t dischargeVCentiV = (uint16_t)(_dischargeVoltage * 0.1);
    int32_t tailDA = (int32_t)(_tailCurrentmA / 100);
    if (tailDA < 1) tailDA = 1;
    uint16_t rechargeOffCentiV = _rechargeVoltageOffset / 10;

    // BULK enters ABSORPTION at (target - 5 centivolts). If the recharge offset is
    // smaller than that, the ABSORPTION->BULK condition overlaps it and the phase
    // flips every cycle - 1 Hz log spam, constant MQTT churn, and the absorption
    // and tail timers never accumulate so charging can never complete. Enforce a
    // minimum so the two thresholds cannot cross, whatever is configured.
    if (rechargeOffCentiV > 0 && rechargeOffCentiV <= 5) rechargeOffCentiV = 6;

    // If SOC=100 but voltage hasn't reached target, use 99 to keep charging.
    // Not in float: the pack is held below the charge target there by design, so
    // this would read a deliberate float as an unfinished charge and never stop
    // pushing the current back up.
    uint8_t workingSOC = _battSOC;
    if (workingSOC >= 100 && _battVoltage < chargeVCentiV && _chargePhase != PHASE_FLOAT) {
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
      uint8_t slowDiv = 0;
      if (_slowchargeSOC[1] > 0 && workingSOC >= _slowchargeSOC[1] && _slowchargeSOCdiv[1] > 0)
        slowDiv = _slowchargeSOCdiv[1];
      else if (_slowchargeSOC[0] > 0 && workingSOC >= _slowchargeSOC[0] && _slowchargeSOCdiv[0] > 0)
        slowDiv = _slowchargeSOCdiv[0];

      if (slowDiv > 0) {
        // Capacity is mAh, so capacity/divider is the C-rate current in mA. Round
        // the division, then round again to a whole 0.1A - the CAN frames carry
        // deciamps and truncate, so C/24 of a 280Ah pack (11666 mA) would otherwise
        // leave as 11.6A when 11.7A is the nearer answer.
        uint32_t taperedmA = (_battCapacity + (slowDiv / 2)) / slowDiv;
        taperedmA = ((taperedmA + 50) / 100) * 100;
        // A taper only ever slows charging down. Capacity/divider knows nothing of
        // the configured ceiling, so a big pack on a small divider lands above it -
        // C/2 of 280Ah is 140A. That must not raise the limit past what is set.
        if (taperedmA < baseChargeCurrent) baseChargeCurrent = taperedmA;
      }
    }

    /* Retire a stale request once, so the transition is logged and published
       rather than repeating every pass. A controller that dies must not leave
       the pack pinned at whatever it last asked for. */
    if (_reqChargeSet && !ChargeRequestActive()) {
      _reqChargeSet = false;
      _dataChanged = true;
      WS_LOG_W("Charge current request expired after %us, back to configured %u mA",
               _reqTimeoutSecs, _maxChargeCurrentmA);
    }
    if (_reqDischargeSet && !DischargeRequestActive()) {
      _reqDischargeSet = false;
      _dataChanged = true;
      WS_LOG_W("Discharge current request expired after %us, back to configured %u mA",
               _reqTimeoutSecs, _maxDischargeCurrentmA);
    }

    /* A live request caps whatever the phase logic works out. Applied after the
       slow-charge taper so the lowest of the three - configured max, taper,
       request - is what goes out; the other order would let a taper computed from
       capacity hand back more current than the controller asked for. */
    if (ChargeRequestActive() && _reqChargeCurrentmA < baseChargeCurrent)
      baseChargeCurrent = _reqChargeCurrentmA;

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
          && (uint32_t)(t - _lastAdjustTime) >= ((uint32_t)SMARTINTERVAL * 1000UL)) {
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
      // Min Charge is a floor under the configured ceiling, not a licence to
      // exceed it. Without this a live request of 0 would still charge at the
      // minimum, and 0 has to mean 0.
      if (absorptionCurrent > baseChargeCurrent)
        absorptionCurrent = baseChargeCurrent;

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

      // Both conditions share their definition with the UI via TailVoltageOK()
      if (_battCurrentmA < tailDA && TailVoltageOK()) {
        if (!_tailCurrentSustained) {
          _tailCurrentSustained = true;
          _tailCurrentStartTime = t;
          WS_LOG_D("Tail current detected: I=%d DA (threshold=%d DA), timer started", _battCurrentmA, tailDA);
        } else if ((uint32_t)(t - _tailCurrentStartTime) >= ((uint32_t)_tailCurrentDuration * 1000UL)) {
          _chargePhase = FloatEnabled() ? PHASE_FLOAT : PHASE_COMPLETE;
          WS_LOG_I("CC-CV: ABSORPTION -> %s (tail current sustained %ds)",
                   GetChargePhaseName(), _tailCurrentDuration);
          ClearFullChargeRequest();
          break;
        }
      } else {
        if (_tailCurrentSustained) {
          WS_LOG_D("Tail current lost: I=%d DA, timer reset", _battCurrentmA);
        }
        _tailCurrentSustained = false;
        _tailCurrentStartTime = 0;
      }

      // _absorptionStartTime is always set on entry to ABSORPTION and the phase
      // resets to BULK on boot, so no "is it set" guard is needed with millis().
      if (_maxAbsorptionTime > 0
          && (uint32_t)(t - _absorptionStartTime) >= ((uint32_t)_maxAbsorptionTime * 60000UL)) {
        _chargePhase = FloatEnabled() ? PHASE_FLOAT : PHASE_COMPLETE;
        WS_LOG_I("CC-CV: ABSORPTION -> %s (max absorption time %d min)",
                 GetChargePhaseName(), _maxAbsorptionTime);
        ClearFullChargeRequest();
      }
      break;
    }

    case PHASE_FLOAT:
    {
      // Charging stays enabled here. The pack is full, so the inverter is given a
      // lower voltage target and a small current allowance rather than a flat
      // refusal: an inverter told "hold 55.2V, but 0A" has no way to shed the
      // surplus and some resolve it by discharging the battery instead.
      if (!FloatEnabled()) {
        // Float turned off, or its voltage raised above the charge target, while
        // the pack was sitting in it.
        _chargePhase = PHASE_COMPLETE;
        WS_LOG_I("CC-CV: FLOAT -> COMPLETE (float no longer configured)");
        break;
      }

      // Min Charge is the smallest current the inverter can actually act on, so it
      // is a floor here as much as in absorption - a float allowance under it is a
      // number the inverter cannot honour, and rounds back to the 0A this phase
      // exists to avoid. The configured ceiling still wins over both, so a max of
      // 0 or a controller asking for 0 still means 0.
      uint32_t floatCurrent = _floatCurrentmA;
      if (floatCurrent < _minChargeCurrent) floatCurrent = _minChargeCurrent;
      if (floatCurrent > baseChargeCurrent) floatCurrent = baseChargeCurrent;
      if (floatCurrent != GetChargeCurrent())
        SetChargeCurrent(floatCurrent);

      if (_rechargeSOC > 0 && _battSOC < _rechargeSOC) {
        _chargePhase = PHASE_BULK;
        WS_LOG_I("CC-CV: FLOAT -> BULK (SOC %d < recharge %d)", _battSOC, _rechargeSOC);
      }
      // Measured against the float target, not the charge target. The pack sits
      // below the charge target throughout float by design, so comparing against
      // that would restart bulk on the first cycle in the phase.
      else if (rechargeOffCentiV > 0 && floatVCentiV > rechargeOffCentiV
               && _battVoltage < (floatVCentiV - rechargeOffCentiV)) {
        _chargePhase = PHASE_BULK;
        WS_LOG_I("CC-CV: FLOAT -> BULK (voltage %d < %d)",
                 _battVoltage, floatVCentiV - rechargeOffCentiV);
      }
      break;
    }

    case PHASE_COMPLETE:
      // Configuring a float voltage takes effect at once rather than waiting for
      // the next full cycle. That matters because the reason to reach for it is
      // usually an inverter misbehaving right now, with the pack sitting here.
      if (FloatEnabled()) {
        _chargePhase = PHASE_FLOAT;
        WS_LOG_I("CC-CV: COMPLETE -> FLOAT (float target %u mV%s)",
                 ActiveFloatVoltage(), FloatUsingAutoVoltage() ? ", automatic" : "");
        break;
      }

      _tempChargeEnabled = false;
      SetChargeCurrent(0);

      // Use the real pack SOC, not workingSOC. In COMPLETE the charge current is
      // zero so voltage sags below target, which forces workingSOC to 99 - with
      // rechargeSOC set to 100 that made "99 < 100" true forever and the pack
      // cycled BULK->ABSORPTION->COMPLETE without ever resting.
      if (_rechargeSOC > 0 && _battSOC < _rechargeSOC) {
        _chargePhase = PHASE_BULK;
        WS_LOG_I("CC-CV: COMPLETE -> BULK (SOC %d < recharge %d)", _battSOC, _rechargeSOC);
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
    case PHASE_FLOAT:      return "Float";
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

void CANBUS::RequestFullCharge(bool State) {
  if (State != _requestFullCharge) _dataChanged = true;
  _requestFullCharge = State;
  }

void CANBUS::ClearFullChargeRequest() {
  if (!_requestFullCharge) return;
  _requestFullCharge = false;
  _dataChanged = true;
  WS_LOG_I("Full charge request cleared - charge complete");
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
// Handles SOC trick, 100% override, and never100SOC logic.
// outReported/outReason record what actually went on the wire and why, so the web UI
// can surface the fact that the inverter is being told something other than the truth.
void inline CANBUS_BuildSOC(uint8_t* msg, uint8_t battSOC, bool enableSOCTrick, bool forceCharge,
                            CANBUS::ChargePhase chargePhase, bool never100SOC,
                            uint8_t* outReported = nullptr, uint8_t* outReason = nullptr) {
  uint8_t sent;
  uint8_t reason;

  // Float means the charge finished, same as Complete. Holding 99% through float
  // would tell the inverter the pack still needs charging while the limits say it
  // may not - a contradiction some inverters answer by discharging.
  bool chargeDone = (chargePhase == CANBUS::PHASE_COMPLETE || chargePhase == CANBUS::PHASE_FLOAT);

  if (enableSOCTrick && forceCharge) {
    sent = uint8_t(battSOC * 0.1);
    reason = CANBUS::SOC_OVR_TRICK;
  }
  else if (battSOC >= 100 && (!chargeDone || never100SOC)) {
    sent = 99;
    // Both conditions can hold at once; the charge-phase hold is the more
    // informative of the two, so report that in preference.
    reason = !chargeDone ? CANBUS::SOC_OVR_HOLD99 : CANBUS::SOC_OVR_NEVER100;
  }
  else {
    sent = battSOC;
    reason = CANBUS::SOC_OVR_NONE;
  }

  msg[0] = lowByte(sent);
  msg[1] = highByte(sent);
  if (outReported) *outReported = sent;
  if (outReason)   *outReason = reason;
}

bool CANBUS::SendCANData_Pylontech(){
  CAN_SEND_BEGIN();

  // Pylontech units: voltage in 0.01V (centivolts), current in 0.1A (deciamps)
  uint16_t _tempChargeVolt = (ActiveChargeVoltage() * 0.01);
  uint16_t _tempDisCharVolt = (_dischargeVoltage * 0.01);
  uint16_t _tempChargeCurr = (_chargeCurrentmA * 0.01);
  // Already min(live, configured max, controller request) - see CANBUS.h
  uint16_t _tempDisChargeCurr = (EffectiveDischargeCurrent() * 0.01);
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
  CANBUS_BuildSOC(CAN_MSG, _battSOC, _enableSOCTrick, _forceCharge, _chargePhase, _never100SOC,
                  &_reportedSOC, &_socOverride);
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
    // mAh -> 0.1Ah directly. Going via whole Ah first threw away the tenths, so a
    // 280.5Ah pack reported itself as 280.0.
    uint16_t capAh10 = (_battCapacity > 0) ? (uint16_t)((_battCapacity + 50) / 100) : 0;
    CAN_MSG[4] = lowByte(capAh10);
    CAN_MSG[5] = highByte(capAh10);
    CAN_MSG[6] = 0x00;  // Manufacturer ID
    CAN_MSG[7] = 0x00;
    CAN_SEND_MSG(0x35F, 8, CAN_MSG);
  }

  /* 0x35C - Battery charge request flags. v1.2 and Growatt always send the
     message, with the flag bits filled in only when Request Flags is on. The
     v1.3 spec replaces it with 0x35A/0x35F, but inverters that read the flags
     still act on 0x35C when it arrives, and force charge has no other carrier -
     so send it on v1.3 too when Request Flags is enabled. Left off there by
     default, since the spec does not ask for it. */
  if (_canProtocol != PROTO_PYLONTECH_13 || _enableRequestFlags) {
    memset(CAN_MSG,0x00,sizeof(CAN_MSG));
    CAN_MSG[0] = 0xC0;
    CAN_MSG[1] = 0x00;
    if(_enableRequestFlags) {
      // Force charge is bit 4, not bit 3 - see the flag comments in CANBUS.h.
      // This previously sent bit 3, which asks an inverter to finish a charge it
      // is already doing rather than to start one, so force charge did nothing
      // on inverters that read the flags (EG4 6000XP among them).
      CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagForceCharge,_forceCharge);
      CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagRequestFullCharge,_requestFullCharge);
      CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagChargeEnable,(_chargeEnabled && _ManualAllowCharge) ? true : false);
      CAN_MSG[0] = bit_set_to(CAN_MSG[0],flagDischargeEnable,(_dischargeEnabled && _ManualAllowDischarge) ? true : false);
    }
    CAN_SEND_MSG(0x35C, 2, CAN_MSG);
  }

  // 0x35E - Manufacturer name
  CAN_SEND_MSG(0x35E, 8, MSG_PYLON);

  CAN_SEND_END();
}

bool CANBUS::SendCANData_SMA(){
  CAN_SEND_BEGIN();

  // SMA units: voltage in 0.1V (decivolts), current in 0.1A (deciamps)
  uint16_t _tempChargeVolt = (ActiveChargeVoltage() * 0.1);
  uint16_t _tempDisCharVolt = (_dischargeVoltage * 0.1);
  uint16_t _tempChargeCurr = (_chargeCurrentmA * 0.01);
  // Already min(live, configured max, controller request) - see CANBUS.h
  uint16_t _tempDisChargeCurr = (EffectiveDischargeCurrent() * 0.01);
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
  CANBUS_BuildSOC(CAN_MSG, _battSOC, _enableSOCTrick, _forceCharge, _chargePhase, _never100SOC,
                  &_reportedSOC, &_socOverride);
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
  uint16_t _tempChargeVolt = (ActiveChargeVoltage() * 0.01);
  uint16_t _tempDisCharVolt = (_dischargeVoltage * 0.01);
  uint16_t _tempChargeCurr = (_chargeCurrentmA * 0.01);
  // Already min(live, configured max, controller request) - see CANBUS.h
  uint16_t _tempDisChargeCurr = (EffectiveDischargeCurrent() * 0.01);
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
  CANBUS_BuildSOC(CAN_MSG, _battSOC, _enableSOCTrick, _forceCharge, _chargePhase, _never100SOC,
                  &_reportedSOC, &_socOverride);
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