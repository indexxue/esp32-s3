/**
 * @file servo_ctrl.c
 * @brief MG996R 双舵机：50 Hz LEDC，脉宽 500–2500 µs ↔ 角度，软限位。
 *        控制入口：网页 REST / 板端按键；不走串口命令。
 */

#include "servo_ctrl.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "log.h"
#include "pwm.h"

#define TAG "servo"

#define SERVO_PERIOD_US (1000000U / BOARD_SERVO_PWM_FREQ_HZ)
#define SERVO_ANGLE_MIN_ABS (0.0f)
#define SERVO_ANGLE_MAX_ABS (180.0f)

typedef struct {
    float    angle_deg;
    uint16_t pulse_us;
    bool_t   ready;
} servo_state_t;

static servo_state_t s_pan;
static servo_state_t s_tilt;
static bool_t        s_inited;

static servo_state_t *servo_state(servo_ch_t ch)
{
    if (ch == SERVO_CH_PAN) {
        return &s_pan;
    }
    if (ch == SERVO_CH_TILT) {
        return &s_tilt;
    }
    return NULL;
}

static PwmChannel_t servo_pwm_ch(servo_ch_t ch)
{
    if (ch == SERVO_CH_PAN) {
        return (PwmChannel_t)BOARD_SERVO_PAN_PWM_CH;
    }
    return (PwmChannel_t)BOARD_SERVO_TILT_PWM_CH;
}

static float servo_clamp_deg(servo_ch_t ch, float deg)
{
    float lo;
    float hi;

    if (ch == SERVO_CH_PAN) {
        lo = (float)BOARD_SERVO_PAN_MIN_DEG;
        hi = (float)BOARD_SERVO_PAN_MAX_DEG;
    } else {
        lo = (float)BOARD_SERVO_TILT_MIN_DEG;
        hi = (float)BOARD_SERVO_TILT_MAX_DEG;
    }
    if (deg < lo) {
        return lo;
    }
    if (deg > hi) {
        return hi;
    }
    return deg;
}

static uint16_t servo_deg_to_pulse(float deg)
{
    float    span;
    float    us;
    uint16_t pulse;

    if (deg < SERVO_ANGLE_MIN_ABS) {
        deg = SERVO_ANGLE_MIN_ABS;
    }
    if (deg > SERVO_ANGLE_MAX_ABS) {
        deg = SERVO_ANGLE_MAX_ABS;
    }

    span  = (float)(BOARD_SERVO_PULSE_MAX_US - BOARD_SERVO_PULSE_MIN_US);
    us    = (float)BOARD_SERVO_PULSE_MIN_US + (deg / SERVO_ANGLE_MAX_ABS) * span;
    pulse = (uint16_t)(us + 0.5f);
    if (pulse < BOARD_SERVO_PULSE_MIN_US) {
        pulse = (uint16_t)BOARD_SERVO_PULSE_MIN_US;
    }
    if (pulse > BOARD_SERVO_PULSE_MAX_US) {
        pulse = (uint16_t)BOARD_SERVO_PULSE_MAX_US;
    }
    return pulse;
}

static float servo_pulse_to_deg(uint16_t us)
{
    float span;
    float deg;

    if (us < BOARD_SERVO_PULSE_MIN_US) {
        us = (uint16_t)BOARD_SERVO_PULSE_MIN_US;
    }
    if (us > BOARD_SERVO_PULSE_MAX_US) {
        us = (uint16_t)BOARD_SERVO_PULSE_MAX_US;
    }
    span = (float)(BOARD_SERVO_PULSE_MAX_US - BOARD_SERVO_PULSE_MIN_US);
    if (span <= 0.0f) {
        return (float)BOARD_SERVO_CENTER_DEG;
    }
    deg = ((float)(us - BOARD_SERVO_PULSE_MIN_US) / span) * SERVO_ANGLE_MAX_ABS;
    return deg;
}

static status_t servo_apply_pulse(servo_ch_t ch, uint16_t us)
{
    servo_state_t *st;
    s32_t          max_duty;
    u32_t          duty;

    if (s_inited == FALSE) {
        return STATUS_INVALID_STATE;
    }
    st = servo_state(ch);
    if (st == NULL) {
        return STATUS_INVALID_ARG;
    }

    if (us < BOARD_SERVO_PULSE_MIN_US) {
        us = (uint16_t)BOARD_SERVO_PULSE_MIN_US;
    }
    if (us > BOARD_SERVO_PULSE_MAX_US) {
        us = (uint16_t)BOARD_SERVO_PULSE_MAX_US;
    }

    max_duty = PwmGetMaxDuty();
    if (max_duty <= 0) {
        return STATUS_FAIL;
    }
    duty = (u32_t)(((uint64_t)us * (uint64_t)max_duty) / (uint64_t)SERVO_PERIOD_US);
    if (duty > (u32_t)max_duty) {
        duty = (u32_t)max_duty;
    }

    if (PwmSetDuty(servo_pwm_ch(ch), duty) != TRUE) {
        LOG_ERROR("%s: PwmSetDuty ch=%d failed err=%d", TAG, (int)ch, (int)PwmGetLastError());
        return STATUS_FAIL;
    }

    st->pulse_us  = us;
    st->angle_deg = servo_pulse_to_deg(us);
    st->ready     = TRUE;
    return STATUS_OK;
}

