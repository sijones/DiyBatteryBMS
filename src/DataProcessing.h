

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
       // taskENTER_CRITICAL(&MainMutex);
        Inverter.BattVoltage((uint16_t) round(parsedValue.toInt() * 0.1));
        Lcd.Data.BattVolts.setValue(Inverter.BattVoltage());
       // taskEXIT_CRITICAL(&MainMutex);
      }
    }
    
    if (key.compareTo(String('I')) == 0)
    {
      log_i("Battery Current Update: %smA",parsedValue.c_str());
      if (dataValid) {
       // taskENTER_CRITICAL(&MainMutex);
        Inverter.BattCurrentmA((int32_t) (parsedValue.toInt() *0.01 ));
        Lcd.Data.BattAmps.setValue(Inverter.BattCurrentmA());
        
       // taskEXIT_CRITICAL(&MainMutex);
      }
    }

    if (key.compareTo(String("SOC")) == 0)
    {
      log_i("Battery SOC Update: %s%%",parsedValue.c_str());
      if (dataValid) {
      //  taskENTER_CRITICAL(&MainMutex);
        Inverter.BattSOC((uint8_t) round((parsedValue.toInt()*0.1)));
        Lcd.Data.BattSOC.setValue(Inverter.BattSOC());
      //  taskEXIT_CRITICAL(&MainMutex);
      }
    }
    
   /* if (key.compareTo(String('SOC')) == 0)
    {
      log_i("Battery Temp Update: %sC",parsedValue.c_str());
      BattTemp((uint16_t) (parsedValue.toInt()*0.1));
    } */
   // taskEXIT_CRITICAL(&MainMutex);

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
