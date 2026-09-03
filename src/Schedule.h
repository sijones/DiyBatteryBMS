#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

/* Compiled out on space-constrained targets via -DDISABLE_SCHEDULER.

   No env defines it, and none ever has - the escape hatch exists but has never
   been taken, so every build ships with scheduling. This used to say the C3
   shipped without it, which was wrong twice over: that flag was never set on
   the C3 either, and the C3 is no longer a target at all (see platformio.ini's
   header). The tightest build now is the classic ESP32 at 72.1% of a 4MB app
   slot, which has room. */
#ifndef DISABLE_SCHEDULER

/*
   Copyright (c) 2022-2026 Nexion Software Solutions Ltd - https://nexion.uk

   Charge scheduler.

   Precedence, highest first:
     1. Safety   - temperature, SOC limits, over-voltage. Always wins.
     2. Schedule - whichever window is active.
     3. Default  - charge on, discharge on, force charge off.

   Safety is enforced structurally rather than by convention: the scheduler only
   ever writes ManualAllowCharge / ManualAllowDischarge, and the CAN layer emits
   (_chargeEnabled && _ManualAllowCharge). _chargeEnabled is owned by the
   protection logic, so a schedule can withhold charging but can never grant it
   past a safety cut-off. Force charge is additionally gated on the same flag,
   since asserting "charge at maximum" while protection has disabled charging
   would be contradictory.

   Two sources:
     UI   - repeating windows, 5 minute steps, weekday/weekend/daily. Persisted.
     MQTT - absolute date+time windows. RAM only, never written to flash.

   MQTT entries are deliberately not persisted. The schedule belongs to whatever
   computed it (typically Home Assistant from tariff prices); persisting it would
   mean resuming a stale plan after a reboot while the source had moved on. HA
   should publish retained, so the broker replays it on reconnect at no cost in
   flash writes. Because MQTT windows are absolute they also expire on their own,
   so there is no staleness timeout to tune.
*/

#define SCHED_MAX_UI    8
#define SCHED_MAX_MQTT  16

// Day bits for repeating entries
#define SCHED_SUN 0x01
#define SCHED_MON 0x02
#define SCHED_TUE 0x04
#define SCHED_WED 0x08
#define SCHED_THU 0x10
#define SCHED_FRI 0x20
#define SCHED_SAT 0x40
#define SCHED_WEEKDAYS (SCHED_MON|SCHED_TUE|SCHED_WED|SCHED_THU|SCHED_FRI)
#define SCHED_WEEKENDS (SCHED_SAT|SCHED_SUN)
#define SCHED_EVERYDAY (SCHED_WEEKDAYS|SCHED_WEEKENDS)

// -1 = leave alone, 0 = off, 1 = on
struct SchedWindow {
  uint16_t startMin = 0;      // repeating: minutes since local midnight
  uint16_t endMin   = 0;
  uint8_t  days     = 0;      // repeating: day bitmask. 0 for absolute entries
  time_t   from     = 0;      // absolute: epoch. 0 for repeating entries
  time_t   to       = 0;
  int8_t   charge    = -1;
  int8_t   discharge = -1;
  int8_t   force     = -1;
  uint8_t  targetSOC = 0;     // 0 = no target, force charge runs the whole window
};

struct SchedDecision {
  bool charge    = true;      // the defaults
  bool discharge = true;
  bool force     = false;
  bool active    = false;     // is any window currently in effect
  bool fromMqtt  = false;     // which source won
  uint8_t targetSOC = 0;
};

class ChargeSchedule {
public:
  // ---- Evaluation -------------------------------------------------------

