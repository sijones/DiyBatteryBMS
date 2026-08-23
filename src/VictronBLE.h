#pragma once
#include <Arduino.h>
/* NimBLE is only linked into -psram builds (see platformio.ini) - the radio
   refuses to run without real PSRAM anyway (HardwareSupported() below), so a
   board that can never use it should not pay to compile the stack: dozens of
   .c files across the mesh/GAP/GATT/HCI layers that this project never calls
   into. Everything here that names a NimBLE type follows the same guard. */
#ifdef BOARD_HAS_PSRAM
#include <NimBLEDevice.h>
#endif
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

    /* What the advertisement says the device IS, rather than what it is
       reading. Every advert carries these and they were parsed and thrown
       away for the configured shunt - only the discovery scan kept them, so a
       board that had finished setting up knew nothing about the device it was
       listening to. On a BLE-only install this is the only identity there is:
       Instant Readout carries no serial number and no firmware version, those
       exist only on the VE.Direct wire. */
    volatile uint16_t ProductId = 0;      // 0 until an advert has been seen
    volatile int8_t   Rssi = 0;
    String            DeviceName;         // as set in VictronConnect

    /* Product id -> the name on the box.

       Victron publishes these ids and they are the only model information an
       advertisement carries, so this is what fills the Model field on a board
       with no VE.Direct cable. Deliberately a short list of the monitors this
       firmware is for, not a copy of the whole Victron catalogue: an id that
       is not here is reported as its hex value rather than guessed at, which
       is honest and still tells a user something they can search for. */
    static const char* ModelFromProductId(uint16_t pid) {
      switch (pid) {
        case 0xA381: return "BMV-712 Smart";
        case 0xA382: return "BMV-712 Smart Rev2";
        case 0xA383: return "SmartShunt 500A/50mV";
        case 0xA389: return "SmartShunt 500A/50mV";
        case 0xA38A: return "SmartShunt 1000A/50mV";
        case 0xA38B: return "SmartShunt 2000A/50mV";
        case 0xA384: return "SmartShunt IP65 500A/50mV";
        case 0xA385: return "SmartShunt IP65 1000A/50mV";
        default:     return nullptr;
      }
    }

    /* Whether this module has PSRAM to spill into, checked at runtime rather
       than trusting BOARD_HAS_PSRAM alone - that flag says what the build
       expected, not what the chip actually reports, and a -psram env on the
       wrong memory_type gets no PSRAM at all (see Diagnostics.cpp). Without
       it the NimBLE stack alone can tip a ~70KB internal heap into the
       failed-allocation-calls-abort() territory Diagnostics.h describes, so
       the radio refuses to start rather than gambling on being the exception. */
#ifdef BOARD_HAS_PSRAM
    static bool HardwareSupported() { return ESP.getPsramSize() > 0; }
#else
    // NimBLE was not even compiled in - nothing to check at runtime.
    static bool HardwareSupported() { return false; }
#endif

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
    bool HaveKey() { return _haveKey; }
    bool StartDiscovery(uint16_t seconds);   // populates the Found list for the UI
    bool DiscoveryTick();                    // true once, on the pass the window closes
    void Stop();
#ifdef BOARD_HAS_PSRAM
    void ParseAdvert(const NimBLEAdvertisedDevice* dev);
#endif

    /* Bring the radio up for a scan or a sniff even when the shunt is being read
       over the wire. Without this, the two things you need in order to set BLE
       up - find the device, watch its adverts - would both require BLE to
       already be selected, which is the wrong way round. */
    bool EnsureRunning();

  private:
    bool _enabled = false;
    bool _sniffer = false;
    bool _haveKey = false;
    bool _haveMac = false;
    bool _discovering = false;
    uint32_t _discoveryEndMs = 0;
    uint8_t _key[16] = {};
    uint8_t _mac[6] = {};
    VictronBLEFound _found[VBLE_MAX_FOUND];
    uint8_t _foundCount = 0;
#ifdef BOARD_HAS_PSRAM
    NimBLEScan* _scan = nullptr;

    void NoteFound(const NimBLEAdvertisedDevice* dev, const uint8_t* vp, size_t len);
#endif
    bool Decrypt(const uint8_t* cipher, size_t len, uint16_t nonce, uint8_t* out);
    void DecodeBatteryMonitor(const uint8_t* plain, size_t len);

#ifdef BOARD_HAS_PSRAM
  friend class VictronScanCallbacks;
#endif
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
