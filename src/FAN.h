#pragma once
#include <Arduino.h>
#include "driver/gpio.h"

/* Fan PWM on LEDC.

   This drove the fan through MCPWM, which was the wrong peripheral for the job:
   it is a motor-control block - dead time, fault handling, capture inputs - and
   none of that is wanted to vary the speed of one 4-pin fan. It also does not
   exist on the ESP32-C3, so that board carried a stub that logged "FAN not
   supported" and did nothing. LEDC is on every variant, so the stub is gone and
   the C3 gets fan control like everything else.

   The move was due anyway: the legacy driver/mcpwm.h this used is deprecated in
   ESP-IDF 5.x and warns on every build.

   Arduino core 3.x addresses LEDC by pin rather than by channel - ledcAttach()
   allocates a channel and timer itself, so there is no channel number to track
   or collide with anything added later. */

#define FANPWMFREQ   25000   // 25kHz - above audible, and what 4-pin fans expect
#define FANPWMBITS   8       // 0-255 duty. At 25kHz the LEDC clock allows ~11 bits,
                             // so 8 is comfortable on every variant including the C3.

uint8_t FAN_PWM = 0;         // last duty as a percentage, for MQTT and the web UI
bool FAN_INIT = false;
static uint8_t _fanPin = 0;

// Percent to raw duty. Rounded rather than truncated, so 100% reaches full scale.
static inline uint32_t FanDutyFromPercent(float pct)
{
    const uint32_t full = (1u << FANPWMBITS) - 1;
    if (pct <= 0.0f)   return 0;
    if (pct >= 100.0f) return full;
    return (uint32_t)(((pct * full) / 100.0f) + 0.5f);
}

static inline void FanSet(float pct, uint8_t reportPercent)
{
    ledcWrite(_fanPin, FanDutyFromPercent(pct));
    FAN_PWM = reportPercent;
}

void FanInit(uint8_t FAN_PIN)
{
    if (FAN_PIN == 0 || FAN_INIT) return;

    // Held low before the peripheral drives it, so the fan does not sit at full
    // speed during boot on a floating pin. Same order as the MCPWM version.
    gpio_pulldown_en((gpio_num_t) FAN_PIN);

    if (!ledcAttach(FAN_PIN, FANPWMFREQ, FANPWMBITS)) {
        log_e("Fan PWM setup failed on GPIO %u", FAN_PIN);
        return;
    }

    _fanPin = FAN_PIN;
    FAN_INIT = true;
    ledcWrite(_fanPin, 0);
    log_d("Fan PWM on GPIO %u at %u Hz", FAN_PIN, FANPWMFREQ);
}

void FanUpdate(float Speed)
{
    if (!FAN_INIT) return;
    if (Speed > 60)      Speed = 100;
    else if (Speed < 30) Speed = 30;
    FanSet(Speed, (uint8_t) Speed);
}

// Temperature-based fan control: allows 0% (off) unlike FanUpdate which clamps to 30%
void FanUpdateTemp(float dutyPercent)
{
    if (!FAN_INIT) return;

    if (dutyPercent <= 0.0f) {
        FanSet(0.0f, 0);
    } else if (dutyPercent >= 100.0f) {
        FanSet(100.0f, 100);
    } else {
        // Map to 30-100% range: most PWM fans stall below ~30%
        float mapped = 30.0f + (dutyPercent * 0.7f);
        FanSet(mapped, (uint8_t) mapped);
    }
}
