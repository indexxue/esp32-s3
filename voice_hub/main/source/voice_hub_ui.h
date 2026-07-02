/**
 * @file voice_hub_ui.h
 * @brief ST7789 240×135 状态栏与预览叠加 UI。
 */

#pragma once

#include "type.h"

#include "st7789.h"

status_t voice_hub_ui_init(st7789_t *lcd);
void voice_hub_ui_draw_idle(st7789_t *lcd);
/** 调用方已持有 LCD 锁时使用（如 init 内部）。 */
void voice_hub_ui_draw_idle_nolock(st7789_t *lcd);
void voice_hub_ui_set_status_line(st7789_t *lcd, const char *line);
void voice_hub_ui_set_intercom_active(st7789_t *lcd, bool_t active);

/** 从 net_wifi 刷新缓存 IP（建议 1–2 s 调用一次）。 */
void voice_hub_ui_refresh_ip(void);

/** STA 取得/失去 IP 时立即刷新 LCD 顶栏（注册到 net_wifi IPv4 事件）。 */
void voice_hub_ui_on_ipv4_changed(void);

/** 当前缓存 IP 文本（永不为 NULL）。 */
const char *voice_hub_ui_ip_line(void);

/** 在画面右上角绘制 IP（不透明底 + 小字，不整屏刷新）。 */
void voice_hub_ui_draw_ip_overlay(st7789_t *lcd);

/** IP 缓存变化后重绘 LCD 条带（预览任务不每帧调用）。 */
void voice_hub_ui_redraw_ip_if_dirty(st7789_t *lcd);

/** 串行化 ST7789 SPI（预览 blit 与 UI 叠加共用）。 */
bool_t voice_hub_ui_lcd_lock(uint32_t timeout_ms);
void voice_hub_ui_lcd_unlock(void);
