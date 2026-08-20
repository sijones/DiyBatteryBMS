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
- Setup from a browser, flash to your ESP32 device then go to http://192.168.4.1 and connect it to your wifi, once connected go to http://diy-batterybms.local or it's IP address to configure all settings.
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

> **Version note:** everything below marked *New in 2.8.0-BETA5* requires firmware **2.8.0-BETA5 or later**.
> The version is shown in the top right of the web interface and on the Firmware tab.
> If you are on an earlier build these sections will not match what you see.

### New in 2.8.0-BETA5 — Redesigned Web Interface
- Dark, light and automatic themes - the toggle is in the header and your choice is remembered
- Laid out for phones as well as desktops; the dashboard packs two readouts per row in portrait
- Charge and discharge are proper toggle switches, reachable by keyboard and screen reader
- The tab you are on is kept in the URL, so a refresh or bookmark returns you to it
- If the connection drops the dashboard greys out and tells you the readings are frozen, instead of showing stale numbers as though they were live
- Settings that conflict are flagged as you type - for example a charge voltage above the over-voltage cutoff, or a low SOC limit above the high one
- Factory Reset now lives behind a "Danger Zone" section and asks you to type ERASE
- **Voltages and currents are now entered in V, A and Ah** rather than mV and mA

### New in 2.8.0-BETA5 — Charging Visibility
The dashboard now shows what the firmware is actually doing rather than leaving you to infer it:
- **SOC sent to the inverter** - when the reported value differs from the real pack SOC, the dashboard shows both and explains why (holding 99% until fully charged, "never send 100%", or the force-charge trick)
- **Absorption progress** - both of the timers that can end absorption, side by side: the tail-current hold and the maximum absorption time. The tail timer resets whenever current rises back above the threshold, and the panel says so, along with which condition is currently holding things up

### New in 2.8.0-BETA5 — Remote Logging (Syslog)
- Forwards the same log lines shown on the Logs tab to a syslog collector over UDP (RFC 3164)
- Configured under Settings, applies immediately without a reboot
- Enter the collector's IP address - hostnames are deliberately not resolved, so logging can never block on DNS
- The Logs tab also gained level filtering, plus Copy and Download buttons for reporting problems

### New in 2.8.0-BETA11 — CAN Sniffer
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

### New in 2.8.0-BETA5 — Clock and Timezone
- **Timezone support.** Log and syslog timestamps are now in your local time rather than UTC.
  Pick a region from the dropdown under Settings and daylight saving is handled automatically -
  the UK preset switches to BST and back on its own with no firmware update. The underlying
  POSIX TZ string stays visible and editable if your zone is not listed. Applies immediately,
  no reboot.
- **NTP now works with a single server.** Previously only a comma-separated pair worked; one
  address silently never synced. See the upgrade notes below.
- **Clock Sync status** on the Firmware tab shows how long ago the clock last synced, and warns
  if syncing has stopped. The device re-syncs automatically every 3 hours.

### Home Assistant Integration
The device now supports **MQTT Discovery** which automatically creates all entities when connected:
- **Sensors**: Battery SOC (%), Voltage (V), Current (A), Power (W), Temperature, Charge/Discharge Current Limits, Charge Phase, Time To Go, Fan PWM, Free Heap
- **Binary Sensors**: Charge/Discharge/Force Charge status indicators  
- **Switches**: Charge Enable, Discharge Enable, Force Charge, Request Full Charge, SOC Trick Enable, Request Flags Enable
- **Number Controls**: Charge Voltage, Charge Current (with 0.1 precision), Float Voltage, Float Current
- **New in 2.8.0-BETA5 — Charging detail**: SOC Sent To Inverter, SOC Override Reason, SOC Override Active, Absorption Elapsed, Absorption Remaining, Tail Current Held, Tail Current Remaining, Tail Current Active
- All entities are grouped under one "DIY Battery BMS" device
- No manual YAML configuration required (HomeAssistant.yaml is now optional for reference only)
- Discovery messages published automatically on MQTT connect

**See [MQTT.md](MQTT.md)** for the whole MQTT interface: every topic published, every `set/` command
accepted with its units and whether it persists, the schedule payload format, and the traps worth
knowing before you automate against it.

**Protocol Control Features:**
- **SOC Trick Enable**: Sends 1/10 of the actual SOC to trick the inverter into force charging
- **Request Flags Enable**: Controls charge/discharge request flags sent to inverter (allows different ways to control inverter response)

### New in 2.8.0-BETA6 — Float Stage

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

*New in 2.8.0-BETA6.*

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

**Fan control now works on the ESP32-C3.** The fan was driven through MCPWM, a motor-control
peripheral the C3 does not have — that board had a stub that logged "FAN not supported" and did
nothing, and the FAN Pin field was hidden from its web interface. It now uses LEDC, which every
ESP32 variant has, so the C3 gets fan control and the field is shown on all boards. Behaviour on
the other boards is unchanged: same 25kHz, same 30–100% mapping.

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

