#pragma once
#include <Arduino.h>

// Forward declaration - implemented in HTTPWSFunctions.h
extern void sendLogToWS(const char* message, const char* level);

#define WS_LOG_D(format, ...) do { \
  char buf[256]; \
  snprintf(buf, sizeof(buf), format, ##__VA_ARGS__); \
  log_d("%s", buf); \
  sendLogToWS(buf, "debug"); \
} while(0)

#define WS_LOG_I(format, ...) do { \
  char buf[256]; \
  snprintf(buf, sizeof(buf), format, ##__VA_ARGS__); \
  log_i("%s", buf); \
  sendLogToWS(buf, "info"); \
} while(0)

#define WS_LOG_W(format, ...) do { \
  char buf[256]; \
  snprintf(buf, sizeof(buf), format, ##__VA_ARGS__); \
  log_w("%s", buf); \
  sendLogToWS(buf, "warning"); \
} while(0)

#define WS_LOG_E(format, ...) do { \
  char buf[256]; \
  snprintf(buf, sizeof(buf), format, ##__VA_ARGS__); \
  log_e("%s", buf); \
  sendLogToWS(buf, "error"); \
} while(0)
