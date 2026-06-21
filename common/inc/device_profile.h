/**
 * @file device_profile.h
 * @brief 单一设备描述表：软件产品 (product_id) 决定外设/平台初始化子集；硬件型号 (hardware_id) 预留引脚差异扩展。
 *
 * 新增产品：在 device_profile.c 登记 board_mask / platform_mask，并同步 nvs.h / nvs_project_id.cmake。
 */

#ifndef COMMON_DEVICE_PROFILE_H
#define COMMON_DEVICE_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "button.h"
#include "nvs.h"
#include "type.h"

/* -------------------------------------------------------------------------- */
/* 板级外设掩码（BoardInit）                                                    */
/* -------------------------------------------------------------------------- */

#define DEVICE_BOARD_MASK_I2C (1U << 0)
#define DEVICE_BOARD_MASK_LCD (1U << 1)
#define DEVICE_BOARD_MASK_BATTERY (1U << 2)
#define DEVICE_BOARD_MASK_IMU (1U << 3)
#define DEVICE_BOARD_MASK_SDCARD (1U << 4)
#define DEVICE_BOARD_MASK_RTC (1U << 5)
#define DEVICE_BOARD_MASK_IR (1U << 6)

#define DEVICE_BOARD_MASK_FULL                                                                                       \
    (DEVICE_BOARD_MASK_I2C | DEVICE_BOARD_MASK_LCD | DEVICE_BOARD_MASK_BATTERY | DEVICE_BOARD_MASK_IMU |           \
     DEVICE_BOARD_MASK_SDCARD)

/* -------------------------------------------------------------------------- */
/* 平台壳层掩码（start.c：按键 / 灯效 / WEB 等）                                   */
/* -------------------------------------------------------------------------- */

#define DEVICE_PLATFORM_MASK_BUTTON (1U << 0)
#define DEVICE_PLATFORM_MASK_LED (1U << 1)
#define DEVICE_PLATFORM_MASK_WEB (1U << 2)
#define DEVICE_PLATFORM_MASK_BUZZER (1U << 3)

#define DEVICE_PLATFORM_MASK_FULL                                                                                    \
    (DEVICE_PLATFORM_MASK_BUTTON | DEVICE_PLATFORM_MASK_LED | DEVICE_PLATFORM_MASK_WEB | DEVICE_PLATFORM_MASK_BUZZER)

/* -------------------------------------------------------------------------- */
/* 产品档案                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    int32_t gpio;
    btn_id_e id;
    const char *name;
    uint8_t active_level;
    uint16_t permission;
} device_button_spec_t;

typedef struct {
    uint32_t product_id;
    const char *name;
    uint32_t board_mask;
    uint32_t platform_mask;
    const device_button_spec_t *buttons;
    uint8_t button_count;
    const char *lcd_smoke_title;
    const char *lcd_smoke_subtitle;
} device_product_profile_t;

/** 当前生效的产品档案（product_id 已解析）。 */
const device_product_profile_t *device_profile_product(void);

/** 当前 NVS 登记的 hardware_id。 */
uint32_t device_profile_hardware_id(void);
const char *device_profile_hardware_name(uint32_t hardware_id);

/** 产品档案中的板级/平台掩码。 */
uint32_t device_profile_board_mask(void);
uint32_t device_profile_platform_mask(void);

bool device_profile_board_wants(uint32_t mask);
bool device_profile_platform_wants(uint32_t mask);

const char *device_profile_lcd_smoke_title(void);
const char *device_profile_lcd_smoke_subtitle(void);

uint8_t device_profile_button_count(void);
const device_button_spec_t *device_profile_button_spec(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_DEVICE_PROFILE_H */