## Upgrading to 2.8.0-BETA10
- **Every charge now ends in float, and the target is worked out if you have not set one.** Resting
  a full pack at the charge voltage with a 0 A limit asks the inverter for a target and forbids it
  the current to reach it. A zero charge current limit is a *prohibition* in these protocols — the
  same signal a BMS sends for over-voltage or over-temperature — not a description of a battery
  that is simply full, and inverters resolve the contradiction however they see fit. A Solis
  S6-EH1P was reported shutting its PV down and importing the whole house load from the grid; Deye
  answers the same frame by discharging the pack.
- **Float Voltage `0` now means "work it out"**, not "no float stage". The target is 1.5% below the
  charge voltage, which suits every chemistry the setup wizard knows — LiFePO4, NMC and LTO all
  float between 1.2% and 1.9% below their charge target — so it needs neither the chemistry nor the
  cell count. A 16S LiFePO4 pack charging at 55.2 V floats at 54.37 V. The BMS tab shows the figure
  under the field. To keep the old hard stop, set Float Voltage at or above your charge voltage.
- **Min Charge Current is a floor under the float allowance**, as it already was during absorption.
  A current the inverter cannot act on rounds back to the 0 A that float exists to avoid. Your
  configured maximum still wins, so a max of 0 or an energy manager requesting 0 still means 0.
- This mattered less before BETA5. Complete used to bounce straight back to Bulk on a pack with
  Recharge SOC at 100, so the contradictory frame was never held for long. Fixing that made
  Complete a stable resting state and exposed what it had been sending all along.

## Upgrading to 2.8.0-BETA9
- **Slow charge dividers round to the nearest 0.1 A and can no longer exceed your charge current
  limit.** The taper truncated twice — once dividing capacity, once converting to the deciamps the
  CAN frames carry — so it always rounded down: the recommended divider of 24 on a 280 Ah pack
  asked for 11.7 A and sent 11.6 A. It was also applied in place of your configured maximum rather
  than against it, so a large pack on a small divider could have "slow charge" *raise* the limit.
  Both date back to the first 2.x release.
- **Factory Reset now actually erases everything.** WiFi and MQTT settings live in their own NVS
  namespace, and the reset only cleared the other one — so credentials survived a full reset, and
  "Keep WiFi" only kept them by accident. Both options now erase the whole NVS partition, with
  Keep WiFi saving and restoring the SSID, password and hostname. **Keep WiFi now clears your MQTT
  settings**, which it previously left untouched.
- **The Logs tab shows the most recent entries, not the oldest.** It replayed the first 50 of the
  100-entry buffer, so on a device that had been running a while you got the boot sequence and
  nothing since. Replayed lines also took the time they arrived in the browser rather than the time
  they were logged, which collapsed a long backlog onto a single second and made it read as a
  reboot that never happened.

## Upgrading to 2.8.0-BETA8
- **Force charge now sends the CAN bit inverters actually act on.** It set *request full charge*
  (0x35C bit 3) rather than *force charge* (bit 4), so on an inverter that reads those flags it
  asked for a charge to be finished rather than started, and force charge appeared to do nothing —
  reported on an EG4 6000XP. If you worked around this with the SOC trick, nothing changes: that
  path never used these bits, and you can now turn it off if you would rather not misreport SOC.
- **Force Charge is now a switch on the dashboard**, alongside Charge and Discharge. It follows the
  device rather than the click, so a force charge set or cleared over MQTT, by the schedule or by
  the temperature cut shows up on the dashboard within a second.
- **Request Full Charge is now its own control** — a switch on the BMS tab, a Home Assistant
  switch, `<topic>/set/RequestFullCharge`, and `{"requestfullcharge": true}` on the WebSocket. Use
  it to let a pack rebalance and reset SOC; it clears itself once that charge completes.
- Both flags only reach the inverter on **Pylontech 1.2**, **Pylontech 1.3** or **Growatt** with
  **Request Flags** enabled — on 1.3 that setting is also what puts 0x35C on the bus, since the 1.3
  spec drops the message. The dashboard now says so rather than leaving you waiting on a charge that
  is not coming.

## Upgrading to 2.8.0-BETA6
- **Charge, discharge and force charge now hold for 5 minutes once set from outside the schedule.**
  Previously the scheduler took them straight back on its next pass, within a second. That affects
  the Dashboard toggles and Home Assistant as well as PowerPilot: a toggle now stays where you put
  it, then reverts on its own. If you would rather the schedule always won, set the override
  timeout to `0` under Schedule → Outside Control.

## Upgrading to 2.8.0-BETA5
A few things changed behaviour in 2.8.0-BETA5, so worth knowing before you flash:

