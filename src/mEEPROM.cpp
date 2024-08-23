#include "mEEPROM.h"

mEEPROM::mEEPROM() {
  // Open Preferences with my-app namespace. Each application module, library, etc
  // has to use a namespace name to prevent key name collisions. We will open storage in
  // RW-mode (second parameter has to be false).
  // Note: Namespace name is limited to 15 chars.

}

void mEEPROM::begin() {
  if (!_preferences.begin(PREF_NAME))
    log_e("Failed to Open EEPROM in RW Mode for settings retrival.");
  _preferences.end();
}

void mEEPROM::end() {
  
}

bool mEEPROM::isKey(String key){
  bool exists;
  _preferences.begin(PREF_NAME);
  exists = _preferences.isKey(key.c_str());
  _preferences.end();
  return exists;
}

bool mEEPROM::clear()
{
  bool _cleared;
  _preferences.begin(PREF_NAME);
  _cleared = _preferences.clear();
  _preferences.end();
  return _cleared;
}

int32_t mEEPROM::getInt(const char* key, int default_value = 0) {
  _preferences.begin(PREF_NAME);
  int32_t ret = _preferences.getInt(key, default_value);
  _preferences.end();
  return ret;
}

int32_t mEEPROM::getInt(String key, int default_value = 0) {
  return getInt(key.c_str(),default_value);
}

bool mEEPROM::putInt(const char* key, int32_t value) {
  _preferences.begin(PREF_NAME);
  _preferences.putInt(key, value);
  _preferences.end();
  return true;
}

bool mEEPROM::putInt(String key, int32_t value) {
  return putInt(key.c_str(),value);
}

uint32_t mEEPROM::getUInt(const char* key, uint32_t default_value = 0) {
  _preferences.begin(PREF_NAME);
  uint32_t ret = _preferences.getUInt(key, default_value);
  _preferences.end();
  return ret;
}

uint32_t mEEPROM::getUInt(String key, uint32_t default_value = 0) {
  return getUInt(key.c_str(),default_value);
}

bool mEEPROM::putUInt(const char* key, uint32_t value) {
  _preferences.begin(PREF_NAME);
  _preferences.putUInt(key, value);
  _preferences.end();
  return true;
}

bool mEEPROM::putUInt(String key, uint32_t value) {
  return putUInt(key.c_str(),value);
}

String mEEPROM::getString(String key, String default_value = String("")) {
  _preferences.begin(PREF_NAME);
  String ret = _preferences.getString(key.c_str(), default_value.c_str());
  //log_d("PrefGetStr: \'%s\' = \'%s\'", key.c_str(),  ret.c_str());
  _preferences.end();
  return ret;
}

String mEEPROM::getString(const char* key, String default_value = String("")) {
  _preferences.begin(PREF_NAME);
  String ret = _preferences.getString(key, default_value.c_str());
  //log_d("PrefGetStr: \'%s\' = \'%s\'", key,  ret.c_str());
  _preferences.end();
  return ret;
}

bool mEEPROM::putString(String key, String value) {
  _preferences.begin(PREF_NAME);
  _preferences.putString(key.c_str(), value);
  //log_d("PrefputStr: \'%s\' = \'%s\'", key.c_str(), value);
  _preferences.end();
  return true;
}

bool mEEPROM::putString(const char* key, String value) {
  _preferences.begin(PREF_NAME);
  _preferences.putString(key, value);
  _preferences.end();
  return true;
}

// Boolean
bool mEEPROM::getBool(const char* key, boolean default_value = false) {
  _preferences.begin(PREF_NAME);
  boolean ret = _preferences.getBool(key, default_value);
  _preferences.end();
  return ret;
}

bool mEEPROM::getBool(String key, boolean default_value = false) {
  return getBool(key.c_str(),default_value);
}

bool mEEPROM::putBool(const char* key, boolean value) {
  _preferences.begin(PREF_NAME);
  _preferences.putBool(key, value);
  _preferences.end();
  return true;
}

bool mEEPROM::putBool(String key, boolean value) {
  return putBool(key.c_str(),value);
}