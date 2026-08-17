

/* Apply the latest decoded BLE advertisement.

   Deliberately lands on the same setters VEDataProcess() uses, so everything
   downstream - charge phases, CAN, MQTT, the LCD - cannot tell which source it
   came from and needs no per-source handling.

   Watch the units. Inverter.BattVoltage() is centivolts and BattCurrentmA() is
   deciamps despite its name, which is what the VE.Direct path converts to
   above; BLE carries mV and mA, so both are scaled here rather than anywhere
   further downstream. */
void BLEDataProcess()
{
  taskENTER_CRITICAL(&(Inverter.CANMutex));

  Inverter.BattVoltage((uint16_t)(VictronBle.VoltagemV / 10));      // mV -> centivolts
  Inverter.BattCurrentmA((int32_t)(VictronBle.CurrentmA / 100));    // mA -> deciamps
  Inverter.BattSOC((uint8_t)(VictronBle.SOCPermille / 10));         // 0.1% -> %

  // The shunt sends power as a derived figure over serial but not over BLE, so
  // it is computed here from the two readings that did arrive.
  Inverter.BattPower((int32_t)(((int64_t)VictronBle.VoltagemV * (int64_t)VictronBle.CurrentmA) / 1000000LL));

  // Victron sends -1 for "not discharging" on the serial path; match that so
  // the same downstream check works for both sources.
  Inverter.TimeToGo(VictronBle.TimeToGoMins ? (int32_t)VictronBle.TimeToGoMins : -1);
  Inverter.AlarmActive(VictronBle.AlarmReason != 0);

  // Aux mode 2 means the aux input is a temperature sensor, sent in 0.01K
  if (VictronBle.AuxMode == 2 && Inverter.BattTempSource() == 0) {
    Inverter.BattTemp((int16_t) lround((VictronBle.AuxRaw / 100.0) - 273.15));
  }

  Lcd.Data.BattVolts.setValue(Inverter.BattVoltage());
  Lcd.Data.BattAmps.setValue(Inverter.BattCurrentmA());
  Lcd.Data.BattSOC.setValue(Inverter.BattSOC());

  taskEXIT_CRITICAL(&(Inverter.CANMutex));
}

void VEDataProcess()
{

 for (int i = 0; i < veHandle.FrameLength(); i++) {
    const char* key = veHandle.veName[i];
    const char* value = veHandle.veValue[i];

    // Parse numeric value: extract digits with optional leading negative sign
    char parsedValue[34] = {0};
    int pLen = 0;
    bool isNeg = (value[0] == '-');
    if (isNeg)
      parsedValue[pLen++] = '-';

    for (const char* p = value; *p; p++) {
      if (isDigit((unsigned char)*p) && pLen < (int)sizeof(parsedValue) - 1)
        parsedValue[pLen++] = *p;
    }
    parsedValue[pLen] = '\0';

    bool dataValid = (pLen > 0);
    long parsedNum = dataValid ? atol(parsedValue) : 0;

    if (strcmp(key, "V") == 0)
    {
      log_i("Battery Voltage Update: %sV", parsedValue);
      if (dataValid) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.BattVoltage((uint16_t) round(parsedNum * 0.1));
        Lcd.Data.BattVolts.setValue(Inverter.BattVoltage());
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    else if (strcmp(key, "I") == 0)
    {
      log_i("Battery Current Update: %smA", parsedValue);
      if (dataValid) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.BattCurrentmA((int32_t)(parsedNum * 0.01));
        Lcd.Data.BattAmps.setValue(Inverter.BattCurrentmA());
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    else if (strcmp(key, "SOC") == 0)
    {
      log_i("Battery SOC Update: %s%%", parsedValue);
      if (dataValid) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.BattSOC((uint8_t)(parsedNum / 10));
        Lcd.Data.BattSOC.setValue(Inverter.BattSOC());
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    else if (strcmp(key, "P") == 0)
    {
      log_i("Battery Power Update: %sW", parsedValue);
      if (dataValid) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        // parsedValue keeps the leading '-', so atol() has already applied the
        // sign - negating again here would flip discharge power positive.
        Inverter.BattPower((int32_t)parsedNum);
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    else if (strcmp(key, "TTG") == 0)
    {
      log_i("Time To Go Update: %s minutes", parsedValue);
      if (dataValid) {
        // Victron sends TTG -1 when not discharging. atol() has already applied
        // the sign; negating again turned that -1 into a bogus "1 minute to go".
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.TimeToGo((int32_t)parsedNum);
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    else if (strcmp(key, "T") == 0)
    {
      log_i("Battery Temperature Update: %s°C", parsedValue);
      if (dataValid && Inverter.BattTempSource() == 0) {  // Only accept VE.Direct if selected
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.BattTemp((int16_t) round(parsedNum * 0.1));
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    else if (strcmp(key, "Alarm") == 0)
    {
      bool alarmState = strcmp(value, "ON") == 0;
      log_i("Alarm State Update: %s", alarmState ? "ON" : "OFF");
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.AlarmActive(alarmState);
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    else if (strcmp(key, "AR") == 0)
    {
      log_i("Alarm Reason Update: %s", value);
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.AlarmReason(String(value));
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    else if (strcmp(key, "PID") == 0)
    {
      log_i("Product ID Update: %s", value);
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.PIDString(String(value));
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    else if (strcmp(key, "FW") == 0)
    {
      log_i("Firmware Version Update: %s", value);
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.FWVersion(String(value));
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    else if (strcmp(key, "SER#") == 0)
    {
      log_i("Serial Number Update: %s", value);
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.SerialNumber(String(value));
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    else if (strcmp(key, "BMV") == 0)
    {
      log_i("Model Update: BMV-%s", value);
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.ModelString(String("BMV-") + value);
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

  }

}

void CheckAndChangeLCD()
{
  bool _wifi = Lcd.Data.WifiConnected.getValue();
  bool _caninit = Lcd.Data.CANInit.getValue();
  bool _candata = Lcd.Data.CANBusData.getValue();
  bool _mqtt = Lcd.Data.MQTTConnected.getValue();
  bool _vedata = Lcd.Data.VEData.getValue();

  if(_wifi && _caninit && _candata && _mqtt && _vedata && Lcd.GetScreen() == Lcd.StartUp)
    Lcd.SetScreen(Lcd.Values);
  else if(Lcd.GetScreen() == Lcd.Values && (!_wifi || !_candata || !_mqtt || !_vedata) )
    Lcd.SetScreen(Lcd.StartUp);
  
}