  // battSOC and safetyAllowsCharge come from the caller so this stays testable
  // and has no dependency on CANBUS.
  SchedDecision evaluate(time_t nowEpoch, uint8_t battSOC, bool safetyAllowsCharge) {
    SchedDecision d;
    if (nowEpoch <= 0) return d;   // clock not set - stay on defaults

    struct tm lt;
    localtime_r(&nowEpoch, &lt);
    uint16_t nowMin = (uint16_t)(lt.tm_hour * 60 + lt.tm_min);
    uint8_t  dayBit = (uint8_t)(1 << lt.tm_wday);   // tm_wday 0 = Sunday

    // MQTT wins outright when it has an applicable window - one source at a
    // time, rather than merging two schedules into something hard to reason about.
    const SchedWindow* win = findAbsolute(nowEpoch);
    if (win) d.fromMqtt = true;
    else     win = findRepeating(nowMin, dayBit);

    if (!win) return d;            // nothing active - defaults

    d.active = true;
    if (win->charge    >= 0) d.charge    = (win->charge == 1);
    if (win->discharge >= 0) d.discharge = (win->discharge == 1);
    if (win->force     >= 0) d.force     = (win->force == 1);
    d.targetSOC = win->targetSOC;

    // Stop forcing once the target is reached, so an over-long window does not
    // keep pushing a pack that is already where it was asked to be.
    if (d.force && win->targetSOC > 0 && battSOC >= win->targetSOC) d.force = false;

    // Never assert force charge when protection has disabled charging.
    if (!safetyAllowsCharge) d.force = false;

    return d;
  }

  // ---- MQTT (RAM only) --------------------------------------------------

  // Returns the number of windows accepted, or -1 on parse failure.
  int setFromJson(JsonArrayConst arr, String &errOut) {
    int n = 0, skipped = 0;
    for (JsonObjectConst o : arr) {
      if (n >= SCHED_MAX_MQTT) { skipped++; continue; }
      SchedWindow w;
      w.from = parseEpoch(o["from"] | "");
      w.to   = parseEpoch(o["to"]   | "");
      if (w.from == 0 || w.to == 0 || w.to <= w.from) { skipped++; continue; }
      applyActions(o, w);
      _mqtt[n++] = w;
    }
    _mqttCount = n;
    _mqttSetMs = millis();
    if (skipped > 0) errOut = String(skipped) + " window(s) ignored (bad times, or over the " +
                              String(SCHED_MAX_MQTT) + " limit)";
    return n;
  }

  void clearMqtt() { _mqttCount = 0; }
  uint8_t mqttCount() const { return _mqttCount; }

  /*
     Ingest a raw MQTT payload. Accepts whatever an HA automation is likely to
     produce, because requiring one exact shape just generates support questions:

       {"from":"...","to":"...","forcecharge":true,"targetsoc":80}   single window
       [ {...}, {...} ]                                              several windows
       {"windows":[ {...} ]}                                         wrapped array
       ""  /  "clear"  /  "[]"                                       clear the schedule

     Returns windows accepted, or -1 if the payload could not be understood.
     errOut carries something worth logging either way.
  */
  int ingest(const char* payload, String &errOut) {
    if (!payload) { errOut = "empty payload"; clearMqtt(); return 0; }

    String p(payload); p.trim();
    if (p.length() == 0 || p.equalsIgnoreCase("clear") || p == "[]" || p == "{}") {
      clearMqtt();
      errOut = "schedule cleared";
      return 0;
    }

    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, p);
    if (e) { errOut = String("not valid JSON: ") + e.c_str(); return -1; }

