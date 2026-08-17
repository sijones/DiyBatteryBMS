#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include "WebLog.h"

/* Victron "Instant Readout" over BLE.

   A SmartShunt broadcasts its live figures about once a second in the
   manufacturer-specific field of an ordinary BLE advertisement. Nothing
   connects and nothing is paired - this only listens, so it cannot disturb
   VictronConnect or anything else already talking to the shunt.

   The payload is AES-128-CTR encrypted with a per-device key, which is read out
   of VictronConnect under Product info > Instant readout via Bluetooth. There
   is no key exchange: possession of the key is the whole authentication.

   Layout of the manufacturer data, after the two-byte company identifier
   0x02E1 (which some BLE stacks strip and some do not - ParseAdvert copes with
   either, see there):

     [0]     0x10    record type: product advertisement
     [1]     0x00
     [2..3]  product id, little endian
     [4]     record type: 0x02 = battery monitor, which is what a SmartShunt is
     [5..6]  nonce / data counter, little endian - the AES counter's start value
     [7]     first byte of the encryption key, as a check
     [8..]   ciphertext

   Byte 7 is what makes this safe to get wrong: if the offsets were misread the
   check byte would not match the configured key, so a bad layout is rejected
   rather than quietly producing a plausible but wrong SOC. */

#define VICTRON_COMPANY_ID      0x02E1
#define VICTRON_REC_PRODUCT_ADV 0x10
#define VICTRON_REC_BATTERY_MON 0x02

// Bit offsets within the decrypted battery-monitor record. The record is a
// little-endian bit stream, not byte-aligned fields - current straddles three
// bytes and SOC straddles two.
#define VBLE_BIT_TTG        0    // 16 bits, minutes,      0xFFFF   = not available
#define VBLE_BIT_VOLTAGE    16   // 16 bits, signed, 10mV, 0x7FFF   = not available
#define VBLE_BIT_ALARM      32   // 16 bits, alarm reason bitfield
#define VBLE_BIT_AUX        48   // 16 bits, meaning set by aux mode
#define VBLE_BIT_AUXMODE    64   // 2 bits,  0=starter V, 1=midpoint V, 2=temperature, 3=none
#define VBLE_BIT_CURRENT    66   // 22 bits, signed, mA,   0x3FFFFF = not available
#define VBLE_BIT_CONSUMED   88   // 20 bits, 0.1Ah,        0xFFFFF  = not available
#define VBLE_BIT_SOC        108  // 10 bits, 0.1%,         0x3FF    = not available
#define VBLE_RECORD_BITS    118

#define VBLE_MAX_FOUND      8    // devices remembered per discovery scan

struct VictronBLEFound {
  char     mac[18];
  char     name[24];
  uint16_t productId;
  uint8_t  recordType;
  int8_t   rssi;
  bool     encryptedOK;   // header parsed and looked like a product advertisement
};

class VictronBLE {
  public:
    // ---- decoded values, all in the same units the VE.Direct path uses ----
    volatile bool     DataValid = false;
    volatile int32_t  VoltagemV = 0;
    volatile int32_t  CurrentmA = 0;
    volatile uint16_t SOCPermille = 0;      // 0.1% steps, as sent
    volatile int32_t  ConsumedmAh = 0;
    volatile uint16_t TimeToGoMins = 0;
    volatile uint16_t AlarmReason = 0;
    volatile int16_t  AuxRaw = 0;
    volatile uint8_t  AuxMode = 3;
    volatile uint32_t LastUpdateMs = 0;
    volatile uint32_t AdvertsSeen = 0;
    volatile uint32_t DecryptFailures = 0;

    bool Enabled() { return _enabled; }
    bool Configured() { return _haveMac && _haveKey; }
    bool Sniffer() { return _sniffer; }
    void SetSniffer(bool on) { _sniffer = on; }
    uint8_t FoundCount() { return _foundCount; }
    const VictronBLEFound* Found(uint8_t i) { return (i < _foundCount) ? &_found[i] : nullptr; }
    bool Scanning() { return _discovering; }

    /* Fresh means an advert decoded within the window. A SmartShunt advertises
       about once a second, so anything beyond a few seconds means it is out of
       range, switched off, or the key changed. */
    bool DataFresh(uint32_t withinMs = 15000) {
      return DataValid && LastUpdateMs > 0 && (millis() - LastUpdateMs) < withinMs;
    }

    bool Begin(bool enabled);
    void SetKeyHex(const String& hex);       // 32 hex chars
    void SetMac(const String& mac);          // "aa:bb:cc:dd:ee:ff"
    String GetMac();
    bool StartDiscovery(uint16_t seconds);   // populates the Found list for the UI
    void Stop();
    void ParseAdvert(const NimBLEAdvertisedDevice* dev);

  private:
    bool _enabled = false;
    bool _sniffer = false;
    bool _haveKey = false;
    bool _haveMac = false;
    bool _discovering = false;
    uint8_t _key[16] = {};
    uint8_t _mac[6] = {};
    VictronBLEFound _found[VBLE_MAX_FOUND];
    uint8_t _foundCount = 0;
    NimBLEScan* _scan = nullptr;

    void NoteFound(const NimBLEAdvertisedDevice* dev, const uint8_t* vp, size_t len);
    bool Decrypt(const uint8_t* cipher, size_t len, uint16_t nonce, uint8_t* out);
    void DecodeBatteryMonitor(const uint8_t* plain, size_t len);

  friend class VictronScanCallbacks;
};

extern VictronBLE VictronBle;

/* ---------------- bit-stream helpers ----------------
   Written out longhand rather than with shifts and masks over a uint64: the
   fields cross byte boundaries at odd offsets and this way the code says what
   the spec says. It runs once a second, so the loop costs nothing worth
   optimising away. */
static inline uint32_t VBleBits(const uint8_t* buf, size_t bitOffset, uint8_t bitCount) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < bitCount; i++) {
    const size_t b = bitOffset + i;
    if (buf[b >> 3] & (1u << (b & 7))) v |= (1ul << i);
  }
  return v;
}

static inline int32_t VBleSigned(uint32_t v, uint8_t bits) {
  const uint32_t signBit = 1ul << (bits - 1);
  return (v & signBit) ? (int32_t)(v | ~((1ul << bits) - 1)) : (int32_t)v;
}
