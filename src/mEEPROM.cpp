#include "mEEPROM.h"

mEEPROM::mEEPROM() {
  // Open Preferences with my-app namespace. Each application module, library, etc
  // has to use a namespace name to prevent key name collisions. We will open storage in
  // RW-mode (second parameter has to be false).
  // Note: Namespace name is limited to 15 chars.

}

uint16_t mEEPROM::freeentries()
{
  return _preferences.freeEntries();
}

void mEEPROM::begin() {
  if (!_preferences.begin(PREF_NAME))
    log_e("Failed to Open EEPROM in RW Mode for settings retrival.");
}

void mEEPROM::begin(const char * nvsspace) {
  if (!_preferences.begin(nvsspace))
    log_e("Failed to Open EEPROM in RW Mode for %c.",nvsspace);
}

void mEEPROM::end() {
  _preferences.end();
}

bool mEEPROM::isKey(String key){
  bool exists;
  exists = _preferences.isKey(key.c_str());
  return exists;
}

bool mEEPROM::clear(bool all = false)
{
  bool _cleared;
  if (all) {
    _cleared = _preferences.clear();
    log_d("NVS Full Flash Cleared");
    end();
  } else
  {
    String _wifissid = getString(ccWifiSSID, String(""));
    String _wifipass = getString(ccWifiPass, String(""));
    if(_preferences.clear()) {
      log_d("NVS Flash Cleared (Wifi Kept)");
      if(_preferences.begin(PREF_NAME)) {
        log_d("NVS Flash Initialised");
        if(putString(ccWifiSSID, _wifissid)) log_d("Wifi SSID Saved to NVS");
        if(putString(ccWifiSSID, _wifissid)) log_d("Wifi Password Saved to NVS");
        end();
      }
    } else
      log_d("NVS Flash failed to clear, please do a manual flash erase!");

  }

  return _cleared;
}

int32_t mEEPROM::getInt32(const char* key, int default_value = 0) {
  int32_t ret = _preferences.getInt(key, default_value);
  return ret;
}

bool mEEPROM::putInt32(const char* key, int32_t value) {
  _preferences.putInt(key, value);
  return true;
}

uint32_t mEEPROM::getUInt32(const char* key, uint32_t default_value = 0) {
  uint32_t ret = _preferences.getUInt(key, default_value);
  return ret;
}

bool mEEPROM::putUInt32(const char* key, uint32_t value) {
  _preferences.putUInt(key, value);
  return true;
}

// 16

int16_t mEEPROM::getInt16(const char* key, int16_t default_value = 0) {
  int16_t ret = _preferences.getShort(key, default_value);
  return ret;
}

bool mEEPROM::putInt16(const char* key, int16_t value) {
  _preferences.putShort(key, value);
  return true;
}

uint16_t mEEPROM::getUInt16(const char* key, uint16_t default_value = 0) {
  uint16_t ret = _preferences.getUShort(key, default_value);
  return ret;
}

bool mEEPROM::putUInt16(const char* key, uint16_t value) {
  _preferences.putUShort(key, value);
  return true;
}

// End of 16

// 8

int8_t mEEPROM::getInt8(const char* key, int8_t default_value = 0) {
  int8_t ret = _preferences.getChar(key, default_value);
  return ret;
}

bool mEEPROM::putInt8(const char* key, int8_t value) {
  _preferences.putChar(key, value);
  return true;
}

uint8_t mEEPROM::getUInt8(const char* key, uint8_t default_value = 0) {
  uint8_t ret = _preferences.getUChar(key, default_value);
  return ret;
}

bool mEEPROM::putUInt8(const char* key, uint8_t value) {
  _preferences.putUChar(key, value);
  return true;
}

// END of 8

String mEEPROM::getString(String key, String default_value = String("")) {
  String ret = _preferences.getString(key.c_str(), default_value);
  return ret;
}

String mEEPROM::getString(const char* key, String default_value = String("")) {
  String ret = _preferences.getString(key, default_value);
  return ret;
}

bool mEEPROM::putString(String key, String value) {
  _preferences.putString(key.c_str(), value);
  return true;
}

bool mEEPROM::putString(const char* key, String value) {
  _preferences.putString(key, value);
  return true;
}

// Boolean
bool mEEPROM::getBool(const char* key, boolean default_value = false) {
  bool ret = _preferences.getBool(key, default_value);
  return ret;
}

bool mEEPROM::getBool(String key, boolean default_value = false) {
  return getBool(key.c_str(),default_value);
}

bool mEEPROM::putBool(const char* key, boolean value) {
  _preferences.putBool(key, value);
  return true;
}

bool mEEPROM::putBool(String key, boolean value) {
  return putBool(key.c_str(),value);
}