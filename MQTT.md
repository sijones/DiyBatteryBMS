# MQTT Interface

Everything the device publishes, and every command it accepts. Written against firmware
**2.8.0-BETA8**; the topic names have been stable since 2.8.0-BETA5, but the `/Data` fields grow
with most releases.

MQTT is configured under **WiFi/MQTT** in the web interface: server, port, optional username and
password, client ID (default `diy-battery`) and **base topic** (default `DIY-BATTERY`). Everything
below is relative to that base topic, written as `<topic>`. Nothing is published until a server
address and port are set.

> **There is no per-topic access control.** Anyone who can publish to `<topic>/set/#` on your broker
> can force-charge the pack, change the charge voltage or disable discharge. Treat broker
> credentials as control of the battery.

---

## Connection

| | |
|---|---|
| Subscribes to | `<topic>/set/#` at QoS 2, plus the two external temperature topics below |
| Birth message | `<topic>/status` = `online`, retained, on connect |
| Last will | `<topic>/status` = `offline`, retained, QoS 2 |
| Reconnect | every 10 s while WiFi is up |
| Payload buffer | 2048 bytes in either direction — the `/Data` JSON is the largest message and runs close to it |

On connect the device publishes its status, the full Home Assistant discovery set, and one round of
`/Param/*` values.

---

## What the device publishes

### Cadence

Telemetry goes out every **VE.Direct loop time** (Settings → Other, default **5 seconds**), or
immediately when a value the firmware tracks as significant changes — a toggled lever, a new charge
phase, a changed current limit. That means a force charge set over MQTT is reflected back on
`/Param/ForceCharge` within a second, not on the next 5-second tick.

> **Telemetry only flows while VE.Direct data is arriving.** If the Victron shunt is unplugged or
> silent for more than 2 seconds, the publish loop stops until it returns. `<topic>/status` stays
> `online` — the device is up, it just has nothing new to say. Watch `/Data` timestamps rather than
> `status` if you need a liveness check on the readings themselves.

`/Schedule/*` is not on that cycle. It is published only when something changes it: a schedule
ingested over MQTT, a window starting or ending, an override cleared or timed out. It is retained,
so a subscriber connecting later still sees the current picture.

### `<topic>/Data` — full state as JSON

The single most useful topic: one JSON object, published every cycle, not retained. It is the same
payload the web UI's WebSocket receives, so the field names match
[PowerPilot.md](PowerPilot.md) exactly.

**Watch the units.** Several fields carry raw protocol units rather than volts and amps, and one
field name is actively misleading — `battcurrent` is in 0.1 A steps despite the `mA` in the
underlying function name.

#### Battery

| Field | Unit | Notes |
|---|---|---|
| `battsoc` | % | Real pack SOC, before any trickery |
| `battvoltage` | 0.01 V | `× 0.01` for volts |
| `battcurrent` | 0.1 A | Signed. Positive charging, negative discharging |
| `battpower` | W | Signed |
| `batttemp` | °C | From the selected temperature source |
| `timetogo` | minutes | `65535` or negative means unknown |
| `alarmactive` | bool | VE.Direct alarm |
| `alarmreason` | string | Text of the above |

#### Charging state

| Field | Unit | Notes |
|---|---|---|
| `chargephase` | string | `Bulk`, `Absorption`, `Float`, `Complete` |
| `chargevoltage` | mV | Target charge voltage |
| `floatvoltage` / `floatcurrent` | mV / mA | Float stage; `0` voltage disables the stage |
| `chargeadjust` | mA | Absorption current trim the firmware is applying |
| `chargeenabled` / `dischargeenabled` | bool | Both the firmware's decision and the manual allow, combined |
| `forcecharge` | bool | See the force charge notes below |
| `requestfullcharge` | bool | One-shot; the device clears it when the charge completes |
| `requestflagsactive` | bool | Whether the above two are actually reaching the inverter |
| `soctrickactive` | bool | SOC is being misreported to provoke a charge |
| `autocharge` | bool | Smart charge enabled |
| `reportedsoc` | % | SOC actually sent over CAN — differs from `battsoc` when overridden |
| `socoverride` | enum | `0` none, `1` SOC trick, `2` holding 99%, `3` never 100% |

