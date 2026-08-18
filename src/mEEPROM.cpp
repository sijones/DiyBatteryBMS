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
  if (!_preferences.begin(nvsspace)) {
    log_e("Failed to Open EEPROM in RW Mode for %c.",nvsspace);
  }

}

void mEEPROM::end() {
  _preferences.end();
}

bool mEEPROM::isKey(String key){
  bool exists;
  exists = _preferences.isKey(key.c_str());
  return exists;
}

bool mEEPROM::clear(bool all)
{
  // Preferences::clear() only empties the namespace the handle is open on, and
  // WiFi/MQTT settings live in their own "network" namespace (see
  // WifiMQTTManager), so clearing PREF_NAME is not a factory reset. Erase the
  // whole NVS partition instead and put back only what the caller asked to keep.
  String _wifissid, _wifipass, _wifihost;

  if (!all) {
    Preferences net;
    if (!net.begin("network")) {
      log_e("Could not read WiFi settings before erase, aborting reset");
      return false;
    }
    _wifissid = net.getString(ccWifiSSID, String(""));
    _wifipass = net.getString(ccWifiPass, String(""));
    _wifihost = net.getString(ccWifiHostName, String(""));
    net.end();
  }

  // Our own handle has to be closed before the partition goes out from under it.
  end();

  esp_err_t err = nvs_flash_erase();
  if (err != ESP_OK) {
    log_e("NVS erase failed (%d), please do a manual flash erase!", err);
    return false;
  }
  err = nvs_flash_init();
  if (err != ESP_OK) {
    log_e("NVS re-init after erase failed (%d)", err);
    return false;
  }

  if (all) {
    log_d("NVS fully erased");
    return true;
  }

  bool restored = true;
  {
    Preferences net;
    if (!net.begin("network")) {
      log_e("NVS erased but the WiFi settings could not be written back");
      return false;
    }
    if (_wifissid.length() && !net.putString(ccWifiSSID, _wifissid.c_str())) restored = false;
    if (_wifipass.length() && !net.putString(ccWifiPass, _wifipass.c_str())) restored = false;
    if (_wifihost.length() && !net.putString(ccWifiHostName, _wifihost.c_str())) restored = false;
    net.end();
  }

  if (restored) log_d("NVS erased (WiFi kept)");
  else          log_e("NVS erased but some WiFi settings failed to save");

  return restored;
}

int32_t mEEPROM::getInt32(const char* key, int32_t default_value) {
  int32_t ret = _preferences.getInt(key, default_value);
  return ret;
}

bool mEEPROM::putInt32(const char* key, int32_t value) {
  size_t result = _preferences.putInt(key, value);
  if (result == 0) {
    log_e("Failed to write Int32 key: %s", key);
    return false;
  }
  return true;
}

uint32_t mEEPROM::getUInt32(const char* key, uint32_t default_value) {
  uint32_t ret = _preferences.getUInt(key, default_value);
  return ret;
}

bool mEEPROM::putUInt32(const char* key, uint32_t value) {
  size_t result = _preferences.putUInt(key, value);
  if (result == 0) {
    log_e("Failed to write UInt32 key: %s", key);
    return false;
  }
  return true;
}

// 16

int16_t mEEPROM::getInt16(const char* key, int16_t default_value = 0) {
  int16_t ret = _preferences.getShort(key, default_value);
  return ret;
}

bool mEEPROM::putInt16(const char* key, int16_t value) {
  size_t result = _preferences.putShort(key, value);
  if (result == 0) {
    log_e("Failed to write Int16 key: %s", key);
    return false;
  }
  return true;
}

uint16_t mEEPROM::getUInt16(const char* key, uint16_t default_value = 0) {
  uint16_t ret = _preferences.getUShort(key, default_value);
  return ret;
}

bool mEEPROM::putUInt16(const char* key, uint16_t value) {
  size_t result = _preferences.putUShort(key, value);
  if (result == 0) {
    log_e("Failed to write UInt16 key: %s", key);
    return false;
  }
  return true;
}

// End of 16

// 8

int8_t mEEPROM::getInt8(const char* key, int8_t default_value = 0) {
  int8_t ret = _preferences.getChar(key, default_value);
  return ret;
}

bool mEEPROM::putInt8(const char* key, int8_t value) {
  size_t result = _preferences.putChar(key, value);
  if (result == 0) {
    log_e("Failed to write Int8 key: %s", key);
    return false;
  }
  return true;
}

uint8_t mEEPROM::getUInt8(const char* key, uint8_t default_value = 0) {
  uint8_t ret = _preferences.getUChar(key, default_value);
  return ret;
}

bool mEEPROM::putUInt8(const char* key, uint8_t value) {
  size_t result = _preferences.putUChar(key, value);
  if (result == 0) {
    log_e("Failed to write UInt8 key: %s", key);
    return false;
  }
  return true;
}

// END of 8

String mEEPROM::getString(String key, String default_value = String("")) {
  return getString(key.c_str(), default_value);
}

/* Length-checked read with no opinion about the bytes. For the few settings that
   are legitimately not text - see the note on getStringRaw in mEEPROM.h. */
String mEEPROM::getStringRaw(const char* key, String default_value) {
  /* Ask how long the value is before reading it. Preferences::getString puts the
     value in "char buf[len]" - a stack array sized by whatever length the entry
     header claims - so an entry with a corrupt length field overflows the task
     stack inside the library, before any check of ours could run. NVS itself
     cannot store a string longer than 4000 bytes including the terminator, so
     anything above that is corruption by definition.
     len == 0 means missing key, wrong type, or a read error; getStringLength has
     already logged which, and the answer in every case is the caller's default. */
  const size_t len = _preferences.getStringLength(key);
  if (len == 0) return default_value;
  if (len > NVS_STRING_MAX) {
    log_e("NVS key '%s' claims %u bytes, longer than NVS can hold - treating as corrupt",
          key, (unsigned)len);
    return default_value;
  }

  String ret = _preferences.getString(key, default_value);
  if (!isValidUTF8(ret))
    log_w("NVS key '%s' is not valid UTF-8 - keeping it, this key is allowed raw bytes", key);
  return ret;
}

String mEEPROM::getString(const char* key, String default_value = String("")) {
  String ret = getStringRaw(key, default_value);
  /* Every string leaving NVS is checked once, here, rather than at each call
     site - an entry left half-written by the 2.8 -> 3.0 upgrade reads back as
     arbitrary bytes, and those bytes otherwise flow straight into the MQTT
     CONNECT, the web UI's JSON and the syslog line. Fall back to the caller's
     default, which is either the compiled-in value or "", so a corrupt entry
     costs a setting rather than the whole service. */
  if (!isValidUTF8(ret)) {
    log_e("NVS key '%s' holds %u bytes that are not valid UTF-8, using the default instead",
          key, (unsigned)ret.length());
    return default_value;
  }
  return ret;
}

bool mEEPROM::putString(String key, String value) {
  size_t result = _preferences.putString(key.c_str(), value);
  if (result == 0) {
    log_e("Failed to write String key: %s", key.c_str());
    return false;
  }
  return true;
}

bool mEEPROM::putString(const char* key, String value) {
  size_t result = _preferences.putString(key, value);
  if (result == 0) {
    log_e("Failed to write String key: %s", key);
    return false;
  }
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
  size_t result = _preferences.putBool(key, value);
  if (result == 0) {
    log_e("Failed to write Bool key: %s", key);
    return false;
  }
  return true;
}

bool mEEPROM::putBool(String key, boolean value) {
  return putBool(key.c_str(),value);
}