status_t servo_init(void)
{
    PwmDriverConfig_t  pwm = {0};
    PwmChannelConfig_t ch  = {0};
    status_t           st;

    if (s_inited != FALSE) {
        return STATUS_OK;
    }

    pwm.speedMode      = PWM_SPEED_MODE_LOW_E;
    pwm.timer          = (PwmTimer_t)BOARD_SERVO_PWM_TIMER;
    pwm.dutyResolution = PWM_DUTY_RES_14BIT_E;
    pwm.frequencyHz    = BOARD_SERVO_PWM_FREQ_HZ;

    if (PwmDriverInit(&pwm) != TRUE) {
        LOG_ERROR("%s: PwmDriverInit failed err=%d", TAG, (int)PwmGetLastError());
        return STATUS_FAIL;
    }

    ch.channel = (PwmChannel_t)BOARD_SERVO_PAN_PWM_CH;
    ch.timer   = (PwmTimer_t)BOARD_SERVO_PWM_TIMER;
    ch.gpioPin = BOARD_SERVO_PAN_PIN;
    ch.duty    = 0U;
    ch.hpoint  = 0;
    if (PwmConfigureChannel(&ch) != TRUE) {
        LOG_ERROR("%s: pan GPIO%d config failed err=%d", TAG, BOARD_SERVO_PAN_PIN, (int)PwmGetLastError());
        return STATUS_FAIL;
    }

    ch.channel = (PwmChannel_t)BOARD_SERVO_TILT_PWM_CH;
    ch.gpioPin = BOARD_SERVO_TILT_PIN;
    if (PwmConfigureChannel(&ch) != TRUE) {
        LOG_ERROR("%s: tilt GPIO%d config failed err=%d", TAG, BOARD_SERVO_TILT_PIN, (int)PwmGetLastError());
        return STATUS_FAIL;
    }

    s_inited = TRUE;
    (void)memset(&s_pan, 0, sizeof(s_pan));
    (void)memset(&s_tilt, 0, sizeof(s_tilt));

    st = servo_center_all();
    if (st != STATUS_OK) {
        s_inited = FALSE;
        return st;
    }

    LOG_INFO("%s: pan=GPIO%d tilt=GPIO%d @ %u Hz center=%u us",
             TAG,
             BOARD_SERVO_PAN_PIN,
             BOARD_SERVO_TILT_PIN,
             (unsigned)BOARD_SERVO_PWM_FREQ_HZ,
             (unsigned)BOARD_SERVO_CENTER_PULSE_US);
    return STATUS_OK;
}

bool_t servo_is_ready(void)
{
    return ((s_inited != FALSE) && (s_pan.ready != FALSE) && (s_tilt.ready != FALSE)) ? TRUE : FALSE;
}

status_t servo_set_pulse_us(servo_ch_t ch, uint16_t us)
{
    return servo_apply_pulse(ch, us);
}

status_t servo_set_angle(servo_ch_t ch, float deg)
{
    float clamped;

    if (servo_state(ch) == NULL) {
        return STATUS_INVALID_ARG;
    }
    clamped = servo_clamp_deg(ch, deg);
    return servo_apply_pulse(ch, servo_deg_to_pulse(clamped));
}

status_t servo_get_angle(servo_ch_t ch, float *deg)
{
    const servo_state_t *st = servo_state(ch);

    if ((st == NULL) || (deg == NULL)) {
        return STATUS_INVALID_ARG;
    }
    if ((s_inited == FALSE) || (st->ready == FALSE)) {
        return STATUS_INVALID_STATE;
    }
    *deg = st->angle_deg;
    return STATUS_OK;
}

status_t servo_center_all(void)
{
    status_t st;

    st = servo_set_angle(SERVO_CH_PAN, (float)BOARD_SERVO_CENTER_DEG);
    if (st != STATUS_OK) {
        return st;
    }
    return servo_set_angle(SERVO_CH_TILT, (float)BOARD_SERVO_CENTER_DEG);
}

status_t servo_nudge(servo_ch_t ch, float delta_deg)
{
    float cur = 0.0f;

    if (servo_get_angle(ch, &cur) != STATUS_OK) {
        return STATUS_INVALID_STATE;
    }
    return servo_set_angle(ch, cur + delta_deg);
}
