# PowerPilot Integration (WebSocket API)

*New in 2.8.0-BETA6.*

PowerPilot is a home energy manager that plans battery charge and discharge cycles against solar
forecasts and tariff prices. It can use a DiyBatteryBMS device as a networked hardware interface:
because the node already reads a Victron Smart Shunt over VE.Direct and speaks Pylontech to an
inverter over CAN, pointing PowerPilot at one gives it battery telemetry, inverter control, or both,
without wiring a CAN HAT or a VE.Direct cable to the Pi.

Add the node under **Settings → Hardware Comms Devices** in PowerPilot, by hostname
(e.g. `diy-batterybms.local`) or IP. Nothing needs enabling on this side — a node on the network is
ready.

The interface it uses is the web UI's own WebSocket at `ws://<device>/ws`, which is open to any
system, not just PowerPilot. It pushes the full state as JSON on every loop and accepts JSON
set-commands using the same keys, so any external energy manager can do the same thing.

> **There is no authentication on this interface.** Anything on the same network can change your
> charge limits. Keep the device off guest and untrusted networks — that applies to PowerPilot
> reaching the device as much as to anything else.

## Contents

- [Steering the device continuously](#steering-the-device-continuously) — `persist`, `forcecharge`,
  the override latch
- [Force charge vs full charge](#force-charge-vs-full-charge) — two different asks, and how to tell
  whether either is reaching the inverter
- [Live current requests](#live-current-requests) — asking for a current limit without rewriting the
  commissioned one

## Steering the device continuously

Three additions in 2.8.0-BETA6 make the interface practical for a supervisor that is steering
continuously rather than clicking a setting occasionally:

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

- **`forcecharge`** — force charge can be set over the WebSocket as well as MQTT:

  ```json
  {"forcecharge": true}
  ```

  It is RAM-only in the firmware either way, so there is nothing to persist. See
  [Force charge vs full charge](#force-charge-vs-full-charge) for what it does on the wire and when
  it will not reach the inverter at all.

- **The override latch** — the charge scheduler re-asserts its decision once a second, so without
  this anything an outside system set would be undone within the second. Setting `forcecharge`,
  `manualallowcharge` or `manualallowdischarge` takes that *one* lever off the scheduler until
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

## Force charge vs full charge

*`requestfullcharge`, `requestflagsactive` and `soctrickactive` are new in 2.8.0-BETA8.*

Both are carried in **CAN message 0x35C byte 0**, and confusing them is easy because the names are
so close. They are not interchangeable, and an inverter silently ignores the one it was not asked
for:

| | `forcecharge` | `requestfullcharge` |
|---|---|---|
| CAN bit | 4 (*force charge II*) | 3 (*request full charge*) |
| Means | **Start charging the pack now** — pulling from the grid if that is what it takes | **When you next charge, take it to 100%** rather than stopping at your own SOC limit |
| Starts a charge? | Yes, that is the whole point | **No.** It only changes where an existing charge stops |
| Use it for | Off-peak grid charging, holding a pack up ahead of a forecast deficit | SOC calibration and cell rebalancing, on a pack normally cycled to 80–90% |
| Override latch | Yes — takes the force lever off the scheduler until the timeout (`ovrforce`) | No. It is not a scheduler lever, so nothing to latch |
| Persistence | RAM only, clears on reboot | RAM only, clears on reboot |
| Cleared by | You, the schedule, or the override timeout | **Itself**, once the charge completes — see below |
| Set it | `{"forcecharge": true}`, MQTT `<topic>/set/ForceCharge` | `{"requestfullcharge": true}`, MQTT `<topic>/set/RequestFullCharge` |
| Read it | `forcecharge` in the pushed JSON, MQTT `<topic>/Param/ForceCharge` | `requestfullcharge`, MQTT `<topic>/Param/RequestFullCharge` |

A supervisor wanting a monthly calibration charge should set **both**: `forcecharge` to make the
charge happen, `requestfullcharge` so it runs to the top rather than to the inverter's own ceiling.
For ordinary tariff-driven charging, `forcecharge` alone is right.

**`requestfullcharge` is a one-shot.** The device clears it when the charge it asked about finishes
— when absorption ends, on sustained tail current or on the max absorption timer — because a flag
left set would quietly send every later charge to 100% as well, which is the opposite of what
cycling a pack to 80–90% is for. A hardware BMS drops the bit the same way. A controller can
therefore treat `requestfullcharge` going `false` as **"the calibration charge you asked for is
done"**, and does not need to clear it itself. `forcecharge` is *not* one-shot: it holds until
something clears it or the override times out.

**Check `requestflagsactive` before trusting either.** The flag bits only go out when **Request
Flags** is enabled under Settings → Inverter Tricks, and only on the **Pylontech 1.2**, **Pylontech
1.3** and **Growatt** protocols. 1.2 and Growatt send 0x35C either way and fill the bits in when the
setting is on; 1.3 drops the message per spec, so enabling the setting is what adds it back. On SMA
or Victron the message is never sent. The device publishes a read-only flag saying whether the bits
are actually reaching the inverter:

| Key | Meaning |
|---|---|
| `requestflagsactive` | `true` when 0x35C is genuinely being sent — protocol carries it *and* Request Flags is on. When `false`, setting `forcecharge` or `requestfullcharge` changes nothing on the wire |
| `soctrickactive` | `true` when the SOC trick is compensating — SOC Trick is enabled *and* force charge is set, so the pack's SOC is reported at 1/10 of its real value to provoke a charge |

Those two are the whole picture for force charge: with `requestflagsactive` false and
`soctrickactive` false, force charge is a no-op and a controller should surface that rather than
wait for a charge that is never coming. With `requestflagsactive` false but `soctrickactive` true,
force charge still works, by lying about SOC rather than by asking. The web UI raises the same
warning, and the device logs one when force charge is set with neither lever available.

`requestfullcharge` has no such fallback — with `requestflagsactive` false it does nothing at all.

> **Before 2.8.0-BETA8, `forcecharge` set bit 3 instead of bit 4**, so on an inverter that reads the
> flags it asked for a full charge rather than starting one, and force charge appeared to do nothing.
> Reported against an EG4 6000XP. Installations that worked around it by relying on the SOC trick
> are unaffected — that path never involved these bits.

## Live current requests

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
