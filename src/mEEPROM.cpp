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

boolean mEEPROM::isKey(String key){
  bool exists;
  _preferences.begin(PREF_NAME);
  exists = _preferences.isKey(key.c_str());
  _preferences.end();
  return exists;
}

boolean mEEPROM::clear()
{
  bool _cleared;
  _preferences.begin(PREF_NAME);
  _cleared = _preferences.clear();
  _preferences.end();
  return _cleared;
}

int32_t mEEPROM::getInt(String key, int default_value = 0) {
  _preferences.begin(PREF_NAME);
  int32_t ret = _preferences.getInt(key.c_str(), default_value);
  //log_d("PrefGetInt; \'%s\' = \'%d\'", key.c_str(),  ret);
  _preferences.end();
  return ret;
}

boolean mEEPROM::putInt(String key, int32_t value) {
  _preferences.begin(PREF_NAME);
  _preferences.putInt(key.c_str(), value);
  //log_d("PrefPutInt; \'%s\' = \'%d\'", key.c_str(),  value);
  _preferences.end();
  return true;
}

int32_t mEEPROM::getInt(int key, int default_value = 0) {
  return mEEPROM::getInt(String(key), default_value);
}

boolean mEEPROM::putInt(int key, int32_t value = 0) {
  return mEEPROM::putInt(String(key), value);
}

uint32_t mEEPROM::getUInt(String key, uint32_t default_value = 0) {
  _preferences.begin(PREF_NAME);
  uint32_t ret = _preferences.getUInt(key.c_str(), default_value);
  //log_d("PrefGetInt; \'%s\' = \'%d\'", key.c_str(),  ret);
  _preferences.end();
  return ret;
}

boolean mEEPROM::putUInt(String key, uint32_t value) {
  _preferences.begin(PREF_NAME);
  _preferences.putUInt(key.c_str(), value);
  log_d("PrefPutInt; \'%s\' = \'%d\'", key.c_str(),  value);
  _preferences.end();
  return true;
}

uint32_t mEEPROM::getUInt(uint32_t key, uint32_t default_value = 0) {
  return mEEPROM::getUInt(String(key), default_value);
}

boolean mEEPROM::putUInt(uint32_t key, uint32_t value = 0) {
  return mEEPROM::putUInt(String(key), value);
}

String mEEPROM::getString(int key, String default_value = String("")){
  return getString(String(key), default_value);
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

boolean mEEPROM::putString(int key, String value){
  return putString(String(key), value);
}

boolean mEEPROM::putString(String key, String value) {
  _preferences.begin(PREF_NAME);
  _preferences.putString(key.c_str(), value);
  //log_d("PrefputStr: \'%s\' = \'%s\'", key.c_str(), value);
  _preferences.end();
  return true;
}

boolean mEEPROM::putString(const char* key, String value) {
  _preferences.begin(PREF_NAME);
  _preferences.putString(key, value);
  //log_d("PrefputStr: \'%s\' = \'%s\'", key, value);
  _preferences.end();
  return true;
}

// Boolean
boolean mEEPROM::getBool(String key, boolean default_value = false) {
  _preferences.begin(PREF_NAME);
  boolean ret = _preferences.getBool(key.c_str(), default_value);
  //log_d("PrefGetInt; \'%s\' = \'%d\'", key.c_str(),  ret);
  _preferences.end();
  return ret;
}

boolean mEEPROM::putBool(String key, boolean value) {
  _preferences.begin(PREF_NAME);
  _preferences.putBool(key.c_str(), value);
  //log_d("PrefPutInt; \'%s\' = \'%d\'", key.c_str(),  value);
  _preferences.end();
  return true;
}

boolean mEEPROM::getBool(int key, boolean default_value = 0) {
  return mEEPROM::getInt(String(key), default_value);
}

boolean mEEPROM::putBool(int key, boolean value = false) {
  return mEEPROM::putInt(String(key), value);
}
