This is a hobby project and I add features as and when I have time, it means sometimes it's not updated for a while and then sometimes I add a number of features!

I now accept donations towards supporting the project: https://buymeacoffee.com/sijones2012

You can now sponsor and donate to me, the link is on here to buy me a coffee ;)

The new UI means no coding knowledge is required, just flash and set up.

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
  See [PowerPilot Integration](#powerpilot-integration-websocket-api) below.

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
- **Switches**: Charge Enable, Discharge Enable, Force Charge, SOC Trick Enable, Request Flags Enable
- **Number Controls**: Charge Voltage, Charge Current (with 0.1 precision), Float Voltage, Float Current
- **New in 2.8.0-BETA5 — Charging detail**: SOC Sent To Inverter, SOC Override Reason, SOC Override Active, Absorption Elapsed, Absorption Remaining, Tail Current Held, Tail Current Remaining, Tail Current Active
- All entities are grouped under one "DIY Battery BMS" device
- No manual YAML configuration required (HomeAssistant.yaml is now optional for reference only)
- Discovery messages published automatically on MQTT connect

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
| Enable it | Settings → CC-CV Charging Configuration → **Float Voltage**. `0` disables the stage |
| Typical value | 3.375–3.40 V per cell — 54.0–54.4 V on a 16S LiFePO4 pack |
| **Float Current** | Charge allowance held during float, default 2 A. **Do not set this to zero** — that recreates the instruction the float stage exists to avoid |
| Leaving float | Same as Complete: SOC below Recharge SOC, or voltage below the float target minus Recharge V Offset |
| Over MQTT | `<topic>/set/FloatVoltage` (V) and `<topic>/set/FloatCurrent` (A), or the matching Home Assistant number entities |

The phase appears as **Float** on the dashboard and on the `ChargePhase` MQTT topic. While in float
the real 100% SOC is sent to the inverter rather than being held at 99%, because the pack genuinely
is full — telling an inverter "99%, and you may not charge" is the contradiction that provoked the
forced discharge in the first place.

**The stage is off by default**, including after an update, so an existing installation keeps
terminating its charge exactly as before until you configure a float voltage.

If you are seeing a forced discharge at 100% SOC and have not enabled float, also check **High SOC
(Stop Charge)** and the **Slow Charge** thresholds — both cut the charge current limit at an SOC
boundary while the voltage limit stays high, which produces the same symptom by a different route.

### PowerPilot Integration (WebSocket API)

*New in 2.8.0-BETA6.*

PowerPilot is a home energy manager that plans battery
charge and discharge cycles against solar forecasts and tariff prices. It can use a
DiyBatteryBMS device as a networked hardware interface: because the node already reads a Victron
Smart Shunt over VE.Direct and speaks Pylontech to an inverter over CAN, pointing PowerPilot at one
gives it battery telemetry, inverter control, or both, without wiring a CAN HAT or a VE.Direct cable
to the Pi.

Add the node under **Settings → Hardware Comms Devices** in PowerPilot, by hostname
(e.g. `diy-batterybms.local`) or IP. Nothing needs enabling on this side — a node on the network is
ready.

The interface it uses is the web UI's own WebSocket at `ws://<device>/ws`, which is open to any
system, not just PowerPilot. It pushes the full state as JSON on every loop and accepts JSON
set-commands using the same keys, so any external energy manager can do the same thing.

Three additions in 2.8.0-BETA6 make that practical for a supervisor that is steering continuously
rather than clicking a setting occasionally:

- **`"persist": false`** — add this to any set-command and the value applies immediately but is
  **not** written to NVS. Defaults to `true`, so the web UI and existing clients are unaffected.
  A supervisor adjusting charge current every few seconds should always use it: it keeps flash out
  of the control loop, and it means a reboot or a lost controller falls back to the limits *you*
  saved locally, rather than whatever some remote system last commanded.

  ```json
  {"maxchargecurrent": 40000, "persist": false}
  ```

  PowerPilot sends everything this way while it is running, and only uses `"persist": true` when it
  is deliberately (re)writing the node's saved fallback — the limits the node runs on by itself once
  PowerPilot is gone.

- **`forcecharge`** — force charge can now be set over the WebSocket as well as MQTT:

  ```json
  {"forcecharge": true}
  ```

  It is RAM-only in the firmware either way, so there is nothing to persist.

- **The override latch** — the charge scheduler re-asserts its decision once a second, so without
  this anything an outside system set would be undone within the second. Setting `forcecharge`,
  `manualallowcharge` or `manualallowdischarge` now takes that *one* lever off the scheduler until
  the override times out. Nothing else is affected: a controller holding force charge does not stop
  the schedule managing discharge.

  The timeout is the safety net, not an inconvenience. A supervisor that crashes, loses the network
  or is simply switched off stops refreshing its latch, and the device falls back to the schedule
  you configured locally instead of sitting indefinitely on the last thing it was told. **Re-send
  well inside the timeout** — treat it as a watchdog, not a lease to renew at the last moment.

  | | |
  |---|---|
  | Default | 300 seconds |
  | Set it | Schedule tab → Outside Control, or `{"overridetimeout": 300}` (accepts `persist`) |
  | Disable it | Set to `0` — the scheduler then always wins, as before |
  | Hand control back early | `{"clearoverride": true}`, or MQTT `<topic>/set/ClearOverride`, or the **Return to Schedule** button |
  | Read the state | `ovrcharge` / `ovrdischarge` / `ovrforce` / `ovrsecs` in the pushed JSON, and MQTT `<topic>/Schedule/Override` and `/Schedule/OverrideSecs` |

  A controller shutting down cleanly should send `clearoverride` rather than leaving the schedule
  waiting out the timeout. One thing a latch cannot hold off is safety: if protection disables
  charging, an externally forced charge is dropped regardless, exactly as a scheduled one would be.

### Live current requests

*New in 2.8.0-BETA7.*

`maxchargecurrent` and `maxdischargecurrent` are **commissioning settings** — validated against
Min Charge, persisted to NVS, meant to be set once. A supervisor rewriting them every 30 seconds
destroyed whatever the operator had configured, made a perfectly valid "0 A, stop charging now"
command appear as an invalid configuration, and could leave 0 A saved as the standing ceiling
once the controller stopped — after which the battery could not charge at all.

Use **`requestedchargecurrent`** and **`requesteddischargecurrent`** instead (milliamps, as with
every other current on this interface):

```json
{"manualallowcharge": true,  "requestedchargecurrent": 100000}
{"manualallowcharge": false, "requestedchargecurrent": 0}
```

| | |
|---|---|
| Effective limit | `min(request, configured max)` — a request can only ever ask for **less**, so a controller fault cannot exceed what was commissioned |
| Persistence | **Never.** No NVS write on any path, whatever `persist` says |
| Min Charge | Does not apply. `0` is a valid instruction, not a broken configuration |
| Expiry | Requests go stale after **Current Request Timeout** (default 120 s, Schedule → Outside Control) and the configured ceilings return |
| Cancel early | `{"clearcurrentrequests": true}` |
| Read back | `maxchargecurrent` (configured), `reqchargecurrent` (`-1` when nothing is live, distinct from a request of `0`), `effchargecurrent` (in force), plus `reqchargeage` in seconds — and the same four for discharge |

Unlike the override latch, this timeout **cannot be disabled**. There, expiry hands control back to
the local schedule, so switching it off fails safe; here the request is what is holding the pack
down, and one that never expires is exactly the failure the timeout exists to prevent. Values
below 30 s are clamped.

The BMS page shows the configured ceilings unchanged and, when a request is in force, a note
naming the requested value and how long ago it arrived. Operators previously saw their own
setting apparently changing by itself.

Setting `maxchargecurrent` still behaves exactly as before, so an existing controller keeps
working untouched until it is updated.

One caveat worth knowing: a request of `0` sets the charge current limit to zero while charging
remains *enabled*, and some hybrid inverters answer that combination by discharging the pack to
shed power. Send `manualallowcharge: false` alongside it to stop cleanly — the device logs a
warning if you do not.

Note that the device's own CC-CV logic recomputes its live charge current on each pass, so a
supervisor should steer the request or the max limits rather than setting `chargecurrent` directly.

> **There is no authentication on this interface.** Anything on the same network can change your
> charge limits. Keep the device off guest and untrusted networks — that applies to PowerPilot
> reaching the device as much as to anything else.

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

## Recommended PIN Configuration

### esp32dev Environment
- **CAN_BUS_CS_PIN**: 2
- **CAN0_INT**: 22
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32

### esp32plus Environment
- **CAN_BUS_CS_PIN**: 5
- **CAN0_INT**: 13
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32

### esp32-ESPCAN Environment (Built-in CAN)
- **CAN_TX_PIN**: 27
- **CAN_RX_PIN**: 26
- **CAN_EN_PIN**: 23
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32

### esp32s3-ESPCAN Environment (Built-in CAN)
- **CAN_TX_PIN**: 27
- **CAN_RX_PIN**: 26
- **CAN_EN_PIN**: 23
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32

### esp32c3-ESPCAN Environment (Built-in CAN)
- **CAN_TX_PIN**: 6
- **CAN_RX_PIN**: 7
- **CAN_EN_PIN**: 5
- **VEDIRECT_RX**: 21
- **VEDIRECT_TX**: 20

**Note**: These PINs can be configured through the web interface after flashing. The forbidden GPIO lists for each environment prevent selection of pins that should not be used.

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


