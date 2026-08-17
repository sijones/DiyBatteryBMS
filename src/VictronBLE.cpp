#include "VictronBLE.h"

VictronBLE VictronBle;

class VictronScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    VictronBle.ParseAdvert(dev);
  }
};

static VictronScanCallbacks _victronScanCallbacks;

bool VictronBLE::Begin(bool enabled)
{
  _enabled = enabled;
  if (!_enabled) return false;

  NimBLEDevice::init("");
  /* Listening only, so the radio never transmits: a passive scan does not send
     scan requests, which keeps us off the air entirely and avoids waking the
     shunt for data it is already broadcasting. */
  NimBLEDevice::setPower(ESP_PWR_LVL_N12);
  _scan = NimBLEDevice::getScan();
  _scan->setScanCallbacks(&_victronScanCallbacks, /*wantDuplicates=*/true);
  _scan->setActiveScan(false);
  /* Window equal to interval is a continuous listen. The shunt advertises about
     once a second and we cannot ask it to repeat, so any gap in the window is
     a reading lost. */
  _scan->setInterval(100);
  _scan->setWindow(100);
  _scan->setMaxResults(0);   // callback only, do not accumulate a result set

  if (!_scan->start(0, false)) {
    WS_LOG_E("Victron BLE: scan failed to start");
    return false;
  }
  WS_LOG_I("Victron BLE listening%s", Configured() ? "" : " (no device configured yet)");
  return true;
}

void VictronBLE::Stop()
{
  if (_scan) _scan->stop();
  _enabled = false;
}