    if (doc.is<JsonArray>())              return setFromJson(doc.as<JsonArrayConst>(), errOut);
    if (doc["windows"].is<JsonArray>())   return setFromJson(doc["windows"].as<JsonArrayConst>(), errOut);
    if (doc.is<JsonObject>()) {
      // A single window - wrap it so there is one code path
      JsonDocument tmp;
      tmp.to<JsonArray>().add(doc.as<JsonObjectConst>());
      return setFromJson(tmp.as<JsonArrayConst>(), errOut);
    }
    errOut = "expected an object or an array of windows";
    return -1;
  }

  // Drop windows whose end time has passed. Absolute windows expire naturally,
  // which is why no staleness timeout is needed.
  int pruneExpired(time_t nowEpoch) {
    if (nowEpoch <= 0) return 0;
    int removed = 0;
    for (int i = 0; i < _mqttCount; ) {
      if (_mqtt[i].to <= nowEpoch) {
        for (int j = i; j < _mqttCount - 1; j++) _mqtt[j] = _mqtt[j + 1];
        _mqttCount--; removed++;
      } else i++;
    }
    return removed;
  }

  // ---- UI (persisted) ---------------------------------------------------

  int setUiFromJson(JsonArrayConst arr) {
    int n = 0;
    for (JsonObjectConst o : arr) {
      if (n >= SCHED_MAX_UI) break;
      SchedWindow w;
      w.startMin = (uint16_t)(o["start"] | 0);
      w.endMin   = (uint16_t)(o["end"]   | 0);
      w.days     = (uint8_t)(o["days"]   | 0);
      if (w.days == 0 || w.startMin == w.endMin) continue;
      w.startMin = roundTo5(w.startMin);
      w.endMin   = roundTo5(w.endMin);
      applyActions(o, w);
      _ui[n++] = w;
    }
    _uiCount = n;
    return n;
  }

  void uiToJson(JsonArray arr) const {
    for (int i = 0; i < _uiCount; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["start"] = _ui[i].startMin;
      o["end"]   = _ui[i].endMin;
      o["days"]  = _ui[i].days;
      writeActions(o, _ui[i]);
    }
  }

  void mqttToJson(JsonArray arr) const {
    for (int i = 0; i < _mqttCount; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["from"] = (uint32_t)_mqtt[i].from;
      o["to"]   = (uint32_t)_mqtt[i].to;
      writeActions(o, _mqtt[i]);
    }
  }

  uint8_t uiCount() const { return _uiCount; }

  /*
     Seconds until the next window starts, or -1 if nothing is coming up.
     Repeating windows are worked out with plain integer arithmetic on
     (day-of-week, minute-of-day) rather than building candidate epochs, so this
     stays cheap enough to call on every status update. That ignores DST
     transitions, which can make the answer an hour out twice a year - fine for
     a "next in" readout, and not used for anything that acts on it.
  */
  int32_t secondsUntilNext(time_t nowEpoch) const {
    if (nowEpoch <= 0) return -1;
    struct tm lt;
    localtime_r(&nowEpoch, &lt);
    const int32_t nowMin = lt.tm_hour * 60 + lt.tm_min;
    const int today = lt.tm_wday;               // 0 = Sunday
    int32_t bestMin = -1;

    for (int i = 0; i < _uiCount; i++) {
      const SchedWindow &w = _ui[i];
      for (int d = 0; d < 7; d++) {
        if (!(w.days & (1 << d))) continue;
        int32_t delta = ((d - today + 7) % 7) * 1440 + (int32_t)w.startMin - nowMin;
        if (delta <= 0) delta += 7 * 1440;       // already gone this week
        if (bestMin < 0 || delta < bestMin) bestMin = delta;
      }
    }

    int32_t best = (bestMin < 0) ? -1 : bestMin * 60;

    for (int i = 0; i < _mqttCount; i++) {
      if (_mqtt[i].from <= nowEpoch) continue;
      int32_t secs = (int32_t)(_mqtt[i].from - nowEpoch);
      if (best < 0 || secs < best) best = secs;
    }
    return best;
  }

  // Seconds until the active window ends, or -1 if none is active
  int32_t secondsUntilEnd(time_t nowEpoch) const {
    if (nowEpoch <= 0) return -1;
    for (int i = 0; i < _mqttCount; i++)
      if (nowEpoch >= _mqtt[i].from && nowEpoch < _mqtt[i].to)
        return (int32_t)(_mqtt[i].to - nowEpoch);

    struct tm lt;
    localtime_r(&nowEpoch, &lt);
    const int32_t nowMin = lt.tm_hour * 60 + lt.tm_min;
    const uint8_t dayBit = (uint8_t)(1 << lt.tm_wday);
    const SchedWindow* w = findRepeating((uint16_t)nowMin, dayBit);
    if (!w) return -1;
    int32_t endMin = (int32_t)w->endMin;
    if (w->endMin <= w->startMin && nowMin >= (int32_t)w->startMin) endMin += 1440;  // wraps midnight
    int32_t left = endMin - nowMin;
    return left > 0 ? left * 60 : -1;
  }

  // Epoch of the next MQTT window start after now, or 0 if none
  time_t nextMqttStart(time_t nowEpoch) const {
    time_t best = 0;
    for (int i = 0; i < _mqttCount; i++) {
      if (_mqtt[i].from > nowEpoch && (best == 0 || _mqtt[i].from < best)) best = _mqtt[i].from;
    }
    return best;
  }

