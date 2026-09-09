#pragma once
#include <Arduino.h>

/*
   Copyright (c) 2022-2026 Nexion Software Solutions Ltd - https://nexion.uk

   Remote override latch.

   The scheduler re-asserts its decision once a second, so anything that writes
   ManualAllowCharge / ManualAllowDischarge / ForceCharge from outside was
   previously undone within a second whenever no window was active.

   Touching a lever now latches it: the scheduler leaves that one lever alone
   until it is given back. Per lever rather than global, so a controller
   asserting force charge does not also stop the schedule managing discharge.

   Two kinds of hold, because "outside" covers two different things:

   - Arm(): a one-off toggle from the dashboard or an MQTT/Home Assistant
     switch. Nothing is watching to refresh it, so it holds indefinitely -
     only a fresh command on the same lever or ClearOverride/Clear() gives it
     back to the schedule. This is what "the toggle silently flipped back
     after 5-7 minutes" was: a one-off write was being held to the same
     timeout meant for a live controller, so it expired with nobody there to
     renew it.

   - ArmTimed(): a continuous supervisor such as PowerPilot, which is expected
     to keep asserting its decision. GetTimeout() seconds of silence hands the
     lever back, so a supervisor that crashes, loses the network or is simply
     switched off cannot leave the device stuck on its last command
     indefinitely. Refresh well inside the timeout - this is a watchdog, not a
     lease to renew at the last moment. Timeout 0 disables it entirely: a
     supervisor can never take the lever, which is the behaviour from before
     this existed.

   Timeout 0 only affects ArmTimed() - it has no bearing on indefinite holds
   taken with Arm().
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

  // One-off toggle (dashboard, MQTT/Home Assistant): take a lever
  // indefinitely. Nothing is expected to refresh it.
  void Arm(RemoteLever lever) {
    if (lever >= OV_COUNT) return;
    _armed[lever] = true;
    _timed[lever] = false;
  }

  // Continuous supervisor (e.g. PowerPilot): take, or refresh, a lever on a
  // watchdog. GetTimeout() seconds without a refresh hands it back.
  void ArmTimed(RemoteLever lever) {
    if (_timeout == 0 || lever >= OV_COUNT) return;
    _armed[lever]   = true;
    _timed[lever]   = true;
    _armedMs[lever] = millis();
  }

  /* Timed levers are worked out from the timestamp rather than from a flag
     some sweep has to clear, so the answer stays right even when nothing has
     swept recently - Expired() stops being called while the clock is unset.
     Indefinite levers have no timestamp to judge - they are active for as
     long as they are armed, full stop. */
  bool Active(RemoteLever lever) const {
    if (lever >= OV_COUNT || !_armed[lever]) return false;
    if (!_timed[lever]) return true;
    if (_timeout == 0) return false;
    return (uint32_t)(millis() - _armedMs[lever]) < (uint32_t)_timeout * 1000UL;
  }

  bool Any() const {
    for (uint8_t i = 0; i < OV_COUNT; i++)
      if (Active((RemoteLever)i)) return true;
    return false;
  }

  // Seconds left on the longest-lived timed latch, 0 if nothing is held on a
  // watchdog (including when every held lever is an indefinite hold).
  uint16_t SecondsLeft() const {
    uint32_t best = 0;
    for (uint8_t i = 0; i < OV_COUNT; i++) {
      if (!_timed[i] || !Active((RemoteLever)i)) continue;   // guarantees the subtraction below is in range
      uint32_t left = (uint32_t)_timeout * 1000UL - (millis() - _armedMs[i]);
      if (left > best) best = left;
    }
    return (uint16_t)((best + 999) / 1000);
  }

  // Hand every lever back to the scheduler now
  void Clear() { for (uint8_t i = 0; i < OV_COUNT; i++) _armed[i] = false; }

  /* Drop timed-out watchdog latches, returning a bitmask of the ones that
     went on this call, so the caller can log and publish exactly once rather
     than on every pass. Indefinite holds never expire on their own. */
  uint8_t Expired() {
    uint8_t went = 0;
    for (uint8_t i = 0; i < OV_COUNT; i++) {
      if (_armed[i] && _timed[i] && !Active((RemoteLever)i)) {
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
  bool     _armed[OV_COUNT]   = { false, false, false };
  bool     _timed[OV_COUNT]   = { false, false, false };
  uint32_t _armedMs[OV_COUNT] = { 0, 0, 0 };
  uint16_t _timeout = 0;      // seconds, 0 = watchdog (ArmTimed) disabled
};

#else   // no scheduler compiled in - nothing re-asserts, so there is nothing to hold off

class RemoteOverrideClass {
public:
  void SetTimeout(uint16_t) {}
  uint16_t GetTimeout() const { return 0; }
  void Arm(RemoteLever) {}
  void ArmTimed(RemoteLever) {}
  bool Active(RemoteLever) const { return false; }
  bool Any() const { return false; }
  uint16_t SecondsLeft() const { return 0; }
  void Clear() {}
  uint8_t Expired() { return 0; }
  static const char* Name(RemoteLever) { return "?"; }
};

#endif  // DISABLE_SCHEDULER

extern RemoteOverrideClass RemoteOverride;