#### Current limits

Four related numbers, all in **mA**. A controller needs all of them to tell "my request was
accepted" from "my request was capped":

| Field | Meaning |
|---|---|
| `maxchargecurrent` / `maxdischargecurrent` | Configured ceilings |
| `reqchargecurrent` / `reqdischargecurrent` | What an external controller is asking for; `-1` means no live request |
| `reqchargeage` / `reqdischargeage` | Seconds since that request arrived |
| `effchargecurrent` / `effdischargecurrent` | What is actually in force — the minimum of the above |
| `chargecurrent` / `dischargecurrent` | The live setpoints going out on CAN |

#### Absorption and tail current

| Field | Unit | Notes |
|---|---|---|
| `absorbelapsed` / `absorbmax` | seconds | `absorbmax` of `0` means no time limit |
| `tailheld` / `tailneed` | seconds | How long tail current has held, and how long it must |
| `tailactive` | bool | Tail condition currently satisfied |
| `tailthreshold` | mA | Current below which the tail timer runs |
| `tailvoltok` | bool | Whether the voltage half of the tail condition is met |
| `tailvoltmin` | 0.01 V | The voltage it is being tested against |

#### Schedule and overrides

| Field | Notes |
|---|---|
| `schedactive` | A window is currently in force |
| `schedsource` | `mqtt`, `ui` or `none` |
| `schedmqttcount` / `scheduicount` | Windows loaded from each source |
| `schednext` | Epoch seconds of the next MQTT window start |
| `schednextin` / `schedendsin` | Seconds until that start / until the current window ends; `-1` if the clock is unset |
| `schedclock` | Whether the clock is valid at all — everything above is meaningless if false |
| `ovrcharge` / `ovrdischarge` / `ovrforce` | Which levers an external controller currently holds off the scheduler |
| `ovrsecs` | Seconds left on the longest-held latch |

#### Device and diagnostics

| Field | Notes |
|---|---|
| `pidstring`, `fwversion`, `serialnumber`, `modelstring` | Identity of the **Victron shunt**, not this device |
| `inverterpresent` | CAN traffic seen from the inverter recently |
| `victrondata` | VE.Direct data is arriving |
| `mqttconnected` | Self-evidently true if you are reading it |
| `cantotalfails` | Cumulative CAN send failures |
| `mqttinvertertemp` / `mqttbatttemp` | Temperatures taken from external MQTT topics; `-127` means not set |
| `fanpwm` | % |
| `totalheap` / `freeheap` | bytes |
| `ntpsyncago` | Seconds since the clock last synced. `-1` never synced, `-2` no NTP server configured |
| `showtempdashboard` | A web UI display preference, published for completeness |
| `RealTime` | Always `true` on this topic — distinguishes a live frame from the settings frame the web UI also receives |

### Flat value topics

The headline numbers again, one per topic, for brokers and dashboards that would rather not parse
JSON. Published on the same cycle, **not retained**.

| Topic | Unit |
|---|---|
| `<topic>/V` | 0.01 V |
| `<topic>/I` | 0.1 A, signed |
| `<topic>/SOC` | % |
| `<topic>/P` | W, signed |
| `<topic>/T` | °C |
| `<topic>/TTG` | minutes |
| `<topic>/Alarm` | `ON` / `OFF` |
| `<topic>/AR` | Alarm reason text |
| `<topic>/FanPWM` | % |
| `<topic>/InverterTemp` | °C — only published when an external inverter temperature is configured |
| `<topic>/MQTTBattTemp` | °C — likewise for the external battery temperature |
| `<topic>/Param/ChargePhase` | `Bulk` / `Absorption` / `Float` / `Complete` |

### `<topic>/Param/*` — switch states

**Retained**, so a restarting Home Assistant sees the current state immediately. Each one is the
state half of a switch whose command half is the matching `<topic>/set/*` topic.

