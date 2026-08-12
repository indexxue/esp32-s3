/**
 * @file desktop_pet_ui.h
 * @brief LVGL UI on GC9A01：init / flush / home 页（与 PC 模拟器第一页对齐）。
 */

#ifndef DESKTOP_PET_UI_H
#define DESKTOP_PET_UI_H

#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 LVGL、绑定 BoardGc9a01() flush，创建 home 页并启动 UI 任务。
 * 需在 BoardInit() 成功且 LCD 已就绪后调用。
 */
status_t desktop_pet_ui_start(void);

#ifdef __cplusplus
}
#endif

#endif /* DESKTOP_PET_UI_H */