void VictronBLE::SetKeyHex(const String& hex)
{
  String h = hex;
  h.trim();
  h.replace(" ", "");
  if (h.length() != 32) {
    _haveKey = false;
    if (h.length() > 0) WS_LOG_W("Victron BLE: key must be 32 hex characters, got %u", h.length());
    return;
  }
  for (uint8_t i = 0; i < 16; i++) {
    _key[i] = (uint8_t) strtoul(h.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  }
  _haveKey = true;
}

void VictronBLE::SetMac(const String& mac)
{
  String m = mac;
  m.trim();
  m.replace("-", ":");
  if (m.length() != 17) {
    _haveMac = false;
    return;
  }
  for (uint8_t i = 0; i < 6; i++) {
    _mac[i] = (uint8_t) strtoul(m.substring(i * 3, i * 3 + 2).c_str(), nullptr, 16);
  }
  _haveMac = true;
}

String VictronBLE::GetMac()
{
  if (!_haveMac) return String("");
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
           _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
  return String(buf);
}

bool VictronBLE::StartDiscovery(uint16_t seconds)
{
  if (!_enabled || !_scan) {
    WS_LOG_W("Victron BLE: cannot scan, BLE is not enabled");
    return false;
  }
  _foundCount = 0;
  _discovering = true;
  WS_LOG_I("Victron BLE: scanning %us for nearby Victron devices", seconds);
  return true;   // the running scan feeds NoteFound; main.cpp ends the window
}

bool VictronBLE::Decrypt(const uint8_t* cipher, size_t len, uint16_t nonce, uint8_t* out)
{
  if (len == 0 || len > 32) return false;

  /* AES-CTR with a 128-bit counter whose low 16 bits are the advertisement's
     data counter, little endian, rest zero. The record is one AES block, so the
     counter never increments and mbedtls counting big-endian past the block
     does not arise. */
  uint8_t nonceCounter[16] = {};
  nonceCounter[0] = (uint8_t)(nonce & 0xFF);
  nonceCounter[1] = (uint8_t)(nonce >> 8);

  uint8_t streamBlock[16] = {};
  size_t ncOff = 0;

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = (mbedtls_aes_setkey_enc(&aes, _key, 128) == 0) &&
            (mbedtls_aes_crypt_ctr(&aes, len, &ncOff, nonceCounter, streamBlock, cipher, out) == 0);
  mbedtls_aes_free(&aes);
  return ok;
}

void VictronBLE::DecodeBatteryMonitor(const uint8_t* plain, size_t len)
{
  if (len * 8 < VBLE_RECORD_BITS) {
    DecryptFailures++;
    WS_LOG_W("Victron BLE: record too short (%u bytes)", len);
    return;
  }

  const uint32_t rawTtg     = VBleBits(plain, VBLE_BIT_TTG, 16);
  const uint32_t rawVolt    = VBleBits(plain, VBLE_BIT_VOLTAGE, 16);
  const uint32_t rawAlarm   = VBleBits(plain, VBLE_BIT_ALARM, 16);
  const uint32_t rawAux     = VBleBits(plain, VBLE_BIT_AUX, 16);
  const uint32_t rawAuxMode = VBleBits(plain, VBLE_BIT_AUXMODE, 2);
  const uint32_t rawCurrent = VBleBits(plain, VBLE_BIT_CURRENT, 22);
  const uint32_t rawUsed    = VBleBits(plain, VBLE_BIT_CONSUMED, 20);
  const uint32_t rawSOC     = VBleBits(plain, VBLE_BIT_SOC, 10);

  /* Voltage and SOC are the two the charge logic cannot run without, so a
     not-available reading in either means the whole frame is not usable rather
     than something to paper over with a stale value. */
  if (rawVolt == 0x7FFF || rawSOC == 0x3FF) {
    DecryptFailures++;
    WS_LOG_W("Victron BLE: shunt reported no voltage/SOC");
    return;
  }

  VoltagemV   = VBleSigned(rawVolt, 16) * 10;          // 0.01V -> mV
  SOCPermille = (uint16_t) rawSOC;                     // already 0.1%
  CurrentmA   = (rawCurrent == 0x3FFFFF) ? 0 : VBleSigned(rawCurrent, 22);
  ConsumedmAh = (rawUsed == 0xFFFFF) ? 0 : -((int32_t) rawUsed) * 100;  // 0.1Ah -> mAh, always a deficit
  TimeToGoMins = (rawTtg == 0xFFFF) ? 0 : (uint16_t) rawTtg;
  AlarmReason = (uint16_t) rawAlarm;
  AuxMode     = (uint8_t) rawAuxMode;
  AuxRaw      = (int16_t) rawAux;

  LastUpdateMs = millis();
  DataValid = true;
}

void VictronBLE::NoteFound(const NimBLEAdvertisedDevice* dev, const uint8_t* vp, size_t len)
{
  const String mac = dev->getAddress().toString().c_str();
  for (uint8_t i = 0; i < _foundCount; i++) {
    if (mac.equalsIgnoreCase(_found[i].mac)) {
      _found[i].rssi = dev->getRSSI();      // keep the freshest signal reading
      return;
    }
  }
  if (_foundCount >= VBLE_MAX_FOUND) return;

  VictronBLEFound& f = _found[_foundCount];
  snprintf(f.mac, sizeof(f.mac), "%s", mac.c_str());
  const std::string name = dev->getName();
  snprintf(f.name, sizeof(f.name), "%s", name.empty() ? "(no name)" : name.c_str());
  f.productId   = (len >= 4) ? (uint16_t)(vp[2] | (vp[3] << 8)) : 0;
  f.recordType  = (len >= 5) ? vp[4] : 0;
  f.rssi        = dev->getRSSI();
  f.encryptedOK = (len >= 8 && vp[0] == VICTRON_REC_PRODUCT_ADV);
  _foundCount++;
}

void VictronBLE::ParseAdvert(const NimBLEAdvertisedDevice* dev)
{
  const std::string md = dev->getManufacturerData();
  if (md.length() < 10) return;

  const uint8_t* raw = (const uint8_t*) md.data();
  size_t len = md.length();

  /* Some stacks hand over the two-byte company identifier and some strip it.
     Detect rather than assume: 0xE1 0x02 is Victron little endian. Getting this
     wrong shifts every field by two, which the key check below would catch, but
     it is better not to rely on that. */
  if (raw[0] == (VICTRON_COMPANY_ID & 0xFF) && raw[1] == (VICTRON_COMPANY_ID >> 8)) {
    raw += 2;
    len -= 2;
  }

  if (len < 9 || raw[0] != VICTRON_REC_PRODUCT_ADV) return;   // not a Victron instant readout

  if (_sniffer) {
    char hex[3 * 24];
    hex[0] = '\0';
    const size_t show = (len > 24) ? 24 : len;
    for (size_t i = 0; i < show; i++) snprintf(hex + (i * 3), sizeof(hex) - (i * 3), "%02X ", raw[i]);
    if (show > 0) hex[(show * 3) - 1] = '\0';
    WS_LOG_I("Victron BLE %s rec=0x%02X keychk=0x%02X [%u] %s",
             dev->getAddress().toString().c_str(), raw[4], raw[7], (unsigned) len, hex);
  }

  if (_discovering) NoteFound(dev, raw, len);

  if (!Configured()) return;

  // Only the configured shunt from here on. Compared as text rather than raw
  // bytes to stay clear of NimBLE's address byte ordering; this runs about once
  // a second, so the cost does not matter.
  if (!GetMac().equalsIgnoreCase(dev->getAddress().toString().c_str())) return;

  AdvertsSeen++;

  if (raw[4] != VICTRON_REC_BATTERY_MON) return;  // a Victron device, but not a shunt

  if (raw[7] != _key[0]) {
    DecryptFailures++;
    WS_LOG_W("Victron BLE: wrong encryption key (advert expects %02X, key starts %02X)",
             raw[7], _key[0]);
    return;
  }

  const uint16_t nonce = (uint16_t)(raw[5] | (raw[6] << 8));
  const uint8_t* cipher = raw + 8;
  const size_t cipherLen = len - 8;

  uint8_t plain[32] = {};
  if (!Decrypt(cipher, cipherLen, nonce, plain)) {
    DecryptFailures++;
    WS_LOG_W("Victron BLE: decrypt failed");
    return;
  }

  DecodeBatteryMonitor(plain, cipherLen);
}