- **Settings are now entered in V, A and Ah.** Your stored values are untouched and are converted for display, so a charge voltage that read 55200 now reads 55.2. Battery Capacity was previously labelled mA, which was wrong - it is amp hours.
- **Battery Power now reports the correct sign over MQTT.** Discharge was being published as a positive number. If you built Home Assistant automations or energy dashboards that worked around this, they will need adjusting.
- **Time To Go now shows blank while charging** instead of a bogus "1 minute". Same root cause as above.
- **If your NTP server never worked, it will now.** A single server address was being passed to the clock as an empty string, so it silently never synced - only two comma-separated servers ever worked. The log claimed success regardless, which is why it looked fine. If you gave up on NTP previously, it is worth setting again.
- **Log and syslog timestamps are now local time, not UTC.** Set your timezone under Settings. Existing devices default to Europe/London. If you have log processing that assumed UTC, it will need adjusting.
- **Settings backups from before this version still import correctly** - the file records its own version and older files are converted on the way in.

## Features to come:

- MQTT to CAN BUS support, use esphome BMS intgrations to feed the data in and send to the inverter.
- Multi Inverter support
- OneWire temperature sensors for charge control
- Heater control (fan control and temperature monitoring are now implemented)

## Limitations
- DiyBatteryBMS is only listening to messages of the VE.Direct device<br>It understands only the "ASCII" part of the protocol that is only good to receive a set of values. You can't request any special data or change any parameters of the VE.Direct device.<br>

## Hardware & Software Installation
See the Wiki page

## Hardware Recommended
esp32dev, esp32plus, lilygo CAN485, ESP32-S3 with built-in CAN (TWAI) or without using MCP2515.

Please use the recommended hardware, as a personal project it's difficult to support other configurations.

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

- **Flash sizes**: `-4mb`, `-8mb`, `-16mb`. All three tables keep NVS at the same offset, so moving a
  board between them keeps your settings.
- **PSRAM**: add `-psram`. It is in the name because PSRAM is a property of the module, not the
  wiring — a board without it builds from a `-psram` env and then quietly runs on internal RAM only.
  The S3 `-psram` builds are for the R8 (octal) modules; an R2 part is quad and needs
  `board_build.arduino.memory_type = qio_qspi` instead.
- **No PSRAM variants** for `esp32c3-ESPCAN`, which has no external RAM interface at all, or for
  `xiao-esp32s3`, which has 8MB of flash and 8MB of PSRAM soldered in package.

The sections below give the pins for each wiring; they apply to every flash and PSRAM variant of it.

## Recommended PIN Configuration

### esp32dev-* Environments
- **CAN_BUS_CS_PIN**: 2
- **CAN0_INT**: 22
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32 (optional)

### esp32plus-* Environments
- **CAN_BUS_CS_PIN**: 5
- **CAN0_INT**: 13
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32 (optional)

### esp32-ESPCAN-* Environments (Built-in CAN)
- **CAN_TX_PIN**: 27
- **CAN_RX_PIN**: 26
- **CAN_EN_PIN**: 23
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32 (optional)

### esp32s3-ESPCAN-* Environments (Built-in CAN)
- **CAN_TX_PIN**: 27
- **CAN_RX_PIN**: 26
- **CAN_EN_PIN**: 23
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32 (optional)

### esp32c3-ESPCAN-* Environments (Built-in CAN)
- **CAN_TX_PIN**: 6
- **CAN_RX_PIN**: 7
- **CAN_EN_PIN**: 5
- **VEDIRECT_RX**: 21
- **VEDIRECT_TX**: 20 (optional)

**Note**: These PINs can be configured through the web interface after flashing. The forbidden GPIO lists for each environment prevent selection of pins that should not be used.

**VEDIRECT_TX is optional.** Nothing is ever sent to the shunt, so only VEDIRECT_RX has to be wired and set. Leave the TX pin blank in the web interface to keep that GPIO free; clearing an existing value unassigns it.

Links:

esp32dev
https://www.amazon.co.uk/gp/product/B0C9THDPXP/

esp32plus
https://www.amazon.co.uk/dp/B0BHZ8H6LM

Lilygo CAN485
https://www.aliexpress.com/item/1005003624034092.html

LCD
https://www.amazon.co.uk/dp/B07V5K3ZVB

Isolated CAN Bus Adapter
https://www.amazon.co.uk/Coolwell-Isolated-Expansion-Raspberry-SN65HVD230/dp/B0C7VX6G6P

Non-Isolated Adapter - Does work and cheaper but can be blown if not careful.
https://www.amazon.co.uk/AZDelivery-MCP2515-Receiver-Development-Compatible/dp/B086TXSFD8/

Victron Smart Shunt
https://www.amazon.co.uk/Victron-Energy-SmartShunt-Battery-Bluetooth/dp/B0856PHNLX

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


