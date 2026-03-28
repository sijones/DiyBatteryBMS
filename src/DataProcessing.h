

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
        int32_t power = parsedNum;
        if (isNeg) power = -power;
        Inverter.BattPower(power);
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    else if (strcmp(key, "TTG") == 0)
    {
      log_i("Time To Go Update: %s minutes", parsedValue);
      if (dataValid) {
        int32_t ttg = parsedNum;
        if (isNeg) ttg = -ttg;
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.TimeToGo(ttg);
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
