/**
 * @file battery.c
 * @brief ESP32-S3：使能 BOARD_BATTERY_PIN_ENABLE，ADC 采样 GPIO1（ADC1_CH0），分压比 2。
 *        `battery_voltage_read_mv` 对真实 ADC 做 2 分钟节流，间隔内返回上次成功采样缓存。
 */

#include "battery.h"

#include "adc.h"
#include "board.h"
#include "gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define BATTERY_ADC_UNIT ADC_UNIT_1_E
/** GPIO1 -> ADC1_CH0，与 `bsp_driver` 枚举一致 */
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0_E

#define BATTERY_SAMPLE_CNT (10U)
#define BATTERY_RC_STABLE_MS (30U)
/** 暂时为 1：IO6 采样使能上电后保持为高，读 ADC 时不再开关（恢复省电分时采样时改为 0）。 */
#define BATTERY_ENABLE_HELD_HIGH (1U)
#define BATTERY_DIVIDER_RATIO (2U)
/**
 * SoC 映射（mV，电池端，已乘分压比）：3300→0%，4000→100%；
 * ≥4200mV 视为正在充电（`battery_info_t.charging`），百分比仍按 100% 饱和。
 */
#define BATTERY_MV_EMPTY (3300U)
#define BATTERY_MV_FULL_SOC (4100U)
#define BATTERY_MV_CHARGING (4200U)
#define BATTERY_LEVEL_PERCENT_VALUES 20, 50, 80, 100

/** 两次真实 ADC 采样之间的最小间隔（毫秒）。 */
#define BATTERY_SAMPLE_PERIOD_MS (120000U)

static const uint8_t s_level_percent[BATTERY_LEVEL_NUM] = {BATTERY_LEVEL_PERCENT_VALUES};

static TickType_t s_last_hw_sample_ticks;
static bool_t s_hw_sample_cache_valid;
static uint32_t s_cached_vbatt_mv;
static battery_voltage_t s_cached_voltage;

static struct {
    bool_t initialized;
    battery_voltage_t voltage;
    battery_info_t info;
    bool_t data_valid;
} s_self;

static void enable_pin_init(void)
{
    GpioPinConfig_t g = {0};

    g.pin        = (s32_t)BOARD_BATTERY_PIN_ENABLE;
    g.mode       = GPIO_MODE_OUTPUT_E;
    g.pullUpEn   = GPIO_PULL_DISABLE_E;
    g.pullDownEn = GPIO_PULL_DISABLE_E;
    g.intrType   = GPIO_INTR_DISABLE_E;
    (void)GpioConfigurePin(&g);
#if BATTERY_ENABLE_HELD_HIGH
    (void)GpioWritePin((s32_t)BOARD_BATTERY_PIN_ENABLE, 1U);
#else
    (void)GpioWritePin((s32_t)BOARD_BATTERY_PIN_ENABLE, 0U);
#endif
}

static uint8_t mv_to_percent(uint32_t mv)
{
    if (mv >= BATTERY_MV_FULL_SOC) {
        return 100U;
    }
    if (mv <= BATTERY_MV_EMPTY) {
        return 0U;
    }
    return (uint8_t)((mv - BATTERY_MV_EMPTY) * 100u / (BATTERY_MV_FULL_SOC - BATTERY_MV_EMPTY));
}

void battery_init(void)
{
    AdcDriverConfig_t adcUnit = {0};
    AdcChannelConfig_t adcCh = {0};

    if (s_self.initialized != FALSE) {
        return;
    }
    (void)memset(&s_self, 0, sizeof(s_self));

    if (GpioDriverInit() != TRUE) {
        return;
    }
    enable_pin_init();

    adcUnit.unit = BATTERY_ADC_UNIT;
    if (AdcDriverInit(&adcUnit) != TRUE) {
        return;
    }

    adcCh.channel  = BATTERY_ADC_CHANNEL;
    adcCh.atten    = ADC_ATTEN_DB_12_E;
    adcCh.bitWidth = ADC_BITWIDTH_DEFAULT_E;
    if (AdcConfigureChannel(&adcCh) != TRUE) {
        return;
    }

    s_hw_sample_cache_valid = FALSE;
    s_last_hw_sample_ticks = 0;

    s_self.initialized = TRUE;
}

/**
 * 执行一次硬件 ADC 采样（不受周期节流；失败不更新缓存时间戳）。
 */
