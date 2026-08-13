/**
 * @file desktop_pet_ui.h
 * @brief LVGL port on GC9A01：flush / touch / pet_view / GPIO0 debug overlay.
 */

#ifndef DESKTOP_PET_UI_H
#define DESKTOP_PET_UI_H

#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 LVGL、绑定 BoardGc9a01() flush，创建宠物主界面并启动 UI 任务。
 * 需在 BoardInit() 成功且 LCD 已就绪后调用。
 */
status_t desktop_pet_ui_start(void);

/** 从按键任务投递：在 LVGL 任务中切换 Rec/Play debug 覆盖层。 */
void desktop_pet_ui_toggle_debug(void);

#ifdef __cplusplus
}
#endif

#endif /* DESKTOP_PET_UI_H */