| Topic | Payload |
|---|---|
| `<topic>/Param/ChargeEnable` | `ON` / `OFF` |
| `<topic>/Param/DischargeEnable` | `ON` / `OFF` |
| `<topic>/Param/ForceCharge` | `ON` / `OFF` |
| `<topic>/Param/RequestFullCharge` | `ON` / `OFF` |
| `<topic>/Param/SOCTrickEnable` | `ON` / `OFF` |
| `<topic>/Param/RequestFlagsEnable` | `ON` / `OFF` |
| `<topic>/Param/SmartCharge` | `ON` / `OFF` |

### `<topic>/Schedule/*` — what the scheduler is doing

**Retained.** Published on change only.

| Topic | Payload |
|---|---|
| `<topic>/Schedule/Active` | `ON` / `OFF` — a window is in force |
| `<topic>/Schedule/Source` | `mqtt`, `ui` or `none` |
| `<topic>/Schedule/Windows` | Count of windows currently loaded from MQTT |
| `<topic>/Schedule/ForceCharge` | `ON` / `OFF` — what the window asks for |
| `<topic>/Schedule/ChargeAllowed` | `ON` / `OFF` |
| `<topic>/Schedule/DischargeAllowed` | `ON` / `OFF` |
| `<topic>/Schedule/TargetSOC` | % — `0` means no target |
| `<topic>/Schedule/NextStart` | Epoch seconds of the next window start |
| `<topic>/Schedule/Override` | `ON` when an external controller holds any lever off the scheduler |
| `<topic>/Schedule/OverrideSecs` | Seconds left on that latch |

The first seven say what the schedule *wants*. The last two say whether it is actually in charge —
a window can be active while a controller holds the levers.

### Home Assistant discovery

Published retained under `homeassistant/<component>/diybatterybms_<MAC>_<id>/config` on every
connect, grouping everything under one **DIY Battery BMS** device. It covers 23 sensors, 7 binary
sensors, 7 switches and 5 number controls; the switches and numbers wire themselves to the
`<topic>/set/*` commands below, so anything you can do from Home Assistant you can also do by hand.

---

## Commands the device accepts

All under `<topic>/set/`. Boolean commands take the literal string `ON`; **any other payload is
treated as off**, including `on`, `true` and `1`. Numeric commands take a plain decimal string.
Nothing is echoed back on the command topic — watch the matching `/Param/*` or `/Data` field to
confirm the device took it.

### Levers

| Command | Payload | Effect |
|---|---|---|
| `<topic>/set/ChargeEnable` | `ON` / other | Manual allow-charge. Latches the lever against the scheduler |
| `<topic>/set/DischargeEnable` | `ON` / other | Manual allow-discharge. Latches |
| `<topic>/set/ForceCharge` | `ON` / other | Ask the inverter to charge now. Latches |
| `<topic>/set/RequestFullCharge` | `ON` / other | Take the next charge to 100% for calibration. Does not start a charge, and does not latch |
| `<topic>/set/ClearOverride` | any | Drop all latches and hand control straight back to the schedule |

**The latch.** Charge, discharge and force charge each hold their lever against the scheduler for
the override timeout (Settings, default 5 minutes) so the next scheduler pass does not immediately
undo what you asked for. `ClearOverride` ends it early; `<topic>/Schedule/OverrideSecs` counts it
down. A latch cannot hold off safety: if temperature protection disables charging, force charge is
dropped regardless.

### Currents and voltages

| Command | Payload | Persisted | Notes |
|---|---|---|---|
| `<topic>/set/ChargeVoltage` | volts, e.g. `55.2` | No | Ignored if ≤ 0 |
| `<topic>/set/ChargeCurrent` | amps, e.g. `50` | No | Live setpoint; smart charge may move it again |
| `<topic>/set/DischargeCurrent` | **milliamps**, e.g. `50000` | No | See the unit warning below |
| `<topic>/set/MaxDischargeCurrent` | amps | No | Runtime ceiling; ignored if ≤ 0 |
| `<topic>/set/TailCurrent` | amps | Yes | Threshold that ends absorption |
| `<topic>/set/RechargeSOC` | % | Yes | SOC at which a finished charge restarts |
| `<topic>/set/FloatVoltage` | volts | Yes | `0` disables the float stage |
| `<topic>/set/FloatCurrent` | amps | Yes | |

