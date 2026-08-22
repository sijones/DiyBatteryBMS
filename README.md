This is a hobby project and I add features as and when I have time, it means sometimes it's not updated for a while and then sometimes I add a number of features!

I now accept donations towards supporting the project: https://buymeacoffee.com/sijones2012

You can now sponsor and donate to me, the link is on here to buy me a coffee ;)

The new UI means no coding knowledge is required, just flash and set up.

A lot of people use it from Diy Solar Forum, support from community users is also available:
https://diysolarforum.com/threads/diy-battery-via-smart-shunt-to-inverter-integration-solis-etc.44750/

DiyBatteryBMS takes data from a Victron Smart Shunt and sends it to a inverter over CAN using Pylontech protocol allowing for "DIY LifePO4" Batteries to be integrated.

The Victron Smart Shunt provides the actual monitoring data, this software translates it to Pylontech protocol that most Inverters understand, the part that you need to configure is the charging voltage, current that is also sent to the Inverter.

As long as the Inverter accepts Pylontech protocol over CAN Bus this software should work with it, it's tested with Solis Inverters but forum users have reported it working with others.

The data is also sent over MQTT and allows commands to be sent back to control Charge/Discharge/Force Charge.

This software supports:
- ESP32 developer boards with MCP2515 CAN Bus adapter
- ESP32 with built-in CAN controller
- ESP32-S3 with built-in CAN controller (using TWAI driver)
- Lot's of different configurations of boards, just try the build that matches your config.

If you don't use the Lilygo CAN485 board, you will need to add a CAN transceiver to your ESP32 or ESP32-S3 board.

See the WIKI for more detailed documentation.

- See also: https://www.victronenergy.com/live/vedirect_protocol:faq

With the help of the MQTT server you can integrate the monitoring data to virtually any Home Automation System. I use Home Assistant to automate off peak battery charging (using Force Charge) and can also enable and disable the charging and discharging.

## Features
- Setup from a browser, flash to your ESP32 device then go to http://192.168.4.1 and connect it to your wifi, once connected go to http://diy-battery-bms.local or it's IP address to configure all settings.
- **NEW**: Improved WiFi Setup Experience:
  - WebSocket-based WiFi network scanning with automatic background refresh
  - Networks sorted by signal strength for easier selection
  - Duplicate networks filtered (strongest signal retained)
  - Preserved saved SSID selection across scans
  - WiFi configuration requires explicit save (manual save button for SSID, password, and mDNS hostname)
