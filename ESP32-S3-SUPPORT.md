# ESP32-S3 ESPCAN Support

## Overview

This document describes the ESP32-S3 support for the built-in CAN controller (TWAI - Two-Wire Automotive Interface).

## Background

The ESP32-S3 uses a different CAN controller implementation compared to the original ESP32. While the original ESP32 uses the CAN controller that can be accessed via the ESP32-CAN-Lib library, the ESP32-S3 uses the TWAI (Two-Wire Automotive Interface) controller which is part of the ESP-IDF driver framework.

## Changes Made

### 1. New PlatformIO Environment

A new build environment `esp32s3-ESPCAN` has been added to `platformio.ini`:

```ini
[env:esp32s3-ESPCAN]
platform = https://github.com/platformio/platform-espressif32.git
board = esp32-s3-devkitc-1
board_build.filesystem = littlefs
framework = arduino
lib_deps = 
    ESP32Async/AsyncTCP@^3.4.5
    ESP32Async/ESPAsyncWebServer@^3.7.10
    https://github.com/sijones/ESPAsyncHTTPUpdateServer.git
    https://github.com/theelims/PsychicMqttClient.git
    https://github.com/sijones/ESP32-CAN-Lib.git
    https://github.com/bblanchon/ArduinoJson.git
    https://github.com/PaulStoffregen/Time.git
    https://github.com/hasenradball/LCD-I2C.git#master
upload_protocol = esptool
monitor_speed = 115200
board_build.partitions = default.csv
build_type = release
build_flags = -DCORE_DEBUG_LEVEL=4 -DVEDIRECT_RX=33 -DVEDIRECT_TX=32 -DONEWIREPIN=12 -DCAN_TX_PIN=27 -DCAN_RX_PIN=26 -DCAN_EN_PIN=23 -D ESPCAN -D ESPCAN_S3
```

Key differences:
- Board: `esp32-s3-devkitc-1` (ESP32-S3 development board)
- Additional build flag: `-D ESPCAN_S3` to enable ESP32-S3 specific code

### 2. Code Changes

#### CANBUS.h
- Added conditional includes to use `<driver/twai.h>` for ESP32-S3 instead of `<ESP32CAN.h>`

#### CANBUS.cpp
- **SendToDriver()**: Added ESP32-S3 implementation using `twai_transmit()` API
- **canSendTask()**: Added ESP32-S3 implementation using `twai_receive()` API for non-blocking CAN frame reception
- **Begin()**: Added ESP32-S3 TWAI initialization using:
  - `twai_general_config_t` for GPIO and mode configuration
  - `twai_timing_config_t` for 500kbps baud rate
  - `twai_filter_config_t` to accept all messages
  - `twai_driver_install()` and `twai_start()` to initialize and start the driver

## Implementation Details

### TWAI Driver Configuration

The ESP32-S3 TWAI driver is configured with:
- **Speed**: 500 kbps (standard for Pylontech protocol)
- **Mode**: Normal mode (TWAI_MODE_NORMAL)
- **Filter**: Accept all messages (TWAI_FILTER_CONFIG_ACCEPT_ALL)
- **Pins**: Configurable via build flags (default: TX=27, RX=26, EN=23)

### Pin Configuration

The ESP32-S3 has different GPIO constraints:
- GPIO range: 0-48 (49 pins total: GPIO0, GPIO1, ..., GPIO47, GPIO48)
- Enable pin: Typically use GPIO 1-48 (avoid GPIO 0 due to boot mode strapping)
- TX and RX pins: Configurable via the TWAI driver (GPIO 0-48)
- Original ESP32 for comparison: GPIO 0-39 (with some restrictions)

### Differences from Original ESP32

| Feature | ESP32 (Original) | ESP32-S3 |
|---------|------------------|----------|
| CAN Controller | CAN Controller | TWAI Controller |
| Library | ESP32-CAN-Lib | ESP-IDF driver/twai.h |
| Initialization | ESP32Can.CANInit() | twai_driver_install() + twai_start() |
| Transmit | ESP32Can.CANWriteFrame() | twai_transmit() |
| Receive | xQueueReceive() | twai_receive() |
| Frame Structure | CAN_frame_t | twai_message_t |

## Building for ESP32-S3

To build the firmware for ESP32-S3 with ESPCAN support:

```bash
platformio run -e esp32s3-ESPCAN
```

To upload:

```bash
platformio run -e esp32s3-ESPCAN -t upload
```

## Hardware Requirements

- ESP32-S3 development board (e.g., ESP32-S3-DevKitC-1)
- CAN transceiver (e.g., SN65HVD230 or similar)
- Connections:
  - CAN_TX (GPIO 27) to CAN transceiver TX
  - CAN_RX (GPIO 26) to CAN transceiver RX
  - CAN_EN (GPIO 23) to transceiver enable (if needed)

## Notes

1. The existing ESP32 environments (`esp32dev`, `esp32plus`, `esp32-ESPCAN`) remain unchanged and continue to work as before.
2. The ESP32-S3 implementation uses the native ESP-IDF TWAI driver, which is more robust and better supported for ESP32-S3.
3. All Pylontech protocol functionality remains the same - only the low-level CAN driver implementation differs.

## Testing

After building and flashing:
1. Monitor the serial output: `platformio device monitor -e esp32s3-ESPCAN`
2. Check for log messages:
   - "TWAI driver installed"
   - "TWAI driver started"
   - "ESP32-S3 CAN Bus (TWAI) initialised"
3. Verify CAN communication with your inverter

## Troubleshooting

If CAN initialization fails:
- Check GPIO pin connections
- Verify CAN transceiver power and connections
- Check that the CAN bus is properly terminated (120Ω resistors)
- Review serial logs for specific error messages

## Future Enhancements

Potential improvements for ESP32-S3 support:
- Add support for different CAN speeds (configurable)
- Implement error recovery mechanisms specific to TWAI
- Add CAN bus statistics and monitoring
