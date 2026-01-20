

void VEDataProcess()
{

 for (int i = 0; i < veHandle.FrameLength(); i++) {
    bool dataValid = false;
    String key = veHandle.veName[i];
    String value = veHandle.veValue[i];
    String parsedValue = "";
    //log_i("Handling Key: %s - Value: %s",key,value);
    
    if (value.startsWith("-"))
      parsedValue = "-";

    for (auto x : value)
    {
      if (isDigit(x))
        parsedValue += x;
    }
    if (parsedValue.length() > 0)
      dataValid = true;

    if (key.compareTo(String('V')) == 0)
    {
      log_i("Battery Voltage Update: %sV", parsedValue.c_str());
      if (dataValid) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.BattVoltage((uint16_t) round(parsedValue.toInt() * 0.1));
        Lcd.Data.BattVolts.setValue(Inverter.BattVoltage());
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }
    
    if (key.compareTo(String('I')) == 0)
    {
      log_i("Battery Current Update: %smA",parsedValue.c_str());
      if (dataValid) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.BattCurrentmA((int32_t) (parsedValue.toInt() *0.01 ));
        Lcd.Data.BattAmps.setValue(Inverter.BattCurrentmA());
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    if (key.compareTo(String("SOC")) == 0)
    {
      log_i("Battery SOC Update: %s%%",parsedValue.c_str());
      if (dataValid) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.BattSOC((uint8_t) round((parsedValue.toInt()*0.1)));
        Lcd.Data.BattSOC.setValue(Inverter.BattSOC());
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    if (key.compareTo(String('P')) == 0)
    {
      log_i("Battery Power Update: %sW",parsedValue.c_str());
      if (dataValid) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        // Power is in watts (1W increments, can be negative for discharge)
        int32_t power = parsedValue.toInt();
        if (value.startsWith("-")) power = -power;
        Inverter.BattPower(power);
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    if (key.compareTo(String("TTG")) == 0)
    {
      log_i("Time To Go Update: %s minutes",parsedValue.c_str());
      if (dataValid) {
        int32_t ttg = parsedValue.toInt();
        if (value.startsWith("-")) ttg = -ttg;
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        // TTG is in minutes (already in correct units)
        Inverter.TimeToGo(ttg);
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    if (key.compareTo(String('T')) == 0)
    {
      log_i("Battery Temperature Update: %s°C",parsedValue.c_str());
      if (dataValid) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        // Temperature is in 0.1°C increments
        Inverter.BattTemp((int16_t) round(parsedValue.toInt() * 0.1));
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
      }
    }

    if (key.compareTo(String("Alarm")) == 0)
    {
      bool alarmState = value.compareTo(String("ON")) == 0;
      log_i("Alarm State Update: %s", alarmState ? "ON" : "OFF");
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.AlarmActive(alarmState);
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    if (key.compareTo(String("AR")) == 0)
    {
      log_i("Alarm Reason Update: %s",value.c_str());
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.AlarmReason(value);
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    if (key.compareTo(String("PID")) == 0)
    {
      log_i("Product ID Update: %s",value.c_str());
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.PIDString(value);
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    if (key.compareTo(String("FW")) == 0)
    {
      log_i("Firmware Version Update: %s",value.c_str());
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.FWVersion(value);
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    if (key.compareTo(String("SER#")) == 0)
    {
      log_i("Serial Number Update: %s",value.c_str());
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.SerialNumber(value);
      taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }

    if (key.compareTo(String("BMV")) == 0)
    {
      log_i("Model Update: BMV-%s",value.c_str());
      taskENTER_CRITICAL(&(Inverter.CANMutex));
      Inverter.ModelString("BMV-" + value);
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
