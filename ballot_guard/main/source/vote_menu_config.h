/**
 * @file vote_menu_config.h
 * @brief ballot_guard 管理员菜单 LCD 布局常量（对齐 tmp/ballot_guard_LCD菜单UI设计.md）。
 */

#ifndef VOTE_MENU_CONFIG_H
#define VOTE_MENU_CONFIG_H

#include <stdint.h>

#define VOTE_MENU_LCD_W (240U)
#define VOTE_MENU_LCD_H (135U)

#define VOTE_MENU_TITLE_H (22U)
#define VOTE_MENU_BODY_Y (24U)
#define VOTE_MENU_BODY_H (92U)
#define VOTE_MENU_FOOT_Y (117U)
#define VOTE_MENU_FOOT_H (16U)

#define VOTE_MENU_ROW_H (22U)
#define VOTE_MENU_VISIBLE_ROWS (4U)

#define VOTE_MENU_RGB565(r, g, b)                                                                                     \
    ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | ((uint16_t)(b) >> 3)))

#define VOTE_MENU_COLOR_BG VOTE_MENU_RGB565(0x0D, 0x12, 0x18)
#define VOTE_MENU_COLOR_SURFACE VOTE_MENU_RGB565(0x1A, 0x23, 0x32)
#define VOTE_MENU_COLOR_SURFACE2 VOTE_MENU_RGB565(0x24, 0x30, 0x44)
#define VOTE_MENU_COLOR_TEXT VOTE_MENU_RGB565(0xE8, 0xEE, 0xF7)
#define VOTE_MENU_COLOR_MUTED VOTE_MENU_RGB565(0x8B, 0x9C, 0xB3)
#define VOTE_MENU_COLOR_ACCENT VOTE_MENU_RGB565(0x3D, 0x8B, 0xFD)
#define VOTE_MENU_COLOR_OK VOTE_MENU_RGB565(0x3D, 0xD6, 0x8C)
#define VOTE_MENU_COLOR_DANGER VOTE_MENU_RGB565(0xE8, 0x5D, 0x6C)
#define VOTE_MENU_COLOR_DANGER_BG VOTE_MENU_RGB565(0x3A, 0x1F, 0x26)
#define VOTE_MENU_COLOR_BORDER VOTE_MENU_RGB565(0x2D, 0x3A, 0x4D)

typedef enum {
    VOTE_MENU_PAGE_ADMIN = 0,
    VOTE_MENU_PAGE_TIME,
    VOTE_MENU_PAGE_COUNT,
    VOTE_MENU_PAGE_COOLDOWN,
    VOTE_MENU_PAGE_RESET,
} vote_menu_page_id_t;

#endif
