#pragma once
#include <Arduino.h>

/* Battery figures taken from MQTT instead of a shunt this board can reach.

   Some installs already have the shunt read by something else - a Cerbo, a
   Node-RED flow, another gateway - and publish the result to a broker. There is
   then no VE.Direct wire and no BLE advert for this board to listen to, but the
   numbers exist; they just arrive over the network. Four independently
   configurable topics carry one plain number each: SOC, voltage, current and
   temperature. Not JSON, same convention as the external temperature topics
   that were already here.

   Nothing here touches Inverter. The MQTT callback runs on the network task,
   and every numeric field the charge logic reads is guarded by
   Inverter.CANMutex - a spinlock taken with interrupts off. Writing from that
   callback would mean taking that lock on a task that may also be inside the
   TCP stack, which is exactly the deadlock DataProcessing.h documents at
   length. So the callback only stashes scalars and timestamps here, and
   loop() applies them under the mutex through MQTTShuntDataProcess(), the same
   way the BLE advertisement callback hands off to BLEDataProcess().

   Values are stored in the units CANBUS wants rather than the units they
   arrive in, so the conversion happens once, at the edge, where the float from
   the payload still exists. */

/* How long a set of readings stays believable. Deliberately far longer than
   BLE's 15s: a SmartShunt adverts about once a second and anything else means
   it is gone, whereas an MQTT publisher may well be on a 10 or 20 second
   timer and be perfectly healthy. */
#define MQTT_SHUNT_STALE_MS 30000

class MqttShuntSource {
  public:
    // ---- decoded values, in the units the CANBUS setters take ----
    volatile uint16_t VoltageCentiV = 0;   // 0.01 V  -> Inverter.BattVoltage()
    volatile int32_t  CurrentDeciA  = 0;   // 0.1 A   -> Inverter.BattCurrentDeciA()
    volatile uint16_t SOCPermille   = 0;   // 0.1 %   -> Inverter.BattSOCPermille()
    volatile int16_t  TempC         = 0;   // whole C -> Inverter.BattTemp()

    /* Per field, because the topics are independent: a half-configured source
       has to be distinguishable from one that is configured and quiet. */
    volatile bool HaveVoltage = false;
    volatile bool HaveCurrent = false;
    volatile bool HaveSOC     = false;
    volatile bool HaveTemp    = false;

    volatile uint32_t LastVoltageMs = 0;
    volatile uint32_t LastCurrentMs = 0;
    volatile uint32_t LastSOCMs     = 0;
    volatile uint32_t LastTempMs    = 0;
    volatile uint32_t LastUpdateMs  = 0;   // newest of the three core (SOC/V/I) fields
    volatile uint32_t MessagesSeen  = 0;

    /* Split rather than chained (LastX = LastUpdateMs = millis()) and MessagesSeen
       = MessagesSeen + 1 rather than MessagesSeen++: C++20 deprecates both using
       the value of an assignment to a volatile and incrementing one directly
       (P1152), so what used to be one expression per line is now two or three. */
    void SetVoltage(float v) { VoltageCentiV = (uint16_t)lroundf(v * 100.0f);
                               HaveVoltage = true; LastUpdateMs = millis(); LastVoltageMs = LastUpdateMs;
                               MessagesSeen = MessagesSeen + 1; }
    void SetCurrent(float a) { CurrentDeciA  = (int32_t) lroundf(a * 10.0f);
                               HaveCurrent = true; LastUpdateMs = millis(); LastCurrentMs = LastUpdateMs;
                               MessagesSeen = MessagesSeen + 1; }
    void SetSOC(float pct)   { if (pct < 0) pct = 0; if (pct > 100) pct = 100;
                               SOCPermille   = (uint16_t)lroundf(pct * 10.0f);
                               HaveSOC = true; LastUpdateMs = millis(); LastSOCMs = LastUpdateMs;
                               MessagesSeen = MessagesSeen + 1; }
    /* Temperature does not stamp LastUpdateMs. It is optional, and a board
       publishing only a temperature must not read as a live shunt source. */
    void SetTemp(float c)    { TempC = (int16_t)lroundf(c);
                               HaveTemp = true; LastTempMs = millis(); MessagesSeen = MessagesSeen + 1; }

    /* Fresh means all three of the fields the charge logic and the CAN
       readiness gate depend on have arrived at least once, and the newest of
       them arrived within the window. One window across the three rather than
       one each, because SOC is often published far less often than V and I and
       per-field staleness would flap on a perfectly healthy feed. */
    bool DataFresh(uint32_t withinMs = MQTT_SHUNT_STALE_MS) const {
      return HaveVoltage && HaveCurrent && HaveSOC &&
             LastUpdateMs > 0 && (millis() - LastUpdateMs) < withinMs;
    }

    // Anything at all has arrived - enough for the UI to be worth showing.
    bool Configured() const { return HaveVoltage || HaveCurrent || HaveSOC; }
};

extern MqttShuntSource MqttShunt;