private:
  SchedWindow _ui[SCHED_MAX_UI];
  SchedWindow _mqtt[SCHED_MAX_MQTT];
  uint8_t _uiCount = 0;
  uint8_t _mqttCount = 0;
  uint32_t _mqttSetMs = 0;

  static uint16_t roundTo5(uint16_t m) { return (uint16_t)((m / 5) * 5); }

  // Read up to maxDigits decimal digits, advancing p. False if none found.
  static bool readInt(const char*& p, int& out, int maxDigits) {
    int v = 0, n = 0;
    while (*p >= '0' && *p <= '9' && n < maxDigits) { v = v * 10 + (*p - '0'); p++; n++; }
    if (n == 0) return false;
    out = v;
    return true;
  }

  static void applyActions(JsonObjectConst o, SchedWindow &w) {
    if (!o["charge"].isNull())    w.charge    = o["charge"]    ? 1 : 0;
    if (!o["discharge"].isNull()) w.discharge = o["discharge"] ? 1 : 0;
    if (!o["forcecharge"].isNull()) w.force   = o["forcecharge"] ? 1 : 0;
    w.targetSOC = (uint8_t)(o["targetsoc"] | 0);
    if (w.targetSOC > 100) w.targetSOC = 100;
  }

  static void writeActions(JsonObject o, const SchedWindow &w) {
    if (w.charge    >= 0) o["charge"]      = (w.charge == 1);
    if (w.discharge >= 0) o["discharge"]   = (w.discharge == 1);
    if (w.force     >= 0) o["forcecharge"] = (w.force == 1);
    if (w.targetSOC > 0)  o["targetsoc"]   = w.targetSOC;
  }

  const SchedWindow* findAbsolute(time_t now) const {
    for (int i = 0; i < _mqttCount; i++)
      if (now >= _mqtt[i].from && now < _mqtt[i].to) return &_mqtt[i];
    return nullptr;
  }

  // Handles windows that wrap past midnight, e.g. 23:30 to 05:30. The day bit
  // is tested against the day the window STARTED on, so an overnight window set
  // for Monday runs Monday night into Tuesday morning.
  const SchedWindow* findRepeating(uint16_t nowMin, uint8_t dayBit) const {
    uint8_t prevDayBit = (uint8_t)(dayBit == SCHED_SUN ? SCHED_SAT : (dayBit >> 1));
    for (int i = 0; i < _uiCount; i++) {
      const SchedWindow &w = _ui[i];
      if (w.startMin < w.endMin) {
        if ((w.days & dayBit) && nowMin >= w.startMin && nowMin < w.endMin) return &w;
      } else {
        // wraps midnight
        if ((w.days & dayBit) && nowMin >= w.startMin) return &w;
        if ((w.days & prevDayBit) && nowMin < w.endMin) return &w;
      }
    }
    return nullptr;
  }

  /*
     Deliberately forgiving, because the caller is usually a Home Assistant
     template and there is no good reason to make people fight a format.
     Accepted:
       2026-07-29T02:00       ISO with T
       2026-07-29 02:00:00    what HA's datetime templates actually produce
       2026-07-29T02:00:00Z   trailing zone marker ignored - see note below
       1785024000             bare epoch seconds
     Everything is read as LOCAL time, honouring the configured TZ, since that is
     what a user means when they type a schedule. A trailing Z is tolerated rather
     than honoured; publish local times, or epoch if you need UTC precision.
  */
  static time_t parseEpoch(const char* s) {
    if (!s || !*s) return 0;

    bool digits = true;
    for (const char* p = s; *p; p++) if (*p < '0' || *p > '9') { digits = false; break; }
    if (digits) return (time_t)strtoul(s, nullptr, 10);

    // Parsed by hand rather than with sscanf: pulling in scanf costs ~9KB of
    // flash for the float-capable variant, which is absurd for reading a date.
    const char* p = s;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (!readInt(p, y, 4) || *p++ != '-') return 0;
    if (!readInt(p, mo, 2) || *p++ != '-') return 0;
    if (!readInt(p, d, 2)) return 0;
    if (*p != 'T' && *p != 't' && *p != ' ') return 0;
    p++;
    if (!readInt(p, h, 2) || *p++ != ':') return 0;
    if (!readInt(p, mi, 2)) return 0;
    if (*p == ':') { p++; if (!readInt(p, sec, 2)) return 0; }

    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h > 23 || mi > 59 || sec > 60) return 0;

    struct tm tmv; memset(&tmv, 0, sizeof(tmv));

    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = sec;
    tmv.tm_isdst = -1;     // let the TZ rules decide GMT vs BST
    return mktime(&tmv);
  }
};

extern ChargeSchedule Schedule;

#endif  // DISABLE_SCHEDULER
