#ifndef VEDIRECTEEPROM_H
#define VEDIRECTEEPROM_H

// Constants
// no key larger than 15 chars

#include <Preferences.h>
#define RW_MODE true
#define RO_MODE false

const char* const ccChargeVolt = "ChargeVolt";
const char* const ccDischargeVolt = "DischargeVolt";
const char* const ccChargeCurrent = "ChargeCurr";
const char* const ccDischargeCurrent = "DischargeCurr";

const char* const ccLowSOCLimit = "LowSOCLimit";
const char* const ccHighSOCLimit = "HighSOCLimit";

const char* const ccSlowSOCCharge1 = "SlowSOCC1";
const char* const ccSlowSOCCharge2 = "SlowSOCC2";
const char* const ccSlowSOCDivider1 = "SlowSOCD1";
const char* const ccSlowSOCDivider2 = "SlowSOCD2";

const char* const ccBattCapacity = "BattCapacity";
const char* const ccPylonTech = "PylonTech";

const char* const ccWifiSSID = "WifiSSID";
const char* const ccWifiPass = "WifiPass";
const char* const ccWifiHostName = "WifiHostName";

const char* const ccMQTTServerIP = "MQTTServerIP";
const char* const ccMQTTClientID = "MQTTClientID";
const char* const ccMQTTUser = "MQTTUser";
const char* const ccMQTTPass = "MQTTPass";
const char* const ccMQTTPort = "MQTTPort";
const char* const ccMQTTTopic = "MQTTTopic";
const char* const ccMQTTParam = "MQTTParam";

const char* const ccVictronRX = "VictronRX";
const char* const ccVictronTX = "VictronTX";
const char* const ccCanCSPin = "CAN_CS_PIN";

const char* const ccVELOOPTIME = "VE_LOOP_TIME";
const char* const ccCANBusEnabled  = "CANEnabled";
const char* const ccLcdEnabled = "lcdenabled";
const char* const ccNTPServer = "NTPServer";
const char* const PREF_NAME = "smartbms";


class mEEPROM {
  public:
    mEEPROM();
    void begin();
    void end();

    Preferences _preferences;
    bool isKey(String key);
    bool clear();
    String getString(String key, String default_value);
    String getString(const char* key, String default_value);
    bool putString(String key, String value);
    bool putString(const char* key, String value);
    int32_t getInt(String key, int default_value);
    bool putInt(String key, int32_t value);
    int32_t getInt(const char* key, int default_value);
    bool putInt(const char* key, int32_t value);
    uint32_t getUInt(String key, uint32_t default_value);
    bool putUInt(String key, uint32_t value);
    uint32_t getUInt(const char* key, uint32_t default_value);
    bool putUInt(const char* key, uint32_t value);
    bool getBool(String key, boolean default_value);
    bool putBool(String key, boolean value);
    bool getBool(const char* key, boolean default_value);
    bool putBool(const char* key, boolean value);

};


#endif






