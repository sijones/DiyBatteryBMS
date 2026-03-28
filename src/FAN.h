#pragma once
#include <Arduino.h>

#if !defined(ESPCAN_C3)
#include "soc/mcpwm_struct.h"
#include "driver/mcpwm.h"

uint8_t FAN_PWM = 0;
bool FAN_INIT = false;
mcpwm_config_t pwm_config = {};

#define FANPWMFREQ              25000   // 25kHz

void FanInit(uint8_t FAN_PIN)
{
    if (FAN_PIN > 0 && FAN_INIT == false)
    {
        gpio_pulldown_en((gpio_num_t) FAN_PIN); 
        mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM1A, FAN_PIN); 
        pwm_config.frequency = FANPWMFREQ;              
        pwm_config.cmpr_b = 0;                          //duty cycle of PWMxA = 0
        pwm_config.counter_mode = MCPWM_UP_DOWN_COUNTER;
        pwm_config.duty_mode = MCPWM_DUTY_MODE_0;
        FAN_INIT = true;
        mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_1, &pwm_config);    //Configure PWM1A timer 0 with above settings
        log_d( "MCPWM complete" );
    }
}

void FanUpdate(float Speed)
{
    if(FAN_INIT) {
        if(Speed > 60)
            Speed = 100;
        else if(Speed < 30)
            Speed = 30;
        mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, Speed);
        FAN_PWM = Speed;
    }
}

// Temperature-based fan control: allows 0% (off) unlike FanUpdate which clamps to 30%
void FanUpdateTemp(float dutyPercent)
{
    if(FAN_INIT) {
        if(dutyPercent <= 0.0f) {
            mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 0.0f);
            FAN_PWM = 0;
        } else if(dutyPercent >= 100.0f) {
            mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 100.0f);
            FAN_PWM = 100;
        } else {
            // Map to 30-100% range: most PWM fans stall below ~30%
            float mapped = 30.0f + (dutyPercent * 0.7f);
            mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, mapped);
            FAN_PWM = (uint8_t)mapped;
        }
    }
}

#else
// ESP32-C3 stub (no MCPWM support)
uint8_t FAN_PWM = 0;
bool FAN_INIT = false;
void FanInit(uint8_t FAN_PIN) { log_w("FAN not supported on C3"); }
void FanUpdate(float Speed) { /* No-op on C3 */ }
void FanUpdateTemp(float dutyPercent) { /* No-op on C3 */ }
#endif