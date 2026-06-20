/**
 * @file vote_menu_demo.h
 * @brief ballot_guard LCD UI：业务屏 + 管理员菜单，按键与 Web 调试。
 */

#ifndef VOTE_MENU_DEMO_H
#define VOTE_MENU_DEMO_H

#include <stdbool.h>

#include "button.h"
#include "menu.h"
#include "type.h"
#include "vote_menu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动 LCD UI 任务（需 BoardInit 已完成且 LCD 就绪）。
 * 默认显示主界面；左=上/减，右=下/增，左长按=确认，右长按=返回。
 */
status_t vote_menu_demo_start(void);

bool vote_menu_demo_is_active(void);

/** 由 start.c 按键回调转发。返回 true 表示事件已被 UI 消费。 */
bool vote_menu_demo_on_button(btn_id_e id, btn_event_e event);

/** 当前 LCD 画面 ID。 */
vote_lcd_screen_id_t vote_menu_demo_current_screen(void);

/** @deprecated 使用 vote_menu_demo_current_screen。 */
vote_menu_page_id_t vote_menu_demo_current_page(void);

/** 跳转到任意 LCD 画面（Web 调试 / 预览）。 */
bool vote_menu_demo_goto_screen(vote_lcd_screen_id_t screen);

/** @deprecated 使用 vote_menu_demo_goto_screen。 */
bool vote_menu_demo_goto_page(vote_menu_page_id_t page);

/** 向当前界面注入逻辑动作（菜单内或部分业务屏）。 */
bool vote_menu_demo_dispatch(menu_evt_t evt);

#ifdef __cplusplus
}
#endif

#endif
