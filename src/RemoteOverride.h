#pragma once
#include <Arduino.h>

/*
   Copyright (c) 2022-2026 Nexion Software Solutions Ltd - https://nexion.uk

   Remote override latch.

   The scheduler re-asserts its decision once a second, so anything that writes
   ManualAllowCharge / ManualAllowDischarge / ForceCharge from outside - the web
   UI toggles, MQTT, or a supervisor such as PowerPilot on the WebSocket - was
   previously undone within a second whenever no window was active.

   Touching a lever now latches it: the scheduler leaves that one lever alone
   until the latch times out, then takes it back. Per lever rather than global,
   so a controller asserting force charge does not also stop the schedule
   managing discharge.

   The timeout is the point of the design. A supervisor that crashes, loses the
   network or is simply switched off stops refreshing its latch, and the device
   falls back to the schedule the operator configured locally instead of sitting
   indefinitely on whatever that controller last commanded. Refresh well inside
   the timeout - this is a watchdog, not a lease to renew at the last moment.

   Timeout 0 disables the latch entirely: the scheduler always wins, which is
   the behaviour from before this existed.

   That watchdog is wrong for a human setting a lever by hand - the dashboard's
   own Charge/Discharge toggles, and a Home Assistant switch on the
   ChargeEnable/DischargeEnable MQTT topics - who was reading "5 to 7 minutes
   after I turn discharge off, it turns itself back on" as a bug, not a
   watchdog doing its job. Those callers Arm() a lever indefinitely instead:
   it holds until Clear()ed or set again, with no timeout to outlive. A
   supervisor steering a lever continuously (PowerPilot, or anything else on
   the generic WebSocket set-commands) still gets the timed latch, since that
   is exactly the crash/disconnect case the watchdog exists for.
*/

// Which scheduler-driven lever a latch covers
enum RemoteLever : uint8_t {
  OV_CHARGE    = 0,   // ManualAllowCharge
  OV_DISCHARGE = 1,   // ManualAllowDischarge
  OV_FORCE     = 2,   // ForceCharge
  OV_COUNT     = 3
};

#define OVERRIDE_TIMEOUT_MAX 3600

#ifndef DISABLE_SCHEDULER

class RemoteOverrideClass {
public:
  void SetTimeout(uint16_t secs) {
    _timeout = (secs > OVERRIDE_TIMEOUT_MAX) ? OVERRIDE_TIMEOUT_MAX : secs;
  }
  uint16_t GetTimeout() const { return _timeout; }

  /* Take, or refresh, one lever. indefinite is for a human directly setting the
     lever - the dashboard toggle, or a Home Assistant switch over MQTT - who
     means "hold this until I say otherwise", not "steer this continuously".
     It holds regardless of the configured watchdog timeout, including when
     that timeout is 0, since 0 only says "an unattended supervisor should
     never be trusted to hold a lever", not "a person can't". */
  void Arm(RemoteLever lever, bool indefinite = false) {
    if (lever >= OV_COUNT) return;
    if (!indefinite && _timeout == 0) return;
    _armed[lever]      = true;
    _armedMs[lever]    = millis();
    _indefinite[lever] = indefinite;
  }

  /* Worked out from the timestamp rather than from a flag some sweep has to
     clear, so the answer stays right even when nothing has swept recently -
     Expired() stops being called while the clock is unset. */
  bool Active(RemoteLever lever) const {
    if (lever >= OV_COUNT || !_armed[lever]) return false;
    if (_indefinite[lever]) return true;
    if (_timeout == 0) return false;
    return (uint32_t)(millis() - _armedMs[lever]) < (uint32_t)_timeout * 1000UL;
  }

  bool Any() const {
    for (uint8_t i = 0; i < OV_COUNT; i++)
      if (Active((RemoteLever)i)) return true;
    return false;
  }

  // Seconds left on the longest-lived timed latch, 0 if nothing timed is held
  // (an indefinite hold has no countdown, so it is left out of this)
  uint16_t SecondsLeft() const {
    uint32_t best = 0;
    for (uint8_t i = 0; i < OV_COUNT; i++) {
      if (!Active((RemoteLever)i) || _indefinite[i]) continue;   // guarantees the subtraction below is in range
      uint32_t left = (uint32_t)_timeout * 1000UL - (millis() - _armedMs[i]);
      if (left > best) best = left;
    }
    return (uint16_t)((best + 999) / 1000);
  }

  // Hand every lever back to the scheduler now
  void Clear() {
    for (uint8_t i = 0; i < OV_COUNT; i++) { _armed[i] = false; _indefinite[i] = false; }
  }

  /* Drop timed-out latches, returning a bitmask of the ones that went on this
     call, so the caller can log and publish exactly once rather than on every
     pass. */
  uint8_t Expired() {
    uint8_t went = 0;
    for (uint8_t i = 0; i < OV_COUNT; i++) {
      if (_armed[i] && !Active((RemoteLever)i)) {
        _armed[i] = false;
        went |= (uint8_t)(1 << i);
      }
    }
    return went;
  }

  static const char* Name(RemoteLever lever) {
    switch (lever) {
      case OV_CHARGE:    return "allow charge";
      case OV_DISCHARGE: return "allow discharge";
      case OV_FORCE:     return "force charge";
      default:           return "?";
    }
  }

private:
  bool     _armed[OV_COUNT]      = { false, false, false };
  bool     _indefinite[OV_COUNT] = { false, false, false };
  uint32_t _armedMs[OV_COUNT]    = { 0, 0, 0 };
  uint16_t _timeout = 0;      // seconds, 0 = latch disabled
};

#else   // no scheduler compiled in - nothing re-asserts, so there is nothing to hold off

class RemoteOverrideClass {
public:
  void SetTimeout(uint16_t) {}
  uint16_t GetTimeout() const { return 0; }
  void Arm(RemoteLever, bool indefinite = false) {}
  bool Active(RemoteLever) const { return false; }
  bool Any() const { return false; }
  uint16_t SecondsLeft() const { return 0; }
  void Clear() {}
  uint8_t Expired() { return 0; }
  static const char* Name(RemoteLever) { return "?"; }
};

#endif  // DISABLE_SCHEDULER

extern RemoteOverrideClass RemoteOverride;
