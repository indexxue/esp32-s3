/**
 * @file ui.h
 * @brief LVGL port on GC9A01：flush / touch / pet_view；可选 debug 覆盖层。
 */

#ifndef UI_H
#define UI_H

#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 LVGL、绑定 BoardGc9a01() flush，创建宠物主界面并启动 UI 任务。
 * 需在 BoardInit() 成功且 LCD 已就绪后调用。
 */
status_t desktop_pet_ui_start(void);

/**
 * GPIO0 单击入口。仅当 DESKTOP_PET_ENABLE_DEBUG_UI=1 时切换 debug 覆盖层；
 * 默认关闭时为空操作（产品路径，语义另定）。
 */
void desktop_pet_ui_toggle_debug(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
