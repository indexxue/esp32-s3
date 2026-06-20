/**
 * @file vote_menu_lcd.h
 * @brief ballot_guard 管理员菜单 ST7789 绘制。
 */

#ifndef VOTE_MENU_LCD_DRAW_H
#define VOTE_MENU_LCD_DRAW_H

#include "menu.h"
#include "st7789.h"

#ifdef __cplusplus
extern "C" {
#endif

void vote_menu_lcd_draw(st7789_t *lcd, const menu_engine_t *eng);
/** 仅刷新顶栏左侧时钟（约 72×12px），用于 1s 周期更新，避免整屏重绘。 */
void vote_menu_lcd_draw_clock(st7789_t *lcd);
void vote_menu_lcd_draw_toast(st7789_t *lcd, const char *msg, int is_error);

#ifdef __cplusplus
}
#endif

#endif
