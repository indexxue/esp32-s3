#include "pwm.h"

#include <stddef.h>

#include "driver/ledc.h"

static bool_t s_pwmDriverInited = FALSE;
static ledc_mode_t s_pwmSpeedMode = LEDC_LOW_SPEED_MODE;
static ledc_timer_t s_pwmTimer = LEDC_TIMER_0;
static ledc_timer_bit_t s_pwmDutyResolution = LEDC_TIMER_10_BIT;
static u32_t s_pwmFrequencyHz = 1000U;
static u32_t s_pwmMaxDuty = 1023U;
static bool_t s_pwmChannelInited[LEDC_CHANNEL_MAX] = {FALSE};
static esp_err_t s_pwmLastErr = ESP_OK;

static bool_t pwmSetLastErr(esp_err_t err)
{
    s_pwmLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

static bool_t pwmConvertSpeedMode(PwmSpeedMode_t speedMode, ledc_mode_t *outSpeedMode)
{
    if (outSpeedMode == NULL) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (speedMode) {
        case PWM_SPEED_MODE_LOW_E: *outSpeedMode = LEDC_LOW_SPEED_MODE; break;
        case PWM_SPEED_MODE_HIGH_E:
#if defined(LEDC_HIGH_SPEED_MODE)
            *outSpeedMode = LEDC_HIGH_SPEED_MODE;
            break;
#else
            return pwmSetLastErr(ESP_ERR_NOT_SUPPORTED);
#endif
        default: return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return pwmSetLastErr(ESP_OK);
}

static bool_t pwmConvertTimer(PwmTimer_t timer, ledc_timer_t *outTimer)
{
    if (outTimer == NULL) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (timer) {
        case PWM_TIMER_0_E: *outTimer = LEDC_TIMER_0; break;
        case PWM_TIMER_1_E: *outTimer = LEDC_TIMER_1; break;
        case PWM_TIMER_2_E: *outTimer = LEDC_TIMER_2; break;
        case PWM_TIMER_3_E: *outTimer = LEDC_TIMER_3; break;
        default: return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return pwmSetLastErr(ESP_OK);
}

static bool_t pwmConvertDutyResolution(PwmDutyResolution_t resolution, ledc_timer_bit_t *outResolution)
{
    if (outResolution == NULL) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (resolution) {
        case PWM_DUTY_RES_10BIT_E: *outResolution = LEDC_TIMER_10_BIT; break;
        case PWM_DUTY_RES_11BIT_E: *outResolution = LEDC_TIMER_11_BIT; break;
        case PWM_DUTY_RES_12BIT_E: *outResolution = LEDC_TIMER_12_BIT; break;
        case PWM_DUTY_RES_13BIT_E: *outResolution = LEDC_TIMER_13_BIT; break;
        case PWM_DUTY_RES_14BIT_E: *outResolution = LEDC_TIMER_14_BIT; break;
        default: return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return pwmSetLastErr(ESP_OK);
}

static bool_t pwmConvertChannel(PwmChannel_t channel, ledc_channel_t *outChannel)
{
    if (outChannel == NULL) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (channel) {
        case PWM_CHANNEL_0_E: *outChannel = LEDC_CHANNEL_0; break;
        case PWM_CHANNEL_1_E: *outChannel = LEDC_CHANNEL_1; break;
        case PWM_CHANNEL_2_E: *outChannel = LEDC_CHANNEL_2; break;
        case PWM_CHANNEL_3_E: *outChannel = LEDC_CHANNEL_3; break;
        case PWM_CHANNEL_4_E: *outChannel = LEDC_CHANNEL_4; break;
        case PWM_CHANNEL_5_E: *outChannel = LEDC_CHANNEL_5; break;
        case PWM_CHANNEL_6_E: *outChannel = LEDC_CHANNEL_6; break;
        case PWM_CHANNEL_7_E: *outChannel = LEDC_CHANNEL_7; break;
        default: return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return pwmSetLastErr(ESP_OK);
}

bool_t PwmDriverInit(const PwmDriverConfig_t *config)
{
    ledc_timer_config_t timerConfig = {0};
    ledc_mode_t speedMode = LEDC_LOW_SPEED_MODE;
    ledc_timer_t timer = LEDC_TIMER_0;
    ledc_timer_bit_t dutyResolution = LEDC_TIMER_10_BIT;
    u8_t i = 0U;
    esp_err_t ret = ESP_OK;

    if (s_pwmDriverInited == TRUE) {
        return pwmSetLastErr(ESP_OK);
    }
    if (config == NULL) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (config->frequencyHz == 0U) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }

    if (pwmConvertSpeedMode(config->speedMode, &speedMode) == FALSE) {
        return FALSE;
    }
    if (pwmConvertTimer(config->timer, &timer) == FALSE) {
        return FALSE;
    }
    if (pwmConvertDutyResolution(config->dutyResolution, &dutyResolution) == FALSE) {
        return FALSE;
    }

    timerConfig.speed_mode = speedMode;
    timerConfig.duty_resolution = dutyResolution;
    timerConfig.timer_num = timer;
    timerConfig.freq_hz = config->frequencyHz;
    timerConfig.clk_cfg = LEDC_AUTO_CLK;

    ret = ledc_timer_config(&timerConfig);
    if (ret != ESP_OK) {
        return pwmSetLastErr(ret);
    }

    s_pwmSpeedMode = speedMode;
    s_pwmTimer = timer;
    s_pwmDutyResolution = dutyResolution;
    s_pwmFrequencyHz = config->frequencyHz;
    s_pwmMaxDuty = (1UL << (u32_t)dutyResolution) - 1UL;
    for (i = 0U; i < LEDC_CHANNEL_MAX; i++) {
        s_pwmChannelInited[i] = FALSE;
    }
    s_pwmDriverInited = TRUE;
    return pwmSetLastErr(ESP_OK);
}

bool_t PwmDriverDeinit(void)
{
    ledc_channel_t channel = LEDC_CHANNEL_0;
    u8_t i = 0U;
    esp_err_t ret = ESP_OK;

    if (s_pwmDriverInited == FALSE) {
        return pwmSetLastErr(ESP_OK);
    }

    for (i = 0U; i < LEDC_CHANNEL_MAX; i++) {
        if (s_pwmChannelInited[i] == TRUE) {
            channel = (ledc_channel_t)i;
            ret = ledc_stop(s_pwmSpeedMode, channel, 0U);
            if (ret != ESP_OK) {
                return pwmSetLastErr(ret);
            }
            s_pwmChannelInited[i] = FALSE;
        }
    }

    s_pwmSpeedMode = LEDC_LOW_SPEED_MODE;
    s_pwmTimer = LEDC_TIMER_0;
    s_pwmDutyResolution = LEDC_TIMER_10_BIT;
    s_pwmFrequencyHz = 1000U;
    s_pwmMaxDuty = 1023U;
    s_pwmDriverInited = FALSE;
    return pwmSetLastErr(ESP_OK);
}

bool_t PwmConfigureChannel(const PwmChannelConfig_t *config)
{
    ledc_channel_config_t channelConfig = {0};
    ledc_channel_t channel = LEDC_CHANNEL_0;
    ledc_timer_t timer = LEDC_TIMER_0;
    esp_err_t ret = ESP_OK;

    if (s_pwmDriverInited == FALSE) {
        return pwmSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (config == NULL) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (config->duty > s_pwmMaxDuty) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (pwmConvertChannel(config->channel, &channel) == FALSE) {
        return FALSE;
    }
    if (pwmConvertTimer(config->timer, &timer) == FALSE) {
        return FALSE;
    }

    channelConfig.gpio_num = config->gpioPin;
    channelConfig.speed_mode = s_pwmSpeedMode;
    channelConfig.channel = channel;
    channelConfig.intr_type = LEDC_INTR_DISABLE;
    channelConfig.timer_sel = timer;
    channelConfig.duty = config->duty;
    channelConfig.hpoint = config->hpoint;
    channelConfig.flags.output_invert = 0;
    channelConfig.sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;

    ret = ledc_channel_config(&channelConfig);
    if (ret != ESP_OK) {
        return pwmSetLastErr(ret);
    }

    s_pwmChannelInited[(u8_t)channel] = TRUE;
    return pwmSetLastErr(ESP_OK);
}

bool_t PwmSetDuty(PwmChannel_t channel, u32_t duty)
{
    ledc_channel_t ledcChannel = LEDC_CHANNEL_0;
    esp_err_t ret = ESP_OK;

    if (s_pwmDriverInited == FALSE) {
        return pwmSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (duty > s_pwmMaxDuty) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (pwmConvertChannel(channel, &ledcChannel) == FALSE) {
        return FALSE;
    }
    if (s_pwmChannelInited[(u8_t)ledcChannel] == FALSE) {
        return pwmSetLastErr(ESP_ERR_INVALID_STATE);
    }

    ret = ledc_set_duty(s_pwmSpeedMode, ledcChannel, duty);
    if (ret != ESP_OK) {
        return pwmSetLastErr(ret);
    }

    ret = ledc_update_duty(s_pwmSpeedMode, ledcChannel);
    return pwmSetLastErr(ret);
}

bool_t PwmSetFrequency(u32_t frequencyHz)
{
    u32_t freqSet = 0U;

    if (s_pwmDriverInited == FALSE) {
        return pwmSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (frequencyHz == 0U) {
        return pwmSetLastErr(ESP_ERR_INVALID_ARG);
    }

    freqSet = (u32_t)ledc_set_freq(s_pwmSpeedMode, s_pwmTimer, frequencyHz);
    if (freqSet != frequencyHz) {
        return pwmSetLastErr(ESP_FAIL);
    }

    s_pwmFrequencyHz = frequencyHz;
    return pwmSetLastErr(ESP_OK);
}

bool_t PwmStart(PwmChannel_t channel)
{
    ledc_channel_t ledcChannel = LEDC_CHANNEL_0;
    esp_err_t ret = ESP_OK;

    if (s_pwmDriverInited == FALSE) {
        return pwmSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (pwmConvertChannel(channel, &ledcChannel) == FALSE) {
        return FALSE;
    }
    if (s_pwmChannelInited[(u8_t)ledcChannel] == FALSE) {
        return pwmSetLastErr(ESP_ERR_INVALID_STATE);
    }

    ret = ledc_update_duty(s_pwmSpeedMode, ledcChannel);
    return pwmSetLastErr(ret);
}

bool_t PwmStop(PwmChannel_t channel, bool_t idleLevelHigh)
{
    ledc_channel_t ledcChannel = LEDC_CHANNEL_0;
    esp_err_t ret = ESP_OK;

    if (s_pwmDriverInited == FALSE) {
        return pwmSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (pwmConvertChannel(channel, &ledcChannel) == FALSE) {
        return FALSE;
    }
    if (s_pwmChannelInited[(u8_t)ledcChannel] == FALSE) {
        return pwmSetLastErr(ESP_ERR_INVALID_STATE);
    }

    ret = ledc_stop(s_pwmSpeedMode, ledcChannel, (idleLevelHigh == TRUE) ? 1U : 0U);
    return pwmSetLastErr(ret);
}

s32_t PwmGetMaxDuty(void)
{
    return (s32_t)s_pwmMaxDuty;
}

s32_t PwmGetLastError(void)
{
    return (s32_t)s_pwmLastErr;
}
