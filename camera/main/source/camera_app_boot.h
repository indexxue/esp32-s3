/**
 * @file camera_app_boot.h
 * @brief 板级 / 摄像头 / WiFi 门控 / Web 启动。
 */

#ifndef CAMERA_APP_BOOT_H
#define CAMERA_APP_BOOT_H

#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

/** LCD 状态栏一行；无 LCD 时为空操作。 */
void camera_app_status_set(const char *line);

/** log / NVS / Board / servo / SPI / cam_mod / web_boot。 */
status_t camera_app_boot_init(void);

/** WS2812 灯效任务。 */
status_t camera_app_led_init(void);

/** CALIB 模式下注册 USB 串口舵机命令（非 CALIB 为空操作）。 */
status_t camera_app_calib_cmd_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_APP_BOOT_H */
