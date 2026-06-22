/**
 * @file vote_menu_demo.h
 * @brief ballot_guard LCD UI：业务屏 + 管理员菜单，按键与 Web 调试。
 */

#ifndef VOTE_MENU_DEMO_H
#define VOTE_MENU_DEMO_H

#include <stdbool.h>

#include "button.h"
#include "board.h"
#include "menu.h"
#include "type.h"
#include "vote_menu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动 LCD UI 任务（需 BoardInit 已完成且 LCD 就绪）。
 * 自检完成后进入当前 Phase 对应业务屏。
 * 6 键：Home/Locked/History 上确认键长按 3s 进管理员；选人屏短按确认=有效票，长按返回 2s=废票。
 */
status_t vote_menu_demo_start(void);

bool vote_menu_demo_is_active(void);

/** 由 start.c 按键回调转发。返回 true 表示事件已被 UI 消费。 */
bool vote_menu_demo_on_button(btn_id_e id, btn_event_e event);

/** 红外电平变化（start.c 轮询）；靠近=HIGH→LOW，离开=LOW→HIGH 忽略。 */
bool vote_menu_demo_on_ir_level(board_ir_channel_e channel, u32_t prev_level, u32_t new_level);

/** @deprecated 请使用 vote_menu_demo_on_ir_level。 */
bool vote_menu_demo_on_ir(void);

/** 当前 LCD 画面 ID。 */
vote_lcd_screen_id_t vote_menu_demo_current_screen(void);

/** @deprecated 使用 vote_menu_demo_current_screen。 */
vote_menu_page_id_t vote_menu_demo_current_page(void);

/** 跳转到任意 LCD 画面（Web 调试 / 预览）。 */
bool vote_menu_demo_goto_screen(vote_lcd_screen_id_t screen);

/** 退出管理员菜单到指定业务屏（Admin Menu Exit / Vote Reset）。 */
void vote_menu_demo_exit_admin_to(vote_lcd_screen_id_t screen);

/** Vote Reset / Web 清零票数后：恢复红外就绪并清除交互态。 */
void vote_menu_demo_on_data_reset(void);

/** @deprecated 使用 vote_menu_demo_goto_screen。 */
bool vote_menu_demo_goto_page(vote_menu_page_id_t page);

/** 向当前界面注入逻辑动作（菜单内或部分业务屏）。 */
bool vote_menu_demo_dispatch(menu_evt_t evt);

#ifdef __cplusplus
}
#endif

#endif
