#pragma once

#include <Arduino.h>

#ifndef FORBIDDEN_GPIO_LIST
#define FORBIDDEN_GPIO_LIST ""
#endif

static inline bool IsForbiddenPin(uint8_t pin) {
  static bool initialized = false;
  static uint8_t pins[64];
  static size_t count = 0;

  if (!initialized) {
    String list = String(FORBIDDEN_GPIO_LIST);
    list.trim();
    count = 0;
    int start = 0;
    while (true) {
      int comma = list.indexOf(',', start);
      String token = (comma == -1) ? list.substring(start) : list.substring(start, comma);
      token.trim();
      if (token.length() > 0) {
        long v = token.toInt();
        if (v >= 0 && v <= 255) {
          pins[count++] = (uint8_t)v;
        }
      }
      if (comma == -1) break;
      start = comma + 1;
      if (count >= 64) break;
    }
    initialized = true;
  }

  for (size_t i = 0; i < count; ++i) {
    if (pins[i] == pin) return true;
  }
  return false;
}
