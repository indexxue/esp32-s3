/**
 * @file vote_menu_demo.h
 * @brief ballot_guard 菜单 LCD 演示：上电进入管理员菜单，按键导航。
 */

#ifndef VOTE_MENU_DEMO_H
#define VOTE_MENU_DEMO_H

#include <stdbool.h>

#include "button.h"
#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动菜单演示任务（需 BoardInit 已完成且 LCD 就绪）。
 * 按键：左=上/减，右=下/增，左长按=确认，右长按=返回。
 */
status_t vote_menu_demo_start(void);

bool vote_menu_demo_is_active(void);

/** 由 start.c 按键回调转发。返回 true 表示事件已被菜单消费。 */
bool vote_menu_demo_on_button(btn_id_e id, btn_event_e event);

#ifdef __cplusplus
}
#endif

#endif