- Listen to VE.Direct messages and publish some of the information to a MQTT broker<br> The MQTT Topic is fully configurable.
- Home Assistant MQTT Discovery - Automatically creates all sensors and switches in Home Assistant with no manual configuration required
- Supports MQTT Commands to enable and disable charge/discharging of an inverter, force charge the batteries to be able to charge over night at off peak rates.
- Supports single MQTT server
- OTA (Over The Air Update)<br> use your browser and go to http://IPADDRESS/update and upload the lastest binary
- LCD Screen Support (LCD 20x4 via I2C)
- Configurable NTP Server with timezone support - used for log and syslog timestamps
- Voltage Limited Charging, automatically reducing charge current to keep the voltage stable
- SOC Reset, if the Smart Shunt is at 100% but battery voltage is under charged voltage sends 99% until met
- SOC recharge, hold off recharging until under the SOC restart limit.
- Temperature protection - separate charge and discharge cut-offs, with the battery temperature taken from the Smart Shunt or an external MQTT topic
- Fan control - linear PWM ramp between configurable off and full-speed temperatures
- Backup and restore all settings to a JSON file
- **PowerPilot integration** - PowerPilot can use a DiyBatteryBMS device as a networked hardware node -
  battery telemetry, inverter control, or both - over WiFi with no CAN HAT on the Pi and no MQTT broker.
  See [PowerPilot.md](PowerPilot.md) for the WebSocket interface, or the summary
  [below](#powerpilot-integration-websocket-api).

### Web Interface
- Dark, light and automatic themes - the toggle is in the header and your choice is remembered
- Laid out for phones as well as desktops; the dashboard packs two readouts per row in portrait
- Charge and discharge are proper toggle switches, reachable by keyboard and screen reader
- The tab you are on is kept in the URL, so a refresh or bookmark returns you to it
- If the connection drops the dashboard greys out and tells you the readings are frozen, instead of showing stale numbers as though they were live
- Settings that conflict are flagged as you type - for example a charge voltage above the over-voltage cutoff, or a low SOC limit above the high one
- Factory Reset now lives behind a "Danger Zone" section and asks you to type ERASE
- **Voltages and currents are now entered in V, A and Ah** rather than mV and mA

### Charging Visibility
The dashboard now shows what the firmware is actually doing rather than leaving you to infer it:
- **SOC sent to the inverter** - when the reported value differs from the real pack SOC, the dashboard shows both and explains why (holding 99% until fully charged, "never send 100%", or the force-charge trick)
- **Absorption progress** - both of the timers that can end absorption, side by side: the tail-current hold and the maximum absorption time. The tail timer resets whenever current rises back above the threshold, and the panel says so, along with which condition is currently holding things up

### Remote Logging (Syslog)
- Forwards the same log lines shown on the Logs tab to a syslog collector over UDP (RFC 3164)
- Configured under Settings, applies immediately without a reboot
- Enter the collector's IP address - hostnames are deliberately not resolved, so logging can never block on DNS
- The Logs tab also gained level filtering, plus Copy and Download buttons for reporting problems

### CAN Sniffer
A **CAN sniffer** checkbox on the Logs tab logs every frame received on the bus, with its data
bytes, so you can see exactly what another BMS puts on the wire. It exists for the question that
comes up whenever an inverter misbehaves — *"but my JK/Pace/Seplos works fine"* — which can only
really be answered by looking at what the other BMS sends at the moment the two differ.

- **The device sends nothing while it is on.** That is deliberate: it means you can clip a spare
  node onto someone else's BMS-to-inverter bus without the two fighting over the same message IDs.
  It also means **do not leave it enabled on your own inverter**, which is left without a BMS.
- Turns itself off after 30 minutes, and a reboot always clears it. The setting is never saved and
  is not part of settings export/import.
- Repeated identical frames are logged once and then every 30 seconds, so a long capture stays
  readable and shows only the bytes that actually moved.
- Switching it on selects the new **CAN frames only** log level, so the capture is not buried in
  ordinary log lines; switching it off puts the previous level back. The level is a normal
  dropdown choice, so you can move off it and back at any time.
- Use Copy or Download on the Logs tab to get the capture out - both follow the filter, so with
  CAN frames only selected you get just the capture.
- Nothing on the Logs tab counts as configuration, so none of it raises the unsaved-changes bar.

### Clock and Timezone
- **Timezone support.** Log and syslog timestamps are now in your local time rather than UTC.
  Pick a region from the dropdown under Settings and daylight saving is handled automatically -
  the UK preset switches to BST and back on its own with no firmware update. The underlying
  POSIX TZ string stays visible and editable if your zone is not listed. Applies immediately,
  no reboot.
- **Clock Sync status** on the Firmware tab shows how long ago the clock last synced, and warns
  if syncing has stopped. The device re-syncs automatically every 3 hours.

### Home Assistant Integration
The device now supports **MQTT Discovery** which automatically creates all entities when connected:
- **Sensors**: Battery SOC (%), Voltage (V), Current (A), Power (W), Temperature, Charge/Discharge Current Limits, Charge Phase, Time To Go, Fan PWM, Free Heap
- **Binary Sensors**: Charge/Discharge/Force Charge status indicators  
- **Switches**: Charge Enable, Discharge Enable, Force Charge, Request Full Charge, SOC Trick Enable, Request Flags Enable
- **Number Controls**: Charge Voltage, Charge Current (with 0.1 precision), Float Voltage, Float Current
- **Charging detail**: SOC Sent To Inverter, SOC Override Reason, SOC Override Active, Absorption Elapsed, Absorption Remaining, Tail Current Held, Tail Current Remaining, Tail Current Active
- All entities are grouped under one "DIY Battery BMS" device
- No manual YAML configuration required (HomeAssistant.yaml is now optional for reference only)
- Discovery messages published automatically on MQTT connect

**See [MQTT.md](MQTT.md)** for the whole MQTT interface: every topic published, every `set/` command
accepted with its units and whether it persists, the schedule payload format, and the traps worth
knowing before you automate against it.

**Protocol Control Features:**
- **SOC Trick Enable**: Sends 1/10 of the actual SOC to trick the inverter into force charging
- **Request Flags Enable**: Controls charge/discharge request flags sent to inverter (allows different ways to control inverter response)

### Float Stage

Until now the charge cycle ended at **Complete**: the charge current limit dropped to zero and the
charge-enable flag cleared, while the voltage limit stayed at the absorption target. Several hybrid
inverters — Deye in particular — read that combination as an over-voltage they can only resolve by
*discharging* the pack, so instead of resting after a full charge the battery was pushed back into
the house or the grid at a kilowatt or so until the voltage fell.

A hardware BMS does not do this. It lowers the voltage target and keeps a small current allowance
open. The firmware can now do the same:

| | |
|---|---|
| Setting it | Settings → CC-CV Charging Configuration → **Float Voltage**. `0` works the target out from the charge voltage; a value at or above the charge voltage turns the stage off |
| Typical value | 3.375–3.40 V per cell — 54.0–54.4 V on a 16S LiFePO4 pack |
| **Float Current** | Charge allowance held during float, default 2 A. Never drops below **Min Charge Current** — an allowance the inverter cannot act on recreates the instruction the float stage exists to avoid |
| Leaving float | Same as Complete: SOC below Recharge SOC, or voltage below the float target minus Recharge V Offset |
| Over MQTT | `<topic>/set/FloatVoltage` (V) and `<topic>/set/FloatCurrent` (A), or the matching Home Assistant number entities |

The phase appears as **Float** on the dashboard and on the `ChargePhase` MQTT topic. While in float
the real 100% SOC is sent to the inverter rather than being held at 99%, because the pack genuinely
is full — telling an inverter "99%, and you may not charge" is the contradiction that provoked the
forced discharge in the first place.

**Since 2.8.0-BETA10 every charge ends in float**, with the target worked out for you if you have
not set one. See [Upgrading to 2.8.0-BETA10](#upgrading-to-280-beta10) — in BETA6 to BETA9 the stage
was off unless you configured a float voltage.

If you are still seeing a forced discharge at 100% SOC with float running, check **High SOC (Stop
Charge)** and the **Slow Charge** thresholds — both cut the charge current limit at an SOC boundary
while the voltage limit stays high, which produces the same symptom by a different route. On BETA9
and earlier, check first that a float voltage is configured at all.

### PowerPilot Integration (WebSocket API)

PowerPilot is a home energy manager that plans battery charge and discharge cycles against solar
forecasts and tariff prices. It can use a DiyBatteryBMS device as a networked hardware interface,
over the web UI's own WebSocket at `ws://<device>/ws` — an interface open to any external energy
manager, not just PowerPilot.

**See [PowerPilot.md](PowerPilot.md)** for the full interface: non-persisting set-commands, the
override latch, force charge versus full charge, and live current requests.

### New in 3.0.0 — Arduino core 3.3.11 (ESP-IDF 5.5.5)

Every board now builds against Arduino core 3.3.11 on ESP-IDF 5.5.5, up from core 2.0.17 on IDF
4.4. All seven environments share one platform definition in `[common]`, so there is a single line
to change next time.

The core update is the reason the partition change had to come first: the same source is about
155KB larger on the new core, which no longer fits the old 1.25MB app slot at all (the ESP32-C3
build would be at 114% of it).

**Fan control now works on the ESP32-C3.** 

## Upgrading to 3.0.0 — you must re-flash over USB

3.0 replaces the Arduino `default.csv` partition table. **An over-the-air update cannot replace a
partition table**, so this one upgrade has to be done over USB with all three files —
`bootloader.bin`, `partitions.bin` and `firmware.bin`. The zip in `dist/` contains them along with a
ready-made `flash_example.cmd` / `flash_example.sh`.

**Your settings are kept.** The new tables leave `nvs` at exactly the offset and size it has always
had (`0x9000`, `0x5000`), so everything you have configured survives the re-flash untouched. Do not
pass `erase_flash` — that erases NVS and is what actually loses settings.

If you update over the air anyway, the device runs fine but keeps 2.x's 1.25MB app slot, and will
refuse a future update once the firmware outgrows it. The Firmware tab shows the running **App
Partition** size and warns when a device is still on the old table.

### What changed and why

The old layout spent 1.375MB on a SPIFFS partition this project has never used — the web interface
is compiled into the firmware and every setting lives in NVS. That space now goes to the app:

| | Old (`default.csv`) | New |
|---|---|---|
| App slot (4MB boards) | 1.25MB | **1.9375MB** (`partitions_4mb.csv`) |
| App slot (8MB boards) | 1.25MB | **3.9375MB** (`partitions_8mb.csv`) |
| SPIFFS | 1.375MB, unused | removed |
| NVS | 20KB @ `0x9000` | unchanged, deliberately |

The 8MB boards were running the 4MB table, so half the chip was not addressed at all, and the 16MB
S3 was running the 8MB one. They are sized generously now on purpose: a partition change costs every
user a re-flash, so the one forced at 3.0 should be the last one those boards ever need.

Which table a build uses is now stated in its environment name — see
[Choosing an environment](#choosing-an-environment) — rather than being a property of the board name
that nobody could see.

Two OTA app slots are kept throughout, so the firmware update page still works as before.

## Features to come:

- MQTT to CAN BUS support, use esphome BMS intgrations to feed the data in and send to the inverter.
- Multi Inverter support
- OneWire temperature sensors for charge control
- Heater control (fan control and temperature monitoring are now implemented)

## Limitations
- DiyBatteryBMS is only listening to messages of the VE.Direct device via Serial or BLE<br>You can't request any special data or change any parameters of the VE.Direct device.<br>

## Hardware & Software Installation
See the Wiki page

## Choosing an environment

An environment name is the wiring, then the flash size, then whether the module has PSRAM:

```
esp32s3-ESPCAN-16mb-psram
└──────┬─────┘ └─┬─┘ └─┬─┘
   wiring     flash   PSRAM
```

Pick the one that matches the module in your hand. Flash size is not cosmetic: a 16MB partition
table on a 4MB chip is the fastest way to end up with a board that does not boot, and a build that
addresses only 4MB of a 16MB chip wastes three quarters of it. If you are unsure what you have, the
browser flasher at [diy.power-pilot.uk](https://diy.power-pilot.uk) reads it off the chip and greys
out everything that would not work.

**VEDIRECT_TX is optional.** Nothing is ever sent to the shunt, so only VEDIRECT_RX has to be wired and set. Leave the TX pin blank in the web interface to keep that GPIO free; clearing an existing value unassigns it.

## Disclaimer
I WILL NOT BE HELD LIABLE FOR ANY DAMAGE THAT YOU DO TO YOU, ONE OF YOUR DEVICES, BURN YOUR HOUSE DOWN, ETC.
A CERTAIN LEVEL OF KNOWLEDGE IS EXPECTED, LIKE HOW TO WIRE THINGS AND PROGRAM THE SOFTWARE TO YOUR DEVICE.

## Licence
Copyright (c) 2022-2026 [Nexion Software Solutions Ltd](https://nexion.uk).

Free to use, modify and share for **personal, non-commercial** use. Selling it, or
shipping it as part of a product or on pre-loaded hardware, needs written permission —
commercial licensing enquiries via [nexion.uk](https://nexion.uk).
See [LICENSE](LICENSE) for the full terms.

Third-party components keep their own licences — `src/ONEWIRE.h` (Ralf Lehmann),
`src/VeDirectFrameHandler.cpp` (Victron Energy BV), and the libraries listed in
`platformio.ini`.


