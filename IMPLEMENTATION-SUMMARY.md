# ESP32-S3 ESPCAN Implementation Summary

## Overview
This PR implements support for ESP32-S3 with built-in CAN controller (TWAI - Two-Wire Automotive Interface), creating a separate build environment and implementation as requested in the issue.

## Changes Summary

### Files Changed (5 files, +294 lines, -25 lines)

1. **platformio.ini** (+22 lines)
   - Added new `[env:esp32s3-ESPCAN]` build environment
   - Configured for ESP32-S3-DevKitC-1 board
   - Added `ESPCAN_S3` build flag to distinguish from original ESP32

2. **src/CANBUS.h** (+10 lines, -2 lines)
   - Added conditional includes for ESP32-S3 TWAI driver
   - Maintained backward compatibility with original ESP32 implementation

3. **src/CANBUS.cpp** (+140 lines, -23 lines)
   - Added ESP32-S3 TWAI initialization in `Begin()` function
   - Implemented ESP32-S3 specific `SendToDriver()` using `twai_transmit()`
   - Updated `canSendTask()` with ESP32-S3 `twai_receive()` implementation
   - All changes use conditional compilation to maintain existing functionality

4. **ESP32-S3-SUPPORT.md** (+136 lines, new file)
   - Comprehensive documentation of ESP32-S3 implementation
   - Hardware requirements and pin configurations
   - Build instructions and troubleshooting guide
   - Comparison table between ESP32 and ESP32-S3 implementations

5. **README.md** (+11 lines, -2 lines)
   - Updated hardware recommendations to include ESP32-S3
   - Added link to ESP32-S3 documentation
   - Clarified supported platforms

## Key Implementation Details

### Conditional Compilation Strategy
```c
#ifdef ESPCAN
  #ifdef ESPCAN_S3
    // ESP32-S3 specific TWAI code
  #else
    // Original ESP32 CAN code
  #endif
#else
  // MCP2515 code (unchanged)
#endif
```

### ESP32-S3 TWAI Configuration
- **Baud Rate**: 500 kbps (Pylontech protocol standard)
- **Mode**: TWAI_MODE_NORMAL
- **Filter**: Accept all messages (TWAI_FILTER_CONFIG_ACCEPT_ALL)
- **Default Pins**: TX=27, RX=26, EN=23

### API Differences Handled
| Function | ESP32 (Original) | ESP32-S3 |
|----------|------------------|----------|
| Initialize | ESP32Can.CANInit() | twai_driver_install() + twai_start() |
| Transmit | ESP32Can.CANWriteFrame() | twai_transmit() |
| Receive | xQueueReceive() | twai_receive() |
| Frame Type | CAN_frame_t | twai_message_t |

## Build Environments

### Available Environments
1. `esp32dev` - ESP32 with MCP2515 (unchanged)
2. `esp32plus` - ESP32 with MCP2515 (unchanged)
3. `esp32-ESPCAN` - ESP32 with built-in CAN (unchanged)
4. `esp32s3-ESPCAN` - **NEW** ESP32-S3 with built-in CAN (TWAI)

### Building for ESP32-S3
```bash
platformio run -e esp32s3-ESPCAN
platformio run -e esp32s3-ESPCAN -t upload
```

## Testing Recommendations

1. **Compilation Testing**: Build all 4 environments to ensure no regressions
2. **Hardware Testing**: Test ESP32-S3 with CAN transceiver
3. **Protocol Testing**: Verify Pylontech protocol communication with inverter
4. **Serial Monitoring**: Check for initialization messages:
   - "TWAI driver installed"
   - "TWAI driver started"
   - "ESP32-S3 CAN Bus (TWAI) initialised"

## Backward Compatibility

✅ All existing builds (esp32dev, esp32plus, esp32-ESPCAN) remain unchanged
✅ No modifications to MCP2515 implementation
✅ No changes to Pylontech protocol logic
✅ Web interface configuration remains compatible
✅ MQTT functionality unaffected

## Hardware Requirements for ESP32-S3

- ESP32-S3 development board (ESP32-S3-DevKitC-1 or similar)
- CAN transceiver (SN65HVD230 or compatible)
- Proper connections:
  - GPIO 27 → CAN TX
  - GPIO 26 → CAN RX
  - GPIO 23 → CAN Enable (optional)
- 120Ω termination resistors on CAN bus

## Future Enhancements

Potential improvements identified:
- Configurable CAN speeds
- TWAI-specific error recovery
- CAN bus statistics and monitoring
- Auto-detection of ESP32 variant

## Issue Resolution

This implementation addresses the issue: **"ESP32-S3 requires a different implementation to the original ESP32"**

✅ Created separate build environment (`esp32s3-ESPCAN`)
✅ Implemented ESP32-S3 specific TWAI driver code
✅ Maintained backward compatibility with all existing builds
✅ Provided comprehensive documentation
✅ Used conditional compilation for clean separation

## Notes

- The TWAI driver is native to ESP-IDF and more robust for ESP32-S3
- No external libraries required beyond ESP-IDF (included in Arduino framework)
- Pin constraints differ: ESP32-S3 supports GPIO 0-48 vs ESP32's 0-34
- All Pylontech protocol functionality remains identical across platforms
