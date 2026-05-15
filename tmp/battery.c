/**
 * @file    battery.c
 * @brief   Battery voltage/SoC for STM32F103: enable PA3, ADC PA0, 100K+100K divider, 100nF cap.
 *          3.7V Li-ion: Vbatt = 2 * Vadc (divider 1:2).
 *          Note: PA3 is driven as GPIO output; if USART2_RX is on PA3, reassign UART or avoid using both.
 */

#include "battery.h"
#include "main.h"
#include "adc.h"
#include <string.h>

#define BATTERY_ENABLE_PORT        GPIOA
#define BATTERY_ENABLE_PIN         GPIO_PIN_3
#define BATTERY_SAMPLE_CNT         10
#define BATTERY_RC_STABLE_MS       30
#define BATTERY_ADC_TIMEOUT_MS      50
#define BATTERY_VREF_MV             3300
#define BATTERY_DIVIDER_RATIO       2
#define BATTERY_MV_EMPTY            3000
#define BATTERY_MV_FULL             4000
#define BATTERY_LEVEL_PERCENT      { 20, 50, 80, 100 }

static const uint8_t level_percent[BATTERY_LEVEL_NUM] = BATTERY_LEVEL_PERCENT;

static struct
{
    bool initialized;
    battery_voltage_t voltage;
    battery_info_t info;
    bool data_valid;
} self;

static void enable_pin_init(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin   = BATTERY_ENABLE_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BATTERY_ENABLE_PORT, &g);
    HAL_GPIO_WritePin(BATTERY_ENABLE_PORT, BATTERY_ENABLE_PIN, GPIO_PIN_RESET);
}

static void adc_ensure_software_trigger(void)
{
    if (hadc1.Instance == NULL)
        return;
    CLEAR_BIT(hadc1.Instance->CR2, ADC_CR2_DMA);
    if (hadc1.Init.ExternalTrigConv == ADC_SOFTWARE_START)
        return;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    MODIFY_REG(hadc1.Instance->CR2, ADC_CR2_EXTSEL, ADC_SOFTWARE_START);
    hadc1.State = HAL_ADC_STATE_READY;
}

static uint32_t adc_read_single(void)
{
    if (hadc1.Instance == NULL)
        return 0;
    hadc1.State = HAL_ADC_STATE_READY;
    hadc1.ErrorCode = HAL_ADC_ERROR_NONE;
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
        return 0;
    if (HAL_ADC_PollForConversion(&hadc1, BATTERY_ADC_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&hadc1);
        return 0;
    }
    uint32_t raw = (uint32_t)HAL_ADC_GetValue(&hadc1);
    (void)HAL_ADC_Stop(&hadc1);
    return raw;
}

static uint8_t mv_to_percent(uint32_t mv)
{
    if (mv >= BATTERY_MV_FULL)
        return 100;
    if (mv <= BATTERY_MV_EMPTY)
        return 0;
    return (uint8_t)((mv - BATTERY_MV_EMPTY) * 100u / (BATTERY_MV_FULL - BATTERY_MV_EMPTY));
}

void battery_init(void)
{
    if (self.initialized)
        return;
    memset(&self, 0, sizeof(self));
    __HAL_RCC_GPIOA_CLK_ENABLE();
    enable_pin_init();
    adc_ensure_software_trigger();
    self.initialized = true;
}

uint32_t battery_voltage_read_mv(battery_voltage_t *voltage)
{
    if (!self.initialized)
        return 0;

    HAL_GPIO_WritePin(BATTERY_ENABLE_PORT, BATTERY_ENABLE_PIN, GPIO_PIN_SET);
    HAL_Delay(BATTERY_RC_STABLE_MS);

    uint32_t sum = 0;
    uint32_t min_raw = 0xFFFFFFFFu;
    uint32_t max_raw = 0;
    uint16_t count = 0;

    for (uint16_t i = 0; i < BATTERY_SAMPLE_CNT; i++)
    {
        uint32_t raw = adc_read_single();
        sum += raw;
        if (count == 0)
        {
            min_raw = max_raw = raw;
            count = 1;
            continue;
        }
        count++;
        if (raw < min_raw)
            min_raw = raw;
        if (raw > max_raw)
            max_raw = raw;
    }

    HAL_GPIO_WritePin(BATTERY_ENABLE_PORT, BATTERY_ENABLE_PIN, GPIO_PIN_RESET);

    if (count < 3)
        return 0;

    sum -= min_raw;
    sum -= max_raw;
    count -= 2;
    uint32_t avg_raw = sum / count;
    uint32_t vadc_mv = avg_raw * BATTERY_VREF_MV / 4095u;
    uint32_t vbatt_mv = vadc_mv * BATTERY_DIVIDER_RATIO;

    if (voltage != NULL)
    {
        voltage->current_mv = (uint16_t)vbatt_mv;
        voltage->min_mv    = (uint16_t)((min_raw * BATTERY_VREF_MV / 4095u) * BATTERY_DIVIDER_RATIO);
        voltage->max_mv    = (uint16_t)((max_raw * BATTERY_VREF_MV / 4095u) * BATTERY_DIVIDER_RATIO);
    }

    return vbatt_mv;
}

bool battery_percent_update(void)
{
    if (!self.initialized)
        return false;

    battery_voltage_t v;
    uint32_t mv = battery_voltage_read_mv(&v);
    if (mv == 0)
        return false;

    self.voltage = v;
    self.info.percent = mv_to_percent(mv);
    self.info.level   = BATTERY_LEVEL_NUM - 1;
    for (uint8_t i = 0; i < BATTERY_LEVEL_NUM; i++)
    {
        if (self.info.percent < level_percent[i])
        {
            self.info.level = i;
            break;
        }
    }
    self.data_valid = true;
    return true;
}

bool battery_info_read(battery_info_t *info, battery_voltage_t *voltage)
{
    if (!self.initialized || !self.data_valid)
        return false;
    if (info != NULL)
        *info = self.info;
    if (voltage != NULL)
        *voltage = self.voltage;
    return true;
}
