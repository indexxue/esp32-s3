/**
 * @file voice_hub_ui.h
 * @brief ST7789 240×135 状态栏与预览窗 UI 骨架。
 */

#pragma once

#include "type.h"

#include "st7789.h"

status_t voice_hub_ui_init(st7789_t *lcd);
void voice_hub_ui_draw_idle(st7789_t *lcd);
void voice_hub_ui_set_status_line(st7789_t *lcd, const char *line);
void voice_hub_ui_set_intercom_active(st7789_t *lcd, bool_t active);
