/**
 * @file camera_ui.h
 * @brief ST7789 240×135 状态栏与预览区叠加 UI。
 */

#pragma once

#include "type.h"

#include "st7789.h"

/** 顶部状态栏高度（像素）。预览 blit 跳过该条带。 */
#define CAMERA_UI_IP_BAND_H (14U)

status_t camera_ui_init(st7789_t *lcd);
void camera_ui_draw_idle(st7789_t *lcd);

/** 调用方已持有 LCD 锁时使用（如 init 内部）。 */
void camera_ui_draw_idle_nolock(st7789_t *lcd);
void camera_ui_set_status_line(st7789_t *lcd, const char *line);

/** 在 LCD 预览区绘制检测框。
 * @param boxes  扁平数组，每框 5 个 uint16: [x, y, w, h, color_565]
 * @param count  框数量
 * @param cam_w  摄像头帧宽（如 240）
 * @param cam_h  摄像头帧高（如 240）
 */
void camera_ui_draw_detection_boxes(st7789_t *lcd,
                                     const uint16_t *boxes,
                                     uint8_t count,
                                     uint16_t cam_w,
                                     uint16_t cam_h);

/** 刷新顶部状态栏（无需整屏重绘）。 */
void camera_ui_draw_status_band(st7789_t *lcd);

/** 串行化 ST7789 SPI（预览 blit 与 UI 叠加共用）。 */
bool_t camera_ui_lcd_lock(uint32_t timeout_ms);
void camera_ui_lcd_unlock(void);
