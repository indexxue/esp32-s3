/**
 * @file vote_menu_lcd.h
 * @brief ballot_guard 管理员菜单 ST7789 绘制。
 */

#ifndef VOTE_MENU_LCD_DRAW_H
#define VOTE_MENU_LCD_DRAW_H

#include "menu.h"
#include "st7789.h"
#include "vote_menu_config.h"
#include "vote_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    vote_lcd_screen_id_t screen;
    const menu_engine_t *menu_eng;
    uint8_t select_idx;
    uint8_t history_idx;
    uint8_t home_scroll_idx;
    uint8_t locked_scroll_idx;
    uint8_t cooldown_sec;
    uint8_t select_timeout_sec;
    vote_spoiled_type_e spoiled_type;
    uint8_t admin_entry_progress;
} vote_lcd_ctx_t;

void vote_menu_lcd_draw(st7789_t *lcd, const menu_engine_t *eng);
/** 按当前画面 ID 绘制（业务屏或管理员菜单）。 */
void vote_lcd_draw(st7789_t *lcd, const vote_lcd_ctx_t *ctx);
/** 仅刷新顶栏时钟；选人/待开启看板/投票等待屏同时刷新居中倒计时。 */
void vote_lcd_draw_clock(st7789_t *lcd, const vote_lcd_ctx_t *ctx);
/** 选人屏顶栏（时钟 + 倒计时 + 「选票」），由 vote_lcd_draw_clock 在 1s 周期调用。 */
void vote_lcd_draw_select_title(st7789_t *lcd, uint8_t timeout_sec);
/** 仅刷新顶栏左侧时钟与右上角 IP（约 1s 周期），避免整屏重绘。 */
void vote_menu_lcd_draw_clock(st7789_t *lcd);
void vote_menu_lcd_draw_toast(st7789_t *lcd, const char *msg, int is_error);

#ifdef __cplusplus
}
#endif

#endif
