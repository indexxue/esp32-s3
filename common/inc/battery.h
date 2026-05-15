/**
 * @file battery.h
 * @brief ESP32-S3 电池电压与档位（与 `tmp/battery.*` 行为对齐：使能脚 + ADC + 分压比）。
 */

#ifndef COMMON_BATTERY_H
#define COMMON_BATTERY_H

#include "type.h"

#include <stdint.h>

#define BATTERY_LEVEL_NUM (4U)

typedef struct {
    uint16_t min_mv;
    uint16_t max_mv;
    uint16_t current_mv;
} battery_voltage_t;

typedef struct {
    uint8_t percent;
    uint8_t level;
    bool_t charging; /**< 电池端电压 ≥4200mV 时视为外接充电（与 SoC 满电 4000mV 区分） */
} battery_info_t;

void battery_init(void);

uint32_t battery_voltage_read_mv(battery_voltage_t *voltage);

bool_t battery_percent_update(void);

bool_t battery_info_read(battery_info_t *info, battery_voltage_t *voltage);

#endif
