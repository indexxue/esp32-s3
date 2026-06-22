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

/** 顶栏左侧时钟起点。 */
#define VOTE_MENU_TITLE_CLK_X (4U)
/** 时钟文字与标题之间的间距。 */
#define VOTE_MENU_TITLE_CLK_GAP (4U)
/** 顶栏右侧 IP 预留宽度。 */
#define VOTE_MENU_TITLE_IP_W (100U)

/** 选人屏：返回键长按超过该时长记废票（ms）。 */
#define VOTE_SELECT_SPOILED_HOLD_MS (2000U)
/** 选人屏：无操作超时退回投票等待（秒），不记票。 */
#define VOTE_SELECT_SESSION_TIMEOUT_SEC (15U)
/** 废票声光报警持续时间（ms）。 */
#define VOTE_SPOILED_ALARM_MS (5000U)
/** 投票时段开始蓝灯慢闪时长（ms）；与 led_scene 中 5 个 1 Hz 周期一致。 */
#define VOTE_VOTING_LED_BLINK_MS (5000U)

/** 业务屏确认键长按进入管理员菜单（ms）。 */
#define VOTE_ADMIN_CONFIRM_HOLD_MS (3000U)

#define VOTE_MENU_RGB565(r, g, b)                                                                                     \
    ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | ((uint16_t)(b) >> 3)))

#define VOTE_MENU_COLOR_BG VOTE_MENU_RGB565(0x0D, 0x12, 0x18)
#define VOTE_MENU_COLOR_SURFACE VOTE_MENU_RGB565(0x1A, 0x23, 0x32)
#define VOTE_MENU_COLOR_SURFACE2 VOTE_MENU_RGB565(0x24, 0x30, 0x44)
#define VOTE_MENU_COLOR_TEXT VOTE_MENU_RGB565(0xE8, 0xEE, 0xF7)
#define VOTE_MENU_COLOR_MUTED VOTE_MENU_RGB565(0x8B, 0x9C, 0xB3)
#define VOTE_MENU_COLOR_ACCENT VOTE_MENU_RGB565(0x3D, 0x8B, 0xFD)
#define VOTE_MENU_COLOR_OK VOTE_MENU_RGB565(0x3D, 0xD6, 0x8C)
#define VOTE_MENU_COLOR_WARN VOTE_MENU_RGB565(0xE6, 0xC3, 0x5C)
#define VOTE_MENU_COLOR_DANGER VOTE_MENU_RGB565(0xE8, 0x5D, 0x6C)
#define VOTE_MENU_COLOR_DANGER_BG VOTE_MENU_RGB565(0x3A, 0x1F, 0x26)
#define VOTE_MENU_COLOR_BORDER VOTE_MENU_RGB565(0x2D, 0x3A, 0x4D)

/** 全部 LCD 画面（业务屏 + 管理员菜单），对齐设计稿 §2.1。 */
typedef enum {
    VOTE_LCD_SCREEN_HOME = 0,
    VOTE_LCD_SCREEN_VOTING,
    VOTE_LCD_SCREEN_SELECT,
    VOTE_LCD_SCREEN_COOLDOWN,
    VOTE_LCD_SCREEN_VIOLATION,
    VOTE_LCD_SCREEN_LOCKED,
    VOTE_LCD_SCREEN_HISTORY,
    VOTE_LCD_SCREEN_ADMIN,
    VOTE_LCD_SCREEN_ADMIN_TIME,
    VOTE_LCD_SCREEN_ADMIN_COUNT,
    VOTE_LCD_SCREEN_ADMIN_COOLDOWN,
    VOTE_LCD_SCREEN_ADMIN_RESET,
} vote_lcd_screen_id_t;

/** 管理员菜单页（menu 引擎子集）。 */
typedef enum {
    VOTE_MENU_PAGE_ADMIN = (int)VOTE_LCD_SCREEN_ADMIN,
    VOTE_MENU_PAGE_TIME = (int)VOTE_LCD_SCREEN_ADMIN_TIME,
    VOTE_MENU_PAGE_COUNT = (int)VOTE_LCD_SCREEN_ADMIN_COUNT,
    VOTE_MENU_PAGE_COOLDOWN = (int)VOTE_LCD_SCREEN_ADMIN_COOLDOWN,
    VOTE_MENU_PAGE_RESET = (int)VOTE_LCD_SCREEN_ADMIN_RESET,
} vote_menu_page_id_t;

static inline bool vote_lcd_screen_is_menu(vote_lcd_screen_id_t id)
{
    return id >= VOTE_LCD_SCREEN_ADMIN;
}

#endif
