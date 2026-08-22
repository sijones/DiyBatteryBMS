

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
  Inverter.BattSOCPermille(VictronBle.SOCPermille);                 // kept at 0.1%

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

/* Apply the figures stashed by the MQTT shunt topics.

   Same contract as BLEDataProcess(): it lands on the setters VEDataProcess()
   uses, so nothing downstream can tell which source a reading came from. The
   values were already converted to CANBUS units when they arrived - see
   MqttShunt.h for why the conversion happens at the edge - so there is no
   scaling to do here beyond power.

   Only called when MqttShunt.DataFresh() is true, which means voltage, current
   and SOC have each arrived at least once and recently. The three unguarded
   writes below rely on that.

   Temperature is separate because it is optional: HaveTemp says whether that
   topic has ever produced anything, and BattTempSource() == 0 is the same rule
   BLEDataProcess() applies to its aux reading - a source-specific temperature
   never overwrites one the user has explicitly pointed at MQTT or a sensor.

   TimeToGo and AlarmActive are deliberately left alone. There is no MQTT field
   for either, and writing a made-up value would be worse than leaving the last
   known one; a shunt read this way simply does not report them. */
void MQTTShuntDataProcess()
{
  taskENTER_CRITICAL(&(Inverter.CANMutex));

  Inverter.BattVoltage(MqttShunt.VoltageCentiV);
  Inverter.BattCurrentmA(MqttShunt.CurrentDeciA);
  Inverter.BattSOCPermille(MqttShunt.SOCPermille);

  /* Power is derived, as it is for BLE - no topic carries it. Units:
     centivolts x deciamps = (V*100) * (A*10) = (V*A) * 1000, so dividing by
     1000 gives whole watts. 52.00V at 10.0A -> 5200 * 100 / 1000 = 520W. */
  Inverter.BattPower((int32_t)(((int64_t)MqttShunt.VoltageCentiV * (int64_t)MqttShunt.CurrentDeciA) / 1000LL));

  if (MqttShunt.HaveTemp && Inverter.BattTempSource() == 0)
    Inverter.BattTemp(MqttShunt.TempC);

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
        // VE.Direct sends SOC in 0.1%, so it is stored that way and the
        // whole-percent figure the CAN frame needs is derived from it.
        Inverter.BattSOCPermille((uint16_t)parsedNum);
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

    /* The five identity/alarm strings below are assigned OUTSIDE the critical
       section, unlike the numeric fields above.

       taskENTER_CRITICAL disables interrupts and takes a spinlock. Assigning an
       Arduino String allocates, and the heap has a lock of its own - so if
       another core is holding the heap lock at that moment, this core spins for
       it forever with interrupts off. That is a hard deadlock: no output, no
       watchdog (its interrupt cannot be delivered either), no reboot. Exactly
       what was seen on two boards that went silent on serial and off the
       network at the same time, with no panic to show for it.

       Same rule that mqttsetup() had to learn: nothing that allocates or blocks
       may happen inside a critical section. These are descriptive fields shown
       in the UI, so a torn read is worth far less than a hang is worth
       avoiding - the numeric values the charge logic depends on keep their
       guard above. */
    else if (strcmp(key, "AR") == 0)
    {
      log_i("Alarm Reason Update: %s", value);
      Inverter.AlarmReason(String(value));
    }

    else if (strcmp(key, "PID") == 0)
    {
      log_i("Product ID Update: %s", value);
      Inverter.PIDString(String(value));
    }

    else if (strcmp(key, "FW") == 0)
    {
      log_i("Firmware Version Update: %s", value);
      Inverter.FWVersion(String(value));
    }

    else if (strcmp(key, "SER#") == 0)
    {
      log_i("Serial Number Update: %s", value);
      Inverter.SerialNumber(String(value));
    }

    else if (strcmp(key, "BMV") == 0)
    {
      log_i("Model Update: BMV-%s", value);
      Inverter.ModelString(String("BMV-") + value);
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
