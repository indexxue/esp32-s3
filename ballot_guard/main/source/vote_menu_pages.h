/**
 * @file vote_menu_pages.h
 * @brief ballot_guard 管理员菜单页表（menu 引擎数据模型）。
 */

#ifndef VOTE_MENU_PAGES_H
#define VOTE_MENU_PAGES_H

#include "menu.h"
#include "vote_menu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t start_h;
    uint8_t start_m;
    uint8_t end_h;
    uint8_t end_m;
    uint8_t candidate_count;
    uint8_t cooldown_sec;
} vote_menu_settings_t;

const menu_page_t *vote_menu_root_page(void);
menu_engine_t *vote_menu_engine(void);
vote_menu_settings_t *vote_menu_settings(void);

/** 保存后显示 Toast 等；由 demo 层注册。 */
typedef void (*vote_menu_ui_notify_cb)(void *ctx, const char *msg, int is_error);
void vote_menu_set_ui_notify(vote_menu_ui_notify_cb cb, void *ctx);

/** 重置完成或需回到业务主界面时调用（由 demo 注册）。 */
typedef void (*vote_menu_leave_app_cb)(void *ctx);
void vote_menu_set_leave_app_cb(vote_menu_leave_app_cb cb, void *ctx);

/** 初始化 menu 引擎（须在绘制/按键之前调用一次）。 */
void vote_menu_pages_init(void);

const menu_page_t *vote_menu_page_by_id(vote_menu_page_id_t id);
vote_menu_page_id_t vote_menu_page_id_of(const menu_page_t *page);

/** 末位字段 CONFIRM 时保存并返回上级（替代独立 Save 按钮）。 */
bool vote_menu_pages_confirm_save(menu_engine_t *eng);

#ifdef __cplusplus
}
#endif

#endif
