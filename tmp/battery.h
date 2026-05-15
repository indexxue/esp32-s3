/**
 * @file    battery.h
 * @brief   Battery voltage and SoC detection for STM32F103 (3.7V Li-ion, divider on PA0, enable PA3).
 */

#ifndef __BATTERY_H
#define __BATTERY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define BATTERY_LEVEL_NUM  4

typedef struct
{
    uint16_t min_mv;
    uint16_t max_mv;
    uint16_t current_mv;
} battery_voltage_t;

typedef struct
{
    uint8_t percent;
    uint8_t level;
} battery_info_t;

void battery_init(void);

uint32_t battery_voltage_read_mv(battery_voltage_t *voltage);

bool battery_percent_update(void);

bool battery_info_read(battery_info_t *info, battery_voltage_t *voltage);

#ifdef __cplusplus
}
#endif

#endif
