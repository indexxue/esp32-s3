/**
 * @file buzzer.c
 * @brief 有源 / 无源蜂鸣器公共驱动。
 */

#include "buzzer.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpio.h"
#include "log.h"

#define BUZZER_SHORT_ON_MS (120U)
#define BUZZER_SHORT_OFF_MS (120U)
#define BUZZER_LONG_ON_MS (500U)
#define BUZZER_LONG_OFF_MS (200U)

#define BUZZER_PASSIVE_DEFAULT_HZ (3000U)
#define BUZZER_PATTERN_TASK_STACK (2048U)
#define BUZZER_PATTERN_TASK_PRIO (3U)

static buzzer_config_t s_cfg;
static bool_t s_ready;
static volatile bool_t s_pattern_stop;
static volatile bool_t s_pattern_busy;
static TaskHandle_t s_pattern_task;

static u32_t buzzer_passive_duty(void)
{
    if (s_cfg.passive_duty != 0U) {
        return s_cfg.passive_duty;
    }
    {
        const s32_t max = PwmGetMaxDuty();
        return (max > 0) ? (u32_t)(max / 2) : 512U;
    }
}

static u32_t buzzer_inactive_level(void)
{
    return (s_cfg.active_level != 0U) ? 0U : 1U;
}

static status_t buzzer_gpio_output_init(void)
{
    GpioPinConfig_t pin = {0};

    if (GpioDriverInit() != TRUE) {
        LOG_ERROR("buzzer: GpioDriverInit failed, esp err %d", (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    pin.pin = s_cfg.gpio;
    pin.mode = GPIO_MODE_OUTPUT_E;
    pin.pullUpEn = GPIO_PULL_DISABLE_E;
    pin.pullDownEn = GPIO_PULL_DISABLE_E;
    pin.intrType = GPIO_INTR_DISABLE_E;
    if (GpioConfigurePin(&pin) != TRUE) {
        LOG_ERROR("buzzer: GpioConfigurePin GPIO%d failed, esp err %d",
                  (int)s_cfg.gpio,
                  (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    (void)GpioWritePin(s_cfg.gpio, buzzer_inactive_level());
    return STATUS_OK;
}

static status_t buzzer_passive_pwm_init(void)
{
    PwmDriverConfig_t pwm = {0};
    PwmChannelConfig_t ch = {0};
    const u32_t freq = (s_cfg.passive_freq_hz != 0U) ? s_cfg.passive_freq_hz : BUZZER_PASSIVE_DEFAULT_HZ;

    pwm.speedMode = PWM_SPEED_MODE_LOW_E;
    pwm.timer = s_cfg.pwm_timer;
    pwm.dutyResolution = PWM_DUTY_RES_10BIT_E;
    pwm.frequencyHz = freq;

    if (PwmDriverInit(&pwm) != TRUE) {
        LOG_ERROR("buzzer: PwmDriverInit failed, esp err %d", (int)PwmGetLastError());
        return STATUS_FAIL;
    }

    ch.channel = s_cfg.pwm_channel;
    ch.timer = s_cfg.pwm_timer;
    ch.gpioPin = s_cfg.gpio;
    ch.duty = 0U;
    ch.hpoint = 0;
    if (PwmConfigureChannel(&ch) != TRUE) {
        LOG_ERROR("buzzer: PwmConfigureChannel GPIO%d failed, esp err %d",
                  (int)s_cfg.gpio,
                  (int)PwmGetLastError());
        return STATUS_FAIL;
    }

    return STATUS_OK;
}

static void buzzer_delay_ms(u32_t ms)
{
    if (ms == 0U) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void buzzer_beep_block(u32_t on_ms, u32_t off_ms)
{
    buzzer_on();
    buzzer_delay_ms(on_ms);
    buzzer_off();
    buzzer_delay_ms(off_ms);
}

static void buzzer_pattern_task_fn(void *arg)
{
    const buzzer_pattern_e pattern = (buzzer_pattern_e)(uintptr_t)arg;
    u8_t i;

    s_pattern_stop = FALSE;

    switch (pattern) {
        case BUZZER_PATTERN_SHORT:
            buzzer_beep_block(BUZZER_SHORT_ON_MS, 0U);
            break;
        case BUZZER_PATTERN_DOUBLE_SHORT:
            for (i = 0U; i < 2U; i++) {
                if (s_pattern_stop) {
                    break;
                }
                buzzer_beep_block(BUZZER_SHORT_ON_MS, BUZZER_SHORT_OFF_MS);
            }
            break;
        case BUZZER_PATTERN_TRIPLE_LONG:
            for (i = 0U; i < 3U; i++) {
                if (s_pattern_stop) {
                    break;
                }
                buzzer_beep_block(BUZZER_LONG_ON_MS, BUZZER_LONG_OFF_MS);
            }
            break;
        case BUZZER_PATTERN_ALARM:
            buzzer_on();
            while (!s_pattern_stop) {
                vTaskDelay(pdMS_TO_TICKS(50U));
            }
            buzzer_off();
            break;
        default:
            break;
    }

    buzzer_off();
    s_pattern_busy = FALSE;
    s_pattern_task = NULL;
    vTaskDelete(NULL);
}

status_t buzzer_init(const buzzer_config_t *config)
{
    if (config == NULL) {
        return STATUS_INVALID_ARG;
    }

    buzzer_deinit();
    s_cfg = *config;

    if (s_cfg.type == BUZZER_TYPE_PASSIVE) {
        if (buzzer_passive_pwm_init() != STATUS_OK) {
            return STATUS_FAIL;
        }
    } else if (buzzer_gpio_output_init() != STATUS_OK) {
        return STATUS_FAIL;
    }

    s_ready = TRUE;
    LOG_INFO("buzzer: init GPIO%d type=%s",
             (int)s_cfg.gpio,
             (s_cfg.type == BUZZER_TYPE_PASSIVE) ? "passive" : "active");
    return STATUS_OK;
}

void buzzer_deinit(void)
{
    buzzer_stop_pattern();

    if (s_ready) {
        buzzer_off();
        if (s_cfg.type == BUZZER_TYPE_PASSIVE) {
            (void)PwmDriverDeinit();
        }
    }

    s_ready = FALSE;
    memset(&s_cfg, 0, sizeof(s_cfg));
}

bool_t buzzer_is_ready(void)
{
    return s_ready;
}

void buzzer_on(void)
{
    if (!s_ready) {
        return;
    }

    if (s_cfg.type == BUZZER_TYPE_PASSIVE) {
        (void)PwmSetDuty(s_cfg.pwm_channel, buzzer_passive_duty());
        (void)PwmStart(s_cfg.pwm_channel);
    } else {
        (void)GpioWritePin(s_cfg.gpio, (u32_t)s_cfg.active_level);
    }
}

void buzzer_off(void)
{
    if (!s_ready) {
        return;
    }

    if (s_cfg.type == BUZZER_TYPE_PASSIVE) {
        (void)PwmStop(s_cfg.pwm_channel, FALSE);
    } else {
        (void)GpioWritePin(s_cfg.gpio, buzzer_inactive_level());
    }
}

status_t buzzer_play_pattern(buzzer_pattern_e pattern)
{
    if (!s_ready) {
        return STATUS_INVALID_STATE;
    }

    buzzer_stop_pattern();
    s_pattern_stop = FALSE;
    s_pattern_busy = TRUE;

    if (xTaskCreate(buzzer_pattern_task_fn,
                    "buzzer_pat",
                    BUZZER_PATTERN_TASK_STACK,
                    (void *)(uintptr_t)pattern,
                    BUZZER_PATTERN_TASK_PRIO,
                    &s_pattern_task) != pdPASS) {
        s_pattern_busy = FALSE;
        s_pattern_task = NULL;
        LOG_ERROR("buzzer: create pattern task failed");
        return STATUS_NO_MEM;
    }

    return STATUS_OK;
}

void buzzer_stop_pattern(void)
{
    s_pattern_stop = TRUE;
    buzzer_off();

    if (s_pattern_task != NULL) {
        for (u8_t i = 0U; i < 20U; i++) {
            if (s_pattern_task == NULL) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10U));
        }
    }

    s_pattern_busy = FALSE;
}

bool_t buzzer_pattern_busy(void)
{
    return s_pattern_busy;
}