static uint32_t battery_voltage_sample_hw(battery_voltage_t *voltage)
{
    uint32_t sum = 0U;
    s32_t min_vadc = 0;
    s32_t max_vadc = 0;
    uint16_t count = 0U;

#if !BATTERY_ENABLE_HELD_HIGH
    (void)GpioWritePin((s32_t)BOARD_BATTERY_PIN_ENABLE, 1U);
    vTaskDelay(pdMS_TO_TICKS(BATTERY_RC_STABLE_MS));
#endif

    for (uint16_t i = 0; i < BATTERY_SAMPLE_CNT; i++) {
        s32_t vadc = 0;
        if (AdcReadVoltageMv(BATTERY_ADC_CHANNEL, &vadc) != TRUE) {
#if !BATTERY_ENABLE_HELD_HIGH
            (void)GpioWritePin((s32_t)BOARD_BATTERY_PIN_ENABLE, 0U);
#endif
            return 0U;
        }
        sum += (uint32_t)vadc;
        if (count == 0U) {
            min_vadc = max_vadc = vadc;
            count = 1U;
            continue;
        }
        count++;
        if (vadc < min_vadc) {
            min_vadc = vadc;
        }
        if (vadc > max_vadc) {
            max_vadc = vadc;
        }
    }

#if !BATTERY_ENABLE_HELD_HIGH
    (void)GpioWritePin((s32_t)BOARD_BATTERY_PIN_ENABLE, 0U);
#endif

    if (count < 3U) {
        return 0U;
    }

    sum -= (uint32_t)min_vadc;
    sum -= (uint32_t)max_vadc;
    count = (uint16_t)((uint32_t)count - 2U);

    {
        uint32_t avg_vadc = sum / (uint32_t)count;
        uint32_t vbatt_mv = avg_vadc * BATTERY_DIVIDER_RATIO;

        if (voltage != NULL) {
            voltage->current_mv = (uint16_t)vbatt_mv;
            voltage->min_mv = (uint16_t)((uint32_t)min_vadc * BATTERY_DIVIDER_RATIO);
            voltage->max_mv = (uint16_t)((uint32_t)max_vadc * BATTERY_DIVIDER_RATIO);
        }
        return vbatt_mv;
    }
}

uint32_t battery_voltage_read_mv(battery_voltage_t *voltage)
{
    TickType_t now;
    TickType_t period_ticks;

    if (s_self.initialized == FALSE) {
        return 0U;
    }

    now = xTaskGetTickCount();
    period_ticks = pdMS_TO_TICKS(BATTERY_SAMPLE_PERIOD_MS);
    if (period_ticks == 0) {
        period_ticks = 1;
    }

    if ((s_hw_sample_cache_valid != FALSE) &&
        ((TickType_t)(now - s_last_hw_sample_ticks) < period_ticks)) {
        if (voltage != NULL) {
            *voltage = s_cached_voltage;
        }
        return s_cached_vbatt_mv;
    }

    {
        battery_voltage_t v_local;
        battery_voltage_t *vout = (voltage != NULL) ? voltage : &v_local;
        uint32_t vbatt_mv = battery_voltage_sample_hw(vout);

        if (vbatt_mv == 0U) {
            return 0U;
        }

        s_cached_vbatt_mv = vbatt_mv;
        s_cached_voltage = *vout;
        s_last_hw_sample_ticks = now;
        s_hw_sample_cache_valid = TRUE;
        return vbatt_mv;
    }
}

bool_t battery_percent_update(void)
{
    battery_voltage_t v;
    uint32_t mv;

    if (s_self.initialized == FALSE) {
        return FALSE;
    }

    mv = battery_voltage_read_mv(&v);
    if (mv == 0U) {
        return FALSE;
    }

    s_self.voltage = v;
    s_self.info.percent = mv_to_percent(mv);
    s_self.info.charging = (mv >= BATTERY_MV_CHARGING) ? TRUE : FALSE;
    s_self.info.level = BATTERY_LEVEL_NUM - 1U;
    for (uint8_t i = 0; i < BATTERY_LEVEL_NUM; i++) {
        if (s_self.info.percent < s_level_percent[i]) {
            s_self.info.level = i;
            break;
        }
    }
    s_self.data_valid = TRUE;
    return TRUE;
}

bool_t battery_info_read(battery_info_t *info, battery_voltage_t *voltage)
{
    if ((s_self.initialized == FALSE) || (s_self.data_valid == FALSE)) {
        return FALSE;
    }
    if (info != NULL) {
        *info = s_self.info;
    }
    if (voltage != NULL) {
        *voltage = s_self.voltage;
    }
    return TRUE;
}
