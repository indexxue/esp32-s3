/**
 * @file ui.h
 * @brief LVGL port on GC9A01：flush / touch / pet_view；可选 debug 覆盖层。
 */

#ifndef UI_H
#define UI_H

#include "type.h"

#include <stdbool.h>
#include <stdint.h>

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

/**
 * 开始四点触摸校准（屏上十字；GPIO0 单击取消）。须在 UI 任务已启动后调用。
 */
bool desktop_pet_ui_touch_calib_start(void);

/** 取消进行中的校准，不改 NVS。 */
bool desktop_pet_ui_touch_calib_cancel(void);

/** 清除 NVS 校准并恢复 1:1 映射。 */
bool desktop_pet_ui_touch_calib_reset(void);

bool desktop_pet_ui_touch_calib_is_running(void);

/**
 * @param running 进行中
 * @param step    当前点 0..steps-1（未开始则为 0）
 * @param steps   总点数
 * @param saved   NVS 中已有有效校准
 */
void desktop_pet_ui_touch_calib_status(bool *running, uint8_t *step, uint8_t *steps, bool *saved);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