> **`DischargeCurrent` takes milliamps while every other current command takes amps.** Publishing
> `50` sets 50 mA, not 50 A, and the inverter will be told to stop discharging almost entirely. Use
> `MaxDischargeCurrent` (amps) unless you specifically want the live setpoint.

### Settings

| Command | Payload | Persisted | Notes |
|---|---|---|---|
| `<topic>/set/SOCTrickEnable` | `ON` / other | **No** | Reverts to the saved value on reboot |
| `<topic>/set/RequestFlagsEnable` | `ON` / other | **No** | Likewise |
| `<topic>/set/SmartCharge` | `ON` / other | **No** | Likewise. Echoes to `/Param/SmartCharge` immediately |

> These three are saved to flash when changed in the web interface, but **not** when changed over
> MQTT — the MQTT path only moves the value in RAM. Set them over MQTT for a session; set them in
> the web UI to make them stick.

### `<topic>/set/Schedule`

A JSON plan of absolute-time windows, up to 16. Publish it **retained** and the broker will replay
it after a device reboot, so the plan survives without any flash writes.

Four shapes are accepted:

```jsonc
{"from":"2026-08-14T01:30","to":"2026-08-14T05:30","forcecharge":true,"targetsoc":80}
[ {...}, {...} ]                    // several windows
{"windows":[ {...} ]}               // wrapped array
""   |   "clear"   |   "[]"   |   "{}"      // clear the MQTT schedule
```

Per-window keys:

| Key | Type | Notes |
|---|---|---|
| `from`, `to` | string | Required. `YYYY-MM-DDTHH:MM[:SS]` in **local time**, or epoch seconds as a bare number. A window with `to` ≤ `from` is dropped |
| `charge` | bool | Omit to leave the lever alone during the window |
| `discharge` | bool | Omit to leave alone |
| `forcecharge` | bool | Omit to leave alone |
| `targetsoc` | 0-100 | Stop charging at this SOC. `0` means no target |

Windows are validated individually — bad ones are skipped and the rest accepted, with a count
logged to the Logs tab. Expired windows are pruned automatically, so there is no staleness timeout
to manage. This replaces the whole MQTT schedule each time; it does not merge with what is already
loaded, and it does not touch the repeating windows configured in the web UI.

### External temperature topics

Not under `<topic>`. Configure these under Settings and the device subscribes to them directly, to
take battery or inverter temperature from any existing sensor on your broker. Payload is a plain
number in °C; it is rounded to a whole degree. Each is only used if the matching temperature
**source** is also set to MQTT.

---

## Force charge over MQTT

Force charge reaches the inverter in **CAN message 0x35C**, which is only transmitted on the
**Pylontech 1.2**, **Pylontech 1.3** and **Growatt** protocols, and only with **Request Flags**
enabled under Settings → Inverter Tricks. On 1.2 and Growatt the message is always sent and the
setting fills in the flag bits; on 1.3 the setting is what puts the message on the bus at all,
since the 1.3 spec drops it.

**Check `requestflagsactive` in `/Data` before trusting `<topic>/set/ForceCharge`.** When it is
`false`, setting force charge changes nothing on the wire unless `soctrickactive` is `true` — the
SOC trick provokes a charge by misreporting SOC instead, and works on any protocol.

`forcecharge` holds until something clears it. `requestfullcharge` is a one-shot and the device
clears it when the charge it asked about finishes, so a controller can treat
`/Param/RequestFullCharge` going `OFF` as "the calibration charge you asked for is done".

---

## See also

- [PowerPilot.md](PowerPilot.md) — the WebSocket API, which carries the same `/Data` fields plus
  live current requests and set-commands MQTT does not expose
- [README.md](README.md) — Home Assistant entity list and setup
