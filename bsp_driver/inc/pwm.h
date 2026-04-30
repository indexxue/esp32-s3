#ifndef DRIVER_PWM_H
#define DRIVER_PWM_H

#include "type.h"

typedef enum {
    PWM_SPEED_MODE_LOW_E = 0,
    PWM_SPEED_MODE_HIGH_E = 1
} PwmSpeedMode_t;

typedef enum {
    PWM_TIMER_0_E = 0,
    PWM_TIMER_1_E = 1,
    PWM_TIMER_2_E = 2,
    PWM_TIMER_3_E = 3
} PwmTimer_t;

typedef enum {
    PWM_CHANNEL_0_E = 0,
    PWM_CHANNEL_1_E = 1,
    PWM_CHANNEL_2_E = 2,
    PWM_CHANNEL_3_E = 3,
    PWM_CHANNEL_4_E = 4,
    PWM_CHANNEL_5_E = 5,
    PWM_CHANNEL_6_E = 6,
    PWM_CHANNEL_7_E = 7
} PwmChannel_t;

typedef enum {
    PWM_DUTY_RES_10BIT_E = 10,
    PWM_DUTY_RES_11BIT_E = 11,
    PWM_DUTY_RES_12BIT_E = 12,
    PWM_DUTY_RES_13BIT_E = 13,
    PWM_DUTY_RES_14BIT_E = 14
} PwmDutyResolution_t;

typedef struct {
    PwmSpeedMode_t speedMode;
    PwmTimer_t timer;
    PwmDutyResolution_t dutyResolution;
    u32_t frequencyHz;
} PwmDriverConfig_t;

typedef struct {
    PwmChannel_t channel;
    PwmTimer_t timer;
    s32_t gpioPin;
    u32_t duty;
    s32_t hpoint;
} PwmChannelConfig_t;

bool_t PwmDriverInit(const PwmDriverConfig_t *config);
bool_t PwmDriverDeinit(void);

bool_t PwmConfigureChannel(const PwmChannelConfig_t *config);
bool_t PwmSetDuty(PwmChannel_t channel, u32_t duty);
bool_t PwmSetFrequency(u32_t frequencyHz);
bool_t PwmStart(PwmChannel_t channel);
bool_t PwmStop(PwmChannel_t channel, bool_t idleLevelHigh);

s32_t PwmGetMaxDuty(void);
s32_t PwmGetLastError(void);

#endif
