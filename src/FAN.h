#pragma once
#include <Arduino.h>
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