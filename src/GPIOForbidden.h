#pragma once

#include <Arduino.h>

#ifndef FORBIDDEN_GPIO_LIST
#define FORBIDDEN_GPIO_LIST ""
#endif

/* Two kinds of unavailable pin, kept apart because they are decided in
   different places.
 *
 * FORBIDDEN_GPIO_LIST, from platformio.ini, describes a BOARD: which header
 * pins the MCP2515, the RS485 transceiver or the expansion header is already
 * sitting on. Every env states its own, because that is a property of the
 * wiring and nothing else can know it.
 *
 * The lists below describe the CHIP, and no env should have to state them. The
 * SPI flash this firmware is executing out of is on the same pins on every
 * module of a part, and a GPIO number the package does not bond out is not a
 * wiring choice either. Leaving them to the per-env lists is how the S3 ended
 * up able to accept CAN TX 27 - a flash pin - from the web UI on every one of
 * its five envs, while the classic ESP32 lists had correctly carried 6-11 all
 * along. The failure is silent and total: the pin is driven, the board does not
 * come back, and nothing has been written down anywhere that says why.
 *
 * Anything added here must be true of the part or of the build, never of one
 * board - that is what the ini list is for.
 */
#if CONFIG_IDF_TARGET_ESP32S3
  /* 22-25 do not exist on the S3 at all - the numbering skips them. 26-32 are
     the in-package or on-module SPI flash (SPICS1, SPIHD, SPIWP, SPICS0,
     SPICLK, SPIQ, SPID) on every module this project builds for. */
  #define CHIP_RESERVED_GPIO_LIST "22,23,24,25,26,27,28,29,30,31,32"
#elif CONFIG_IDF_TARGET_ESP32
  // 6-11 are the SPI flash on every classic ESP32 module.
  #define CHIP_RESERVED_GPIO_LIST "6,7,8,9,10,11"
#else
  #define CHIP_RESERVED_GPIO_LIST ""
#endif

/* Octal PSRAM takes 33-37 as well, which is why this is behind the flag rather
   than in the list above: a quad part leaves them free, and every -psram env
   here is an octal (qio_opi) module. See platformio.ini's PSRAM section. */
#if CONFIG_IDF_TARGET_ESP32S3 && defined(BOARD_HAS_PSRAM)
  #define CHIP_RESERVED_PSRAM_GPIO_LIST "33,34,35,36,37"
#else
  #define CHIP_RESERVED_PSRAM_GPIO_LIST ""
#endif

/* 19/20 are the S3's native USB D-/D+. They are ordinary GPIOs on a build whose
   console is UART0, and on the two boards that have no UART to fall back on
   (Waveshare, XIAO) they are the only console and the only flashing route the
   board has - so on those, handing them to a fan or a CAN transceiver from the
   web UI is a one-way trip. Gated on the build's own console setting rather
   than on a board name, so it stays right for any env added later. */
#if CONFIG_IDF_TARGET_ESP32S3 && defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  #define CHIP_RESERVED_USB_GPIO_LIST "19,20"
#else
  #define CHIP_RESERVED_USB_GPIO_LIST ""
#endif

static inline bool IsForbiddenPin(uint8_t pin) {
  static bool initialized = false;
  static uint8_t pins[96];
  static size_t count = 0;

  if (!initialized) {
    // The board's list first, then the chip's. Overlap is expected - the XIAO's
    // ini list names most of the flash pins itself - so entries are deduped
    // rather than counted twice against the cap.
    const char* const lists[] = {
      FORBIDDEN_GPIO_LIST,
      CHIP_RESERVED_GPIO_LIST,
      CHIP_RESERVED_PSRAM_GPIO_LIST,
      CHIP_RESERVED_USB_GPIO_LIST,
    };
    count = 0;
    for (const char* entry : lists) {
      String list = String(entry);
      list.trim();
      int start = 0;
      while (true) {
        int comma = list.indexOf(',', start);
        String token = (comma == -1) ? list.substring(start) : list.substring(start, comma);
        token.trim();
        if (token.length() > 0) {
          long v = token.toInt();
          if (v >= 0 && v <= 255) {
            bool seen = false;
            for (size_t i = 0; i < count; ++i) {
              if (pins[i] == (uint8_t)v) { seen = true; break; }
            }
            if (!seen && count < (sizeof(pins) / sizeof(pins[0]))) {
              pins[count++] = (uint8_t)v;
            }
          }
        }
        if (comma == -1) break;
        start = comma + 1;
      }
    }
    initialized = true;
  }

  for (size_t i = 0; i < count; ++i) {
    if (pins[i] == pin) return true;
  }
  return false;
}
