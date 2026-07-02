/**
 * @file voice_hub_ui.h
 * @brief ST7789 240×135 状态栏与预览叠加 UI。
 */

#pragma once

#include "type.h"

#include "st7789.h"

/** LCD 右上角 IP 文字：距顶/距右像素（显示坐标系，已含翻转后方向）。 */
#define VOICE_HUB_UI_IP_MARGIN_X (2U)
#define VOICE_HUB_UI_IP_MARGIN_Y (1U)
/** IP 字体高度（点阵 sizey）；预览 blit 跳过该条带，避免每帧覆盖后重绘闪烁。 */
#define VOICE_HUB_UI_IP_FONT_SIZE (12U)
#define VOICE_HUB_UI_IP_BAND_H (VOICE_HUB_UI_IP_MARGIN_Y + VOICE_HUB_UI_IP_FONT_SIZE)

/** 后台查询 IP 间隔（ms）；GOT_IP 事件会立即刷新。 */
#define VOICE_HUB_UI_IP_REFRESH_MS (3000U)

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
