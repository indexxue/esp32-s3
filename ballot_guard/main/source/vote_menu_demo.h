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
 * 自检完成后默认进入管理员菜单。
 * 6 键：上/下/左/右/确认/返回独立；确认短按=有效票，确认长按=废票（选人屏）。
 * 开发板 2 键：左=上/长按确认，右=下/长按返回。
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

/** 退出管理员菜单，进入当前 phase 对应业务屏（LCD 菜单 / Web 均可调用）。 */
bool vote_menu_demo_enter_voting(void);

/** @deprecated 使用 vote_menu_demo_goto_screen。 */
bool vote_menu_demo_goto_page(vote_menu_page_id_t page);

/** 向当前界面注入逻辑动作（菜单内或部分业务屏）。 */
bool vote_menu_demo_dispatch(menu_evt_t evt);

#ifdef __cplusplus
}
#endif

#endif
