/**
 * @file ble_led.h
 * @brief BLE 演示工程的 WS2812 灯效（复用 common/led_scene，与 project 一致）。
 */

#ifndef BLE_LED_H
#define BLE_LED_H

#include <stdint.h>

#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

status_t ble_led_init(void);

/** 等待手机连接：蓝青律动（PAIRING 场景） */
void ble_led_on_advertising(void);

/** 已连接：彩虹渐变（WORKING 场景） */
void ble_led_on_connected(void);

/** 断开连接：回到等待连接灯效 */
void ble_led_on_disconnected(void);

/**
 * 手机写入特征值时切换灯效。
 * - 1 字节 ASCII：'0' bootup, '1' pairing, '2' trigger, '3' success, '4' working, '5' error
 * - 其它文本：黄灯闪 5 次（TRIGGER）
 */
void ble_led_on_write(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_LED_H */
